# Michael OC development log

## Baseline

- Date: 2026-10-16.
- Target: Ubuntu Linux + QEMU, 32-bit x86 guest, GRUB Multiboot v1.
- Toolchain: freestanding C, GNU assembler/binutils, GRUB rescue ISO, xorriso, mtools, QEMU, and Python 3.
- Preserve the current Multiboot boot route unless tests demonstrate a concrete reason to change it.

## Status key

- **Verified**: the corresponding automated runtime/integration test passed.
- **Implemented, pending verification**: source exists but the latest changes have not passed runtime tests yet.
- **Partial**: a feature works at a limited scope.
- **Not implemented**: work remains.

## Current status

| Component | Status | Evidence / limitation |
|---|---|---|
| Multiboot v1 entry and reproducible ISO build | Verified | CI run 38915742572 built the ISO, verified the Multiboot header, and booted the guest |
| Bootstrap GDT and CPU exception IDT | Verified | Latest CI passed the INT3 exception-dispatch test |
| One-bit canvas / framebuffer renderer | Verified | QEMU boot check confirms the 800×600 packed one-bit canvas and framebuffer output |
| PS/2 keyboard IRQ1 | Verified | Latest QEMU keyboard test exercises key delivery, navigation, and the debug console |
| Physical page allocator | Verified | RAM-map parsing and allocation/free self-test; paging now also enabled |
| ATA PIO disk driver | Verified | QEMU persistence and filesystem integration tests use the primary-master IDE device |
| MOCFS1 persistent filesystem | Verified | Latest CI passed two-cold-boot persistence and CRUD; fixed metadata table and contiguous extents remain limitations |
| File manager and text editor | Verified | Latest CI passed file-manager/editor writes and reopened persisted content after reboot |
| MOCApp on-disk interpreter | Verified by integration test | Latest CI exercised PRINT, WAITKEY, IFKEY/JUMP, SET/ADD/PRINTNUM and filesystem LS/READ/WRITE |
| MOCApp file-manager workflow | Verified by integration test | Disk-resident script launch and exit path passed QEMU app test; host installer has separate checks |
| Paging and virtual memory | Partial, verified | 4 KiB global supervisor identity map, absent null page, CR0.WP and read-only kernel text; no per-process address spaces |
| Userspace isolation, syscall ABI, native ELF loader | Not implemented | UI and script interpreter run in kernel context; no protected user processes |
| PIT timer and PS/2 mouse | Verified foundation | PIT IRQ0 at about 100 Hz; PS/2 IRQ12 movement/button and mouse-driven menu navigation passed QEMU test |
| Filesystem journaling/recovery | Not implemented | Metadata can become inconsistent if power-off occurs during an update |

## Source layout

- src/boot.S — Multiboot header, stack, and flat GDT setup.
- src/multiboot.h — Multiboot structures.
- src/memory.c and src/memory.h — physical frame allocator.
- src/disk.c and src/disk.h — ATA PIO disk I/O.
- src/fs.c and src/fs.h — MOCFS1 filesystem.
- src/interrupts.c, src/interrupts.h, and src/isr_stubs.S — exceptions, PIC routing, and keyboard IRQ queue.
- src/kernel.c — one-bit UI, file manager/editor, MOCApp parser and interpreter.
- scripts/create_disk_image.py — creates a fresh MOCFS1 image and seeds welcome.mapp.
- scripts/test_keyboard_input.py — QEMU keyboard injection.
- scripts/test_exceptions.py — dedicated exception test kernel.
- scripts/test_storage.py — persistence and CRUD test over two cold boots.
- scripts/test_apps.py — editor persistence and MOCApp integration workflow.
- docs/MOCAPP.md — script format and developer/user guide.
- Makefile and .github/workflows/build.yml — reproducible build and CI.

## Build and run

Install the Ubuntu dependencies:

    sudo apt update
    sudo apt install build-essential gcc-multilib grub-pc-bin xorriso mtools qemu-system-x86 make python3

Then run from the repository root:

    make clean
    make
    make test
    make run

The ISO is build/michael-oc.iso. Persistent user files are stored in build/michael-oc-disk.img.

## Latest CI status

