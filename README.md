# Michael OC

Michael OC is an independent operating-system project targeting QEMU on Ubuntu Linux. Its key visual requirement is a strictly one-bit user interface: the software canvas stores exactly one bit per pixel, and the renderer outputs only black or full RGB white. This limit applies to the UI only; it does not limit the CPU, kernel, memory, storage, or applications.

## Current implementation

- Bootable 32-bit x86 kernel using GRUB Multiboot v1.
- Reproducible ISO build and automated QEMU tests.
- 800x600 packed one-bit software canvas and monochrome framebuffer renderer.
- Keyboard-driven diagnostic UI: home, system status, keyboard test and framebuffer test.
- PS/2 keyboard input through the 8259 PIC's IRQ1, with a bounded scan-code queue and an end-to-end QEMU key-injection test.
- 8254 PIT channel 0 configured at approximately 100 Hz, with a monotonic kernel tick counter and a live tick value on the system-status screen.
- 32-bit paging enabled with aligned 4 KiB page tables, a null-page guard, mapping/query/unmapping helpers, and a QEMU self-test that remaps a virtual address between physical frames.
- Legacy ATA PIO disk support over QEMU's primary IDE master, including IDENTIFY, LBA28 sector reads/writes and capability-aware cache flushing.
- MOCFS v1 persistent filesystem with mount/format, nested directories, file create/remove/list/exists/read/write, and 6 KiB maximum file size per file.
- A 32 MiB persistent virtual disk (`runtime/michael-oc-disk.img`) generated on first run. CI performs a write-and-read test, completely terminates QEMU, then boots a second guest on the same disk to verify data survives.
- Multiboot memory-map parser and physical page allocator for fully usable RAM pages below 4 GiB.
- Boot-time allocator self-test checks page alignment, read/write access, free-page accounting and double-free rejection.
- Reserved low memory, kernel image, Multiboot data structures, boot modules, and framebuffer pages.
- Protected-mode IDT with exception gates for CPU vectors 0–31, selecting the active Multiboot code segment rather than assuming a fixed GDT selector.
- Fatal exception diagnostics report the exception vector, error code, instruction pointer, code segment and flags to the serial port, then halt the CPU.

**This is still early OS development, not a completed general-purpose operating system.** The UI currently runs in kernel context. CPU exceptions and the legacy PIC are configured; only timer IRQ0 and keyboard IRQ1 are unmasked, and other hardware IRQs remain masked. Per-process address spaces and page-level isolation, mouse input, userspace, a general executable loader, app installation, and the requested built-in applications remain unfinished. MOCFS is a functional kernel-level filesystem core; it does not yet have a graphical file manager or text editor, and it does not have journaling or crash recovery.

The system-status page shows allocator readiness and the current count of free physical pages after successful initialization.

## Build requirements (Ubuntu)

    sudo apt update
    sudo apt install build-essential gcc-multilib grub-pc-bin xorriso mtools qemu-system-x86 make python3

## Build and run

From the repository root:

    make
    make run

The bootable ISO is generated at `build/michael-oc.iso`; the persistent 32 MiB data disk is `runtime/michael-oc-disk.img`. `make run` creates the disk if missing and reuses it on later launches. Keep this image to retain guest files; deleting it erases the filesystem. In QEMU, press 1 for system status, 2 for the keyboard test, 3 for the framebuffer test, or Escape to return home. On the keyboard-test screen, press keys to see their make scancodes. Input arrives through keyboard IRQ1 and is queued for the main kernel loop; the IRQ handler does not draw to the framebuffer.

Run automated tests:

    make test

The test builds the normal ISO and disk, verifies Multiboot and runtime startup markers (IDT, PIC, PIT, ATA, MOCFS, PMM, paging and monochrome UI), and injects keys via QEMU HMP to test keyboard-page navigation. A separate ISO executes INT3 and must report vector 3 before halting. Finally, `make test-fs-persistence` boots a writer kernel, creates a nested directory/file and checks list/read/write, fully terminates QEMU, starts a separate verifier kernel using the same disk, validates the persisted payload and directory, then removes the test objects. These tests validate only the implemented subset, not the remaining OS requirements.

## Planned development sequence

Next milestones are a graphical file manager and text editor on top of MOCFS, rename/copy/truncate operations, and stronger filesystem consistency checks; then a stable application ABI/ELF loader, userspace with per-process virtual address spaces, install/remove support for external apps, and later mouse/window support. The current filesystem is intentionally small and has no journal, permissions, or power-loss recovery. Paging establishes a shared kernel identity map rather than process isolation; the PIT counter is a timebase, not a scheduler or sleep API. Features must be labelled as verified, partial, or not implemented rather than represented by demo-only placeholders.
