/*
 * Michael OC physical page/frame allocator.
 * Runs before paging; returned physical frame addresses are directly addressable.
 * Only complete usable RAM pages below 4 GiB are tracked and released.
 */
#include "memory.h"

#define PMM_PHYSICAL_LIMIT 0x100000000ull
#define PMM_FRAME_COUNT    1048576u
#define PMM_BITMAP_BYTES   (PMM_FRAME_COUNT / 8u)
#define PMM_FRAME_SHIFT    12u
#define PMM_FRAME_MASK     (PMM_PAGE_SIZE - 1u)

static u8 used_bitmap[PMM_BITMAP_BYTES];
static u8 allocated_bitmap[PMM_BITMAP_BYTES];
static u32 free_pages;
static u32 next_hint = 256u;
static int initialized;

extern char _kernel_start;
extern char _kernel_end;

static int bitmap_get(const u8 *bitmap, u32 frame)
{
    return (bitmap[frame >> 3u] & (u8)(1u << (frame & 7u))) != 0u;
}

static void bitmap_set(u8 *bitmap, u32 frame)
{
    bitmap[frame >> 3u] |= (u8)(1u << (frame & 7u));
}

static void bitmap_clear(u8 *bitmap, u32 frame)
{
    bitmap[frame >> 3u] &= (u8)~(u8)(1u << (frame & 7u));
}

static void reset_bitmaps(void)
{
    for (u32 i = 0; i < PMM_BITMAP_BYTES; ++i) {
        used_bitmap[i] = 0xFFu;
        allocated_bitmap[i] = 0u;
    }
    free_pages = 0u;
    next_hint = 256u;
    initialized = 0;
}

static void mark_usable_range(u64 base, u64 length)
{
    if (length == 0u || base >= PMM_PHYSICAL_LIMIT) {
        return;
    }

    u64 end = base + length;
    if (end < base || end > PMM_PHYSICAL_LIMIT) {
        end = PMM_PHYSICAL_LIMIT;
    }

    /* A page is free only when it is fully inside a usable range. */
    const u64 first = (base + (u64)PMM_FRAME_MASK) & ~((u64)PMM_FRAME_MASK);
    const u64 last = end & ~((u64)PMM_FRAME_MASK);
    if (first >= last) {
        return;
    }

    u32 first_frame = (u32)(first >> PMM_FRAME_SHIFT);
    u32 last_frame = (u32)(last >> PMM_FRAME_SHIFT);
    if (last_frame > PMM_FRAME_COUNT) {
        last_frame = PMM_FRAME_COUNT;
    }

    for (u32 frame = first_frame; frame < last_frame; ++frame) {
        if (bitmap_get(used_bitmap, frame)) {
            bitmap_clear(used_bitmap, frame);
            ++free_pages;
        }
    }
}

static void reserve_range(u64 base, u64 length)
{
    if (length == 0u || base >= PMM_PHYSICAL_LIMIT) {
        return;
    }

    u64 end = base + length;
    if (end < base || end > PMM_PHYSICAL_LIMIT) {
        end = PMM_PHYSICAL_LIMIT;
    }

    const u64 first = base & ~((u64)PMM_FRAME_MASK);
    u64 last = (end + (u64)PMM_FRAME_MASK) & ~((u64)PMM_FRAME_MASK);
    if (last > PMM_PHYSICAL_LIMIT) {
        last = PMM_PHYSICAL_LIMIT;
    }

    const u32 first_frame = (u32)(first >> PMM_FRAME_SHIFT);
    u32 last_frame = (u32)(last >> PMM_FRAME_SHIFT);
    if (last_frame > PMM_FRAME_COUNT) {
        last_frame = PMM_FRAME_COUNT;
    }

    for (u32 frame = first_frame; frame < last_frame; ++frame) {
        if (!bitmap_get(used_bitmap, frame)) {
            bitmap_set(used_bitmap, frame);
            if (free_pages > 0u) {
                --free_pages;
            }
        }
        bitmap_clear(allocated_bitmap, frame);
    }
}

static int parse_memory_map(const multiboot_info_t *info)
{
    if (info == 0 ||
        (info->flags & MULTIBOOT_INFO_MEMORY_MAP) == 0u ||
        info->mmap_addr == 0u ||
        info->mmap_length < 24u) {
        return 0;
    }

    const u64 map_start = (u64)info->mmap_addr;
    const u64 map_end = map_start + (u64)info->mmap_length;
    if (map_end > PMM_PHYSICAL_LIMIT || map_end < map_start) {
        return 0;
    }

    u32 offset = 0u;
    u32 entry_count = 0u;
    while (offset < info->mmap_length) {
        const u32 remaining = info->mmap_length - offset;
        if (remaining < (u32)sizeof(u32)) {
            return 0;
        }

        const u8 *raw = (const u8 *)(info->mmap_addr + offset);
        const multiboot_mmap_entry_t *entry =
            (const multiboot_mmap_entry_t *)raw;

        if (entry->size < 20u ||
            entry->size > remaining - (u32)sizeof(u32)) {
            return 0;
        }

        const u32 total_size = entry->size + (u32)sizeof(u32);
        if (entry->type == 1u) {
            mark_usable_range(entry->addr, entry->len);
        }

        offset += total_size;
        ++entry_count;
    }

    return offset == info->mmap_length && entry_count > 0u;
}

