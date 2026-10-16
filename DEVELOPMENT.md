# Michael OC development log

## Project baseline

- Date: 2026-10-16
- The GitHub repository was initially empty; the project started from a new bootable-kernel baseline.
- Target: Ubuntu Linux + QEMU, 32-bit x86 guest, GRUB Multiboot v1.
- Toolchain: freestanding C, GNU assembler/binutils, GRUB rescue ISO, xorriso, mtools, QEMU and Python 3 for the QEMU monitor test.
- The architecture can evolve if necessary, but do not redesign the working boot path without a concrete reason.

## Status key

- **Implemented, smoke-tested**: exercised by an automated runtime test.
- **Implemented, not yet runtime-verified**: code exists, but a working system run has not confirmed the behaviour.
- **Partial**: the foundation works but the full feature is incomplete.
- **Not implemented**: work remains.

## Current status

| Component | Status | Evidence / limitation |
|---|---|---|
| Freestanding x86 kernel entry | Implemented, smoke-tested | GRUB enters kernel_main; serial marker checked by make test |
| Multiboot v1 header | Implemented, smoke-tested | grub-file check passes |
| Reproducible bootable ISO | Implemented, smoke-tested | make builds build/michael-oc.iso in CI |
| Kernel linker layout | Implemented, smoke-tested | Separate RX and RW PT_LOAD segments |
| Serial diagnostics | Implemented, smoke-tested | COM1 output is used by automated tests and diagnosis |
| Packed one-bit canvas | Implemented, smoke-tested | 800x600 canvas, one bit per pixel; set/clear self-test required by CI |
| Monochrome framebuffer rendering | Implemented, smoke-tested | GRUB mode 800x600x32; only black and full RGB white are emitted |
| Keyboard-driven diagnostic pages | Implemented, smoke-tested | Home, system status, keyboard test and framebuffer test |
| Keyboard handling state logic | Implemented, self-tested | Navigation, make-code handling, break-code filtering and Escape return |
| PS/2 keyboard IRQ1 | Implemented, end-to-end QEMU test | PIC vector 33; IRQ handler queues scan bytes; QEMU HMP injects 2, A and Escape and CI checks page transition and scan-code marker |
| Multiboot memory-map parsing | Implemented, smoke-tested | Parses variable-sized entries and frees only complete pages covered by type-1 RAM ranges |
| Physical page allocator | Implemented, smoke-tested | Two bitmaps track used and allocated frames below 4 GiB; 4096-byte pages; free count and double-free protection |
| Memory-region reservation | Implemented, smoke-tested | Reserves low 1 MiB, kernel image including BSS, Multiboot info/map, boot modules and framebuffer pages |
| Physical-memory allocator self-test | Implemented, smoke-tested | Allocates a frame, verifies alignment/read/write/count restoration and rejects double-free |
| IDT setup | Implemented, smoke-tested | Boot installs exception gates 0–31 and PIC interrupt gates 32–47; lidt/sidt base and limit check emits a marker required by CI |
| CPU exception stubs and fatal report | Implemented, smoke-tested | Separate test ISO executes int3; CI verifies vector 3/breakpoint diagnostics and the guest halt |
| PIC remapping and EOI handling | Implemented, smoke-tested for timer and keyboard | Legacy PIC remapped to 0x20/0x28; IRQ0 and IRQ1 are enabled, other lines stay masked; EOI is sent to the appropriate PIC |
| Keyboard scan-code queue | Implemented, end-to-end QEMU test | IRQ handler only queues bytes; the main loop processes them and uses a CLI plus atomic STI/HLT wait to avoid lost wakeups |
| PIT timer | Implemented, smoke-tested | PIT channel 0 runs at approximately 100 Hz; IRQ0 increments a monotonic tick count; CI requires a marker emitted after at least three ticks |
| 32-bit paging | Implemented, runtime self-tested | 4 GiB identity map with 4 KiB PTEs; null page absent; test changes a virtual page from one PMM frame to another, checks data, unmaps it, then restores identity mapping. No per-process isolation yet |
| Paging / virtual memory | Basic paging implemented; per-process virtual memory not implemented | Static 4 KiB page tables map 4 GiB identity-wise; null page is absent; map/query/unmap APIs pass a PMM frame-aliasing self-test. Page tables are part of kernel BSS and reserved by the PMM. Dynamic page-table allocation and process isolation remain unfinished |
| Mouse input | Not implemented | No mouse packet decoder or pointer yet |
| Persistent disk/filesystem | Not implemented | No block driver, filesystem or persistent user files yet |
| Userspace and system-call API | Not implemented | The UI currently runs in kernel context |
| External app loading/install/remove | Not implemented | No executable loader, app ABI or package format yet |
| File manager/editor/monitor/installer apps | Not implemented | These depend on persistent storage and a stable application interface |

