SHELL := /bin/sh

CC := gcc
LD := ld
GRUB_FILE := grub-file
GRUB_MKRESCUE := grub-mkrescue
QEMU := qemu-system-i386
TIMEOUT := timeout

BUILD_DIR := build
ISO_DIR := $(BUILD_DIR)/iso
KERNEL := $(BUILD_DIR)/kernel.elf
ISO := $(BUILD_DIR)/michael-oc.iso
EXCEPTION_TEST_DIR := $(BUILD_DIR)/exception-test
EXCEPTION_TEST_ISO_DIR := $(EXCEPTION_TEST_DIR)/iso
EXCEPTION_TEST_KERNEL := $(EXCEPTION_TEST_DIR)/kernel.elf
EXCEPTION_TEST_ISO := $(EXCEPTION_TEST_DIR)/michael-oc-exception-test.iso
EXCEPTION_TEST_OBJECTS := $(EXCEPTION_TEST_DIR)/boot.o $(EXCEPTION_TEST_DIR)/kernel.o $(EXCEPTION_TEST_DIR)/memory.o $(EXCEPTION_TEST_DIR)/interrupts.o $(EXCEPTION_TEST_DIR)/isr_stubs.o
OBJECTS := $(BUILD_DIR)/boot.o $(BUILD_DIR)/kernel.o $(BUILD_DIR)/memory.o $(BUILD_DIR)/interrupts.o $(BUILD_DIR)/isr_stubs.o

CFLAGS := -m32 -std=gnu11 -O2 -Wall -Wextra -Werror \
	-ffreestanding -fno-builtin -fno-pie -fno-pic \
	-fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables
ASFLAGS := -m32 -ffreestanding -fno-pie -fno-pic \
	-fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables
LDFLAGS := -m elf_i386 -T linker.ld -nostdlib

.PHONY: all kernel iso run test test-exception clean check-tools

all: check-tools iso

kernel: $(KERNEL)

iso: $(ISO)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/boot.o: src/boot.S | $(BUILD_DIR)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel.o: src/kernel.c src/multiboot.h src/memory.h src/interrupts.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/memory.o: src/memory.c src/memory.h src/multiboot.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/interrupts.o: src/interrupts.c src/interrupts.h src/multiboot.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/isr_stubs.o: src/isr_stubs.S | $(BUILD_DIR)
	$(CC) $(ASFLAGS) -c $< -o $@

$(EXCEPTION_TEST_DIR):
	mkdir -p $(EXCEPTION_TEST_DIR)

$(EXCEPTION_TEST_DIR)/boot.o: src/boot.S | $(EXCEPTION_TEST_DIR)
	$(CC) $(ASFLAGS) -c $< -o $@

$(EXCEPTION_TEST_DIR)/kernel.o: src/kernel.c src/multiboot.h src/memory.h src/interrupts.h | $(EXCEPTION_TEST_DIR)
	$(CC) $(CFLAGS) -DMICHAEL_OC_EXCEPTION_SELF_TEST -c $< -o $@

