# Michael OC

Michael OC is an independent operating-system project targeting QEMU on Ubuntu Linux. Its key visual requirement is a strictly one-bit user interface: the software canvas stores exactly one bit per pixel, and the renderer outputs only black or full RGB white. This limit applies to the UI only; it does not limit the CPU, kernel, memory, storage, or applications.

## Current implementation

- Bootable 32-bit x86 kernel using GRUB Multiboot v1.
- Reproducible ISO build and automated QEMU tests.
- 800x600 packed one-bit software canvas and monochrome framebuffer renderer.
- Keyboard-driven diagnostic UI: home, system status, keyboard test and framebuffer test.
- PS/2 keyboard polling with an end-to-end QEMU key-injection test.
- Multiboot memory-map parser and physical page allocator for fully usable RAM pages below 4 GiB.
- Boot-time allocator self-test checks page alignment, read/write access, free-page accounting and double-free rejection.
- Reserved low memory, kernel image, Multiboot data structures, boot modules, and framebuffer pages.
- Protected-mode IDT with exception gates for CPU vectors 0–31. Boot tests verify that the IDT is loaded and its limit/base can be read back.
- Fatal exception diagnostics report the exception vector, error code, instruction pointer, code segment and flags to the serial port, then halt the CPU.

**This is still early OS development, not a completed general-purpose operating system.** The UI currently runs in kernel context. The IDT covers CPU exceptions only: the PIC/IRQ subsystem is not initialized, hardware interrupts remain disabled, and keyboard input still polls the PS/2 controller. Paging/virtual memory, mouse input, persistent filesystem, userspace, a general executable loader, app installation, and the requested built-in applications remain unfinished. The fatal exception handler is implemented but CI does not yet deliberately trigger a CPU fault to test the report path end-to-end.

The system-status page shows allocator readiness and the current count of free physical pages after successful initialization.

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

The test builds the ISO, verifies the Multiboot header, boots QEMU, and requires serial markers for kernel entry, IDT setup, the physical-memory manager self-test, one-bit canvas self-test, UI input-state self-test, framebuffer output and keyboard polling. It then starts QEMU with its HMP monitor and injects key 2, A and Escape. The test checks the keyboard-page transition, A's make scancode and the return to the home page. These checks do not prove the remaining OS requirements are complete.

## Planned development sequence

Next milestones are hardware IRQ handling, paging and virtual memory, persistent block storage and a filesystem, a stable application ABI/ELF loader, independent applications and installation/removal, and later mouse/window support. Features must be labelled as verified, partial, or not implemented rather than represented by demo-only placeholders.
