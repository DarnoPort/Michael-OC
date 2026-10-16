/*
 * MOCFS v1: small persistent filesystem for Michael OC's 32 MiB QEMU disk.
 *
 * Layout (512-byte sectors):
 *   LBA 0       reserved
 *   LBA 1       superblock (written last when formatting)
 *   LBA 2..17   sector allocation bitmap (one bit per LBA, 65536 sectors)
 *   LBA 18..33  128 inode records, 64 bytes each
 *   LBA 34..127 reserved; LBA 63 is used by the ATA driver's scratch test
 *   LBA 128..   allocatable data sectors
 *
 * Files and directories each have up to 12 direct sector pointers (6 KiB).
 * Directory entries are 32 bytes; paths support nested directories. This
 * first version has no journal, permissions, symbolic links or crash recovery.
 * The kernel is single-core and invokes filesystem routines outside IRQ context.
 */
#include "fs.h"
#include "ata.h"

#define FS_SECTOR_SIZE 512u
#define FS_MAX_SECTORS 65536u
#define FS_SUPER_LBA 1u
#define FS_BITMAP_LBA 2u
#define FS_BITMAP_SECTORS 16u
#define FS_INODE_LBA 18u
#define FS_INODE_COUNT 128u
#define FS_INODE_SIZE 64u
#define FS_DATA_LBA 128u
#define FS_ROOT_INODE 0u
#define FS_DIRECT_BLOCKS 12u
#define FS_MAX_FILE_BLOCKS 12u

typedef struct __attribute__((packed)) {
    u32 size;
    u32 blocks;
    u32 direct[FS_DIRECT_BLOCKS];
    u8 type;
    u8 flags;
    u16 reserved;
    u32 generation;
} fs_inode_t;

typedef struct __attribute__((packed)) {
    u32 inode;
    u8 type;
    u8 reserved[3];
    char name[24];
} fs_disk_dirent_t;

typedef struct __attribute__((packed)) {
    u8 magic[8];
    u32 version;
    u32 total_sectors;
    u32 bitmap_lba;
    u32 bitmap_sectors;
    u32 inode_lba;
    u32 inode_count;
    u32 inode_size;
    u32 data_lba;
    u32 root_inode;
} fs_superblock_t;

typedef char fs_inode_size_must_be_64[(sizeof(fs_inode_t) == FS_INODE_SIZE) ? 1 : -1];
typedef char fs_dirent_size_must_be_32[(sizeof(fs_disk_dirent_t) == 32u) ? 1 : -1];

static u8 sector_bitmap[FS_MAX_SECTORS / 8u];
static fs_inode_t inode_table[FS_INODE_COUNT];
static u8 sector_buffer[FS_SECTOR_SIZE];
static u32 volume_sectors;
static int filesystem_ready;

static void bytes_zero(void *memory, u32 count)
{
    u8 *p = (u8 *)memory;
    for (u32 i = 0u; i < count; ++i) p[i] = 0u;
}

static void bytes_copy(void *destination, const void *source, u32 count)
{
    u8 *d = (u8 *)destination;
    const u8 *s = (const u8 *)source;
    for (u32 i = 0u; i < count; ++i) d[i] = s[i];
}

static int bytes_equal(const void *left, const void *right, u32 count)
{
    const u8 *a = (const u8 *)left;
    const u8 *b = (const u8 *)right;
    for (u32 i = 0u; i < count; ++i) if (a[i] != b[i]) return 0;
    return 1;
}

static u32 string_length(const char *s)
{
    u32 length = 0u;
    if (s == 0) return 0u;
    while (s[length] != '\0') {
        if (length == 256u) return 256u;
        ++length;
    }
    return length;
}

static int string_equals(const char *left, const char *right)
{
    if (left == 0 || right == 0) return 0;
    u32 i = 0u;
    while (left[i] != '\0' && right[i] != '\0') {
        if (left[i] != right[i]) return 0;
        ++i;
        if (i > 255u) return 0;
    }
    return left[i] == '\0' && right[i] == '\0';
}

static int disk_name_equals(const char stored[24], const char *name)
{
    if (name == 0) return 0;
    for (u32 i = 0u; i < 24u; ++i) {
        if (stored[i] != name[i]) return 0;
        if (stored[i] == '\0') return 1;
    }
    return 0; /* Reject malformed on-disk names lacking a terminator. */
}

