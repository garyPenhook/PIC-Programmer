# NUCLEO-G071RB Security Template

Bare-metal STM32G071RB project skeleton for security-sensitive firmware.

## Goals

- Signed firmware/update policy hooks.
- Authenticated command parser.
- Fail-closed boot decisions.
- Watchdog and fault hooks.
- No private signing key in firmware.

## Memory Map

- `0x08000000`: bootloader / first-stage app
- `0x08010000`: application slot
- metadata page: TBD (the old `0x0803F000` is outside the 128 KiB flash)
- `0x08020000`: end of 128 KiB flash (STM32G071RB; 36 KiB SRAM)

Adjust in `linker/STM32G071RB_FLASH.ld` before production.

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

Each build writes `nucleo_g071rb_security` (ELF), `.bin` and `.hex` into its build directory, and the link step prints flash and RAM used out of the device maximum, for example `FLASH: 512 B / 128 KB`. RAM covers static data only (`.data` + `.bss`); the stack is not counted.

## Clock and LED Blink

`platform_init()` runs the core at **1 MHz** and toggles the on-board user LED (LD4, PA5 / Arduino D13, active high) from a timer interrupt every **1 s** (1 s on, 1 s off).

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
STM32_Programmer_CLI -c port=SWD mode=UR -w build/Release/nucleo_g071rb_security.hex -v -rst
```

## Production Checklist

- Enable BOR and watchdog.
- Set RDP Level 1 after provisioning.
- Disable or gate debug command handlers.
- Use signed updates only.
- Add anti-rollback counter in protected flash/option bytes.
- Replace `crypto_stub.c` with reviewed crypto backend.
- Provision per-device identity/key material.

## Debug Lock Warning

RDP Level 2 is often irreversible. Use Level 1 for normal production unless you have a tested factory recovery plan.

