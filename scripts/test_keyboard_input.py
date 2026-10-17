#!/usr/bin/env python3
"""End-to-end PS/2 input test using QEMU's HMP monitor."""

from __future__ import annotations

import argparse
import socket
import subprocess
import sys
import time
from pathlib import Path


def read_until_prompt(conn: socket.socket, timeout: float = 3.0) -> bytes:
    end = time.monotonic() + timeout
    data = bytearray()
    conn.settimeout(0.2)
    while time.monotonic() < end:
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


def wait_for_marker(log_path: Path, marker: str, process: subprocess.Popen, timeout: float) -> None:
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if process.poll() is not None:
            log = log_path.read_text(errors="replace") if log_path.exists() else "(serial log missing)"
            raise RuntimeError(f"QEMU exited before marker {marker}.\n{log}")
        log = log_path.read_text(errors="replace") if log_path.exists() else ""
        if marker in log:
            return
        time.sleep(0.05)
    log = log_path.read_text(errors="replace") if log_path.exists() else "(serial log missing)"
    raise TimeoutError(f"timed out waiting for serial marker {marker}.\nSerial log:\n{log}")


def send_command(conn: socket.socket, command: str) -> bytes:
    conn.sendall(command.encode("ascii") + b"\n")
    response = read_until_prompt(conn)
    lowered = response.lower()
    if b"unknown command" in lowered or b"invalid parameter" in lowered:
        raise RuntimeError(f"QEMU monitor rejected command {command!r}: {response!r}")
    return response


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", required=True)
    parser.add_argument("--iso", required=True)
    parser.add_argument("--disk", required=True)
    parser.add_argument("--work-dir", required=True)
    args = parser.parse_args()

    work_dir = Path(args.work_dir)
    work_dir.mkdir(parents=True, exist_ok=True)
    serial_log = work_dir / "serial-keyboard.log"
    monitor_path = work_dir / "qemu-monitor.sock"
    for path in (serial_log, monitor_path):
        try:
            path.unlink()
        except FileNotFoundError:
            pass

    command = [
        args.qemu,
        "-m", "128M",
        "-cdrom", args.iso,
        "-hda", args.disk,
        "-display", "none",
        "-serial", f"file:{serial_log}",
        "-monitor", f"unix:{monitor_path},server,nowait",
        "-no-reboot",
        "-no-shutdown",
    ]

    process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    monitor = None
    try:
        wait_for_marker(serial_log, "MICHAEL_OC_KEYBOARD_EVENT_LOOP_OK", process, 10.0)

        deadline = time.monotonic() + 5.0
        while not monitor_path.exists() and time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError("QEMU exited before its monitor socket appeared")
            time.sleep(0.05)

        monitor = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        monitor.settimeout(0.5)
        monitor.connect(str(monitor_path))
        read_until_prompt(monitor)

        send_command(monitor, "sendkey 2")
        wait_for_marker(serial_log, "MICHAEL_OC_UI_SCREEN=KEYBOARD", process, 3.0)

        send_command(monitor, "sendkey a")
        wait_for_marker(serial_log, "MICHAEL_OC_UI_KEY_SCANCODE=0x0000001E", process, 3.0)

        send_command(monitor, "sendkey esc")
        wait_for_marker(serial_log, "MICHAEL_OC_UI_SCREEN=HOME", process, 3.0)

        send_command(monitor, "sendkey 6")
        wait_for_marker(serial_log, "MICHAEL_OC_UI_SCREEN=CONSOLE", process, 3.0)
        send_command(monitor, "sendkey h")
        send_command(monitor, "sendkey e")
        send_command(monitor, "sendkey l")
        send_command(monitor, "sendkey p")
        send_command(monitor, "sendkey ret")
        wait_for_marker(serial_log, "MICHAEL_OC_CONSOLE_COMMAND=help", process, 3.0)
        wait_for_marker(serial_log, "MICHAEL_OC_CONSOLE_COMMANDS_OK", process, 3.0)

        send_command(monitor, "sendkey m")
        send_command(monitor, "sendkey e")
        send_command(monitor, "sendkey m")
        send_command(monitor, "sendkey ret")
        wait_for_marker(serial_log, "MICHAEL_OC_CONSOLE_MEMORY_OK", process, 3.0)

        send_command(monitor, "sendkey f")
        send_command(monitor, "sendkey s")
        send_command(monitor, "sendkey ret")
        wait_for_marker(serial_log, "MICHAEL_OC_CONSOLE_FS_OK", process, 3.0)

        send_command(monitor, "sendkey u")
        send_command(monitor, "sendkey p")
        send_command(monitor, "sendkey t")
        send_command(monitor, "sendkey i")
        send_command(monitor, "sendkey m")
        send_command(monitor, "sendkey e")
        send_command(monitor, "sendkey ret")
        wait_for_marker(serial_log, "MICHAEL_OC_CONSOLE_UPTIME_OK", process, 3.0)

        send_command(monitor, "sendkey up")
        send_command(monitor, "sendkey end")
        send_command(monitor, "sendkey esc")
        wait_for_marker(serial_log, "MICHAEL_OC_UI_SCREEN=HOME", process, 3.0)

        print("PASS: keyboard IRQ navigated the UI, executed console help/mem/fs/uptime commands, scrolled history, and exited home.")
        return 0
    except Exception as exc:
        print(f"FAIL: keyboard input test: {exc}", file=sys.stderr)
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
