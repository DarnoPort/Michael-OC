#ifndef MICHAEL_OC_ATA_H
#define MICHAEL_OC_ATA_H

#include "multiboot.h"

/* Legacy primary-master ATA PIO device. Each sector is 512 bytes. */
int ata_init(void);
u32 ata_sector_count(void);
int ata_read_sector(u32 lba, u8 *buffer);
int ata_write_sector(u32 lba, const u8 *buffer);
int ata_flush_cache(void);

/* Uses reserved scratch sector 63 and restores its previous contents. */
int ata_self_test(void);
u32 ata_self_test_failure_stage(void);
u32 ata_self_test_io_failure_stage(void);
u32 ata_last_write_failure_stage(void);
u32 ata_self_test_io_failure_stage(void);

#endif
