#include "i2c.h"

#include "clock.h"
#include "icsp.h"

/*
 * Register addresses (RM0444). I2C1 base is 0x40005400 (Table 6); offsets from 32.9:
 * CR1 0x00, CR2 0x04, TIMINGR 0x10, ISR 0x18, ICR 0x1C, TXDR 0x28. GPIOB base 0x50000400
 * with MODER 0x00, OTYPER 0x04, PUPDR 0x0C, BSRR 0x18, AFRH 0x24 (7.5).
 */
#define I2C1_CR1     (*(volatile uint32_t *)0x40005400u)
#define I2C1_CR2     (*(volatile uint32_t *)0x40005404u)
#define I2C1_TIMINGR (*(volatile uint32_t *)0x40005410u)
#define I2C1_ISR     (*(volatile uint32_t *)0x40005418u)
#define I2C1_ICR     (*(volatile uint32_t *)0x4000541Cu)
#define I2C1_TXDR    (*(volatile uint32_t *)0x40005428u)
#define RCC_IOPENR   (*(volatile uint32_t *)0x40021034u)
#define RCC_APBENR1  (*(volatile uint32_t *)0x4002103Cu)
#define GPIOB_MODER  (*(volatile uint32_t *)0x50000400u)
#define GPIOB_OTYPER (*(volatile uint32_t *)0x50000404u)
#define GPIOB_PUPDR  (*(volatile uint32_t *)0x5000040Cu)
#define GPIOB_AFRH   (*(volatile uint32_t *)0x50000424u)
#define GPIOB_BSRR   (*(volatile uint32_t *)0x50000418u)

#define RCC_IOPENR_GPIOBEN (1u << 1)
#define RCC_APBENR1_I2C1EN (1u << 21)

#define PIN_SCL 8u /* PB8 = I2C1_SCL, AF6 (datasheet Table 14) */
#define PIN_SDA 9u /* PB9 = I2C1_SDA, AF6 */
#define AF_I2C1 6u
#define MODE_AF 2u
#define PULL_UP 1u

#define I2C_CR1_PE           (1u << 0)
#define I2C_CR2_START        (1u << 13)
#define I2C_CR2_STOP         (1u << 14)
#define I2C_CR2_RELOAD       (1u << 24)
#define I2C_CR2_AUTOEND      (1u << 25)
#define I2C_CR2_NBYTES_SHIFT 16u
#define I2C_ISR_TXIS         (1u << 1)
#define I2C_ISR_NACKF        (1u << 4)
#define I2C_ISR_STOPF        (1u << 5)
#define I2C_ISR_TCR          (1u << 7)
#define I2C_ISR_BUSY         (1u << 15)
#define I2C_ICR_NACKCF       (1u << 4)
#define I2C_ICR_STOPCF       (1u << 5)
#define I2C_ICR_BERRCF       (1u << 8)
#define I2C_ICR_ARLOCF       (1u << 9)

static void set_field(volatile uint32_t *reg, uint32_t shift, uint32_t width_mask, uint32_t v)
{
    *reg = (*reg & ~(width_mask << shift)) | (v << shift);
}

/* Open drain with the weak internal pull-ups, so an unconnected bus idles high (and, with
 * the hardware driver, a missing display reads as a NACK). The bus still needs proper
 * external pull-ups (the SSD1309 requires them, 8.1.5); see docs/hookup.md. */
static void pins_open_drain(void)
{
    RCC_IOPENR |= RCC_IOPENR_GPIOBEN;
    (void)RCC_IOPENR;
    GPIOB_OTYPER |= (1u << PIN_SCL) | (1u << PIN_SDA);
    set_field(&GPIOB_PUPDR, PIN_SCL * 2u, 3u, PULL_UP);
    set_field(&GPIOB_PUPDR, PIN_SDA * 2u, 3u, PULL_UP);
}

#if !I2C_IGNORE_ACK

