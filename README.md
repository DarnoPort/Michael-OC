# Michael OC

Michael OC is an independent operating-system project targeting QEMU on Ubuntu Linux. Its key visual requirement is a strictly one-bit user interface: the software canvas stores exactly one bit per pixel, and the renderer outputs only black or full RGB white. This limit applies to the UI only; it does not limit the CPU, kernel, memory, storage, or applications.

## Current implementation

- Bootable 32-bit x86 kernel using GRUB Multiboot v1.
- Reproducible ISO build and automated QEMU tests.
- 800x600 packed one-bit software canvas and monochrome framebuffer renderer.
- Keyboard-driven diagnostic UI: home, system status, keyboard test and framebuffer test.
- PS/2 keyboard polling with an end-to-end QEMU key-injection test.
- IDT entries for CPU exception vectors 0–31, serial fatal-exception reporting, and a dedicated QEMU `INT3` dispatch test.
- Multiboot memory-map parser and physical page allocator for fully usable RAM pages below 4 GiB.
- Boot-time allocator self-test that allocates a page, checks page alignment, writes and reads a test value, frees the page, checks the free-page count, and rejects double-free.
- Reserved low memory, kernel image, Multiboot data structures, boot modules, and framebuffer pages.

**This is still early OS development, not a completed general-purpose operating system.** The UI currently runs in kernel context. The allocator is physical-only; paging/virtual memory, hardware IRQ routing and interrupt-driven keyboard input, mouse input, persistent filesystem, userspace, a general executable loader, app installation, and the requested built-in applications remain unfinished. CPU exceptions 0–31 have IDT gates and a fatal serial diagnostic path.

The system-status page shows the allocator readiness and current count of free physical pages after a successful initialization.

## Build requirements (Ubuntu)

    sudo apt update
    sudo apt install build-essential gcc-multilib grub-pc-bin xorriso mtools qemu-system-x86 make python3

## Build and run

From the repository root:

    make
    make run

The bootable ISO is generated at build/michael-oc.iso. In QEMU, press 1 for system status, 2 for the keyboard test, 3 for the framebuffer test, or Escape to return home. On the keyboard-test screen, press keys to see their make scancodes. Input is polled from the emulated PS/2 controller; this is not yet an interrupt-driven keyboard driver.

Run automated tests:

    make test

The test builds the ISO, verifies the Multiboot header, boots QEMU, and requires serial markers for kernel entry, IDT setup, physical-memory manager self-test, one-bit canvas self-test, UI input-state self-test, framebuffer flush and keyboard polling. It then starts QEMU with its HMP monitor and injects key 2, A and Escape. A second, separately compiled diagnostic kernel executes `INT3`; the test checks the exception vector and synthetic error-code reporting.

## Planned development sequence

Next milestones are PIC setup and hardware IRQ handlers, interrupt-driven keyboard input, paging and virtual memory, mouse support, persistent storage and a filesystem, a stable application ABI and loader, and working built-in applications. Features must be labelled as verified, partial, or not implemented rather than represented by demo-only placeholders.
