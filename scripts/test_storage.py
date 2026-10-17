#!/usr/bin/env python3
"""Two-cold-boot test for Michael OC disk persistence and filesystem operations."""
from __future__ import annotations
import argparse
import struct
import subprocess
import sys
from pathlib import Path
WRITE_MARKER = "MICHAEL_OC_STORAGE_PHASE=WRITE"
VERIFY_MARKER = "MICHAEL_OC_STORAGE_PHASE=VERIFY"
CRUD_MARKER = "MICHAEL_OC_STORAGE_CRUD_OK"
def run_phase(qemu: str, iso: Path, disk: Path, log_path: Path) -> str:
    try:
        log_path.unlink()
    except FileNotFoundError:
        pass
    command = [qemu, "-m", "128M", "-cdrom", str(iso), "-hda", str(disk),
               "-display", "none", "-monitor", "none", "-serial", f"file:{log_path}",
               "-no-reboot", "-no-shutdown"]
    try:
        process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except OSError as exc:
        raise RuntimeError(f"could not start QEMU: {exc}") from exc
    timed_out = False
    try:
        process.wait(timeout=4.0)
    except subprocess.TimeoutExpired:
        timed_out = True
        process.kill()
        process.wait(timeout=3.0)
    log = log_path.read_text(errors="replace") if log_path.exists() else ""
    if not timed_out:
        raise RuntimeError(f"QEMU exited before the expected test halt (code {process.returncode}).\n{log}")
    return log
def require(log: str, markers: tuple[str, ...], phase: str) -> None:
    missing = [marker for marker in markers if marker not in log]
    if missing:
        raise RuntimeError(f"{phase}: missing marker(s) {missing}\nSerial log:\n{log}")
    failures = [line for line in log.splitlines() if "MICHAEL_OC_STORAGE_TEST_FAIL" in line]
    if failures:
        raise RuntimeError(f"{phase}: kernel reported failure: {failures}\n{log}")
def create_corrupted_disk(create_script: Path, disk: Path, corruption: str) -> None:
    subprocess.run(
        [sys.executable, str(create_script), "--output", str(disk)],
        check=True,
        stdout=subprocess.DEVNULL,
    )
    image = bytearray(disk.read_bytes())
    header = struct.unpack_from("<8sIIIIIIIII", image, 0)
    bitmap_start = header[3]
    directory_start = header[5]
    data_start = header[7]
    entry_count = header[8]
    entry_size = header[9]

    if corruption == "overlapping-extents":
        for index in range(entry_count):
            offset = directory_start * 512 + index * entry_size
            entry = struct.unpack_from("<32sHBBIIH18s", image, offset)
            if entry[2] == 0:
                # Point a second file at welcome.mapp's existing data sector.
                struct.pack_into(
                    "<32sHBBIIH18s", image, offset,
                    b"overlap.txt".ljust(32, b"\x00"),
                    0xFFFF, 1, 0, data_start, 1, 1, bytes(18),
                )
                break
        else:
            raise RuntimeError("fresh image has no free directory entry")
    elif corruption == "orphan-allocated-sector":
        # Mark a free data sector allocated without giving it a directory owner.
        sector = data_start + 1
        byte_offset = bitmap_start * 512 + (sector >> 3)
        image[byte_offset] |= 1 << (sector & 7)
    else:
        raise ValueError(f"unknown corruption case: {corruption}")

    disk.write_bytes(image)


def journal_checksum(payload: bytes) -> int:
    checksum = 2166136261
    for byte in payload:
        checksum = ((checksum ^ byte) * 16777619) & 0xFFFFFFFF
    return checksum


