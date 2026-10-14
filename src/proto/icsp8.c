#include "icsp8.h"

#include "icsp.h"

#define LVP_KEY      0x4D434850u
#define PAYLOAD_BITS 24u

#define T_CK_US   1u
#define T_DLY_US  2u
#define T_EXIT_US 1000u

/* One clock with the data line set up first. Data is latched on the falling edge. */
static void send_bit(uint8_t bit)
{
    icsp_set_dat(bit);
    icsp_set_clk(1u);
    icsp_delay_us(T_CK_US);
    icsp_set_clk(0u);
    icsp_delay_us(T_CK_US);
}

static void send_bits(uint32_t value, uint32_t count)
{
    for (uint32_t i = count; i > 0u; i--) {
        send_bit((uint8_t)((value >> (i - 1u)) & 1u));
    }
}

void icsp8_enter(uint32_t hold_us)
{
    /* Both lines low before MCLR falls, so the target sees a clean start. */
    icsp_set_clk(0u);
    icsp_set_dat(0u);
    icsp_delay_us(T_DLY_US);

    icsp_set_mclr(0u);
    icsp_delay_us(hold_us);

    send_bits(LVP_KEY, 32u);               /* MSb first */
    icsp_delay_us(hold_us);
}

void icsp8_exit(void)
{
    icsp_set_clk(0u);
    icsp_release_dat();
    icsp_set_mclr(1u);                     /* raise MCLR: the target leaves programming mode */
    icsp_delay_us(T_EXIT_US);
}

void icsp8_command(uint8_t cmd)
{
    send_bits(cmd, 8u);
    icsp_delay_us(T_DLY_US);
}

void icsp8_payload(uint32_t value)
{
    send_bits(value << 1, PAYLOAD_BITS);   /* value sits between the start and stop bit */
    icsp_delay_us(T_DLY_US);
}

uint32_t icsp8_read(uint8_t cmd)
{
    uint32_t raw = 0u;

    icsp8_command(cmd);

    /* The target takes over ICSPDAT on the first falling edge, so let go of it now. */
    icsp_release_dat();
    icsp_delay_us(T_DLY_US);

    /* Sample just before each falling edge, after a full clock high time. */
    for (uint32_t i = 0; i < PAYLOAD_BITS; i++) {
        icsp_set_clk(1u);
        icsp_delay_us(T_CK_US);
        raw = (raw << 1) | icsp_get_dat();
        icsp_set_clk(0u);
        icsp_delay_us(T_CK_US);
    }
    icsp_delay_us(T_DLY_US);
    return raw >> 1;                       /* drop the stop bit */
}
