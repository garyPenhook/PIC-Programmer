#include "ui.h"

#include <stddef.h>

#include "oled.h"

/*
 * Layout (8 rows of 21 characters):
 *   0  PIC programmer
 *   2  device name, or "no device"
 *   3  ID xxxx  rev xxxx
 *   5  last command and its result
 *   6  error text, if it failed
 */
#define ROW_TITLE  0u
#define ROW_DEVICE 2u
#define ROW_IDS    3u
#define ROW_LAST   5u
#define ROW_ERROR  6u

#define LINE_LEN (OLED_COLS + 1u)

/* Small append-to-buffer helpers; text past the end of the line is dropped. */
static void put_str(char *line, size_t *n, const char *s)
{
    while (*s != '\0' && *n < LINE_LEN - 1u) {
        line[(*n)++] = *s++;
    }
    line[*n] = '\0';
}

static void put_hex4(char *line, size_t *n, uint16_t value)
{
    char digits[5];

    for (uint32_t i = 0; i < 4u; i++) {
        digits[i] = "0123456789ABCDEF"[(value >> (12u - 4u * i)) & 0xFu];
    }
    digits[4] = '\0';
    put_str(line, n, digits);
}

static void put_dec(char *line, size_t *n, int32_t value)
{
    char digits[12];
    size_t len = 0;
    uint32_t magnitude = value < 0 ? 0u - (uint32_t)value : (uint32_t)value;

    do {
        digits[len++] = (char)('0' + magnitude % 10u);
        magnitude /= 10u;
    } while (magnitude != 0u);
    if (value < 0) {
        digits[len++] = '-';
    }
    char out[12];
    for (size_t i = 0; i < len; i++) {
        out[i] = digits[len - 1u - i];
    }
    out[len] = '\0';
    put_str(line, n, out);
}

static void show_status(pic_status_t status)
{
    oled_line(ROW_ERROR, status == PIC_OK ? "" : pic_strerror(status));
}

void ui_init(void)
{
    if (oled_init()) {
        oled_line(ROW_TITLE, "PIC programmer");
        oled_line(ROW_DEVICE, "no device");
        (void)oled_flush();
    }
}

void ui_probe(pic_status_t status, const pic_device_t *dev, uint16_t id, uint16_t rev)
{
    char line[LINE_LEN];
    size_t n = 0;

    if (!oled_present()) {
        return;
    }
    oled_line(ROW_DEVICE, dev != NULL ? dev->name : "no device");

    put_str(line, &n, "ID ");
    put_hex4(line, &n, id);
    put_str(line, &n, "  rev ");
    put_hex4(line, &n, rev);
    oled_line(ROW_IDS, line);

    n = 0;
    put_str(line, &n, status == PIC_OK ? "PROBE ok" : "PROBE ERR ");
    if (status != PIC_OK) {
        put_dec(line, &n, status);
    }
    oled_line(ROW_LAST, line);
    show_status(status);
    (void)oled_flush();
}

void ui_result(const char *command, pic_status_t status, uint32_t count)
{
    char line[LINE_LEN];
    size_t n = 0;

    if (!oled_present()) {
        return;
    }
    put_str(line, &n, command);
    if (status == PIC_OK) {
        put_str(line, &n, " ok ");
        put_dec(line, &n, (int32_t)count);
    } else {
        put_str(line, &n, " ERR ");
        put_dec(line, &n, status);
    }
    oled_line(ROW_LAST, line);
    show_status(status);
    (void)oled_flush();
}
