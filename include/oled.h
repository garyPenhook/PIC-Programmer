#ifndef OLED_H
#define OLED_H

#include <stdint.h>

/*
 * 128 x 64 monochrome OLED on an SSD1309 controller, over I2C (i2c.h). Text only: a 5 x 7
 * font in 6 x 8 cells gives 8 rows of 21 characters.
 *
 * Drawing goes into a RAM frame buffer; oled_flush() sends only the 128-byte pages that
 * changed since the last flush. If no display answers at init, every call is a no-op, so
 * the programmer works the same without one.
 *
 * Module wiring and assumptions (check against your module):
 *   - SDA, SCL, VDD (3.3 V), GND. The SSD1309 needs a separate 7 to 16 V panel supply VCC
 *     (SSD1309 2, 8.8), so the module must generate it from its own supply.
 *   - RES# is pulsed low by oled_port_reset() at init (wiring in oled_port.h).
 *   - The address is 0x3C (SA0 = D/C# low) or 0x3D (SA0 high) (SSD1309 8.1.5); both are
 *     tried.
 */
#define OLED_COLS 21u
#define OLED_ROWS 8u

/*
 * Set to 1 if the picture is upside down: it sends segment re-map (A1h) and COM scan
 * remapped (C8h) instead of the reset values. How the panel is mounted on the module
 * decides which is right.
 */
#ifndef OLED_FLIP
#define OLED_FLIP 0
#endif

/* Reset, find and initialise the display, then clear it. Returns 1 if one answered,
 * else 0. Call after icsp_init(): the reset pulse is timed with icsp_delay_us(). */
int oled_init(void);

/* 1 if oled_init() found a display. */
int oled_present(void);

/* Write text to a row, padded with spaces to the full width; longer text is cut off.
 * Only bytes 0x20 to 0x7E are drawn; anything else shows as '?'. */
void oled_line(uint32_t row, const char *text);

/* Send the pages that changed. Returns 0 on success (or no display), -1 if the display
 * stopped answering, after which it is treated as absent. */
int oled_flush(void);

#endif
