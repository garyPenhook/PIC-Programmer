# Hookup guide

How to wire the NUCLEO-G071RB to a target PIC and to the optional OLED status display.
Every pin below is taken from a document; the source is given with each table.

![Hookup schematic](hookup.svg)

## MCU pins in use

| MCU pin | Header | Arduino | Use | Source |
|---|---|---|---|---|
| PB3 | CN9-4 | D3 | ICSPCLK to the target (output) | UM2324 Table 12, `include/icsp.h` |
| PB4 | CN9-6 | D5 | ICSPDAT to the target (bidirectional, weak pull-down) | UM2324 Table 12, `include/icsp.h` |
| PB5 | CN9-5 | D4 | MCLR to the target (output) | UM2324 Table 12, `include/icsp.h` |
| PB8 | CN5-10 | D15 | I2C1_SCL to the OLED (open drain, AF6) | UM2324 Table 12, DS Table 14 |
| PB9 | CN5-9 | D14 | I2C1_SDA to the OLED (open drain, AF6) | UM2324 Table 12, DS Table 14 |
| PA8 | CN9-8 | D7 | OLED RES (output, pulsed low at boot) | UM2324 Table 12 |
| PA2 | - | - | USART2 TX to the ST-LINK virtual COM port | UM2324 6.9, Table 8 |
| PA3 | - | - | USART2 RX from the ST-LINK virtual COM port | UM2324 6.9, Table 8 |
| PA5 | - | D13 | On-board user LED LD4 | UM2324 6.10 |

Power and ground come from the CN6 power header: 3V3 on CN6-4, GND on CN6-6 and CN6-7, and
a further GND on CN5-7 (UM2324 Table 12).

PA2/PA3 reach the virtual COM port through solder bridges SB16/SB18, which are fitted by
default (UM2324 Table 8). There is nothing to wire for the serial link: the ST-LINK USB
cable carries power, SWD and the serial port.

Do not connect anything to CN8-5 or CN8-6 (A4/A5). UM2324 Table 12 lists I2C1 on those pins
too ("PB9 or PB11", "PB8 or PB12", depending on solder bridges), so they may be tied to the
I2C pins the OLED uses.

## Target PIC

The PIC is powered from the Nucleo's 3.3 V, within the 1.8 to 5.5 V programming range of
the PIC16F171XX programming specification. Only low-voltage programming is supported: the
part must have its LVP configuration bit set (the factory default), because the Nucleo
cannot supply the 7.9 to 9.0 V high-voltage MCLR level.

| Nucleo | Signal | PIC16F17124 14-pin SOIC/TSSOP/PDIP | PIC16F17124 16-pin QFN/VQFN | PIC16F17146 20-pin SSOP/SOIC/PDIP | PIC16F17146 20-pin QFN/VQFN-S |
|---|---|---|---|---|---|
| D3 (PB3) | ICSPCLK (RA1) | 12 | 11 | 18 | 15 |
| D5 (PB4) | ICSPDAT (RA0) | 13 | 12 | 19 | 16 |
| D4 (PB5) | MCLR (RA3) | 4 | 3 | 4 | 1 |
| 3V3 (CN6-4) | VDD | 1 | 16 | 1 | 18 |
| GND (CN6-6) | VSS | 14 | 13 | 20 | 17 |

Source: PIC16F171XX Family Programming Specification DS40002266B, Table 6-1. For other
parts in the device table, take the ICSPCLK, ICSPDAT, MCLR, VDD and VSS pins from that
part's own programming specification.

## OLED status display (optional)

For the 2.42" SSD1309 128 x 64 module with a 7-pin header (GND VCC SCL SDA RES DC CS). The
header order does not match the Nucleo's Arduino header (SCL and SDA are swapped relative
to D15/D14, and the STM32 cannot swap them: PB8 only offers SCL and PB9 only SDA), so use
jumper leads.

| Module | Nucleo | Notes |
|---|---|---|
| GND | GND (CN6-7) | |
| VCC | 3V3 (CN6-4) | Not CN5-8: that pin is AVDD/VREF+, the analog reference supply |
| SCL | D15 (PB8) | |
| SDA | D14 (PB9) | |
| RES | D7 (PA8) | Held low for 10 us at boot (SSD1309 minimum 3 us, datasheet Table 13-1) |
| DC | GND | Selects I2C address 0x3C (3V3 gives 0x3D; the firmware tries both) |
| CS | GND | Not used in I2C mode |

Prepare the module first (from seller listings and user reports, not a module datasheet;
check the silkscreen on your board):

1. **Switch it from SPI to I2C.** It ships set to SPI. On boards with R3/R4/R5 next to the
   header, remove R4 and bridge R3 and R5. Other revisions use R13/R15/R17 instead.
2. **Pull-ups.** The SSD1309 requires pull-ups on SDA and SCL (datasheet 8.1.5), and it is
   not confirmed that the module has them. If the display does not answer, fit 4.7 kOhm from
   each line to 3V3.

The SSD1309 also needs a 7 to 16 V panel supply besides its logic supply (datasheet 2, 8.8),
so the module must generate that from its VCC pin.

Without a display, leave these pins unconnected; the firmware finds no display and does
not use the bus.

## Before powering up

1. Unplug the ST-LINK USB cable while wiring.
2. Check VDD and VSS on the target against the table for its package.
3. Check that nothing else on the target board drives MCLR, ICSPCLK or ICSPDAT.
4. Plug in USB. LD4 toggles every second when the firmware is running.

## Checking the result

- Over SWD: `g_pic_status` is 0 when a part was recognised. A PIC16F17146 reads device ID
  `0x30E1`, a PIC16F17124 `0x30DC` (DS40002266B 2.6).
- Over the serial port (921600 baud, 8N1): send `PROBE`; the reply is
  `OK <name> <device id> <revision id>` or `ERR <code> <text>`.
- `ERR -5 no target answered`: the ID read back as 0. Check the wiring, target power, and
  that LVP is enabled on the part.
- No picture on the OLED: check the SPI-to-I2C change, the pull-ups and the RES wire. If
  the picture is upside down, rebuild with `-DOLED_FLIP=1`.
