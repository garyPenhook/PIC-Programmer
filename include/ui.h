#ifndef UI_H
#define UI_H

#include <stdint.h>

#include "pic.h"

/*
 * Status screen on the optional OLED (oled.h). Each call redraws only the lines that
 * changed and sends them before returning, so callers make these calls before they send
 * their final reply on the serial port: the host cannot send a new command while the
 * screen is being written, and the polled UART cannot drop its bytes.
 *
 * Every call is a no-op when no display is attached.
 */

/* Find the display and draw the title. */
void ui_init(void);

/* Result of a probe: the device row (or NULL), the IDs read, and the status. */
void ui_probe(pic_status_t status, const pic_device_t *dev, uint16_t id, uint16_t rev);

/* Result of any other command that reached the target: its name, status and cell count. */
void ui_result(const char *command, pic_status_t status, uint32_t count);

/* A free-form result: `summary` on the result row, `detail` (may be "") under it. */
void ui_note(const char *summary, const char *detail);

#endif
