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
V1_DATA_START = 17
DATA_START = 34
DIRECTORY_ENTRIES = 64
ENTRY_SIZE = 64
MAGIC = b"MOCFS1\x00\x00"

def create_image(path: Path, version: int = 2) -> None:
    if version not in (1, 2):
        raise ValueError("MOCFS1 filesystem version must be 1 or 2")
    data_start = V1_DATA_START if version == 1 else DATA_START
    path.parent.mkdir(parents=True, exist_ok=True)
    app_source = (
        b"PRINT MICHAEL OC SCRIPT APP\n"
        b"PRINT LOADED FROM DISK\n"
        b"SET 40\n"
        b"ADD 2\n"
        b"PRINTNUM\n"
        b"WRITE mocapp.txt DATA FROM SCRIPT\n"
        b"READ mocapp.txt\n"
        b"LS\n"
        b"WAITKEY\n"
        b"IFKEY a 12\n"
        b"PRINT WRONG KEY\n"
        b"JUMP 12\n"
        b"PRINT KEY RECEIVED\n"
        b"EXIT\n"
    )
    with path.open("wb") as image:
        image.truncate(TOTAL_SECTORS * SECTOR_SIZE)
        superblock = struct.pack(
            "<8sIIIIIIIII", MAGIC, version, TOTAL_SECTORS, BITMAP_START,
            BITMAP_SECTORS, DIRECTORY_START, DIRECTORY_SECTORS,
            data_start, DIRECTORY_ENTRIES, ENTRY_SIZE)
        image.seek(0)
        image.write(superblock)

        bitmap = bytearray(BITMAP_SECTORS * SECTOR_SIZE)
        for sector in range(data_start):
            bitmap[sector >> 3] |= 1 << (sector & 7)
        bitmap[data_start >> 3] |= 1 << (data_start & 7)
        image.seek(BITMAP_START * SECTOR_SIZE)
        image.write(bitmap)

        directory = bytearray(DIRECTORY_SECTORS * SECTOR_SIZE)
        entry = struct.pack(
            "<32sHBBIIH18s", b"welcome.mapp", 0xFFFF, 1, 0,
            data_start, len(app_source), 1, bytes(18))
        directory[:ENTRY_SIZE] = entry
        image.seek(DIRECTORY_START * SECTOR_SIZE)
        image.write(directory)
        image.seek(data_start * SECTOR_SIZE)
        image.write(app_source.ljust(SECTOR_SIZE, b"\x00"))

def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--version", type=int, choices=(1, 2), default=2,
                        help="filesystem format version (default: 2 with metadata journal)")
    args = parser.parse_args()
    create_image(args.output, version=args.version)
    data_start = V1_DATA_START if args.version == 1 else DATA_START
    print(f"Created {args.output} ({IMAGE_MIB} MiB, {TOTAL_SECTORS} sectors, MOCFS1 v{args.version}, data starts at sector {data_start}; seeded welcome.mapp)")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
