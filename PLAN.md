# PIC ICSP Programmer on the Nucleo-G071RB: Plan

Goal: the Nucleo-G071RB drives the ICSP pins of a target PIC so it can identify, read,
erase, program and verify it.

- **First and test targets: PIC16F17146 and PIC16F17124** (PIC16F171XX family).
- **Scope: PIC10, PIC12 and all of PIC16 and PIC18.**

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

To add a part that speaks a known protocol: add its row, add its geometry to that
protocol's simulator, and run `tests/run_tests.sh` (README, "Adding a part"). To add a
protocol: write `src/proto/<name>.c` with a `pic_proto_t`, list it in `protocols[]` in
`src/pic.c` and in `PROTOCOLS` in the generator (with the most cells each region can hold
in its address map), and give it a simulator, a test program, and a line in
`tests/run_tests.sh`.

| Protocol | Families | Spec | Status |
|---|---|---|---|
| `p16_8bit` | **PIC16F171XX** (all 14 parts in the table; first targets) | DS40002266B, local | **Implemented** |
| `p16_8bit` | **PIC16F131XX** (PIC16F13113/14/15, 13123/24/25, 13143/44/45: 9 parts, no EEPROM) | DS40002500B, downloaded to `docs/f13145/` | **Implemented**, simulator only. Its NVM time limits are larger than the F171XX's, so `p16_8bit.c` waits for the larger of the two (timing table in the source) |
| `p16_8bit` | **PIC16F132XX** (PIC16F13214/15/24/25/26/44/45/46/54/55/56/74/75/76: 14 parts, 7 Configuration Words, 256-byte EEPROM) | DS40002646A, downloaded to `docs/f13276/` | **Implemented**, simulator only. Bulk erase is up to 40 ms, so the shared waits now cover it. PIC16F17576 still needs rows |
| `p18_8bit` | PIC18-Q41 (all 6 parts in the table) | Q41 data sheet DS40002214; Q24/Q41/Q43 programming specs via the Microchip MCP | Implemented |
| `p18_8bit` | PIC18-Q20, Q24, Q43 | Programming specs via the Microchip MCP | Protocol present; **rows needed**. Q24 has 13 configuration bytes whose addresses "do not increment sequentially" (Q24 spec 2.4), so the config region needs an address list, not just a count, before Q24 can be added |
| `p18_q10` | PIC18F24/25/26/27Q10, PIC18F45/46/47Q10 (all 7) | DS40001874H (`docs/q10/`), data sheets DS40001945, DS40001996, DS40002043 | Implemented and tested against a simulator only; **not yet run on silicon** |
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

Both:

10. The probe accepts a part only when its DCI matches the table row. If the DCI does not
    read back as the spec describes, the probe reports no device; check the DCI read
    first. When nothing matches, `g_pic_device_id` and `g_pic_revision_id` hold what
    `p16_8bit` read (the first protocol tried), so on a PIC16 they are still its raw IDs.

## Milestones

1. **M1 (code done, needs a target):** identify. Enter, read device and revision ID, read
   the DCI, exit. Boot-time `pic_probe()` in `main()`: `p16_8bit` first, then `p18_8bit`,
   accepting a part only when its ID and DCI both match a table row.
2. **M2 (code done, needs a target):** read, erase, program and verify flash, User ID,
   Configuration Words and EEPROM.
3. **M3 (next, needs hardware):** run M1 and M2 on a real PIC16F17146 and PIC16F17124 and
   settle assumptions 1 to 5 and 10. Start read-only: identify, then read the DCI, then dump
   flash, and only then erase and program. Use a scope or logic analyzer if the ID does not
   read back.
