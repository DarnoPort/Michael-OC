#!/usr/bin/env python3
"""QEMU integration test for IDT installation and CPU exception dispatch."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", required=True)
    parser.add_argument("--iso", required=True)
    parser.add_argument("--work-dir", required=True)
    args = parser.parse_args()

    work_dir = Path(args.work_dir)
    work_dir.mkdir(parents=True, exist_ok=True)
    serial_log = work_dir / "serial-exception.log"
    try:
        serial_log.unlink()
    except FileNotFoundError:
        pass

    command = [
        args.qemu,
        "-m", "128M",
        "-cdrom", args.iso,
        "-display", "none",
        "-monitor", "none",
        "-serial", f"file:{serial_log}",
        "-no-reboot",
        "-no-shutdown",
    ]

    try:
        result = subprocess.run(
            command,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
            timeout=5.0,
        )
    except subprocess.TimeoutExpired:
        # The test kernel deliberately halts forever after the exception.
        pass
    else:
        log = serial_log.read_text(errors="replace") if serial_log.exists() else ""
        raise RuntimeError(
            "QEMU exited before the expected fatal-exception halt "
            f"(return code {result.returncode}).\nSerial log:\n{log}"
        )

    log = serial_log.read_text(errors="replace") if serial_log.exists() else ""
    required = (
        "MICHAEL_OC_BOOT_OK",
        "MICHAEL_OC_IDT_OK",
        "MICHAEL_OC_EXCEPTION_SELF_TEST_TRIGGER",
        "MICHAEL_OC_EXCEPTION_VECTOR=0x00000003 ERROR=0x00000000",
    )
    missing = [marker for marker in required if marker not in log]
    if missing:
        print(
            "FAIL: missing exception test marker(s): "
            + ", ".join(missing)
            + "\nSerial log:\n"
            + log,
            file=sys.stderr,
        )
        return 1

    if "MICHAEL_OC_IDT_FAIL" in log or "MICHAEL_OC_EXCEPTION_TEST_FAILED" in log:
        print("FAIL: IDT verification or INT3 dispatch failed.\n" + log, file=sys.stderr)
        return 1

    print("PASS: IDT installation and CPU exception dispatch (INT3 vector/error code).")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"FAIL: exception test: {exc}", file=sys.stderr)
        raise SystemExit(1)
