#!/usr/bin/env python3
"""Install a MOCApp text script into a stopped Michael OC MOCFS1 disk image."""
from __future__ import annotations

import argparse
import os
import re
import stat
import struct
import sys
import tempfile
from pathlib import Path

SECTOR_SIZE = 512
MAGIC = b"MOCFS1\x00\x00"
VERSION_V1 = 1
VERSION_V2 = 2
SUPPORTED_VERSIONS = (VERSION_V1, VERSION_V2)
BITMAP_START = 1
BITMAP_SECTORS = 8
DIRECTORY_START = 9
DIRECTORY_SECTORS = 8
V1_DATA_START = 17
V2_DATA_START = 34
JOURNAL_HEADER_SECTOR = 17
DIRECTORY_JOURNAL_START = 18
BITMAP_JOURNAL_START = 26
JOURNAL_MAGIC = 0x314A4F4D
JOURNAL_DIRECTORY = 1
JOURNAL_BITMAP = 2
DIRECTORY_ENTRIES = 64
ENTRY_SIZE = 64
MAX_SECTORS = BITMAP_SECTORS * SECTOR_SIZE * 8
MAX_APP_BYTES = 16 * 1024
MAX_APP_LINES = 256
FS_ROOT = 0xFFFF
FS_ENTRY_FILE = 1
FS_ENTRY_DIRECTORY = 2
SUPER_FMT = "<8sIIIIIIIII"
ENTRY_FMT = "<32sHBBIIH18s"
VALID_NAME = re.compile(r"^[A-Za-z0-9._ -]+$")


def check_name(name: str) -> bytes:
    try:
        encoded = name.encode("ascii")
    except UnicodeEncodeError as exc:
        raise ValueError("MOCFS1 names must use ASCII characters") from exc
    if not encoded or len(encoded) > 31 or name in (".", ".."):
        raise ValueError("file name must be 1..31 bytes and not '.' or '..'")
    if VALID_NAME.fullmatch(name) is None:
        raise ValueError("file name may contain only ASCII letters, digits, dot, underscore, hyphen, and space")
    if not name.lower().endswith(".mapp"):
        raise ValueError("MOCApp files must have the .mapp extension")
    return encoded


def validate_program(payload: bytes) -> None:
    if len(payload) > MAX_APP_BYTES:
        raise ValueError(f"script is {len(payload)} bytes; runtime limit is {MAX_APP_BYTES}")
    if b"\x00" in payload:
        raise ValueError("script cannot contain NUL bytes")
    if any(byte not in (9, 10, 13) and not 32 <= byte <= 126 for byte in payload):
        raise ValueError("script must use printable ASCII plus tab and newline characters")
    if payload:
        lines = 1 + sum(
            1 for index, byte in enumerate(payload)
            if byte == 10 and index + 1 < len(payload)
        )
        if lines > MAX_APP_LINES:
            raise ValueError(f"script has {lines} source lines; runtime limit is {MAX_APP_LINES}")


def bitmap_used(bitmap: bytes, sector: int) -> bool:
    return bool(bitmap[sector >> 3] & (1 << (sector & 7)))


def journal_checksum(payload: bytes) -> int:
    checksum = 2166136261
    for byte in payload:
        checksum = ((checksum ^ byte) * 16777619) & 0xFFFFFFFF
    return checksum


def recover_pending_journal(image: bytearray, version: int) -> bool:
    """Replay a committed v2 metadata transaction before the host edits the image."""
    if version == VERSION_V1:
        return False
    header_offset = JOURNAL_HEADER_SECTOR * SECTOR_SIZE
    magic, kind, sector_count, checksum, target_start = struct.unpack_from(
        "<IIIII", image, header_offset
    )
    if magic == 0:
        return False
    if magic != JOURNAL_MAGIC or sector_count != DIRECTORY_SECTORS:
        raise ValueError("MOCFS1 v2 journal header is invalid; refusing to modify the image")
    if kind == JOURNAL_DIRECTORY:
        expected_target, shadow_start = DIRECTORY_START, DIRECTORY_JOURNAL_START
    elif kind == JOURNAL_BITMAP:
        expected_target, shadow_start = BITMAP_START, BITMAP_JOURNAL_START
    else:
        raise ValueError("MOCFS1 journal has an unknown metadata transaction type")
    if target_start != expected_target:
        raise ValueError("MOCFS1 journal target does not match its transaction type")

    payload_offset = shadow_start * SECTOR_SIZE
    payload_size = sector_count * SECTOR_SIZE
    payload = bytes(image[payload_offset:payload_offset + payload_size])
    if len(payload) != payload_size or journal_checksum(payload) != checksum:
        raise ValueError("MOCFS1 journal payload checksum failed; refusing to modify the image")

    target_offset = target_start * SECTOR_SIZE
    image[target_offset:target_offset + payload_size] = payload
    image[header_offset:header_offset + SECTOR_SIZE] = bytes(SECTOR_SIZE)
    return True