4. **M4 (done, needs a target):** command interface over the ST-LINK virtual COM port (USART2 on
   PA2/PA3), so a PC tool can drive the programmer. Done: polled UART at 921600 baud, line
   parser, `PROBE`, `ID`, `DCI`, `READ` (text, per-line CRC), `READB` (binary frames),
   `ERASE`, and `WRITE` / `VERIFY` with lock-step NEXT/RESEND data upload (README, "Serial
   commands"). Later: DMA transmit so ICSP reads overlap the serial link, and
   skipping erased runs. PA2/PA3 reach the virtual COM port by default (UM2324 Table 8);
   the highest baud rate the ST-LINK accepts still has to be tried on the board. Optional
   SSD1309 OLED status screen on I2C1 (README, "OLED status display").
5. **M5 (started):** host tool (Python, `host/picprog`): Intel HEX parsing and writing,
   `probe`, `id`, `dci`, `read`, `dump`, `erase`, `write` (bulk erase, program, verify) and
   `verify`, with the DCI checked against `data/pic_devices.csv`. Tested end to end against
   the firmware's command layer built for the PC. To do: confirm the PIC16 EEPROM and
   PIC18 word placement in MPLAB hex files; decode Configuration Words: done for all PIC16 rows (`picprog config`, table `data/pic_config_bits.csv`
   from Appendix B of DS40002266B, DS40002500B and DS40002646A; `write` warns before setting
   ICSPDIS, LVP = 0 or code protection), still to do for PIC18-Q41 and PIC18F2X/4XQ10 (see
   "Program extraction"); read the DIA (needs a read-only region in `pic.h` first).
6. **M6:** more rows for the existing protocols (PIC16F13145 and 13276 families done; 17576 left; PIC18-Q20,
   Q43; Q24 after the config address list), then new protocols for PIC10, PIC12, older
   PIC16 and older PIC18, once their programming specs are in the project.
7. **M7:** high-voltage entry (MCLR 7.9 to 9.0 V) to recover parts with LVP = 0.
8. **M8:** seed a bootloader over ICSP (see "Bootloaders"). Needs M3 (real silicon) and
   the Intel HEX parser from M5.
9. **M9:** act as a bootloader host and push application images over UART (see
   "Bootloaders"). Needs M4 and a second USART for the target.

## Program extraction

Read a target's memory back into an Intel HEX file: flash, User ID, Configuration Words
and EEPROM, plus the device and revision IDs. It is for your own parts: backup before an
erase or a bootloader seed, cloning a board, checking what a part holds.

What already exists: `pic_read()` reads every region through the (region, offset)
addressing, and both protocols implement it (`p16_read`, `p18_read`) with framing checks.
Nothing is missing in the firmware's read path. What is missing is the way out: there is
no command interface (M4) and no host tool (M5) to turn reads into a file.

Design:

1. **Firmware (M4):** a `read <region> <offset> <count>` command that returns the cells
   in chunks, each with a length and a CRC so the host can detect corrupt transfers over
   the virtual COM port. Chunking bounds the buffer, as `VERIFY_CHUNK` does for verify.
   The ICSP link is the slow part: a read costs about 70 us per cell with the current
   1 us clock phases (8 command clocks, 24 payload clocks, and the inter-item waits, from
   `icsp8.c`), so about 14,000 cells a second and roughly a second for 16 KW. Not worth
   optimising before M3 shows the real limits.
2. **Host tool (M5):** `dump` walks the device's regions using geometry from the CSV and
   the DCI, then writes Intel HEX. Addresses follow the existing hex rule (PLAN, PIC16F171XX
   facts, 3.5): two bytes per PIC16 word, byte address 2 x word address, so Configuration
   Words land at 0x1000E. PIC18 uses byte addresses directly (flash 0, User ID 0x200000,
   config 0x300000, EEPROM 0x380000) and needs extended linear address records. Where the
   PIC16 EEPROM and User ID land in the hex file is **to confirm** against spec 3.5.
3. **Verify the dump:** read twice and compare before trusting a file, and offer
   `verify <file>` afterwards (already `pic_verify()`).
4. **Code protection:** a protected region reads back as zeros (`pic.h`). A dump of a
   protected part is therefore not a copy, and an all-zero flash looks the same as a
   blank-looking dump. The tool must say so rather than write a plausible-looking file:
   decode the code-protect bits from the Configuration Words per part (the per-part bit
   layout is **to confirm** from each data sheet, and fits the config-word editor in
   IDEAS.md) and warn, and flag a flash that is all zeros. This project does not try to
   defeat code protection.
5. **Other areas:** the DIA (calibration data) needs the new read-only region already
   noted in M5, and is worth including in a dump so a cloned board keeps its calibration.
6. **Bootloaders:** the MDFU commands found in the specification (GetClientInfo,
   StartTransfer, WriteChunk, GetImageState, EndTransfer) include no read, so extracting
   an application from a target that has a bootloader goes through ICSP, not the
   bootloader (to confirm against the full command list).
7. **Tests:** the simulators already model memory, so add a round trip per table row:
   write a pattern, `dump`, parse the hex, compare, and check the PIC16 and PIC18 address
   mapping and the protected-region warning.

## Bootloaders

Two jobs, both wanted: **seed** a bootloader into a blank target over ICSP, and **host**
application updates to targets that already have one. Nothing is implemented yet. The
facts below come from Microchip documents found through the Microchip MCP server; items
marked "to confirm" were not found there.

What Microchip's 8-bit bootloaders are (MCC Melody "8-Bit MDFU Client Library", the
successor of the older Melody 8-bit Bootloader module and its Unified Bootloader Host
Application):

- Three parts: the bootloader client firmware (runs first after reset, checks the
  application, jumps to it or waits for the host), the user application, and a host.
  Microchip's host is `pymdfu`; `pyfwimagebuilder` turns the application hex file plus the
  client's `bootloader_configuration.toml` into the image file the host sends. The image
  carries the memory unlock keys, flash row size, application start address and device ID
  along with the program data. An embedded host is allowed: the library docs say an
  embedded host can send the image built by `pyfwimagebuilder`, which is what M9 would be.