$(EXCEPTION_TEST_DIR)/memory.o: src/memory.c src/memory.h src/multiboot.h | $(EXCEPTION_TEST_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(EXCEPTION_TEST_DIR)/interrupts.o: src/interrupts.c src/interrupts.h src/multiboot.h | $(EXCEPTION_TEST_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(EXCEPTION_TEST_DIR)/isr_stubs.o: src/isr_stubs.S | $(EXCEPTION_TEST_DIR)
	$(CC) $(ASFLAGS) -c $< -o $@

$(EXCEPTION_TEST_KERNEL): $(EXCEPTION_TEST_OBJECTS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(EXCEPTION_TEST_OBJECTS)
	$(GRUB_FILE) --is-x86-multiboot $@

$(EXCEPTION_TEST_ISO): $(EXCEPTION_TEST_KERNEL) grub/grub.cfg
	mkdir -p $(EXCEPTION_TEST_ISO_DIR)/boot/grub
	cp $(EXCEPTION_TEST_KERNEL) $(EXCEPTION_TEST_ISO_DIR)/boot/kernel.elf
	cp grub/grub.cfg $(EXCEPTION_TEST_ISO_DIR)/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $@ $(EXCEPTION_TEST_ISO_DIR)

$(KERNEL): $(OBJECTS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJECTS)
	$(GRUB_FILE) --is-x86-multiboot $@

$(ISO): $(KERNEL) grub/grub.cfg
	mkdir -p $(ISO_DIR)/boot/grub
	cp $(KERNEL) $(ISO_DIR)/boot/kernel.elf
	cp grub/grub.cfg $(ISO_DIR)/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $@ $(ISO_DIR)

check-tools:
	@command -v $(CC) >/dev/null || { echo "Missing compiler: $(CC)"; exit 1; }
	@command -v $(LD) >/dev/null || { echo "Missing linker: $(LD)"; exit 1; }
	@command -v $(GRUB_FILE) >/dev/null || { echo "Missing grub-file (install grub-pc-bin)"; exit 1; }
	@command -v $(GRUB_MKRESCUE) >/dev/null || { echo "Missing grub-mkrescue (install grub-pc-bin and xorriso)"; exit 1; }
	@command -v mformat >/dev/null || { echo "Missing mformat (install mtools)"; exit 1; }
	@command -v python3 >/dev/null || { echo "Missing python3 (needed for injected-key test)"; exit 1; }

run: $(ISO)
	$(QEMU) -m 128M -cdrom $(ISO) -monitor none -serial stdio -no-reboot -no-shutdown

test: $(ISO)
	@command -v $(QEMU) >/dev/null || { echo "Missing $(QEMU); install qemu-system-x86"; exit 1; }
	@command -v $(TIMEOUT) >/dev/null || { echo "Missing timeout (GNU coreutils)"; exit 1; }
	@rm -f $(BUILD_DIR)/serial.log
	@set +e; $(TIMEOUT) 5s $(QEMU) -m 128M -cdrom $(ISO) -display none -monitor none -serial file:$(BUILD_DIR)/serial.log -no-reboot -no-shutdown; rc=$$?; set -e; \
	if [ "$$rc" -ne 0 ] && [ "$$rc" -ne 124 ]; then echo "FAIL: QEMU exited with status $$rc"; cat $(BUILD_DIR)/serial.log 2>/dev/null || true; exit "$$rc"; fi; \
	for marker in MICHAEL_OC_BOOT_OK MICHAEL_OC_PMM_OK MICHAEL_OC_MONO_TEST_OK MICHAEL_OC_INPUT_TEST_OK MICHAEL_OC_FB_OK MICHAEL_OC_KEYBOARD_IRQ_OK; do \
		if ! grep -q "$$marker" $(BUILD_DIR)/serial.log; then echo "FAIL: missing $$marker"; cat $(BUILD_DIR)/serial.log 2>/dev/null || true; exit 1; fi; \
	done; \
	echo "PASS: kernel boot, 1-bit canvas, UI input logic, framebuffer output, and keyboard IRQ startup."

	python3 scripts/test_keyboard_input.py --qemu $(QEMU) --iso $(ISO) --work-dir $(BUILD_DIR)
	$(MAKE) test-exception

test-exception: $(EXCEPTION_TEST_ISO)
	@command -v $(QEMU) >/dev/null || { echo "Missing $(QEMU); install qemu-system-x86"; exit 1; }
	@command -v $(TIMEOUT) >/dev/null || { echo "Missing timeout (GNU coreutils)"; exit 1; }
	python3 scripts/test_exceptions.py --qemu $(QEMU) --iso $(EXCEPTION_TEST_ISO) --work-dir $(EXCEPTION_TEST_DIR)

clean:
	rm -rf $(BUILD_DIR)