static int bitmap_test(u32 lba)
{
    return (sector_bitmap[lba >> 3] & (u8)(1u << (lba & 7u))) != 0u;
}

static void bitmap_change(u32 lba, int used)
{
    const u8 mask = (u8)(1u << (lba & 7u));
    if (used) sector_bitmap[lba >> 3] |= mask;
    else sector_bitmap[lba >> 3] &= (u8)~mask;
}

static int bitmap_flush_sector_for_lba(u32 lba)
{
    const u32 bitmap_sector = (lba >> 3) / FS_SECTOR_SIZE;
    if (bitmap_sector >= FS_BITMAP_SECTORS) return 0;
    return ata_write_sector(FS_BITMAP_LBA + bitmap_sector,
                            &sector_bitmap[bitmap_sector * FS_SECTOR_SIZE]);
}

static int bitmap_flush_all(void)
{
    for (u32 i = 0u; i < FS_BITMAP_SECTORS; ++i) {
        if (!ata_write_sector(FS_BITMAP_LBA + i,
                              &sector_bitmap[i * FS_SECTOR_SIZE])) return 0;
    }
    return 1;
}

static int inode_flush(u32 inode_number)
{
    if (inode_number >= FS_INODE_COUNT) return 0;
    const u32 first_inode = (inode_number / 8u) * 8u;
    const u32 sector_index = first_inode / 8u;
    return ata_write_sector(FS_INODE_LBA + sector_index,
                            (const u8 *)&inode_table[first_inode]);
}

static int inode_flush_all(void)
{
    for (u32 i = 0u; i < FS_INODE_COUNT / 8u; ++i) {
        if (!ata_write_sector(FS_INODE_LBA + i,
                              (const u8 *)&inode_table[i * 8u])) return 0;
    }
    return 1;
}

static int lba_valid_data(u32 lba)
{
    return lba >= FS_DATA_LBA && lba < volume_sectors;
}

static int zero_sector(u32 lba)
{
    bytes_zero(sector_buffer, FS_SECTOR_SIZE);
    return ata_write_sector(lba, sector_buffer);
}

static int allocate_data_sector(u32 *result)
{
    if (result == 0) return 0;
    for (u32 lba = FS_DATA_LBA; lba < volume_sectors; ++lba) {
        if (bitmap_test(lba)) continue;
        bitmap_change(lba, 1);
        if (!bitmap_flush_sector_for_lba(lba)) {
            bitmap_change(lba, 0);
            return 0;
        }
        if (!zero_sector(lba)) {
            bitmap_change(lba, 0);
            (void)bitmap_flush_sector_for_lba(lba);
            return 0;
        }
        *result = lba;
        return 1;
    }
    return 0;
}

static int release_data_sector(u32 lba)
{
    if (!lba_valid_data(lba) || !bitmap_test(lba)) return 0;
    bitmap_change(lba, 0);
    if (!bitmap_flush_sector_for_lba(lba)) {
        bitmap_change(lba, 1);
        return 0;
    }
    return 1;
}

static int inode_allocate(u32 *result)
{
    if (result == 0) return 0;
    for (u32 i = 1u; i < FS_INODE_COUNT; ++i) {
        if (inode_table[i].type == 0u) {
            bytes_zero(&inode_table[i], sizeof(inode_table[i]));
            inode_table[i].generation = 1u;
            *result = i;
            return 1;
        }
    }
    return 0;
}

/* Resolve one path segment; return 1, 0 at end, or -1 for invalid input. */
static int path_component(const char **cursor, char name[24], int *has_more)
{
    while (**cursor == '/') ++(*cursor);
    if (**cursor == '\0') return 0;

    u32 length = 0u;
    while (**cursor != '\0' && **cursor != '/') {
        if (length >= FS_NAME_MAX) return -1;
        name[length++] = **cursor;
        ++(*cursor);
    }
    name[length] = '\0';
    while (**cursor == '/') ++(*cursor);
    *has_more = **cursor != '\0';

    if (string_equals(name, ".") || string_equals(name, "..")) return -1;
    return 1;
}

