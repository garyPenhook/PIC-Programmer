# PIC ICSP Programmer on the Nucleo-G071RB: Plan

Goal: the Nucleo-G071RB drives the ICSP pins of a target PIC so it can identify, read,
erase, program and verify it.

- **First and test targets: PIC16F17146 and PIC16F17124** (PIC16F171XX family).
- **Scope: all PIC18 devices as well.**

Rule for this project: every PIC fact must come from a Microchip document (the Microchip
MCP server, or a PDF on disk). Where no source can be found, the family stays unsupported
and this file says what is needed.

Note on part numbers: "PIC16F1746" is not in Microchip's catalogue; it was taken to mean
PIC16F17146, whose documents are already in `~/Documents/Microchip/PIC/16F17146/`.

## Programming interfaces (one driver each)

The same command byte can mean different things on different families, so each interface
is its own driver over the shared pin layer `src/icsp.c`.

| Family | Spec | Driver | Status |
|---|---|---|---|
| **PIC16F171XX**: PIC16F17114/15/24/25/26, 17144/45/46, 17154/55/56, 17174/75/76 | DS40002266B, local | `src/pic16f171.c` | **Implemented**, first targets |
| PIC18-Q20, Q24, Q41, Q43 | Programming specs via the Microchip MCP | `src/pic18q.c` | Implemented |
| Older PIC18F (PIC18F452, 2620, 4550, 8722), J-series (PIC18F45J10), K-series (K22, K42, ...) | Not found | none | **Need the specs** |

Their data sheets are on disk (`~/Projects`, `~/Documents/Microchip/PIC/datasheets/`) but
a data sheet does not hold the programming algorithm. To add those families, put each
family's **Flash Programming Specification** PDF in the project root; each is its own
driver. Example of why they cannot be shared: 0xC0 and 0xE0 are "Begin Programming" on
PIC16F171XX but "Program Data" on PIC18-Q.

## PIC16F171XX facts (DS40002266B; section numbers refer to it)

- **Pins (Table 6-1):** MCLR = RA3, ICSPCLK = RA1, ICSPDAT = RA0 on both targets. Pin
  numbers per package are in `include/icsp.h` and the README.
- **Entry (3.1.2):** MCLR low, then the 32-bit key 0x4D434850 ("MCHP") on ICSPDAT, most
  significant bit first; the last bit is a don't-care but its clock is required. Hold MCLR
  low for as long as programming mode is wanted. Exit by raising MCLR. Works only while the
  LVP configuration bit is 1 (the factory default); LVP can only be cleared through
  high-voltage entry (MCLR 7.9 to 9.0 V), which the Nucleo cannot supply.
- **Bit protocol (3.1, 3.2):** MSb first, data changes on the rising clock edge and is
  latched on the falling edge. Commands are 8 bits. A payload is 24 clocks: start bit, pad
  bits, data MSb first, stop bit. Data fields: 16 bits (Load PC), 14 (Load Data to flash,
  config, User ID), 8 (EEPROM), 4 (Bulk Erase), 14 (Read Data).
- **Commands (Table 3-1):** Load PC 0x80, Bulk Erase 0x18, Row Erase 0xF0, Load Data
  0x00 / 0x02 (second form advances the PC), Read Data 0xFC / 0xFE, Increment Address 0xF8,
  Begin Internally Timed Programming 0xE0, Begin Externally Timed 0xC0, End Externally
  Timed 0x82. The driver uses the internally timed form only.
- **Memory (Table 2-1, word addresses, 14-bit words):** flash from 0x0000 (4 KW on
  PIC16F17124, 16 KW on PIC16F17146), User IDs 0x8000 to 0x8003, revision ID 0x8005,
  device ID 0x8006, Configuration Words 0x8007 to 0x800B, DIA 0x8100, DCI 0x8200, EEPROM
  from 0xF000 (128 bytes on PIC16F17124, 256 on PIC16F17146). Not-present flash reads as 0.
