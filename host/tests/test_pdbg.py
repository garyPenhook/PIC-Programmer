"""picprog.debug.pdbg against the PC build of the debug agent (target/pdbg) on a fake part.

The fake part: RAM byte i is i & 0xFF, return stack 0x001234 / 0x000456 / 0x000789, PC 0x1002
(tests/pdbg_sim_main.c).
"""
import pytest

from picprog.debug import pdbg
from picprog.debug.pdbg import Pdbg, PdbgError, PdbgTimeout
from picprog.link import BRIDGE_EXIT, crc16


# -- framing, without the agent ----------------------------------------------------------

@pytest.mark.parametrize("data", [b"", b"\x00", b"\x00\x00", b"abc", b"a\x00b", b"\x01" * 253,
                                  b"\x01" * 254, b"\x01" * 255, b"\x01" * 254 + b"\x00",
                                  bytes(range(256))])
def test_cobs_round_trips_and_has_no_zero(data):
    encoded = pdbg.cobs_encode(data)
    assert 0 not in encoded
    assert pdbg.cobs_decode(encoded) == data


def test_cobs_known_vectors():
    assert pdbg.cobs_encode(b"\x00") == b"\x01\x01"
    assert pdbg.cobs_encode(b"\x11\x22\x00\x33") == b"\x03\x11\x22\x02\x33"


@pytest.mark.parametrize("bad", [b"\x00", b"\x05\x01", b"\x02\x01\x00\x01"])
def test_cobs_rejects_bad_input(bad):
    with pytest.raises(ValueError):
        pdbg.cobs_decode(bad)


def test_crc_is_the_links():
    assert crc16(b"123456789") == 0x29B1


def test_frame_reader_drops_bad_frames_and_resynchronises():
    good = pdbg.encode_frame(b"\x01\x02\x03")
    flipped = bytearray(good)
    flipped[1] ^= 0x40
    reader = pdbg.FrameReader()
    frames = reader.feed(b"\x07junk\x00" + bytes(flipped) + good + b"\x00\x00")
    assert frames == [b"\x01\x02\x03"]
    assert reader.bad == 2


# -- commands against the agent ----------------------------------------------------------

def test_hello(agent):
    info = agent.hello()
    assert (info.version, info.max_data, info.bp_sites) == (1, 32, 32)
    assert (info.device_id, info.revision, info.reset_cause) == (0x7100, 0xA042, 0x3C)
    assert not info.halted


def test_read_data_memory_in_chunks(agent):
    agent.hello()
    assert agent.read(0x10, 4) == bytes([0x10, 0x11, 0x12, 0x13])
    assert agent.read(0x20, 100) == bytes((0x20 + i) & 0xFF for i in range(100))
    assert agent.read(0xF0, 0x20) == bytes((0xF0 + i) & 0xFF for i in range(0x20))


def test_read_flash(agent):
    assert agent.read_flash(0, 5) == bytes((i * 7 + 3) & 0xFF for i in range(5))


def test_state_commands_are_refused_while_running(agent):
    for call in (agent.cont, agent.regs, agent.stack, lambda: agent.write(0x100, b"\x01")):
        with pytest.raises(PdbgError) as error:
            call()
        assert error.value.code == pdbg.ERR_STATE


def test_halt_regs_stack_write_cont(agent):
    agent.halt()
    event = agent.wait_halt()
    assert (event.reason_text, event.pc) == ("request", 0x1002)
    regs = agent.regs()
    assert (regs.pc, regs.wreg, regs.status, regs.bsr) == (0x1002, 0x11, 0x04, 0x01)
    assert (regs.fsr0, regs.fsr1, regs.fsr2) == (0x0210, 0x0320, 0x0430)
    assert (regs.prod, regs.tblptr, regs.tablat, regs.stkptr) == (0xBEEF, 0x012345, 0x99, 3)
    assert agent.stack() == [0x1234, 0x456, 0x789]
    agent.write(0x200, bytes(range(1, 90)))
    assert agent.read(0x200, 89) == bytes(range(1, 90))
    agent.cont()
    with pytest.raises(PdbgError):
        agent.regs()


def test_cont_discards_the_duplicate_halt_announcement(spawn_agent):
    agent, _ = spawn_agent("--hit", "5@4")
    agent.halt()  # two empty frames: the agent announces the halt twice
    agent.wait_halt()
    agent.cont()
    assert not agent.events
    agent.set_breakpoint(5)
    agent.read(0, 1)
    assert agent.wait_halt().arg == 5  # the real event, not a leftover


