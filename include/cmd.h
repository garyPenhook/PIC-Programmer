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
 *   ID
 *       Read the device ID and revision ID of the probed device. The ID is checked against
 *       the device first, so a different part gives ERR -6.
 *       OK <device id, 4 hex digits> <revision id, 4 hex digits>
 *
 *   DCI
 *       Read the Device Configuration Information of the probed device, in decimal:
 *       OK <ERSIZ erase row words> <WLSIZ write latches> <URSIZ user rows> <EESIZ EEPROM
 *          bytes> <PCNT pin count>
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
 *   READB <region> <offset> <count>
 *       Same as READ, but each chunk is a binary frame instead of a text line, which
 *       takes 2 bytes per cell instead of 5 characters. All fields are little endian:
 *           0x42 ('B')  offset (4 bytes)  n (1 byte, 1 to CMD_CHUNK_CELLS)
 *           n cells (2 bytes each)  crc (2 bytes)
 *       The crc is the same CRC-16/CCITT-FALSE, over every frame byte from the 0x42 up to
 *       and including the last cell. The final OK or ERR is still a text line. A host tells
 *       them apart by the first byte: 0x42 starts a frame, 'O' or 'E' a text line.
 *
 *   CRC <region> [<offset> <count>]
 *       CRC-32 (zlib's crc32: polynomial 0x04C11DB7, reflected, initial value and final
 *       XOR 0xFFFFFFFF) of the cells, each taken as two bytes, low byte first. The whole
 *       region if offset and count are left out. Computed on the programmer, so only the
 *       result crosses the serial port.
 *       OK <crc, 8 hex digits>
 *
 *   BLANK <region> [<offset> <count>]
 *       Check that every cell reads as erased (all the bits the cell stores set).
 *       OK <count>, or ERR not-blank @<offset, 8 hex digits> <value, 4 hex digits> for the
 *       first cell that is not.
 *
 *   SELFTEST
 *       Wiring check of the ICSP lines (see selftest.h). Needs no PROBE.
 *           LINE <CLK|DAT|MCLR> <float|high|low>     for each line
 *           SHORT <line> <line>                       for each short found
 *       then OK, or ERR wiring when a line or a short is wrong.
 *
 *   POWER [ON|OFF]
 *       Switch or report the target supply (target_power.h). The reply gives the state
 *       and the target VDD in millivolts, or -1 if the ADC could not measure it.
 *       OK <on|off> <mV>
 *
 *   VDD
 *       Target VDD and the programmer's own VDDA in millivolts, or ERR adc if the ADC
 *       failed (a failed measurement is never reported as 0 mV).
 *       OK <target mV> <VDDA mV>
 *
 *   STATUS
 *       Faults found at start-up. clock=hsi16 means the PLL setup failed and the chip
 *       runs at 16 MHz (the serial baud rate is then a few percent off, so this may only
 *       be readable at a slower host baud or over SWD: g_clock_status); adc=fail means
 *       the ADC never came up, so VDD and VDDA fail.
 *       OK clock=<pll|hsi16> adc=<ok|fail>
 *
 *   ERASE ALL
 *   ERASE <region>
 *   ERASE ROW <region> <offset>
 *       Bulk erase every region the device has, bulk erase one region, or erase the row
 *       (PIC18: page) that holds offset. Row erase works on flash and userid only
 *       (ERR -4 otherwise). Programming can only turn 1s into 0s, so erase before WRITE.
 *       OK
 *
 *   WRITE <region> <offset> <count>
 *   VERIFY <region> <offset> <count>
 *       Program count cells, or compare them with what the target holds. The host sends the
 *       data after the command, one line at a time, and only when asked:
 *           NEXT <offset, 8 hex digits> <n>     send the n cells starting at offset
 *       The host answers with one data line in the READ format,
 *           D <offset, 8 hex digits> <cell, 4 hex digits> ... *<crc, 4 hex digits>
 *       carrying exactly n cells (at most CMD_DATA_LINE_CELLS). A line with a bad crc, a
 *       different offset or cell count, or a receive error is asked for again with
 *           RESEND <offset> <n>
 *       and any other line ends the command with ERR aborted and releases the target.
 *       Flash writes on a device with write latches must start on a row boundary
 *       (ERR -3 otherwise); the cells are written a whole row at a time as they arrive, and
 *       a short last row is padded with blank cells. VERIFY reports the first difference as
 *       ERR -8 verify mismatch @<offset, 8 hex digits>.
 *       OK <count>
 *       The target stays in programming mode until the final OK or ERR.
 *
 * Errors: "ERR <pic_status_t value> <text>" for a failure reported by pic.h (for example
 * "ERR -2 range outside region"), or "ERR <word>" for a problem with the command itself:
 * bad-command, bad-argument, no-device, line-too-long, rx-error, aborted, not-blank,
 * wiring, adc.
 *
 * The serial port is polled and has no receive buffer, so the host must wait for the final
 * OK or ERR line (or, during WRITE and VERIFY, for each NEXT or RESEND) before it sends
 * anything else.
 */
#define CMD_CHUNK_CELLS     16u
#define CMD_FRAME_MARKER    0x42u /* first byte of a READB data frame ('B') */
#define CMD_DATA_LINE_CELLS 8u    /* most cells in one WRITE/VERIFY data line */

/* Set the device later commands act on (NULL for none). PROBE also sets it. */
void cmd_set_device(const pic_device_t *dev);

/* Feed one received byte. Runs the command when the line ends. */
void cmd_feed(uint8_t byte);

/* Tell the parser the receiver reported an error, so the current line is rejected. */
void cmd_rx_error(void);

#endif
