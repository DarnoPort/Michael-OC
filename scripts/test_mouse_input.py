#!/usr/bin/env python3
"""End-to-end PS/2 mouse IRQ12 test using QEMU's HMP monitor."""
from __future__ import annotations

import argparse
import socket
import subprocess
import sys
import time
from pathlib import Path


def read_until_prompt(conn: socket.socket, timeout: float = 3.0) -> bytes:
    deadline = time.monotonic() + timeout
    data = bytearray()
    conn.settimeout(0.2)
    while time.monotonic() < deadline:
        try:
            chunk = conn.recv(4096)
        except socket.timeout:
            continue
        if not chunk:
            raise RuntimeError("QEMU monitor disconnected")
        data.extend(chunk)
        if b"(qemu)" in data:
            return bytes(data)
    raise TimeoutError("timed out waiting for QEMU monitor prompt")


def wait_marker(log: Path, marker: str, process: subprocess.Popen, timeout: float = 5.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            contents = log.read_text(errors="replace") if log.exists() else ""
            raise RuntimeError(f"QEMU exited before {marker}\n{contents}")
        contents = log.read_text(errors="replace") if log.exists() else ""
        if marker in contents:
            return
        time.sleep(0.05)
    contents = log.read_text(errors="replace") if log.exists() else ""
    raise TimeoutError(f"timed out waiting for {marker}\nSerial log:\n{contents}")


def command(conn: socket.socket, text: str) -> bytes:
    conn.sendall(text.encode("ascii") + b"\n")
    response = read_until_prompt(conn)
    if b"unknown command" in response.lower() or b"invalid parameter" in response.lower():
        raise RuntimeError(f"QEMU rejected {text!r}: {response!r}")
    return response


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", required=True)
    parser.add_argument("--iso", required=True)
    parser.add_argument("--disk", required=True)
    parser.add_argument("--work-dir", required=True)
    args = parser.parse_args()
    work = Path(args.work_dir)
    work.mkdir(parents=True, exist_ok=True)
    log = work / "serial-mouse.log"
    monitor_path = work / "qemu-mouse-monitor.sock"
    for path in (log, monitor_path):
        try:
            path.unlink()
        except FileNotFoundError:
            pass

    process = subprocess.Popen([
        args.qemu, "-m", "128M", "-cdrom", args.iso, "-hda", args.disk,
        "-display", "none", "-serial", f"file:{log}",
        "-monitor", f"unix:{monitor_path},server,nowait",
        "-no-reboot", "-no-shutdown",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    monitor = None
    try:
        wait_marker(log, "MICHAEL_OC_MOUSE_IRQ_OK", process, 10.0)
        wait_marker(log, "MICHAEL_OC_KEYBOARD_EVENT_LOOP_OK", process, 3.0)
        deadline = time.monotonic() + 5.0
        while not monitor_path.exists() and time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError("QEMU exited before monitor socket appeared")
            time.sleep(0.05)
        monitor = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        monitor.settimeout(0.5)
        monitor.connect(str(monitor_path))
        read_until_prompt(monitor)

        command(monitor, "mouse_move 40 20")
        wait_marker(log, "MICHAEL_OC_MOUSE_EVENT", process, 3.0)
        command(monitor, "mouse_button 1")
        wait_marker(log, "MICHAEL_OC_MOUSE_CLICK", process, 3.0)
        command(monitor, "mouse_button 0")
        print("PASS: QEMU mouse movement and left-click reached the kernel through PS/2 IRQ12.")
        return 0
    except Exception as exc:
        contents = log.read_text(errors="replace") if log.exists() else ""
        print(f"FAIL: mouse input test: {exc}\nSerial log:\n{contents}", file=sys.stderr)
        return 1
    finally:
        if monitor is not None:
            monitor.close()
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=3.0)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=3.0)
        try:
            monitor_path.unlink()
        except FileNotFoundError:
            pass


if __name__ == "__main__":
    raise SystemExit(main())
