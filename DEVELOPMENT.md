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
| Multiboot v1 entry and reproducible ISO build | Verified on earlier commit; latest fix pending CI | The previous green workflow built an ISO and started QEMU; this revision must rebuild |
| Bootstrap GDT and CPU exception IDT | Verified on earlier commit | A separate QEMU INT3 test checked fatal exception reporting |
| One-bit canvas / framebuffer renderer | Verified on earlier commit | 800×600 one-bit software backing buffer; output uses black or full RGB white |
| PS/2 keyboard IRQ1 | Verified on earlier commit | QEMU monitor injection test checked UI navigation and scan-code delivery |
| Physical page allocator | Verified on earlier commit | Parses usable RAM map and self-tests allocation/free; paging is absent |
| ATA PIO disk driver | Verified by earlier storage test | Primary-master IDE, LBA28, bounded status polling, and cache flush |
| MOCFS1 persistent filesystem | Verified by earlier storage test | Two-cold-boot persistence and CRUD test; fixed metadata table and contiguous extents |
| File manager and text editor | Verified on earlier commit | UI test created/saved a file and reopened it after restart |
| MOCApp on-disk interpreter | Implemented, latest CI pending | Bounded text-script interpreter with PRINT, CLEAR, WAITKEY, IFKEY, JUMP, SET/ADD/SUB, PRINTNUM, LS, READ, WRITE, and EXIT |
| MOCApp file-manager workflow | Implemented, latest CI pending | .mapp launches from the file manager; F1 stops it and Escape returns |
| Paging and virtual memory | Not implemented | The allocator returns directly addressable physical addresses |
| Userspace isolation, syscall ABI, native ELF loader | Not implemented | UI and script interpreter run in kernel context; no protected user processes |
| Timer and mouse drivers | Not implemented | Only keyboard IRQ1 has an active device handler |
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

## Latest CI diagnosis

Workflow run [38911241812](https://github.com/DarnoPort/Michael-OC/actions/runs/38911241812) failed during compilation. GCC reported an invalid storage class for editor_insert_byte followed by end-of-file parse errors. Inspection found a missing closure and misplaced file-dialog input/navigation code in ui_handle_file_manager_scancode. Commit cc12793573443af3822f242644cef3184bdb18c5 restores the previously working dialog and navigation flow while preserving the MOCApp launch branch. The follow-up CI run is the required verification of this fix.

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

## Paging implementation (latest change pending CI)

A 4 KiB page-table implementation has been added. It identity-maps the 32-bit address space to preserve the current direct-physical-pointer convention, unmaps the null page, makes the linked kernel text/rodata region read-only, enables CR0.WP, and marks the Multiboot framebuffer with write-through/cache-disable flags. The boot self-test verifies page-zero absence, kernel text permissions, writable kernel data, temporary virtual-to-physical remapping, unmapping, translation failure after unmap, identity-map restoration, and no leaked physical frame.

This is not per-process virtual memory or userspace isolation. The initial mapping remains a single global supervisor address space, with most addresses writable. Latest CI must pass before this milestone is marked verified.

A dedicated page-fault diagnostic kernel now attempts a write to a kernel text address after enabling paging. Its test requires #PF vector 14, error code 3 (present-page write protection violation from supervisor mode), and CR2 matching the exact target address. This moves protection checking from inspecting PTE bits alone to observing CPU-enforced behavior; the test must pass in CI before marked verified.

## Mouse input (latest change pending CI)

Added a PS/2 auxiliary-port initialization sequence, IRQ12 packet handling, packet resynchronization, signed delta decoding, bounded event queue, monochrome cursor rendering, and click dispatch to the existing keyboard-driven UI actions. Mouse initialization is optional on a machine without an auxiliary device; in that case boot continues with keyboard input only. QEMU testing now verifies movement and a click that navigates from the home screen into the file manager. In this kernel-resident prototype, pointer events and click handling execute in ring 0, as does the rest of the UI.

## PIT timer (latest change pending CI)

PIT channel 0 is configured for approximately 100 Hz. IRQ0 increments a monotonic 32-bit tick counter, the boot test requires both timer setup and observed runtime ticks, and the system status screen reports whole-second tick-based uptime. The PIC mask now enables IRQ0 alongside IRQ1 and, when available, the IRQ2 cascade for mouse IRQ12. This does not yet implement preemptive multitasking or blocking sleep primitives.
