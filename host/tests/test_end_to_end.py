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


def test_crc_matches_the_file_and_spots_a_change(target):
    device = programmer.connect(target)
    image = make_image(device)
    programmer.program(target, device, image)
    cells, _ = devices.image_to_cells(device, image)
    for region in devices.REGIONS:
        assert target.crc(region) == devices.region_crc(device, region, cells[region]), region
    changed = dict(cells["flash"])
    changed[7] ^= 1
    assert target.crc("flash") != devices.region_crc(device, "flash", changed)


def test_blank_check(target):
    device = programmer.connect(target)
    programmer.program(target, device, make_image(device))
    with pytest.raises(LinkError, match="not-blank @00000000"):
        target.blank("flash")
    assert target.blank("flash", 200, 50) == 50
    target.erase("ALL")
    assert target.blank("flash") == 16384


def test_selftest(target):
    states, shorts, ok = target.selftest()
    assert states == {"CLK": "float", "DAT": "float", "MCLR": "float"}
    assert shorts == [] and ok


def test_command_line_crc_blank_and_selftest(target, tmp_path, monkeypatch, capsys):
    from picprog import cli

    monkeypatch.setattr(cli, "open_link", lambda args: target)
    device = devices.find("PIC16F17146")
    source = tmp_path / "in.hex"
    source.write_text(ihex.to_text(make_image(device)))
    assert cli.main(["-q", "write", str(source)]) == 0
    assert cli.main(["crc", str(source)]) == 0
    other = tmp_path / "other.hex"
    image = make_image(device)
    image[0] ^= 1
    other.write_text(ihex.to_text(image))
    assert cli.main(["crc", str(other)]) == 1
    assert cli.main(["selftest"]) == 0
    assert cli.main(["erase", "all"]) == 0
    assert cli.main(["blank"]) == 0
    out = capsys.readouterr().out
    assert "DIFFERENT" in out and "flash: blank (16384 cells)" in out and "wiring ok" in out


def test_power_and_vdd(target):
    assert target.power() == (True, 3300)
    assert target.vdd() == (3300, 3300)
    assert target.power(False) == (False, 0)
    with pytest.raises(LinkError, match="power-off"):
        target.read("flash", 0, 1)
    assert target.power(True) == (True, 3300)
    assert programmer.connect(target).name == "PIC16F17146"  # probed again after power-up


class ScriptedStream:
    """Replies from a fixed byte string; records what was written."""

    def __init__(self, data):
        self.data = data
        self.written = b""

    def read(self, n):
        chunk, self.data = self.data[:n], self.data[n:]
        return chunk

    def write(self, data):
        self.written += data


@pytest.mark.parametrize("reply, call", [
    (b"OK garbage\n", lambda link: link.probe()),
    (b"OK 30E1\n", lambda link: link.ids()),
    (b"OK 1 2\n", lambda link: link.dci()),
    (b"OK on\n", lambda link: link.power()),
    (b"OK x y\n", lambda link: link.vdd()),
    (b"OK zz\n", lambda link: link.crc("flash")),
    (b"OK many\n", lambda link: link.blank("flash")),
    (b"NEXT 00000000\n", lambda link: link.write("flash", 0, [1, 2])),
    (b"NEXT zz 2\n", lambda link: link.write("flash", 0, [1, 2])),
    (b"LINE CLK\nOK\n", lambda link: link.selftest()),
])
def test_a_malformed_reply_is_a_link_error(reply, call):
    from picprog.link import Link

    with pytest.raises(LinkError, match="unexpected reply"):
        call(Link(ScriptedStream(reply)))


def test_a_malformed_reply_is_reported_not_a_traceback(monkeypatch, capsys):
    from picprog import cli
    from picprog.link import Link

    monkeypatch.setattr(cli, "open_link", lambda args: Link(ScriptedStream(b"OK garbage\n")))
    assert cli.main(["probe"]) == 1
    assert "picprog: unexpected reply" in capsys.readouterr().err


def test_ctrl_c_during_a_write_ends_the_transfer_and_drains_replies(target, monkeypatch):
    device = programmer.connect(target)

    def interrupt(done, total):
        raise KeyboardInterrupt

    with pytest.raises(KeyboardInterrupt):
        target.write("flash", 0, list(range(40)), progress=interrupt)
    # The transfer is over and nothing is left to confuse the next command.
    assert target.ids()[0] == device.device_id
    assert target.read("flash", 0, 1) is not None


def test_ctrl_c_just_after_a_transfer_finished_still_leaves_a_clean_link(target):
    programmer.connect(target)
    target._send("PROBE")  # an idle programmer answers the abort with ERR bad-command
    target._abort_session()
    assert target.ids()[0] == 0x30E1


def test_command_line_ctrl_c_exits_130(target, monkeypatch, capsys):
    from picprog import cli

    monkeypatch.setattr(cli, "open_link", lambda args: target)
    monkeypatch.setattr(cli, "cmd_probe", lambda link, args: (_ for _ in ()).throw(KeyboardInterrupt))
    assert cli.main(["probe"]) == 130
    assert "interrupted" in capsys.readouterr().err


def test_status_and_vdd_on_a_healthy_programmer(target):
    assert target.status() == {"clock": "pll", "adc": "ok", "reset": "normal"}
    assert target.vdd() == (3300, 3300)


def test_status_command_line_reports_a_fallback_clock(monkeypatch, capsys):
    from picprog import cli
    from picprog.link import Link

    monkeypatch.setattr(cli, "open_link",
                        lambda args: Link(ScriptedStream(b"OK clock=hsi16 adc=fail\n")))
    assert cli.main(["status"]) == 1
    captured = capsys.readouterr()
    assert "16 MHz fallback" in captured.out and "fallback clock" in captured.err