The latest complete workflow, [38915742572](https://github.com/DarnoPort/Michael-OC/actions/runs/38915742572), passed all steps, including ISO build, Multiboot header, QEMU boot, console navigation, mouse click navigation, exception dispatch, enforced kernel text protection, file persistence/CRUD, editor/MOCApp execution, and the host-side installer. Earlier failures on intermediate commits were fixed; do not treat those old run results as the current status.

## Next tasks

1. Pass the complete make test workflow after the file-manager handler fix.
2. Expand MOCApp tests to cover arithmetic, WRITE/READ and LS (added to the current test scenario; awaiting CI).
3. Add paging and virtual memory, including tests for mapping/unmapping and reserved-region protection.
4. Design ring-3 execution and a stable syscall ABI before implementing native ELF loading.
5. Improve filesystem crash consistency, possibly with journaling or copy-on-write metadata.
6. Add timer and mouse support.

## Verification policy

A source file or successful compile is not proof of runtime correctness. Track build, boot, keyboard input, exception dispatch, storage persistence, editor workflow, and script execution separately. Do not mark features verified until the relevant tests pass.

## External MOCApp installation

A host-side installer now places a .mapp source file into the root of an existing MOCFS1 disk image without rebuilding the kernel ISO. It validates filesystem geometry, metadata relationships, extents, file-name rules, and the interpreter's size/line constraints. The operation edits a temporary copy and replaces the image only after a complete successful write. A standalone test checks the on-disk entry/data, duplicate-name rejection, oversized-source rejection, and unchanged image bytes after rejected operations. The QEMU app workflow installs another app after the ISO has already been built and then launches it from the guest file manager. This milestone remains pending until the latest CI run passes.

## Paging implementation (verified by QEMU tests)

A 4 KiB page-table implementation has been added. It identity-maps the 32-bit address space to preserve the current direct-physical-pointer convention, unmaps the null page, makes the linked kernel text/rodata region read-only, enables CR0.WP, and marks the Multiboot framebuffer with write-through/cache-disable flags. The boot self-test verifies page-zero absence, kernel text permissions, writable kernel data, temporary virtual-to-physical remapping, unmapping, translation failure after unmap, identity-map restoration, and no leaked physical frame.

This is not per-process virtual memory or userspace isolation. The initial mapping remains a single global supervisor address space, with most addresses writable. Latest CI passed, including a dedicated page-fault test that exercises CPU enforcement.

A dedicated page-fault diagnostic kernel attempts a write to a kernel text address after enabling paging. CI confirms #PF vector 14, error code 3 (present-page write protection violation from supervisor mode), and CR2 matching the exact target address.

## Mouse input (verified by QEMU tests)

Added a PS/2 auxiliary-port initialization sequence, IRQ12 packet handling, packet resynchronization, signed delta decoding, bounded event queue, monochrome cursor rendering, and click dispatch to the existing keyboard-driven UI actions. Mouse initialization is optional on a machine without an auxiliary device; in that case boot continues with keyboard input only. QEMU testing now verifies movement and a click that navigates from the home screen into the file manager. In this kernel-resident prototype, pointer events and click handling execute in ring 0, as does the rest of the UI.

## PIT timer (verified by QEMU boot tests)

PIT channel 0 is configured for approximately 100 Hz. IRQ0 increments a monotonic 32-bit tick counter, the boot test requires both timer setup and observed runtime ticks, and the system status screen reports whole-second tick-based uptime. The PIC mask now enables IRQ0 alongside IRQ1 and, when available, the IRQ2 cascade for mouse IRQ12. This does not yet implement preemptive multitasking or blocking sleep primitives.

## Kernel debug console (latest change pending CI)

A bounded 64-line ring captures printable messages written by the kernel's `serial_write` path. The monochrome console renders 17 lines at a time and provides Up/Down scrolling, R to return to the latest logs, and Escape to return home. The home screen exposes it as shortcut 6 and it also supports mouse clicks on the menu entry. The keyboard integration test now opens the console, exercises scrolling/latest navigation, and exits. This is a view of kernel-side serial writes, not an interactive command shell; diagnostics emitted through the standalone serial routines in `interrupts.c` are not captured.

## Debug console (verified in QEMU)

The latest CI test opened the console from home with key 6, sent Up to exercise older-log scrolling, used R to return to the latest entries, and pressed Escape to return home. It uses a 64-line ring of kernel `serial_write` messages with a 17-line one-bit display window. The current console is a log viewer, not an interactive shell.
