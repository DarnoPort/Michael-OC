# Michael OC development log

## Project baseline

- Date: 2026-10-16
- Repository began empty; there was no existing kernel or implementation to continue.
- First implementation target: reproducible 32-bit x86 Multiboot v1 kernel booted by GRUB in QEMU.
- Language/toolchain: freestanding C and GNU assembler, GNU binutils, GRUB rescue ISO, QEMU.
- This architecture is an initial, deliberately small bring-up baseline. Revisit it if later requirements are better served by a different boot protocol or 64-bit design.

## Status key

- **Implemented, smoke-tested**: a test that exercises the feature has passed in an execution environment.
- **Implemented, not yet runtime-verified**: code/configuration exists, but it has not yet been confirmed by a run.
- **Partial**: a minimal foundation exists, but the intended feature is incomplete.
- **Not implemented**: work remains.

## Current milestone: bootable kernel foundation

| Component | Status | Evidence / limitation |
|---|---|---|
| Freestanding x86 kernel entry | Implemented, not yet runtime-verified | src/boot.S, src/kernel.c |
| Multiboot v1 header | Implemented, not yet runtime-verified | src/boot.S; make checks with grub-file |
| Linker layout | Implemented, not yet runtime-verified | linker.ld, loads kernel at 1 MiB |
| Reproducible ISO build | Implemented, not yet runtime-verified | Makefile, grub/grub.cfg |
| Serial boot diagnostics | Implemented, not yet runtime-verified | COM1 output from the kernel |
| Black/white bootstrap screen | Partial | VGA text mode uses black and white attributes only; this is not yet the required graphical desktop or a packed 1-bit framebuffer |
| Automated QEMU boot smoke test | Implemented, not yet runtime-verified | make test expects MICHAEL_OC_BOOT_OK in serial output; CI runs it |
| Graphical desktop/window management | Not implemented | Must move to a real monochrome framebuffer and input handling |
| Keyboard and mouse input | Not implemented | No input drivers yet |
| Memory manager | Not implemented | No page/frame allocator yet |
| Persistent storage/filesystem | Not implemented | No disk driver or filesystem yet |
| System-call/API boundary | Not implemented | No userspace ABI yet |
| Loadable external applications | Not implemented | No executable loader or install/remove logic yet |
| File manager, editor, monitor, installer | Not implemented | Must be real programs using the future system API |

## Initial project layout

- src/boot.S — Multiboot header, stack setup and C entry transfer.
- src/kernel.c — early kernel initialization, VGA text output and serial diagnostics.
- linker.ld — kernel image layout.
- grub/grub.cfg — GRUB boot menu.
- Makefile — build, ISO creation, run and smoke-test targets.
- .github/workflows/build.yml — Ubuntu CI build and QEMU boot check.

## Build and test

Install dependencies on Ubuntu:

    sudo apt update
    sudo apt install build-essential gcc-multilib grub-pc-bin xorriso qemu-system-x86 make

Then run:

    make clean
    make
    make test
    make run

The ISO should be generated at build/michael-oc.iso.

## Next implementation tasks

1. Get the bootstrap build and CI smoke test passing; fix actual compiler, GRUB, linker, or QEMU failures if reported.
2. Add a graphics mode/framebuffer boot request and a packed 1-bit software framebuffer, plus tests that ensure framebuffer writes map only to black and white.
3. Add PS/2 keyboard input and a simple interactive command shell for early diagnostics.
4. Add memory-map parsing and a physical page/frame allocator; add exception/interrupt handling.
5. Add a block-device abstraction and persistent storage in a QEMU disk image; choose or implement a documented filesystem.
6. Define a stable kernel service ABI and executable format/loader before writing user applications.
7. Add a desktop/window model and real applications in incremental, tested steps.
8. Add installation/removal for external applications and document the developer workflow.

## Verification honesty

A source file existing is not evidence it boots. The implementation should be labelled runtime-verified only after the QEMU smoke test or an appropriate functional test runs successfully. A successful boot marker only proves that GRUB reached the kernel entry path; it does not prove the full operating system requirements have been met.