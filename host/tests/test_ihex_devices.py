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


def _words(dev, **fields):
    """Configuration Words of `dev` with every named field set; all other bits stay 1."""
    from picprog import configbits
    words = [0x3FFF] * dev.size("config")
    for fld in configbits.fields_for(dev):
        if fld.name in fields:
            mask = ((1 << (fld.hi - fld.lo + 1)) - 1) << fld.lo
            words[fld.word - 1] = (words[fld.word - 1] & ~mask) | (fields[fld.name] << fld.lo)
    return words


def test_every_config_word_of_every_table_row_is_decoded():
    from picprog import configbits
    for dev in devices.load():
        fields = configbits.fields_for(dev)
        if dev.protocol != "p16_8bit":
            continue  # PIC18 rows are not transcribed yet
        assert {f.word for f in fields} == set(range(1, dev.size("config") + 1)), dev.name
        for f in fields:
            assert f.values or f.name == "CLBSA", (dev.name, f.name)


def test_config_fields_do_not_overlap_and_fit_14_bits():
    from picprog import configbits
    for dev in devices.load():
        used = {}
        for f in configbits.fields_for(dev):
            assert 0 <= f.lo <= f.hi <= 13, (dev.name, f.name)
            for bit in range(f.lo, f.hi + 1):
                assert (f.word, bit) not in used, (dev.name, f.name, used[(f.word, bit)])
                used[(f.word, bit)] = f.name


def test_value_tables_cover_every_value():
    from picprog import configbits
    for f in configbits.load():
        if f.values:
            for v in range(1 << (f.hi - f.lo + 1)):
                assert f.lookup(f.values, v) is not None, (f.devices, f.name, v)


def test_config_decode_f13276():
    from picprog import configbits
    dev = devices.find("PIC16F13276")
    lines, warnings = configbits.decode(dev, _words(dev, LVP=0, ICSPDIS=0, CP=0, BOREN=0b00))
    text = "\n".join(lines)
    assert "CONFIG7" in text and "CONFIG6" in text
    assert "BOREN" in text and "BOR enabled (SBOREN ignored)" in text  # DS40002646A 6.2
    assert any("ICSPDIS" in w for w in warnings) and any("LVP" in w for w in warnings)
    assert any(w.startswith("CONFIG5 CP") for w in warnings)
    assert configbits.decode(dev, _words(dev))[1] == []  # erased words warn about nothing


def test_boren_00_means_opposite_things_on_the_two_families():
    # DS40002500B 6.2: 00 = BOR disabled.  DS40002646A 6.2: 00 = BOR enabled.
    from picprog import configbits
    for name, expect in (("PIC16F13145", "BOR disabled"), ("PIC16F13276", "BOR enabled")):
        dev = devices.find(name)
        lines, _ = configbits.decode(dev, _words(dev, BOREN=0))
        assert any(l.strip().startswith("BOREN") and expect in l for l in lines), name


def test_wdtcps_ranges():
    from picprog import configbits
    dev = devices.find("PIC16F13276")
    f = next(x for x in configbits.fields_for(dev) if x.name == "WDTCPS")
    assert f.lookup(f.values, 0b11111).startswith("1:65536")
    assert f.lookup(f.values, 0b10100).startswith("1:32")
    assert f.lookup(f.values, 0b01011).startswith("1:65536 (2 s)")


def test_picprog_config_decodes_a_hex_file_without_a_programmer(tmp_path, capsys):
    from picprog import cli
    dev = devices.find("PIC16F13276")
    words = _words(dev, ICSPDIS=0, LVP=0)
    image = devices.cells_to_image(dev, "config", 0, words)
    path = tmp_path / "cfg.hex"
    path.write_text(ihex.to_text(image))
    assert cli.main(["config", "--device", "PIC16F13276", str(path)]) == 0
    out = capsys.readouterr()
    assert "CONFIG7 = 0x3FFF" in out.out and "ICSPDIS" in out.out
    assert "permanently disabled" in out.err and "LVP cleared" in out.err
    assert cli.main(["config", "--device", "PIC18F16Q41", str(path)]) == 1  # no table yet
    assert "no Configuration Word table" in capsys.readouterr().err