- **Device IDs (2.6):** PIC16F17124 = 0x30DC, PIC16F17146 = 0x30E1 (14-bit value with
  reserved bits 13:12 set); the full family table is in the driver.
- **Algorithms (3.3):** flash is written one 32-word row at a time: Load PC (row start),
  32 x Load Data (advancing the PC after all but the last, so the PC still points into the
  row at commit), then Begin Programming. Configuration Words and EEPROM are written one
  word/byte per cycle. Programming can only turn 1s into 0s, so erase first. Row Erase at
  0x8000 to 0x8004 erases only the User IDs.
- **Timing (Table 4-1):** clock pulses >= 100 ns; 1.0 us between commands and data; entry
  hold >= 250 us; exit >= 1 us; row erase <= 2.8 ms; flash program <= 2.8 ms; Configuration
  Word program <= 5.6 ms; bulk erase <= 8.4 ms. VDD 1.8 to 5.5 V; bulk erase needs VDD above
  the BOR level. Every value in the driver is at or beyond these.
- **Hex files (3.5):** two bytes per word, byte address = 2 x word address, so the
  Configuration Words at 0x8007 appear at 0x1000E in the hex file. A device ID in the hex
  file must be checked against the part.

## Assumptions to confirm on real silicon

The simulators are written from the same reading of the spec as the drivers, so the host
tests prove the drivers are self-consistent and respect every timing limit, but they
cannot prove these.

PIC16F171XX:

1. User ID and EEPROM program times have no figure of their own in the spec; the driver
   uses the longer Configuration Word time (6 ms).
2. EEPROM reads are assumed to return the byte right-aligned in the data field.
3. Bulk Erase follows the Configuration Memory flowchart (Figure 3-20) and loads a PC
   first; the text (3.2.2) says only the payload matters. If a real device disagrees, this
   is the first thing to change.
4. Section 3.4 mentions "bit 4" of the Bulk Erase payload for clearing code protection,
   which does not match the four-bit region list in 3.2.2. The driver only uses bits 0 to 3.
5. Read data is sampled just before the falling edge, after a 1 us high time.

PIC18-Q:

6. The 24-clock payload layout (`value << 1`) is taken from the PIC16F171XX figures; the
   PIC18-Q figures are images the search tool did not return as text.
7. 8-bit items (EEPROM, config bytes) are assumed right-aligned.
8. Bulk Erase is sent without loading a PC first.
9. Only the Q41 device IDs are known (from its data sheet); other Q-series parts report a
   raw ID.

## Milestones

1. **M1 (code done, needs a target):** identify. Enter, read device and revision ID, read
   the DCI, exit. Boot-time probe in `main()`: PIC16F171XX first, then PIC18-Q.
2. **M2 (code done, needs a target):** read, erase, program and verify flash, User ID,
   Configuration Words and EEPROM.
3. **M3 (next, needs hardware):** run M1 and M2 on a real PIC16F17146 and PIC16F17124 and
   settle assumptions 1 to 5. Start read-only: identify, then read the DCI, then dump
   flash, and only then erase and program. Use a scope or logic analyzer if the ID does not
   read back.
4. **M4:** command interface over the ST-LINK virtual COM port (USART2 on PA2/PA3), so a
   PC tool can drive the programmer.
5. **M5:** host tool (Python): parse Intel HEX, `id`, `read`, `erase`, `write`, `verify`,
   using the DCI to learn the flash and EEPROM size.
6. **M6:** the other PIC18 interfaces, once their programming specs are in the project.
7. **M7:** high-voltage entry (MCLR 7.9 to 9.0 V) to recover parts with LVP = 0.

## Debugging

The programming specs cover programming and verification only. Microchip's PICkit 4 guide
says on-chip debug (halt, step, breakpoints) uses a debug executive in program memory plus
special debug registers, which these documents do not describe. Debug support would be a
UART-based firmware agent on the target, as in `~/apps/Python_PIC_Debugger`, not hardware
ICD.
