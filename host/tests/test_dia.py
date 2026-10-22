"""Decoding of the Device Information Area for each family layout (no programmer needed)."""
from picprog import devices, dia


def words_for(device, **at):
    """A DIA of distinct words (index + 0x100) with the given indices set to a value."""
    words = [0x100 + i for i in range(device.size("dia"))]
    for index, value in at.items():
        words[int(index[1:])] = value
    return words


def test_host_table_has_the_window_sizes_the_firmware_uses():
    # Literal copy of src/proto dia_words: 32 for p16_8bit (8100h-811Fh), p18_8bit and p18_k42,
    # 0 for p18_q10. The end-to-end tests read the p16 window from the firmware itself.
    sizes = {d.protocol: d.size("dia") for d in devices.load()}
    assert sizes == {"p16_8bit": 32, "p18_8bit": 32, "p18_q10": 0, "p18_k42": 32}


def test_dia_is_readable_but_not_a_writable_region():
    assert "dia" in devices.READ_REGIONS and "dia" not in devices.REGIONS
    assert devices.find("PIC18F16Q41").cell_mask("dia") == 0xFFFF
    assert devices.find("PIC16F17146").cell_mask("dia") == 0x3FFF


def test_standard_layout_on_a_pic16_and_a_pic18q():
    for name, blank in (("PIC16F17146", 0x3FFF), ("PIC18F16Q41", 0xFFFF)):
        device = devices.find(name)
        words = words_for(device)
        for i in range(10, 18):
            words[i] = blank
        words[24:27] = [1024, 2048, 4096]
        text = "\n".join(dia.decode(device, words))
        assert "MUI  0100 0101 0102 0103 0104 0105 0106 0107 0108\n" in text + "\n"
        assert "EUI  not programmed" in text
        assert "TSLR1  0112" in text and "TSHR3  0117" in text  # words 18 and 23
        assert "FVRA1X  1024 mV" in text and "FVRA4X  4096 mV" in text
        assert "FVRC4X  " in text  # word 29


def test_blank_calibration_words_are_not_shown_as_measurements():
    device = devices.find("PIC16F17146")
    words = words_for(device, w24=0x3FFF, w18=0x3FFF)
    text = "\n".join(dia.decode(device, words))
    assert "FVRA1X  not programmed" in text and "TSLR1  not programmed" in text
    assert "16383" not in text


def test_programmed_external_id_is_shown():
    device = devices.find("PIC16F17146")
    words = words_for(device)
    text = "\n".join(dia.decode(device, words))
    assert "EUI  010A 010B 010C 010D 010E 010F 0110 0111" in text


def test_pic16f152xx_has_no_temperature_data_and_only_the_adc_fvr():
    device = devices.find("PIC16F15213")
    text = "\n".join(dia.decode(device, words_for(device)))
    assert "TSL" not in text and "TSH" not in text and "temperature" not in text
    assert "FVRA1X" in text and "FVRA4X" in text and "FVRC" not in text


def test_k42_layout_differs_from_the_q_parts():
    device = devices.find("PIC18F27K42")
    words = words_for(device)
    words[19], words[22] = 0x0A20, 0x0A60
    text = "\n".join(dia.decode(device, words))
    assert "MUI  0100 0101 0102 0103 0104 0105\n" in text + "\n"  # six words, not nine
    assert "EUI  0108 0109 010A 010B 010C 010D 010E 010F 0110 0111" in text  # ten, from word 8
    assert "TSLR2  0A20" in text and "TSHR2  0A60" in text
    assert "TSLR1" not in text and "TSLR3" not in text
    assert "FVRC4X" in text


def test_k42_lf_parts_have_no_fvrc4x():
    device = devices.find("PIC18LF27K42")
    text = "\n".join(dia.decode(device, words_for(device)))
    assert "FVRC2X" in text and "FVRC4X" not in text


def test_a_family_without_a_dia_decodes_to_nothing():
    device = devices.find("PIC18F24Q10")
    assert device.size("dia") == 0 and dia.decode(device, []) == []
