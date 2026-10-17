/*
 * Michael OC File System v1: fixed directory table, bitmap, contiguous extents
 * and parent-index directories. It is not journaled; mount validates media
 * instead of silently formatting an unknown disk.
 */
#include "fs.h"
#include "disk.h"
#define FS_SECTOR_SIZE 512u
#define FS_MAX_SECTORS 32768u
#define FS_BITMAP_BYTES (FS_MAX_SECTORS / 8u)
#define FS_VERSION 1u
#define FS_BITMAP_START 1u
#define FS_BITMAP_SECTORS 8u
#define FS_DIRECTORY_START 9u
#define FS_DIRECTORY_SECTORS 8u
#define FS_DATA_START 17u

typedef struct __attribute__((packed)) {
    u8 magic[8];
    u32 version;
    u32 total_sectors;
    u32 bitmap_start;
    u32 bitmap_sectors;
    u32 directory_start;
    u32 directory_sectors;
    u32 data_start;
    u32 entry_count;
    u32 entry_size;
    u8 reserved[468];
} fs_superblock_t;

_Static_assert(sizeof(fs_superblock_t) == FS_SECTOR_SIZE, "bad superblock size");
_Static_assert(sizeof(fs_entry_t) == 64u, "bad directory entry size");
_Static_assert(sizeof(fs_entry_t) * FS_DIRECTORY_LIMIT ==
               FS_DIRECTORY_SECTORS * FS_SECTOR_SIZE, "bad directory geometry");
static u8 bitmap[FS_BITMAP_BYTES];
/* Scratch map used during mount to prove that every data sector has one owner. */
static u8 validation_bitmap[FS_BITMAP_BYTES];
static fs_entry_t directory[FS_DIRECTORY_LIMIT];
static u8 io_buffer[FS_SECTOR_SIZE] __attribute__((aligned(2)));
static u32 mounted_total_sectors;
static int mounted;
static int mount_bitmap_repaired;