def test_power_reports_an_unavailable_vdd(monkeypatch, capsys):
    from picprog import cli
    from picprog.link import Link

    monkeypatch.setattr(cli, "open_link", lambda args: Link(ScriptedStream(b"OK on -1\n")))
    assert cli.main(["power"]) == 0
    assert "unavailable" in capsys.readouterr().out


def test_status_reports_a_watchdog_reset(monkeypatch, capsys):
    from picprog import cli
    from picprog.link import Link

    monkeypatch.setattr(cli, "open_link", lambda args: Link(ScriptedStream(
        b"OK clock=pll adc=ok reset=watchdog\n")))
    assert cli.main(["status"]) == 1
    captured = capsys.readouterr()
    assert "watchdog" in captured.out and "hung" in captured.err


def test_status_finds_a_programmer_on_its_fallback_baud_rate(monkeypatch, capsys):
    """After a failed PLL the programmer talks at half the usual rate; `picprog status`
    tries that when nothing answers at the first rate, and reports the fault."""
    from picprog import cli
    from picprog.link import Link

    bauds = []

    def open_link(args):
        bauds.append(args.baud)
        reply = b"" if len(bauds) == 1 else b"OK clock=hsi16 adc=ok reset=normal\n"
        return Link(ScriptedStream(reply))

    monkeypatch.setattr(cli, "open_link", open_link)
    assert cli.main(["--baud", "921600", "status"]) == 1
    assert bauds == [921600, 460800]
    captured = capsys.readouterr()
    assert "460800 baud" in captured.err and "fallback clock" in captured.err


def test_status_does_not_retry_after_a_reply_that_is_not_a_timeout(monkeypatch):
    from picprog import cli
    from picprog.link import Link

    calls = []

    def open_link(args):
        calls.append(args.baud)
        return Link(ScriptedStream(b"ERR bad-command\n"))

    monkeypatch.setattr(cli, "open_link", open_link)
    assert cli.main(["status"]) == 1
    assert len(calls) == 1


def test_status_with_no_programmer_says_both_rates_were_tried(monkeypatch, capsys):
    from picprog import cli
    from picprog.link import Link

    bauds = []

    def open_link(args):
        bauds.append(args.baud)
        return Link(ScriptedStream(b""))

    monkeypatch.setattr(cli, "open_link", open_link)
    assert cli.main(["--baud", "921600", "status"]) == 1
    assert bauds == [921600, 460800]
    assert "no answer at 921600 or 460800 baud" in capsys.readouterr().err


def test_a_timeout_on_another_command_points_at_status(monkeypatch, capsys):
    from picprog import cli
    from picprog.link import Link

    monkeypatch.setattr(cli, "open_link", lambda args: Link(ScriptedStream(b"")))
    assert cli.main(["probe"]) == 1
    assert "picprog status" in capsys.readouterr().err


def test_dia_words_come_from_the_part(target):
    programmer.connect(target)
    words = target.read("dia", 0, 64)
    assert words[:3] == [0x0A01, 0x0B02, 0x0C03]       # MUI0 to MUI2 of the simulator
    assert words[10:18] == [0x3FFF] * 8                  # EUI not programmed
    assert words[24:30] == [1024, 2048, 4096] * 2        # FVR readings
    with pytest.raises(LinkError, match="range"):
        target.read("dia", 60, 5)                        # past the 64-word window


def test_dia_is_read_only_and_survives_an_erase(target):
    programmer.connect(target)
    before = target.read("dia", 0, 64)
    for call in (lambda: target.write("dia", 0, [1]), lambda: target.erase("dia"),
                 lambda: target.erase_row("dia", 0), lambda: target.blank("dia")):
        with pytest.raises(LinkError, match="not supported"):
            call()
    target.erase("ALL")
    assert target.read("dia", 0, 64) == before
    assert target.crc("dia", 0, 64) == target.crc("dia", 0, 64)  # the CRC command accepts it


def test_dia_is_not_part_of_a_dump(target):
    device = programmer.connect(target)
    image, _ = programmer.dump(target, device)
    cells, unused = devices.image_to_cells(device, image)
    assert unused == [] and "dia" not in cells


def test_command_line_dia(target, monkeypatch, capsys):
    from picprog import cli

    monkeypatch.setattr(cli, "open_link", lambda args: target)
    assert cli.main(["dia"]) == 0
    out = capsys.readouterr().out
    assert out.startswith("PIC16F17146\n")
    assert "MUI  0A01 0B02 0C03 0D04 0E05 0F06 1007 1108 1209" in out
    assert "EUI  not programmed" in out
    assert "TSLR2  0A20" in out and "TSHR2  0A60" in out
    assert "FVRA1X  1024 mV" in out and "FVRC4X  4096 mV" in out
    assert cli.main(["dia", "--raw"]) == 0
    out = capsys.readouterr().out
    assert "000000: 0A01 0B02 0C03 0D04 0E05 0F06 1007 1108" in out
    assert "000038: " in out                             # 64 words, eight per row
    assert cli.main(["read", "dia", "0", "2"]) == 0
    assert "000000: 0A01 0B02" in capsys.readouterr().out


@pytest.mark.parametrize("target", ["30E1"], indirect=True)
def test_the_whole_device_table_agrees_on_the_p16_window(target):
    # The firmware reads 64 words for every p16_8bit row; the host's table says the same.
    assert devices.find("PIC16F17146").size("dia") == 64
