#include "cmd.h"

#include <stddef.h>

#include "platform.h"
#include "uart.h"
#include "ui.h"

#define LINE_MAX 64u
#define ARG_MAX  4u

static const pic_device_t *device;
static char line[LINE_MAX];
static size_t line_len;
static int line_too_long;
static int line_rx_error;

/* CRC of the data line being sent; only bytes sent while crc_on is set are included. */
static uint16_t crc;
static int crc_on;

/* CRC-16/CCITT-FALSE, one byte at a time. */
static uint16_t crc16_update(uint16_t value, uint8_t byte)
{
    value = (uint16_t)(value ^ (uint16_t)((uint16_t)byte << 8));
    for (int i = 0; i < 8; i++) {
        value = (value & 0x8000u) != 0u ? (uint16_t)((uint16_t)(value << 1) ^ 0x1021u)
                                        : (uint16_t)(value << 1);
    }
    return value;
}

static void out_char(char c)
{
    if (crc_on) {
        crc = crc16_update(crc, (uint8_t)c);
    }
    uart_write_byte((uint8_t)c);
}

static void out_str(const char *s)
{
    while (*s != '\0') {
        out_char(*s++);
    }
}

/* `digits` upper-case hex digits of value, most significant first. */
static void out_hex(uint32_t value, uint32_t digits)
{
    while (digits > 0u) {
        digits--;
        out_char("0123456789ABCDEF"[(value >> (digits * 4u)) & 0xFu]);
    }
}

static void out_int(int32_t value)
{
    char digits[11];
    size_t n = 0;
    uint32_t magnitude = value < 0 ? 0u - (uint32_t)value : (uint32_t)value;

    if (value < 0) {
        out_char('-');
    }
    do {
        digits[n++] = (char)('0' + magnitude % 10u);
        magnitude /= 10u;
    } while (magnitude != 0u);
    while (n > 0u) {
        out_char(digits[--n]);
    }
}

static void reply_error(const char *word)
{
    out_str("ERR ");
    out_str(word);
    out_char('\n');
}

static void reply_status(pic_status_t status)
{
    out_str("ERR ");
    out_int(status);
    out_char(' ');
    out_str(pic_strerror(status));
    out_char('\n');
}

static int streq(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* Decimal or 0x hex, no sign, no spaces, must fit in 32 bits. Returns 1 on success. */
static int parse_number(const char *s, uint32_t *value)
{
    uint32_t base = 10u;
    uint32_t acc = 0u;

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16u;
        s += 2;
    }
    if (*s == '\0') {
        return 0;
    }
    for (; *s != '\0'; s++) {
        uint32_t digit;

        if (*s >= '0' && *s <= '9') {
            digit = (uint32_t)(*s - '0');
        } else if (base == 16u && *s >= 'a' && *s <= 'f') {
            digit = (uint32_t)(*s - 'a') + 10u;
        } else if (base == 16u && *s >= 'A' && *s <= 'F') {
            digit = (uint32_t)(*s - 'A') + 10u;
        } else {
            return 0;
        }
        if (acc > (0xFFFFFFFFu - digit) / base) {
            return 0;
        }
        acc = acc * base + digit;
    }
    *value = acc;
    return 1;
}

static int parse_region(const char *s, pic_region_t *region)
{
    if (streq(s, "flash")) {
        *region = PIC_REGION_FLASH;
    } else if (streq(s, "userid")) {
        *region = PIC_REGION_USERID;
    } else if (streq(s, "config")) {
        *region = PIC_REGION_CONFIG;
    } else if (streq(s, "eeprom")) {
        *region = PIC_REGION_EEPROM;
    } else {
        return 0;
    }
    return 1;
}

/* Split the line in place at spaces and tabs. Returns the field count, or -1 if there
 * are more than ARG_MAX. */
static int split(char *text, char *argv[ARG_MAX])
{
    int argc = 0;

    while (*text != '\0') {
        if (*text == ' ' || *text == '\t') {
            *text++ = '\0';
            continue;
        }
        if ((size_t)argc == ARG_MAX) {
            return -1;
        }
        argv[argc++] = text;
        while (*text != '\0' && *text != ' ' && *text != '\t') {
            text++;
        }
    }
    return argc;
}

static void do_probe(void)
{
    const pic_device_t *found = NULL;
    uint16_t id = 0u;
    uint16_t rev = 0u;
    pic_status_t st = pic_probe(&found, &id, &rev);

    device = found; /* NULL after a failed probe: forget the old device */
    ui_probe(st, found, id, rev);
    if (st != PIC_OK) {
        reply_status(st);
        return;
    }
    out_str("OK ");
    out_str(found->name);
    out_char(' ');
    out_hex(id, 4u);
    out_char(' ');
    out_hex(rev, 4u);
    out_char('\n');
}

