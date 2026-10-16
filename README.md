# Michael OC

Michael OC is an independent operating-system project targeting QEMU on Ubuntu Linux. Its defining UI requirement is a strict one-bit palette: the software canvas stores one bit per pixel and the framebuffer renderer emits only black or full RGB white. This restriction applies to graphics, not to CPU instructions, memory, disk data, or application logic.

## Current implementation

- Bootable 32-bit x86 kernel using GRUB Multiboot v1 and an owned flat GDT.
- IDT exception handlers for CPU vectors 0–31, PIC routing, and interrupt-driven PS/2 keyboard input through IRQ1.
- Reproducible ISO build and automated QEMU integration tests.
- 800×600 packed one-bit canvas and monochrome framebuffer output.
- Keyboard-operated system status, keyboard diagnostics, framebuffer test, file manager, and text editor.
- ATA PIO sector I/O to a QEMU IDE disk, plus MOCFS1 persistent storage with nested directories and file/directory create, list, rename, read, write, and delete operations.
- File editor with cursor navigation, insertion/deletion, F2 save, and QEMU integration coverage across restarts.
- MOCApp: a disk-loaded text-script runtime supporting output, key input, branches, an integer register, and basic file-system operations.
- Fresh virtual-disk image seeded with a runnable welcome.mapp demonstration.
- Integration test targets for boot markers, keyboard input, exception dispatch, file-system persistence, and UI-driven script execution.

**Michael OC is still an early-stage OS, not a completed general-purpose operating system.** The UI and MOCApp interpreter currently run in kernel context; applications do not have isolated address spaces or protected user mode. Paging/virtual memory, a system-call ABI, native ELF execution, process scheduling, timer and mouse drivers, and filesystem journaling remain unimplemented. The filesystem and interpreter are deliberately small and have documented limitations.

## Build requirements (Ubuntu)

    sudo apt update
    sudo apt install build-essential gcc-multilib grub-pc-bin xorriso mtools qemu-system-x86 make python3

## Build and run

From the repository root:

    make
    make run

The build creates build/michael-oc.iso. The first make run creates a 16 MiB disk at build/michael-oc-disk.img. Keep that disk image if you want files to persist across QEMU restarts.

Home-screen shortcuts: 1 opens system status, 2 keyboard diagnostics, 3 the framebuffer test, 4 the file manager, and 5 the welcome document in the editor. Press Escape to return to the home screen except when a script is running, where Escape returns to the file manager.

In the file manager, use the arrow keys to select entries, Enter to open files or directories, Backspace to go to the parent directory, N to create a file, D to create a directory, R to rename, X to delete with confirmation, and F2 to refresh. The text editor uses arrows to navigate, Backspace/Delete to edit, Enter for newline, and F2 to save.

## Creating and running an app

A .mapp file is an ordinary text file stored on the virtual disk. Create one in the file manager using N, enter its source in the editor, save with F2, return to the file manager, select the script, and press Enter. Files with the .mapp suffix are interpreted as MOCApp programs; other files open in the editor.

The fresh-disk image includes welcome.mapp, which demonstrates printing, integer arithmetic, writing and reading a file, listing a directory, waiting for a key, and conditional branching. See docs/MOCAPP.md for the language reference and more examples.

Scripts are limited to 16 KiB and 256 source lines. MOCApp is interpreted by the kernel, not native machine code; it has no user-mode isolation and must not be treated as a sandbox for untrusted programs. F1 stops a script; Escape returns to the file manager.

## Tests

    make test

The automated suite compiles the kernel, verifies its Multiboot header, boots QEMU headlessly, checks serial startup markers, injects keyboard events through QEMU's monitor, tests exception dispatch using a separately compiled diagnostic kernel, verifies persistent filesystem CRUD across two cold boots, and exercises file-manager/editor/MOCApp workflows. The current run for the latest fix must pass before that change can be described as fully verified.

## Known limitations and next priorities

- No paging, virtual address spaces, user-mode separation, or process scheduler.
- The interpreter operates in kernel context and supports only its documented instructions.
- MOCFS1 is a custom, fixed-table filesystem with contiguous file extents and no journal or crash recovery.
- No mouse pointer or timer interrupt driver.
- The UI tools are kernel-resident screens rather than separate protected processes.

Next priorities are tested paging and safe memory mappings, user-mode execution and a stable system-call ABI, filesystem consistency/recovery, and mouse/timer support. Only label a feature as verified when its corresponding test actually passes.
