#ifndef REGS_H
#define REGS_H

#include <stdint.h>

/*
 * STM32G071RB peripheral registers used by this firmware, one definition each.
 *
 * Every register carries a doc comment so editor hover (clangd) shows its address, reset value,
 * access and fields. Sources: RM0444 Rev 6 (peripheral base addresses: Table 6, section 2.2)
 * and PM0223 Rev 9 for the Cortex-M0+ core peripherals. Section numbers are those documents'.
 *
 * Access codes: r read-only, w write-only, rw read/write, rs set by writing 1 (cleared by
 * hardware), rc_w1 cleared by writing 1, rc_w0 cleared by writing 0.
 *
 * Bit-field masks and shifts stay next to the code that uses them, except the RCC peripheral
 * clock enable bits, which several files share.
 */

/* ------------------------------------------------------------------------------------------ *
 * RCC, base 0x40021000 (RM0444 chapter 5)
 * ------------------------------------------------------------------------------------------ */

/**
 * RCC_CR: clock control register (RM0444 5.4.1). Offset 0x00, reset 0x00000500. Word or
 * half-word access only.
 *   [25] PLLRDY  r   PLL locked
 *   [24] PLLON   rw  PLL enable; cannot be cleared while the PLL is the system clock
 *   [23] HSI48RDY r  [22] HSI48ON rw  HSI48 oscillator (device dependent)
 *   [19] CSSON   rs  clock security system on HSE; cleared only by reset
 *   [18] HSEBYP  rw  [17] HSERDY r  [16] HSEON rw
 *   [13:11] HSIDIV rw  HSI16 divider for HSISYS: 000 /1, 001 /2, 010 /4, 011 /8, 100 /16,
 *                      101 /32, 110 /64, 111 /128
 *   [10] HSIRDY  r   HSI16 ready (set only while HSION = 1)
 *   [9]  HSIKERON rw force HSI16 on for peripheral kernels, also in Stop
 *   [8]  HSION   rw  HSI16 enable; hardware keeps it on while it is the system clock
 */
#define RCC_CR (*(volatile uint32_t *)0x40021000u)

/**
 * RCC_CFGR: clock configuration register (RM0444 5.4.3). Offset 0x08, reset 0x00000000.
 * Wait states are inserted if accessed during a clock switch or prescaler update.
 *   [31:28] MCOPRE rw  MCO prescaler (0000 /1 ... 0111 /128; /256../1024 on G0B1/G0C1 only)
 *   [27:24] MCOSEL rw  MCO source: 0001 SYSCLK, 0011 HSI16, 0100 HSE, 0101 PLLRCLK, 0110 LSI,
 *                      0111 LSE, 0000 no clock
 *   [23:20] MCO2PRE rw [19:16] MCO2SEL rw  second MCO output (device dependent)
 *   [14:12] PPRE   rw  APB prescaler: 0xx /1, 100 /2, 101 /4, 110 /8, 111 /16
 *   [11:8]  HPRE   rw  AHB prescaler: 0xxx /1, 1000 /2, 1001 /4, 1010 /8, 1011 /16,
 *                      1100 /64, 1101 /128, 1110 /256, 1111 /512
 *   [5:3]   SWS    r   clock in use: 000 HSISYS, 001 HSE, 010 PLLRCLK, 011 LSI, 100 LSE
 *   [2:0]   SW     rw  clock to select, same encoding as SWS. Hardware forces 000 on exit
 *                      from Stop/Standby/Shutdown or on HSE failure.
 */
#define RCC_CFGR (*(volatile uint32_t *)0x40021008u)

/**
 * RCC_PLLCFGR: PLL configuration register (RM0444 5.4.4). Offset 0x0C, reset 0x00001000.
 * fVCO = fPLLIN * N / M; fPLLR = fVCO / R. Write the divider fields only while the PLL is
 * off (PLLON = 0 and PLLRDY = 0).
 *   [31:29] PLLR   rw  R divider: 001 /2, 010 /3, 011 /4 ... 111 /8 (000 reserved); keep
 *                      PLLRCLK <= 64 MHz
 *   [28]    PLLREN rw  PLLRCLK output enable; not writable while it is the system clock
 *   [27:25] PLLQ   rw  Q divider, same encoding as R; PLLQCLK <= 128 MHz
 *   [24]    PLLQEN rw  PLLQCLK output enable
 *   [21:17] PLLP   rw  P divider: 00001 /2 ... 11111 /32; PLLPCLK <= 122 MHz
 *   [16]    PLLPEN rw  PLLPCLK output enable
 *   [14:8]  PLLN   rw  VCO multiplier: 0001000 = 8 ... 1010110 = 86; VCO must be 96..344 MHz
 *   [6:4]   PLLM   rw  input divider: 000 /1, 001 /2 ... 111 /8; PLL input after /M must be
 *                      2.66..16 MHz
 *   [1:0]   PLLSRC rw  PLL input: 00 none, 10 HSI16, 11 HSE (01 reserved)
 */
