"""Decode Configuration Words into named fields (data/pic_config_bits.csv).

The table is transcribed from each family's programming specification (Appendix B). A device
with no rows in the table can still be read and written; it just cannot be decoded.
"""
import csv
import fnmatch
import os

from .devices import HERE, ROOT


def _table_path():
    packaged = os.path.join(HERE, "data", "pic_config_bits.csv")
    return packaged if os.path.exists(packaged) else os.path.join(ROOT, "data", "pic_config_bits.csv")


TABLE_PATH = _table_path()


class Field:
    def __init__(self, row):
        self.devices = row["devices"].split("|")
        self.word = int(row["word"])  # 1-based, CONFIGn
        self.hi = int(row["hi"])
        self.lo = int(row["lo"])
        self.name = row["name"]
        self.description = row["description"]
        self.values = _entries(row["values"])
        self.warns = _entries(row["warn"])

    def extract(self, word):
        return (word >> self.lo) & ((1 << (self.hi - self.lo + 1)) - 1)

    def lookup(self, entries, value):
        bits = format(value, f"0{self.hi - self.lo + 1}b")
        for key, text in entries:
            if _matches(key, bits):
                return text
        return None


def _entries(text):
    return [tuple(item.split("=", 1)) for item in text.split("|")] if text else []


def _matches(key, bits):
    if "-" in key:
        low, high = key.split("-")
        return low <= bits <= high
    return all(k in ("x", b) for k, b in zip(key, bits))


def load(path=TABLE_PATH):
    with open(path, newline="") as f:
        lines = (line for line in f if not line.startswith("#") and line.strip())
        return [Field(row) for row in csv.DictReader(lines)]


def fields_for(device, path=TABLE_PATH):
    """The decodable fields of `device`'s Configuration Words, in word and bit order."""
    found = [fld for fld in load(path) if any(fnmatch.fnmatch(device.name, g) for g in fld.devices)]
    return sorted(found, key=lambda fld: (fld.word, -fld.hi))


def decode(device, words):
    """Decode `words` (a list, CONFIG1 first; None for a word that is not known).

    Returns (lines, warnings): one printable line per field and a message for each field
    holding a value the specification warns about.
    """
    lines, warnings = [], []
    fields = fields_for(device)
    for number in sorted({fld.word for fld in fields}):
        word = words[number - 1] if number <= len(words) else None
        if word is None:
            lines.append(f"CONFIG{number}: not present")
            continue
        lines.append(f"CONFIG{number} = 0x{word:04X}")
        for fld in (x for x in fields if x.word == number):
            value = fld.extract(word)
            width = fld.hi - fld.lo + 1
            meaning = fld.lookup(fld.values, value) or fld.description
            bits = f"{fld.hi}" if width == 1 else f"{fld.hi}:{fld.lo}"
            lines.append(f"  {fld.name:<10s} [{bits:>4s}] = {value:0{width}b}  {meaning}")
            warning = fld.lookup(fld.warns, value)
            if warning:
                warnings.append(f"CONFIG{number} {fld.name}: {warning}")
    return lines, warnings
