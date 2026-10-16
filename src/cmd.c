#include "cmd.h"

#include <stddef.h>

#include "adc.h"
#include "bridge.h"
#include "clock.h"
#include "platform.h"
#include "selftest.h"
#include "target_power.h"
#include "target_uart.h"
#include "uart.h"
#include "ui.h"

#define LINE_MAX 64u
#define ARG_MAX  4u

/* A WRITE/VERIFY data line: "D", the offset, up to CMD_DATA_LINE_CELLS cells, "*crc". */
#define DATA_ARG_MAX (2u + CMD_DATA_LINE_CELLS + 1u)

/* Cells held back before one pic_write(): a whole row of write latches. Every device in
 * the table has 32 or fewer (data/pic_devices.csv); a larger row is refused. */
#define WRITE_BUF_CELLS 64u

static const pic_device_t *device;
static char line[LINE_MAX];
static size_t line_len;
static int line_too_long;
static int line_rx_error;

/*
 * A WRITE or VERIFY in progress. The target stays in programming mode while the host
 * sends the data, one line per NEXT request.
 */
typedef enum { SESSION_NONE = 0, SESSION_WRITE, SESSION_VERIFY } session_kind_t;

static struct {
    session_kind_t kind;
    const char *name; /* "WRITE" or "VERIFY", for the status screen */
    pic_region_t region;
    uint32_t offset;    /* first cell of the transfer */
    uint32_t count;     /* cells in the transfer */
    uint32_t received;  /* cells accepted so far */
    uint32_t buf_start; /* cell offset of buf[0] */
    uint32_t buffered;  /* cells in buf */
    uint32_t flush_at;  /* cells per pic_write() or pic_verify() call */
    uint16_t buf[WRITE_BUF_CELLS];
} session;

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
 * are more than max. */
