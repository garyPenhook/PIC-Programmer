# PIC Programmer/Debugger

Bare-metal C firmware for the STM32G071RB (NUCLEO-G071RB board) that programs PIC microcontrollers over ICSP: no HAL, no RTOS.

## Memory

- `0x08000000`: flash, 128 KiB (vector table first, then code)
- `0x20000000`: SRAM, 36 KiB (STM32G071RB without RAM parity)

Sizes are set in `linker/STM32G071RB_FLASH.ld`.

## Build

Requires `arm-none-eabi-gcc` (the toolchain file pins the GNU Tools for STM32 14.3.1 bundle)
and Python 3, which generates the device table.

| Preset | Flags | Output |
|---|---|---|
| `Debug` | `-Og -g3` | `build/Debug/` |
| `Release` | `-Os -DNDEBUG` | `build/Release/` |

```sh
cmake --preset Debug   && cmake --build --preset Debug
cmake --preset Release && cmake --build --preset Release
```

Each build writes `pic_programmer_debugger` (ELF), `.bin` and `.hex` into its build directory, and the link step prints flash and RAM used out of the device maximum, for example `FLASH: 14416 B  128 KB  11.00%` (Release). RAM covers static data only (`.data` + `.bss`); the stack is not counted.

## Clock and LED Blink

`platform_init()` calls `clock_init()` (`src/clock.c`, `include/clock.h`) to run the core at its maximum, **64 MHz**, and toggles the on-board user LED (LD4, PA5 / Arduino D13, active high) from a timer interrupt every **1 s** (1 s on, 1 s off).

- **SYSCLK**: PLL R output, 64 MHz: HSI16 (16 MHz) / M=1 x N=8 = 128 MHz VCO, / R=2. All three values are inside the limits in RM0444 5.4.4 and datasheet Table 43 (PLL input 2.66 to 16 MHz, VCO 96 to 344 MHz, PLLR max 64 MHz). HCLK and PCLK are undivided, so they also run at 64 MHz, and the timers run at PCLK.
- **Voltage range**: Range 1, the reset default (`PWR_CR1.VOS = 01`, read back on the board), which 64 MHz requires.
- **Flash**: 2 wait states and prefetch (RM0444 Table 13). `clock_init()` sets the wait states first and waits for them to take effect before it raises the clock.
- **Fallback**: every hardware wait in `clock_init()` is bounded. If the PLL fails to lock, the chip stays on 16 MHz and `SystemCoreClock` says so, so timer and delay maths stay correct.
- **Timer**: TIM16 on PCLK. The prescaler is computed from `SystemCoreClock` (PSC = 6399 at 64 MHz, a 10 kHz tick) and `ARR = 9999` gives a 1 s update event. The update interrupt is enabled in `TIM16_DIER` and in the NVIC (IRQ 21).
- **Vector table**: `src/startup_stm32g071rb.s` covers the core exceptions plus IRQ 0..21. Slot 0x94 (`TIM16 / FDCAN_IT0`) points to `TIM16_IRQHandler` in `src/platform.c`; the other IRQ slots use `Default_Handler`. Anything else enabled later needs its own slot.
- **ISR**: `TIM16_IRQHandler` clears `UIF` (rc_w0) and toggles PA5. It is the only code that writes PA5, so the main loop needs no locking.
- **Change the rate**: edit `LED_TOGGLE_MS` in `src/platform.c` (max about 6.5 s at 64 MHz, since ARR is 16-bit).

Verified on hardware by reading registers over SWD: `RCC_CR = 0x03000500` (PLL on and locked), `RCC_CFGR = 0x12` (PLL selected and confirmed), `FLASH_ACR` latency 2, `SystemCoreClock = 64000000`, and PA5 changing state every ~1 s.

## PIC Programmer

