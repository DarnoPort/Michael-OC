# Michael OC

Michael OC is an independent operating-system project for QEMU on Ubuntu Linux. Its defining visual requirement is a strictly 1-bit user interface: every displayed UI pixel must be black or white. This restriction applies to the UI only; it does not restrict the CPU architecture, kernel, memory, filesystem, or applications.

## Current state

The repository was empty at project start. The first milestone establishes a reproducible 32-bit x86 Multiboot kernel, a GRUB bootable ISO, serial diagnostics, a black-and-white VGA text bootstrap screen, and an automated QEMU boot smoke test. **This is an early kernel bootstrap, not a completed operating system.** It does not yet provide a graphical desktop, persistent filesystem, general application loader, or the built-in applications listed in the project goals.

Implementation and verified status are tracked in [`DEVELOPMENT.md`](DEVELOPMENT.md).

## Build requirements (Ubuntu)

```sh
sudo apt update
sudo apt install build-essential gcc-multilib grub-pc-bin xorriso qemu-system-x86 make
```

## Build and run

```sh
make
make run
```

The generated bootable ISO is `build/michael-oc.iso`. Close QEMU to exit the interactive run.

Run the headless boot smoke test:

```sh
make test
```

The smoke test boots the ISO in QEMU and verifies that the kernel emits `MICHAEL_OC_BOOT_OK` over the emulated serial port. This confirms kernel entry, not full OS readiness.

## Project direction

The planned implementation sequence is: establish the boot path; replace the bootstrap display with a real 1-bit framebuffer; add input and a usable shell/desktop; implement memory and device abstractions; add persistent storage and filesystem operations; implement a real executable/application ABI and package installation; then build and test the included applications.

The architecture will be allowed to evolve when required by test results. Features must be labelled as implemented, verified, partial, or not implemented rather than represented by demo-only placeholders.