/*
 * Timing, from the I2C_TIMINGR fields (32.9.5) with tPRESC = 125 ns, as in the RM0444
 * Fast-mode examples (Tables 173 and 174):
 *   SCLL 12: tSCLL = 13 x 125 ns = 1625 ns
 *   SCLH 6:  tSCLH =  7 x 125 ns =  875 ns
 *   tSCL is at least tSCLL + tSCLH = 2.5 us (the sync delays only add to it), which meets
 *   the SSD1309 minimum clock cycle of 2.5 us (SSD1309 Table 13-6). The RM0444 example
 *   values (SCLL 9, SCLH 3) give about 2.5 us only with typical sync delays.
 *   SDADEL 3: data hold 375 ns, above the SSD1309 SDAIN hold minimum of 300 ns.
 *   SCLDEL 3: data setup 4 x 125 ns = 500 ns, above the SSD1309 minimum of 100 ns.
 * I2C1 runs from PCLK (RCC_CCIPR I2C1SEL reset value), which equals SystemCoreClock.
 */
#define T_PRESC_HZ 8000000u
#define SCLL       12u
#define SCLH       6u
#define SDADEL     3u
#define SCLDEL     3u

/* Upper bound on any wait. At 380 kHz one byte takes about 24 us; this is far longer. */
#define WAIT_LOOPS 200000u

/* Wait for any bit of `mask` in ISR. Returns the ISR value, or 0 on timeout. */
static uint32_t wait_isr(uint32_t mask)
{
    for (uint32_t i = 0; i < WAIT_LOOPS; i++) {
        uint32_t isr = I2C1_ISR;
        if ((isr & mask) != 0u) {
            return isr;
        }
    }
    return 0u;
}

/* PE = 0 releases the lines and resets the state machine and flags (32.9.1). It must stay
 * low for at least three APB clock cycles; the read-backs provide that. */
static void reset_peripheral(void)
{
    I2C1_CR1 &= ~I2C_CR1_PE;
    (void)I2C1_CR1;
    (void)I2C1_CR1;
    (void)I2C1_CR1;
    I2C1_CR1 |= I2C_CR1_PE;
}

void i2c_init(void)
{
    uint32_t presc = SystemCoreClock / T_PRESC_HZ - 1u; /* 7 at 64 MHz, 1 at 16 MHz */

    RCC_APBENR1 |= RCC_APBENR1_I2C1EN;
    (void)RCC_APBENR1;

    pins_open_drain();
    set_field(&GPIOB_AFRH, (PIN_SCL - 8u) * 4u, 0xFu, AF_I2C1);
    set_field(&GPIOB_AFRH, (PIN_SDA - 8u) * 4u, 0xFu, AF_I2C1);
    set_field(&GPIOB_MODER, PIN_SCL * 2u, 3u, MODE_AF);
    set_field(&GPIOB_MODER, PIN_SDA * 2u, 3u, MODE_AF);

    /* TIMINGR can only be written while PE = 0, which it is after reset. */
    I2C1_TIMINGR = (presc << 28) | (SCLDEL << 20) | (SDADEL << 16) | (SCLH << 8) | SCLL;
    I2C1_CR1 = I2C_CR1_PE;
}

/* After a NACK. The peripheral sends STOP by itself (RM0444 32.4.8, controller
 * transmitter); wait for it, force one if it never comes, then clear the flags. */
static i2c_status_t finish_nack(void)
{
    if (wait_isr(I2C_ISR_STOPF) == 0u) {
        I2C1_CR2 |= I2C_CR2_STOP;
        if (wait_isr(I2C_ISR_STOPF) == 0u) {
            reset_peripheral();
            return I2C_TIMEOUT;
        }
    }
    I2C1_ICR = I2C_ICR_NACKCF | I2C_ICR_STOPCF;
    return I2C_NACK;
}

/* NBYTES and RELOAD/AUTOEND for the next block of a transfer with `remaining` bytes left. */
static uint32_t block_bits(size_t remaining)
{
    if (remaining > 255u) {
        return (255u << I2C_CR2_NBYTES_SHIFT) | I2C_CR2_RELOAD;
    }
    return ((uint32_t)remaining << I2C_CR2_NBYTES_SHIFT) | I2C_CR2_AUTOEND;
}