The Nucleo acts as an ICSP programmer. **First targets: PIC16F17146 and PIC16F17124**
(both in the PIC16F171XX family). The PIC16F13113/14/15, 13123/24/25 and 13143/44/45, the fourteen
PIC16F13214 to PIC16F13276 parts, the twelve
PIC16F17524 to PIC16F17576 parts and the
PIC18F04/05/06/14/15/16Q41, PIC18F04/05/06/14/15/16Q40, PIC18F04/05/06/14/15/16Q20, the seven PIC18F24/25/26/45/46/55/56Q24, the nine PIC18F24/25/26/44/45/46/54/55/56Q71, the twelve PIC18F26/27/46/47/56/57Q83 and Q84 parts and the nine PIC18F25/26/27/45/46/47/55/56/57Q43 parts are also in the device table, and the plan covers PIC10, PIC12 and all of PIC16 and PIC18. `PLAN.md` has the
scope, sources, assumptions and milestones.

Full wiring, with a schematic, is in [`docs/hookup.md`](docs/hookup.md).

Wiring (Arduino header, UM2324 Table 12; target pin numbers from the PIC16F171XX
programming spec Table 6-1, where MCLR = RA3, ICSPCLK = RA1, ICSPDAT = RA0):

| Nucleo | MCU pin | Signal | PIC16F17124 14-pin SOIC/TSSOP/PDIP | PIC16F17124 16-pin QFN | PIC16F17146 20-pin SSOP/SOIC/PDIP | PIC16F17146 20-pin QFN |
|---|---|---|---|---|---|---|
| D3 | PB3 | ICSPCLK | 12 | 11 | 18 | 15 |
| D5 | PB4 | ICSPDAT | 13 | 12 | 19 | 16 |
| D4 | PB5 | MCLR | 4 | 3 | 4 | 1 |
| 3V3 | - | VDD | 1 | 16 | 1 | 18 |
| GND | - | VSS | 14 | 13 | 20 | 17 |

The target is powered from the Nucleo 3.3 V (the specs allow 1.8 to 5.5 V).

Supported parts are data, not code. Each one is a row in `data/pic_devices.csv`: its
device ID, the programming protocol it speaks, and its memory geometry, with the Microchip
document every value came from. A protocol is one programming specification command set,
shared by every family that uses it, so adding a part that speaks a known protocol is a
new row and nothing else (see "Adding a part" below). The build turns the CSV into C with
`tools/gen_devices.py`, which fails the build on a bad row: a duplicate ID or name, a
missing source, sizes larger than the protocol's address map can hold (so a row cannot
reach into another region), flash that does not divide into rows, write latches on a
protocol that programs one word at a time (or none on one that needs them), and text
that would break the generated C.

Layers:

- `include/pic.h`, `src/pic.c`: the device API used by everything above the protocols.
  Memory is addressed as (region, offset) with region = flash, User ID, config or EEPROM,
  so callers never deal with a family's own address map. It checks every range against
  the device's geometry before anything reaches the wires, and `pic_probe()` tries each
  protocol until one returns a device ID that is in the table and the part's own DCI
  (row size, latches, flash and EEPROM size) agrees with that row. The ID alone is not
  enough: a part from another protocol answers the same read commands with whatever its
  memory holds at that address. Verify is built here from the protocol's read, for every
  protocol.

  Every operation that can fail returns a `pic_status_t` (`pic_strerror()` gives text):
  `PIC_ERR_ARG`, `_RANGE`, `_ALIGN` and `_UNSUPPORTED` are caught before anything is sent;
  `PIC_ERR_NO_TARGET` and `_MISMATCH` come from `pic_enter()`, which reads the device ID
  and compares it with the device you named; `PIC_ERR_PROTOCOL` means a read payload had a
  bad start or stop bit (a data line stuck or pulled high; an absent target reads as
  all zeros, which is well framed, so it is caught by the ID check instead); and
  `PIC_ERR_VERIFY` reports where the first mismatch was. Writes and erases are open loop
  (a fixed worst-case wait), so `pic_verify()` is how a failed write is found.