static int directory_find(u32 directory_number, const char *name,
                          u32 *inode_number, u32 *sector_lba,
                          u32 *slot_index)
{
    if (directory_number >= FS_INODE_COUNT || name == 0) return 0;
    fs_inode_t *directory = &inode_table[directory_number];
    if (directory->type != FS_TYPE_DIRECTORY ||
        directory->blocks > FS_DIRECT_BLOCKS) return 0;

    for (u32 b = 0u; b < directory->blocks; ++b) {
        const u32 lba = directory->direct[b];
        if (!lba_valid_data(lba) || !ata_read_sector(lba, sector_buffer)) return 0;
        const fs_disk_dirent_t *entries = (const fs_disk_dirent_t *)sector_buffer;
        for (u32 e = 0u; e < FS_SECTOR_SIZE / sizeof(fs_disk_dirent_t); ++e) {
            if (entries[e].inode != 0u && disk_name_equals(entries[e].name, name)) {
                if (inode_number != 0) *inode_number = entries[e].inode;
                if (sector_lba != 0) *sector_lba = lba;
                if (slot_index != 0) *slot_index = e;
                return 1;
            }
        }
    }
    return 0;
}

static int resolve_path(const char *path, u32 *result)
{
    if (path == 0 || result == 0 || string_length(path) >= 256u) return 0;
    const char *cursor = path;
    u32 current = FS_ROOT_INODE;
    char name[24];
    int more = 0;
    int parsed;

    while ((parsed = path_component(&cursor, name, &more)) > 0) {
        u32 child = 0u;
        if (!directory_find(current, name, &child, 0, 0) ||
            child >= FS_INODE_COUNT || inode_table[child].type == 0u) return 0;
        if (more && inode_table[child].type != FS_TYPE_DIRECTORY) return 0;
        current = child;
    }
    if (parsed < 0) return 0;
    *result = current;
    return 1;
}

static int resolve_parent(const char *path, u32 *parent, char name[24])
{
    if (path == 0 || parent == 0 || string_length(path) >= 256u) return 0;
    const char *cursor = path;
    u32 current = FS_ROOT_INODE;
    char component[24];
    int more = 0;
    int parsed = path_component(&cursor, component, &more);
    if (parsed <= 0) return 0;

    for (;;) {
        if (!more) {
            bytes_copy(name, component, string_length(component) + 1u);
            *parent = current;
            return 1;
        }

        u32 child = 0u;
        if (!directory_find(current, component, &child, 0, 0) ||
            child >= FS_INODE_COUNT ||
            inode_table[child].type != FS_TYPE_DIRECTORY) return 0;
        current = child;
        parsed = path_component(&cursor, component, &more);
        if (parsed <= 0) return 0;
    }
}

static int directory_insert(u32 parent_number, const char *name, u32 child,
                            u8 type)
{
    if (parent_number >= FS_INODE_COUNT || child == 0u ||
        child >= FS_INODE_COUNT || name == 0) return 0;
    fs_inode_t *parent = &inode_table[parent_number];
    if (parent->type != FS_TYPE_DIRECTORY ||
        parent->blocks > FS_DIRECT_BLOCKS) return 0;

    for (;;) {
        for (u32 b = 0u; b < parent->blocks; ++b) {
            const u32 lba = parent->direct[b];
            if (!lba_valid_data(lba) || !ata_read_sector(lba, sector_buffer)) return 0;
            fs_disk_dirent_t *entries = (fs_disk_dirent_t *)sector_buffer;
            for (u32 slot = 0u; slot < FS_SECTOR_SIZE / sizeof(fs_disk_dirent_t); ++slot) {
                if (entries[slot].inode != 0u) continue;
                bytes_zero(&entries[slot], sizeof(entries[slot]));
                entries[slot].inode = child;
                entries[slot].type = type;
                bytes_copy(entries[slot].name, name, string_length(name) + 1u);
                if (!ata_write_sector(lba, sector_buffer)) return 0;
                parent->size += (u32)sizeof(fs_disk_dirent_t);
                return inode_flush(parent_number);
            }
        }

        if (parent->blocks >= FS_DIRECT_BLOCKS) return 0;
        u32 new_lba = 0u;
        if (!allocate_data_sector(&new_lba)) return 0;
        parent->direct[parent->blocks++] = new_lba;
        if (!inode_flush(parent_number)) return 0;
    }
}

