"""Intel HEX handling and the region <-> hex address mapping (no programmer needed)."""
import pytest

from picprog import devices, ihex


def test_hex_round_trip_across_64k_pages():
    image = {0x0000: 0x12, 0x0001: 0x34, 0xFFFF: 0x56, 0x10000: 0x78, 0x1000E: 0x9A}
    text = ihex.to_text(image)
    assert ":020000040001F9" in text  # extended linear address 0001 (upper 16 bits)
    assert ihex.parse(text) == image


def test_hex_rejects_damage():
    good = ihex.to_text({0: 1, 1: 2})
    with pytest.raises(ihex.HexError, match="checksum"):
        ihex.parse(good.replace(":020000000102FB", ":020000000103FB"))
    with pytest.raises(ihex.HexError, match="end-of-file"):
        ihex.parse(good.replace(":00000001FF\n", ""))
    with pytest.raises(ihex.HexError, match="given twice"):
        ihex.parse(":0100000001FE\n:0100000002FD\n:00000001FF\n")


def test_p16_layout_follows_the_programming_spec():
    dev = devices.find("PIC16F17146")
    # DS40002266B 3.5: Configuration Word 1 (8007h) is at 1000Eh-1000Fh, LSB first.
    assert dev.layout.cell_hex_bytes("config", 0) == (0x1000E, 0x1000F)
    assert devices.cells_to_image(dev, "config", 0, [0x3ABC]) == {0x1000E: 0xBC, 0x1000F: 0x3A}
    # 3.5.2: the device ID (8006h) is at 1000Ch.
    assert dev.layout.device_id_hex == 0x1000C
    assert dev.layout.cell_hex_bytes("userid", 3) == (0x10006, 0x10007)


def test_p18_layout_is_byte_addressed():
    dev = devices.find("PIC18F16Q41")
    assert dev.layout.cell_hex_bytes("flash", 2) == (0x000004, 0x000005)
    assert dev.layout.cell_hex_bytes("config", 9) == (0x300009, None)
    assert dev.layout.cell_hex_bytes("eeprom", 1) == (0x380001, None)


def test_p18_q10_layout_follows_the_programming_spec():
    dev = devices.find("PIC18F27Q10")
    # DS40001874H Figure 2-1 and 3.4: Configuration Words from 300000h, two addresses each
    # (1L, 1H, 2L, ...); data EEPROM from 310000h, one byte per address.
    assert dev.layout.cell_hex_bytes("config", 0) == (0x300000, 0x300001)
    assert dev.layout.cell_hex_bytes("config", 5) == (0x30000A, 0x30000B)
    assert dev.layout.cell_hex_bytes("eeprom", 1) == (0x310001, None)
    assert dev.layout.cell_hex_bytes("userid", 1) == (0x200002, 0x200003)
    assert dev.layout.cell_mask("config") == 0xFFFF and dev.layout.cell_mask("eeprom") == 0xFF
    assert devices.cells_to_image(dev, "config", 0, [0x29A7]) == {0x300000: 0xA7, 0x300001: 0x29}


def test_image_to_cells_masks_values_and_reports_strays():
    dev = devices.find("PIC16F17146")
    image = devices.cells_to_image(dev, "flash", 4, [0x1234, 0x3FFF])
    image[0x1000C] = 0xE1  # device ID bytes are not a writable region
    image[0x1000D] = 0x30
    image[0x2] = 0xFF      # low byte only: the high byte is taken as erased
    cells, unused = devices.image_to_cells(dev, image)
    assert cells["flash"] == {1: 0x3FFF, 4: 0x1234, 5: 0x3FFF}
    assert unused == [0x1000C, 0x1000D]


def test_runs():
    assert devices.runs([5, 1, 2, 3, 9]) == [(1, 3), (5, 1), (9, 1)]
    assert devices.runs([]) == []


class ScriptedStream:
    """Replays a fixed reply and records what is written."""

    def __init__(self, reply):
        self.reply = reply
        self.written = b""

    def read(self, n):
        data, self.reply = self.reply[:n], self.reply[n:]
        return data

    def write(self, data):
        self.written += data


def test_bridge_open_and_close_parse_the_replies():
    from picprog.link import BRIDGE_EXIT, Link

    stream = ScriptedStream(b"OK bridge\nlast words\r\nOK bridge closed 7\n")
    link = Link(stream)
    link.bridge_open(9600)
    assert stream.written == b"BRIDGE 9600\n"
    assert link.bridge_close() == (b"last words", 7)
    assert stream.written.endswith(BRIDGE_EXIT)
