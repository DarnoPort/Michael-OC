#ifndef MICHAEL_OC_FS_H
#define MICHAEL_OC_FS_H

#include "multiboot.h"

#define FS_NAME_MAX 23u
#define FS_MAX_FILE_SIZE (12u * 512u)
#define FS_TYPE_FILE 1u
#define FS_TYPE_DIRECTORY 2u

typedef struct {
    char name[24];
    u32 size;
    u8 type;
} fs_dir_entry_t;

/* Mount an existing MOCFS volume or format a blank Michael OC virtual disk. */
int fs_init(void);
int fs_is_ready(void);

/* Rooted paths, e.g. "/notes.txt" or "/docs/ideas.txt". */
int fs_create(const char *path, u8 type);
int fs_remove(const char *path); /* directories must be empty */
int fs_list(const char *path, fs_dir_entry_t *entries,
            u32 capacity, u32 *entry_count);
int fs_exists(const char *path, u8 *type, u32 *size);
int fs_read(const char *path, u32 offset, u8 *buffer,
            u32 length, u32 *bytes_read);
int fs_write(const char *path, u32 offset, const u8 *buffer, u32 length);

#endif
