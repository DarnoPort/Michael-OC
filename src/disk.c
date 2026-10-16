/*
 * Primary-master ATA PIO driver for QEMU's legacy IDE controller.
 * Bounded polling, LBA28, no DMA or disk IRQ dependency.
 */
#include "disk.h"
#define ATA_DATA 0x1F0u
#define ATA_SECTOR_CNT 0x1F2u
#define ATA_LBA_LOW 0x1F3u
#define ATA_LBA_MID 0x1F4u
#define ATA_LBA_HIGH 0x1F5u
#define ATA_DRIVE 0x1F6u
#define ATA_STATUS 0x1F7u
#define ATA_COMMAND 0x1F7u
#define ATA_ALT_STATUS 0x3F6u
#define ATA_STATUS_ERR 0x01u
#define ATA_STATUS_DRQ 0x08u
#define ATA_STATUS_DF 0x20u
#define ATA_STATUS_BSY 0x80u
#define ATA_CMD_IDENTIFY 0xECu
#define ATA_CMD_READ_SECTORS 0x20u
#define ATA_CMD_WRITE_SECTORS 0x30u
#define ATA_CMD_CACHE_FLUSH 0xE7u
#define ATA_LBA28_LIMIT 0x10000000u
#define ATA_POLL_LIMIT 2000000u
static int initialized;
static u32 total_sectors;
static inline void outb(u16 port, u8 value)
{ __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port)); }
static inline u8 inb(u16 port)
{ u8 value; __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port)); return value; }
static inline void outw(u16 port, u16 value)
{ __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port)); }
static inline u16 inw(u16 port)
{ u16 value; __asm__ volatile ("inw %1, %0" : "=a"(value) : "Nd"(port)); return value; }
static void io_delay(void)
{
    (void)inb(ATA_ALT_STATUS); (void)inb(ATA_ALT_STATUS);
    (void)inb(ATA_ALT_STATUS); (void)inb(ATA_ALT_STATUS);
}
static int wait_not_busy(void)
{
    for (u32 i = 0u; i < ATA_POLL_LIMIT; ++i) {
        const u8 status = inb(ATA_STATUS);
        if (status == 0u || status == 0xFFu) return 0;
        if ((status & ATA_STATUS_BSY) == 0u)
            return (status & (ATA_STATUS_ERR | ATA_STATUS_DF)) == 0u;
        __asm__ volatile ("pause");
    }
    return 0;
}
static int wait_data_ready(void)
{
    for (u32 i = 0u; i < ATA_POLL_LIMIT; ++i) {
        const u8 status = inb(ATA_STATUS);
        if (status == 0u || status == 0xFFu) return 0;
        if ((status & ATA_STATUS_BSY) != 0u) { __asm__ volatile ("pause"); continue; }
        if ((status & (ATA_STATUS_ERR | ATA_STATUS_DF)) != 0u) return 0;
        if ((status & ATA_STATUS_DRQ) != 0u) return 1;
    }
    return 0;
}
static int read_identify(u16 identify[256])
{
    outb(ATA_DRIVE, 0xA0u);
    io_delay();
    outb(ATA_SECTOR_CNT, 0u);
    outb(ATA_LBA_LOW, 0u);
    outb(ATA_LBA_MID, 0u);
    outb(ATA_LBA_HIGH, 0u);
    outb(ATA_COMMAND, ATA_CMD_IDENTIFY);
    const u8 status = inb(ATA_STATUS);
    if (status == 0u || status == 0xFFu) return 0;
    if (!wait_data_ready()) return 0;
    for (u32 i = 0u; i < 256u; ++i) identify[i] = inw(ATA_DATA);
    return wait_not_busy();
}
int ata_init(void)
{
    initialized = 0;
    total_sectors = 0u;
    u16 identify[256];
    if (!read_identify(identify)) return 0;
    const u32 sectors = (u32)identify[60] | ((u32)identify[61] << 16);
    if (sectors < 64u || sectors > ATA_LBA28_LIMIT) return 0;
    total_sectors = sectors;
    initialized = 1;
    return 1;
}
int ata_is_initialized(void) { return initialized; }
u32 ata_sector_count(void) { return initialized ? total_sectors : 0u; }
static int begin_sector_command(u32 lba, u8 command)
{
    if (!initialized || lba >= total_sectors || lba >= ATA_LBA28_LIMIT) return 0;
    if (!wait_not_busy()) return 0;
    outb(ATA_DRIVE, (u8)(0xE0u | ((lba >> 24) & 0x0Fu)));
    io_delay();
    outb(ATA_SECTOR_CNT, 1u);
    outb(ATA_LBA_LOW, (u8)(lba & 0xFFu));
    outb(ATA_LBA_MID, (u8)((lba >> 8) & 0xFFu));
    outb(ATA_LBA_HIGH, (u8)((lba >> 16) & 0xFFu));
    outb(ATA_COMMAND, command);
    io_delay();
    return wait_data_ready();
}
int ata_read_sector(u32 lba, void *buffer)
{
    if (buffer == 0 || !begin_sector_command(lba, ATA_CMD_READ_SECTORS)) return 0;
    u8 *bytes = (u8 *)buffer;
    for (u32 i = 0u; i < 256u; ++i) {
        const u16 word = inw(ATA_DATA);
        bytes[i * 2u] = (u8)(word & 0xFFu);
        bytes[i * 2u + 1u] = (u8)(word >> 8);
    }
    return wait_not_busy();
}
int ata_write_sector(u32 lba, const void *buffer)
{
    if (buffer == 0 || !begin_sector_command(lba, ATA_CMD_WRITE_SECTORS)) return 0;
    const u8 *bytes = (const u8 *)buffer;
    for (u32 i = 0u; i < 256u; ++i) {
        const u16 word = (u16)bytes[i * 2u] | ((u16)bytes[i * 2u + 1u] << 8);
        outw(ATA_DATA, word);
    }
    if (!wait_not_busy()) return 0;
    outb(ATA_COMMAND, ATA_CMD_CACHE_FLUSH);
    io_delay();
    return wait_not_busy();
}
