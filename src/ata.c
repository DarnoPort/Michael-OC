/*
 * Michael OC legacy ATA PIO driver.
 * This is polled LBA28 I/O; ATA IRQ14 remains masked. Sector 63 is reserved
 * for a nondestructive read/write test and must not be allocated to the FS.
 */
#include "ata.h"

#define ATA_DATA 0x1F0u
#define ATA_ERROR_FEATURES 0x1F1u
#define ATA_SECTOR_COUNT 0x1F2u
#define ATA_LBA_LOW 0x1F3u
#define ATA_LBA_MID 0x1F4u
#define ATA_LBA_HIGH 0x1F5u
#define ATA_DEVICE 0x1F6u
#define ATA_STATUS_COMMAND 0x1F7u
#define ATA_ALT_STATUS_CONTROL 0x3F6u

#define ATA_CMD_READ 0x20u
#define ATA_CMD_WRITE 0x30u
#define ATA_CMD_IDENTIFY 0xECu
#define ATA_CMD_FLUSH_CACHE 0xE7u
#define ATA_CMD_FLUSH_CACHE_EXT 0xEAu
#define ATA_FLUSH_WAIT_LIMIT 100000000u

#define ATA_STATUS_ERR 0x01u
#define ATA_STATUS_DRQ 0x08u
#define ATA_STATUS_DF 0x20u
#define ATA_STATUS_BSY 0x80u
#define ATA_LBA28_LIMIT 0x10000000u
#define ATA_WAIT_LIMIT 1000000u
#define ATA_TEST_LBA 63u

static u16 identify_words[256];
static u8 original_sector[512];
static u8 pattern_sector[512];
static u8 verify_sector[512];
static u32 disk_sectors;
static int device_ready;
static u8 flush_command;
static u32 self_test_failure_stage;
static u32 self_test_io_failure_stage;
static u32 write_failure_stage;

