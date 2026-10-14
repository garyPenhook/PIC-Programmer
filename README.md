# NUCLEO-G071RB Bare-Metal Template

Bare-metal C project for the STM32G071RB (NUCLEO-G071RB board): no HAL, no RTOS. It runs the core at 1 MHz and blinks the user LED from a timer interrupt.

## Memory

- `0x08000000`: flash, 128 KiB (vector table first, then code)
- `0x20000000`: SRAM, 36 KiB (STM32G071RB without RAM parity)

Sizes are set in `linker/STM32G071RB_FLASH.ld`.

## Build

Requires `arm-none-eabi-gcc` (the toolchain file pins the GNU Tools for STM32 14.3.1 bundle).

| Preset | Flags | Output |
|---|---|---|
| `Debug` | `-Og -g3` | `build/Debug/` |
| `Release` | `-Os -DNDEBUG` | `build/Release/` |

```sh
cmake --preset Debug   && cmake --build --preset Debug
cmake --preset Release && cmake --build --preset Release
```

Each build writes `nucleo_g071rb` (ELF), `.bin` and `.hex` into its build directory, and the link step prints flash and RAM used out of the device maximum, for example `FLASH: 512 B / 128 KB`. RAM covers static data only (`.data` + `.bss`); the stack is not counted.

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
(both in the PIC16F171XX family). PIC18-Q devices are also supported, and the plan covers
all PIC18. `PLAN.md` has the scope, sources, assumptions and milestones.

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

Layers:

- `src/icsp.c`: pins and microsecond delays only. The only hardware-specific file.
- `src/pic16f171.c`: the PIC16F171XX protocol: low-voltage entry, identify, read, bulk and
  row erase, 32-word row programming, User ID, Configuration Words and EEPROM, verify.
- `src/pic18q.c`: the PIC18-Q protocol, which uses similar commands that mean different
  things, so it is a separate driver.
- `tests/`: each driver run on the PC against a simulated device that also checks every
  wait the spec requires. The PIC16F171XX tests run once as a PIC16F17124 (4 KW flash,
  128 byte EEPROM) and once as a PIC16F17146 (16 KW, 256 bytes). Run `tests/run_tests.sh`.

At boot `main()` probes the target once, trying PIC16F171XX first and PIC18-Q second. Read
the result over SWD from `g_pic_family` (16, 18, or 0 for nothing recognised),
`g_pic_device_id` and `g_pic_revision_id` (addresses in the map file). A PIC16F17146 should
read `0x30E1` and a PIC16F17124 `0x30DC`. With nothing attached, all three are 0.

Only low-voltage entry is supported: a part whose LVP configuration bit is 0 cannot be
reached without a high-voltage (7.9 to 9.0 V) MCLR supply.

Not yet tested on real silicon. The PIC16F171XX payload layout comes from figures in the
spec and the drivers share one reading of it with the simulator, so a first run on a real
PIC16F17146 or PIC16F17124 is the real test. See `PLAN.md`.

## Tests

```sh
tests/run_tests.sh
```

Builds with the PC compiler, not the ARM toolchain.

### Flash

```sh
STM32_Programmer_CLI -c port=SWD mode=UR -w build/Release/nucleo_g071rb.hex -v -rst
```
