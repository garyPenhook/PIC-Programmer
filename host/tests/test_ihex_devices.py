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


def test_p18_q20_config_bytes_are_split_in_the_address_map():
    dev = devices.find("PIC18F16Q20")
    # DS40002327D 2.4 and 6.14: CONFIG1-8, CONFIG10-12 at 300000h-30000Ah, then CONFIG14 at
    # 300018h and CONFIG9 at 300019h.
    assert dev.size("config") == 13
    assert dev.layout.cell_hex_bytes("config", 10) == (0x30000A, None)
    assert dev.layout.cell_hex_bytes("config", 11) == (0x300018, None)
    assert dev.layout.cell_hex_bytes("config", 12) == (0x300019, None)
    assert [dev.config_name(n) for n in (8, 9, 12, 13)] == ["CONFIG8", "CONFIG10", "CONFIG14", "CONFIG9"]
    cells, unused = devices.image_to_cells(dev, {0x300019: 0x7F, 0x30000B: 0x00})
    assert cells["config"] == {12: 0x7F} and unused == [0x30000B]
    assert devices.find("PIC18F16Q41").config_name(9) == "CONFIG9"


def test_p18_q20_config_decode():
    from picprog import configbits
    dev = devices.find("PIC18F04Q20")
    words = [0xFF] * 13
    words[11] = 0xFE  # CONFIG14: SAFLOCK cleared
    lines, warnings = configbits.decode(dev, words)
    assert any(line.startswith("CONFIG14 = 0xFE") for line in lines)
    assert any(line.startswith("CONFIG9 = 0xFF") for line in lines)
    assert [w for w in warnings if "SAFLOCK" in w] and warnings[0].startswith("CONFIG14 SAFLOCK")


def test_p18_q24_config_bytes_follow_the_register_summary():
    dev = devices.find("PIC18F26Q24")
    # DS40002503 8.6: CONFIG1-7 at 300000h-300006h, CONFIG9-11 at 300008h-30000Ah, CONFIG13 at
    # 300018h, CONFIG8 at 300019h, CONFIG12 at 30001Ch.
    assert dev.size("config") == 13 and dev.size("eeprom") == 512 and dev.size("flash") == 32768
    addresses = [dev.layout.cell_hex_bytes("config", n)[0] for n in range(13)]
    assert addresses == [0x300000 + a for a in (0, 1, 2, 3, 4, 5, 6, 8, 9, 10, 0x18, 0x19, 0x1C)]
    assert [dev.config_name(n) for n in range(1, 14)] == [
        "CONFIG1", "CONFIG2", "CONFIG3", "CONFIG4", "CONFIG5", "CONFIG6", "CONFIG7", "CONFIG9",
        "CONFIG10", "CONFIG11", "CONFIG13", "CONFIG8", "CONFIG12"]
    cells, unused = devices.image_to_cells(dev, {0x30001C: 0x7E, 0x300007: 0x00})
    assert cells["config"] == {12: 0x7E} and unused == [0x300007]


def test_p18_q24_config_decode():
    from picprog import configbits
    dev = devices.find("PIC18F45Q24")
    words = [0xFF] * 13
    words[12] = 0xFE  # CONFIG12: ICSPDIS cleared
    lines, warnings = configbits.decode(dev, words)
    assert any(line.startswith("CONFIG12 = 0xFE") for line in lines)
    assert any(line.startswith("CONFIG8 = 0xFF") for line in lines)
    assert [w for w in warnings if "ICSPDIS" in w]


def test_p18_q40_matches_the_q41_layout():
    dev = devices.find("PIC18F16Q40")
    # DS40002185B Figure 2-1: 32 KW of flash, 512 bytes of EEPROM, Configuration Bytes at 30 0000h-30 0009h.
    assert dev.size("config") == 10 and dev.size("eeprom") == 512 and dev.size("flash") == 32768
    assert dev.device_id == 0x75A0
    assert dev.config_name(9) == "CONFIG9"


def test_p18_q40_config_decode():
    from picprog import configbits
    dev = devices.find("PIC18F05Q40")
    words = [0xFF] * 10
    words[8] = 0xFE  # CONFIG9 = CP: code protection on
    lines, warnings = configbits.decode(dev, words)
    assert any(line.startswith("CONFIG9 = 0xFE") for line in lines)
    assert [w for w in warnings if "CP" in w]


