#ifndef PDBG_H
#define PDBG_H

#include <stdint.h>

/*
 * Debug agent core: the wire protocol and the commands (DEBUGGER.md). Everything that
 * touches the part (EUSART1, memory, the saved context) is behind pdbg_port.h, so this
 * file builds with gcc for the tests and with XC8 for the PIC18F27Q10.
 *
 * Frames: payload + CRC-16/CCITT-FALSE (low byte first), COBS-encoded, then a 0x00.
 * A 0x00 with nothing before it is an empty frame and means "halt".
 *
 * PC to agent:  cmd, seq, args...
 * Agent to PC:  PDBG_CH_REPLY,   seq, cmd, status, data...
 *               PDBG_CH_EVENT,   PDBG_EV_HALTED, reason, arg, pc (3 bytes)
 *               PDBG_CH_CONSOLE, text...
 * Multi-byte values are low byte first. A frame that fails COBS or CRC is dropped without
 * an answer; the PC times out and asks again.
 */

#define PDBG_VERSION      1u
#define PDBG_RX_MAX       48u  /* encoded request, delimiter not counted */
#define PDBG_TX_MAX       36u  /* reply payload, CRC not counted */
#define PDBG_MAX_DATA     32u  /* bytes per RD, WR, RDPFM and console frame */
#define PDBG_MAX_STACK    10u  /* return addresses per STACK reply */
#define PDBG_BP_SITES     32u

#define PDBG_CH_REPLY     1u
#define PDBG_CH_CONSOLE   2u
#define PDBG_CH_EVENT     3u

#define PDBG_EV_HALTED    1u

#define PDBG_REASON_REQUEST 1u  /* the PC asked */
#define PDBG_REASON_BREAK   2u  /* arg = site number */
#define PDBG_REASON_ASSERT  3u

#define PDBG_CMD_HELLO    1u  /* -> version, max data, bp sites, device id (2), revision (2),
                                 reset cause, halted */
#define PDBG_CMD_HALT     2u
#define PDBG_CMD_CONT     3u  /* halted only */
#define PDBG_CMD_REGS     4u  /* halted only -> pc (3), wreg, status, bsr, fsr0 (2), fsr1 (2),
                                 fsr2 (2), prod (2), tblptr (3), tablat, stkptr */
#define PDBG_CMD_STACK    5u  /* start -> depth, count, count x 3-byte address (top first);
                                 halted only */
#define PDBG_CMD_RD       6u  /* addr (2), len -> data */
#define PDBG_CMD_WR       7u  /* addr (2), data...; halted only */
#define PDBG_CMD_RDPFM    8u  /* addr (3), len -> data */
#define PDBG_CMD_BP       9u  /* [site, on] -> enabled sites as a 32-bit mask */
#define PDBG_CMD_RESET    10u /* answers, then resets the part */

#define PDBG_OK           0u
#define PDBG_ERR_CMD      1u  /* unknown command */
#define PDBG_ERR_ARG      2u  /* wrong length or out of range */
#define PDBG_ERR_STATE    3u  /* needs the agent halted */

/* What pdbg_feed() asks of its caller. */
#define PDBG_NONE         0u
#define PDBG_HALT         1u  /* running: call pdbg_enter() */

uint16_t pdbg_crc16(const uint8_t *data, uint8_t len);

/* Initialise the state. Breakpoint sites start disabled. */
void pdbg_init(void);

/* Feed one received byte. Serves a complete request on the spot (the reply goes out
 * through pdbg_port_tx). Returns PDBG_HALT when running and the PC asked to halt. */
uint8_t pdbg_feed(uint8_t byte);

/* Running: take every byte the port has, entering the halted loop on a halt request. */
void pdbg_poll(void);

/* Halted loop: announce the halt, then serve requests until CONT. Interrupts must be off. */
void pdbg_enter(uint8_t reason, uint8_t arg);

/* Halt now with interrupts off for the duration. */
void pdbg_halt(uint8_t reason, uint8_t arg);

/* A compiled breakpoint site: halts if the PC enabled site n. */
void pdbg_break(uint8_t site);

/* Send application text to the PC (the console channel). */
void pdbg_console(const uint8_t *text, uint8_t len);

#define PDBG_BREAK(n)     pdbg_break(n)
#define PDBG_ASSERT(cond) do { if (!(cond)) { pdbg_halt(PDBG_REASON_ASSERT, 0u); } } while (0)

#endif
