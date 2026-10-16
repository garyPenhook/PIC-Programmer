#include "bridge.h"

#include "platform.h"
#include "target_uart.h"
#include "uart.h"

static const char exit_sequence[] = BRIDGE_EXIT;
#define EXIT_LEN (sizeof exit_sequence - 1u)

int bridge_match(bridge_matcher_t *m, uint8_t byte, void (*emit)(uint8_t byte))
{
    for (;;) {
        if (byte == (uint8_t)exit_sequence[m->matched]) {
            m->matched++;
            if (m->matched == EXIT_LEN) {
                m->matched = 0u;
                return 1;
            }
            return 0;
        }
        if (m->matched == 0u) {
            emit(byte);
            return 0;
        }
        /* Not the sequence after all: send the first held byte and try again from the
         * next one. The held bytes are a prefix of the sequence, so they can be
         * replayed from it. */
        uint32_t held = m->matched;
        emit((uint8_t)exit_sequence[0]);
        m->matched = 0u;
        for (uint32_t i = 1; i < held; i++) {
            (void)bridge_match(m, (uint8_t)exit_sequence[i], emit);
        }
    }
}

/* Polls without the target transmitter taking a byte before the final drain gives up.
 * Far longer than one byte at the slowest baud rate (8 ms at 1200 baud). */
#define DRAIN_TRIES 2000000u

/* PC-to-target bytes waiting for the slower target UART. */
static uint8_t to_target[BRIDGE_BUFFER];
static uint32_t head;
static uint32_t tail;
static uint32_t dropped;

static void queue_for_target(uint8_t byte)
{
    uint32_t next = (head + 1u) % BRIDGE_BUFFER;

    if (next == tail) {
        dropped++;
        return;
    }
    to_target[head] = byte;
    head = next;
}

uint32_t bridge_run(void)
{
    bridge_matcher_t matcher = {0u};
    uint8_t from_target = 0u;
    int holding = 0; /* a target byte waiting for the PC transmitter */

    head = tail = dropped = 0u;
    for (;;) {
        uint8_t byte;

        platform_watchdog_kick();
        if (uart_read_byte(&byte) > 0 && bridge_match(&matcher, byte, queue_for_target)) {
            break;
        }
        if (tail != head && target_uart_try_write(to_target[tail])) {
            tail = (tail + 1u) % BRIDGE_BUFFER;
        }
        /* The PC link is faster than any target baud rate, so one byte of holding is
         * enough in this direction. */
        if (!holding && target_uart_read(&from_target) > 0) {
            holding = 1;
        }
        if (holding && uart_try_write_byte(from_target)) {
            holding = 0;
        }
    }
    /* Let what the PC already sent reach the target, but give up (and count the rest as
     * dropped) if the transmitter stops taking bytes. */
    for (uint32_t idle = 0; tail != head && idle < DRAIN_TRIES;) {
        platform_watchdog_kick();
        if (target_uart_try_write(to_target[tail])) {
            tail = (tail + 1u) % BRIDGE_BUFFER;
            idle = 0u;
        } else {
            idle++;
        }
    }
    dropped += (head + BRIDGE_BUFFER - tail) % BRIDGE_BUFFER;
    return dropped;
}
