# PIC Programmer: Utility Ideas

The firmware uses about 3% of flash and only four pins, so there is room for a full bench toolkit around the ICSP programmer.

## Where the project stands

- STM32G071RB on a NUCLEO-G071RB, bare-metal C, about 600 lines; flash use is 3,664 B of 128 KB in the Release build.
- Pins in use: PB3 (ICSPCLK), PB4 (ICSPDAT), PB5 (MCLR) and PA5 (LED).
- Planned already (`PLAN.md`): M4 command interface over the ST-LINK virtual COM port, M5 Python host tool, M6 more device rows, M7 high-voltage MCLR entry.
- Everything below is additional to those milestones, except where noted.

## Highest value for PIC dev

These five make the programmer safer and more useful day to day, and most are firmware plus a few cheap parts.

| Idea | What it does | Extra hardware |
| --- | --- | --- |
| UART bridge and logger | Bridges a spare USART to the host over the ST-LINK VCP, so target `printf` output shows on the PC, even while the programmer holds the part in reset or reflashes it. Optional baud autodetect. This is the debug-agent channel in `PLAN.md`. | None beyond two wires |
| Target power control | A GPIO switches target VDD through a P-FET or load switch, so the host can power-cycle the target for a clean LVP entry. | P-FET or load switch |
| VDD and current measurement | ADC reads target VDD before entering programming mode and warns or refuses if it is out of range; optional shunt amplifier reads target current to catch shorts and show sleep current. | Divider, optional shunt amplifier |
| High-voltage MCLR (M7) | Boost converter (about 8 to 9 V) and a switch recover parts with LVP off. An ADC check confirms the rail has settled before it touches MCLR. | Boost converter, switch |
| Wiring diagnostic | Before programming, drive each ICSP line and read the others to report shorts, a floating MCLR or swapped CLK and DAT. | None |

## Debug and bring-up tools

These turn the programmer into a general bench instrument for the PIC being developed.

| Idea | What it does |
| --- | --- |
| Logic analyzer or ICSP sniffer | Captures ICSP traffic with a timer and DMA into SRAM (about 36 KB available) and streams it to the host. Useful for checking your own protocol timing and for comparing against a PICkit. |
| Signal generator | Uses spare timers for a test clock or PWM, so a target with no crystal gets a known clock, or a PIC input gets a known stimulus. |
| I2C or SPI host | Pokes the PIC's I2C or SPI slave peripherals from the programmer so they can be tested without a separate board. |
| GPIO monitor and driver | Names a few spare pins as test points that the host can set or read, plus a wait-for-edge command with a timeout for automated tests. |

## Production and safety

These make the tool trustworthy on the bench and usable without a PC.

| Idea | What it does |
| --- | --- |
| Stored hex image | Keeps a golden image in spare STM32 flash or an external SPI flash, with a button to program standalone and pass/fail LEDs. A small bench production tool. |
| Protection handling | Detects code-protect and read-protect bits, warns before an erase, and adds blank-check and checksum commands. |
| Config-word editor | Reads, decodes and writes config words by name (WDT, BOR, LVP and so on), driven by the same CSV device table so each part decodes its own bits. |
| Self-test mode | Checks the 3.3 V rail, pin directions and timing against a known loopback fixture before the programmer is trusted. |

## Where to start

Start with the UART bridge together with target power and VDD measurement: they are mostly firmware, need one FET and a resistor divider, and make the M4 command interface far more useful. High-voltage MCLR is the next real feature, since it recovers parts that otherwise cannot be programmed.

## Open questions

- [ ] Which spare pins, timers, DMA channels and ADC inputs are free on the STM32G071RB? Not yet checked against the datasheet and RM0444; confirm before committing to any pin plan.
- [ ] Is the standalone hex-image feature wanted, and if so, internal flash or external SPI flash?
- [ ] Which idea should be planned or built first?
