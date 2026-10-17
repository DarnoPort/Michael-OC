#!/usr/bin/env python3
"""QEMU integration test for file manager and text editor workflows."""
from __future__ import annotations
import argparse
import socket
import subprocess
import sys
import time
from pathlib import Path

def read_prompt(conn: socket.socket, timeout: float = 3.0) -> bytes:
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
    raise TimeoutError(f"timed out waiting for {marker}\n{contents}")

def wait_marker_count(log: Path, marker: str, expected: int, process: subprocess.Popen,
                      timeout: float = 5.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            contents = log.read_text(errors="replace") if log.exists() else ""
            raise RuntimeError(f"QEMU exited before {marker} occurred {expected} times\n{contents}")
        contents = log.read_text(errors="replace") if log.exists() else ""
        if contents.count(marker) >= expected:
            return
        time.sleep(0.05)
    contents = log.read_text(errors="replace") if log.exists() else ""
    raise TimeoutError(f"timed out waiting for {marker} occurrence {expected}\n{contents}")


def stop_qemu(process: subprocess.Popen, monitor: socket.socket | None) -> None:
    if monitor is not None:
        monitor.close()
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=3.0)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=3.0)

def boot(qemu: str, iso: Path, disk: Path, log: Path, monitor_path: Path):
    for path in (log, monitor_path):
        try:
            path.unlink()
        except FileNotFoundError:
            pass
    command = [
        qemu, "-m", "128M", "-cdrom", str(iso), "-hda", str(disk),
        "-display", "none", "-serial", f"file:{log}",
        "-monitor", f"unix:{monitor_path},server,nowait",
        "-no-reboot", "-no-shutdown",
    ]
    process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    monitor = None
    try:
        wait_marker(log, "MICHAEL_OC_KEYBOARD_EVENT_LOOP_OK", process, 10.0)
        deadline = time.monotonic() + 5.0
        while not monitor_path.exists() and time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError("QEMU exited before monitor socket appeared")
            time.sleep(0.05)
        monitor = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        monitor.settimeout(0.5)
        monitor.connect(str(monitor_path))
        read_prompt(monitor)
        return process, monitor
    except Exception:
        stop_qemu(process, monitor)
        raise

def send_key(monitor: socket.socket, key: str) -> None:
    monitor.sendall(f"sendkey {key}\n".encode("ascii"))
    response = read_prompt(monitor)
    if b"unknown command" in response.lower() or b"invalid parameter" in response.lower():
        raise RuntimeError(f"QEMU rejected sendkey {key}: {response!r}")
    time.sleep(0.07)

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
    host_app = args.work_dir / "host-installed.mapp"
    host_app.write_text("PRINT HOST INSTALL WORKS\nEXIT\n", encoding="ascii")
    subprocess.run(
        [sys.executable, str(Path(__file__).with_name("install_mocapp.py")),
         "--disk", str(args.disk), "--source", str(host_app)],
        check=True,
    )

    log = args.work_dir / "serial-apps.log"
    monitor_path = args.work_dir / "qemu-apps-monitor.sock"
    process, monitor = boot(args.qemu, args.iso, args.disk, log, monitor_path)
    try:
        send_key(monitor, "4")
        wait_marker(log, "MICHAEL_OC_UI_SCREEN=FILES", process)
        send_key(monitor, "n")
        wait_marker(log, "MICHAEL_OC_FILE_DIALOG=NEW_FILE", process)
        for key in ("d", "e", "m", "o"):
            send_key(monitor, key)
        send_key(monitor, "ret")
        wait_marker(log, "MICHAEL_OC_UI_SCREEN=EDITOR", process)
        send_key(monitor, "h")
        send_key(monitor, "i")
        send_key(monitor, "f2")
        wait_marker(log, "MICHAEL_OC_EDITOR_SAVED", process)
        wait_marker(log, "MICHAEL_OC_EDITOR_SIZE=0x00000002", process)
        send_key(monitor, "esc")
        wait_marker(log, "MICHAEL_OC_UI_SCREEN=FILES", process)
        stop_qemu(process, monitor)
        monitor = None

        process, monitor = boot(args.qemu, args.iso, args.disk, log, monitor_path)
        send_key(monitor, "4")
        wait_marker(log, "MICHAEL_OC_UI_SCREEN=FILES", process)
        send_key(monitor, "ret")
        wait_marker(log, "MICHAEL_OC_APP_STARTED=welcome.mapp", process)
        wait_marker(log, "MICHAEL_OC_APP_PRINT=42", process)
        wait_marker(log, "MICHAEL_OC_APP_PRINT=WRITE OK", process)
        wait_marker(log, "MICHAEL_OC_APP_PRINT=DATA FROM SCRIPT", process)
        wait_marker(log, "MICHAEL_OC_APP_PRINT=mocapp.txt", process)
        wait_marker(log, "MICHAEL_OC_APP_WAITKEY", process)
        send_key(monitor, "a")
        wait_marker(log, "MICHAEL_OC_APP_KEY=a", process)
        wait_marker(log, "MICHAEL_OC_APP_PRINT=KEY RECEIVED", process)
        wait_marker(log, "MICHAEL_OC_APP_EXIT", process)
        wait_marker(log, "MICHAEL_OC_UI_SCREEN=FILES", process)

        # The second root-level app was installed into the disk image on the host,
        # after the ISO was built. Launching it proves no kernel rebuild is needed.
        send_key(monitor, "down")
        send_key(monitor, "ret")
        wait_marker(log, "MICHAEL_OC_APP_STARTED=host-installed.mapp", process)
        wait_marker(log, "MICHAEL_OC_APP_PRINT=HOST INSTALL WORKS", process)
        wait_marker_count(log, "MICHAEL_OC_APP_EXIT", 2, process)

        # The file manager's next entry is the document created during boot one.
        send_key(monitor, "down")
        send_key(monitor, "ret")
        wait_marker(log, "MICHAEL_OC_UI_SCREEN=EDITOR", process)
        wait_marker(log, "MICHAEL_OC_EDITOR_OPENED=demo SIZE=0x00000002", process)
        print("PASS: file manager/editor persisted a document, MOCApp executed a disk-resident script, and a fresh boot reopened the document.")
        return 0
    except Exception as exc:
        contents = log.read_text(errors="replace") if log.exists() else ""
        print(f"FAIL: file application test: {exc}\nSerial log:\n{contents}", file=sys.stderr)
        return 1
    finally:
        stop_qemu(process, monitor)
        try:
            monitor_path.unlink()
        except FileNotFoundError:
            pass

if __name__ == "__main__":
    raise SystemExit(main())