static u32 text_length(const char *text)
{
    if (text == 0) return 0u;
    u32 len = 0u;
    while (text[len] != '\0' && len < 4096u) ++len;
    return len;
}
static int text_equal(const char *left, const char *right)
{
    if (left == 0 || right == 0) return 0;
    u32 i = 0u;
    while (left[i] != '\0' && right[i] != '\0') {
        if (left[i] != right[i]) return 0;
        ++i;
    }
    return left[i] == right[i];
}
static int valid_name(const char *name)
{
    const u32 length = text_length(name);
    if (length == 0u || length > FS_NAME_MAX ||
        text_equal(name, ".") || text_equal(name, "..")) return 0;
    for (u32 i = 0u; i < length; ++i) {
        const char ch = name[i];
        const int letter = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
        const int digit = ch >= '0' && ch <= '9';
        if (!letter && !digit && ch != '.' && ch != '_' && ch != '-' && ch != ' ')
            return 0;
    }
    return 1;
}
static int valid_stored_name(const char name[32])
{
    u32 length = 0u;
    while (length < 32u && name[length] != '\0') ++length;
    if (length == 32u) return 0;
    return valid_name(name);
}
static int bitmap_get(u32 sector)
{ return (bitmap[sector >> 3u] & (u8)(1u << (sector & 7u))) != 0u; }
static void bitmap_set(u32 sector)
{ bitmap[sector >> 3u] |= (u8)(1u << (sector & 7u)); }
static void bitmap_clear(u32 sector)
{ bitmap[sector >> 3u] &= (u8)~(u8)(1u << (sector & 7u)); }
static int persist_bitmap(void)
{
    for (u32 i = 0u; i < FS_BITMAP_SECTORS; ++i)
        if (!ata_write_sector(FS_BITMAP_START + i, &bitmap[i * FS_SECTOR_SIZE])) return 0;
    return 1;
}
static int persist_directory(void)
{
    const u8 *bytes = (const u8 *)directory;
    for (u32 i = 0u; i < FS_DIRECTORY_SECTORS; ++i)
        if (!ata_write_sector(FS_DIRECTORY_START + i, &bytes[i * FS_SECTOR_SIZE])) return 0;
    return 1;
}
static int valid_parent(u16 parent)
{
    return parent == FS_ROOT ||
           (parent < FS_DIRECTORY_LIMIT && directory[parent].type == FS_ENTRY_DIRECTORY);
}
static int find_free_entry(u16 *index_out)
{
    for (u32 i = 0u; i < FS_DIRECTORY_LIMIT; ++i)
        if (directory[i].type == 0u) { *index_out = (u16)i; return 1; }
    return 0;
}
static int name_exists(u16 parent, const char *name)
{
    for (u32 i = 0u; i < FS_DIRECTORY_LIMIT; ++i)
        if (directory[i].type != 0u && directory[i].parent_index == parent &&
            text_equal(directory[i].name, name)) return 1;
    return 0;
}
static int allocate_contiguous(u32 needed, u32 *start_out)
{
    if (needed == 0u) { *start_out = 0u; return 1; }
    if (needed > mounted_total_sectors - FS_DATA_START) return 0;
    u32 run = 0u, run_start = FS_DATA_START;
    for (u32 sector = FS_DATA_START; sector < mounted_total_sectors; ++sector) {
        if (!bitmap_get(sector)) {
            if (run == 0u) run_start = sector;
            if (++run >= needed) { *start_out = run_start; return 1; }
        } else run = 0u;
    }
    return 0;
}
static void mark_extent(u32 start, u32 count, int allocated)
{
    for (u32 i = 0u; i < count; ++i) {
        if (allocated) bitmap_set(start + i);
        else bitmap_clear(start + i);
    }
}
static int validate_parent_chain(u16 index)
{
    u16 parent = directory[index].parent_index;
    for (u32 depth = 0u; depth < FS_DIRECTORY_LIMIT; ++depth) {
        if (parent == FS_ROOT) return 1;
        if (parent >= FS_DIRECTORY_LIMIT || parent == index ||
            directory[parent].type != FS_ENTRY_DIRECTORY) return 0;
        parent = directory[parent].parent_index;
    }
    return 0;
}
static int validate_directory(void)
{
    for (u32 i = 0u; i < FS_BITMAP_BYTES; ++i) {
        validation_bitmap[i] = 0u;
    }

    /*
     * The superblock, bitmap, and directory table occupy the fixed reserved
     * prefix. Reconstruct these bits rather than trusting a potentially stale
     * bitmap left by an interrupted allocation or deletion.
     */
    for (u32 sector = 0u; sector < FS_DATA_START; ++sector) {
        validation_bitmap[sector >> 3u] |= (u8)(1u << (sector & 7u));
    }

    for (u32 i = 0u; i < FS_DIRECTORY_LIMIT; ++i) {
        const fs_entry_t *entry = &directory[i];
        if (entry->type == 0u) continue;
        if (entry->type != FS_ENTRY_FILE && entry->type != FS_ENTRY_DIRECTORY) return 0;
        if (!valid_stored_name(entry->name) || !valid_parent(entry->parent_index) ||
            !validate_parent_chain((u16)i)) return 0;

        if (entry->type == FS_ENTRY_DIRECTORY) {
            if (entry->sector_count != 0u || entry->size_bytes != 0u ||
                entry->start_sector != 0u) return 0;
        } else {
            if (entry->size_bytes > (mounted_total_sectors - FS_DATA_START) * FS_SECTOR_SIZE)
                return 0;
            const u32 required = (entry->size_bytes + FS_SECTOR_SIZE - 1u) / FS_SECTOR_SIZE;
            if (required != entry->sector_count) return 0;
            if (required == 0u) {
                if (entry->start_sector != 0u) return 0;
            } else {
                if (entry->start_sector < FS_DATA_START ||
                    entry->start_sector >= mounted_total_sectors ||
                    required > mounted_total_sectors - entry->start_sector) return 0;

                for (u32 j = 0u; j < required; ++j) {
                    const u32 sector = entry->start_sector + j;
                    const u8 mask = (u8)(1u << (sector & 7u));
                    u8 *owner_byte = &validation_bitmap[sector >> 3u];

                    /*
                     * File extents are authoritative for data-sector ownership.
                     * A missing bitmap bit can be restored. An already-owned
                     * sector cannot be assigned to a second file: that is
                     * ambiguous data ownership and mounting must fail.
                     */
                    if ((*owner_byte & mask) != 0u) return 0;
                    *owner_byte |= mask;
                }
            }
        }

        for (u32 j = i + 1u; j < FS_DIRECTORY_LIMIT; ++j) {
            if (directory[j].type != 0u &&
                directory[j].parent_index == entry->parent_index &&
                text_equal(directory[j].name, entry->name)) return 0;
        }
    }

    /*
     * The expected map is now derived from reserved structures and validated
     * file extents. Repair stale/orphan bits, missing allocation bits, and
     * out-of-range bits. This recovers common interrupted bitmap updates.
     * It is not a journal and cannot repair arbitrary torn directory records.
     */
    int bitmap_differs = 0;
    for (u32 i = 0u; i < FS_BITMAP_BYTES; ++i) {
        if (bitmap[i] != validation_bitmap[i]) {
            bitmap_differs = 1;
            break;
        }
    }

    if (bitmap_differs) {
        for (u32 i = 0u; i < FS_BITMAP_BYTES; ++i) {
            bitmap[i] = validation_bitmap[i];
        }
        if (!persist_bitmap()) return 0;
        mount_bitmap_repaired = 1;
    }

    return 1;
}
int fs_mount(void)
{
    mounted = 0;
    mount_bitmap_repaired = 0;
    mounted_total_sectors = 0u;
    if (!ata_init() || ata_sector_count() < FS_DATA_START + 1u ||
        ata_sector_count() > FS_MAX_SECTORS) return 0;
    if (!ata_read_sector(0u, io_buffer)) return 0;
    const fs_superblock_t *super = (const fs_superblock_t *)io_buffer;
    if (super->magic[0] != 'M' || super->magic[1] != 'O' ||
        super->magic[2] != 'C' || super->magic[3] != 'F' ||
        super->magic[4] != 'S' || super->magic[5] != '1' ||
        super->version != FS_VERSION || super->total_sectors != ata_sector_count() ||
        super->bitmap_start != FS_BITMAP_START || super->bitmap_sectors != FS_BITMAP_SECTORS ||
        super->directory_start != FS_DIRECTORY_START ||
        super->directory_sectors != FS_DIRECTORY_SECTORS ||
        super->data_start != FS_DATA_START || super->entry_count != FS_DIRECTORY_LIMIT ||
        super->entry_size != sizeof(fs_entry_t) || super->total_sectors > FS_MAX_SECTORS)
        return 0;
    mounted_total_sectors = super->total_sectors;
    for (u32 i = 0u; i < FS_BITMAP_SECTORS; ++i) {
        if (!ata_read_sector(FS_BITMAP_START + i, &bitmap[i * FS_SECTOR_SIZE])) {
            mounted_total_sectors = 0u; return 0;
        }
    }
    u8 *directory_bytes = (u8 *)directory;
    for (u32 i = 0u; i < FS_DIRECTORY_SECTORS; ++i) {
        if (!ata_read_sector(FS_DIRECTORY_START + i, &directory_bytes[i * FS_SECTOR_SIZE])) {
            mounted_total_sectors = 0u; return 0;
        }
    }
    if (!validate_directory()) { mounted_total_sectors = 0u; return 0; }
    mounted = 1;
    return 1;
}
int fs_is_mounted(void) { return mounted; }
int fs_mount_repaired_bitmap(void) { return mounted && mount_bitmap_repaired; }
u32 fs_free_sectors(void)
{
    if (!mounted) return 0u;
    u32 count = 0u;
    for (u32 sector = FS_DATA_START; sector < mounted_total_sectors; ++sector)
        if (!bitmap_get(sector)) ++count;
    return count;
}
int fs_lookup(u16 parent, const char *name, u16 *index_out)
{
    if (!mounted || index_out == 0 || !valid_parent(parent) || !valid_name(name)) return 0;
    for (u32 i = 0u; i < FS_DIRECTORY_LIMIT; ++i)
        if (directory[i].type != 0u && directory[i].parent_index == parent &&
            text_equal(directory[i].name, name)) { *index_out = (u16)i; return 1; }
    return 0;
}
int fs_list(u16 parent, fs_entry_t *entries_out, u32 capacity, u32 *count_out)
{
    if (!mounted || entries_out == 0 || count_out == 0 || !valid_parent(parent)) return 0;
    u32 count = 0u;
    for (u32 i = 0u; i < FS_DIRECTORY_LIMIT; ++i) {
        if (directory[i].type == 0u || directory[i].parent_index != parent) continue;
        if (count >= capacity) return 0;
        u8 *dst = (u8 *)&entries_out[count];
        const u8 *src = (const u8 *)&directory[i];
        for (u32 b = 0u; b < sizeof(fs_entry_t); ++b) dst[b] = src[b];
        ++count;
    }
    *count_out = count;
    return 1;
}
static int create_entry(u16 parent, const char *name, u8 type, u16 *index_out)
{
    if (!mounted || index_out == 0 || !valid_parent(parent) ||
        !valid_name(name) || name_exists(parent, name)) return 0;
    u16 index;
    if (!find_free_entry(&index)) return 0;
    fs_entry_t *entry = &directory[index];
    for (u32 i = 0u; i < sizeof(fs_entry_t); ++i) ((u8 *)entry)[i] = 0u;
    const u32 len = text_length(name);
    for (u32 i = 0u; i < len; ++i) entry->name[i] = name[i];
    entry->name[len] = '\0';
    entry->parent_index = parent;
    entry->type = type;
    if (!persist_directory()) { entry->type = 0u; return 0; }
    *index_out = index;
    return 1;
}
int fs_create_file(u16 parent, const char *name, u16 *index_out)
{ return create_entry(parent, name, FS_ENTRY_FILE, index_out); }
int fs_make_directory(u16 parent, const char *name, u16 *index_out)
{ return create_entry(parent, name, FS_ENTRY_DIRECTORY, index_out); }
int fs_read_file(u16 index, u8 *buffer, u32 capacity, u32 *size_out)
{
    if (!mounted || index >= FS_DIRECTORY_LIMIT ||
        directory[index].type != FS_ENTRY_FILE || size_out == 0) return 0;
    const u32 size = directory[index].size_bytes;
    if (size > capacity || (size != 0u && buffer == 0)) return 0;
    u32 copied = 0u;
    for (u32 i = 0u; i < directory[index].sector_count; ++i) {
        if (!ata_read_sector(directory[index].start_sector + i, io_buffer)) return 0;
        u32 remaining = size - copied;
        const u32 amount = remaining < FS_SECTOR_SIZE ? remaining : FS_SECTOR_SIZE;
        for (u32 b = 0u; b < amount; ++b) buffer[copied + b] = io_buffer[b];
        copied += amount;
    }
    *size_out = size;
    return 1;
}
int fs_write_file(u16 index, const u8 *buffer, u32 size)
{
    if (!mounted || index >= FS_DIRECTORY_LIMIT || directory[index].type != FS_ENTRY_FILE ||
        (size != 0u && buffer == 0) ||
        size > (mounted_total_sectors - FS_DATA_START) * FS_SECTOR_SIZE) return 0;
    const u32 needed = (size + FS_SECTOR_SIZE - 1u) / FS_SECTOR_SIZE;
    const u32 old_start = directory[index].start_sector;
    const u32 old_count = directory[index].sector_count;
    const u32 old_size = directory[index].size_bytes;
    if (needed <= old_count) {
        u32 copied = 0u;
        for (u32 i = 0u; i < needed; ++i) {
            for (u32 b = 0u; b < FS_SECTOR_SIZE; ++b) {
                const u32 pos = copied + b;
                io_buffer[b] = pos < size ? buffer[pos] : 0u;
            }
            if (!ata_write_sector(old_start + i, io_buffer)) return 0;
            copied += FS_SECTOR_SIZE;
        }
        directory[index].size_bytes = size;
        directory[index].sector_count = (u16)needed;
        directory[index].start_sector = needed == 0u ? 0u : old_start;
        if (!persist_directory()) {
            directory[index].size_bytes = old_size;
            directory[index].sector_count = (u16)old_count;
            directory[index].start_sector = old_start;
            return 0;
        }
        if (needed < old_count) {
            mark_extent(old_start + needed, old_count - needed, 0);
            if (!persist_bitmap()) return 0;
        }
        return 1;
    }
    u32 new_start;
    if (!allocate_contiguous(needed, &new_start)) return 0;
    mark_extent(new_start, needed, 1);
    if (!persist_bitmap()) { mark_extent(new_start, needed, 0); return 0; }
    u32 copied = 0u;
    for (u32 i = 0u; i < needed; ++i) {
        for (u32 b = 0u; b < FS_SECTOR_SIZE; ++b) {
            const u32 pos = copied + b;
            io_buffer[b] = pos < size ? buffer[pos] : 0u;
        }
        if (!ata_write_sector(new_start + i, io_buffer)) {
            mark_extent(new_start, needed, 0);
            (void)persist_bitmap();
            return 0;
        }
        copied += FS_SECTOR_SIZE;
    }
    directory[index].start_sector = new_start;
    directory[index].size_bytes = size;
    directory[index].sector_count = (u16)needed;
    if (!persist_directory()) {
        directory[index].start_sector = old_start;
        directory[index].size_bytes = old_size;
        directory[index].sector_count = (u16)old_count;
        mark_extent(new_start, needed, 0);
        (void)persist_bitmap();
        return 0;
    }
    if (old_count != 0u) {
        mark_extent(old_start, old_count, 0);
        if (!persist_bitmap()) return 0;
    }
    return 1;
}
int fs_rename(u16 index, const char *new_name)
{
    if (!mounted || index >= FS_DIRECTORY_LIMIT || directory[index].type == 0u ||
        !valid_name(new_name)) return 0;
    if (text_equal(directory[index].name, new_name)) return 1;
    if (name_exists(directory[index].parent_index, new_name)) return 0;
    char old_name[32];
    for (u32 i = 0u; i < sizeof(old_name); ++i) {
        old_name[i] = directory[index].name[i];
        directory[index].name[i] = 0;
    }
    const u32 len = text_length(new_name);
    for (u32 i = 0u; i < len; ++i) directory[index].name[i] = new_name[i];
    if (!persist_directory()) {
        for (u32 i = 0u; i < sizeof(old_name); ++i) directory[index].name[i] = old_name[i];
        return 0;
    }
    return 1;
}
int fs_delete(u16 index)
{
    if (!mounted || index >= FS_DIRECTORY_LIMIT || directory[index].type == 0u) return 0;
    if (directory[index].type == FS_ENTRY_DIRECTORY) {
        for (u32 i = 0u; i < FS_DIRECTORY_LIMIT; ++i)
            if (directory[i].type != 0u && directory[i].parent_index == index) return 0;
    }
    const u32 old_start = directory[index].start_sector;
    const u32 old_count = directory[index].sector_count;
    fs_entry_t old;
    const u8 *src = (const u8 *)&directory[index];
    u8 *dst = (u8 *)&old;
    for (u32 i = 0u; i < sizeof(fs_entry_t); ++i) dst[i] = src[i];
    for (u32 i = 0u; i < sizeof(fs_entry_t); ++i) ((u8 *)&directory[index])[i] = 0u;
    if (!persist_directory()) {
        for (u32 i = 0u; i < sizeof(fs_entry_t); ++i) ((u8 *)&directory[index])[i] = ((u8 *)&old)[i];
        return 0;
    }
    if (old_count != 0u) {
        mark_extent(old_start, old_count, 0);
        if (!persist_bitmap()) return 0;
    }
    return 1;
}
