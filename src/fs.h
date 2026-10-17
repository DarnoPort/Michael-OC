#ifndef MICHAEL_OC_FS_H
#define MICHAEL_OC_FS_H
#include "multiboot.h"
#define FS_ROOT 0xFFFFu
#define FS_ENTRY_FILE 1u
#define FS_ENTRY_DIRECTORY 2u
#define FS_NAME_MAX 31u
#define FS_DIRECTORY_LIMIT 64u
typedef struct __attribute__((packed)) {
    char name[32];
    u16 parent_index;
    u8 type;
    u8 reserved;
    u32 start_sector;
    u32 size_bytes;
    u16 sector_count;
    u8 padding[18];
} fs_entry_t;
int fs_mount(void);
int fs_is_mounted(void);
int fs_mount_repaired_bitmap(void);
u32 fs_free_sectors(void);
int fs_lookup(u16 parent, const char *name, u16 *index_out);
int fs_list(u16 parent, fs_entry_t *entries_out, u32 capacity, u32 *count_out);
int fs_create_file(u16 parent, const char *name, u16 *index_out);
int fs_make_directory(u16 parent, const char *name, u16 *index_out);
int fs_read_file(u16 index, u8 *buffer, u32 capacity, u32 *size_out);
int fs_write_file(u16 index, const u8 *buffer, u32 size);
int fs_rename(u16 index, const char *new_name);
int fs_delete(u16 index);
#endif
