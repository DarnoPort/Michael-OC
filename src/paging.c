/*
 * 32-bit x86 paging for Michael OC.
 *
 * The first stage deliberately uses a complete supervisor-only identity map
 * backed by 4 KiB page tables. It keeps all current physical pointers usable,
 * leaves the null page absent, protects the kernel text/rodata from writes,
 * and marks the Multiboot framebuffer uncached. This is not userspace
 * isolation: most physical addresses remain identity mapped and writable.
 */
#include "paging.h"
#include "memory.h"

#define PAGE_SIZE 4096u
#define PAGE_MASK 0xFFFFF000u
#define PAGE_PRESENT 0x001u
#define PAGE_DIRECTORY_COUNT 1024u
#define PAGE_TABLE_ENTRIES 1024u
#define PAGE_FLAG_MASK (PAGING_FLAG_WRITABLE | PAGING_FLAG_WRITE_THROUGH | PAGING_FLAG_CACHE_DISABLE)
#define CR0_WRITE_PROTECT 0x00010000u
#define CR0_PAGING_ENABLE 0x80000000u
#define TEST_VIRTUAL_ADDRESS 0xFFF00000u
#define TEST_PATTERN 0x4D4F4350u

static u32 page_directory[PAGE_DIRECTORY_COUNT]
    __attribute__((aligned(PAGE_SIZE)));
static u32 page_tables[PAGE_DIRECTORY_COUNT][PAGE_TABLE_ENTRIES]
    __attribute__((aligned(PAGE_SIZE)));
static volatile u32 paging_writable_probe __attribute__((aligned(PAGE_SIZE)));
static int paging_initialized;

extern char _kernel_start;
extern char _kernel_end;
extern char _kernel_text_end;

static u32 *page_entry_for(u32 virtual_address)
{
    const u32 directory_index = virtual_address >> 22u;
    const u32 table_index = (virtual_address >> 12u) & 0x3FFu;
    return &page_tables[directory_index][table_index];
}

int paging_is_enabled(void)
{
    u32 cr0 = 0u;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    return (cr0 & CR0_PAGING_ENABLE) != 0u;
}

static int page_is_kernel_owned(u32 virtual_address)
{
    const u32 start = (u32)&_kernel_start & PAGE_MASK;
    const u32 end = (u32)&_kernel_end;
    return virtual_address >= start && virtual_address < end;
}

static void invalidate_page(u32 virtual_address)
{
    __asm__ volatile ("invlpg (%0)" : : "r"(virtual_address) : "memory");
}

static void mark_framebuffer_uncached(const multiboot_info_t *info)
{
    if (info == 0 ||
        (info->flags & MULTIBOOT_INFO_FRAMEBUFFER) == 0u ||
        info->framebuffer_addr >= 0x100000000ull ||
        info->framebuffer_pitch == 0u ||
        info->framebuffer_height == 0u) {
        return;
    }

    const u64 base = info->framebuffer_addr;
    const u64 length = (u64)info->framebuffer_pitch * (u64)info->framebuffer_height;
    const u64 end = base + length;
    if (length == 0u || end < base || end > 0x100000000ull) return;

    const u64 first = base & ~((u64)PAGE_SIZE - 1u);
    const u64 limit = (end + PAGE_SIZE - 1u) & ~((u64)PAGE_SIZE - 1u);
    for (u64 page = first; page < limit; page += PAGE_SIZE) {
        const u32 address = (u32)page;
        u32 *entry = page_entry_for(address);
        if ((*entry & PAGE_PRESENT) != 0u)
            *entry |= PAGING_FLAG_WRITE_THROUGH | PAGING_FLAG_CACHE_DISABLE;
    }
}

int paging_init(const multiboot_info_t *info)
{
    if (paging_initialized ||
        (((u32)page_directory & (PAGE_SIZE - 1u)) != 0u) ||
        (((u32)page_tables & (PAGE_SIZE - 1u)) != 0u)) {
        return 0;
    }

    const u32 read_only_start = (u32)&_kernel_start & PAGE_MASK;
    const u32 read_only_end = (u32)&_kernel_text_end;

    for (u32 directory_index = 0u; directory_index < PAGE_DIRECTORY_COUNT;
         ++directory_index) {
        const u32 table_address = (u32)&page_tables[directory_index][0];
        page_directory[directory_index] = table_address | PAGE_PRESENT |
                                          PAGING_FLAG_WRITABLE;

        for (u32 table_index = 0u; table_index < PAGE_TABLE_ENTRIES; ++table_index) {
            const u32 address = (directory_index << 22u) |
                                (table_index << 12u);
            u32 entry = address | PAGE_PRESENT | PAGING_FLAG_WRITABLE;

            if (address == 0u) {
                entry = 0u; /* Catch null dereferences early. */
            } else if (address >= read_only_start && address < read_only_end) {
                entry &= ~PAGING_FLAG_WRITABLE;
            }
            page_tables[directory_index][table_index] = entry;
        }
    }

    mark_framebuffer_uncached(info);

    const u32 page_directory_address = (u32)page_directory;
    __asm__ volatile ("mov %0, %%cr3" : : "r"(page_directory_address) : "memory");

    u32 cr0 = 0u;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= CR0_PAGING_ENABLE | CR0_WRITE_PROTECT;
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0) : "memory");
    __asm__ volatile ("jmp 1f\n1:" : : : "memory");

    paging_initialized = 1;
    if (!paging_is_enabled()) return 0;

    u32 current_cr3 = 0u;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(current_cr3));
    return (current_cr3 & PAGE_MASK) == page_directory_address;
}