def create_pending_journal_disk(create_script: Path, disk: Path, kind: str) -> None:
    """Build a v2 image whose metadata transaction committed but was not replayed."""
    subprocess.run(
        [sys.executable, str(create_script), "--output", str(disk)],
        check=True,
        stdout=subprocess.DEVNULL,
    )
    image = bytearray(disk.read_bytes())
    header = struct.unpack_from("<8sIIIIIIIII", image, 0)
    version, bitmap_start, bitmap_sectors = header[1], header[3], header[4]
    directory_start, directory_sectors = header[5], header[6]
    data_start, entry_count, entry_size = header[7], header[8], header[9]
    if version != 2 or data_start != 34 or bitmap_sectors != 8 or directory_sectors != 8:
        raise RuntimeError("journal recovery fixtures require the v2 disk geometry")

    directory_offset = directory_start * 512
    directory_payload = bytearray(
        image[directory_offset:directory_offset + directory_sectors * 512]
    )
    free_slot = None
    for index in range(entry_count):
        entry = struct.unpack_from("<32sHBBIIH18s", directory_payload, index * entry_size)
        if entry[2] == 0:
            free_slot = index
            break
    if free_slot is None:
        raise RuntimeError("fresh disk has no directory entry for the recovery fixture")
    struct.pack_into(
        "<32sHBBIIH18s", directory_payload, free_slot * entry_size,
        b"journal-recovered.txt".ljust(32, b"\x00"),
        0xFFFF, 1, 0, 0, 0, 0, bytes(18),
    )

    if kind in ("directory", "bad-checksum"):
        journal_kind, target_start, shadow_start = 1, directory_start, 18
        payload = bytes(directory_payload)
        # Leave the live directory unchanged: the journal must recreate this entry.
    elif kind == "bitmap":
        # The file entry is already live; replay only the prepared bitmap copy.
        image[directory_offset:directory_offset + len(directory_payload)] = directory_payload
        journal_kind, target_start, shadow_start = 2, bitmap_start, 26
        bitmap_offset = bitmap_start * 512
        payload = bytes(image[bitmap_offset:bitmap_offset + bitmap_sectors * 512])
        # Simulate an interrupted home write after the complete shadow was prepared.
        orphan_sector = data_start + 1
        image[bitmap_offset + (orphan_sector >> 3)] |= 1 << (orphan_sector & 7)
    else:
        raise ValueError(f"unknown pending journal case: {kind}")

    payload_offset = shadow_start * 512
    image[payload_offset:payload_offset + len(payload)] = payload
    checksum = journal_checksum(payload)
    if kind == "bad-checksum":
        image[payload_offset + 17] ^= 0x01
    journal_header = struct.pack(
        "<IIIII", 0x314A4F4D, journal_kind, 8, checksum, target_start
    ).ljust(512, b"\x00")
    image[17 * 512:18 * 512] = journal_header
    disk.write_bytes(image)


def verify_journal_recovery(disk: Path, kind: str) -> None:
    image = disk.read_bytes()
    if struct.unpack_from("<I", image, 17 * 512)[0] != 0:
        raise RuntimeError(f"{kind}: replay left the commit header in place")
    header = struct.unpack_from("<8sIIIIIIIII", image, 0)
    if kind == "directory":
        _, size, count, _ = find_root_file(disk, "journal-recovered.txt")
        if size != 0 or count != 0:
            raise RuntimeError("directory journal replay created an invalid recovery entry")
        home = image[header[5] * 512:(header[5] + 8) * 512]
        shadow = image[18 * 512:26 * 512]
        if home != shadow:
            raise RuntimeError("directory journal replay did not reproduce the prepared table")
    elif kind == "bitmap":
        home = image[header[3] * 512:(header[3] + 8) * 512]
        shadow = image[26 * 512:34 * 512]
        if home != shadow:
            raise RuntimeError("bitmap journal replay did not reproduce the prepared bitmap")
        sector = header[7] + 1
        if home[sector >> 3] & (1 << (sector & 7)):
            raise RuntimeError("bitmap journal replay did not clear the simulated stray bit")


