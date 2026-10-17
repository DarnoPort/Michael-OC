#!/usr/bin/env python3
"""Host-side MOCFS1 installer tests."""
from __future__ import annotations

import subprocess
import sys
import struct
import tempfile
from pathlib import Path

from create_disk_image import create_image
from install_mocapp import (
    DIRECTORY_START,
    ENTRY_FMT,
    ENTRY_SIZE,
    FS_ROOT,
    SECTOR_SIZE,
    SUPER_FMT,
    install,
)


def find_root_file(image: bytes, wanted_name: str):
    header = struct.unpack_from(SUPER_FMT, image, 0)
    directory_offset = header[5] * SECTOR_SIZE
    for index in range(header[8]):
        values = struct.unpack_from(ENTRY_FMT, image, directory_offset + index * ENTRY_SIZE)
        raw_name, parent, entry_type, reserved, start, size, count, padding = values
        name = raw_name.split(b"\x00", 1)[0].decode("ascii")
        if entry_type == 1 and parent == FS_ROOT and name == wanted_name:
            return index, start, size, count
    raise AssertionError(f"root file not found: {wanted_name}")


def run_cli(script: Path, disk: Path, source: Path) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, str(Path(__file__).with_name("install_mocapp.py")),
         "--disk", str(disk), "--source", str(source)],
        text=True, capture_output=True, check=False,
    )


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="mocapp-installer-test-") as tmp:
        root = Path(tmp)
        disk = root / "disk.img"
        source = root / "external.mapp"
        payload = (
            b"PRINT INSTALLED FROM HOST\n"
            b"SET 40\n"
            b"ADD 2\n"
            b"PRINTNUM\n"
            b"EXIT\n"
        )
        create_image(disk)
        source.write_bytes(payload)

        result = run_cli(source, disk, source)
        if result.returncode != 0:
            raise AssertionError(f"installer failed: {result.stderr}\n{result.stdout}")

        image = disk.read_bytes()
        header_after_install = struct.unpack_from(SUPER_FMT, image, 0)
        index, start, size, count = find_root_file(image, "external.mapp")
        if size != len(payload) or count != 1 or start < header_after_install[7]:
            raise AssertionError(f"bad installed entry: index={index}, start={start}, size={size}, count={count}")
        recovered = image[start * SECTOR_SIZE:(start + count) * SECTOR_SIZE][:size]
        if recovered != payload:
            raise AssertionError("installed script bytes do not match the source file")

        before = image
        duplicate = run_cli(source, disk, source)
        if duplicate.returncode == 0:
            raise AssertionError("duplicate name unexpectedly succeeded")
        if disk.read_bytes() != before:
            raise AssertionError("failed duplicate installation modified the image")

        # A same-name directory is just as invalid as a same-name file.
        directory_image = root / "directory-collision.img"
        create_image(directory_image)
        image_bytes = bytearray(directory_image.read_bytes())
        header = struct.unpack_from(SUPER_FMT, image_bytes, 0)
        directory_offset = header[5] * SECTOR_SIZE
        directory_count = header[8]
        free_slot = None
        for entry_index in range(directory_count):
            values = struct.unpack_from(ENTRY_FMT, image_bytes, directory_offset + entry_index * ENTRY_SIZE)
            if values[2] == 0:
                free_slot = entry_index
                break
        if free_slot is None:
            raise AssertionError("seed disk unexpectedly has no free directory entry")
        struct.pack_into(
            ENTRY_FMT, image_bytes, directory_offset + free_slot * ENTRY_SIZE,
            b"external.mapp\x00".ljust(32, b"\x00"), FS_ROOT, 2, 0, 0, 0, 0, bytes(18),
        )
        directory_image.write_bytes(image_bytes)
        directory_before = directory_image.read_bytes()
        collision = run_cli(source, directory_image, source)
        if collision.returncode == 0:
            raise AssertionError("file install colliding with a directory unexpectedly succeeded")
        if directory_image.read_bytes() != directory_before:
            raise AssertionError("directory-name collision modified the image")

        oversized = root / "oversized.mapp"
        oversized.write_bytes(b"X" * (16 * 1024 + 1))
        too_large = run_cli(oversized, disk, oversized)
        if too_large.returncode == 0:
            raise AssertionError("oversized script unexpectedly succeeded")
        if disk.read_bytes() != before:
            raise AssertionError("failed oversized installation modified the image")

        wrong_type = root / "not-a-script.txt"
        wrong_type.write_text("PRINT NO\n", encoding="ascii")
        unsupported = run_cli(wrong_type, disk, wrong_type)
        if unsupported.returncode == 0:
            raise AssertionError("non-MAPP file unexpectedly succeeded")
        if disk.read_bytes() != before:
            raise AssertionError("failed unsupported-file installation modified the image")

        # Existing v1 images remain usable and retain their original data offset.
        legacy_disk = root / "legacy-v1.img"
        create_image(legacy_disk, version=1)
        legacy_result = run_cli(source, legacy_disk, source)
        if legacy_result.returncode != 0:
            raise AssertionError(f"installer rejected a legacy v1 image: {legacy_result.stderr}")
        legacy_image = legacy_disk.read_bytes()
        legacy_header = struct.unpack_from(SUPER_FMT, legacy_image, 0)
        legacy_index, legacy_start, legacy_size, legacy_count = find_root_file(
            legacy_image, "external.mapp"
        )
        if (legacy_header[1] != 1 or legacy_start < legacy_header[7] or
                legacy_size != len(payload) or legacy_count != 1):
            raise AssertionError(
                f"legacy v1 compatibility failed: version={legacy_header[1]}, index={legacy_index}, "
                f"start={legacy_start}, data_start={legacy_header[7]}"
            )
        legacy_data = legacy_image[legacy_start * SECTOR_SIZE:
                                   (legacy_start + legacy_count) * SECTOR_SIZE][:legacy_size]
        if legacy_data != payload:
            raise AssertionError("legacy v1 installed script bytes do not match the source")

    print("PASS: external MOCApp install on MOCFS1 v1/v2, metadata/data validation, duplicate rejection, size checks, and failure atomicity.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
