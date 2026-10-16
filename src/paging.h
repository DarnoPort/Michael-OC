#ifndef MICHAEL_OC_PAGING_H
#define MICHAEL_OC_PAGING_H

#include "multiboot.h"

#define PAGING_PAGE_SIZE 4096u
#define PAGING_PRESENT 0x001u
#define PAGING_WRITABLE 0x002u
#define PAGING_USER 0x004u

/* Enable supervisor-only identity mapping and leave page zero unmapped. */
int paging_init(void);

/* Map/query/unmap aligned 4 KiB pages. All addresses are 32-bit physical/virtual. */
int paging_map_page(u32 virtual_address, u32 physical_address, u32 flags);
int paging_get_mapping(u32 virtual_address, u32 *physical_page, u32 *flags);
int paging_unmap_page(u32 virtual_address);

/* Requires the physical-memory manager to be initialized first. */
int paging_self_test(void);

#endif
