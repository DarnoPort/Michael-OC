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
OBJECTS := $(BUILD_DIR)/boot.o $(BUILD_DIR)/kernel.o

CFLAGS := -m32 -std=gnu11 -O2 -Wall -Wextra -Werror \
	-ffreestanding -fno-builtin -fno-pie -fno-pic \
	-fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables
ASFLAGS := -m32 -ffreestanding -fno-pie -fno-pic \
	-fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables
LDFLAGS := -m elf_i386 -T linker.ld -nostdlib

.PHONY: all kernel iso run test clean check-tools

all: iso

kernel: $(KERNEL)

iso: $(ISO)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/boot.o: src/boot.S | $(BUILD_DIR)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel.o: src/kernel.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

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

run: $(ISO)
	$(QEMU) -m 128M -cdrom $(ISO) -monitor none -serial stdio -no-reboot -no-shutdown

test: $(ISO)
	@command -v $(QEMU) >/dev/null || { echo "Missing $(QEMU); install qemu-system-x86"; exit 1; }
	@command -v $(TIMEOUT) >/dev/null || { echo "Missing timeout (GNU coreutils)"; exit 1; }
	@rm -f $(BUILD_DIR)/serial.log
	@set +e; $(TIMEOUT) 5s $(QEMU) -m 128M -cdrom $(ISO) -display none -monitor none -serial file:$(BUILD_DIR)/serial.log -no-reboot -no-shutdown; rc=$$?; set -e; \
	if [ "$$rc" -ne 0 ] && [ "$$rc" -ne 124 ]; then echo "FAIL: QEMU exited with status $$rc"; cat $(BUILD_DIR)/serial.log 2>/dev/null || true; exit "$$rc"; fi; \
	if ! grep -q "MICHAEL_OC_BOOT_OK" $(BUILD_DIR)/serial.log; then echo "FAIL: kernel boot marker not found"; cat $(BUILD_DIR)/serial.log 2>/dev/null || true; exit 1; fi; \
	echo "PASS: QEMU reached the Michael OC kernel entry point."

clean:
	rm -rf $(BUILD_DIR)
