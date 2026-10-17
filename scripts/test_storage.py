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
    print("PASS: second cold QEMU boot recovered persisted bytes and passed filesystem CRUD checks.")

    for corruption in ("overlapping-extents", "orphan-allocated-sector"):
        corrupt_disk = args.work_dir / f"corrupt-{corruption}.img"
        create_corrupted_disk(args.create_disk_script, corrupt_disk, corruption)
        corrupt_log = run_phase(
            args.qemu, args.iso, corrupt_disk,
            args.work_dir / f"corrupt-{corruption}.log",
        )
        require_mount_rejected(corrupt_log, corruption)
        print(f"PASS: MOCFS1 mount rejected {corruption} metadata.")

    return 0
if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"FAIL: storage test: {exc}", file=sys.stderr)
        raise SystemExit(1)
