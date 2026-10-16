"""Whole-device operations built on Link: connect, dump to a hex image, program from one."""
from . import devices
from .link import LinkError


def connect(link, table_path=devices.CSV_PATH):
    """Probe the target and return its Device row, checked against the part's own DCI.

    The firmware already refuses a part whose DCI disagrees with its table row; this repeats
    the check against the host's copy of the table, so a host and firmware built from
    different tables are caught too.
    """
    name, device_id, _ = link.probe()
    device = devices.find(name, table_path)
    if device.device_id != device_id:
        raise LinkError(f"{name}: device ID 0x{device_id:04X}, table says 0x{device.device_id:04X}")
    dci = link.dci()
    if (dci["erase_words"] != device.erase_words or dci["latch_words"] != device.latch_words
            or dci["user_rows"] * dci["erase_words"] != device.flash_words
            or dci["eeprom_bytes"] != device.size("eeprom")):
        raise LinkError(f"{name}: the part's DCI {dci} does not match data/pic_devices.csv")
    return device


def dump(link, device, progress=None):
    """Read every region into a hex image. Returns (image, warnings)."""
    image = {}
    warnings = []
    for region in devices.REGIONS:
        size = device.size(region)
        if size == 0:
            continue
        cells = link.read(region, 0, size, progress=_named(progress, region))
        image.update(devices.cells_to_image(device, region, 0, cells))
        if region == "flash" and not any(cells):
            # DS40002266B 3.4: with code protection on, program memory and EEPROM read as 0.
            warnings.append("flash reads as all zeros: the part is probably code protected, "
                            "so this dump is not a copy of its program")
    return image, warnings


def _named(progress, region):
    if progress is None:
        return None
    return lambda done, total: progress(region, done, total)


def _write_runs(device, region, offsets):
    """Runs to send for the cells in `offsets`, widened to whole rows where flash is written
    through row latches (the firmware writes a whole row at a time)."""
    rows = device.latch_words if region == "flash" else 0
    spans = devices.runs(offsets)
    if not rows:
        return spans
    widened = []
    for start, count in spans:
        first = start - start % rows
        end = -(-(start + count) // rows) * rows
        end = min(end, device.size(region))
        if widened and first <= widened[-1][1]:
            widened[-1][1] = max(widened[-1][1], end)
        else:
            widened.append([first, end])
    return [(first, end - first) for first, end in widened]


def program(link, device, image, erase=True, verify=True, progress=None):
    """Write a hex image to the target. Returns the warnings to show.

    With erase, the whole part is bulk erased first; without it, the cells written must
    already be erased (programming only turns 1s into 0s). Every cell the image does not
    give, but which shares a flash row with one it does, is written as blank, which leaves
    it unchanged.
    """
    cells, unused = devices.image_to_cells(device, image)
    warnings = []

    id_low = device.layout.device_id_hex
    if id_low in image and id_low + 1 in image:
        hex_id = image[id_low] | (image[id_low + 1] << 8)
        # DS40002266B 3.5.2: a device ID in the hex file must be checked against the part.
        mask = device.cell_mask("flash")  # 14-bit IDs on PIC16, 16-bit on PIC18
        if hex_id & mask != device.device_id & mask:
            warnings.append(f"hex file is for device ID 0x{hex_id:04X}, the part is "
                            f"{device.name} (0x{device.device_id:04X})")
    if device.size("config") and not cells["config"]:
        # DS40002266B 3.5.1: warn when the Configuration Words are not in the hex file.
        warnings.append("the hex file has no Configuration Words")
    skipped = [a for a in unused if a not in (id_low, id_low + 1)]
    if skipped:
        warnings.append(f"{len(skipped)} hex bytes are outside every region of {device.name} "
                        f"and were skipped (first at 0x{skipped[0]:X})")

    if erase:
        link.erase("ALL")
    plan = []
    for region in devices.REGIONS:
        if not cells[region]:
            continue
        blank = device.blank(region)
        for start, count in _write_runs(device, region, cells[region]):
            data = [cells[region].get(start + i, blank) for i in range(count)]
            plan.append((region, start, data))
    for region, start, data in plan:
        link.write(region, start, data, progress=_named(progress, region))
    if verify:
        verify_cells(link, cells, progress)
    return warnings


def verify_cells(link, cells, progress=None):
    """Compare the target with {region: {offset: value}}. Only the given cells are checked:
    the blank padding written around them is not, since without an erase those cells may
    hold older data that the write left alone."""
    for region in devices.REGIONS:
        for start, count in devices.runs(cells[region]):
            data = [cells[region][start + i] for i in range(count)]
            link.verify(region, start, data, progress=_named(progress, region))
