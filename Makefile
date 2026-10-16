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
DISK_IMAGE := $(BUILD_DIR)/michael-oc-disk.img
STORAGE_TEST_DIR := $(BUILD_DIR)/storage-test
STORAGE_TEST_ISO_DIR := $(STORAGE_TEST_DIR)/iso
STORAGE_TEST_KERNEL := $(STORAGE_TEST_DIR)/kernel.elf
STORAGE_TEST_ISO := $(STORAGE_TEST_DIR)/michael-oc-storage-test.iso
EXCEPTION_TEST_DIR := $(BUILD_DIR)/exception-test
EXCEPTION_TEST_ISO_DIR := $(EXCEPTION_TEST_DIR)/iso
EXCEPTION_TEST_KERNEL := $(EXCEPTION_TEST_DIR)/kernel.elf
EXCEPTION_TEST_ISO := $(EXCEPTION_TEST_DIR)/michael-oc-exception-test.iso
EXCEPTION_TEST_OBJECTS := $(EXCEPTION_TEST_DIR)/boot.o $(EXCEPTION_TEST_DIR)/kernel.o $(EXCEPTION_TEST_DIR)/memory.o $(EXCEPTION_TEST_DIR)/interrupts.o $(EXCEPTION_TEST_DIR)/isr_stubs.o $(EXCEPTION_TEST_DIR)/disk.o $(EXCEPTION_TEST_DIR)/fs.o $(EXCEPTION_TEST_DIR)/paging.o
STORAGE_TEST_OBJECTS := $(STORAGE_TEST_DIR)/boot.o $(STORAGE_TEST_DIR)/kernel.o $(STORAGE_TEST_DIR)/memory.o $(STORAGE_TEST_DIR)/interrupts.o $(STORAGE_TEST_DIR)/isr_stubs.o $(STORAGE_TEST_DIR)/disk.o $(STORAGE_TEST_DIR)/fs.o $(STORAGE_TEST_DIR)/paging.o
OBJECTS := $(BUILD_DIR)/boot.o $(BUILD_DIR)/kernel.o $(BUILD_DIR)/memory.o $(BUILD_DIR)/interrupts.o $(BUILD_DIR)/isr_stubs.o $(BUILD_DIR)/disk.o $(BUILD_DIR)/fs.o $(BUILD_DIR)/paging.o

CFLAGS := -m32 -std=gnu11 -O2 -Wall -Wextra -Werror \
	-ffreestanding -fno-builtin -fno-pie -fno-pic \
	-fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables
ASFLAGS := -m32 -ffreestanding -fno-pie -fno-pic \
	-fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables
LDFLAGS := -m elf_i386 -T linker.ld -nostdlib

.PHONY: all kernel iso run test test-exception test-storage test-apps test-mocapp-installer clean check-tools

all: check-tools iso

kernel: $(KERNEL)

iso: $(ISO)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/boot.o: src/boot.S | $(BUILD_DIR)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel.o: src/kernel.c src/multiboot.h src/memory.h src/interrupts.h src/fs.h src/paging.h | $(BUILD_DIR)
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

$(EXCEPTION_TEST_DIR)/kernel.o: src/kernel.c src/multiboot.h src/memory.h src/interrupts.h src/fs.h src/paging.h | $(EXCEPTION_TEST_DIR)
	$(CC) $(CFLAGS) -DMICHAEL_OC_EXCEPTION_SELF_TEST -c $< -o $@

