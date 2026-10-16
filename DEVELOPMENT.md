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
| Keyboard handling state logic | Implemented, self-tested | Navigation, make-code handling, break-code filtering and Escape return |
| PS/2 keyboard polling | Implemented, end-to-end QEMU test | QEMU HMP injects 2, A and Escape; CI checks page transition and scan-code marker |
| Multiboot memory-map parsing | Implemented, smoke-tested | Parses variable-sized entries and frees only complete pages covered by type-1 RAM ranges |
| Physical page allocator | Implemented, smoke-tested | Two bitmaps track used and allocated frames below 4 GiB; 4096-byte pages; free count and double-free protection |
| Memory-region reservation | Implemented, smoke-tested | Reserves low 1 MiB, kernel image including BSS, Multiboot info/map, boot modules and framebuffer pages |
| Physical-memory allocator self-test | Implemented, smoke-tested | Allocates a frame, verifies alignment/read/write/count restoration and rejects double-free |
| IDT setup | Implemented, smoke-tested | Boot installs 32 exception gates; lidt/sidt base and limit check emits a marker required by CI |
| CPU exception stubs and fatal report | Implemented, not yet runtime-verified | Assembly stubs account for vectors with/without CPU error codes; handler reports vector, error code, EIP, CS and EFLAGS then halts. CI does not deliberately trigger a fault yet |
| Hardware IRQ/PIC handling | Not implemented | IF remains disabled; no IRQ remapping, EOI handling, timer IRQ or keyboard IRQ |
| Paging / virtual memory | Not implemented | Allocator still returns directly addressable physical addresses; page tables and virtual address spaces are absent |
| Mouse input | Not implemented | No mouse packet decoder or pointer yet |
| Persistent disk/filesystem | Not implemented | No block driver, filesystem or persistent user files yet |
| Userspace and system-call API | Not implemented | The UI currently runs in kernel context |
| External app loading/install/remove | Not implemented | No executable loader, app ABI or package format yet |
| File manager/editor/monitor/installer apps | Not implemented | These depend on persistent storage and a stable application interface |

## Source layout

- src/boot.S — Multiboot header, stack setup, IDT initialization call and C entry transfer.
- src/multiboot.h — shared Multiboot info and memory-map structures.
- src/memory.h and src/memory.c — physical frame allocator and allocator self-test.
- src/interrupts.h, src/interrupts.c and src/interrupts.S — IDT gates, exception stubs and fatal serial diagnostics.
- src/kernel.c — serial diagnostics, one-bit canvas, monochrome UI pages, keyboard state handler and polled PS/2 input.
- linker.ld — kernel image layout and exported kernel bounds.
- grub/grub.cfg — video mode, serial diagnostics and boot entry.
- Makefile — kernel/ISO build, tool checks, run and test targets.
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

GitHub Actions run [38902485702](https://github.com/DarnoPort/Michael-OC/actions/runs/38902485702) passed for commit 3e98a755e18600be627ce5fae77bc645f4eb5364. CI compiled the new assembly/C interrupt module, checked the Multiboot header, built the ISO, booted QEMU and required MICHAEL_OC_IDT_OK alongside the existing kernel, allocator, graphics and input markers. The existing HMP keyboard interaction test also passed. This verifies IDT initialization and continued boot/UI behaviour; it does not independently force an exception to execute the fatal-report branch.

The physical allocator remains a pre-paging allocator: it tracks RAM below 4 GiB in a bitmap in kernel BSS and assumes a single-core boot environment. Its self-test accesses the allocated physical address directly because paging is not enabled.

## Next tasks

1. Add CPU exception self-tests in a separate test boot mode, then add PIC remapping and hardware IRQ dispatch with explicit EOI handling.
2. Implement paging and virtual memory, including page-table creation, mapping/unmapping, and tests for invalid accesses and allocator reservations.
3. Add a QEMU block device and persistent filesystem; test create/write/read/delete and data survival across a VM restart.
4. Define a kernel-to-app ABI, an executable format/loader and a safe external-app installation/removal mechanism.
5. Implement real file manager, editor, system monitor and installer applications using that interface.
6. Add mouse support and evolve the current full-screen diagnostic UI into a proper desktop/window manager.

## Verification rules

A source file existing is not proof it runs. Distinguish boot, allocator self-test, IDT initialization, deliberate exception injection, keyboard input, persistence across restart and full feature completion. Do not mark a feature smoke-tested without a relevant passing test.
