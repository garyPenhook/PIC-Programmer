"""The debug agent's wire protocol (target/pdbg/pdbg.h), on any byte stream.

A stream needs read(n) -> bytes (empty on timeout) and write(bytes); a pyserial Serial on the
programmer's bridge, or the pipes of the PC build of the agent used in the tests, both work.

Frames are payload + CRC-16 (low byte first), COBS-encoded, then a 0x00. The agent answers
every request with a reply frame carrying the request's sequence number; it also sends halt
events and console text whenever it likes, so those are collected on the side while a reply
is awaited.
"""
from collections import deque
from dataclasses import dataclass

from ..link import BRIDGE_EXIT, crc16

CH_REPLY = 1
CH_CONSOLE = 2
CH_EVENT = 3

EV_HALTED = 1

REASONS = {1: "request", 2: "break", 3: "assert"}

CMD_HELLO = 1
CMD_HALT = 2
CMD_CONT = 3
CMD_REGS = 4
CMD_STACK = 5
CMD_RD = 6
CMD_WR = 7
CMD_RDPFM = 8
CMD_BP = 9
CMD_RESET = 10

ERR_CMD = 1
ERR_ARG = 2
ERR_STATE = 3
STATUS_TEXT = {ERR_CMD: "unknown command", ERR_ARG: "bad argument", ERR_STATE: "not halted"}

HALT_REQUEST = b"\x00\x00"  # an empty frame; two, so a half-received frame is flushed first


class PdbgError(Exception):
    """The agent refused a request (code = its status byte), or the reply made no sense."""

    def __init__(self, text, code=None):
        super().__init__(text)
        self.code = code


class BridgeExitError(PdbgError):
    """The request's bytes would contain the bridge's exit sequence, whatever the sequence number."""


class PdbgTimeout(PdbgError):
    """Nothing (or nothing valid) arrived from the agent in time."""


def cobs_encode(data):
    """COBS: the output has no 0x00 byte. The delimiter is not added."""
    out = bytearray()
    block = bytearray()
    for byte in data:
        if byte == 0:
            out.append(len(block) + 1)
            out += block
            block.clear()
        else:
            block.append(byte)
            if len(block) == 254:
                out.append(0xFF)
                out += block
                block.clear()
    out.append(len(block) + 1)
    out += block
    return bytes(out)


def cobs_decode(data):
    """Inverse of cobs_encode; ValueError if `data` is not valid COBS."""
    out = bytearray()
    i = 0
    while i < len(data):
        code = data[i]
        i += 1
        if code == 0 or i + code - 1 > len(data):
            raise ValueError("bad COBS block")
        out += data[i:i + code - 1]
        i += code - 1
        if code != 0xFF and i < len(data):
            out.append(0)
    return bytes(out)


def encode_frame(payload):
    """payload -> the bytes on the wire, delimiter included."""
    crc = crc16(payload)
    return cobs_encode(bytes(payload) + bytes([crc & 0xFF, crc >> 8])) + b"\x00"


class FrameReader:
    """Cuts a byte stream into frames; a frame that fails COBS or CRC is counted and dropped."""

    def __init__(self):
        self.buffer = bytearray()
        self.bad = 0

    def feed(self, data):
        frames = []
        for byte in data:
            if byte != 0:
                self.buffer.append(byte)
                continue
            raw = bytes(self.buffer)
            self.buffer.clear()
            if not raw:
                continue
            try:
                decoded = cobs_decode(raw)
            except ValueError:
                self.bad += 1
                continue
            if len(decoded) < 3 or crc16(decoded[:-2]) != decoded[-2] | decoded[-1] << 8:
                self.bad += 1
                continue
            frames.append(decoded[:-2])
        return frames


@dataclass
class Hello:
    version: int
    max_data: int
    bp_sites: int
    device_id: int
    revision: int
    reset_cause: int
    halted: bool


@dataclass
class Halted:
    reason: int
    arg: int  # the site number, for a breakpoint
    pc: int

    @property
    def reason_text(self):
        return REASONS.get(self.reason, f"reason {self.reason}")


@dataclass
class Regs:
    pc: int
    wreg: int
    status: int
    bsr: int
    fsr0: int
    fsr1: int
    fsr2: int
    prod: int
    tblptr: int
    tablat: int
    stkptr: int


def _le(data):
    return int.from_bytes(data, "little")


