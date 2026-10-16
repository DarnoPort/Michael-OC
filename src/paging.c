/*
 * Michael OC 32-bit paging bootstrap.
 *
 * All 4 GiB are initially identity-mapped using 4 KiB pages so existing kernel,
 * Multiboot, allocator and framebuffer addresses remain valid. Page zero is
 * deliberately absent. This is a kernel-only address space, not process
 * isolation; page-table sharing and dynamic table allocation come later.
 */
#include "paging.h"
#include "memory.h"

#define PAGE_ENTRIES 1024u
#define PAGE_ADDRESS_MASK 0xFFFFF000u
#define PAGE_FLAG_MASK 0x00000FFFu
#define CR0_PG 0x80000000u
#define CR0_WP 0x00010000u
#define CR4_PSE_TEST_ADDRESS 0x70000000u

static u32 page_directory[PAGE_ENTRIES] __attribute__((aligned(PAGING_PAGE_SIZE)));
static u32 page_tables[PAGE_ENTRIES][PAGE_ENTRIES]
    __attribute__((aligned(PAGING_PAGE_SIZE)));
static int paging_enabled;

static void invalidate_page(u32 virtual_address)
{
    __asm__ volatile ("invlpg (%0)" : : "r"(virtual_address) : "memory");
}

int paging_init(void)
{
    if (paging_enabled) {
        return 1;
    }

    for (u32 directory_index = 0u; directory_index < PAGE_ENTRIES; ++directory_index) {
        page_directory[directory_index] =
            ((u32)&page_tables[directory_index][0]) | PAGING_PRESENT | PAGING_WRITABLE;

        for (u32 table_index = 0u; table_index < PAGE_ENTRIES; ++table_index) {
            const u32 page_number = directory_index * PAGE_ENTRIES + table_index;
            const u32 physical_address = page_number << 12;
            page_tables[directory_index][table_index] =
                physical_address | PAGING_PRESENT | PAGING_WRITABLE;
        }
    }

    /* Catch null dereferences while keeping the rest of low memory identity-mapped. */
    page_tables[0][0] = 0u;

    const u32 directory_address = (u32)&page_directory[0];
    __asm__ volatile ("mov %0, %%cr3" : : "r"(directory_address) : "memory");

    u32 cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= CR0_PG | CR0_WP;
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0) : "memory");

    u32 cr0_readback;
    u32 cr3_readback;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0_readback));
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3_readback));
    if ((cr0_readback & CR0_PG) == 0u || cr3_readback != directory_address) {
        return 0;
    }

    paging_enabled = 1;
    return 1;
}

int paging_map_page(u32 virtual_address, u32 physical_address, u32 flags)
{
    if (!paging_enabled ||
        (virtual_address & (PAGING_PAGE_SIZE - 1u)) != 0u ||
        (physical_address & (PAGING_PAGE_SIZE - 1u)) != 0u ||
        virtual_address == 0u ||
        (flags & PAGING_PRESENT) == 0u ||
        (flags & ~(PAGING_PRESENT | PAGING_WRITABLE | PAGING_USER)) != 0u) {
        return 0;
    }

    const u32 directory_index = virtual_address >> 22;
    const u32 table_index = (virtual_address >> 12) & 0x3FFu;
    page_tables[directory_index][table_index] =
        (physical_address & PAGE_ADDRESS_MASK) | (flags & PAGE_FLAG_MASK);
    invalidate_page(virtual_address);
    return 1;
}

int paging_get_mapping(u32 virtual_address, u32 *physical_page, u32 *flags)
{
    if (!paging_enabled) {
        return 0;
    }

    const u32 directory_index = virtual_address >> 22;
    const u32 table_index = (virtual_address >> 12) & 0x3FFu;
    const u32 entry = page_tables[directory_index][table_index];
    if ((entry & PAGING_PRESENT) == 0u) {
        return 0;
    }

    if (physical_page != 0) {
        *physical_page = (entry & PAGE_ADDRESS_MASK) | (virtual_address & 0xFFFu);
    }
    if (flags != 0) {
        *flags = entry & PAGE_FLAG_MASK;
    }
    return 1;
}

int paging_unmap_page(u32 virtual_address)
{
    if (!paging_enabled ||
        (virtual_address & (PAGING_PAGE_SIZE - 1u)) != 0u ||
        virtual_address == 0u) {
        return 0;
    }

    const u32 directory_index = virtual_address >> 22;
    const u32 table_index = (virtual_address >> 12) & 0x3FFu;
    page_tables[directory_index][table_index] = 0u;
    invalidate_page(virtual_address);
    return 1;
}

int paging_self_test(void)
{
    if (!paging_enabled || paging_get_mapping(0u, 0, 0)) {
        return 0;
    }

    const u32 first_frame = pmm_alloc_frame();
    if (first_frame == 0u) {
        return 0;
    }
    const u32 second_frame = pmm_alloc_frame();
    if (second_frame == 0u) {
        (void)pmm_free_frame(first_frame);
        return 0;
    }

    const u32 test_virtual = CR4_PSE_TEST_ADDRESS;
    const u32 first_value = 0x13579BDFu;
    const u32 second_value = 0x2468ACE0u;
    volatile u32 *alias = (volatile u32 *)test_virtual;
    int ok = paging_map_page(test_virtual, first_frame,
                             PAGING_PRESENT | PAGING_WRITABLE);

    if (ok) {
        u32 mapped = 0u;
        *alias = first_value;
        ok = paging_get_mapping(test_virtual, &mapped, 0) &&
             mapped == first_frame && *(volatile u32 *)first_frame == first_value;
    }

    if (ok) {
        u32 mapped = 0u;
        ok = paging_map_page(test_virtual, second_frame,
                             PAGING_PRESENT | PAGING_WRITABLE);
        if (ok) {
            *alias = second_value;
            ok = paging_get_mapping(test_virtual, &mapped, 0) &&
                 mapped == second_frame &&
                 *(volatile u32 *)second_frame == second_value &&
                 *(volatile u32 *)first_frame == first_value;
        }
    }

    if (!paging_unmap_page(test_virtual) ||
        paging_get_mapping(test_virtual, 0, 0)) {
        ok = 0;
    }
    /* Restore the original identity mapping even if an assertion failed. */
    if (!paging_map_page(test_virtual, test_virtual,
                         PAGING_PRESENT | PAGING_WRITABLE)) {
        ok = 0;
    }
    if (!pmm_free_frame(first_frame)) {
        ok = 0;
    }
    if (!pmm_free_frame(second_frame)) {
        ok = 0;
    }
    return ok;
}