int paging_translate(u32 virtual_address, u32 *physical_address_out)
{
    if (!paging_initialized || physical_address_out == 0) return 0;
    const u32 entry = *page_entry_for(virtual_address);
    if ((entry & PAGE_PRESENT) == 0u) return 0;
    *physical_address_out = (entry & PAGE_MASK) | (virtual_address & (PAGE_SIZE - 1u));
    return 1;
}

int paging_page_is_writable(u32 virtual_address)
{
    if (!paging_initialized) return 0;
    const u32 entry = *page_entry_for(virtual_address);
    return (entry & PAGE_PRESENT) != 0u &&
           (entry & PAGING_FLAG_WRITABLE) != 0u;
}

int paging_map(u32 virtual_address, u32 physical_address, u32 flags)
{
    if (!paging_initialized ||
        (virtual_address & (PAGE_SIZE - 1u)) != 0u ||
        (physical_address & (PAGE_SIZE - 1u)) != 0u ||
        virtual_address == 0u || page_is_kernel_owned(virtual_address) ||
        (flags & ~PAGE_FLAG_MASK) != 0u) {
        return 0;
    }

    *page_entry_for(virtual_address) = (physical_address & PAGE_MASK) |
                                      PAGE_PRESENT | flags;
    invalidate_page(virtual_address);
    return 1;
}

int paging_unmap(u32 virtual_address)
{
    if (!paging_initialized ||
        (virtual_address & (PAGE_SIZE - 1u)) != 0u ||
        virtual_address == 0u || page_is_kernel_owned(virtual_address)) {
        return 0;
    }

    u32 *entry = page_entry_for(virtual_address);
    if ((*entry & PAGE_PRESENT) == 0u) return 0;
    *entry = 0u;
    invalidate_page(virtual_address);
    return 1;
}

int paging_self_test(void)
{
    if (!paging_initialized || !pmm_is_initialized()) return 0;

    u32 translated = 0u;
    if (paging_translate(0u, &translated) ||
        !paging_page_is_writable((u32)&paging_writable_probe) ||
        paging_page_is_writable((u32)&paging_init)) {
        return 0;
    }

    const u32 free_before = pmm_free_page_count();
    const u32 frame = pmm_alloc_frame();
    if (frame == 0u) return 0;

    int ok = 1;
    int mapping_changed = 0;
    if (!paging_map(TEST_VIRTUAL_ADDRESS, frame, PAGING_FLAG_WRITABLE)) {
        ok = 0;
    } else {
        mapping_changed = 1;
        volatile u32 *scratch = (volatile u32 *)TEST_VIRTUAL_ADDRESS;
        *scratch = TEST_PATTERN;
        if (*scratch != TEST_PATTERN) ok = 0;

        if (!paging_unmap(TEST_VIRTUAL_ADDRESS)) {
            ok = 0;
        } else {
            u32 ignored = 0u;
            if (paging_translate(TEST_VIRTUAL_ADDRESS, &ignored)) ok = 0;
        }

        if (paging_map(TEST_VIRTUAL_ADDRESS, TEST_VIRTUAL_ADDRESS,
                       PAGING_FLAG_WRITABLE)) {
            mapping_changed = 0;
        } else {
            ok = 0;
        }
    }

    if (mapping_changed) {
        /* Ensure the temporary mapping is not left in place after a failed check. */
        if (paging_map(TEST_VIRTUAL_ADDRESS, TEST_VIRTUAL_ADDRESS,
                       PAGING_FLAG_WRITABLE)) {
            mapping_changed = 0;
        } else {
            ok = 0;
        }
    }

    if (!pmm_free_frame(frame) || pmm_free_page_count() != free_before) ok = 0;
    if (paging_translate(TEST_VIRTUAL_ADDRESS, &translated) &&
        translated != TEST_VIRTUAL_ADDRESS) ok = 0;
    return ok;
}