#define RCC_PLLCFGR (*(volatile uint32_t *)0x4002100Cu)

/**
 * RCC_IOPENR: I/O port clock enable register (RM0444 5.4.13). Offset 0x34, reset 0x00000000.
 * One bit per port, 1 = clock enabled; a GPIO register is not usable until its port is on.
 *   [5] GPIOFEN  [4] GPIOEEN  [3] GPIODEN  [2] GPIOCEN  [1] GPIOBEN  [0] GPIOAEN   (all rw)
 */
#define RCC_IOPENR (*(volatile uint32_t *)0x40021034u)
#define RCC_IOPENR_GPIOAEN (1u << 0)
#define RCC_IOPENR_GPIOBEN (1u << 1)
#define RCC_IOPENR_GPIOCEN (1u << 2)

/**
 * RCC_APBENR1: APB peripheral clock enable register 1 (RM0444 5.4.15). Offset 0x3C,
 * reset 0x00000000. 1 = clock enabled (all rw).
 *   [31] LPTIM1EN [30] LPTIM2EN [29] DAC1EN [28] PWREN [27] DBGEN [26] UCPD2EN [25] UCPD1EN
 *   [24] CECEN [23] I2C3EN [22] I2C2EN [21] I2C1EN [20] LPUART1EN [19] USART4EN
 *   [18] USART3EN [17] USART2EN [16] CRSEN [15] SPI3EN [14] SPI2EN [13] USBEN [12] FDCANEN
 *   [11] WWDGEN [10] RTCAPBEN [9] USART6EN [8] USART5EN [7] LPUART2EN [5] TIM7EN [4] TIM6EN
 *   [2] TIM4EN [1] TIM3EN [0] TIM2EN
 * Several peripherals exist only on some devices (RM0444 1.4).
 */
#define RCC_APBENR1 (*(volatile uint32_t *)0x4002103Cu)
#define RCC_APBENR1_USART2EN (1u << 17)
#define RCC_APBENR1_I2C1EN   (1u << 21)
#define RCC_APBENR1_DBGEN    (1u << 27) /* debug support clock enable */

/**
 * RCC_APBENR2: APB peripheral clock enable register 2 (RM0444 5.4.16). Offset 0x40,
 * reset 0x00000000. 1 = clock enabled (all rw).
 *   [20] ADCEN   [18] TIM17EN  [17] TIM16EN  [16] TIM15EN  [15] TIM14EN  [14] USART1EN
 *   [12] SPI1EN  [11] TIM1EN   [0] SYSCFGEN
 */
#define RCC_APBENR2 (*(volatile uint32_t *)0x40021040u)
#define RCC_APBENR2_USART1EN (1u << 14)
#define RCC_APBENR2_TIM16EN  (1u << 17)
#define RCC_APBENR2_ADCEN    (1u << 20)

/**
 * RCC_CSR: control/status register (RM0444 5.4.24). Offset 0x60, reset 0xXX000000. Up to
 * three wait states on successive accesses. The register is reset by a system reset except for
 * the reset flags, which only a power reset or RMVF clears.
 *   [31] LPWRRSTF r  illegal Stop/Standby/Shutdown entry reset
 *   [30] WWDGRSTF r  window watchdog reset
 *   [29] IWDGRSTF r  independent watchdog reset
 *   [28] SFTRSTF  r  software reset
 *   [27] PWRRSTF  r  BOR or POR/PDR reset
 *   [26] PINRSTF  r  reset from the NRST pin (or any other system reset source)
 *   [25] OBLRSTF  r  option byte loader reset
 *   [23] RMVF     rw write 1 to clear all the reset flags
 *   [1]  LSIRDY   r  LSI ready (can be 1 with LSION = 0 if the IWDG/RTC/CSS requests it)
 *   [0]  LSION    rw LSI oscillator enable
 */
#define RCC_CSR (*(volatile uint32_t *)0x40021060u)

/* ------------------------------------------------------------------------------------------ *
 * FLASH, base 0x40022000 (RM0444 chapter 3)
 * ------------------------------------------------------------------------------------------ */

/**
 * FLASH_ACR: flash access control register (RM0444 3.7.1). Offset 0x000, reset 0x00040600
 * (bit 18 DBG_SWEN set; EMPTY follows the option byte load, not system reset).
 *   [18] DBG_SWEN rw  software enable of debugger read access
 *   [16] EMPTY    rw  main flash looked erased at option byte load
 *   [11] ICRST    rw  instruction cache reset (only writable while ICEN = 0)
 *   [9]  ICEN     rw  instruction cache enable
 *   [8]  PRFTEN   rw  CPU prefetch enable
 *   [2:0] LATENCY rw  wait states: 000 zero, 001 one, 010 two (others reserved). A write takes
 *                     effect when reading back returns the same value.
 */
