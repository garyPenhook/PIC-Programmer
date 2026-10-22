"""The device table (data/pic_devices.csv) and how each protocol lays memory out in a hex file.

Regions and cells are the same as in the firmware (include/pic.h): offsets count cells from
the start of the region; flash and User ID cells are program words, config cells are
Configuration Words (PIC16) or configuration bytes (PIC18), EEPROM cells are bytes.
"""
import importlib.util
import os
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
REGIONS = ("flash", "userid", "config", "eeprom")


def _shared_file(packaged, in_repo):
    """An installed picprog carries copies of the shared files in picprog/data (see
    pyproject.toml); a source checkout uses the ones at the repository root."""
    path = os.path.join(HERE, "data", packaged)
    return path if os.path.exists(path) else os.path.join(ROOT, *in_repo)


CSV_PATH = _shared_file("pic_devices.csv", ("data", "pic_devices.csv"))
GENERATOR_PATH = _shared_file("gen_devices.py", ("tools", "gen_devices.py"))


def _load_generator():
    spec = importlib.util.spec_from_file_location("gen_devices", GENERATOR_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Device:
    """One row of the device table, with its protocol's hex file layout."""

    def __init__(self, row):
        self.name = row["name"]
        self.protocol = row["protocol"]
        self.device_id = row["device_id"]
        self.flash_words = row["flash_words"]
        self.erase_words = row["erase_words"]
        self.latch_words = row["latch_words"]
        self.sizes = {"flash": row["flash_words"], "userid": row["userid_words"],
                      "config": row["config_count"], "eeprom": row["eeprom_bytes"]}
        self.layout = LAYOUTS[self.protocol]
        if row["config_map"]:
            self.layout = P18SplitConfigLayout(row["config_map"])

    def size(self, region):
        return self.sizes[region]

    def cell_mask(self, region):
        return self.layout.cell_mask(region)

    def config_name(self, position):
        """The register name of Configuration cell `position` (1-based)."""
        namer = getattr(self.layout, "config_name", None)
        return namer(position) if namer else f"CONFIG{position}"

    def blank(self, region):
        """The value an erased cell reads back as (all bits the cell stores set)."""
        return self.layout.cell_mask(region)

    def __repr__(self):
        return f"Device({self.name})"


class P16Layout:
    """p16_8bit: word-addressed, 14-bit words.

    DS40002266B 3.5: two bytes per program word in INHX32, LSB first, at twice the word
    address (Configuration Word 1 at 8007h is at 1000Eh-1000Fh). Word addresses from
    Table 2-1, as in src/proto/p16_8bit.c. The device ID is at 8006h (hex 1000Ch).
    The EEPROM placement (word address F000h, so hex 1E000h, one byte per word with the
    high byte 0) follows the same rule; the specification does not show it, so it is an
    assumption to confirm against an MPLAB-produced hex file (PLAN.md).
    """
    bases = {"flash": 0x0000, "userid": 0x8000, "config": 0x8007, "eeprom": 0xF000}
    device_id_hex = 2 * 0x8006

    @staticmethod
    def cell_mask(region):
        return 0xFF if region == "eeprom" else 0x3FFF

    def cell_hex_bytes(self, region, offset):
        """Hex addresses holding cell `offset` of `region`: (low byte, high byte or None)."""
        address = 2 * (self.bases[region] + offset)
        return address, address + 1


class P18Layout:
    """p18_8bit: byte-addressed, 16-bit program words.

    The hex file is byte oriented (TB3163), so hex addresses are the part's own addresses
    (Q41 data sheet DS40002214 memory map, as in src/proto/p18_8bit.c). Flash and User ID
    words take two addresses; config and EEPROM cells take one. Words are assumed to be
    stored low byte first at the even address, as on PIC16; this is not stated in the
    documents used so far and should be confirmed.
    """
    bases = {"flash": 0x000000, "userid": 0x200000, "config": 0x300000, "eeprom": 0x380000}
    device_id_hex = 0x3FFFFE

    @staticmethod
    def cell_mask(region):
        return 0xFFFF if region in ("flash", "userid") else 0xFF

    def cell_hex_bytes(self, region, offset):
        if region in ("flash", "userid"):
            address = self.bases[region] + 2 * offset
            return address, address + 1
        return self.bases[region] + offset, None


class P18SplitConfigLayout(P18Layout):
    """p18_8bit parts whose Configuration Bytes do not increment sequentially.

    config_map 1 is the PIC18-Q20 (DS40002327D 2.4, 6.14): 11 bytes at 30 0000h-30 000Ah, then
    CONFIG14 at 30 0018h and CONFIG9 at 30 0019h. config_map 2 is the PIC18-Q24 (DS40002503
    8.6): 30 0000h-30 0006h, CONFIG9-11 at 30 0008h-30 000Ah, CONFIG13 and CONFIG8 at 30 0018h
    and 30 0019h, CONFIG12 at 30 001Ch. Same tables as config_map in src/proto/p18_8bit.c.
    Cell n of the config region is the n-th byte in address order, so config_name() gives the
    register name of each position.
    """
    # config_map -> ((register name, address from 30 0000h) for each cell)
    maps = {
        1: tuple((f"CONFIG{n}", a) for n, a in
                 zip((1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 12, 14, 9), tuple(range(11)) + (0x18, 0x19))),
        2: tuple((f"CONFIG{n}", a) for n, a in
                 zip((1, 2, 3, 4, 5, 6, 7, 9, 10, 11, 13, 8, 12),
                     tuple(range(7)) + (8, 9, 10, 0x18, 0x19, 0x1C))),
    }

    def __init__(self, config_map):
        self.cells = self.maps[config_map]

    def cell_hex_bytes(self, region, offset):
        if region == "config":
            return self.bases["config"] + self.cells[offset][1], None
        return super().cell_hex_bytes(region, offset)

    def config_name(self, position):
        return self.cells[position - 1][0]


class P18Q10Layout:
    """p18_q10: byte-addressed, 16-bit program words (PIC18F2X/4XQ10, DS40001874H).

    Hex addresses are the part's own byte addresses (Figure 2-1; TB3163 for the hex format),
    as in src/proto/p18_q10.c. Flash, User ID and Configuration Words are 16-bit cells at
    two addresses each (Configuration Word nL at 30 0000h + 2(n-1), nH on the next address,
    spec Table B-1 and section 3.4.1); EEPROM cells are bytes from 31 0000h (section 3.4.2).
    As for P18Layout, the low byte is assumed to sit at the even address.
    """
    bases = {"flash": 0x000000, "userid": 0x200000, "config": 0x300000, "eeprom": 0x310000}
    device_id_hex = 0x3FFFFE

    @staticmethod
    def cell_mask(region):
        return 0xFF if region == "eeprom" else 0xFFFF

    def cell_hex_bytes(self, region, offset):
        if region == "eeprom":
            return self.bases[region] + offset, None
        address = self.bases[region] + 2 * offset
        return address, address + 1


LAYOUTS = {"p16_8bit": P16Layout(), "p18_8bit": P18Layout(), "p18_q10": P18Q10Layout()}


def load(path=CSV_PATH):
    """All devices in the table, validated by tools/gen_devices.py's own checks."""
    return [Device(row) for row in _load_generator().load(path)]


def find(name, path=CSV_PATH):
    for device in load(path):
        if device.name == name:
            return device
    raise KeyError(f"{name} is not in {path}")


def cells_to_image(device, region, offset, cells):
    """Hex image bytes for `cells` of `region` starting at `offset`."""
    image = {}
    for i, value in enumerate(cells):
        low, high = device.layout.cell_hex_bytes(region, offset + i)
        image[low] = value & 0xFF
        if high is not None:
            image[high] = (value >> 8) & 0xFF
    return image


def image_to_cells(device, image):
    """Split a hex image into {region: {offset: value}} for this device.

    Returns (cells, unused) where `unused` lists the hex addresses that are not inside any
    region of the device (the device ID, for example). A word whose second byte is missing
    takes the erased value for it.
    """
    cells = {region: {} for region in REGIONS}
    used = set()
    for region in REGIONS:
        mask = device.cell_mask(region)
        for offset in range(device.size(region)):
            low, high = device.layout.cell_hex_bytes(region, offset)
            if low not in image and (high is None or high not in image):
                continue
            lo = image.get(low, 0xFF)
            hi = image.get(high, 0xFF) if high is not None else 0
            cells[region][offset] = ((hi << 8) | lo) & mask
            used.add(low)
            if high is not None:
                used.add(high)
    unused = sorted(set(image) - used)
    return cells, unused


def region_crc(device, region, cells):
    """CRC-32 the programmer's CRC command gives for a whole region holding `cells`
    ({offset: value}); cells not given count as erased."""
    blank = device.blank(region)
    data = bytearray()
    for offset in range(device.size(region)):
        value = cells.get(offset, blank)
        data += bytes((value & 0xFF, value >> 8))
    return zlib.crc32(data)


def runs(offsets):
    """Group sorted cell offsets into contiguous (start, count) runs."""
    result = []
    for offset in sorted(offsets):
        if result and result[-1][0] + result[-1][1] == offset:
            result[-1][1] += 1
        else:
            result.append([offset, 1])
    return [(start, count) for start, count in result]