static void send_chunk(uint32_t offset, const uint16_t *cells, size_t count)
{
    crc = 0xFFFFu;
    crc_on = 1;
    out_char('D');
    out_char(' ');
    out_hex(offset, 8u);
    for (size_t i = 0; i < count; i++) {
        out_char(' ');
        out_hex(cells[i], 4u);
    }
    crc_on = 0;
    out_str(" *");
    out_hex(crc, 4u);
    out_char('\n');
}

static void out_byte(uint8_t byte)
{
    out_char((char)byte);
}

static void out_le16(uint16_t value)
{
    out_byte((uint8_t)(value & 0xFFu));
    out_byte((uint8_t)(value >> 8));
}

/* Binary data frame for READB (layout in cmd.h). */
static void send_chunk_binary(uint32_t offset, const uint16_t *cells, size_t count)
{
    crc = 0xFFFFu;
    crc_on = 1;
    out_byte(CMD_FRAME_MARKER);
    out_le16((uint16_t)(offset & 0xFFFFu));
    out_le16((uint16_t)(offset >> 16));
    out_byte((uint8_t)count);
    for (size_t i = 0; i < count; i++) {
        out_le16(cells[i]);
    }
    crc_on = 0;
    out_le16(crc);
}

static void do_read(char *argv[ARG_MAX], int binary)
{
    pic_region_t region;
    uint32_t offset;
    uint32_t count;
    uint16_t cells[CMD_CHUNK_CELLS];

    if (device == NULL) {
        reply_error("no-device");
        return;
    }
    if (!parse_region(argv[1], &region) || !parse_number(argv[2], &offset) ||
        !parse_number(argv[3], &count)) {
        reply_error("bad-argument");
        return;
    }
    if (count == 0u) {
        out_str("OK 0\n");
        return;
    }
    /* Check the range before touching the target, so a bad request sends nothing to it. */
    uint32_t size = pic_region_size(device, region);
    if (offset >= size || count > size - offset) {
        reply_status(PIC_ERR_RANGE);
        return;
    }

    const char *name = binary ? "READB" : "READ";
    pic_status_t st = pic_enter(device);
    if (st != PIC_OK) {
        ui_result(name, st, 0u);
        reply_status(st);
        return;
    }
    for (uint32_t done = 0; done < count;) {
        uint32_t n = count - done < CMD_CHUNK_CELLS ? count - done : CMD_CHUNK_CELLS;

        /* A long read can outlast a watchdog timeout, so kick it once per chunk. */
        platform_watchdog_kick();
        st = pic_read(device, region, offset + done, cells, n);
        if (st != PIC_OK) {
            pic_exit(device);
            ui_result(name, st, done);
            reply_status(st);
            return;
        }
        if (binary) {
            send_chunk_binary(offset + done, cells, n);
        } else {
            send_chunk(offset + done, cells, n);
        }
        done += n;
    }
    pic_exit(device);
    ui_result(name, PIC_OK, count);
    out_str("OK ");
    out_int((int32_t)count);
    out_char('\n');
}

static void execute(void)
{
    char *argv[ARG_MAX];
    int argc = split(line, argv);

    if (argc == 0) {
        return; /* blank line */
    }
    if (argc > 0 && streq(argv[0], "PROBE") && argc == 1) {
        do_probe();
    } else if (argc > 0 && streq(argv[0], "READ") && argc == 4) {
        do_read(argv, 0);
    } else if (argc > 0 && streq(argv[0], "READB") && argc == 4) {
        do_read(argv, 1);
    } else {
        reply_error("bad-command");
    }
}

void cmd_set_device(const pic_device_t *dev)
{
    device = dev;
}

void cmd_rx_error(void)
{
    line_rx_error = 1;
}

void cmd_feed(uint8_t byte)
{
    if (byte == (uint8_t)'\r') {
        return;
    }
    if (byte != (uint8_t)'\n') {
        if (line_len < LINE_MAX - 1u) {
            line[line_len++] = (char)byte;
        } else {
            line_too_long = 1;
        }
        return;
    }

    line[line_len] = '\0';
    if (line_too_long) {
        reply_error("line-too-long");
    } else if (line_rx_error) {
        reply_error("rx-error");
    } else {
        execute();
    }
    line_len = 0u;
    line_too_long = 0;
    line_rx_error = 0;
}