#define FLASH_ACR (*(volatile uint32_t *)0x40022000u)

/* ------------------------------------------------------------------------------------------ *
 * GPIO, ports A..F at 0x50000000 + 0x400 * n (RM0444 chapter 7, registers 7.5)
 * Fields are per pin y = 0..15 unless noted.
 * ------------------------------------------------------------------------------------------ */

/**
 * GPIOA_MODER: port mode register (RM0444 7.5.1). Offset 0x00, reset 0xEBFFFFFF (port A;
 * 0xFFFFFFFF on the other ports).
 * MODEy[1:0] at bits [2y+1:2y]: 00 input, 01 general-purpose output, 10 alternate function,
 * 11 analog (reset state).
 */
#define GPIOA_MODER (*(volatile uint32_t *)0x50000000u)

/**
 * GPIOA_ODR: port output data register (RM0444 7.5.6). Offset 0x14, reset 0x00000000.
 * OD[15:0] (rw) at bits [15:0], bit y drives pin y when it is an output. Bits 31:16 reserved.
 * For an atomic change use GPIOA_BSRR instead of read-modify-write.
 */
#define GPIOA_ODR (*(volatile uint32_t *)0x50000014u)

/**
 * GPIOA_BSRR: port bit set/reset register (RM0444 7.5.7). Offset 0x18, reset 0x00000000.
 * Write-only (reads 0). One write changes several pins atomically.
 *   [31:16] BR[15:0] w  1 resets ODR bit y
 *   [15:0]  BS[15:0] w  1 sets ODR bit y; BS wins if BSy and BRy are both set
 */
#define GPIOA_BSRR (*(volatile uint32_t *)0x50000018u)

/**
 * GPIOA_AFRL: alternate function low register (RM0444 7.5.9). Offset 0x20, reset 0x00000000.
 * AFSELy[3:0] (rw) at bits [4y+3:4y] for pins 0..7: 0000 AF0 ... 0111 AF7 (others reserved).
 */
#define GPIOA_AFRL (*(volatile uint32_t *)0x50000020u)

/**
 * GPIOB_MODER: port mode register (RM0444 7.5.1). Offset 0x00, reset 0xFFFFFFFF.
 * MODEy[1:0] at bits [2y+1:2y]: 00 input, 01 output, 10 alternate function, 11 analog.
 */
#define GPIOB_MODER (*(volatile uint32_t *)0x50000400u)

/**
 * GPIOB_OTYPER: port output type register (RM0444 7.5.2). Offset 0x04, reset 0x00000000.
 * OT[15:0] (rw) at bits [15:0]: 0 push-pull (reset), 1 open-drain. Bits 31:16 reserved.
 */
#define GPIOB_OTYPER (*(volatile uint32_t *)0x50000404u)

/**
 * GPIOB_OSPEEDR: port output speed register (RM0444 7.5.3). Offset 0x08, reset 0x00000000
 * (0x0C000000 on port A).
 * OSPEEDy[1:0] at bits [2y+1:2y]: 00 very low, 01 low, 10 high, 11 very high speed. See the
 * datasheet for the frequency each setting supports.
 */
#define GPIOB_OSPEEDR (*(volatile uint32_t *)0x50000408u)

/**
 * GPIOB_PUPDR: port pull-up/pull-down register (RM0444 7.5.4). Offset 0x0C, reset 0x00000000
 * (0x24000000 on port A).
 * PUPDy[1:0] at bits [2y+1:2y]: 00 none, 01 pull-up, 10 pull-down, 11 reserved.
 */
#define GPIOB_PUPDR (*(volatile uint32_t *)0x5000040Cu)

/**
 * GPIOB_IDR: port input data register (RM0444 7.5.5). Offset 0x10, reset 0x0000XXXX.
 * ID[15:0] (r) at bits [15:0]: input level of pin y. Bits 31:16 reserved.
 */
#define GPIOB_IDR (*(volatile uint32_t *)0x50000410u)

/**
 * GPIOB_BSRR: port bit set/reset register (RM0444 7.5.7). Offset 0x18, reset 0x00000000.
 * Write-only. [31:16] BR[15:0] reset ODR bit y, [15:0] BS[15:0] set it; BS wins over BR.
 */
#define GPIOB_BSRR (*(volatile uint32_t *)0x50000418u)

/**
 * GPIOB_AFRH: alternate function high register (RM0444 7.5.10). Offset 0x24, reset 0x00000000.
 * AFSELy[3:0] (rw) at bits [4(y-8)+3:4(y-8)] for pins 8..15: 0000 AF0 ... 0111 AF7.
 */
#define GPIOB_AFRH (*(volatile uint32_t *)0x50000424u)

