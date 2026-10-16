# Michael OC

Michael OC is an independent operating-system project targeting QEMU on Ubuntu Linux. Its key visual requirement is a strictly one-bit user interface: the software canvas stores exactly one bit per pixel, and the renderer outputs only black or full RGB white. This limit applies to the UI only; it does not limit the CPU, kernel, memory, storage, or applications.

## Current implementation

- Bootable 32-bit x86 kernel using GRUB Multiboot v1.
- Reproducible ISO build and automated QEMU tests.
- 800x600 packed one-bit software canvas and monochrome framebuffer renderer.
- Keyboard-driven diagnostic UI: home, system status, keyboard test and framebuffer test.
- PS/2 keyboard input through the 8259 PIC's IRQ1, with a bounded scan-code queue and an end-to-end QEMU key-injection test.
- 8254 PIT channel 0 configured at approximately 100 Hz, with a monotonic kernel tick counter and a live tick value on the system-status screen.
- Multiboot memory-map parser and physical page allocator for fully usable RAM pages below 4 GiB.
- Boot-time allocator self-test checks page alignment, read/write access, free-page accounting and double-free rejection.
- Reserved low memory, kernel image, Multiboot data structures, boot modules, and framebuffer pages.
- Protected-mode IDT with exception gates for CPU vectors 0–31, selecting the active Multiboot code segment rather than assuming a fixed GDT selector.
- Fatal exception diagnostics report the exception vector, error code, instruction pointer, code segment and flags to the serial port, then halt the CPU.

**This is still early OS development, not a completed general-purpose operating system.** The UI currently runs in kernel context. CPU exceptions and the legacy PIC are configured; only timer IRQ0 and keyboard IRQ1 are unmasked, and other hardware IRQs remain masked. Paging/virtual memory, mouse input, persistent filesystem, userspace, a general executable loader, app installation, and the requested built-in applications remain unfinished.

The system-status page shows allocator readiness and the current count of free physical pages after successful initialization.

## Build requirements (Ubuntu)

    sudo apt update
    sudo apt install build-essential gcc-multilib grub-pc-bin xorriso mtools qemu-system-x86 make python3

## Build and run

From the repository root:

    make
    make run

The bootable ISO is generated at build/michael-oc.iso. In QEMU, press 1 for system status, 2 for the keyboard test, 3 for the framebuffer test, or Escape to return home. On the keyboard-test screen, press keys to see their make scancodes. Input arrives through keyboard IRQ1 and is queued for the main kernel loop; the IRQ handler does not draw to the framebuffer.

Run automated tests:

    make test

The test builds the normal ISO, verifies Multiboot and runtime startup markers (including IDT, PIC remap, PIT setup and tick delivery, timer/keyboard IRQ enablement), and checks the physical-memory manager and monochrome UI. QEMU HMP injects key 2, A and Escape to test keyboard-page navigation. A separate test-only ISO executes INT3; CI requires the breakpoint exception report (vector 3) and safe guest halt. These tests do not prove the remaining OS requirements are complete.

## Planned development sequence

Next milestones are paging and virtual memory, persistent block storage and a filesystem, a stable application ABI/ELF loader, independent applications and installation/removal, and later mouse/window support. The PIT tick counter is a kernel timebase, not yet a scheduler or sleep API. Features must be labelled as verified, partial, or not implemented rather than represented by demo-only placeholders.
