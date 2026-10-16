#ifndef CMD_H
#define CMD_H

#include <stdint.h>

#include "pic.h"

/*
 * Line-based command interface over the serial port (see uart.h). Each command is one
 * line ending in '\n' ('\r' is ignored); commands are upper case and fields are separated
 * by spaces. Every command ends with exactly one "OK ..." or "ERR ..." line.
 *
 *   PROBE
 *       Identify the target and remember the device for later commands.
 *       OK <name> <device id, 4 hex digits> <revision id, 4 hex digits>
 *
 *   READ <region> <offset> <count>
 *       Read count cells of region (flash, userid, config or eeprom) starting at offset.
 *       Numbers are decimal or 0x hex. Needs a successful PROBE first. The target is put
 *       into programming mode for the read and released afterwards. The data arrives in
 *       lines of up to CMD_CHUNK_CELLS cells before the final OK:
 *           D <offset, 8 hex digits> <cell, 4 hex digits> ... *<crc, 4 hex digits>
 *       The crc is CRC-16/CCITT-FALSE (polynomial 0x1021, initial value 0xFFFF) over the
 *       line's text from the 'D' up to and including the last cell digit. Offsets and cells
 *       are in the units of pic.h (PIC16 flash cells are 14-bit words, EEPROM cells bytes).
 *       OK <count>
 *       A failure partway through sends ERR instead of OK, and the data lines already sent
 *       must be discarded.
 *
 * Errors: "ERR <pic_status_t value> <text>" for a failure reported by pic.h (for example
 * "ERR -2 range outside region"), or "ERR <word>" for a problem with the command itself:
 * bad-command, bad-argument, no-device, line-too-long, rx-error.
 *
 * The serial port is polled and has no receive buffer, so the host must wait for the final
 * OK or ERR line before it sends the next command.
 */
#define CMD_CHUNK_CELLS 16u

/* Set the device later commands act on (NULL for none). PROBE also sets it. */
void cmd_set_device(const pic_device_t *dev);

/* Feed one received byte. Runs the command when the line ends. */
void cmd_feed(uint8_t byte);

/* Tell the parser the receiver reported an error, so the current line is rejected. */
void cmd_rx_error(void);

#endif