/**
 * GPIOC_MODER: port mode register (RM0444 7.5.1). Offset 0x00, reset 0xFFFFFFFF.
 * MODEy[1:0] at bits [2y+1:2y]: 00 input, 01 output, 10 alternate function, 11 analog.
 */
#define GPIOC_MODER (*(volatile uint32_t *)0x50000800u)

/**
 * GPIOC_AFRL: alternate function low register (RM0444 7.5.9). Offset 0x20, reset 0x00000000.
 * AFSELy[3:0] (rw) at bits [4y+3:4y] for pins 0..7: 0000 AF0 ... 0111 AF7.
 */
#define GPIOC_AFRL (*(volatile uint32_t *)0x50000820u)

/* ------------------------------------------------------------------------------------------ *
 * TIM16, base 0x40014400 (RM0444 chapter 25, registers 25.6). 16-bit registers.
 * ------------------------------------------------------------------------------------------ */

/**
 * TIM16_CR1: control register 1 (RM0444 25.6.1). Offset 0x00, reset 0x0000.
 *   [11] UIFREMAP rw  copy UIF into CNT bit 31
 *   [9:8] CKD     rw  clock division for dead-time/filters: 00 x1, 01 x2, 10 x4
 *   [7]  ARPE     rw  auto-reload preload (ARR buffered)
 *   [3]  OPM      rw  one-pulse mode: stop at the next update event
 *   [2]  URS      rw  update request source: 1 = only overflow raises an update interrupt
 *   [1]  UDIS     rw  update event disable
 *   [0]  CEN      rw  counter enable
 */
#define TIM16_CR1 (*(volatile uint32_t *)0x40014400u)

/**
 * TIM16_DIER: DMA/interrupt enable register (RM0444 25.6.3). Offset 0x0C, reset 0x0000.
 *   [9] CC1DE [8] UDE (DMA requests)  [7] BIE break  [5] COMIE  [1] CC1IE  [0] UIE update
 * interrupt enable (all rw).
 */
#define TIM16_DIER (*(volatile uint32_t *)0x4001440Cu)

/**
 * TIM16_SR: status register (RM0444 25.6.4). Offset 0x10, reset 0x0000. Flags are cleared by
 * writing 0 (rc_w0); writing 1 has no effect.
 *   [9] CC1OF  [7] BIF  [5] COMIF  [1] CC1IF  [0] UIF update interrupt flag (set by hardware
 *   on an update event)
 */
#define TIM16_SR (*(volatile uint32_t *)0x40014410u)

/**
 * TIM16_EGR: event generation register (RM0444 25.6.5). Offset 0x14, reset 0x0000. Write-only,
 * bits self-clear.
 *   [7] BG  [5] COMG  [1] CC1G  [0] UG: re-initialise the counter and update the registers
 *   (also clears the prescaler counter; the prescaler ratio is kept)
 */
#define TIM16_EGR (*(volatile uint32_t *)0x40014414u)

/**
 * TIM16_PSC: prescaler (RM0444 25.6.10). Offset 0x28, reset 0x0000. PSC[15:0] (rw).
 * Counter clock = fCK_PSC / (PSC + 1); the value loads at the next update event.
 */
#define TIM16_PSC (*(volatile uint32_t *)0x40014428u)

/**
 * TIM16_ARR: auto-reload register (RM0444 25.6.11). Offset 0x2C, reset 0xFFFF. ARR[15:0] (rw)
 * is the period value; the counter is blocked while ARR is 0.
 */
#define TIM16_ARR (*(volatile uint32_t *)0x4001442Cu)

/* ------------------------------------------------------------------------------------------ *
 * IWDG, base 0x40003000 (RM0444 chapter 28, registers 28.4). Half-word or word access.
 * ------------------------------------------------------------------------------------------ */

/**
 * IWDG_KR: key register (RM0444 28.4.1). Offset 0x00, reset 0x00000000. KEY[15:0] (w, reads 0).
 *   0xAAAA reload the counter from IWDG_RLR (must happen regularly or the watchdog resets)
 *   0x5555 unlock write access to IWDG_PR, IWDG_RLR and IWDG_WINR
 *   0xCCCC start the watchdog (unless the hardware watchdog option is selected)
 */
#define IWDG_KR (*(volatile uint32_t *)0x40003000u)

/**
 * IWDG_PR: prescaler register (RM0444 28.4.2). Offset 0x04, reset 0x00000000. Write-protected
 * (unlock with 0x5555); wait for IWDG_SR.PVU = 0 before changing.
 * PR[2:0] (rw): 000 /4, 001 /8, 010 /16, 011 /32, 100 /64, 101 /128, 110 /256, 111 /256.
 */
#define IWDG_PR (*(volatile uint32_t *)0x40003004u)

/**
 * IWDG_RLR: reload register (RM0444 28.4.3). Offset 0x08, reset 0x00000FFF. Write-protected;
 * wait for IWDG_SR.RVU = 0 before changing.
 * RL[11:0] (rw): value loaded into the down-counter on each 0xAAAA key write.
 */