def test_p18_q71_has_eleven_contiguous_config_bytes():
    dev = devices.find("PIC18F46Q71")
    # DS40002306D Figure 2-1: Configuration Bytes at 30 0000h-30 000Ah, 256 bytes of EEPROM.
    assert dev.size("config") == 11 and dev.size("eeprom") == 256 and dev.size("flash") == 32768
    assert dev.layout.cell_hex_bytes("config", 10) == (0x30000A, None)
    assert dev.config_name(11) == "CONFIG11"


def test_p18_q71_config_decode():
    from picprog import configbits
    dev = devices.find("PIC18F24Q71")
    words = [0xFF] * 11
    words[10] = 0xFE  # CONFIG11 = CP: code protection on
    words[9] = 0xFE   # CONFIG10 = CPD
    lines, warnings = configbits.decode(dev, words)
    assert any(line.startswith("CONFIG11 = 0xFE") for line in lines)
    assert [w for w in warnings if "CONFIG11 CP" in w] and [w for w in warnings if "CONFIG10 CPD" in w]


def test_p18_q8x_has_35_contiguous_config_bytes():
    for name, flash in (("PIC18F27Q84", 65536), ("PIC18F46Q83", 32768)):
        dev = devices.find(name)
        # 40002137D Figure 2-1 and 2.4: Configuration Bytes at 30 0000h-30 0022h, 1024-byte EEPROM.
        assert dev.size("config") == 35 and dev.size("eeprom") == 1024 and dev.size("flash") == flash
        assert dev.layout.cell_hex_bytes("config", 34) == (0x300022, None)
    assert devices.find("PIC18F57Q84").device_id == 0x9905
    assert devices.find("PIC18F56Q83").device_id == 0xA308


def test_p18_q8x_config_decode():
    from picprog import configbits
    dev = devices.find("PIC18F47Q83")
    words = [0xFF] * 35
    words[9] = 0xFE   # CONFIG10: CP on
    words[6] = 0xDF   # CONFIG7: DEBUG = 0
    lines, warnings = configbits.decode(dev, words)
    assert any(line.startswith("CONFIG10 = 0xFE") for line in lines)
    assert [w for w in warnings if "CONFIG10 CP" in w]


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


def test_p18_k42_layout_follows_the_programming_spec():
    dev = devices.find("PIC18LF47K42")
    # 40001886B Table 2-1 and 3.4: User IDs from 200000h, Configuration Words from 300000h (1L,
    # 1H, 2L, ...), data EEPROM from 310000h, device ID at 3FFFFEh.
    assert dev.layout.cell_hex_bytes("config", 4) == (0x300008, 0x300009)
    assert dev.layout.cell_hex_bytes("eeprom", 1) == (0x310001, None)
    assert dev.layout.cell_hex_bytes("userid", 7) == (0x20000E, 0x20000F)
    assert dev.layout.cell_mask("config") == 0xFFFF and dev.layout.cell_mask("eeprom") == 0xFF
    assert dev.size("config") == 5 and dev.size("userid") == 8 and dev.size("eeprom") == 1024


def test_config_decode_k42():
    from picprog import configbits
    # BORV 11 means 2.45 V on the F parts and 1.90 V on the LF parts (40001886B Register B-4).
    for name, expect in (("PIC18F26K42", "2.45 V"), ("PIC18LF26K42", "1.90 V")):
        dev = devices.find(name)
        lines, warnings = configbits.decode(dev, _words(dev))
        assert expect in "\n".join(lines), name
        assert warnings == []
    dev = devices.find("PIC18F26K42")
    lines, warnings = configbits.decode(dev, _words(dev, LVP=0, CP=0))
    assert any("LVP" in w for w in warnings) and any("CP" in w for w in warnings)


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
    words = [dev.cell_mask("flash")] * dev.size("config")
    for fld in configbits.fields_for(dev):
        if fld.name in fields:
            mask = ((1 << (fld.hi - fld.lo + 1)) - 1) << fld.lo
            words[fld.word - 1] = (words[fld.word - 1] & ~mask) | (fields[fld.name] << fld.lo)
    return words