static int directory_remove_entry(u32 parent_number, const char *name,
                                  u32 expected_inode)
{
    u32 found_inode = 0u, lba = 0u, slot = 0u;
    if (!directory_find(parent_number, name, &found_inode, &lba, &slot) ||
        found_inode != expected_inode || !ata_read_sector(lba, sector_buffer)) return 0;

    fs_disk_dirent_t *entries = (fs_disk_dirent_t *)sector_buffer;
    bytes_zero(&entries[slot], sizeof(entries[slot]));
    if (!ata_write_sector(lba, sector_buffer)) return 0;

    fs_inode_t *parent = &inode_table[parent_number];
    if (parent->size >= sizeof(fs_disk_dirent_t)) parent->size -= (u32)sizeof(fs_disk_dirent_t);
    return inode_flush(parent_number);
}

int fs_init(void)
{
    filesystem_ready = 0;
    volume_sectors = ata_sector_count();
    if (volume_sectors > FS_MAX_SECTORS) volume_sectors = FS_MAX_SECTORS;
    if (volume_sectors <= FS_DATA_LBA + 1u) return 0;

    if (!ata_read_sector(FS_SUPER_LBA, sector_buffer)) return 0;

    static const u8 expected_magic[8] = {'M','O','C','F','S','1',0,0};
    if (bytes_equal(sector_buffer, expected_magic, sizeof(expected_magic))) {
        const fs_superblock_t *super = (const fs_superblock_t *)sector_buffer;
        if (super->version != 1u ||
            super->total_sectors < FS_DATA_LBA + 1u ||
            super->total_sectors > volume_sectors ||
            super->bitmap_lba != FS_BITMAP_LBA ||
            super->bitmap_sectors != FS_BITMAP_SECTORS ||
            super->inode_lba != FS_INODE_LBA ||
            super->inode_count != FS_INODE_COUNT ||
            super->inode_size != FS_INODE_SIZE ||
            super->data_lba != FS_DATA_LBA ||
            super->root_inode != FS_ROOT_INODE) return 0;

        volume_sectors = super->total_sectors;
        for (u32 i = 0u; i < FS_BITMAP_SECTORS; ++i) {
            if (!ata_read_sector(FS_BITMAP_LBA + i,
                                 &sector_bitmap[i * FS_SECTOR_SIZE])) return 0;
        }
        for (u32 i = 0u; i < FS_INODE_COUNT / 8u; ++i) {
            if (!ata_read_sector(FS_INODE_LBA + i,
                                 (u8 *)&inode_table[i * 8u])) return 0;
        }

        const fs_inode_t *root = &inode_table[FS_ROOT_INODE];
        if (root->type != FS_TYPE_DIRECTORY || root->blocks == 0u ||
            root->blocks > FS_DIRECT_BLOCKS ||
            !lba_valid_data(root->direct[0]) ||
            !bitmap_test(root->direct[0])) return 0;

        filesystem_ready = 1;
        return 1;
    }

    /* Only an all-zero superblock is considered a new disk; unknown media is not reformatted. */
    for (u32 i = 0u; i < FS_SECTOR_SIZE; ++i) {
        if (sector_buffer[i] != 0u) return 0;
    }

    bytes_zero(sector_bitmap, sizeof(sector_bitmap));
    bytes_zero(inode_table, sizeof(inode_table));
    for (u32 lba = 0u; lba < FS_DATA_LBA; ++lba) bitmap_change(lba, 1);
    bitmap_change(FS_DATA_LBA, 1);

    fs_inode_t *root = &inode_table[FS_ROOT_INODE];
    root->type = FS_TYPE_DIRECTORY;
    root->blocks = 1u;
    root->direct[0] = FS_DATA_LBA;
    root->generation = 1u;

    if (!bitmap_flush_all() || !inode_flush_all() ||
        !zero_sector(FS_DATA_LBA)) return 0;

    bytes_zero(sector_buffer, FS_SECTOR_SIZE);
    fs_superblock_t *super = (fs_superblock_t *)sector_buffer;
    const u8 magic[8] = {'M','O','C','F','S','1',0,0};
    bytes_copy(super->magic, magic, sizeof(magic));
    super->version = 1u;
    super->total_sectors = volume_sectors;
    super->bitmap_lba = FS_BITMAP_LBA;
    super->bitmap_sectors = FS_BITMAP_SECTORS;
    super->inode_lba = FS_INODE_LBA;
    super->inode_count = FS_INODE_COUNT;
    super->inode_size = FS_INODE_SIZE;
    super->data_lba = FS_DATA_LBA;
    super->root_inode = FS_ROOT_INODE;
    if (!ata_write_sector(FS_SUPER_LBA, sector_buffer)) return 0;

    filesystem_ready = 1;
    return 1;
}