#define IWDG_RLR (*(volatile uint32_t *)0x40003008u)

/**
 * IWDG_SR: status register (RM0444 28.4.4). Offset 0x0C, reset 0x00000000. All bits are r and
 * set while an update crosses into the VDD domain (up to five prescaled/LSI cycles).
 *   [2] WVU window value update  [1] RVU reload value update  [0] PVU prescaler value update
 * Wait for the matching bit to clear before writing that register again.
 */
#define IWDG_SR (*(volatile uint32_t *)0x4000300Cu)

/* ------------------------------------------------------------------------------------------ *
 * DBG, base 0x40015800 (RM0444 chapter 40)
 * ------------------------------------------------------------------------------------------ */

/**
 * DBG_APB_FZ1: APB freeze register 1 (RM0444 40.10.3). Offset 0x08, reset 0x00000000 (power-on
 * only, not reset by system reset). 32-bit access only. 1 = clock to the peripheral stops
 * while the core is halted by the debugger.
 *   [31] DBG_LPTIM1_STOP  [30] DBG_LPTIM2_STOP
 *   [22] DBG_I2C2_SMBUS_TIMEOUT  [21] DBG_I2C1_SMBUS_TIMEOUT
 *   [12] DBG_IWDG_STOP  [11] DBG_WWDG_STOP  [10] DBG_RTC_STOP
 *   [5] DBG_TIM7_STOP  [4] DBG_TIM6_STOP  [2] DBG_TIM4_STOP  [1] DBG_TIM3_STOP
 *   [0] DBG_TIM2_STOP
 */
#define DBG_APB_FZ1 (*(volatile uint32_t *)0x40015808u)

/* ------------------------------------------------------------------------------------------ *
 * ADC, base 0x40012400 (RM0444 chapter 15, registers 15.12)
 * ------------------------------------------------------------------------------------------ */

/**
 * ADC_ISR: interrupt and status register (RM0444 15.12.1). Offset 0x00, reset 0x00000000.
 * Flags are rc_w1: write 1 to clear.
 *   [13] CCRDY channel configuration applied (wait for it after writing CHSELR)
 *   [11] EOCAL end of calibration  [9] AWD3  [8] AWD2  [7] AWD1  analog watchdog flags
 *   [4]  OVR   overrun (a conversion finished while EOC was still set)
 *   [3]  EOS   end of sequence    [2] EOC end of conversion (also cleared by reading ADC_DR)
 *   [1]  EOSMP end of sampling    [0] ADRDY ADC ready after ADEN
 */
#define ADC_ISR (*(volatile uint32_t *)0x40012400u)

/**
 * ADC_CR: control register (RM0444 15.12.3). Offset 0x08, reset 0x00000000.
 *   [31] ADCAL    rs  start calibration (only while ADEN/ADSTART/ADSTP/ADDIS are 0); reads 1
 *                     until done
 *   [28] ADVREGEN rw  ADC voltage regulator enable (needed before ADEN; wait tADCVREG_STUP)
 *   [4]  ADSTP    rs  stop and discard the running conversion
 *   [2]  ADSTART  rs  start conversion (needs ADEN = 1, ADDIS = 0; after CHSELR wait for CCRDY)
 *   [1]  ADDIS    rs  disable the ADC; hardware clears ADEN when done
 *   [0]  ADEN     rs  enable the ADC (needs ADCAL, ADSTP, ADSTART, ADDIS = 0 and ADVREGEN = 1);
 *                     ready once ADRDY is set
 */
#define ADC_CR (*(volatile uint32_t *)0x40012408u)

/**
 * ADC_CFGR2: configuration register 2 (RM0444 15.12.5). Offset 0x10, reset 0x00000000. Write
 * only while ADEN = 0.
 *   [31:30] CKMODE rw  00 asynchronous ADCCLK, 01 PCLK/2, 10 PCLK/4, 11 PCLK (50 % duty only)
 *   [29]    LFTRIG rw  low frequency trigger mode
 *   [9]     TOVS   rw  triggered oversampling
 *   [8:5]   OVSS   rw  oversampling right shift 0..8 bits
 *   [4:2]   OVSR   rw  oversampling ratio: 000 2x, 001 4x ... 111 256x
 *   [0]     OVSE   rw  oversampler enable
 */
#define ADC_CFGR2 (*(volatile uint32_t *)0x40012410u)

/**
 * ADC_SMPR: sampling time register (RM0444 15.12.6). Offset 0x14, reset 0x00000000. Write only
 * while ADSTART = 0.
 *   [26:8] SMPSELx rw  channel x uses SMP2 (1) or SMP1 (0)
 *   [6:4]  SMP2    rw  sampling time 2   [2:0] SMP1 rw  sampling time 1
 * SMPx: 000 1.5, 001 3.5, 010 7.5, 011 12.5, 100 19.5, 101 39.5, 110 79.5, 111 160.5 ADC clocks.
 */
