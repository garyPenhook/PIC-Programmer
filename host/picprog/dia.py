"""Decode the Device Information Area (DIA): the factory-set unique ID and calibration data.

The DIA is read only. The words are laid out per family; the tables below are transcribed
from the programming specifications (PIC16: DS40002266B, DS40002149B, DS40002276A,
DS40002500B, DS40002540B, DS40002646A Table 2-2) and data sheets (PIC18-Q: Table 9-2 of
DS40002214G and the other Q data sheets, which agree; K42: DS40001919G Table 5-3). Each entry
is (name, word index from the start of the DIA, meaning). The calibration values are shown as
the part stores them; they were measured at VDD = 3 V with VREF+ = 2.048 V from FVR1.
"""

TS_GAIN = "gain, 0.1 C per count x 256"
TS_90C = "ADC reading at 90 C"
TS_OFFSET = "offset"


def _temperature(first, prefix, range_name):
    return [(f"{prefix}1", first, f"{range_name} range {TS_GAIN}"),
            (f"{prefix}2", first + 1, f"{range_name} range {TS_90C}"),
            (f"{prefix}3", first + 2, f"{range_name} range {TS_OFFSET}")]


def _fvr(adc_first, comparator_first, comparator_gains=(1, 2, 4)):
    adc = [(f"FVRA{g}X", adc_first + i, f"ADC FVR1 output, {g}x setting")
           for i, g in enumerate((1, 2, 4))]
    comp = [(f"FVRC{g}X", comparator_first + i, f"comparator FVR2 output, {g}x setting")
            for i, g in enumerate((1, 2, 4)) if g in comparator_gains]
    return adc + comp


class Layout:
    def __init__(self, mui, eui, temperature, fvr):
        self.mui = mui  # (first word, count)
        self.eui = eui
        self.temperature = temperature
        self.fvr = fvr


# PIC16F171XX, F181XX, F131XX, F132XX, F175XX and every p18_8bit part: MUI0-8, MUI9 reserved,
# EUI0-7, TSLR1-3, TSHR1-3, FVRA1X-4X, FVRC1X-4X.
STANDARD = Layout((0, 9), (10, 8), _temperature(18, "TSLR", "low") + _temperature(21, "TSHR", "high"), _fvr(24, 27))
# PIC16F152XX (DS40002149B Table 2-2): no temperature indicator data, ADC FVR only.
F152 = Layout((0, 9), (10, 8), [], _fvr(24, 27, ()))
# PIC18(L)F26/27/45/46/47/55/56/57K42 (DS40001919G Table 5-3): six MUI words, ten EUI words,
# only the 90 C reading of each range; FVRC4X is missing on the LF parts.
K42 = Layout((0, 6), (8, 10),
             [("TSLR2", 19, f"low range {TS_90C}"), ("TSHR2", 22, f"high range {TS_90C}")],
             _fvr(24, 27))
K42_LF = Layout(K42.mui, K42.eui, K42.temperature, _fvr(24, 27, (1, 2)))


def layout_for(device):
    """The Layout of this device's DIA, or None if the family has none."""
    if not device.size("dia"):
        return None
    if device.protocol == "p18_k42":
        return K42_LF if device.name.startswith("PIC18LF") else K42
    if device.name.startswith("PIC16F152"):
        return F152
    return STANDARD


def decode(device, words):
    """Text lines for the DIA words read from `device` (all device.size("dia") of them)."""
    layout = layout_for(device)
    if layout is None:
        return []
    width = 4  # hex digits of a word: 14 bits on PIC16, 16 on PIC18
    blank = device.blank("dia")

    def words_of(first, count):
        return words[first:first + count]

    mui = words_of(*layout.mui)
    eui = words_of(*layout.eui)
    lines = ["MUI  " + " ".join(f"{w:0{width}X}" for w in mui)]
    if all(w == blank for w in eui):
        lines.append("EUI  not programmed")
    else:
        lines.append("EUI  " + " ".join(f"{w:0{width}X}" for w in eui))
    if layout.temperature:
        lines.append("temperature indicator (raw):")
        for name, index, meaning in layout.temperature:
            lines.append(f"  {name}  {words[index]:0{width}X}  {meaning}")
    lines.append("fixed voltage reference:")
    for name, index, meaning in layout.fvr:
        lines.append(f"  {name}  {words[index]} mV  {meaning}")
    return lines
