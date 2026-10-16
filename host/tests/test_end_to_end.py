"""The host tool against the firmware's command layer and the simulated PIC16F17146."""
import pytest

from picprog import devices, ihex, programmer
from picprog.link import LinkError


def test_probe_ids_and_dci(target):
    assert target.probe()[:2] == ("PIC16F17146", 0x30E1)
    assert target.ids()[0] == 0x30E1
    assert target.dci() == {"erase_words": 32, "latch_words": 32, "user_rows": 512,
                            "eeprom_bytes": 256, "pin_count": 20}
    assert programmer.connect(target).name == "PIC16F17146"


def test_commands_before_probe_are_refused(target):
    with pytest.raises(LinkError, match="no-device"):
        target.read("flash", 0, 1)


def test_dump_of_a_blank_part(target):
    device = programmer.connect(target)
    image, warnings = programmer.dump(target, device)
    assert warnings == []
    cells, unused = devices.image_to_cells(device, image)
    assert unused == []
    assert len(cells["flash"]) == 16384 and set(cells["flash"].values()) == {0x3FFF}
    assert set(cells["eeprom"].values()) == {0xFF}
    # And it survives the trip through a hex file.
    assert ihex.parse(ihex.to_text(image)) == image


def make_image(device):
    image = {}
    image.update(devices.cells_to_image(device, "flash", 0, [(0x100 + i) & 0x3FFF
                                                             for i in range(40)]))
    image.update(devices.cells_to_image(device, "flash", 100, [0x2AAA, 0x1555]))
    image.update(devices.cells_to_image(device, "userid", 0, [1, 2, 3, 4]))
    image.update(devices.cells_to_image(device, "config", 0, [0x3FEF, 0x3FFF, 0x3FFE,
                                                              0x3FFF, 0x3FFF]))
    image.update(devices.cells_to_image(device, "eeprom", 0, [0xA5, 0x5A]))
    return image


def test_program_then_dump_gives_the_image_back(target):
    device = programmer.connect(target)
    image = make_image(device)
    assert programmer.program(target, device, image) == []
    dumped, _ = programmer.dump(target, device)
    for address, byte in image.items():
        assert dumped[address] == byte, hex(address)
    cells, _ = devices.image_to_cells(device, dumped)
    assert cells["flash"][40] == 0x3FFF  # rest of the first row: padded, so unchanged
    assert cells["flash"][99] == 0x3FFF


def test_verify_reports_the_first_difference(target):
    device = programmer.connect(target)
    programmer.program(target, device, make_image(device))
    data = [(0x100 + i) & 0x3FFF for i in range(40)]
    target.verify("flash", 0, data)
    data[17] ^= 1
    with pytest.raises(LinkError) as err:
        target.verify("flash", 0, data)
    assert err.value.code == -8 and str(err.value).endswith("@00000011")


def test_program_without_erase_needs_a_blank_part(target):
    device = programmer.connect(target)
    programmer.program(target, device, make_image(device))
    other = devices.cells_to_image(device, "flash", 0, [0x0001])
    with pytest.raises(LinkError) as err:  # 1s cannot be written back without an erase
        programmer.program(target, device, other, erase=False)
    assert err.value.code == -8


def test_warnings_for_a_foreign_or_incomplete_hex_file(target):
    device = programmer.connect(target)
    image = devices.cells_to_image(device, "flash", 0, [0x0123])
    image[0x1000C], image[0x1000D] = 0xDC, 0x30  # a PIC16F17124's device ID
    warnings = programmer.program(target, device, image)
    assert any("device ID 0x30DC" in w for w in warnings)
    assert any("no Configuration Words" in w for w in warnings)


def test_all_zero_flash_is_reported_as_probably_code_protected(target):
    device = programmer.connect(target)
    target.write("flash", 0, [0] * 16384)
    _, warnings = programmer.dump(target, device)
    assert any("code protected" in w for w in warnings)


@pytest.mark.parametrize("target", ["30DC"], indirect=True)
def test_a_different_part_is_found_as_itself(target):
    assert programmer.connect(target).name == "PIC16F17124"


def test_command_line_dump_and_write(target, tmp_path, monkeypatch, capsys):
    from picprog import cli

    monkeypatch.setattr(cli, "open_link", lambda args: target)
    device = devices.find("PIC16F17146")
    source = tmp_path / "in.hex"
    source.write_text(ihex.to_text(make_image(device)))
    assert cli.main(["-q", "write", str(source)]) == 0
    assert cli.main(["-q", "verify", str(source)]) == 0
    out = tmp_path / "out.hex"
    assert cli.main(["-q", "dump", str(out)]) == 0
    dumped = ihex.parse(out.read_text())
    for address, byte in make_image(device).items():
        assert dumped[address] == byte
    assert "PIC16F17146: matches" in capsys.readouterr().out
    assert cli.main(["read", "flash", "0x10000", "1"]) == 1  # outside the region


def test_program_without_erase_does_not_verify_the_padding(target):
    """A second image in the same flash row, written without an erase: the padding around
    it covers cells the first image programmed, and must not count as a mismatch."""
    device = programmer.connect(target)
    first = devices.cells_to_image(device, "flash", 0, [0x0101])
    second = devices.cells_to_image(device, "flash", 5, [0x0202])
    programmer.program(target, device, first)
    assert programmer.program(target, device, second, erase=False) == [
        "the hex file has no Configuration Words"]
    cells, _ = devices.image_to_cells(device, programmer.dump(target, device)[0])
    assert cells["flash"][0] == 0x0101 and cells["flash"][5] == 0x0202
