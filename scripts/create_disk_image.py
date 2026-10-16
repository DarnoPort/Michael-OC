#!/usr/bin/env python3
"""Create a fresh 16 MiB MOCFS1 disk image and seed a sample MOCApp program."""
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
    app_source = (
        b"PRINT MICHAEL OC SCRIPT APP\n"
        b"PRINT LOADED FROM DISK\n"
        b"WAITKEY\n"
        b"IFKEY a 6\n"
        b"PRINT WRONG KEY\n"
        b"JUMP 7\n"
        b"PRINT KEY RECEIVED\n"
        b"EXIT\n"
    )
    with path.open("wb") as image:
        image.truncate(TOTAL_SECTORS * SECTOR_SIZE)
        superblock = struct.pack(
            "<8sIIIIIIIII", MAGIC, 1, TOTAL_SECTORS, BITMAP_START,
            BITMAP_SECTORS, DIRECTORY_START, DIRECTORY_SECTORS,
            DATA_START, DIRECTORY_ENTRIES, ENTRY_SIZE)
        image.seek(0)
        image.write(superblock)

        bitmap = bytearray(BITMAP_SECTORS * SECTOR_SIZE)
        for sector in range(DATA_START):
            bitmap[sector >> 3] |= 1 << (sector & 7)
        bitmap[DATA_START >> 3] |= 1 << (DATA_START & 7)
        image.seek(BITMAP_START * SECTOR_SIZE)
        image.write(bitmap)

        directory = bytearray(DIRECTORY_SECTORS * SECTOR_SIZE)
        entry = struct.pack(
            "<32sHBBIIH18s", b"welcome.mapp", 0xFFFF, 1, 0,
            DATA_START, len(app_source), 1, bytes(18))
        directory[:ENTRY_SIZE] = entry
        image.seek(DIRECTORY_START * SECTOR_SIZE)
        image.write(directory)
        image.seek(DATA_START * SECTOR_SIZE)
        image.write(app_source.ljust(SECTOR_SIZE, b"\x00"))

def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    create_image(args.output)
    print(f"Created {args.output} ({IMAGE_MIB} MiB, {TOTAL_SECTORS} sectors, MOCFS1; seeded welcome.mapp)")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
