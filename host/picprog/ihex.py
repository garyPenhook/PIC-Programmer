"""Intel HEX (INHX32) reading and writing.

A hex image is a dict {byte address: byte value}. Records supported: 00 data, 01 end of
file, 04 extended linear address. Type 02 (extended segment address) is also accepted on
input. Every record's checksum is checked.
"""


class HexError(ValueError):
    pass


def parse(text):
    """Return {address: byte} for the records in `text`."""
    image = {}
    upper = 0
    ended = False
    for number, raw in enumerate(text.splitlines(), start=1):
        line = raw.strip()
        if not line:
            continue
        if ended:
            raise HexError(f"line {number}: data after the end-of-file record")
        if line[0] != ":":
            raise HexError(f"line {number}: record does not start with ':'")
        try:
            record = bytes.fromhex(line[1:])
        except ValueError as e:
            raise HexError(f"line {number}: {e}") from None
        if len(record) < 5 or len(record) != record[0] + 5:
            raise HexError(f"line {number}: record length does not match its byte count")
        if sum(record) & 0xFF:
            raise HexError(f"line {number}: bad checksum")
        count, offset, kind = record[0], (record[1] << 8) | record[2], record[3]
        data = record[4:4 + count]
        if kind == 0x00:
            for i, byte in enumerate(data):
                address = upper + offset + i
                if address in image and image[address] != byte:
                    raise HexError(f"line {number}: address 0x{address:X} given twice")
                image[address] = byte
        elif kind == 0x01:
            ended = True
        elif kind == 0x04 and count == 2:
            upper = ((data[0] << 8) | data[1]) << 16
        elif kind == 0x02 and count == 2:
            upper = ((data[0] << 8) | data[1]) << 4
        elif kind in (0x03, 0x05):
            pass  # start address records: irrelevant to a programmer
        else:
            raise HexError(f"line {number}: unsupported record type {kind:02X}")
    if not ended:
        raise HexError("no end-of-file record")
    return image


def _record(kind, offset, data):
    body = bytes([len(data), (offset >> 8) & 0xFF, offset & 0xFF, kind]) + bytes(data)
    checksum = (-sum(body)) & 0xFF
    return ":" + (body + bytes([checksum])).hex().upper()


def to_text(image, row=16):
    """Return INHX32 text for `image`, `row` data bytes per record at most."""
    lines = []
    upper = None
    addresses = sorted(image)
    i = 0
    while i < len(addresses):
        start = addresses[i]
        if start >> 16 != upper:
            upper = start >> 16
            lines.append(_record(0x04, 0, [(upper >> 8) & 0xFF, upper & 0xFF]))
        chunk = [image[start]]
        i += 1
        # Extend while contiguous, inside the same 64 KiB page and the record size.
        while (i < len(addresses) and addresses[i] == start + len(chunk) and len(chunk) < row
               and addresses[i] >> 16 == upper):
            chunk.append(image[addresses[i]])
            i += 1
        lines.append(_record(0x00, start & 0xFFFF, chunk))
    lines.append(_record(0x01, 0, []))
    return "\n".join(lines) + "\n"
