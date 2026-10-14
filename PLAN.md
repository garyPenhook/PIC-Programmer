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

## Structure: protocols plus a device table

The target range is PIC10, PIC12 and all of PIC16 and PIC18: hundreds of parts. They fall
into a few programming protocols (command sets), and within a protocol the parts differ
only in data: device ID, memory sizes, row size, number of Configuration Words. So:

- **Protocol** = code, one per programming specification command set, in `src/proto/`,
  exposed as a `pic_proto_t` (`include/pic_proto.h`). The same command byte can mean
  different things in different protocols: 0xC0 and 0xE0 are "Begin Programming" in
  `p16_8bit` but "Program Data" in `p18_8bit`.
- **Device** = a row in `data/pic_devices.csv`, built into C by `tools/gen_devices.py`.
  Every value in a row must come from the Microchip document named in its `source`
  column.
- **API** = `include/pic.h`: (region, offset) addressing, range checks against the row's
  geometry, and `pic_probe()`.

To add a part that speaks a known protocol: add its row and run `tests/run_tests.sh`. To
add a protocol: write `src/proto/<name>.c`, list it in `protocols[]` in `src/pic.c` and in
`PROTOCOLS` in the generator, and give it a simulator and a test program in `tests/`.

| Protocol | Families | Spec | Status |
|---|---|---|---|
| `p16_8bit` | **PIC16F171XX** (all 14 parts in the table; first targets) | DS40002266B, local | **Implemented** |
| `p16_8bit` | PIC16F13145, PIC16F13276, PIC16F17576 (their programming specs describe the same Load Data for NVM command, PC increment on bit 1, row latches) | Via the Microchip MCP | Protocol present; **rows needed** (IDs, geometry from each family's spec) |
| `p18_8bit` | PIC18-Q41 (all 6 parts in the table) | Q41 data sheet DS40002214; Q24/Q41/Q43 programming specs via the Microchip MCP | Implemented |
| `p18_8bit` | PIC18-Q20, Q24, Q43 | Programming specs via the Microchip MCP | Protocol present; **rows needed**. Q24 has 13 configuration bytes whose addresses "do not increment sequentially" (Q24 spec 2.4), so the config region needs an address list, not just a count, before Q24 can be added |
| ? | Baseline PIC10/12/16F5x, older mid-range PIC16, older PIC18F (452, 2620, 4550, 8722), J-series (45J10), K-series (K22, K42, ...) | Not found | **Need the specs** |

Their data sheets are on disk (`~/Projects`, `~/Documents/Microchip/PIC/datasheets/`) but
a data sheet does not hold the programming algorithm. To add those families, put each
family's **Flash Programming Specification** PDF in the project root. Families whose
specs turn out to share a command set share one protocol.

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
  Timed 0x82. The protocol uses the internally timed form only.
- **Memory (Table 2-1, word addresses, 14-bit words):** flash from 0x0000 (4 KW on
  PIC16F17124, 16 KW on PIC16F17146), User IDs 0x8000 to 0x8003, revision ID 0x8005,
  device ID 0x8006, Configuration Words 0x8007 to 0x800B, DIA 0x8100, DCI 0x8200, EEPROM
  from 0xF000 (128 bytes on PIC16F17124, 256 on PIC16F17146). Not-present flash reads as 0.
- **Device IDs (2.6):** PIC16F17124 = 0x30DC, PIC16F17146 = 0x30E1 (14-bit value with
  reserved bits 13:12 set); the full family is in `data/pic_devices.csv`.
- **Algorithms (3.3):** flash is written one 32-word row at a time: Load PC (row start),
  32 x Load Data (advancing the PC after all but the last, so the PC still points into the
  row at commit), then Begin Programming. Configuration Words and EEPROM are written one
  word/byte per cycle. Programming can only turn 1s into 0s, so erase first. Row Erase at
  0x8000 to 0x8004 erases only the User IDs.
- **Timing (Table 4-1):** clock pulses >= 100 ns; 1.0 us between commands and data; entry
  hold >= 250 us; exit >= 1 us; row erase <= 2.8 ms; flash program <= 2.8 ms; Configuration
  Word program <= 5.6 ms; bulk erase <= 8.4 ms. VDD 1.8 to 5.5 V; bulk erase needs VDD above
  the BOR level. Every value in `p16_8bit.c` is at or beyond these.
- **Hex files (3.5):** two bytes per word, byte address = 2 x word address, so the
  Configuration Words at 0x8007 appear at 0x1000E in the hex file. A device ID in the hex
  file must be checked against the part.

## Assumptions to confirm on real silicon

The simulators are written from the same reading of the spec as the protocols, so the host
tests prove the protocols are self-consistent and respect every timing limit, but they
cannot prove these.

PIC16F171XX:

1. User ID and EEPROM program times have no figure of their own in the spec; the protocol
   uses the longer Configuration Word time (6 ms).
2. EEPROM reads are assumed to return the byte right-aligned in the data field.
3. Bulk Erase follows the Configuration Memory flowchart (Figure 3-20) and loads a PC
   first; the text (3.2.2) says only the payload matters. If a real device disagrees, this
   is the first thing to change.
4. Section 3.4 mentions "bit 4" of the Bulk Erase payload for clearing code protection,
   which does not match the four-bit region list in 3.2.2. Only bits 0 to 3 are used.
5. Read data is sampled just before the falling edge, after a 1 us high time.

PIC18-Q:

6. The 24-clock payload layout (`value << 1`) is taken from the PIC16F171XX figures; the
   PIC18-Q figures are images the search tool did not return as text.
7. 8-bit items (EEPROM, config bytes) are assumed right-aligned.
8. Bulk Erase is sent without loading a PC first.
9. Only the Q41 parts are in the device table, so the probe does not recognise another
   Q-series part.

## Milestones

1. **M1 (code done, needs a target):** identify. Enter, read device and revision ID, read
   the DCI, exit. Boot-time `pic_probe()` in `main()`: `p16_8bit` first, then `p18_8bit`.
2. **M2 (code done, needs a target):** read, erase, program and verify flash, User ID,
   Configuration Words and EEPROM.
3. **M3 (next, needs hardware):** run M1 and M2 on a real PIC16F17146 and PIC16F17124 and
   settle assumptions 1 to 5. Start read-only: identify, then read the DCI, then dump
   flash, and only then erase and program. Use a scope or logic analyzer if the ID does not
   read back.
4. **M4:** command interface over the ST-LINK virtual COM port (USART2 on PA2/PA3), so a
   PC tool can drive the programmer.
5. **M5:** host tool (Python): parse Intel HEX, `id`, `read`, `erase`, `write`, `verify`.
   It reads the same `data/pic_devices.csv` for geometry and checks it against the DCI.
6. **M6:** more rows for the existing protocols (PIC16F13145, 13276, 17576; PIC18-Q20,
   Q43; Q24 after the config address list), then new protocols for PIC10, PIC12, older
   PIC16 and older PIC18, once their programming specs are in the project.
7. **M7:** high-voltage entry (MCLR 7.9 to 9.0 V) to recover parts with LVP = 0.

## Debugging

The programming specs cover programming and verification only. Microchip's PICkit 4 guide
says on-chip debug (halt, step, breakpoints) uses a debug executive in program memory plus
special debug registers, which these documents do not describe. Debug support would be a
UART-based firmware agent on the target, as in `~/apps/Python_PIC_Debugger`, not hardware
ICD.