class Pdbg:
    def __init__(self, stream, max_data=32, retries=2):
        self.stream = stream
        self.max_data = max_data  # bytes per RD, WR and RDPFM frame; HELLO sets it
        self.retries = retries
        self.reader = FrameReader()
        self.seq = 0
        self.replies = {}  # seq -> (cmd, status, data)
        self.events = deque()  # Halted, oldest first
        self.console = bytearray()  # text from the application not yet taken

    @property
    def bad_frames(self):
        return self.reader.bad

    # -- low level -------------------------------------------------------------------------

    def _write(self, data):
        self.stream.write(data)
        if hasattr(self.stream, "flush"):
            self.stream.flush()

    def _pump(self):
        """Read once and file what arrived. False if the read timed out."""
        chunk = self.stream.read(64)
        if not chunk:
            return False
        for frame in self.reader.feed(chunk):
            channel, body = frame[0], frame[1:]
            if channel == CH_REPLY and len(body) >= 3:
                self.replies[body[0]] = (body[1], body[2], body[3:])
            elif channel == CH_EVENT and len(body) >= 6 and body[0] == EV_HALTED:
                self.events.append(Halted(body[1], body[2], _le(body[3:6])))
            elif channel == CH_CONSOLE:
                self.console += body
        return True

    def _encode_request(self, cmd, args):
        """The bytes for a request. The link's bridge closes when it sees BRIDGE_EXIT in the
        PC's bytes, so a frame holding it is re-encoded under another sequence number; if the
        arguments alone make it, that is an error."""
        for _ in range(4):
            self.seq = (self.seq + 1) & 0xFF
            wire = encode_frame(bytes([cmd, self.seq]) + bytes(args))
            if BRIDGE_EXIT not in wire:
                return self.seq, wire
        raise BridgeExitError("request would close the bridge: its bytes contain the exit "
                              "sequence")

    def request(self, cmd, args=b"", retries=None):
        """Send a request and return the reply data. A request that gets no answer is sent
        again `retries` times (a lost byte on either wire drops the frame): leave it at 0 for
        commands that are not safe to run twice."""
        seq, wire = self._encode_request(cmd, args)
        self.replies.clear()  # leftovers of earlier requests (a retried one answers twice)
        for _ in range((self.retries if retries is None else retries) + 1):
            self._write(wire)
            while seq not in self.replies:
                if not self._pump():
                    break
            if seq in self.replies:
                _, status, data = self.replies.pop(seq)
                if status != 0:
                    raise PdbgError(STATUS_TEXT.get(status, f"status {status}"), status)
                return data
        raise PdbgTimeout(f"no answer from the agent to command {cmd}")

    # -- commands --------------------------------------------------------------------------

    def hello(self):
        data = self.request(CMD_HELLO)
        if len(data) != 9:
            raise PdbgError("HELLO reply has the wrong length")
        info = Hello(data[0], data[1], data[2], _le(data[3:5]), _le(data[5:7]), data[7],
                     bool(data[8]))
        self.max_data = info.max_data
        return info

    def halt(self):
        """Ask the agent to stop. It answers with a halt event: see wait_halt()."""
        self.events.clear()
        self._write(HALT_REQUEST)

    def wait_halt(self):
        """The next halt event (a breakpoint, or the answer to halt()); PdbgTimeout if none
        comes before a read times out."""
        while not self.events:
            if not self._pump():
                raise PdbgTimeout("the agent did not halt")
        return self.events.popleft()

    def cont(self):
        self.request(CMD_CONT, retries=0)
        self.events.clear()  # the duplicate announcements of the halt just ended

    def regs(self):
        data = self.request(CMD_REGS)
        if len(data) != 19:
            raise PdbgError("REGS reply has the wrong length")
        return Regs(_le(data[0:3]), data[3], data[4], data[5], _le(data[6:8]), _le(data[8:10]),
                    _le(data[10:12]), _le(data[12:14]), _le(data[14:17]), data[17], data[18])

    def stack(self):
        """Return addresses, innermost first, however many replies that takes."""
        addresses = []
        while True:
            data = self.request(CMD_STACK, bytes([len(addresses)]))
            depth, count = data[0], data[1]
            addresses += [_le(data[2 + 3 * i:5 + 3 * i]) for i in range(count)]
            if len(addresses) >= depth or count == 0:
                return addresses

    def read(self, addr, count):
        out = bytearray()
        while len(out) < count:
            n = min(count - len(out), self.max_data)
            out += self.request(CMD_RD, (addr + len(out)).to_bytes(2, "little") + bytes([n]))
        return bytes(out)

    def write(self, addr, data):
        """Write data memory (halted only). A chunk whose frame would contain the bridge's
        exit sequence is split in two."""
        data = bytes(data)
        for start in range(0, len(data), self.max_data):
            self._write_chunk(addr + start, data[start:start + self.max_data])

    def _write_chunk(self, addr, chunk):
        try:
            self.request(CMD_WR, addr.to_bytes(2, "little") + chunk)
        except BridgeExitError:
            if len(chunk) < 2:
                raise
            half = len(chunk) // 2
            self._write_chunk(addr, chunk[:half])
            self._write_chunk(addr + half, chunk[half:])

    def read_flash(self, addr, count):
        out = bytearray()
        while len(out) < count:
            n = min(count - len(out), self.max_data)
            out += self.request(CMD_RDPFM, (addr + len(out)).to_bytes(3, "little") + bytes([n]))
        return bytes(out)

    def breakpoints(self):
        """The enabled sites as a set of site numbers."""
        return self._sites(self.request(CMD_BP))

    def set_breakpoint(self, site, on=True):
        return self._sites(self.request(CMD_BP, bytes([site, 1 if on else 0])))

    @staticmethod
    def _sites(data):
        mask = _le(data)
        return {n for n in range(8 * len(data)) if mask >> n & 1}

    def reset(self):
        """The agent answers, then resets the part."""
        self.request(CMD_RESET, retries=0)

    def take_console(self):
        """Application text received so far (and not yet taken)."""
        text = bytes(self.console)
        self.console.clear()
        return text
