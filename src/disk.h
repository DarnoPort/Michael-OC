#ifndef MICHAEL_OC_DISK_H
#define MICHAEL_OC_DISK_H
#include "multiboot.h"
#define ATA_SECTOR_SIZE 512u
int ata_init(void);
int ata_is_initialized(void);
u32 ata_sector_count(void);
int ata_read_sector(u32 lba, void *buffer);
int ata_write_sector(u32 lba, const void *buffer);
#endif
