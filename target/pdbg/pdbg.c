#include "pdbg.h"

#include "pdbg_port.h"

#define BP_BYTES (PDBG_BP_SITES / 8u)

static struct {
    uint8_t rx[PDBG_RX_MAX];
    uint8_t rx_len;
    uint8_t rx_overflow;  /* the frame in progress was too long: skip it */
    uint8_t halted;
    uint8_t halt_reason;
    uint8_t halt_arg;
    uint8_t bp[BP_BYTES];
} st;

static uint8_t tx[PDBG_TX_MAX + 2u];

uint16_t pdbg_crc16(const uint8_t *data, uint8_t len)
{
    uint16_t crc = 0xFFFFu;
    uint8_t i;
    uint8_t bit;

    for (i = 0u; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (bit = 0u; bit < 8u; bit++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

/* COBS-encode tx[0..len) plus its CRC and send it with the delimiter. */
static void send_frame(uint8_t len)
{
    uint16_t crc = pdbg_crc16(tx, len);
    uint8_t total;
    uint8_t pos = 0u;
    uint8_t n;
    uint8_t k;

    tx[len] = (uint8_t)crc;
    tx[len + 1u] = (uint8_t)(crc >> 8);
    total = (uint8_t)(len + 2u);
    for (;;) {
        n = 0u;
        while ((uint8_t)(pos + n) < total && tx[pos + n] != 0u && n < 254u) {
            n++;
        }
        pdbg_port_tx((uint8_t)(n + 1u));
        for (k = 0u; k < n; k++) {
            pdbg_port_tx(tx[pos + k]);
        }
        pos = (uint8_t)(pos + n);
        if (pos >= total) {
            break;
        }
        if (n < 254u) {
            pos++;  /* the zero this block stood in for */
        }
    }
    pdbg_port_tx(0u);
}

/* Decode in place. Returns the decoded length, 0 if the encoding is invalid. */
static uint8_t cobs_decode(uint8_t *b, uint8_t len)
{
    uint8_t r = 0u;
    uint8_t w = 0u;
    uint8_t code;
    uint8_t n;

    while (r < len) {
        code = b[r++];
        if (code == 0u) {
            return 0u;
        }
        n = (uint8_t)(code - 1u);
        if ((uint8_t)(len - r) < n) {
            return 0u;
        }
        while (n-- != 0u) {
            b[w++] = b[r++];
        }
        if (code != 0xFFu && r < len) {
            b[w++] = 0u;
        }
    }
    return w;
}

static uint16_t u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (uint16_t)((uint16_t)p[1] << 8));
}

static uint8_t put3(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    return 3u;
}

static uint8_t put2(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    return 2u;
}

static void send_halted_event(void)
{
    pdbg_regs_t r;

    pdbg_port_regs(&r);
    tx[0] = PDBG_CH_EVENT;
    tx[1] = PDBG_EV_HALTED;
    tx[2] = st.halt_reason;
    tx[3] = st.halt_arg;
    put3(&tx[4], r.pc);
    send_frame(7u);
}

/* st.rx[0..n) is a checked request: cmd, seq, args. */
static uint8_t serve(uint8_t n)
{
    const uint8_t cmd = st.rx[0];
    const uint8_t seq = st.rx[1];
    const uint8_t *arg = &st.rx[2];
    const uint8_t alen = (uint8_t)(n - 2u);
    uint8_t *out = &tx[4];
    uint8_t status = PDBG_OK;
    uint8_t dlen = 0u;
    uint8_t action = PDBG_NONE;
    uint8_t reset = 0u;
    uint8_t i;

    switch (cmd) {
    case PDBG_CMD_HELLO: {
        pdbg_info_t info;

        pdbg_port_info(&info);
        out[0] = PDBG_VERSION;
        out[1] = PDBG_MAX_DATA;
        out[2] = PDBG_BP_SITES;
        put2(&out[3], info.device_id);
        put2(&out[5], info.revision);
        out[7] = info.reset_cause;
        out[8] = st.halted;
        dlen = 9u;
        break;
    }
    case PDBG_CMD_HALT:
        if (!st.halted) {
            st.halt_reason = PDBG_REASON_REQUEST;
            st.halt_arg = 0u;
            action = PDBG_HALT;
        }
        break;
    case PDBG_CMD_CONT:
        if (!st.halted) {
            status = PDBG_ERR_STATE;
        } else {
            st.halted = 0u;
        }
        break;
    case PDBG_CMD_REGS:
        if (!st.halted) {
            status = PDBG_ERR_STATE;
        } else {
            pdbg_regs_t r;

            pdbg_port_regs(&r);
            dlen = put3(out, r.pc);
            out[dlen++] = r.wreg;
            out[dlen++] = r.status;
            out[dlen++] = r.bsr;
            dlen = (uint8_t)(dlen + put2(&out[dlen], r.fsr0));
            dlen = (uint8_t)(dlen + put2(&out[dlen], r.fsr1));
            dlen = (uint8_t)(dlen + put2(&out[dlen], r.fsr2));
            dlen = (uint8_t)(dlen + put2(&out[dlen], r.prod));
            dlen = (uint8_t)(dlen + put3(&out[dlen], r.tblptr));
            out[dlen++] = r.tablat;
            out[dlen++] = r.stkptr;
        }
        break;
    case PDBG_CMD_STACK:
        if (!st.halted) {
            status = PDBG_ERR_STATE;
        } else if (alen != 1u || arg[0] > pdbg_port_stack_depth()) {
            status = PDBG_ERR_ARG;
        } else {
            const uint8_t depth = pdbg_port_stack_depth();
            uint8_t count = (uint8_t)(depth - arg[0]);

            if (count > PDBG_MAX_STACK) {
                count = PDBG_MAX_STACK;
            }
            out[0] = depth;
            out[1] = count;
            dlen = 2u;
            for (i = 0u; i < count; i++) {
                dlen = (uint8_t)(dlen + put3(&out[dlen],
                                             pdbg_port_stack_entry((uint8_t)(arg[0] + i))));
            }
        }
        break;
    case PDBG_CMD_RD:
        if (alen != 3u || arg[2] == 0u || arg[2] > PDBG_MAX_DATA) {
            status = PDBG_ERR_ARG;
        } else {
            for (i = 0u; i < arg[2]; i++) {
                out[i] = pdbg_port_rd((uint16_t)(u16(arg) + i));
            }
            dlen = arg[2];
        }
        break;
    case PDBG_CMD_WR:
        if (!st.halted) {
            status = PDBG_ERR_STATE;
        } else if (alen < 3u || alen > 2u + PDBG_MAX_DATA) {
            status = PDBG_ERR_ARG;
        } else {
            for (i = 2u; i < alen; i++) {
                pdbg_port_wr((uint16_t)(u16(arg) + i - 2u), arg[i]);
            }
        }
        break;
    case PDBG_CMD_RDPFM:
        if (alen != 4u || arg[3] == 0u || arg[3] > PDBG_MAX_DATA) {
            status = PDBG_ERR_ARG;
        } else {
            const uint32_t addr = (uint32_t)u16(arg) | ((uint32_t)arg[2] << 16);

            for (i = 0u; i < arg[3]; i++) {
                out[i] = pdbg_port_rdpfm(addr + i);
            }
            dlen = arg[3];
        }
        break;
    case PDBG_CMD_BP:
        if (alen == 2u && arg[0] < PDBG_BP_SITES && arg[1] <= 1u) {
            const uint8_t bit = (uint8_t)(1u << (arg[0] & 7u));

            if (arg[1] != 0u) {
                st.bp[arg[0] >> 3] |= bit;
            } else {
                st.bp[arg[0] >> 3] &= (uint8_t)~bit;
            }
        } else if (alen != 0u) {
            status = PDBG_ERR_ARG;
            break;
        }
        for (i = 0u; i < BP_BYTES; i++) {
            out[i] = st.bp[i];
        }
        dlen = BP_BYTES;
        break;
    case PDBG_CMD_RESET:
        reset = 1u;
        break;
    default:
        status = PDBG_ERR_CMD;
        break;
    }

    tx[0] = PDBG_CH_REPLY;
    tx[1] = seq;
    tx[2] = cmd;
    tx[3] = status;
    send_frame((uint8_t)(4u + dlen));
    if (reset) {
        pdbg_port_reset();
    }
    return action;
}

void pdbg_init(void)
{
    uint8_t i;

    st.rx_len = 0u;
    st.rx_overflow = 0u;
    st.halted = 0u;
    for (i = 0u; i < BP_BYTES; i++) {
        st.bp[i] = 0u;
    }
}

uint8_t pdbg_feed(uint8_t byte)
{
    uint8_t n;
    uint8_t action = PDBG_NONE;

    if (byte != 0u) {
        if (st.rx_len < PDBG_RX_MAX) {
            st.rx[st.rx_len++] = byte;
        } else {
            st.rx_overflow = 1u;
        }
        return PDBG_NONE;
    }

    /* delimiter */
    n = st.rx_len;
    st.rx_len = 0u;
    if (st.rx_overflow) {
        st.rx_overflow = 0u;
        return PDBG_NONE;
    }
    if (n == 0u) {  /* empty frame: halt, or say again that we are */
        if (st.halted) {
            send_halted_event();
        } else {
            st.halt_reason = PDBG_REASON_REQUEST;
            st.halt_arg = 0u;
            action = PDBG_HALT;
        }
        return action;
    }
    n = cobs_decode(st.rx, n);
    if (n < 4u) {
        return PDBG_NONE;
    }
    n = (uint8_t)(n - 2u);
    if (pdbg_crc16(st.rx, n) != u16(&st.rx[n])) {
        return PDBG_NONE;
    }
    return serve(n);
}

void pdbg_poll(void)
{
    uint8_t byte;

    while (pdbg_port_rx(&byte)) {
        if (pdbg_feed(byte) == PDBG_HALT) {
            pdbg_enter(st.halt_reason, st.halt_arg);
        }
    }
}

void pdbg_enter(uint8_t reason, uint8_t arg)
{
    uint8_t byte;

    st.halted = 1u;
    st.halt_reason = reason;
    st.halt_arg = arg;
    send_halted_event();
    while (st.halted) {
        pdbg_port_service();
        if (pdbg_port_rx(&byte)) {
            (void)pdbg_feed(byte);
        }
    }
}

void pdbg_halt(uint8_t reason, uint8_t arg)
{
    const uint8_t irq = pdbg_port_irq_off();

    pdbg_enter(reason, arg);
    pdbg_port_irq_restore(irq);
}

void pdbg_break(uint8_t site)
{
    if (site < PDBG_BP_SITES && (st.bp[site >> 3] & (1u << (site & 7u))) != 0u) {
        pdbg_halt(PDBG_REASON_BREAK, site);
    }
}

/* The reply buffer is shared with the receive interrupt, so keep it out while we use it. */
void pdbg_console(const uint8_t *text, uint8_t len)
{
    const uint8_t irq = pdbg_port_irq_off();
    uint8_t n;
    uint8_t i;

    while (len != 0u) {
        n = len < PDBG_MAX_DATA ? len : (uint8_t)PDBG_MAX_DATA;
        tx[0] = PDBG_CH_CONSOLE;
        for (i = 0u; i < n; i++) {
            tx[1u + i] = text[i];
        }
        send_frame((uint8_t)(1u + n));
        text += n;
        len = (uint8_t)(len - n);
    }
    pdbg_port_irq_restore(irq);
}