#define ADC_SMPR (*(volatile uint32_t *)0x40012414u)

/**
 * ADC_CHSELR: channel selection register (RM0444 15.12.9). Offset 0x28, reset 0x00000000.
 * Write only while ADSTART = 0; a write is ignored if CCRDY from the previous change is pending.
 * CHSELRMOD = 0 (ADC_CFGR1): CHSEL[18:0] (rw), bit x = 1 includes channel x in the sequence.
 * CHSELRMOD = 1: SQ1..SQ8, 4 bits each at [4n-1:4(n-1)], channel number or 1111 = end of
 * sequence.
 */
#define ADC_CHSELR (*(volatile uint32_t *)0x40012428u)

/**
 * ADC_DR: data register (RM0444 15.12.12). Offset 0x40, reset 0x00000000. DATA[15:0] (r) is the
 * last conversion result, left or right aligned per ADC_CFGR1.ALIGN. Reading clears EOC. Right
 * after calibration DATA[6:0] holds the calibration factor.
 */
#define ADC_DR (*(volatile uint32_t *)0x40012440u)

/**
 * ADC_CCR: common configuration register (RM0444 15.12.16). Offset 0x308, reset 0x00000000.
 *   [24] VBATEN rw  VBAT channel enable
 *   [23] TSEN   rw  temperature sensor buffer enable
 *   [22] VREFEN rw  VREFINT buffer enable (clear TSEN/VREFEN before low-power modes)
 *   [21:18] PRESC rw  ADC clock divider: 0000 /1, 0001 /2, 0010 /4, 0011 /6, 0100 /8, 0101 /10,
 *                     0110 /12, 0111 /16, 1000 /32, 1001 /64, 1010 /128, 1011 /256
 * Write only while ADSTART = 0 (PRESC: ADC disabled).
 */
#define ADC_CCR (*(volatile uint32_t *)0x40012708u)

/* ------------------------------------------------------------------------------------------ *
 * I2C1, base 0x40005400 (RM0444 chapter 32, registers 32.9). Word access.
 * ------------------------------------------------------------------------------------------ */

/**
 * I2C1_CR1: control register 1 (RM0444 32.9.1). Offset 0x00, reset 0x00000000.
 *   [23] PECEN  [22] ALERTEN  [21] SMBDEN  [20] SMBHEN  [19] GCEN  [18] WUPEN
 *   [17] NOSTRETCH  [16] SBC  [15] RXDMAEN  [14] TXDMAEN
 *   [12] ANFOFF analog noise filter off (PE = 0 only)
 *   [11:8] DNF digital noise filter, spikes up to DNF * tI2CCLK (PE = 0 only)
 *   [7] ERRIE  [6] TCIE  [5] STOPIE  [4] NACKIE  [3] ADDRIE  [2] RXIE  [1] TXIE
 *   [0] PE peripheral enable; clearing it releases SCL/SDA and resets state, keep it low for
 *       at least three APB cycles
 */
#define I2C1_CR1 (*(volatile uint32_t *)0x40005400u)

/**
 * I2C1_CR2: control register 2 (RM0444 32.9.2). Offset 0x04, reset 0x00000000.
 *   [25] AUTOEND  automatic STOP after NBYTES (ignored if RELOAD = 1)
 *   [24] RELOAD   NBYTES reload mode; TCR is set after each NBYTES
 *   [23:16] NBYTES number of bytes to transfer (do not change while START is set)
 *   [15] NACK (target mode)  [14] STOP rs  generate STOP after the current byte
 *   [13] START rs generate (re)START; cleared by hardware once sent
 *   [11] ADD10 10-bit addressing  [10] RD_WRN 0 write, 1 read
 *   [9:0] SADD target address; in 7-bit mode the address is in SADD[7:1]
 * START/STOP/NACK are set-only: writing 0 has no effect.
 */
#define I2C1_CR2 (*(volatile uint32_t *)0x40005404u)

/**
 * I2C1_TIMINGR: timing register (RM0444 32.9.5). Offset 0x10, reset 0x00000000. Configure
 * with PE = 0. tPRESC = (PRESC + 1) * tI2CCLK.
 *   [31:28] PRESC  timing prescaler
 *   [23:20] SCLDEL data setup: (SCLDEL + 1) * tPRESC
 *   [19:16] SDADEL data hold: SDADEL * tPRESC
 *   [15:8]  SCLH   SCL high period: (SCLH + 1) * tPRESC
 *   [7:0]   SCLL   SCL low period: (SCLL + 1) * tPRESC
 */
#define I2C1_TIMINGR (*(volatile uint32_t *)0x40005410u)