$(EXCEPTION_TEST_DIR)/memory.o: src/memory.c src/memory.h src/multiboot.h | $(EXCEPTION_TEST_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(EXCEPTION_TEST_DIR)/interrupts.o: src/interrupts.c src/interrupts.h src/multiboot.h | $(EXCEPTION_TEST_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(EXCEPTION_TEST_DIR)/isr_stubs.o: src/isr_stubs.S | $(EXCEPTION_TEST_DIR)
	$(CC) $(ASFLAGS) -c $< -o $@

$(EXCEPTION_TEST_DIR)/disk.o: src/disk.c src/disk.h src/multiboot.h | $(EXCEPTION_TEST_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(EXCEPTION_TEST_DIR)/fs.o: src/fs.c src/fs.h src/disk.h src/multiboot.h | $(EXCEPTION_TEST_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(EXCEPTION_TEST_DIR)/paging.o: src/paging.c src/paging.h src/memory.h src/multiboot.h | $(EXCEPTION_TEST_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/disk.o: src/disk.c src/disk.h src/multiboot.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/fs.o: src/fs.c src/fs.h src/disk.h src/multiboot.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/paging.o: src/paging.c src/paging.h src/memory.h src/multiboot.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(DISK_IMAGE): scripts/create_disk_image.py | $(BUILD_DIR)
	python3 scripts/create_disk_image.py --output $@

$(STORAGE_TEST_DIR):
	mkdir -p $(STORAGE_TEST_DIR)

$(STORAGE_TEST_DIR)/boot.o: src/boot.S | $(STORAGE_TEST_DIR)
	$(CC) $(ASFLAGS) -c $< -o $@

$(STORAGE_TEST_DIR)/kernel.o: src/kernel.c src/multiboot.h src/memory.h src/interrupts.h src/fs.h src/paging.h | $(STORAGE_TEST_DIR)
	$(CC) $(CFLAGS) -DMICHAEL_OC_STORAGE_SELF_TEST -c $< -o $@

$(STORAGE_TEST_DIR)/memory.o: src/memory.c src/memory.h src/multiboot.h | $(STORAGE_TEST_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(STORAGE_TEST_DIR)/interrupts.o: src/interrupts.c src/interrupts.h src/multiboot.h | $(STORAGE_TEST_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(STORAGE_TEST_DIR)/isr_stubs.o: src/isr_stubs.S | $(STORAGE_TEST_DIR)
	$(CC) $(ASFLAGS) -c $< -o $@

$(STORAGE_TEST_DIR)/disk.o: src/disk.c src/disk.h src/multiboot.h | $(STORAGE_TEST_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(STORAGE_TEST_DIR)/fs.o: src/fs.c src/fs.h src/disk.h src/multiboot.h | $(STORAGE_TEST_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(STORAGE_TEST_DIR)/paging.o: src/paging.c src/paging.h src/memory.h src/multiboot.h | $(STORAGE_TEST_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(STORAGE_TEST_KERNEL): $(STORAGE_TEST_OBJECTS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(STORAGE_TEST_OBJECTS)
	$(GRUB_FILE) --is-x86-multiboot $@

$(STORAGE_TEST_ISO): $(STORAGE_TEST_KERNEL) grub/grub.cfg
	mkdir -p $(STORAGE_TEST_ISO_DIR)/boot/grub
	cp $(STORAGE_TEST_KERNEL) $(STORAGE_TEST_ISO_DIR)/boot/kernel.elf
	sed 's|multiboot /boot/kernel.elf|multiboot /boot/kernel.elf storage-test|' grub/grub.cfg > $(STORAGE_TEST_ISO_DIR)/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $@ $(STORAGE_TEST_ISO_DIR)

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

run: $(ISO) $(DISK_IMAGE)
	$(QEMU) -m 128M -cdrom $(ISO) -hda $(DISK_IMAGE) -monitor none -serial stdio -no-reboot -no-shutdown

test: $(ISO) $(DISK_IMAGE)
	@command -v $(QEMU) >/dev/null || { echo "Missing $(QEMU); install qemu-system-x86"; exit 1; }
	@command -v $(TIMEOUT) >/dev/null || { echo "Missing timeout (GNU coreutils)"; exit 1; }
	@rm -f $(BUILD_DIR)/serial.log
	@set +e; $(TIMEOUT) 5s $(QEMU) -m 128M -cdrom $(ISO) -hda $(DISK_IMAGE) -display none -monitor none -serial file:$(BUILD_DIR)/serial.log -no-reboot -no-shutdown; rc=$$?; set -e; \
	if [ "$$rc" -ne 0 ] && [ "$$rc" -ne 124 ]; then echo "FAIL: QEMU exited with status $$rc"; cat $(BUILD_DIR)/serial.log 2>/dev/null || true; exit "$$rc"; fi; \
	for marker in MICHAEL_OC_BOOT_OK MICHAEL_OC_PMM_OK MICHAEL_OC_PAGING_OK MICHAEL_OC_PAGING_SELF_TEST_OK MICHAEL_OC_FS_OK MICHAEL_OC_KEYBOARD_IRQ_OK MICHAEL_OC_MONO_TEST_OK MICHAEL_OC_INPUT_TEST_OK MICHAEL_OC_FB_OK; do \
		if ! grep -q "$$marker" $(BUILD_DIR)/serial.log; then echo "FAIL: missing $$marker"; cat $(BUILD_DIR)/serial.log 2>/dev/null || true; exit 1; fi; \
	done; \
	echo "PASS: kernel boot, 1-bit canvas, UI input logic, framebuffer output, and keyboard IRQ startup."

	python3 scripts/test_keyboard_input.py --qemu $(QEMU) --iso $(ISO) --disk $(DISK_IMAGE) --work-dir $(BUILD_DIR)
	$(MAKE) test-exception
	$(MAKE) test-storage
	$(MAKE) test-apps
	$(MAKE) test-mocapp-installer

test-exception: $(EXCEPTION_TEST_ISO)
	@command -v $(QEMU) >/dev/null || { echo "Missing $(QEMU); install qemu-system-x86"; exit 1; }
	@command -v $(TIMEOUT) >/dev/null || { echo "Missing timeout (GNU coreutils)"; exit 1; }
	python3 scripts/test_exceptions.py --qemu $(QEMU) --iso $(EXCEPTION_TEST_ISO) --work-dir $(EXCEPTION_TEST_DIR)

test-storage: $(STORAGE_TEST_ISO)
	@command -v $(QEMU) >/dev/null || { echo "Missing $(QEMU); install qemu-system-x86"; exit 1; }
	python3 scripts/test_storage.py --qemu $(QEMU) --iso $(STORAGE_TEST_ISO) --disk $(STORAGE_TEST_DIR)/test-disk.img --work-dir $(STORAGE_TEST_DIR) --create-disk-script scripts/create_disk_image.py

test-apps: $(ISO)
	@command -v $(QEMU) >/dev/null || { echo "Missing $(QEMU); install qemu-system-x86"; exit 1; }
	python3 scripts/test_apps.py --qemu $(QEMU) --iso $(ISO) --disk $(STORAGE_TEST_DIR)/ui-test-disk.img --work-dir $(STORAGE_TEST_DIR) --create-disk-script scripts/create_disk_image.py

test-mocapp-installer:
	python3 scripts/test_mocapp_installer.py

clean:
	rm -rf $(BUILD_DIR)
