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


def last_position(log: Path) -> tuple[int, int] | None:
    import re
    contents = log.read_text(errors="replace") if log.exists() else ""
    matches = re.findall(
        r"MICHAEL_OC_MOUSE_POSITION=(0x[0-9A-Fa-f]{8}),(0x[0-9A-Fa-f]{8})",
        contents,
    )
    if not matches:
        return None
    x_text, y_text = matches[-1]
    return int(x_text, 16), int(y_text, 16)


def wait_position_change(log: Path, process: subprocess.Popen,
                         previous: tuple[int, int], timeout: float = 3.0) -> tuple[int, int]:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError("QEMU exited while waiting for mouse movement")
        current = last_position(log)
        if current is not None and current != previous:
            return current
        time.sleep(0.03)
    raise TimeoutError(f"mouse position did not change from {previous}")


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
        position = last_position(log)
        if position is None:
            raise RuntimeError("mouse event did not report a cursor position")

        command(monitor, "mouse_button 1")
        wait_marker(log, "MICHAEL_OC_MOUSE_CLICK", process, 3.0)
        command(monitor, "mouse_button 0")

        # Determine the active Y-axis convention from a measured movement rather
        # than assuming whether the monitor reports screen or device coordinates.
        command(monitor, "mouse_move 0 10")
        probed = wait_position_change(log, process, position)
        y_delta = probed[1] - position[1]
        if y_delta == 0:
            command(monitor, "mouse_move 0 -20")
            reversed_probe = wait_position_change(log, process, probed)
            y_delta = reversed_probe[1] - probed[1]
            probed = reversed_probe
        if y_delta == 0:
            raise RuntimeError("QEMU mouse movement did not change the cursor Y coordinate")

        target_y = 193
        direction = 1 if y_delta > 0 else -1
        move_y = target_y - probed[1]
        input_y = move_y * direction
        if input_y < -255 or input_y > 255:
            raise RuntimeError(f"mouse test cannot reach menu row from Y={probed[1]}")
        if input_y != 0:
            previous = probed
            command(monitor, f"mouse_move 0 {input_y}")
            arrived = wait_position_change(log, process, previous)
            # Correct for any controller scaling by using the observed position.
            correction = target_y - arrived[1]
            if correction != 0:
                previous = arrived
                command(monitor, f"mouse_move 0 {correction * direction}")
                arrived = wait_position_change(log, process, previous)
            probed = arrived

        # X stays near 440 (inside the File Manager item); the click must navigate.
        command(monitor, "mouse_button 1")
        wait_marker(log, "MICHAEL_OC_UI_SCREEN=FILES", process, 3.0)
        command(monitor, "mouse_button 0")
        print("PASS: QEMU mouse movement, left-click, and desktop navigation reached the kernel through PS/2 IRQ12.")
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
