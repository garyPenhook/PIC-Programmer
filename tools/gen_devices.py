#!/usr/bin/env python3
"""Turn data/pic_devices.csv into the C device table (pic_devices.c).

Usage: gen_devices.py <input.csv> <output.c>

Rows are checked before anything is written, so a bad table fails the build instead of
producing firmware with a wrong geometry.
"""
import csv
import sys

# Per protocol: the most cells each region can hold before it runs into the next area of
# that protocol's address map (see the ADDR_* constants in src/proto/<protocol>.c), and
# whether flash is written through row latches. src/pic.c bounds every access by the row,
# so these limits are what keep a bad row from addressing another region.
PROTOCOLS = {
    # Word addresses: flash below 0x8000, User IDs 0x8000-0x8003 (0x8005 is the revision
    # ID), Configuration Words from 0x8007 below the DIA at 0x8100, EEPROM 0xF000-0xFFFF.
    "p16_8bit": {"latches": True, "flash_words": 0x8000, "userid_words": 4,
                 "config_count": 0x8100 - 0x8007, "eeprom_bytes": 0x1000},
    # Byte addresses: flash words below 0x200000, User ID words from 0x200000, config
    # bytes from 0x300000, EEPROM bytes from 0x380000, DCI at 0x3C0000.
    "p18_8bit": {"latches": False, "flash_words": 0x100000, "userid_words": 0x80000,
                 "config_count": 0x80000, "eeprom_bytes": 0x40000},
}
COLUMNS = ("name", "protocol", "device_id", "flash_words", "erase_words", "latch_words",
           "userid_words", "config_count", "eeprom_bytes", "source")
U16 = ("erase_words", "latch_words", "userid_words", "config_count", "eeprom_bytes")


def fail(path, line, msg):
    sys.exit(f"{path}:{line}: {msg}")


def records(path):
    """(line number, fields) for each data row, skipping blank lines and # comments."""
    with open(path, newline="") as f:
        reader = csv.reader(f)
        for fields in reader:
            if not fields or not "".join(fields).strip() or fields[0].lstrip().startswith("#"):
                continue
            yield reader.line_num, fields


def load(path):
    recs = records(path)
    line, header = next(recs, (1, None))
    if tuple(header or ()) != COLUMNS:
        fail(path, line, f"header must be {','.join(COLUMNS)}")

    rows, seen_ids, seen_names = [], set(), set()
    for line, fields in recs:
        if len(fields) != len(COLUMNS):
            fail(path, line, f"expected {len(COLUMNS)} fields, got {len(fields)}")
        row = dict(zip(COLUMNS, (x.strip() for x in fields)))
        try:
            row["device_id"] = int(row["device_id"], 16)
            for key in ("flash_words",) + U16:
                row[key] = int(row[key], 10)
        except ValueError as e:
            fail(path, line, str(e))

        proto = PROTOCOLS.get(row["protocol"])
        if proto is None:
            fail(path, line, f"unknown protocol {row['protocol']!r}")
        if not row["source"]:
            fail(path, line, "every row needs a source document")
        for key in ("name", "source"):
            if any(bad in row[key] for bad in ('"', "\\", "*/", "\n")):
                fail(path, line, f"{key} must not contain quotes, backslashes, newlines or */")
        if not 0 <= row["device_id"] <= 0xFFFF:
            fail(path, line, "device_id must fit in 16 bits")
        for key in U16:
            if not 0 <= row[key] <= 0xFFFF:
                fail(path, line, f"{key} must fit in 16 bits")
        for key in ("flash_words", "userid_words", "config_count", "eeprom_bytes"):
            if row[key] > proto[key]:
                fail(path, line, f"{key} {row[key]} is more than {row['protocol']} can address ({proto[key]})")
        if row["flash_words"] <= 0:
            fail(path, line, "flash_words must be positive")
        if row["erase_words"] == 0 or row["flash_words"] % row["erase_words"]:
            fail(path, line, "flash_words must be a whole number of erase rows")
        if proto["latches"] and row["latch_words"] == 0:
            fail(path, line, f"{row['protocol']} writes flash through row latches; latch_words must be > 0")
        if not proto["latches"] and row["latch_words"] != 0:
            fail(path, line, f"{row['protocol']} writes one word at a time; latch_words must be 0")
        if row["latch_words"] and row["flash_words"] % row["latch_words"]:
            fail(path, line, "flash_words must be a whole number of write rows")
        key = (row["protocol"], row["device_id"])
        if key in seen_ids:
            fail(path, line, f"duplicate device ID 0x{row['device_id']:04X} for {row['protocol']}")
        if row["name"] in seen_names:
            fail(path, line, f"duplicate name {row['name']}")
        seen_ids.add(key)
        seen_names.add(row["name"])
        rows.append(row)
    if not rows:
        fail(path, 1, "no devices")
    return rows


def emit(rows, out):
    with open(out, "w") as f:
        f.write("/* Generated from data/pic_devices.csv by tools/gen_devices.py. Do not edit. */\n")
        f.write('#include "pic_proto.h"\n\nconst pic_device_t pic_devices[] = {\n')
        for r in rows:
            f.write(f"    /* {r['source']} */\n")
            f.write(f"    {{ .name = \"{r['name']}\", .proto = &pic_proto_{r['protocol']}, "
                    f".device_id = 0x{r['device_id']:04X}u,\n"
                    f"      .flash_words = {r['flash_words']}u, .erase_words = {r['erase_words']}u, "
                    f".latch_words = {r['latch_words']}u, .userid_words = {r['userid_words']}u,\n"
                    f"      .config_count = {r['config_count']}u, .eeprom_bytes = {r['eeprom_bytes']}u }},\n")
        f.write("};\n\nconst size_t pic_device_count = sizeof pic_devices / sizeof pic_devices[0];\n")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    emit(load(sys.argv[1]), sys.argv[2])