int pmm_init(const multiboot_info_t *info)
{
    reset_bitmaps();

    if (!parse_memory_map(info)) {
        reset_bitmaps();
        return 0;
    }

    /* Reserve low memory, kernel image, Multiboot structures and display RAM. */
    reserve_range(0u, 0x100000u);

    const u64 kernel_start = (u64)(u32)&_kernel_start;
    const u64 kernel_end = (u64)(u32)&_kernel_end;
    if (kernel_end <= kernel_start) {
        reset_bitmaps();
        return 0;
    }
    reserve_range(kernel_start, kernel_end - kernel_start);

    reserve_range((u64)(u32)info, (u64)sizeof(*info));
    reserve_range((u64)info->mmap_addr, (u64)info->mmap_length);

    if ((info->flags & MULTIBOOT_INFO_MODULES) != 0u &&
        info->mods_count > 0u && info->mods_addr != 0u) {
        const u64 modules_bytes = (u64)info->mods_count * sizeof(multiboot_module_t);
        if ((u64)info->mods_addr + modules_bytes <= PMM_PHYSICAL_LIMIT) {
            reserve_range((u64)info->mods_addr, modules_bytes);
            const multiboot_module_t *modules =
                (const multiboot_module_t *)info->mods_addr;
            for (u32 i = 0; i < info->mods_count; ++i) {
                if (modules[i].mod_end >= modules[i].mod_start) {
                    reserve_range((u64)modules[i].mod_start,
                                  (u64)(modules[i].mod_end - modules[i].mod_start));
                }
            }
        }
    }

    if ((info->flags & MULTIBOOT_INFO_FRAMEBUFFER) != 0u &&
        info->framebuffer_addr < PMM_PHYSICAL_LIMIT &&
        info->framebuffer_pitch != 0u && info->framebuffer_height != 0u) {
        reserve_range(info->framebuffer_addr,
                      (u64)info->framebuffer_pitch * (u64)info->framebuffer_height);
    }

    if (free_pages == 0u) {
        reset_bitmaps();
        return 0;
    }

    initialized = 1;
    return 1;
}

u32 pmm_alloc_frame(void)
{
    if (!initialized || free_pages == 0u) {
        return 0u;
    }

    const u32 start_hint = next_hint < 256u ? 256u : next_hint;
    for (u32 pass = 0u; pass < 2u; ++pass) {
        const u32 first = pass == 0u ? start_hint : 256u;
        const u32 limit = pass == 0u ? PMM_FRAME_COUNT : start_hint;

        for (u32 frame = first; frame < limit; ++frame) {
            if (!bitmap_get(used_bitmap, frame)) {
                bitmap_set(used_bitmap, frame);
                bitmap_set(allocated_bitmap, frame);
                --free_pages;

                next_hint = frame + 1u;
                if (next_hint >= PMM_FRAME_COUNT) {
                    next_hint = 256u;
                }
                return frame << PMM_FRAME_SHIFT;
            }
        }
    }

    return 0u;
}

int pmm_free_frame(u32 physical_address)
{
    if (!initialized || physical_address == 0u ||
        (physical_address & PMM_FRAME_MASK) != 0u) {
        return 0;
    }

    const u32 frame = physical_address >> PMM_FRAME_SHIFT;
    if (frame >= PMM_FRAME_COUNT ||
        !bitmap_get(allocated_bitmap, frame) ||
        !bitmap_get(used_bitmap, frame)) {
        return 0;
    }

    bitmap_clear(allocated_bitmap, frame);
    bitmap_clear(used_bitmap, frame);
    ++free_pages;

    if (frame < next_hint) {
        next_hint = frame;
    }
    return 1;
}

u32 pmm_free_page_count(void)
{
    return initialized ? free_pages : 0u;
}

int pmm_is_initialized(void)
{
    return initialized;
}

int pmm_self_test(void)
{
    if (!initialized) {
        return 0;
    }

    const u32 before = free_pages;
    const u32 physical_address = pmm_alloc_frame();
    if (physical_address == 0u ||
        (physical_address & PMM_FRAME_MASK) != 0u ||
        free_pages + 1u != before) {
        if (physical_address != 0u) {
            (void)pmm_free_frame(physical_address);
        }
        return 0;
    }

    volatile u32 *test_word = (volatile u32 *)physical_address;
    *test_word = 0x4D4F4E4Fu;
    if (*test_word != 0x4D4F4E4Fu) {
        (void)pmm_free_frame(physical_address);
        return 0;
    }

    if (!pmm_free_frame(physical_address) ||
        free_pages != before ||
        pmm_free_frame(physical_address)) {
        return 0;
    }

    return 1;
}
