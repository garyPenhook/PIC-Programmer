#ifndef OLED_PORT_H
#define OLED_PORT_H

/*
 * Board wiring for the OLED module that is not the I2C bus itself. oled.c calls this, so it
 * stays free of GPIO code and runs in the PC tests (which provide a stub).
 *
 * Configured for the 2.42" SSD1309 module with a 7-pin header (GND VCC SCL SDA RES DC CS),
 * after it has been changed from SPI to I2C on its back:
 *
 *     Module   Nucleo
 *     GND      GND
 *     VCC      3V3
 *     SCL      D15 (PB8)
 *     SDA      D14 (PB9)
 *     RES      D7  (PA8)   driven by oled_port_reset()
 *     DC       GND         SA0 = 0, address 0x3C (0x3D is tried too)
 *     CS       GND         not used in I2C mode
 */

/*
 * Pulse RES# low and release it. The SSD1309 needs at least 3 us low (datasheet Table
 * 13-1, 8.8); afterwards every register is at its reset value (8.4).
 */
void oled_port_reset(void);

#endif