def validate_image(image: bytearray):
    if len(image) < (V1_DATA_START + 1) * SECTOR_SIZE or len(image) % SECTOR_SIZE:
        raise ValueError("disk image size is not a valid MOCFS1 sector-aligned image")

    total_sectors = len(image) // SECTOR_SIZE
    (
        magic, version, sb_total, bitmap_start, bitmap_sectors,
        directory_start, directory_sectors, data_start, entry_count, entry_size,
    ) = struct.unpack_from(SUPER_FMT, image, 0)

    if magic != MAGIC or version not in SUPPORTED_VERSIONS:
        raise ValueError("disk image does not contain a supported MOCFS1 superblock")
    expected_data_start = V1_DATA_START if version == VERSION_V1 else V2_DATA_START
    if sb_total != total_sectors or total_sectors > MAX_SECTORS:
        raise ValueError("MOCFS1 sector count does not match the image or exceeds bitmap capacity")
    geometry = (
        bitmap_start, bitmap_sectors, directory_start, directory_sectors,
        data_start, entry_count, entry_size,
    )
    expected_geometry = (
        BITMAP_START, BITMAP_SECTORS, DIRECTORY_START, DIRECTORY_SECTORS,
        expected_data_start, DIRECTORY_ENTRIES, ENTRY_SIZE,
    )
    if geometry != expected_geometry:
        raise ValueError("MOCFS1 metadata geometry is unsupported")

    recover_pending_journal(image, version)

    bitmap_offset = BITMAP_START * SECTOR_SIZE
    bitmap = bytearray(image[bitmap_offset:bitmap_offset + BITMAP_SECTORS * SECTOR_SIZE])
    for sector in range(data_start):
        if not bitmap_used(bitmap, sector):
            raise ValueError(f"filesystem metadata sector {sector} is marked free")

    directory_offset = DIRECTORY_START * SECTOR_SIZE
    directory = bytearray(
        image[directory_offset:directory_offset + DIRECTORY_SECTORS * SECTOR_SIZE]
    )
    entries = []
    for index in range(DIRECTORY_ENTRIES):
        (
            name_raw, parent, entry_type, reserved, start_sector,
            size_bytes, sector_count, padding,
        ) = struct.unpack_from(ENTRY_FMT, directory, index * ENTRY_SIZE)
        if entry_type == 0:
            entries.append({
                "name": "", "parent": parent, "type": 0, "start": start_sector,
                "size": size_bytes, "count": sector_count,
            })
            continue
        if entry_type not in (FS_ENTRY_FILE, FS_ENTRY_DIRECTORY):
            raise ValueError(f"directory entry {index} has an unknown type")
        if b"\x00" not in name_raw:
            raise ValueError(f"directory entry {index} has an unterminated name")
        try:
            name = name_raw.split(b"\x00", 1)[0].decode("ascii")
            check_plain_name(name)
        except (UnicodeDecodeError, ValueError) as exc:
            raise ValueError(f"directory entry {index} has an invalid name: {exc}") from exc
        entries.append({
            "name": name, "parent": parent, "type": entry_type, "start": start_sector,
            "size": size_bytes, "count": sector_count,
        })

    names = set()
    owned_sectors = set()
    for index, entry in enumerate(entries):
        if entry["type"] == 0:
            continue

        parent = entry["parent"]
        if parent != FS_ROOT and (
            parent >= DIRECTORY_ENTRIES or entries[parent]["type"] != FS_ENTRY_DIRECTORY
        ):
            raise ValueError(f"directory entry {index} points to an invalid parent")

        seen = {index}
        ancestor = parent
        for _ in range(DIRECTORY_ENTRIES):
            if ancestor == FS_ROOT:
                break
            if ancestor in seen or ancestor >= DIRECTORY_ENTRIES:
                raise ValueError(f"directory entry {index} participates in a parent cycle")
            seen.add(ancestor)
            ancestor = entries[ancestor]["parent"]
        else:
            raise ValueError(f"directory entry {index} has an excessively deep parent chain")

        name_key = (parent, entry["name"])
        if name_key in names:
            raise ValueError(f"duplicate name in directory: {entry['name']}")
        names.add(name_key)

        if entry["type"] == FS_ENTRY_DIRECTORY:
            if entry["start"] or entry["size"] or entry["count"]:
                raise ValueError(f"directory entry {index} incorrectly owns file data")
            continue

        expected_count = (entry["size"] + SECTOR_SIZE - 1) // SECTOR_SIZE
        if expected_count != entry["count"]:
            raise ValueError(f"file entry {index} has an inconsistent sector count")
        if expected_count == 0:
            if entry["start"] != 0:
                raise ValueError(f"empty file entry {index} has a nonzero start sector")
            continue
        if (
            entry["start"] < data_start or entry["start"] >= total_sectors or
            expected_count > total_sectors - entry["start"]
        ):
            raise ValueError(f"file entry {index} has an invalid data extent")
        for sector in range(entry["start"], entry["start"] + expected_count):
            if not bitmap_used(bitmap, sector):
                raise ValueError(f"file entry {index} points to a free sector")
            if sector in owned_sectors:
                raise ValueError(f"files overlap at sector {sector}")
            owned_sectors.add(sector)

    return total_sectors, bitmap, directory, entries, bitmap_offset, directory_offset


