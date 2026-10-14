#!/usr/bin/env python3
"""Development helper placeholder.

Production signing should run offline with private key stored outside repo.
"""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=Path)
    parser.add_argument("--version", type=int, required=True)
    args = parser.parse_args()

    data = args.image.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    print(f"version={args.version}")
    print(f"size={len(data)}")
    print(f"sha256={digest}")
    print("signature=<offline signer output goes here>")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

