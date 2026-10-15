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

Each build writes `pic_programmer_debugger` (ELF), `.bin` and `.hex` into its build directory, and the link step prints flash and RAM used out of the device maximum, for example `FLASH: 3664 B  128 KB  2.80%` (Release). RAM covers static data only (`.data` + `.bss`); the stack is not counted.

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
(both in the PIC16F171XX family). The PIC18F04/05/06/14/15/16Q41 are also in the device
table, and the plan covers PIC10, PIC12 and all of PIC16 and PIC18. `PLAN.md` has the
scope, sources, assumptions and milestones.

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
  - `p16_8bit.c`: the PIC16F171XX command set: identify, read, bulk and row erase, row
    programming through the write latches, User ID, Configuration Words and EEPROM.
  - `p18_8bit.c`: the PIC18-Q command set, which uses similar command bytes that mean
    different things, so it is a separate protocol.
- `src/icsp.c`: pins and microsecond delays only, and the only hardware-specific file
  under the protocols. The three pins use low output speed (OSPEEDR `01`,
  RM0444 7.5.3): ICSPCLK runs at about 500 kHz, far below that setting's limit, and the
  slower edges ring less on a programming cable. ICSPDAT has a weak pull-down, so a
  missing target reads as 0. Delays poll SysTick as a free-running counter on the core
  clock (its interrupt stays off), so they stay correct at 64 MHz or the 16 MHz fallback.
- `tests/`: each protocol run on the PC against a simulated device that also checks every
  wait the spec requires, once for every table row that uses the protocol (14 PIC16F171XX
  parts, 6 PIC18-Q41 parts). The simulators hold their own memory sizes, copied from the
  spec, and the tests compare the part's DCI with its table row, so a wrong size in the
  CSV fails the tests. Run `tests/run_tests.sh`.

At boot `main()` probes the target once, trying `p16_8bit` first and `p18_8bit` second.
Read the result over SWD: `g_pic_status` is 0 when a part was found, else the `PIC_ERR_*` code saying why not. `g_pic_device` points at the matching table row (`print
*g_pic_device` in GDB shows its name and geometry) or is NULL, and `g_pic_device_id` and
`g_pic_revision_id` hold the IDs (addresses in the map file). A PIC16F17146 should read
`0x30E1` and a PIC16F17124 `0x30DC`. With nothing attached, the IDs are 0.

The API reads the device ID, revision ID and the five DCI values, but not the other
read-only areas (the PIC16 DIA at 0x8100, for example); that would be a new read-only
region.

Only low-voltage entry is supported: a part whose LVP configuration bit is 0 cannot be
reached without a high-voltage (7.9 to 9.0 V) MCLR supply.

Not yet tested on real silicon. The PIC16F171XX payload layout comes from figures in the
spec and the protocols share one reading of it with the simulators, so a first run on a real
PIC16F17146 or PIC16F17124 is the real test. See `PLAN.md`.

### Adding a part

1. Find the part's programming specification and data sheet and check which protocol its
   command set matches (`include/pic_proto.h` describes each one).
2. Add a row to `data/pic_devices.csv` with the device ID, flash size in words, erase and
   latch row sizes (the DCI table), User ID words, configuration count, EEPROM bytes, and
   the document sections they came from in `source`.
3. Add the part's geometry to that protocol's simulator in `tests/`, also from the
   document, so the tests cross-check the row, then run `tests/run_tests.sh`.

A part whose command set matches no protocol needs a new protocol; `PLAN.md` says how.

## Tests

```sh
tests/run_tests.sh
```

Builds with the PC compiler, not the ARM toolchain.

### Flash

```sh
STM32_Programmer_CLI -c port=SWD mode=UR -w build/Release/pic_programmer_debugger.hex -v -rst
```

## License

MIT, see `LICENSE`.