/**
 * I2C1_ISR: interrupt and status register (RM0444 32.9.7). Offset 0x18, reset 0x00000001.
 *   [23:17] ADDCODE  [16] DIR   (target mode only)
 *   [15] BUSY  bus busy between START and STOP
 *   [10] OVR  [9] ARLO arbitration lost  [8] BERR bus error   (cleared via ICR)
 *   [7]  TCR  transfer complete reload (cleared by writing a non-zero NBYTES)
 *   [6]  TC   transfer complete (AUTOEND = 0, RELOAD = 0); cleared by START or STOP
 *   [5]  STOPF STOP detected   [4] NACKF NACK received   [3] ADDR (target)  (cleared via ICR)
 *   [2]  RXNE receive data ready   [1] TXIS transmit data needed (write TXDR)
 *   [0]  TXE  transmit data register empty
 * ARLO/BERR/OVR/STOPF/NACKF/ADDR are cleared by writing the matching bit in I2C1_ICR.
 */
#define I2C1_ISR (*(volatile uint32_t *)0x40005418u)

/**
 * I2C1_ICR: interrupt clear register (RM0444 32.9.8). Offset 0x1C, reset 0x00000000. Write 1
 * to clear the matching I2C1_ISR flag (all w).
 *   [13] ALERTCF  [12] TIMOUTCF  [11] PECCF  [10] OVRCF  [9] ARLOCF  [8] BERRCF
 *   [5] STOPCF  [4] NACKCF  [3] ADDRCF (also clears CR2.START)
 */
#define I2C1_ICR (*(volatile uint32_t *)0x4000541Cu)

/**
 * I2C1_TXDR: transmit data register (RM0444 32.9.11). Offset 0x28, reset 0x00000000.
 * TXDATA[7:0] (rw): byte to send; write only when TXE = 1 (or on a TXIS event).
 */
#define I2C1_TXDR (*(volatile uint32_t *)0x40005428u)

/* ------------------------------------------------------------------------------------------ *
 * USART1 (0x40013800) and USART2 (0x40004400), RM0444 chapter 33, registers 33.8. Word access.
 * Both instances share one layout. ISR and CR1 are described with FIFO mode disabled
 * (33.8.2, 33.8.10); with FIFOEN = 1 the RXNE/TXE bits become RXFNE/TXFNF (33.8.1, 33.8.9).
 *
 * A source file sees only its own instance: define REGS_USART1 or REGS_USART2 before including
 * this header, so using the other USART's registers is a compile error.
 * ------------------------------------------------------------------------------------------ */

/**
 * USARTx_CR1: control register 1 (RM0444 33.8.2). Offset 0x00, reset 0x00000000. Most fields
 * can only be written while UE = 0.
 *   [29] FIFOEN  [28] M1 and [12] M0 word length: M1M0 00 = 8 data bits, 01 = 9, 10 = 7
 *   [27] EOBIE  [26] RTOIE  [25:21] DEAT  [20:16] DEDT
 *   [15] OVER8 oversampling by 8 (1) or 16 (0)   [14] CMIE  [13] MME  [11] WAKE
 *   [10] PCE parity enable   [9] PS parity odd (1) / even (0)   [8] PEIE
 *   [7] TXEIE  [6] TCIE  [5] RXNEIE  [4] IDLEIE   (interrupt enables)
 *   [3] TE transmitter enable   [2] RE receiver enable   [1] UESM wake from low power
 *   [0] UE USART enable; clearing it discards ongoing work and resets the ISR flags
 */
#ifdef REGS_USART1
#define USART1_CR1 (*(volatile uint32_t *)0x40013800u)
#endif
/** USART2_CR1: same layout as USART1_CR1 (RM0444 33.8.2), instance at 0x40004400. */
#ifdef REGS_USART2
#define USART2_CR1 (*(volatile uint32_t *)0x40004400u)
#endif

/**
 * USARTx_BRR: baud rate register (RM0444 33.8.5). Offset 0x0C, reset 0x00000000. Write only
 * while UE = 0. BRR[15:0] (rw): with OVER8 = 0, BRR = USARTDIV; with OVER8 = 1, BRR[15:4] =
 * USARTDIV[15:4], BRR[2:0] = USARTDIV[3:0] >> 1 and BRR[3] must stay 0.
 */
#ifdef REGS_USART1
#define USART1_BRR (*(volatile uint32_t *)0x4001380Cu)
#endif
/** USART2_BRR: same layout as USART1_BRR (RM0444 33.8.5), instance at 0x40004400. */
#ifdef REGS_USART2
#define USART2_BRR (*(volatile uint32_t *)0x4000440Cu)
#endif

