#!/usr/bin/env python3
"""Two-cold-boot test for Michael OC disk persistence and filesystem operations."""
from __future__ import annotations
import argparse
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
    return 0
if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"FAIL: storage test: {exc}", file=sys.stderr)
        raise SystemExit(1)