- `src/proto/`: one file per protocol, each a `pic_proto_t` (`include/pic_proto.h`).
  - `icsp8.c`: the framing shared by the 8-bit-command protocols: low-voltage key entry,
    8-bit commands, 24-clock payloads.
  - `p16_8bit.c`: the PIC16F171XX and PIC16F131XX command set: identify, read, bulk and row erase, row
    programming through the write latches, User ID, Configuration Words and EEPROM.
  - `p18_8bit.c`: the PIC18-Q command set, which uses similar command bytes that mean
    different things, so it is a separate protocol.
  - `p18_q10.c`: the PIC18F2X/4XQ10 command set (DS40001874H). Same command bytes as
    `p18_8bit`, but EEPROM at 31 0000h, no DCI (the part is confirmed by its device ID and
    the fixed nibble of its revision ID), 16-bit Configuration Words, a Bulk Erase with no
    payload whose region set follows the PC (flash+config, flash+User ID+config, or EEPROM
    only; other sets are rejected), and different timing.
- `include/cmd.h`, `src/cmd.c`, `include/uart.h`, `src/uart.c`: the serial command
  interface (see "Serial commands" below). `uart.c` is a polled USART2 driver and the only
  hardware-specific part; `cmd.c` is plain C and runs on the PC in the tests.
- `include/i2c.h`, `src/i2c.c`, `include/oled.h`, `src/oled.c`, `src/oled_port.c`, `include/ui.h`,
  `src/ui.c`: the optional OLED status screen (see "OLED status display" below). `i2c.c`
  is a polled I2C1 driver; `oled.c` (SSD1309) and `ui.c` are plain C and are tested on the
  PC against a stub bus.
- `src/icsp.c`: pins and microsecond delays only, and the only hardware-specific file
  under the protocols. The three pins use low output speed (OSPEEDR `01`,
  RM0444 7.5.3): ICSPCLK runs at about 500 kHz, far below that setting's limit, and the
  slower edges ring less on a programming cable. ICSPDAT has a weak pull-down, so a
  missing target reads as 0. Delays poll SysTick as a free-running counter on the core
  clock (its interrupt stays off), so they stay correct at 64 MHz or the 16 MHz fallback.
- `tests/`: each protocol run on the PC against a simulated device that also checks every
  wait the spec requires, once for every table row that uses the protocol (14 PIC16F171XX
  parts, 9 PIC16F131XX parts, 14 PIC16F132XX parts, 12 PIC16F175XX parts, 6 PIC18-Q41 parts, 6 PIC18-Q40 parts, 6 PIC18-Q20 parts, 7 PIC18-Q24 parts, 9 PIC18-Q71 parts, 12 PIC18-Q83/Q84 parts, 9 PIC18-Q43 parts, 7 PIC18F2X/4XQ10 parts). The simulators hold their own memory sizes, copied from the
  spec, and the tests compare the part's DCI with its table row, so a wrong size in the
  CSV fails the tests. Run `tests/run_tests.sh`.

At boot `main()` probes the target once, trying `p16_8bit`, `p18_8bit` and `p18_q10` in that order.
Read the result over SWD: `g_pic_status` is 0 when a part was found, else the `PIC_ERR_*` code saying why not. `g_pic_device` points at the matching table row (`print
*g_pic_device` in GDB shows its name and geometry) or is NULL, and `g_pic_device_id` and
`g_pic_revision_id` hold the IDs (addresses in the map file). A PIC16F17146 should read
`0x30E1` and a PIC16F17124 `0x30DC`. With nothing attached, the IDs are 0.

### Serial commands

The programmer takes line commands over the ST-LINK virtual COM port (USART2 on PA2/PA3,
921600 baud, 8N1; lower `UART_BAUD` in `include/uart.h` if the board does not run
cleanly at that rate). Commands are upper case, one per line. Every command ends with one
`OK ...` or `ERR ...` line, and the host must wait for it before sending the next command
(the port has no receive buffer). The full format is in `include/cmd.h`.