- The client is generated by MCC on a PC. This project would program the generated hex,
  not generate bootloaders.
- Supported parts: the library lists "all Melody supported PIC16 and PIC18" (release notes
  v1.5.0). Whether the PIC16F171XX and PIC18-Q41 rows in the device table are covered is
  **to confirm** per part.
- Memory: the client occupies the start of flash and the application starts at a
  configured address, which must sit on a page boundary or the bootloader is corrupted.
  Parts with a Memory Access Partition (MAP) divide flash into Boot Block, Application
  Block and Storage Area Flash, set by the BBEN, BBSIZE and SAFEN configuration bits
  (described in the data sheets of several other PIC16 families, and for PIC18 in TB3163;
  whether PIC16F171XX has MAP is **to confirm** in
  DS40002266B and the data sheet). The boot block is write-protected, so it is only
  rewritten by a programmer.
- Hex files: PIC16 hex is word-oriented and PIC18 byte-oriented (TB3163), matching the
  per-family addressing already in `p16_8bit` and `p18_8bit`.

MDFU protocol (Microchip Device Firmware Update Protocol Specification 1.0.0), for M9:

- Host driven command and response pairs. Commands: GetClientInfo, StartTransfer,
  WriteChunk, GetImageState, EndTransfer. Multi-byte fields are little endian. Sequence
  numbers match responses to commands and let either side ask for a resend.
- GetClientInfo reports the client's buffer sizes and command time-outs, so the host must
  size chunks and waits from the reply (its own time-out is a fixed one second).
- UART transport: 8 data bits, no parity, 1 or more stop bits, any data rate, full duplex.
  Frames carry start and end markers (reserved codes are replaced by 2-byte substitutions)
  and an integrity check. Exact framing, check algorithm and the image file layout are
  **to confirm** from the specification pages and `pyfwimagebuilder`.
- SPI and I2C transports also exist. UART first.
- The older Unified Bootloader protocol is in "Unified Host User's Guide" DS40001779B,
  chapter 7. It is not on disk. Support it only if a target needs it.

Seeding (M8), and what it needs from this codebase:

1. Intel HEX parsing (M5 host tool), then write the bootloader hex through the existing
   `pic_write()`, with the Configuration Words programmed last so write protection is not
   active while the boot block is still being written.
2. `pic_bulk_erase()` also wipes the bootloader. The host tool should warn before an erase
   on a target that has one, and prefer `pic_row_erase()` over the application range.
3. The application start address and boot block size depend on how the client was
   generated, not on the part, so the host tool should read them from the client's
   `bootloader_configuration.toml` rather than from the device table.
4. Verify with `pic_verify()` afterwards, then release MCLR so the new client runs.
5. To confirm: whether ICSP can still erase and rewrite a write-protected boot block
   (it should, since a programmer does it, but the exact rule is in each part's spec).

Hosting (M9), and what it needs:

1. A UART transport on the STM32 for the target, separate from USART2 on PA2/PA3 (ST-LINK
   VCP, used by M4): done, USART1 on D1 (PC4, TX) and D0 (PC5, RX).
2. An MDFU host state machine on the STM32, or the PC running `pymdfu` with the Nucleo
   only as a UART bridge. The bridge now exists (`BRIDGE <baud>`, USART1 on D0/D1,
   docs/hookup.md); whether `pymdfu` can run through it (it would need to open the
   bridge first and send the exit sequence after) is still to try. The embedded host is
   for standalone use.
3. Entering the bootloader: the client enters its update cycle when the application is
   invalid or on a signal such as a GPIO level or UART traffic, depending on how it was
   generated. A reset line (MCLR on PB5 already exists) lets the host force a restart.
4. The target's UART pins may be the same pins as ICSPCLK and ICSPDAT, in which case the
   programmer must release them before the UART runs. Which pins the client uses is set
   when it is generated, so this is **to confirm** per bootloader build.

Order: seed first (it reuses everything built so far and can be tested on a PIC16F17146
after M3), then the UART bridge, then the embedded host.

## Debugging

The programming specs cover programming and verification only. Microchip's PICkit 4 guide
says on-chip debug (halt, step, breakpoints) uses a debug executive in program memory plus
special debug registers, which these documents do not describe. Debug support would be a
UART-based firmware agent on the target, as in `~/apps/Python_PIC_Debugger`, not hardware
ICD.

The first step is already usable: `BRIDGE` gives the target `printf`-style output over its
UART. `docs/hookup.md` ("PIC16F17146 as the target") has the pins and register settings. For
the PIC16F17146, use RB7 (TX) and RB5 (RX) so the UART does not share ICSPCLK/ICSPDAT, which
settles the pin conflict noted above for that part. Not yet tried on hardware: no target was
connected when this was written.
