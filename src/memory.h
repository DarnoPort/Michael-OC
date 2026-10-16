#ifndef MICHAEL_OC_MEMORY_H
#define MICHAEL_OC_MEMORY_H

#include "multiboot.h"

#define PMM_PAGE_SIZE 4096u

int pmm_init(const multiboot_info_t *info);
u32 pmm_alloc_frame(void);
int pmm_free_frame(u32 physical_address);
u32 pmm_free_page_count(void);
int pmm_is_initialized(void);
int pmm_self_test(void);

#endif