int fs_is_ready(void)
{
    return filesystem_ready;
}

int fs_exists(const char *path, u8 *type, u32 *size)
{
    if (!filesystem_ready) return 0;
    u32 inode_number = 0u;
    if (!resolve_path(path, &inode_number)) return 0;
    if (type != 0) *type = inode_table[inode_number].type;
    if (size != 0) *size = inode_table[inode_number].size;
    return 1;
}

int fs_create(const char *path, u8 type)
{
    if (!filesystem_ready ||
        (type != FS_TYPE_FILE && type != FS_TYPE_DIRECTORY)) return 0;
    u32 parent = 0u;
    char name[24];
    if (!resolve_parent(path, &parent, name)) return 0;
    if (directory_find(parent, name, 0, 0, 0)) return 0;

    u32 number = 0u;
    if (!inode_allocate(&number)) return 0;
    inode_table[number].type = type;

    if (type == FS_TYPE_DIRECTORY) {
        u32 data_lba = 0u;
        if (!allocate_data_sector(&data_lba)) {
            bytes_zero(&inode_table[number], sizeof(inode_table[number]));
            return 0;
        }
        inode_table[number].blocks = 1u;
        inode_table[number].direct[0] = data_lba;
    }

    if (!inode_flush(number)) {
        if (inode_table[number].type == FS_TYPE_DIRECTORY) {
            (void)release_data_sector(inode_table[number].direct[0]);
        }
        bytes_zero(&inode_table[number], sizeof(inode_table[number]));
        (void)inode_flush(number);
        return 0;
    }

    if (!directory_insert(parent, name, number, type)) {
        for (u32 i = 0u; i < inode_table[number].blocks; ++i) {
            (void)release_data_sector(inode_table[number].direct[i]);
        }
        bytes_zero(&inode_table[number], sizeof(inode_table[number]));
        (void)inode_flush(number);
        return 0;
    }
    return 1;
}

int fs_remove(const char *path)
{
    if (!filesystem_ready) return 0;
    u32 parent = 0u;
    char name[24];
    if (!resolve_parent(path, &parent, name)) return 0;

    u32 number = 0u;
    if (!directory_find(parent, name, &number, 0, 0) ||
        number == FS_ROOT_INODE || number >= FS_INODE_COUNT) return 0;
    fs_inode_t *node = &inode_table[number];
    if (node->type == FS_TYPE_DIRECTORY && node->size != 0u) return 0;
    if (node->blocks > FS_DIRECT_BLOCKS) return 0;

    if (!directory_remove_entry(parent, name, number)) return 0;
    for (u32 i = 0u; i < node->blocks; ++i) {
        if (!release_data_sector(node->direct[i])) return 0;
    }
    bytes_zero(node, sizeof(*node));
    return inode_flush(number);
}

int fs_list(const char *path, fs_dir_entry_t *entries,
            u32 capacity, u32 *entry_count)
{
    if (!filesystem_ready || entry_count == 0 ||
        (capacity != 0u && entries == 0)) return 0;
    *entry_count = 0u;

    u32 number = 0u;
    if (!resolve_path(path, &number) ||
        inode_table[number].type != FS_TYPE_DIRECTORY ||
        inode_table[number].blocks > FS_DIRECT_BLOCKS) return 0;

    u32 total = 0u;
    for (u32 b = 0u; b < inode_table[number].blocks; ++b) {
        const u32 lba = inode_table[number].direct[b];
        if (!lba_valid_data(lba) || !ata_read_sector(lba, sector_buffer)) return 0;
        const fs_disk_dirent_t *disk_entries =
            (const fs_disk_dirent_t *)sector_buffer;
        for (u32 e = 0u; e < FS_SECTOR_SIZE / sizeof(fs_disk_dirent_t); ++e) {
            const u32 child = disk_entries[e].inode;
            if (child == 0u) continue;
            if (child >= FS_INODE_COUNT || inode_table[child].type == 0u) return 0;
            if (total < capacity) {
                bytes_zero(&entries[total], sizeof(entries[total]));
                bytes_copy(entries[total].name, disk_entries[e].name,
                           sizeof(disk_entries[e].name));
                entries[total].type = inode_table[child].type;
                entries[total].size = inode_table[child].size;
            }
            ++total;
        }
    }
    *entry_count = total;
    return total <= capacity;
}

