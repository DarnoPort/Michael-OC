# Michael OC development log

## Project baseline

- Date: 2026-10-16
- The GitHub repository was initially empty; the project started from a new bootable-kernel baseline.
- Target: Ubuntu Linux + QEMU, 32-bit x86 guest, GRUB Multiboot v1.
- Toolchain: freestanding C, GNU assembler/binutils, GRUB rescue ISO, xorriso, mtools, QEMU, and Python 3 for the QEMU monitor test.
- A later architecture change is allowed if necessary; avoid redesigning the working boot path without a concrete reason.

## Status key

- **Implemented, smoke-tested**: exercised by an automated runtime test.
- **Implemented, not yet runtime-verified**: code exists, but a working system run has not confirmed the behaviour.
- **Partial**: the foundation works but the full feature is incomplete.
- **Not implemented**: work remains.

## Current status

| Component | Status | Evidence / limitation |
|---|---|---|
| Freestanding x86 kernel entry | Implemented, smoke-tested | GRUB enters kernel_main; serial marker checked by make test |
| Multiboot v1 header | Implemented, smoke-tested | grub-file check passes |
| Reproducible bootable ISO | Implemented, smoke-tested | make builds build/michael-oc.iso in CI |
| Kernel linker layout | Implemented, smoke-tested | Separate RX and RW PT_LOAD segments |
| Serial diagnostics | Implemented, smoke-tested | COM1 output is used by automated tests and diagnosis |
| Packed one-bit canvas | Implemented, smoke-tested | 800x600 canvas, one bit per pixel; set/clear self-test required by CI |
| Monochrome framebuffer rendering | Implemented, smoke-tested | GRUB mode 800x600x32; only black and full RGB white are emitted |
| Keyboard-driven diagnostic pages | Implemented, smoke-tested | Home, system-status, keyboard-test and framebuffer-test screens |
| Keyboard handling state logic | Implemented, self-tested | Tests navigation, key make code, break-code filtering, and Escape return |
| PS/2 keyboard polling | Implemented, end-to-end QEMU test added | QEMU HMP injects 2, A, Escape; CI checks page change and scan-code marker |
| Mouse input | Not implemented | No mouse packet decoder or pointer yet |
| Full desktop/window manager | Not implemented | Current UI uses full-screen diagnostic pages, not movable/resizable windows |
| Physical memory allocator/paging | Not implemented | No allocator or virtual memory management yet |
| Interrupt/exception framework | Not implemented | Keyboard input polls the controller with interrupts disabled |
| Persistent disk/filesystem | Not implemented | No block driver, filesystem, or persistent user files yet |
| Userspace and system-call API | Not implemented | The UI currently runs in kernel context |
| External app loading/install/remove | Not implemented | No executable loader, app ABI, or package format yet |
| File manager/editor/monitor/installer apps | Not implemented | These depend on storage and a stable application interface |

## Source layout

- src/boot.S — Multiboot header, stack and C entry transfer.
- src/kernel.c — serial diagnostics, one-bit canvas, monochrome UI pages, keyboard state handler and polled PS/2 input.
- linker.ld — kernel image layout with RX and RW load segments.
- grub/grub.cfg — video mode, serial diagnostics and boot entry.
- Makefile — kernel/ISO build, run, tool checks and test targets.
- scripts/test_keyboard_input.py — QEMU HMP key-injection smoke test.
- .github/workflows/build.yml — Ubuntu CI build, QEMU test and ISO artifact upload.
- README.md — setup, build, run, controls and known limitations.

## Build and run

Install on Ubuntu:

    sudo apt update
    sudo apt install build-essential gcc-multilib grub-pc-bin xorriso mtools qemu-system-x86 make python3

From the repository root:

    make clean
    make
    make test
    make run

ISO output: build/michael-oc.iso.

## Verification

The last completed CI revision compiled the initial keyboard UI and passed its original boot/framebuffer smoke test. The current commit strengthens that test with additional input-state markers and a separate QEMU HMP test which injects keys and checks the corresponding serial events. Update this record only after that run completes.

The HMP test covers a real QEMU-generated keyboard event reaching the polling driver, opening the keyboard test page, reporting A's make scancode, and returning home with Escape. It does not test a physical keyboard, mouse, or interrupt-driven input.

## Next tasks

1. Confirm the injected-key QEMU test passes in CI; fix timing or monitor interaction based on logs if needed.
2. Add mouse support and later migrate polling input to a properly initialized interrupt/IDT framework.
3. Implement memory-map handling and a physical page/frame allocator, then paging.
4. Add a block device and persistent filesystem in a QEMU disk image; test that data survives restarts.
5. Define a stable kernel-to-app interface, executable format/loader, and application installation/removal.
6. Implement file manager, editor, system monitor, and installer as real, independently launchable applications.
7. Add a window manager if it is justified by the rest of the architecture.

## Verification rules

A source file existing is not proof it runs. Distinguish startup, logic self-tests, injected input, persistence after reboot, and full feature completion. Do not mark a feature smoke-tested without a relevant passing test.