def test_a_late_duplicate_reply_is_not_taken_for_a_later_one():
    link = Pdbg(object())
    link.replies[7] = (pdbg.CMD_RD, 0, b"stale")
    link.seq = 6  # the next request is seq 7 again, after a wrap

    class Echo:
        def write(self, data):
            self.out = pdbg.encode_frame(bytes([pdbg.CH_REPLY, 7, pdbg.CMD_RD, 0]) + b"fresh")

        def read(self, n):
            out, self.out = self.out, b""
            return out

    link.stream = Echo()
    assert link.request(pdbg.CMD_RD) == b"fresh"


def test_a_second_halt_request_is_answered_again(agent):
    agent.halt()
    agent.wait_halt()
    agent.halt()  # already halted: the agent says so again rather than going quiet
    assert agent.wait_halt().pc == 0x1002


def test_a_write_holding_the_bridge_exit_sequence_is_split(agent):
    agent.halt()
    agent.wait_halt()
    data = b"A" * 5 + BRIDGE_EXIT + b"B" * 5
    agent.write(0x300, data)
    assert agent.read(0x300, len(data)) == data


def test_a_timed_out_write_is_not_split():
    class Silent:
        writes = 0

        def write(self, data):
            self.writes += 1

        def read(self, n):
            return b""

    stream = Silent()
    with pytest.raises(PdbgTimeout):
        Pdbg(stream, retries=0).write(0x100, b"abcdefgh")
    assert stream.writes == 1


def test_breakpoints(agent):
    assert agent.breakpoints() == set()
    assert agent.set_breakpoint(3) == {3}
    assert agent.set_breakpoint(31) == {3, 31}
    assert agent.set_breakpoint(3, False) == {31}
    with pytest.raises(PdbgError) as error:
        agent.set_breakpoint(32)
    assert error.value.code == pdbg.ERR_ARG


def test_an_enabled_site_halts_a_running_program_and_a_disabled_one_does_not(spawn_agent):
    agent, _ = spawn_agent("--hit", "4@3")  # site 4 is reached after the third frame
    agent.hello()
    agent.set_breakpoint(9)  # a different site: stays silent
    agent.set_breakpoint(4)
    agent.read(0, 1)
    event = agent.wait_halt()
    assert (event.reason_text, event.arg) == ("break", 4)
    agent.cont()
    assert agent.read(0, 1) == b"\x00"

    agent, _ = spawn_agent("--hit", "4@3")
    agent.hello()
    agent.read(0, 1)
    agent.read(0, 1)
    agent.stream.timeout = 0.3
    with pytest.raises(PdbgTimeout):  # site 4 never enabled: no event arrives
        agent.wait_halt()


def test_a_lost_request_is_sent_again(spawn_agent):
    agent, proc = spawn_agent()
    agent.hello()
    # Corrupt the first try: the agent drops it silently, the retry gets through.
    good = pdbg.encode_frame(bytes([pdbg.CMD_RD, 0x55, 0x10, 0x00, 1]))
    proc.stdin.write(b"\x03xx\x00" + good[:-1])  # junk frame, then half a frame
    agent.stream.timeout = 0.3
    assert agent.read(0x10, 1) == b"\x10"


def test_no_answer_is_a_timeout_after_the_retries():
    class Silent:
        writes = 0

        def write(self, data):
            self.writes += 1

        def read(self, n):
            return b""

    stream = Silent()
    with pytest.raises(PdbgTimeout):
        Pdbg(stream, retries=2).hello()
    assert stream.writes == 3
    with pytest.raises(PdbgTimeout):
        Pdbg(stream).cont()
    assert stream.writes == 4  # CONT is not safe to run twice: sent once


def test_reset_is_answered(agent):
    agent.reset()
    assert agent.hello().version == 1


def test_console_text_collects_beside_replies():
    class Canned:
        """A stream that returns console frames and the reply in one read."""

        def __init__(self):
            self.sent = []
            self.out = b""

        def write(self, data):
            self.sent.append(data)
            seq = pdbg.cobs_decode(data[:-1])[1]
            self.out = (pdbg.encode_frame(b"\x02hello ") + pdbg.encode_frame(b"\x02world")
                        + pdbg.encode_frame(bytes([pdbg.CH_REPLY, seq, pdbg.CMD_CONT, 0])))

        def read(self, n):
            data, self.out = self.out[:n], self.out[n:]
            return data

    link = Pdbg(Canned())
    link.cont()
    assert link.take_console() == b"hello world"
    assert link.take_console() == b""
