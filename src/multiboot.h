#ifndef MICHAEL_OC_MULTIBOOT_H
#define MICHAEL_OC_MULTIBOOT_H

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002u
#define MULTIBOOT_INFO_MODULES     (1u << 3)
#define MULTIBOOT_INFO_MEMORY_MAP  (1u << 6)
#define MULTIBOOT_INFO_FRAMEBUFFER (1u << 12)

typedef struct __attribute__((packed)) {
    u32 flags;
    u32 mem_lower;
    u32 mem_upper;
    u32 boot_device;
    u32 cmdline;
    u32 mods_count;
    u32 mods_addr;
    u32 symbols[4];
    u32 mmap_length;
    u32 mmap_addr;
    u32 drives_length;
    u32 drives_addr;
    u32 config_table;
    u32 boot_loader_name;
    u32 apm_table;
    u32 vbe_control_info;
    u32 vbe_mode_info;
    u16 vbe_mode;
    u16 vbe_interface_seg;
    u16 vbe_interface_off;
    u16 vbe_interface_len;
    u64 framebuffer_addr;
    u32 framebuffer_pitch;
    u32 framebuffer_width;
    u32 framebuffer_height;
    u8 framebuffer_bpp;
    u8 framebuffer_type;
    struct __attribute__((packed)) {
        u8 red_field_position;
        u8 red_mask_size;
        u8 green_field_position;
        u8 green_mask_size;
        u8 blue_field_position;
        u8 blue_mask_size;
    } color;
} multiboot_info_t;

typedef struct __attribute__((packed)) {
    u32 size;
    u64 addr;
    u64 len;
    u32 type;
} multiboot_mmap_entry_t;

typedef struct __attribute__((packed)) {
    u32 mod_start;
    u32 mod_end;
    u32 string;
    u32 reserved;
} multiboot_module_t;

#endif