/**
 * USARTx_ISR: interrupt and status register (RM0444 33.8.10, FIFO mode disabled). Offset 0x1C,
 * reset 0x000000C0. All flags are r; clear them through USARTx_ICR.
 *   [22] REACK  [21] TEACK  [20] WUF  [19] RWU  [18] SBKF  [17] CMF  [16] BUSY receiving
 *   [15] ABRF  [14] ABRE  [13] UDR  [12] EOBF  [11] RTOF  [10] CTS  [9] CTSIF  [8] LBDF
 *   [7] TXE transmit data register empty (write TDR when set)
 *   [6] TC  transmission complete
 *   [5] RXNE received data ready (read RDR)
 *   [4] IDLE idle line   [3] ORE overrun   [2] NE noise   [1] FE framing error   [0] PE parity
 */
#ifdef REGS_USART1
#define USART1_ISR (*(volatile uint32_t *)0x4001381Cu)
#endif
/** USART2_ISR: same layout as USART1_ISR (RM0444 33.8.10), instance at 0x40004400. */
#ifdef REGS_USART2
#define USART2_ISR (*(volatile uint32_t *)0x4000441Cu)
#endif

/**
 * USARTx_ICR: interrupt flag clear register (RM0444 33.8.11). Offset 0x20, reset 0x00000000.
 * Write 1 to clear the matching USARTx_ISR flag (all w).
 *   [20] WUCF  [17] CMCF  [13] UDRCF  [12] EOBCF  [11] RTOCF  [9] CTSCF  [8] LBDCF
 *   [7] TCBGTCF  [6] TCCF  [5] TXFECF  [4] IDLECF  [3] ORECF  [2] NECF  [1] FECF  [0] PECF
 */
#ifdef REGS_USART1
#define USART1_ICR (*(volatile uint32_t *)0x40013820u)
#endif
/** USART2_ICR: same layout as USART1_ICR (RM0444 33.8.11), instance at 0x40004400. */
#ifdef REGS_USART2
#define USART2_ICR (*(volatile uint32_t *)0x40004420u)
#endif

/**
 * USARTx_RDR: receive data register (RM0444 33.8.12). Offset 0x24, reset 0x00000000.
 * RDR[8:0] (r): received character; with parity enabled the top bit is the parity bit.
 */
#ifdef REGS_USART1
#define USART1_RDR (*(volatile uint32_t *)0x40013824u)
#endif
/** USART2_RDR: same layout as USART1_RDR (RM0444 33.8.12), instance at 0x40004400. */
#ifdef REGS_USART2
#define USART2_RDR (*(volatile uint32_t *)0x40004424u)
#endif

/**
 * USARTx_TDR: transmit data register (RM0444 33.8.13). Offset 0x28, reset 0x00000000.
 * TDR[8:0] (rw): character to send; write only when TXE (TXFNF in FIFO mode) = 1. With parity
 * enabled the top data bit is replaced by the parity.
 */
#ifdef REGS_USART1
#define USART1_TDR (*(volatile uint32_t *)0x40013828u)
#endif
/** USART2_TDR: same layout as USART1_TDR (RM0444 33.8.13), instance at 0x40004400. */
#ifdef REGS_USART2
#define USART2_TDR (*(volatile uint32_t *)0x40004428u)
#endif

/* ------------------------------------------------------------------------------------------ *
 * Cortex-M0+ core peripherals (PM0223 chapter 4)
 * ------------------------------------------------------------------------------------------ */

/**
 * NVIC_ISER: interrupt set-enable register (PM0223 4.2.2). Address 0xE000E100, reset
 * 0x00000000. SETENA[31:0] (rs): write 1 to enable interrupt n (bit n); 0 has no effect. Read
 * back 1 means enabled. Use NVIC_ICER (0xE000E180) to disable.
 */
#define NVIC_ISER (*(volatile uint32_t *)0xE000E100u)

/**
 * STK_CSR: SysTick control and status register (PM0223 4.4.1). Address 0xE000E010, reset
 * 0x00000000.
 *   [16] COUNTFLAG rc_r  1 if the timer reached 0 since the last read of this register
 *   [2]  CLKSOURCE rw    0 external reference clock, 1 processor clock
 *   [1]  TICKINT   rw    1 = counting down to 0 raises the SysTick exception
 *   [0]  ENABLE    rw    counter enable
 */
#define STK_CSR (*(volatile uint32_t *)0xE000E010u)

/**
 * STK_RVR: SysTick reload value register (PM0223 4.4.2). Address 0xE000E014, reset unknown.
 * RELOAD[23:0] (rw): loaded into STK_CVR when the counter is enabled and when it reaches 0.
 * Valid range 0x000001..0xFFFFFF; for a period of N processor clocks use N - 1.
 */
#define STK_RVR (*(volatile uint32_t *)0xE000E014u)

/**
 * STK_CVR: SysTick current value register (PM0223 4.4.3). Address 0xE000E018, reset unknown.
 * CURRENT[23:0] (rw): reads the current count; any write clears it to 0 and clears COUNTFLAG.
 */
#define STK_CVR (*(volatile uint32_t *)0xE000E018u)

#endif