def find_root_file(image_path: Path, wanted_name: str) -> tuple[int, int, int, int]:
    image = image_path.read_bytes()
    header = struct.unpack_from("<8sIIIIIIIII", image, 0)
    directory_offset = header[5] * 512
    entry_count = header[8]
    entry_size = header[9]
    for index in range(entry_count):
        entry = struct.unpack_from("<32sHBBIIH18s", image, directory_offset + index * entry_size)
        name = entry[0].split(b"\x00", 1)[0].decode("ascii", errors="replace")
        if entry[2] == 1 and entry[1] == 0xFFFF and name == wanted_name:
            return (entry[4], entry[5], entry[6], header[7])
    raise RuntimeError(f"root file not found in disk image: {wanted_name}")


def require_mount_rejected(log: str, corruption: str) -> None:
    expected = "MICHAEL_OC_STORAGE_TEST_FAIL=MOUNT"
    if expected not in log:
        raise RuntimeError(
            f"{corruption}: expected mount rejection marker {expected!r}\\nSerial log:\\n{log}"
        )
    if "MICHAEL_OC_STORAGE_FS_OK" in log:
        raise RuntimeError(f"{corruption}: corrupt filesystem was reported mounted\\n{log}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", required=True)
    parser.add_argument("--iso", required=True, type=Path)
    parser.add_argument("--disk", required=True, type=Path)
    parser.add_argument("--work-dir", required=True, type=Path)
    parser.add_argument("--create-disk-script", required=True, type=Path)
    args = parser.parse_args()
    args.work_dir.mkdir(parents=True, exist_ok=True)
    subprocess.run([sys.executable, str(args.create_disk_script), "--output", str(args.disk)], check=True)
    write_log = run_phase(args.qemu, args.iso, args.disk, args.work_dir / "storage-write.log")
    require(write_log, ("MICHAEL_OC_BOOT_OK", "MICHAEL_OC_STORAGE_FS_OK", WRITE_MARKER), "first boot")
    print("PASS: first QEMU boot created and reread a file on the virtual disk.")
    verify_log = run_phase(args.qemu, args.iso, args.disk, args.work_dir / "storage-verify.log")
    require(verify_log, ("MICHAEL_OC_BOOT_OK", "MICHAEL_OC_STORAGE_FS_OK", VERIFY_MARKER, CRUD_MARKER), "second boot")
    require(verify_log, ("MICHAEL_OC_STORAGE_MOVE_OK",), "move and directory-cycle tests")
    require(verify_log, ("MICHAEL_OC_STORAGE_COPY_OK",), "file copy and multi-sector tests")
    require(verify_log, ("MICHAEL_OC_STORAGE_COW_WRITE_OK",), "copy-on-write replacement")
    print("PASS: filesystem moved/copied files and replaced a file using a distinct extent.")

    moved_log = run_phase(
        args.qemu, args.iso, args.disk,
        args.work_dir / "storage-move-verify.log",
    )
    require(
        moved_log,
        ("MICHAEL_OC_BOOT_OK", "MICHAEL_OC_STORAGE_FS_OK",
         VERIFY_MARKER, "MICHAEL_OC_STORAGE_MOVE_PERSIST_OK",
         "MICHAEL_OC_STORAGE_COPY_PERSIST_OK",
         "MICHAEL_OC_STORAGE_COW_PERSIST_OK"),
        "move persistence after cold reboot",
    )
    print("PASS: move/copy content and the copy-on-write replacement survived a cold QEMU reboot.")

    corrupt_disk = args.work_dir / "corrupt-overlapping-extents.img"
    create_corrupted_disk(args.create_disk_script, corrupt_disk, "overlapping-extents")
    corrupt_log = run_phase(
        args.qemu, args.iso, corrupt_disk,
        args.work_dir / "corrupt-overlapping-extents.log",
    )
    require_mount_rejected(corrupt_log, "overlapping-extents")
    print("PASS: MOCFS1 mount rejected overlapping file extents.")

    # A stray allocated bit is recoverable: the directory provides the source
    # of truth for extents, so mounting rebuilds the bitmap and reuses the freed sector.
    orphan_disk = args.work_dir / "corrupt-orphan-allocated-sector.img"
    create_corrupted_disk(args.create_disk_script, orphan_disk, "orphan-allocated-sector")
    orphan_log = run_phase(
        args.qemu, args.iso, orphan_disk,
        args.work_dir / "orphan-recovery-write.log",
    )
    require(
        orphan_log,
        ("MICHAEL_OC_BOOT_OK", "MICHAEL_OC_STORAGE_FS_OK",
         "MICHAEL_OC_FS_BITMAP_REPAIRED", WRITE_MARKER),
        "orphan bitmap recovery",
    )
    start_sector, size_bytes, sector_count, data_start = find_root_file(orphan_disk, "ci-persist.txt")
    if start_sector != data_start + 1 or size_bytes == 0 or sector_count != 1:
        raise RuntimeError(
            "orphan recovery did not release the stray sector for the new file: "
            f"start={start_sector}, size={size_bytes}, count={sector_count}, data_start={data_start}"
        )
    print("PASS: MOCFS1 rebuilt an orphaned allocation bit and reused the recovered sector.")

    recovered_log = run_phase(
        args.qemu, args.iso, orphan_disk,
        args.work_dir / "orphan-recovery-verify.log",
    )
    require(
        recovered_log,
        ("MICHAEL_OC_BOOT_OK", "MICHAEL_OC_STORAGE_FS_OK",
         VERIFY_MARKER, CRUD_MARKER),
        "orphan recovery persistence",
    )
    print("PASS: recovered filesystem remained valid and retained the new file after a cold reboot.")

    # Simulate a crash immediately after the journal commit record reached disk.
    for kind in ("directory", "bitmap"):
        journal_disk = args.work_dir / f"pending-journal-{kind}.img"
        create_pending_journal_disk(args.create_disk_script, journal_disk, kind)
        journal_log = run_phase(
            args.qemu, args.iso, journal_disk,
            args.work_dir / f"pending-journal-{kind}.log",
        )
        require(
            journal_log,
            ("MICHAEL_OC_BOOT_OK", "MICHAEL_OC_STORAGE_FS_OK",
             "MICHAEL_OC_STORAGE_JOURNAL_REPLAY_OK"),
            f"{kind} journal replay",
        )
        verify_journal_recovery(journal_disk, kind)
        print(f"PASS: v2 {kind} journal transaction replayed and the commit header was cleared.")

    # A bad committed payload must fail closed rather than mount ambiguous metadata.
    bad_journal_disk = args.work_dir / "pending-journal-bad-checksum.img"
    create_pending_journal_disk(args.create_disk_script, bad_journal_disk, "bad-checksum")
    bad_journal_log = run_phase(
        args.qemu, args.iso, bad_journal_disk,
        args.work_dir / "pending-journal-bad-checksum.log",
    )
    require_mount_rejected(bad_journal_log, "journal-checksum")
    print("PASS: v2 mount rejected a journal whose shadow checksum did not match.")

    # Format v1 keeps the legacy sector-17 data layout and is still writable.
    legacy_disk = args.work_dir / "legacy-v1-storage.img"
    subprocess.run(
        [sys.executable, str(args.create_disk_script), "--output", str(legacy_disk),
         "--version", "1"],
        check=True, stdout=subprocess.DEVNULL,
    )
    legacy_log = run_phase(
        args.qemu, args.iso, legacy_disk,
        args.work_dir / "legacy-v1-storage.log",
    )
    require(
        legacy_log,
        ("MICHAEL_OC_BOOT_OK", "MICHAEL_OC_STORAGE_FS_OK", WRITE_MARKER),
        "legacy v1 mount/write",
    )
    legacy_header = struct.unpack_from("<8sIIIIIIIII", legacy_disk.read_bytes(), 0)
    if legacy_header[1] != 1 or legacy_header[7] != 17:
        raise RuntimeError("legacy v1 compatibility fixture was unexpectedly upgraded")
    print("PASS: legacy MOCFS1 v1 mounted and accepted persistent writes.")

    return 0
if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"FAIL: storage test: {exc}", file=sys.stderr)
        raise SystemExit(1)
