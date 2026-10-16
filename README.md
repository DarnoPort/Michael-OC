# Michael OC

Michael OC is an independent operating-system project targeting QEMU on Ubuntu Linux. Its key visual requirement is a strictly one-bit user interface: the software canvas stores exactly one bit per pixel, and the renderer outputs only black or full RGB white. This limit applies to the UI only; it does not limit the CPU, kernel, memory, storage, or applications.

## Current state

The repository began empty. The current implementation is a bootable 32-bit x86 Multiboot v1 kernel, a reproducible GRUB ISO build, serial diagnostics, a packed 800x600 one-bit software canvas, a monochrome framebuffer UI, and a polling PS/2 keyboard path. The UI has system-status, keyboard-test, and framebuffer-test pages selected from a home screen with number keys 1, 2, and 3; Escape returns home.

**This is still an early OS bring-up, not a completed general-purpose operating system.** The screens run inside the kernel. There is no persistent filesystem, userspace, general executable loader, app installer, mouse input, or window manager yet.

## Build requirements (Ubuntu)

    sudo apt update
    sudo apt install build-essential gcc-multilib grub-pc-bin xorriso mtools qemu-system-x86 make python3

## Build and run

From the repository root:

    make
    make run

The bootable ISO is generated at build/michael-oc.iso. In QEMU, press 1 for system status, 2 for the keyboard test, 3 for the framebuffer test, or Escape to return home. On the keyboard-test screen, press keys to see their make scancodes. Input is polled from the emulated PS/2 controller; this is not yet an interrupt-driven keyboard driver.

Run the automated tests:

    make test

The test builds and boots the ISO, checks kernel, one-bit canvas, framebuffer and input-state markers, then starts QEMU with its HMP monitor and injects key 2, key A, and Escape. The serial log confirms the keyboard page opens, the A make scancode is received, and Escape returns to the home page. These tests do not prove the full OS requirements are complete.

## Planned development sequence

The next milestones are mouse support, memory-map handling and allocators, interrupt/exception handling, persistent storage and a filesystem, a real application ABI and loader, and working built-in applications. Features must be labelled as verified, partial, or not implemented rather than represented by demo-only placeholders.