def check_plain_name(name: str) -> None:
    if not name or len(name.encode("ascii")) > 31 or name in (".", ".."):
        raise ValueError("invalid name length")
    if VALID_NAME.fullmatch(name) is None:
        raise ValueError("unsupported character in file name")


def install(disk_path: Path, source_path: Path, name: str | None = None) -> None:
    disk_path = disk_path.resolve()
    source_path = source_path.resolve()
    if disk_path == source_path:
        raise ValueError("source script and disk image must be different files")
    if not disk_path.is_file():
        raise ValueError(f"disk image does not exist: {disk_path}")
    if not source_path.is_file():
        raise ValueError(f"script source does not exist: {source_path}")

    payload = source_path.read_bytes()
    validate_program(payload)
    file_name = name if name is not None else source_path.name
    encoded_name = check_name(file_name)

    original_stat = disk_path.stat()
    image = bytearray(disk_path.read_bytes())
    total_sectors, bitmap, directory, entries, bitmap_offset, directory_offset = validate_image(image)
    _, _, _, _, _, _, _, data_start, _, _ = struct.unpack_from(SUPER_FMT, image, 0)

    for entry in entries:
        if entry["type"] != 0 and entry["parent"] == FS_ROOT and entry["name"] == file_name:
            raise ValueError(f"root entry already exists: {file_name}")

    free_index = next(
        (index for index, entry in enumerate(entries) if entry["type"] == 0),
        None,
    )
    if free_index is None:
        raise ValueError("MOCFS1 directory table is full")

    needed = (len(payload) + SECTOR_SIZE - 1) // SECTOR_SIZE
    start_sector = 0
    if needed:
        run_length = 0
        run_start = data_start
        for sector in range(data_start, total_sectors):
            if not bitmap_used(bitmap, sector):
                if run_length == 0:
                    run_start = sector
                run_length += 1
                if run_length == needed:
                    start_sector = run_start
                    break
            else:
                run_length = 0
        if start_sector == 0:
            raise ValueError(f"not enough contiguous free sectors for {len(payload)} bytes")
        for sector in range(start_sector, start_sector + needed):
            bitmap[sector >> 3] |= 1 << (sector & 7)
        data_offset = start_sector * SECTOR_SIZE
        padded = payload.ljust(needed * SECTOR_SIZE, b"\x00")
        image[data_offset:data_offset + len(padded)] = padded

    struct.pack_into(
        ENTRY_FMT, directory, free_index * ENTRY_SIZE,
        encoded_name.ljust(32, b"\x00"), FS_ROOT, FS_ENTRY_FILE, 0,
        start_sector, len(payload), needed, bytes(18),
    )
    image[bitmap_offset:bitmap_offset + len(bitmap)] = bitmap
    image[directory_offset:directory_offset + len(directory)] = directory

    temp_path = None
    try:
        fd, temp_name = tempfile.mkstemp(
            prefix="." + disk_path.name + ".", suffix=".tmp", dir=str(disk_path.parent)
        )
        temp_path = Path(temp_name)
        with os.fdopen(fd, "wb") as stream:
            stream.write(image)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temp_path, stat.S_IMODE(original_stat.st_mode))
        os.replace(temp_path, disk_path)
        temp_path = None
        try:
            dir_fd = os.open(str(disk_path.parent), os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
            try:
                os.fsync(dir_fd)
            finally:
                os.close(dir_fd)
        except OSError:
            pass
    finally:
        if temp_path is not None:
            try:
                temp_path.unlink()
            except FileNotFoundError:
                pass

    print(
        f"Installed {file_name} ({len(payload)} bytes) in the root directory of {disk_path}. "
        "The kernel ISO was not changed."
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Install an external .mapp script in a stopped Michael OC MOCFS1 disk image."
    )
    parser.add_argument("--disk", required=True, type=Path, help="MOCFS1 disk image, e.g. build/michael-oc-disk.img")
    parser.add_argument("--source", required=True, type=Path, help="external .mapp source file")
    parser.add_argument("--name", help="optional destination name (default: source filename)")
    args = parser.parse_args(argv)
    try:
        install(args.disk, args.source, args.name)
    except (OSError, ValueError, struct.error) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