static int split(char *text, char *argv[], size_t max)
{
    int argc = 0;

    while (*text != '\0') {
        if (*text == ' ' || *text == '\t') {
            *text++ = '\0';
            continue;
        }
        if ((size_t)argc == max) {
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

    if (!target_power_on()) {
        reply_error("power-off"); /* probing drives the ICSP lines */
        return;
    }
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

/* Common start of a command that acts on the target: a device, and a range inside the
 * region. Replies and returns 0 if either is missing. */
static int need_power(void)
{
    if (!target_power_on()) {
        reply_error("power-off"); /* the ICSP lines are released while the power is off */
        return 0;
    }
    return 1;
}

static int need_device(void)
{
    if (!need_power()) {
        return 0;
    }
    if (device == NULL) {
        reply_error("no-device");
        return 0;
    }
    return 1;
}

static int range_ok(pic_region_t region, uint32_t offset, uint32_t count)
{
    uint32_t size = pic_region_size(device, region);

    if (offset >= size || count > size - offset) {
        reply_status(PIC_ERR_RANGE);
        return 0;
    }
    return 1;
}

static void reply_ok_count(uint32_t count)
{
    out_str("OK ");
    out_int((int32_t)count);
    out_char('\n');
}

static void do_read(char *argv[ARG_MAX], int binary)
{
    pic_region_t region;
    uint32_t offset;
    uint32_t count;
    uint16_t cells[CMD_CHUNK_CELLS];

    if (!need_device()) {
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
    if (!range_ok(region, offset, count)) {
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
    reply_ok_count(count);
}

/* Exactly `digits` hex digits (no 0x prefix), as in the data lines. Returns 1 on success. */
static int parse_hex_digits(const char *s, uint32_t digits, uint32_t *value)
{
    uint32_t acc = 0u;

    for (uint32_t i = 0; i < digits; i++) {
        char c = s[i];
        uint32_t d;

        if (c >= '0' && c <= '9') {
            d = (uint32_t)(c - '0');
        } else if (c >= 'A' && c <= 'F') {
            d = (uint32_t)(c - 'A') + 10u;
        } else if (c >= 'a' && c <= 'f') {
            d = (uint32_t)(c - 'a') + 10u;
        } else {
            return 0;
        }
        acc = (acc << 4) | d;
    }
    if (s[digits] != '\0') {
        return 0;
    }
    *value = acc;
    return 1;
}

/* Optional "<offset> <count>" after the region (argv[2], argv[3]); the whole region if
 * absent. Replies and returns 0 on a bad argument or range. */
static int region_span(int argc, char *argv[ARG_MAX], pic_region_t *region, uint32_t *offset,
                       uint32_t *count)
{
    if (!parse_region(argv[1], region) ||
        (argc == 4 && (!parse_number(argv[2], offset) || !parse_number(argv[3], count)))) {
        reply_error("bad-argument");
        return 0;
    }
    if (argc == 2) {
        *offset = 0u;
        *count = pic_region_size(device, *region);
    }
    return *count == 0u || range_ok(*region, *offset, *count);
}

/* CRC-32 (polynomial 0x04C11DB7, the one in DS40002266B 3.6), bit-reflected with initial
 * value and final XOR 0xFFFFFFFF: the same as zlib's crc32(). This updates the running
 * value without the final XOR. */
static uint32_t crc32_update(uint32_t value, uint8_t byte)
{
    value ^= byte;
    for (int i = 0; i < 8; i++) {
        value = (value >> 1) ^ (0xEDB88320u & (0u - (value & 1u)));
    }
    return value;
}

/*
 * CRC <region> [<offset> <count>] | BLANK <region> [<offset> <count>]
 * Read the cells on the programmer and reply with their CRC-32, or check they are all
 * erased, without sending the data over the serial port.
 */
static void do_scan(int argc, char *argv[ARG_MAX], int blank)
{
    const char *name = blank ? "BLANK" : "CRC";
    pic_region_t region;
    uint32_t offset = 0u;
    uint32_t count = 0u;
    uint16_t cells[CMD_CHUNK_CELLS];
    uint32_t sum = 0xFFFFFFFFu; /* running CRC-32 */

    if (!need_device() || !region_span(argc, argv, &region, &offset, &count)) {
        return;
    }
    uint16_t erased = pic_cell_mask(device, region);
    pic_status_t st = PIC_OK;
    int entered = 0;
    if (count != 0u) {
        st = pic_enter(device); /* on failure it has already released the target */
        entered = st == PIC_OK;
    }
    for (uint32_t done = 0; st == PIC_OK && done < count;) {
        uint32_t n = count - done < CMD_CHUNK_CELLS ? count - done : CMD_CHUNK_CELLS;

        platform_watchdog_kick();
        st = pic_read(device, region, offset + done, cells, n);
        for (uint32_t i = 0; st == PIC_OK && i < n; i++) {
            if (blank && cells[i] != erased) {
                pic_exit(device);
                ui_note("BLANK no", "not erased");
                out_str("ERR not-blank @");
                out_hex(offset + done + i, 8u);
                out_char(' ');
                out_hex(cells[i], 4u);
                out_char('\n');
                return;
            }
            /* Each cell as two bytes, low byte first, as in a hex file. */
            sum = crc32_update(crc32_update(sum, (uint8_t)(cells[i] & 0xFFu)),
                               (uint8_t)(cells[i] >> 8));
        }
        done += n;
    }
    if (entered) {
        pic_exit(device);
    }
    ui_result(name, st, count);
    if (st != PIC_OK) {
        reply_status(st);
        return;
    }
    out_str("OK ");
    if (blank) {
        out_int((int32_t)count);
    } else {
        out_hex(~sum, 8u);
    }
    out_char('\n');
}

/*
 * SELFTEST
 * Wiring check of the ICSP lines (selftest.h). Needs no PROBE.
 *     LINE <CLK|DAT|MCLR> <float|high|low>     one per line
 *     SHORT <line> <line>                       for each short found
 * then OK, or ERR wiring if anything is wrong.
 */
static void do_selftest(void)
{
    selftest_result_t r;

    if (!need_power()) {
        return; /* it ends by driving the lines to idle, which an unpowered target must not see */
    }
    selftest_run(&r);
    for (uint32_t i = 0; i < ICSP_LINES; i++) {
        out_str("LINE ");
        out_str(selftest_line_name((icsp_line_t)i));
        out_char(' ');
        out_str(selftest_state_name(r.state[i]));
        out_char('\n');
    }
    for (uint32_t i = 0; i < ICSP_LINES; i++) {
        for (uint32_t j = i + 1u; j < ICSP_LINES; j++) {
            if (r.shorted[i][j]) {
                out_str("SHORT ");
                out_str(selftest_line_name((icsp_line_t)i));
                out_char(' ');
                out_str(selftest_line_name((icsp_line_t)j));
                out_char('\n');
            }
        }
    }
    ui_note(r.fault ? "SELFTEST fault" : "SELFTEST ok", r.fault ? "check the wiring" : "");
    out_str(r.fault ? "ERR wiring\n" : "OK\n");
}

static void reply_power(void)
{
    uint32_t vdd;

    out_str("OK ");
    out_str(target_power_on() ? "on " : "off ");
    out_int(target_vdd_mv(&vdd) ? (int32_t)vdd : -1); /* -1: the ADC failed */
    out_char('\n');
}

/*
 * POWER | POWER ON | POWER OFF
 * Report or switch the target supply (target_power.h); the reply gives the state and the
 * target VDD in millivolts.
 */
static void do_power(int argc, char *argv[ARG_MAX])
{
    if (argc == 2 && streq(argv[1], "ON")) {
        target_power_set(1);
        device = NULL; /* a part that was off must be probed again */
    } else if (argc == 2 && streq(argv[1], "OFF")) {
        target_power_set(0);
        device = NULL;
    } else if (argc != 1) {
        reply_error("bad-argument");
        return;
    }
    ui_note(target_power_on() ? "POWER on" : "POWER off", "");
    reply_power();
}

/* VDD: target VDD and the programmer's own VDDA, in millivolts; ERR adc if the ADC failed. */
static void do_vdd(void)
{
    uint32_t vdd;
    uint32_t vdda;

    if (!target_vdd_mv(&vdd) || !adc_vdda_mv(&vdda)) {
        reply_error("adc");
        return;
    }
    out_str("OK ");
    out_int((int32_t)vdd);
    out_char(' ');
    out_int((int32_t)vdda);
    out_char('\n');
}

/* STATUS: self-reported hardware faults from start-up. */
static void do_status(void)
{
    out_str(clock_status() == CLOCK_OK ? "OK clock=pll adc=" : "OK clock=hsi16 adc=");
    out_str(adc_ready() ? "ok\n" : "fail\n");
}

/*
 * BRIDGE <baud>
 * Join the PC to the target's serial port (bridge.h) until the PC sends BRIDGE_EXIT.
 *     OK bridge                    then raw bytes both ways
 *     OK bridge closed <dropped>   after the exit sequence
 */
static void do_bridge(char *argv[ARG_MAX])
{
    uint32_t baud;

    if (!need_power()) {
        return; /* its TX line idles high, which would back-power an unpowered target */
    }
    if (!parse_number(argv[1], &baud) || !target_uart_open(baud)) {
        reply_error("bad-argument");
        return;
    }
    ui_note("BRIDGE open", argv[1]);
    out_str("OK bridge\n");
    uint32_t dropped = bridge_run();
    target_uart_close();
    ui_note("BRIDGE closed", "");
    out_str("\r\nOK bridge closed ");
    out_int((int32_t)dropped);
    out_char('\n');
}

/*
 * ID | DCI
 * Read the device and revision IDs, or the Device Configuration Information, from the
 * probed device. pic_enter() checks the ID first, so a different part answers ERR -6.
 */
static void do_info(int dci)
{
    const char *name = dci ? "DCI" : "ID";
    uint16_t id = 0u;
    uint16_t rev = 0u;
    pic_dci_t info;

    if (!need_device()) {
        return;
    }
    pic_status_t st = pic_enter(device);
    if (st == PIC_OK) {
        st = dci ? pic_read_dci(device, &info) : pic_read_device_id(device, &id);
        if (st == PIC_OK && !dci) {
            st = pic_read_revision_id(device, &rev);
        }
        pic_exit(device);
    }
    ui_result(name, st, 0u);
    if (st != PIC_OK) {
        reply_status(st);
        return;
    }
    out_str("OK ");
    if (dci) {
        /* ERSIZ WLSIZ URSIZ EESIZ PCNT, in decimal, in the order the part stores them */
        const uint16_t fields[] = {info.erase_words, info.latch_words, info.user_rows,
                                   info.eeprom_bytes, info.pin_count};
        for (size_t i = 0; i < sizeof fields / sizeof fields[0]; i++) {
            if (i != 0u) {
                out_char(' ');
            }
            out_int(fields[i]);
        }
    } else {
        out_hex(id, 4u);
        out_char(' ');
        out_hex(rev, 4u);
    }
    out_char('\n');
}

/*
 * ERASE ALL | ERASE <region> | ERASE ROW <region> <offset>
 * Bulk erase of every region the device has, bulk erase of one region, or erase of the
 * row (page on PIC18) holding offset.
 */
static void do_erase(int argc, char *argv[ARG_MAX])
{
    pic_region_t region = PIC_REGION_FLASH;
    uint32_t offset = 0u;
    int row = 0;
    uint8_t mask = 0u;

    if (!need_device()) {
        return;
    }
    if (argc == 2 && streq(argv[1], "ALL")) {
        mask = PIC_ERASE_ALL;
    } else if (argc == 2 && parse_region(argv[1], &region)) {
        static const uint8_t masks[] = {PIC_ERASE_FLASH, PIC_ERASE_USERID, PIC_ERASE_CONFIG,
                                        PIC_ERASE_EEPROM};
        mask = masks[region];
    } else if (argc == 4 && streq(argv[1], "ROW") && parse_region(argv[2], &region) &&
               parse_number(argv[3], &offset)) {
        row = 1;
        if (!range_ok(region, offset, 1u)) {
            return;
        }
    } else {
        reply_error("bad-argument");
        return;
    }

    pic_status_t st = pic_enter(device);
    if (st == PIC_OK) {
        st = row ? pic_row_erase(device, region, offset) : pic_bulk_erase(device, mask);
        pic_exit(device);
    }
    ui_result("ERASE", st, 0u);
    if (st != PIC_OK) {
        reply_status(st);
        return;
    }
    out_str("OK\n");
}

/* Cells the next data line must carry: up to CMD_DATA_LINE_CELLS, and never past the end
 * of the transfer or of the row being buffered. */
static uint32_t requested_cells(void)
{
    uint32_t n = session.count - session.received;

    if (n > CMD_DATA_LINE_CELLS) {
        n = CMD_DATA_LINE_CELLS;
    }
    if (n > session.flush_at - session.buffered) {
        n = session.flush_at - session.buffered;
    }
    return n;
}

/* Ask the host for the next data line ("NEXT"), or for the last one again ("RESEND"). */
static void request(const char *word)
{
    out_str(word);
    out_char(' ');
    out_hex(session.offset + session.received, 8u);
    out_char(' ');
    out_int((int32_t)requested_cells());
    out_char('\n');
}

/* Release the target. The status screen shows the result unless the host aborted. */
static void end_session(pic_status_t st, uint32_t cells, int show)
{
    pic_exit(device);
    if (show) {
        ui_result(session.name, st, cells);
    }
    session.kind = SESSION_NONE;
}

/*
 * WRITE <region> <offset> <count> | VERIFY <region> <offset> <count>
 * Checks everything it can before touching the target, enters programming mode, then
 * asks for the first data line.
 */
static void do_session(char *argv[ARG_MAX], session_kind_t kind)
{
    pic_region_t region;
    uint32_t offset;
    uint32_t count;

    if (!need_device()) {
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
    if (!range_ok(region, offset, count)) {
        return;
    }

    uint32_t flush_at = CMD_DATA_LINE_CELLS;
    if (kind == SESSION_WRITE && region == PIC_REGION_FLASH && device->latch_words != 0u) {
        if (device->latch_words > WRITE_BUF_CELLS) {
            reply_status(PIC_ERR_UNSUPPORTED);
            return;
        }
        if (offset % device->latch_words != 0u) {
            reply_status(PIC_ERR_ALIGN);
            return;
        }
        flush_at = device->latch_words; /* one whole row per pic_write() */
    }

    const char *name = kind == SESSION_WRITE ? "WRITE" : "VERIFY";
    pic_status_t st = pic_enter(device);
    if (st != PIC_OK) {
        ui_result(name, st, 0u);
        reply_status(st);
        return;
    }
    session.kind = kind;
    session.name = name;
    session.region = region;
    session.offset = offset;
    session.count = count;
    session.received = 0u;
    session.buf_start = offset;
    session.buffered = 0u;
    session.flush_at = flush_at;
    request("NEXT");
}

/* Write or compare the buffered cells. Returns 0 and ends the session on failure. */
static int flush_session(void)
{
    pic_status_t st;
    size_t mismatch = 0u;

    if (session.kind == SESSION_WRITE) {
        st = pic_write(device, session.region, session.buf_start, session.buf, session.buffered);
    } else {
        st = pic_verify(device, session.region, session.buf_start, session.buf, session.buffered,
                        &mismatch);
    }
    if (st != PIC_OK) {
        end_session(st, session.buf_start - session.offset, 1);
        if (st == PIC_ERR_VERIFY) {
            /* Say where: the first cell that differs, as an offset in the region. */
            out_str("ERR ");
            out_int(st);
            out_char(' ');
            out_str(pic_strerror(st));
            out_str(" @");
            out_hex(session.buf_start + (uint32_t)mismatch, 8u);
            out_char('\n');
        } else {
            reply_status(st);
        }
        return 0;
    }
    session.buf_start += session.buffered;
    session.buffered = 0u;
    return 1;
}

/*
 * One line received while a WRITE or VERIFY is waiting for data. A good data line is
 * taken; a damaged one (bad CRC, wrong offset or cell count, receive error, line too long)
 * is asked for again with RESEND; anything else aborts the command and releases the
 * target.
 */
static void session_line(int damaged)
{
    char *argv[DATA_ARG_MAX];
    uint32_t n = requested_cells();
    uint32_t value;
    char *star = NULL;

    if (line_len == 0u && !damaged) {
        return; /* blank line, for example the second half of a stray CR LF */
    }
    if (!damaged && !(line[0] == 'D' && line[1] == ' ')) {
        end_session(PIC_OK, session.received, 0);
        reply_error("aborted");
        return;
    }
    for (size_t i = 0; i < line_len; i++) {
        if (line[i] == '*') {
            star = &line[i];
            break;
        }
    }
    /* The crc covers the text from the 'D' up to, not including, the " *". */
    int ok = !damaged && star != NULL && star > line && star[-1] == ' ' &&
             parse_hex_digits(star + 1, 4u, &value);
    if (ok) {
        uint16_t check = 0xFFFFu;
        for (char *c = line; c < star - 1; c++) {
            check = crc16_update(check, (uint8_t)*c);
        }
        ok = check == value;
    }
    int argc = ok ? split(line, argv, DATA_ARG_MAX) : -1;
    ok = ok && argc == (int)(n + 3u) && parse_hex_digits(argv[1], 8u, &value) &&
         value == session.offset + session.received;
    for (uint32_t i = 0; ok && i < n; i++) {
        ok = parse_hex_digits(argv[2u + i], 4u, &value);
        session.buf[session.buffered + i] = (uint16_t)value;
    }
    if (!ok) {
        request("RESEND");
        return;
    }

    platform_watchdog_kick();
    session.buffered += n;
    session.received += n;
    if ((session.buffered == session.flush_at || session.received == session.count) &&
        !flush_session()) {
        return;
    }
    if (session.received == session.count) {
        end_session(PIC_OK, session.count, 1);
        reply_ok_count(session.count);
        return;
    }
    request("NEXT");
}

static void execute(void)
{
    char *argv[ARG_MAX];
    int argc = split(line, argv, ARG_MAX);

    if (argc == 0) {
        return; /* blank line */
    }
    if (argc > 0 && streq(argv[0], "PROBE") && argc == 1) {
        do_probe();
    } else if (argc > 0 && streq(argv[0], "READ") && argc == 4) {
        do_read(argv, 0);
    } else if (argc > 0 && streq(argv[0], "READB") && argc == 4) {
        do_read(argv, 1);
    } else if (argc > 0 && streq(argv[0], "CRC") && (argc == 2 || argc == 4)) {
        do_scan(argc, argv, 0);
    } else if (argc > 0 && streq(argv[0], "BLANK") && (argc == 2 || argc == 4)) {
        do_scan(argc, argv, 1);
    } else if (argc > 0 && streq(argv[0], "POWER") && argc <= 2) {
        do_power(argc, argv);
    } else if (argc > 0 && streq(argv[0], "VDD") && argc == 1) {
        do_vdd();
    } else if (argc > 0 && streq(argv[0], "STATUS") && argc == 1) {
        do_status();
    } else if (argc > 0 && streq(argv[0], "BRIDGE") && argc == 2) {
        do_bridge(argv);
    } else if (argc > 0 && streq(argv[0], "SELFTEST") && argc == 1) {
        do_selftest();
    } else if (argc > 0 && streq(argv[0], "ID") && argc == 1) {
        do_info(0);
    } else if (argc > 0 && streq(argv[0], "DCI") && argc == 1) {
        do_info(1);
    } else if (argc > 0 && streq(argv[0], "ERASE") && (argc == 2 || argc == 4)) {
        do_erase(argc, argv);
    } else if (argc > 0 && streq(argv[0], "WRITE") && argc == 4) {
        do_session(argv, SESSION_WRITE);
    } else if (argc > 0 && streq(argv[0], "VERIFY") && argc == 4) {
        do_session(argv, SESSION_VERIFY);
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
    if (session.kind != SESSION_NONE) {
        session_line(line_too_long || line_rx_error);
    } else if (line_too_long) {
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
