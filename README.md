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

`platform_init()` calls `clock_init()` (`src/clock.c`, `include/clock.h`) to run the core at **1 MHz** and toggles the on-board user LED (LD4, PA5 / Arduino D13, active high) from a timer interrupt every **1 s** (1 s on, 1 s off).

- **SYSCLK**: HSISYS = HSI16 / 16 (`RCC_CR.HSIDIV = 100b`). HSI16 is already on and selected after reset, so no source switch is needed. HCLK and PCLK stay undivided at 1 MHz. `SystemCoreClock` is set to `1000000`.
- **Why 1 MHz**: it is below the 2 MHz limit for Low-power run mode (RM0444 4.3.2), so `PWR_CR1.LPR` can be added later without changing the clock.
- **Timer**: TIM16 on the 1 MHz PCLK. `PSC = 99` gives a 10 kHz tick and `ARR = 9999` gives a 1 s update event. The update interrupt is enabled in `TIM16_DIER` and in the NVIC (IRQ 21).
- **Vector table**: `src/startup_stm32g071rb.s` covers the core exceptions plus IRQ 0..21. Slot 0x94 (`TIM16 / FDCAN_IT0`) points to `TIM16_IRQHandler` in `src/platform.c`; the other IRQ slots use `Default_Handler`. Anything else enabled later needs its own slot.
- **ISR**: `TIM16_IRQHandler` clears `UIF` (rc_w0) and toggles PA5. It is the only code that writes PA5, so the main loop needs no locking. The main loop only kicks the watchdog.
- **Flash wait states**: left at the reset value; no change is needed at 1 MHz.
- **Change the rate**: edit `LED_TOGGLE_MS` in `src/platform.c` (max about 6.5 s with this prescaler, since ARR is 16-bit).

Verified on hardware by reading registers over SWD: `RCC_CR = 0x2500`, `TIM16_PSC = 99`, `TIM16_ARR = 9999`, `NVIC_ISER` bit 21 set, and PA5 changing state every ~1 s.

### Flash

```sh
STM32_Programmer_CLI -c port=SWD mode=UR -w build/Release/nucleo_g071rb.hex -v -rst
```