static inline void outb(u16 port, u8 value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline u8 inb(u16 port)
{
    u8 value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline u16 inw(u16 port)
{
    u16 value;
    __asm__ volatile ("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outw(u16 port, u16 value)
{
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static void ata_delay_400ns(void)
{
    (void)inb(ATA_ALT_STATUS_CONTROL);
    (void)inb(ATA_ALT_STATUS_CONTROL);
    (void)inb(ATA_ALT_STATUS_CONTROL);
    (void)inb(ATA_ALT_STATUS_CONTROL);
}

static int ata_wait_data_request(void)
{
    for (u32 i = 0u; i < ATA_WAIT_LIMIT; ++i) {
        const u8 status = inb(ATA_STATUS_COMMAND);
        if (status == 0u || status == 0xFFu) {
            return 0;
        }
        if ((status & ATA_STATUS_BSY) != 0u) {
            continue;
        }
        if ((status & (ATA_STATUS_ERR | ATA_STATUS_DF)) != 0u) {
            return 0;
        }
        if ((status & ATA_STATUS_DRQ) != 0u) {
            return 1;
        }
    }
    return 0;
}

/* Wait until a command has transferred its data and cleared DRQ. */
static int ata_wait_command_complete(void)
{
    for (u32 i = 0u; i < ATA_WAIT_LIMIT; ++i) {
        const u8 status = inb(ATA_STATUS_COMMAND);
        if (status == 0u || status == 0xFFu) return 0;
        if ((status & ATA_STATUS_BSY) != 0u) continue;
        if ((status & (ATA_STATUS_ERR | ATA_STATUS_DF)) != 0u) return 0;
        if ((status & ATA_STATUS_DRQ) != 0u) continue;
        return 1;
    }
    return 0;
}

int ata_init(void)
{
    device_ready = 0;
    flush_command = 0u;
    disk_sectors = 0u;

    outb(ATA_DEVICE, 0xA0u); /* Primary master, IDENTIFY command. */
    ata_delay_400ns();
    outb(ATA_SECTOR_COUNT, 0u);
    outb(ATA_LBA_LOW, 0u);
    outb(ATA_LBA_MID, 0u);
    outb(ATA_LBA_HIGH, 0u);
    outb(ATA_STATUS_COMMAND, ATA_CMD_IDENTIFY);

    u8 status = inb(ATA_STATUS_COMMAND);
    if (status == 0u || status == 0xFFu) {
        return 0;
    }

    int drq_ready = 0;
    for (u32 i = 0u; i < ATA_WAIT_LIMIT; ++i) {
        status = inb(ATA_STATUS_COMMAND);
        if (status == 0u || status == 0xFFu) {
            return 0;
        }
        if ((status & ATA_STATUS_BSY) != 0u) {
            continue;
        }
        if ((status & ATA_STATUS_ERR) != 0u) {
            return 0;
        }
        if ((status & ATA_STATUS_DRQ) != 0u) {
            drq_ready = 1;
            break;
        }
    }
    if (!drq_ready) {
        return 0;
    }

    for (u32 i = 0u; i < 256u; ++i) {
        identify_words[i] = inw(ATA_DATA);
    }

    if ((identify_words[49] & 0x0200u) == 0u) {
        return 0; /* LBA is required by this driver's addressing scheme. */
    }

    /*
     * IDENTIFY word 83's command-set validity bits must be 01 before its
     * feature bits are trusted. Some older devices do not advertise flushing.
     */
    if ((identify_words[83] & 0xC000u) == 0x4000u) {
        if ((identify_words[83] & 0x1000u) != 0u) {
            flush_command = ATA_CMD_FLUSH_CACHE;
        } else if ((identify_words[83] & 0x2000u) != 0u) {
            flush_command = ATA_CMD_FLUSH_CACHE_EXT;
        }
    }

    disk_sectors = (u32)identify_words[60] |
                   ((u32)identify_words[61] << 16);
    if (disk_sectors <= ATA_TEST_LBA || disk_sectors > ATA_LBA28_LIMIT) {
        disk_sectors = 0u;
        return 0;
    }

    device_ready = 1;
    return 1;
}

u32 ata_sector_count(void)
{
    return device_ready ? disk_sectors : 0u;
}

/* Flush only when the device advertises a supported command in IDENTIFY. */
int ata_flush_cache(void)
{
    if (!device_ready) return 0;
    if (flush_command == 0u) return 1;
    if (!ata_wait_command_complete()) return 0;

    outb(ATA_STATUS_COMMAND, flush_command);
    ata_delay_400ns();
    for (u32 i = 0u; i < ATA_FLUSH_WAIT_LIMIT; ++i) {
        const u8 status = inb(ATA_STATUS_COMMAND);
        if (status == 0u || status == 0xFFu) return 0;
        if ((status & ATA_STATUS_BSY) != 0u) continue;
        if ((status & (ATA_STATUS_ERR | ATA_STATUS_DF)) != 0u) return 0;
        if ((status & ATA_STATUS_DRQ) != 0u) continue;
        return 1;
    }
    return 0;
}

int ata_read_sector(u32 lba, u8 *buffer)
{
    if (!device_ready || buffer == 0 || lba >= disk_sectors ||
        lba >= ATA_LBA28_LIMIT || !ata_wait_command_complete()) {
        return 0;
    }

    outb(ATA_DEVICE, (u8)(0xE0u | ((lba >> 24) & 0x0Fu)));
    ata_delay_400ns();
    outb(ATA_SECTOR_COUNT, 1u);
    outb(ATA_LBA_LOW, (u8)(lba & 0xFFu));
    outb(ATA_LBA_MID, (u8)((lba >> 8) & 0xFFu));
    outb(ATA_LBA_HIGH, (u8)((lba >> 16) & 0xFFu));
    outb(ATA_STATUS_COMMAND, ATA_CMD_READ);

    if (!ata_wait_data_request()) {
        return 0;
    }
    for (u32 i = 0u; i < 256u; ++i) {
        const u16 word = inw(ATA_DATA);
        buffer[i * 2u] = (u8)(word & 0xFFu);
        buffer[i * 2u + 1u] = (u8)(word >> 8);
    }
    ata_delay_400ns();
    return ata_wait_command_complete();
}

int ata_write_sector(u32 lba, const u8 *buffer)
{
    write_failure_stage = 0u;
    if (!device_ready || buffer == 0 || lba >= disk_sectors ||
        lba >= ATA_LBA28_LIMIT) {
        write_failure_stage = 1u;
        return 0;
    }
    if (!ata_wait_command_complete()) {
        write_failure_stage = 2u;
        return 0;
    }

    outb(ATA_DEVICE, (u8)(0xE0u | ((lba >> 24) & 0x0Fu)));
    ata_delay_400ns();
    outb(ATA_SECTOR_COUNT, 1u);
    outb(ATA_LBA_LOW, (u8)(lba & 0xFFu));
    outb(ATA_LBA_MID, (u8)((lba >> 8) & 0xFFu));
    outb(ATA_LBA_HIGH, (u8)((lba >> 16) & 0xFFu));
    outb(ATA_STATUS_COMMAND, ATA_CMD_WRITE);

    if (!ata_wait_data_request()) {
        write_failure_stage = 3u;
        return 0;
    }
    for (u32 i = 0u; i < 256u; ++i) {
        const u16 word = (u16)buffer[i * 2u] |
                         ((u16)buffer[i * 2u + 1u] << 8);
        outw(ATA_DATA, word);
    }
    ata_delay_400ns();
    if (!ata_wait_command_complete()) {
        write_failure_stage = 4u;
        return 0;
    }
    return 1;
}

u32 ata_last_write_failure_stage(void)
{
    return write_failure_stage;
}

static int sector_matches(const u8 *left, const u8 *right)
{
    for (u32 i = 0u; i < 512u; ++i) {
        if (left[i] != right[i]) {
            return 0;
        }
    }
    return 1;
}

int ata_self_test(void)
{
    self_test_failure_stage = 0u;
    self_test_io_failure_stage = 0u;
    if (!device_ready || !ata_read_sector(ATA_TEST_LBA, original_sector)) {
        self_test_failure_stage = 1u;
        return 0;
    }

    for (u32 i = 0u; i < 512u; ++i) {
        pattern_sector[i] = (u8)((i * 17u) ^ (i >> 1) ^ 0x5Au);
    }

    const int write_ok = ata_write_sector(ATA_TEST_LBA, pattern_sector);
    const int flush_ok = write_ok && ata_flush_cache();
    const int read_ok = flush_ok && ata_read_sector(ATA_TEST_LBA, verify_sector);
    if (!write_ok) {
        self_test_failure_stage = 20u + ata_last_write_failure_stage();
        self_test_io_failure_stage = ata_last_write_failure_stage();
    } else if (!flush_ok) self_test_failure_stage = 28u;
    else if (!read_ok) self_test_failure_stage = 3u;
    else if (!sector_matches(pattern_sector, verify_sector)) self_test_failure_stage = 4u;

    /* Always attempt to restore the original scratch-sector contents. */
    const int restore_write_ok = ata_write_sector(ATA_TEST_LBA, original_sector);
    const int restore_flush_ok = restore_write_ok && ata_flush_cache();
    const int restore_read_ok =
        restore_flush_ok && ata_read_sector(ATA_TEST_LBA, verify_sector);
    if (self_test_failure_stage == 0u && !restore_write_ok) {
        self_test_failure_stage = 5u;
    } else if (self_test_failure_stage == 0u && !restore_flush_ok) {
        self_test_failure_stage = 8u;
    } else if (self_test_failure_stage == 0u && !restore_read_ok) {
        self_test_failure_stage = 6u;
    } else if (self_test_failure_stage == 0u &&
               !sector_matches(original_sector, verify_sector)) {
        self_test_failure_stage = 7u;
    }

    return self_test_failure_stage == 0u;
}

u32 ata_self_test_failure_stage(void)
{
    return self_test_failure_stage;
}

u32 ata_self_test_io_failure_stage(void)
{
    return self_test_io_failure_stage;
}