## Source layout

- src/boot.S — Multiboot header, stack setup, IDT initialization call and C entry transfer.
- src/multiboot.h — shared Multiboot info and memory-map structures.
- src/memory.h and src/memory.c — physical frame allocator and allocator self-test.
- src/interrupts.h, src/interrupts.c and src/interrupts.S — IDT gates, exception stubs, PIC remapping/EOI, keyboard IRQ queue and PIT tick counter.
- src/paging.h and src/paging.c — identity page tables, page map/query/unmap functions and a physical-frame aliasing self-test.
- src/kernel.c — serial diagnostics, one-bit canvas, monochrome UI pages, keyboard state handler and IRQ-queued PS/2 input.
- linker.ld — kernel image layout and exported kernel bounds.
- grub/grub.cfg — video mode, serial diagnostics and boot entry.
- Makefile — kernel/ISO build, tool checks, run and test targets.
- scripts/test_keyboard_input.py — QEMU HMP key-injection test.
- .github/workflows/build.yml — Ubuntu CI build, QEMU test and ISO artifact upload.
- README.md — setup, build, run, controls and known limitations.

## Build and run

Install on Ubuntu:

    sudo apt update
    sudo apt install build-essential gcc-multilib grub-pc-bin xorriso mtools qemu-system-x86 make python3

From the repository root:

    make clean
    make
    make test
    make run

ISO output: build/michael-oc.iso.

## Latest verified milestone

GitHub Actions run [38904714276](https://github.com/DarnoPort/Michael-OC/actions/runs/38904714276) passed for commit 08ec5e72f63273c8f0f446b29c4f6214ce248d83. CI built the ISO, enabled paging before PMM initialization, booted QEMU through the paging self-test and UI startup, injected keyboard events, and passed the separate INT3 exception-report test. The paging test confirms that a virtual alias can map two distinct physical allocator frames and then be unmapped. The address space is still shared and identity-mapped; this does not establish userspace isolation.

The physical allocator remains a pre-paging allocator: it tracks RAM below 4 GiB in a bitmap in kernel BSS and assumes a single-core boot environment. Its self-test accesses the allocated physical address directly because paging is not enabled.

## Next tasks

1. Add a QEMU block device and persistent filesystem; test create/write/read/delete and data survival across a VM restart.
2. Replace the all-identity static mapping with dynamic page-table allocation and separate virtual address spaces when the process model is designed.
3. Define a kernel-to-app ABI and a genuine executable loader; implement install/remove after storage and executable validation work.
4. Define a kernel-to-app ABI, an executable format/loader and a safe external-app installation/removal mechanism.
5. Implement real file manager, editor, system monitor and installer applications using that interface.
6. Add mouse support and evolve the current full-screen diagnostic UI into a proper desktop/window manager.

## Verification rules

A source file existing is not proof it runs. Distinguish boot, allocator self-test, IDT initialization, deliberate exception injection, keyboard IRQ input, persistence across restart and full feature completion. Do not mark a feature smoke-tested without a relevant passing test.