i2c_status_t i2c_write(uint8_t addr7, uint8_t first, const uint8_t *data, size_t len)
{
    size_t total = len + 1u;
    size_t sent = 0u;
    size_t block_end;
    uint32_t sadd = (uint32_t)addr7 << 1; /* SADD[7:1] in 7-bit mode (32.9.2) */

    for (uint32_t i = 0; (I2C1_ISR & I2C_ISR_BUSY) != 0u; i++) {
        if (i == WAIT_LOOPS) {
            reset_peripheral();
            return I2C_TIMEOUT;
        }
    }
    I2C1_ICR = I2C_ICR_NACKCF | I2C_ICR_STOPCF | I2C_ICR_BERRCF | I2C_ICR_ARLOCF;

    block_end = total > 255u ? 255u : total;
    I2C1_CR2 = sadd | block_bits(total) | I2C_CR2_START;

    while (sent < total) {
        if (sent == block_end) {
            /* Block done with RELOAD set: wait for TCR, then load the next NBYTES. */
            uint32_t isr = wait_isr(I2C_ISR_TCR | I2C_ISR_NACKF);
            if (isr == 0u) {
                reset_peripheral();
                return I2C_TIMEOUT;
            }
            if ((isr & I2C_ISR_NACKF) != 0u) {
                return finish_nack();
            }
            size_t remaining = total - sent;
            block_end += remaining > 255u ? 255u : remaining;
            I2C1_CR2 = sadd | block_bits(remaining);
        }

        uint32_t isr = wait_isr(I2C_ISR_TXIS | I2C_ISR_NACKF);
        if (isr == 0u) {
            reset_peripheral();
            return I2C_TIMEOUT;
        }
        if ((isr & I2C_ISR_NACKF) != 0u) {
            return finish_nack();
        }
        I2C1_TXDR = sent == 0u ? first : data[sent - 1u];
        sent++;
    }

    /* AUTOEND sends the STOP after the last byte. */
    uint32_t isr = wait_isr(I2C_ISR_STOPF | I2C_ISR_NACKF);
    if (isr == 0u) {
        reset_peripheral();
        return I2C_TIMEOUT;
    }
    if ((isr & I2C_ISR_NACKF) != 0u) {
        return finish_nack();
    }
    I2C1_ICR = I2C_ICR_STOPCF;
    return I2C_OK;
}

#else /* I2C_IGNORE_ACK: software bus on the same pins */

#define MODE_OUTPUT 1u

/* Half a clock period. 2 us gives a 4 us cycle (about 250 kHz), above the SSD1309 minimum
 * cycle of 2.5 us, and every setup, hold, start, stop and idle time in SSD1309 Table 13-6
 * (all 1.3 us or less) is covered by at least one half period. */
#define T_HALF_US   2u

/* Writing 1 releases the line (the pull-up takes it high); writing 0 drives it low.
 * BSRR stores are single writes, so they cannot disturb the ICSP pins on the same port. */
static void scl(uint32_t high)
{
    GPIOB_BSRR = high != 0u ? (1u << PIN_SCL) : (1u << (PIN_SCL + 16u));
}

static void sda(uint32_t high)
{
    GPIOB_BSRR = high != 0u ? (1u << PIN_SDA) : (1u << (PIN_SDA + 16u));
}

static void half(void)
{
    icsp_delay_us(T_HALF_US);
}

/* Eight data bits, MSb first, then a ninth clock for the acknowledge, which is not read. */
static void send_byte(uint8_t byte)
{
    for (uint32_t i = 8u; i > 0u; i--) {
        sda((byte >> (i - 1u)) & 1u); /* data changes only while SCL is low */
        half();
        scl(1u);
        half();
        scl(0u);
    }
    sda(1u);
    half();
    scl(1u);
    half();
    scl(0u);
}

void i2c_init(void)
{
    pins_open_drain();
    /* Released (high) before they become outputs, so the bus never sees a false start. */
    scl(1u);
    sda(1u);
    set_field(&GPIOB_MODER, PIN_SCL * 2u, 3u, MODE_OUTPUT);
    set_field(&GPIOB_MODER, PIN_SDA * 2u, 3u, MODE_OUTPUT);
}

i2c_status_t i2c_write(uint8_t addr7, uint8_t first, const uint8_t *data, size_t len)
{
    /* START: SDA falls while SCL is high. */
    sda(1u);
    scl(1u);
    half();
    sda(0u);
    half();
    scl(0u);

    send_byte((uint8_t)(addr7 << 1)); /* R/W# = 0: write */
    send_byte(first);
    for (size_t i = 0; i < len; i++) {
        send_byte(data[i]);
    }

    /* STOP: SDA rises while SCL is high, then the bus idles before the next START. */
    sda(0u);
    half();
    scl(1u);
    half();
    sda(1u);
    half();
    return I2C_OK;
}

#endif
