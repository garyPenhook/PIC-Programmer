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

    @staticmethod
    def _ints(text, count, base=10):
        """`count` whitespace-separated integers from a reply, or LinkError if it is not that."""
        words = text.split()
        try:
            if len(words) != count:
                raise ValueError
            return [int(w, base) for w in words]
        except ValueError:
            raise LinkError(f"unexpected reply: {text!r}") from None

    def command(self, text):
        """Send a command that answers with a single OK or ERR line; return the OK text."""
        self._send(text)
        return self._check(self._read_line())

    # -- commands --------------------------------------------------------------------------

    def probe(self):
        """(name, device id, revision id) of the part found."""
        text = self.command("PROBE")
        name = text.split(" ", 1)[0]
        device_id, revision = self._ints(text[len(name):], 2, 16)
        return name, device_id, revision

    def ids(self):
        device_id, revision = self._ints(self.command("ID"), 2, 16)
        return device_id, revision

    def dci(self):
        keys = ("erase_words", "latch_words", "user_rows", "eeprom_bytes", "pin_count")
        return dict(zip(keys, self._ints(self.command("DCI"), len(keys))))

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
        return self._ints(self._span("CRC", region, offset, count), 1, 16)[0]

    def blank(self, region, offset=None, count=None):
        """Number of cells checked; raises LinkError ("not-blank @<offset> <value>") if one
        is not erased."""
        return self._ints(self._span("BLANK", region, offset, count), 1)[0]

    def power(self, on=None):
        """Switch the target supply (True/False) or just report it. Returns (on, VDD in mV),
        with VDD None if the programmer's ADC could not measure it."""
        verb = "POWER" if on is None else ("POWER ON" if on else "POWER OFF")
        text = self.command(verb)
        state, _, vdd = text.partition(" ")
        if state not in ("on", "off"):
            raise LinkError(f"unexpected reply: {text!r}")
        vdd = self._ints(vdd, 1)[0]
        return state == "on", (vdd if vdd >= 0 else None)

    def status(self):
        """Faults the programmer reports: {"clock": "pll" | "hsi16", "adc": "ok" | "fail",
        "reset": "normal" | "watchdog"} (reset is missing from older firmware)."""
        text = self.command("STATUS")
        try:
            fields = dict(word.split("=", 1) for word in text.split())
        except ValueError:
            fields = {}
        if not {"clock", "adc"} <= set(fields):
            raise LinkError(f"unexpected reply: {text!r}")
        return fields

    def vdd(self):
        """(target VDD, programmer VDDA) in millivolts."""
        target, own = self._ints(self.command("VDD"), 2)
        return target, own

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
        return before, self._ints(after, 1)[0]

    def selftest(self):
        """Wiring check. Returns (states, shorts, ok): states maps "CLK", "DAT", "MCLR" to
        "float", "high" or "low"; shorts lists (line, line) pairs."""
        self._send("SELFTEST")
        states, shorts = {}, []
        while True:
            line = self._read_line()
            word, _, rest = line.partition(" ")
            if word == "LINE":
                fields = rest.split()
                if len(fields) != 2:
                    raise LinkError(f"unexpected reply: {line!r}")
                states[fields[0]] = fields[1]
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
        try:
            result = self._upload_lines(offset, cells, progress)
        except KeyboardInterrupt:
            self._abort_session()
            raise
        if progress:
            progress(len(cells), len(cells))
        return result

    def _upload_lines(self, offset, cells, progress):
        while True:
            line = self._read_line()
            word, _, rest = line.partition(" ")
            if word not in ("NEXT", "RESEND"):
                return self._check(line)
            fields = rest.split()
            if len(fields) != 2:
                raise LinkError(f"unexpected reply: {line!r}")
            at, n = self._ints(fields[0], 1, 16)[0], self._ints(fields[1], 1)[0]
            if not (offset <= at and at + n <= offset + len(cells) and 0 < n <= DATA_LINE_CELLS):
                raise LinkError(f"programmer asked for {n} cells at 0x{at:X}, outside the data")
            body = f"D {at:08X} " + " ".join(f"{c:04X}" for c in cells[at - offset:at - offset + n])
            self._send(f"{body} *{crc16(body.encode('ascii')):04X}")
            if progress and word == "NEXT":
                progress(at - offset, len(cells))

    def _abort_session(self):
        """After Ctrl-C mid-transfer: end the programmer's WRITE/VERIFY and drain its replies,
        so the next command does not read this one's leftovers. A blank line finishes a
        half-sent data line; ABORT is not a data line, so the programmer releases the target
        and answers "ERR aborted" (or "ERR bad-command" if the transfer had just finished).
        Stale NEXT/RESEND/OK lines come first and are skipped."""
        try:
            self.stream.write(b"\nABORT\n")
            if hasattr(self.stream, "flush"):
                self.stream.flush()
            while not self._read_line().startswith("ERR"):
                pass
        except (LinkError, OSError):
            pass

    def write(self, region, offset, cells, progress=None):
        self._upload("WRITE", region, offset, cells, progress)

    def verify(self, region, offset, cells, progress=None):
        """Raise LinkError (code -8, text ending in "@<offset>") on the first difference."""
        self._upload("VERIFY", region, offset, cells, progress)