| Command | Reply |
|---|---|
| `PROBE` | `OK <name> <device id> <revision id>`; remembers the device for later commands |
| `POWER`, `POWER ON`, `POWER OFF` | `OK on/off <VDD mV>`; while off, target commands answer `ERR power-off` |
| `VDD` | `OK <target VDD mV> <VDDA mV>` |
| `BRIDGE <baud>` | `OK bridge`, then raw bytes to and from the target's serial port until the exit sequence, then `OK bridge closed <dropped>` |
| `SELFTEST` | `LINE <line> <float/high/low>` per ICSP line, `SHORT <a> <b>` per short, then `OK` or `ERR wiring` (no PROBE needed) |
| `ID` | `OK <device id> <revision id>` (ERR -6 if a different part now answers) |
| `DCI` | `OK <ERSIZ> <WLSIZ> <URSIZ> <EESIZ> <PCNT>` in decimal |
| `READ <region> <offset> <count>` | `D <offset> <cells...> *<crc>` lines, then `OK <count>` |
| `READB <region> <offset> <count>` | binary frames (2 bytes per cell), then `OK <count>` |
| `CRC <region> [<offset> <count>]` | `OK <crc32>`: CRC-32 of the cells, computed on the programmer |
| `BLANK <region> [<offset> <count>]` | `OK <count>`, or `ERR not-blank @<offset> <value>` |
| `ERASE ALL`, `ERASE <region>`, `ERASE ROW <region> <offset>` | `OK` |
| `WRITE <region> <offset> <count>` | `NEXT <offset> <n>` requests, then `OK <count>` |
| `VERIFY <region> <offset> <count>` | `NEXT <offset> <n>` requests, then `OK <count>` or `ERR -8 verify mismatch @<offset>` |

`region` is `flash`, `userid`, `config` or `eeprom`; numbers are decimal or `0x` hex. Each
`D` line carries up to 16 cells, with a CRC-16/CCITT-FALSE over the text before ` *`, so
a host can detect corruption on the serial link. Errors read `ERR <code> <text>` (the code
is the `pic_status_t` value, for example `ERR -2 range outside region`) or `ERR <word>`
for a bad command (`bad-command`, `bad-argument`, `no-device`, `line-too-long`,
`rx-error`). On any `ERR`, discard the `D` lines already received. Example (the values
are from the PC simulator, not a real part):

```
> PROBE
< OK PIC16F17146 30E1 2000
> READ flash 0 4
< D 00000000 3FFF 3FFF 3FFF 3FFF *8017
< OK 4
```

`READ` is for typing at a terminal; `READB` is for a host tool. Its frames are
`0x42`, a 4-byte offset, a 1-byte cell count, the cells (2 bytes each) and a CRC, all little
endian (full layout in `include/cmd.h`), so a host tells a frame (`0x42`) from the final
text line (`O` or `E`) by its first byte.

Estimated time for a 16 KW part, from the per-cell costs (ICSP about 70 us per cell from
`icsp8.c`; the two do not overlap yet): `READ` about 2.3 s (5 characters per cell plus the line overhead),
`READB` about 1.6 s (2.5 bytes per cell with the frame overhead). These are
calculations, not measurements; the ST-LINK virtual COM port has not been tested at
921600 yet.
`WRITE` and `VERIFY` take their data from the host in lock-step, which suits the polled
port: the programmer sends `NEXT <offset> <n>`, the host answers with one data line of
exactly n cells in the `READ` format (`D <offset> <cells...> *<crc>`, at most 8 cells),
and waits for the next request. A damaged line is asked for again with `RESEND`; any other
line aborts and releases the target. Flash on a device with write latches is written a
whole row at a time as the cells arrive, so a flash `WRITE` must start on a row boundary.
Erase first: programming only turns 1s into 0s.

```
> ERASE ALL
< OK
> WRITE flash 0 12
< NEXT 00000000 8
> D 00000000 3FFF 3FFF 3FFF 3FFF 3FFF 3FFF 3FFF 3FFF *<crc>
< NEXT 00000008 4
> D 00000008 3FFF 3FFF 3FFF 3FFF *<crc>
< OK 12
```

