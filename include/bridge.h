#ifndef BRIDGE_H
#define BRIDGE_H

#include <stdint.h>

/*
 * Transparent serial bridge between the PC (USART2, the virtual COM port) and the target's
 * serial port (USART1, target_uart.h), for BRIDGE <baud>.
 *
 * Every byte from the PC goes to the target and every byte from the target goes to the
 * PC, until the PC sends BRIDGE_EXIT. That sequence is held back while it is being
 * matched, so the target never sees it; a partial match that turns out to be data is sent
 * on. PC bytes arrive at 921600 baud but may leave at a much lower target baud rate, so
 * they wait in a BRIDGE_BUFFER-byte buffer; past that they are dropped and counted.
 */
#define BRIDGE_EXIT   "\x1b[picprog-bridge-exit]"
#define BRIDGE_BUFFER 2048u

/* Run the bridge until the exit sequence. Returns the number of PC bytes dropped. */
uint32_t bridge_run(void);

/* Exit-sequence matcher, separate so it can be tested on its own. Feed every PC byte.
 * Returns 1 once the whole sequence has arrived. Bytes that turn out not to belong to the
 * sequence are passed to `emit`, in order. */
typedef struct {
    uint32_t matched;
} bridge_matcher_t;

int bridge_match(bridge_matcher_t *m, uint8_t byte, void (*emit)(uint8_t byte));

#endif
