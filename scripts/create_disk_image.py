#!/usr/bin/env python3
"""Create a fresh 16 MiB Michael OC FS v1 disk image."""
from __future__ import annotations
import argparse
import struct
from pathlib import Path
SECTOR_SIZE = 512
IMAGE_MIB = 16
TOTAL_SECTORS = IMAGE_MIB * 1024 * 1024 // SECTOR_SIZE
BITMAP_START = 1
BITMAP_SECTORS = 8
DIRECTORY_START = 9
DIRECTORY_SECTORS = 8
DATA_START = 17
DIRECTORY_ENTRIES = 64
ENTRY_SIZE = 64
MAGIC = b"MOCFS1\x00\x00"
def create_image(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb") as image:
        image.truncate(TOTAL_SECTORS * SECTOR_SIZE)
        superblock = bytearray(SECTOR_SIZE)
        superblock[:44] = struct.pack(
            "<8sIIIIIIIII", MAGIC, 1, TOTAL_SECTORS, BITMAP_START,
            BITMAP_SECTORS, DIRECTORY_START, DIRECTORY_SECTORS,
            DATA_START, DIRECTORY_ENTRIES, ENTRY_SIZE)
        image.seek(0)
        image.write(superblock)
        bitmap = bytearray(BITMAP_SECTORS * SECTOR_SIZE)
        for sector in range(DATA_START):
            bitmap[sector >> 3] |= 1 << (sector & 7)
        image.seek(BITMAP_START * SECTOR_SIZE)
        image.write(bitmap)
        image.seek(DIRECTORY_START * SECTOR_SIZE)
        image.write(bytes(DIRECTORY_SECTORS * SECTOR_SIZE))
def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    create_image(args.output)
    print(f"Created {args.output} ({IMAGE_MIB} MiB, {TOTAL_SECTORS} sectors, MOCFS1)")
    return 0
if __name__ == "__main__":
    raise SystemExit(main())