The API reads the device ID, revision ID and the five DCI values, but not the other
read-only areas (the PIC16 DIA at 0x8100, for example); that would be a new read-only
region.

Only low-voltage entry is supported: a part whose LVP configuration bit is 0 cannot be
reached without a high-voltage (7.9 to 9.0 V) MCLR supply.

Not yet tested on real silicon. The PIC16F171XX payload layout comes from figures in the
spec and the protocols share one reading of it with the simulators, so a first run on a real
PIC16F17146 or PIC16F17124 is the real test. See `PLAN.md`.

### OLED status display

An optional 128 x 64 SSD1309 OLED module on I2C shows the probed device, its IDs and the
result of the last command. Without a display everything works the same: the firmware
looks for one at boot and, if nothing answers, never touches the bus again.

Configured for the 2.42" SSD1309 module with a 7-pin header (GND VCC SCL SDA RES DC CS,
for example HiLetgo's). Its pin order does not match the Arduino header (SCL and SDA come
in the opposite order to D14/D15), so it is wired with jumper leads:

| Module | Nucleo | MCU pin |
|---|---|---|
| GND | GND | - |
| VCC | 3V3 | - |
| SCL | D15 | PB8 (I2C1_SCL, AF6) |
| SDA | D14 | PB9 (I2C1_SDA, AF6) |
| RES | D7 | PA8, pulsed low at boot (`src/oled_port.c`) |
| DC | GND | - (selects address 0x3C) |
| CS | GND | - (unused in I2C mode) |

Pins from UM2324 Table 12 and the STM32G071 datasheet Table 14. The bus runs at about
380 kHz, inside the SSD1309 minimum clock cycle of 2.5 us (SSD1309 Table 13-6), with
375 ns data hold (the SSD1309 needs 300 ns on SDAIN). RES is held low for 10 us (the
SSD1309 minimum is 3 us, Table 13-1).

