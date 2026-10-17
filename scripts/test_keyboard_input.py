#!/usr/bin/env python3
"""End-to-end PS/2 input test using QEMU's HMP monitor."""

from __future__ import annotations

import argparse
import shutil
import socket
import struct
import subprocess
import sys
from typing import List, Optional, Set
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


def prepare_scroll_test_disk(
    source_disk: Path,
    test_disk: Path,
    create_disk_script: Optional[Path],
) -> str:
    if create_disk_script is not None:
        subprocess.run(
            [sys.executable, str(create_disk_script), "--output", str(test_disk)],
            check=True,
            stdout=subprocess.DEVNULL,
        )
    else:
        shutil.copyfile(source_disk, test_disk)

    image = bytearray(test_disk.read_bytes())
    header = struct.unpack_from("<8sIIIIIIIII", image, 0)
    if header[0][:6] != b"MOCFS1":
        raise RuntimeError("scroll test requires a MOCFS1 disk image")

    directory_start = header[5]
    entry_count = header[8]
    entry_size = header[9]
    existing_names: Set[str] = set()
    free_slots: List[int] = []
    for index in range(entry_count):
        offset = directory_start * 512 + index * entry_size
        entry = struct.unpack_from("<32sHBBIIH18s", image, offset)
        if entry[2] == 0:
            free_slots.append(index)
        else:
            name = entry[0].split(b"\x00", 1)[0].decode("ascii", errors="replace")
            if entry[1] == 0xFFFF:
                existing_names.add(name)

    file_count = 20
    if len(free_slots) < file_count:
        raise RuntimeError("not enough free MOCFS1 directory entries for scroll test")

    names: List[str] = []
    serial = 0
    while len(names) < file_count and serial < 1000:
        candidate = f"scroll-{serial:03d}.txt"
        serial += 1
        if candidate not in existing_names:
            names.append(candidate)
            existing_names.add(candidate)
    if len(names) != file_count:
        raise RuntimeError("could not generate unique file names for scroll test")

    for slot, name in zip(free_slots, names):
        offset = directory_start * 512 + slot * entry_size
        struct.pack_into(
            "<32sHBBIIH18s", image, offset,
            name.encode("ascii").ljust(32, b"\x00"),
            0xFFFF, 1, 0, 0, 0, 0, bytes(18),
        )
    test_disk.write_bytes(image)
    return names[-1]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", required=True)
    parser.add_argument("--iso", required=True)
    parser.add_argument("--disk", required=True)
    parser.add_argument("--work-dir", required=True)
    parser.add_argument("--create-disk-script", type=Path)
    args = parser.parse_args()

    work_dir = Path(args.work_dir)
    work_dir.mkdir(parents=True, exist_ok=True)
    test_disk = work_dir / "keyboard-scroll-disk.img"
    last_scroll_file = prepare_scroll_test_disk(
        Path(args.disk), test_disk, args.create_disk_script,
    )
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
        "-hda", str(test_disk),
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

        # The test disk has 21 root entries (the seeded app and 20 empty files).
        # Moving to entry 12 must scroll the first visible entry to index 1.
        send_command(monitor, "sendkey 4")
        wait_for_marker(serial_log, "MICHAEL_OC_UI_SCREEN=FILES", process, 3.0)
        for _ in range(12):
            send_command(monitor, "sendkey down")
        wait_for_marker(serial_log, "MICHAEL_OC_FILE_WINDOW_FIRST=0x00000001", process, 3.0)

        # Continue to the final entry; it must stay visible and open in the editor.
        for _ in range(8):
            send_command(monitor, "sendkey down")
        wait_for_marker(serial_log, "MICHAEL_OC_FILE_WINDOW_FIRST=0x00000009", process, 3.0)
        send_command(monitor, "sendkey ret")
        wait_for_marker(serial_log, f"MICHAEL_OC_EDITOR_OPENED={last_scroll_file}", process, 3.0)
        send_command(monitor, "sendkey esc")
        send_command(monitor, "sendkey esc")

        print("PASS: keyboard IRQ navigated the UI, executed console commands, and scrolled/opened the last file in a 21-entry directory.")
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
