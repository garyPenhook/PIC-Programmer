"""The programmer's serial command protocol (include/cmd.h), on any byte stream.

A stream needs read(n) -> bytes and write(bytes); a pyserial Serial, or the pipes of the
PC build of the firmware used in the tests, both work.
"""
import struct

FRAME_MARKER = 0x42
BRIDGE_EXIT = b"\x1b[picprog-bridge-exit]"  # BRIDGE_EXIT in include/bridge.h
DATA_LINE_CELLS = 8  # CMD_DATA_LINE_CELLS


class LinkError(Exception):
    """The programmer reported an error, or its reply could not be understood."""

    def __init__(self, text, code=None):
        super().__init__(text)
        self.code = code  # the pic_status_t value for "ERR <code> <text>" replies


def crc16(data):
    """CRC-16/CCITT-FALSE (polynomial 0x1021, initial value 0xFFFF), as in src/cmd.c."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


class Link:
    def __init__(self, stream):
        self.stream = stream

    # -- low level -------------------------------------------------------------------------

    def _read_exact(self, n):
        data = b""
        while len(data) < n:
            chunk = self.stream.read(n - len(data))
            if not chunk:
                raise LinkError("timeout waiting for the programmer")
            data += chunk
        return data

    def _read_line(self, first=b""):
        line = first
        while not line.endswith(b"\n"):
            line += self._read_exact(1)
        return line[:-1].decode("ascii", errors="replace").rstrip("\r")

    def _send(self, text):
        self.stream.write(text.encode("ascii") + b"\n")
        if hasattr(self.stream, "flush"):
            self.stream.flush()

    @staticmethod
    def _check(line):
        """Return the text after "OK", or raise for an ERR line."""
        if line == "OK" or line.startswith("OK "):
            return line[3:]
        if line.startswith("ERR "):
            rest = line[4:]
            word, _, text = rest.partition(" ")
            try:
                code = int(word)
            except ValueError:
                raise LinkError(rest) from None
            raise LinkError(text or rest, code)
        raise LinkError(f"unexpected reply: {line!r}")

    def command(self, text):
        """Send a command that answers with a single OK or ERR line; return the OK text."""
        self._send(text)
        return self._check(self._read_line())

    # -- commands --------------------------------------------------------------------------

    def probe(self):
        """(name, device id, revision id) of the part found."""
        name, device_id, revision = self.command("PROBE").split()
        return name, int(device_id, 16), int(revision, 16)

    def ids(self):
        device_id, revision = self.command("ID").split()
        return int(device_id, 16), int(revision, 16)

    def dci(self):
        keys = ("erase_words", "latch_words", "user_rows", "eeprom_bytes", "pin_count")
        return dict(zip(keys, (int(v) for v in self.command("DCI").split())))

    def erase(self, region="ALL"):
        self.command(f"ERASE {region}")

    def erase_row(self, region, offset):
        self.command(f"ERASE ROW {region} {offset}")

    def _span(self, verb, region, offset, count):
        span = f" {offset} {count}" if offset is not None else ""
        return self.command(f"{verb} {region}{span}")

    def crc(self, region, offset=None, count=None):
        """CRC-32 the programmer computes over the cells (each as two bytes, low first);
        the whole region unless offset and count are given."""
        return int(self._span("CRC", region, offset, count), 16)

    def blank(self, region, offset=None, count=None):
        """Number of cells checked; raises LinkError ("not-blank @<offset> <value>") if one
        is not erased."""
        return int(self._span("BLANK", region, offset, count))

    def power(self, on=None):
        """Switch the target supply (True/False) or just report it. Returns (on, VDD in mV)."""
        verb = "POWER" if on is None else ("POWER ON" if on else "POWER OFF")
        state, vdd = self.command(verb).split()
        return state == "on", int(vdd)

    def vdd(self):
        """(target VDD, programmer VDDA) in millivolts."""
        target, own = self.command("VDD").split()
        return int(target), int(own)

    def bridge_open(self, baud):
        """Start BRIDGE: from here on, bytes written go to the target's serial port."""
        reply = self.command(f"BRIDGE {baud}")
        if reply != "bridge":
            raise LinkError(f"unexpected reply to BRIDGE: {reply!r}")

    def bridge_close(self):
        """Send the exit sequence; returns (bytes the target sent meanwhile, bytes dropped)."""
        self.stream.write(BRIDGE_EXIT)
        if hasattr(self.stream, "flush"):
            self.stream.flush()
        tail = b""
        marker = b"\r\nOK bridge closed "
        while not (marker in tail and tail.endswith(b"\n")):
            tail += self._read_exact(1)
        before, _, after = tail.rpartition(marker)
        return before, int(after.strip())

    def selftest(self):
        """Wiring check. Returns (states, shorts, ok): states maps "CLK", "DAT", "MCLR" to
        "float", "high" or "low"; shorts lists (line, line) pairs."""
        self._send("SELFTEST")
        states, shorts = {}, []
        while True:
            line = self._read_line()
            word, _, rest = line.partition(" ")
            if word == "LINE":
                name, state = rest.split()
                states[name] = state
            elif word == "SHORT":
                shorts.append(tuple(rest.split()))
            elif line == "ERR wiring":
                return states, shorts, False
            else:
                self._check(line)
                return states, shorts, True

    def read(self, region, offset, count, progress=None):
        """`count` cells of `region` from `offset`, using the binary READB frames."""
        self._send(f"READB {region} {offset} {count}")
        cells = []
        while True:
            first = self._read_exact(1)
            if first[0] != FRAME_MARKER:
                self._check(self._read_line(first))
                break
            header = self._read_exact(5)
            frame_offset, n = struct.unpack("<IB", header)
            body = self._read_exact(2 * n)
            (crc,) = struct.unpack("<H", self._read_exact(2))
            if crc16(first + header + body) != crc:
                raise LinkError(f"bad crc on the data frame at offset 0x{frame_offset:X}")
            if frame_offset != offset + len(cells):
                raise LinkError(f"data frame at 0x{frame_offset:X}, expected "
                                f"0x{offset + len(cells):X}")
            cells.extend(struct.unpack(f"<{n}H", body))
            if progress:
                progress(len(cells), count)
        if len(cells) != count:
            raise LinkError(f"got {len(cells)} cells, expected {count}")
        return cells

    def _upload(self, verb, region, offset, cells, progress=None):
        self._send(f"{verb} {region} {offset} {len(cells)}")
        while True:
            line = self._read_line()
            word, _, rest = line.partition(" ")
            if word not in ("NEXT", "RESEND"):
                result = self._check(line)
                if progress:
                    progress(len(cells), len(cells))
                return result
            at, n = rest.split()
            at, n = int(at, 16), int(n)
            if not (offset <= at and at + n <= offset + len(cells) and 0 < n <= DATA_LINE_CELLS):
                raise LinkError(f"programmer asked for {n} cells at 0x{at:X}, outside the data")
            body = f"D {at:08X} " + " ".join(f"{c:04X}" for c in cells[at - offset:at - offset + n])
            self._send(f"{body} *{crc16(body.encode('ascii')):04X}")
            if progress and word == "NEXT":
                progress(at - offset, len(cells))

    def write(self, region, offset, cells, progress=None):
        self._upload("WRITE", region, offset, cells, progress)

    def verify(self, region, offset, cells, progress=None):
        """Raise LinkError (code -8, text ending in "@<offset>") on the first difference."""
        self._upload("VERIFY", region, offset, cells, progress)