Preparing the module (from seller listings and user reports, not a module datasheet;
check your board's silkscreen first):

- **It ships set to SPI.** On boards with R3/R4/R5 next to the header, I2C is selected by
  removing R4 and bridging R3 and R5. Other board revisions use R13/R15/R17 instead.
- **Pull-ups on SDA and SCL.** The SSD1309 requires them (8.1.5) and it is not confirmed
  that this module has them. If the display does not answer, fit 4.7 kOhm from each line to
  3V3. The STM32's internal pull-ups are enabled but are too weak to rely on.
- **DC must be tied to a fixed level** (GND for 0x3C, 3V3 for 0x3D); both are tried.
- **VCC panel supply.** The SSD1309 needs 7 to 16 V for the panel in addition to VDD
  (SSD1309 2, 8.8), so the module must generate it from its VCC pin.

To use a module with a different reset pin, change `PIN_RES` in `src/oled_port.c`.

For a module that never acknowledges (an SSD1309 whose SDAOUT pin is not connected),
configure with `-DI2C_IGNORE_ACK=ON`: the I2C pins are then driven in software and
acknowledges are ignored (see `include/i2c.h` and `docs/hookup.md`).

If the picture is upside down, build with `OLED_FLIP=1` (in `include/oled.h`); that sends
the segment re-map and COM scan direction commands (A1h, C8h) instead of the reset values.

The screen is redrawn before a command sends its final `OK` or `ERR`, and only the
128-byte pages that changed are sent (about 3.5 ms each), so a host that waits for the
reply never sends while the bus is busy. This adds a few milliseconds per command, not per
cell.

### Adding a part

1. Find the part's programming specification and data sheet and check which protocol its
   command set matches (`include/pic_proto.h` describes each one).
2. Add a row to `data/pic_devices.csv` with the device ID, flash size in words, erase and
   latch row sizes (the DCI table), User ID words, configuration count, EEPROM bytes, and
   the document sections they came from in `source`.
3. Add the part's geometry to that protocol's simulator in `tests/`, also from the
   document, so the tests cross-check the row, then run `tests/run_tests.sh`.

A part whose command set matches no protocol needs a new protocol; `PLAN.md` says how.

## Host tool

`host/picprog` is a Python command-line tool that drives the programmer over the virtual
COM port. It is a uv project (`host/pyproject.toml`, locked in `host/uv.lock`); install it
as a command with:

```sh
uv tool install ./host        # puts `picprog` on the PATH (~/.local/bin)
uv tool install --reinstall ./host   # after changing the tool or data/pic_devices.csv
```

```sh
picprog probe                    # identify the target
picprog dump backup.hex          # read every region into an Intel HEX file
picprog write firmware.hex       # bulk erase, program and verify
picprog verify firmware.hex      # compare the target with a file
picprog erase all                # or: erase flash, erase flash --row 0x40
picprog read flash 0 32          # print cells
picprog dci                      # Device Configuration Information
picprog crc firmware.hex         # compare by CRC-32, without reading the data back
picprog blank                    # check every region is erased (or: blank flash)
picprog config                   # decode the part's Configuration Words field by field
picprog config --device PIC16F13276 firmware.hex   # the same for a hex file, no programmer needed
picprog selftest                 # check the ICSP wiring for shorts and stuck lines
picprog power off                # switch the target supply (needs the switch, docs/hookup.md)
picprog vdd                      # target VDD and the programmer's VDDA
picprog bridge 115200            # terminal to the target's serial port; Ctrl-] ends
```

`crc` computes the CRC-32 of each region on the programmer and compares it with the same
CRC of the file (each cell as two bytes, low byte first; cells the file does not give count
as erased), so it is much faster than `verify` but only says whether a region differs, not
where. The polynomial is the one DS40002266B 3.6 names for MPLAB's hex file checksum, but
whether these values equal the checksum MPLAB shows has not been confirmed.

`selftest` reads each ICSP line with the STM32's weak pull-up and pull-down to find lines
held high or low, then drives floating lines one at a time to find shorts between them. It
never drives a line it found held, so a short to a supply is reported, not fought. It
cannot find an open wire (that floats like a connected input) or ICSPCLK and ICSPDAT
swapped; `probe` failing on wiring that passes `selftest` points at those. The details are
in `include/selftest.h`.

Without installing, run it from the checkout with `uv run --project host picprog ...`.
The installed tool carries its own copy of `data/pic_devices.csv`, taken at install time
(`host/hatch_build.py`), so reinstall it after adding a part.

`--port` selects the serial port (default `/dev/ttyACM0`). Every command probes the part
first and checks its DCI against `data/pic_devices.csv`, the same table the firmware is
built from.

Hex files follow the PIC16F171XX programming specification (DS40002266B 3.5): INHX32, two
bytes per word, low byte first, at twice the word address, so Configuration Word 1 is at
`0x1000E`. Configuration Words and User IDs are written and dumped with the program. A
device ID in the file (at `0x1000C`) is checked against the part, and a missing
Configuration Word section gives a warning, as 3.5.1 and 3.5.2 ask. A dump whose flash
reads as all zeros is flagged as probably code protected (3.4).

Not yet confirmed: where an MPLAB hex file puts PIC16 EEPROM data (assumed `0x1E000`, one
byte per word), and the byte order of PIC18 words in a hex file (assumed low byte first).
Check both against a hex file built by MPLAB before relying on them.

## Tests

```sh
tests/run_tests.sh
```

Builds with the PC compiler, not the ARM toolchain. The host tool's tests (pytest) run the
firmware's command layer compiled for the PC (`tests/host_sim_main.c`) in front of the
simulated PIC, so they go through the real serial protocol end to end.

### Flash

```sh
STM32_Programmer_CLI -c port=SWD mode=UR -w build/Release/pic_programmer_debugger.hex -v -rst
```

## License

MIT, see `LICENSE`.