def test_every_config_word_of_every_table_row_is_decoded():
    from picprog import configbits
    for dev in devices.load():
        fields = configbits.fields_for(dev)
        # PIC18-Q has 10 Configuration Bytes with one reserved: the tenth (30 0009h) on
        # the Q41, the ninth (30 0008h) on the Q43.
        # PIC16F152XX: CONFIG3 is reserved (DS40002149B 6.3).
        reserved = ({10} if "Q41" in dev.name or "Q40" in dev.name else {9} if "Q43" in dev.name
                    else {3} if dev.name.startswith("PIC16F152") else set())
        expected = set(range(1, dev.size("config") + 1)) - reserved
        assert {f.word for f in fields} == expected, dev.name
        for f in fields:
            assert f.values or f.name == "CLBSA", (dev.name, f.name)


def test_config_fields_do_not_overlap_and_fit_14_bits():
    from picprog import configbits
    for dev in devices.load():
        used = {}
        for f in configbits.fields_for(dev):
            assert 0 <= f.lo <= f.hi <= {"p16_8bit": 13, "p18_8bit": 7, "p18_q10": 15, "p18_k42": 15}[dev.protocol], (dev.name, f.name)
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


def test_config_decode_f17576_has_no_zcd_and_five_words():
    # DS40002540B 6.1-6.5: five words, no ZCD bit (the F171XX has one), BOREN 00 = BOR enabled.
    from picprog import configbits
    dev = devices.find("PIC16F17576")
    fields = configbits.fields_for(dev)
    assert {f.word for f in fields} == {1, 2, 3, 4, 5}
    assert not any(f.name == "ZCD" for f in fields)
    lines, warnings = configbits.decode(dev, _words(dev, LVP=0, CP=0, BOREN=0b00))
    assert any(l.strip().startswith("BOREN") and "BOR enabled" in l for l in lines)
    assert any("LVP" in w for w in warnings) and any(w.startswith("CONFIG5 CP") for w in warnings)


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
    assert cli.main(["config", "--device", "PIC18F16Q41", str(path)]) == 0
    assert "CONFIG1: not present" in capsys.readouterr().out  # a PIC16 hex has no Q41 config bytes


def test_config_decode_q10():
    from picprog import configbits
    dev = devices.find("PIC18F46Q10")
    lines, warnings = configbits.decode(dev, _words(dev, LVP=0, CP=0, BOREN=0b00, WRT3=0, BORV=0b10))
    text = "\n".join(lines)
    assert "CONFIG6 = 0xFFFF" in text and "CONFIG7" not in text
    assert "BOR disabled" in text and "VBOR 2.45 V" in text
    assert any("LVP" in w for w in warnings) and any(w.startswith("CONFIG5 CP") for w in warnings)
    assert sum("WRT3" in l and "protected" in l and "not" not in l for l in lines) == 1
    assert configbits.decode(dev, _words(dev))[1] == []


def test_config_decode_q41():
    from picprog import configbits
    dev = devices.find("PIC18F16Q41")
    words = [0xFF] * dev.size("config")
    fields = {f.name: f for f in configbits.fields_for(dev)}
    for name, value in (("LVP", 0), ("CP", 0), ("MVECEN", 0), ("BORV", 0b01)):
        f = fields[name]
        mask = ((1 << (f.hi - f.lo + 1)) - 1) << f.lo
        words[f.word - 1] = (words[f.word - 1] & ~mask) | (value << f.lo)
    assert words[3] == 0xDD and words[8] == 0xFE  # LVP bit 5, BORV 01; CP bit 0
    lines, warnings = configbits.decode(dev, words)
    text = "\n".join(lines)
    assert "CONFIG9 = 0xFE" in text and "CONFIG10" not in text
    assert "legacy interrupt behaviour" in text and "VBOR 2.7 V" in text
    assert any("LVP" in w for w in warnings) and any(w.startswith("CONFIG9 CP") for w in warnings)
    assert configbits.decode(dev, [0xFF] * dev.size("config"))[1] == []