int fs_read(const char *path, u32 offset, u8 *buffer,
            u32 length, u32 *bytes_read)
{
    if (!filesystem_ready || (length != 0u && buffer == 0)) return 0;
    if (bytes_read != 0) *bytes_read = 0u;

    u32 number = 0u;
    if (!resolve_path(path, &number) ||
        inode_table[number].type != FS_TYPE_FILE) return 0;
    const fs_inode_t *node = &inode_table[number];
    if (offset >= node->size || length == 0u) return 1;
    if (node->blocks > FS_MAX_FILE_BLOCKS) return 0;

    u32 remaining = node->size - offset;
    if (remaining > length) remaining = length;
    const u32 requested = remaining;
    u32 copied = 0u;
    while (remaining != 0u) {
        const u32 file_position = offset + copied;
        const u32 block_index = file_position / FS_SECTOR_SIZE;
        const u32 block_offset = file_position % FS_SECTOR_SIZE;
        if (block_index >= node->blocks ||
            !lba_valid_data(node->direct[block_index]) ||
            !ata_read_sector(node->direct[block_index], sector_buffer)) return 0;
        u32 chunk = FS_SECTOR_SIZE - block_offset;
        if (chunk > remaining) chunk = remaining;
        bytes_copy(&buffer[copied], &sector_buffer[block_offset], chunk);
        copied += chunk;
        remaining -= chunk;
    }
    if (bytes_read != 0) *bytes_read = requested;
    return 1;
}

int fs_write(const char *path, u32 offset, const u8 *buffer, u32 length)
{
    if (!filesystem_ready || (length != 0u && buffer == 0) ||
        offset > FS_MAX_FILE_SIZE || length > FS_MAX_FILE_SIZE - offset) return 0;

    u32 number = 0u;
    if (!resolve_path(path, &number) ||
        inode_table[number].type != FS_TYPE_FILE) return 0;
    fs_inode_t *node = &inode_table[number];
    if (node->blocks > FS_MAX_FILE_BLOCKS) return 0;
    if (length == 0u) return 1;

    const u32 new_size = offset + length > node->size ? offset + length : node->size;
    const u32 required_blocks = (new_size + FS_SECTOR_SIZE - 1u) / FS_SECTOR_SIZE;
    while (node->blocks < required_blocks) {
        if (node->blocks >= FS_MAX_FILE_BLOCKS) return 0;
        u32 lba = 0u;
        if (!allocate_data_sector(&lba)) return 0;
        node->direct[node->blocks++] = lba;
        if (!inode_flush(number)) return 0;
    }

    u32 consumed = 0u;
    while (consumed < length) {
        const u32 file_position = offset + consumed;
        const u32 block_index = file_position / FS_SECTOR_SIZE;
        const u32 block_offset = file_position % FS_SECTOR_SIZE;
        if (block_index >= node->blocks ||
            !lba_valid_data(node->direct[block_index])) return 0;

        u32 chunk = FS_SECTOR_SIZE - block_offset;
        if (chunk > length - consumed) chunk = length - consumed;

        if (block_offset == 0u && chunk == FS_SECTOR_SIZE) {
            if (!ata_write_sector(node->direct[block_index], &buffer[consumed])) return 0;
        } else {
            if (!ata_read_sector(node->direct[block_index], sector_buffer)) return 0;
            bytes_copy(&sector_buffer[block_offset], &buffer[consumed], chunk);
            if (!ata_write_sector(node->direct[block_index], sector_buffer)) return 0;
        }
        consumed += chunk;
    }

    if (new_size != node->size) {
        node->size = new_size;
        if (!inode_flush(number)) return 0;
    }
    return 1;
}
