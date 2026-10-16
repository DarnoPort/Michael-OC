#!/usr/bin/env python3
"""Verify that paging actually prevents writes to the kernel text mapping."""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", required=True)
    parser.add_argument("--iso", required=True, type=Path)
    parser.add_argument("--work-dir", required=True, type=Path)
    args = parser.parse_args()

    args.work_dir.mkdir(parents=True, exist_ok=True)
    log_path = args.work_dir / "serial-page-fault.log"
    try:
        log_path.unlink()
    except FileNotFoundError:
        pass

    command = [
        args.qemu, "-m", "128M", "-cdrom", str(args.iso),
        "-display", "none", "-monitor", "none",
        "-serial", f"file:{log_path}", "-no-reboot", "-no-shutdown",
    ]
    try:
        result = subprocess.run(
            command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            check=False, timeout=5.0,
        )
    except subprocess.TimeoutExpired:
        pass  # Expected: the exception handler halts after reporting #PF.
    else:
        log = log_path.read_text(errors="replace") if log_path.exists() else ""
        raise RuntimeError(f"QEMU exited before expected protection fault (code {result.returncode}).\n{log}")

    log = log_path.read_text(errors="replace") if log_path.exists() else ""
    required = (
        "MICHAEL_OC_PAGING_OK",
        "MICHAEL_OC_PAGING_SELF_TEST_OK",
        "MICHAEL_OC_PAGE_FAULT_SELF_TEST_TRIGGER",
        "MICHAEL_OC_EXCEPTION_VECTOR=0x0000000E ERROR=0x00000003",
    )
    missing = [marker for marker in required if marker not in log]
    if missing:
        print("FAIL: missing page-fault test marker(s): " + ", ".join(missing) + "\n" + log, file=sys.stderr)
        return 1

    target_match = re.search(r"MICHAEL_OC_PROTECTED_WRITE_TARGET=(0x[0-9A-Fa-f]{8})", log)
    cr2_match = re.search(r"MICHAEL_OC_PAGE_FAULT_CR2=(0x[0-9A-Fa-f]{8})", log)
    if not target_match or not cr2_match or target_match.group(1).lower() != cr2_match.group(1).lower():
        print("FAIL: CR2 did not match the address written by the self-test.\n" + log, file=sys.stderr)
        return 1
    if "MICHAEL_OC_PAGE_FAULT_TEST_FAILED" in log:
        print("FAIL: write to protected kernel text unexpectedly succeeded.\n" + log, file=sys.stderr)
        return 1

    print("PASS: x86 write-protect enforced on kernel text; page-fault error bits and CR2 match the faulting address.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"FAIL: page-fault test: {exc}", file=sys.stderr)
        raise SystemExit(1)
