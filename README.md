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

**Michael OC is still an early-stage OS, not a completed general-purpose operating system.** The UI and MOCApp interpreter run in kernel context; applications do not have isolated address spaces or protected user mode. The current 4 KiB paging setup is a single supervisor-only identity map with protected kernel text, not per-process virtual memory. A stable syscall ABI, native ELF loader, and process scheduler are not implemented. PIT timer and PS/2 mouse support are basic; MOCFS1 still lacks journaling and crash recovery.

## Build requirements (Ubuntu)

    sudo apt update
    sudo apt install build-essential gcc-multilib grub-pc-bin xorriso mtools qemu-system-x86 make python3

## Build and run

From the repository root:

    make
    make run

The build creates build/michael-oc.iso. The first make run creates a 16 MiB disk at build/michael-oc-disk.img. Keep that disk image if you want files to persist across QEMU restarts.

Home-screen shortcuts: 1 opens system status, 2 keyboard diagnostics, 3 the framebuffer test, 4 the file manager, and 5 the welcome document in the editor. Press Escape to return to the home screen except when a script is running, where Escape returns to the file manager.

In the file manager, use the arrow keys to select entries, Enter to open files or directories, Backspace to go to the parent directory, N to create a file, D to create a directory, R to rename, M to move, X to delete with confirmation, and F2 to refresh. The file manager shows 12 rows at a time and scrolls automatically as the selection moves through larger directories; the current item counter and scrollbar indicate position. To move an item, press M and enter the destination directory name in the current folder, or type PARENT to move it into the current folder's parent. Moves reject duplicate names and directory cycles. The text editor uses arrows to navigate, Backspace/Delete to edit, Enter for newline, and F2 to save.

## Creating and running an app

A .mapp file is an ordinary text file stored on the virtual disk. Create one in the file manager using N, enter its source in the editor, save with F2, return to the file manager, select the script, and press Enter. Files with the .mapp suffix are interpreted as MOCApp programs; other files open in the editor.

The fresh-disk image includes welcome.mapp, which demonstrates printing, integer arithmetic, writing and reading a file, listing a directory, waiting for a key, and conditional branching. See docs/MOCAPP.md for the language reference and more examples.

Scripts are limited to 16 KiB and 256 source lines. MOCApp is interpreted by the kernel, not native machine code; it has no user-mode isolation and must not be treated as a sandbox for untrusted programs. F1 stops a script; Escape returns to the file manager.

## Tests

    make test

The `make test` suite builds and boots the ISO in QEMU, checks keyboard and mouse input, exception dispatch and CPU-enforced page protection, verifies disk persistence/CRUD over cold boots, exercises the editor and MOCApp runtime, and tests the host-side installer. GitHub Actions runs this suite on pushes and pull requests; see the repository's Actions page for the result of the current revision.

## Known limitations and next priorities

- No per-process address spaces, ring-3 user mode, stable syscall ABI, native ELF loader, or process scheduler. Paging currently uses one global identity map, with kernel text protected from writes.
- The interpreter operates in kernel context and supports only its documented instructions; it is not a security boundary.
- MOCFS1 is a custom, fixed-table filesystem with contiguous file extents. Mount rebuilds the allocation bitmap from validated extents, repairs orphan/missing allocation bits, and rejects overlapping extents. It has no journal; torn multi-sector directory updates or interrupted in-place data writes can still require recovery.
- PIT timer and PS/2 mouse support are rudimentary and do not provide preemptive scheduling.
- The UI tools are kernel-resident screens rather than separate protected processes.

Next priorities are stronger filesystem consistency and crash recovery, ring-3 execution with a stable syscall ABI, per-process memory spaces, and a process scheduler. Only label a feature as verified when its corresponding test actually passes.

## Installing an app created outside QEMU

Shut down QEMU before changing its disk image. Then use the host-side installer to add an external script to the guest's root directory:

    python3 scripts/install_mocapp.py --disk build/michael-oc-disk.img --source ./hello.mapp
    make run

The installer validates MOCFS1 metadata and script limits, rejects duplicate root names instead of overwriting files, and atomically replaces the image file after writing the script, bitmap, and directory entry. It changes the virtual disk only; the kernel and bootable ISO are not rebuilt. The tool supports this project's MOCFS1 disk-image format, not arbitrary FAT/NTFS disks.

## Paging milestone

The kernel now enables 32-bit 4 KiB page tables with a supervisor-only identity map, leaves virtual page zero absent, enables the CPU write-protect bit, maps kernel text and read-only data without write permission, and marks the Multiboot framebuffer as uncached. A boot-time self-test checks those permissions, maps a scratch virtual page to an allocated physical frame, verifies read/write access, unmaps it, confirms translation is absent, restores the identity mapping, and checks that the frame allocator's free-page count is unchanged.

This is a kernel-memory foundation, not userspace isolation. The current map still identity-maps most physical addresses as writable supervisor pages. User-mode address spaces, demand paging, page reclamation, page-fault recovery, and per-process page tables remain unimplemented.

A separate QEMU test deliberately attempts to write to the kernel's protected text page. It requires x86 page-fault vector 14, the expected supervisor-write/protection error bits, and a CR2 address equal to the faulting target. This validates actual CPU enforcement of the read-only mapping, not just the page-table flags.

## Mouse input

The guest initializes the PS/2 auxiliary device and IRQ12 when the emulated controller supports it. The UI draws a black/white pointer directly into the one-bit canvas. Click the home-screen menu items to open screens; click a file-manager row to open it; use the footer/tool area for common file actions and editor cursor placement. Dialog text is still entered through the keyboard. The QEMU integration test injects movement and a left-click, adjusts for the observed mouse Y-axis convention, and verifies that clicking the desktop's File Manager item changes the active screen.

## PIT timer

The kernel programs PIT channel 0 for approximately 100 interrupts per second and counts IRQ0 ticks. The system status screen reports the tick-based uptime in whole seconds. This is a basic hardware clock source, not yet a preemptive scheduler: the kernel still uses a single event loop, and there is no process scheduling or sleep syscall.

## Debug console

Press `6` on the home screen to open the monochrome debug console. It displays the latest 64 lines captured from kernel-side serial diagnostics, with lines truncated to a fixed-width buffer to keep memory use bounded. Use Up to scroll toward older lines, Down to return toward newer lines, `R` to jump to the latest entries, and Escape to return home. This is an in-kernel log viewer, not an interactive shell or a replacement for the serial port. Kernel diagnostics written directly by the interrupt module's own UART routines are not part of this capture buffer.

The debug console also accepts simple diagnostic commands in its prompt. Type `help` to list them, `mem` for allocator/page-protection status, `uptime` for PIT ticks and elapsed seconds, `fs` for filesystem mount/free-sector status, `input` for keyboard/mouse/timer status, `ls` to list the current file-manager directory, and `clear` to clear the in-memory log history. Enter runs a command and Backspace edits the prompt. This is still an in-kernel diagnostic command interpreter, not a general-purpose shell.
