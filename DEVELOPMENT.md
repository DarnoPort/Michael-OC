# Michael OC development log

## Project baseline

- Date: 2026-10-16
- The GitHub repository was initially empty; the project started from a new bootable-kernel baseline.
- Target: Ubuntu Linux + QEMU, 32-bit x86 guest, GRUB Multiboot v1.
- Toolchain: freestanding C, GNU assembler/binutils, GRUB rescue ISO, xorriso, mtools, QEMU and Python 3 for the QEMU monitor test.
- The architecture can evolve if necessary, but do not redesign the working boot path without a concrete reason.

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
| Keyboard-driven diagnostic pages | Implemented, smoke-tested | Home, system status, keyboard test and framebuffer test |
| Keyboard handling state logic | Implemented, self-tested | Navigation, make code, break-code filtering and Escape return |
| PS/2 keyboard polling | Implemented, end-to-end QEMU test | QEMU HMP injects 2, A and Escape; CI checks page transition and scan-code marker |
| Multiboot memory-map parsing | Implemented, smoke-tested | Parses variable-sized entries and frees only complete pages covered by type-1 RAM ranges |
| Physical page allocator | Implemented, smoke-tested | Two bitmaps track used and allocated frames below 4 GiB; 4096-byte pages; free count and double-free protection |
| Memory-region reservation | Implemented, smoke-tested | Reserves low 1 MiB, kernel image including BSS, Multiboot info/map, boot modules and framebuffer |
| Physical-memory allocator self-test | Implemented, smoke-tested | Allocates one page, checks alignment/count, writes and reads a value, frees and checks count restoration and double-free rejection |
| Paging / virtual memory | Not implemented | The current allocator returns physical addresses and runs before paging |
| Mouse input | Not implemented | No mouse packet decoder or pointer yet |
| CPU exception IDT and fatal handler | Implemented; runtime test pending | Gates for vectors 0–31; separate test kernel triggers `INT3` and checks vector/error-code output. Hardware IRQ routing and recovery are not implemented |
| Persistent disk/filesystem | Not implemented | No block driver, filesystem or persistent user files yet |
| Userspace and system-call API | Not implemented | The UI currently runs in kernel context |
| External app loading/install/remove | Not implemented | No executable loader, app ABI or package format yet |
| File manager/editor/monitor/installer apps | Not implemented | These depend on persistent storage and a stable application interface |

## Source layout

- src/boot.S — Multiboot header, stack, owned flat GDT, and C entry transfer.
- src/multiboot.h — shared Multiboot info and memory-map structures.
- src/memory.h and src/memory.c — physical frame allocator and allocator self-test.
- src/kernel.c — serial diagnostics, one-bit canvas, monochrome UI pages, keyboard state handler and polled PS/2 input.
- src/interrupts.h and src/interrupts.c — IDT setup, descriptor-register verification and fatal CPU exception diagnostics.
- src/isr_stubs.S — 32 exception entry stubs and the common assembly frame builder.
- linker.ld — kernel image layout and exported kernel bounds.
- grub/grub.cfg — video mode, serial diagnostics and boot entry.
- Makefile — kernel/ISO build, run, tool checks, keyboard test, and a separate QEMU exception-dispatch test kernel.
- scripts/test_keyboard_input.py — QEMU HMP key-injection test.
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

## Latest verified milestone

GitHub Actions run [38852152656](https://github.com/DarnoPort/Michael-OC/actions/runs/38852152656) passed at commit 5777a5aafa826e577fa01d1e5b25757706ba1fc4. The pipeline compiled the kernel and memory module, checked the Multiboot header, generated the ISO, booted QEMU, required the memory manager and monochrome UI self-test markers, passed the QEMU HMP key-injection test, and uploaded the ISO artifact.

The allocator is intentionally only a physical frame allocator at this stage. Paging, virtual address spaces and concurrency protection are not implemented. It tracks RAM below 4 GiB using a bitmap in kernel BSS and assumes the current single-core, pre-paging boot environment. The page self-test uses direct physical addressing because paging is not enabled.

## Next tasks

1. Configure and remap the PIC, then add tested hardware IRQ handlers while keeping unhandled IRQs masked.
2. Migrate PS/2 input from polling to interrupt-driven input after IRQ routing is stable.
3. Add paging and a virtual memory model, with tests for page mapping and unmapping.
4. Add a block device and persistent filesystem in a QEMU disk image; test that data survives restarts.
5. Define a stable kernel-to-app interface, executable format/loader, and application installation/removal.
6. Implement file manager, editor, system monitor and installer as real, independently launchable applications.
7. Add mouse support and a proper desktop/window manager after the storage and app interfaces exist.

## Verification rules

A source file existing is not proof it runs. Distinguish boot, allocator self-test, injected input, persistence after reboot and full feature completion. Do not mark a feature smoke-tested without a relevant passing test.
