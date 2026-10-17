#ifndef MICHAEL_OC_PAGING_H
#define MICHAEL_OC_PAGING_H

#include "multiboot.h"

#define PAGING_FLAG_WRITABLE      0x002u
#define PAGING_FLAG_WRITE_THROUGH 0x008u
#define PAGING_FLAG_CACHE_DISABLE 0x010u

int paging_init(const multiboot_info_t *info);
int paging_is_enabled(void);
int paging_translate(u32 virtual_address, u32 *physical_address_out);
int paging_page_is_writable(u32 virtual_address);
int paging_map(u32 virtual_address, u32 physical_address, u32 flags);
int paging_unmap(u32 virtual_address);
int paging_self_test(void);

#endif
