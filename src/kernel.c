/*
 * Michael OC early kernel bootstrap.
 *
 * This file intentionally avoids libc and performs direct device I/O.
 * VGA text mode is an early bring-up console only; a true packed 1-bit
 * framebuffer and graphical desktop are later milestones.
 */

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002u

#define VGA_WIDTH  80u
#define VGA_HEIGHT 25u
#define VGA_MEMORY ((volatile u16 *)0xB8000u)

#define COM1 0x3F8u

static u32 vga_index(u32 row, u32 col)
{
    return row * VGA_WIDTH + col;
}

static void vga_clear(void)
{
    volatile u16 *vga = VGA_MEMORY;
    const u16 blank = (u16)(0x0Fu << 8) | (u16)' ';

    for (u32 i = 0; i < VGA_WIDTH * VGA_HEIGHT; ++i) {
        vga[i] = blank;
    }
}

static void vga_write_at(u32 row, u32 col, const char *text, u8 attribute)
{
    volatile u16 *vga = VGA_MEMORY;

    if (row >= VGA_HEIGHT || col >= VGA_WIDTH) {
        return;
    }

    u32 index = vga_index(row, col);
    while (*text != '\0' && col < VGA_WIDTH) {
        vga[index] = (u16)(((u16)attribute << 8) | (u8)*text);
        ++text;
        ++col;
        ++index;
    }
}

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

static void serial_init(void)
{
    outb((u16)(COM1 + 1u), 0x00u);
    outb((u16)(COM1 + 3u), 0x80u);
    outb((u16)(COM1 + 0u), 0x03u);
    outb((u16)(COM1 + 1u), 0x00u);
    outb((u16)(COM1 + 3u), 0x03u);
    outb((u16)(COM1 + 2u), 0xC7u);
    outb((u16)(COM1 + 4u), 0x0Bu);
}

static void serial_putc(char ch)
{
    /* Bound the wait so a missing UART cannot hang kernel startup forever. */
    for (u32 i = 0; i < 100000u; ++i) {
        if ((inb((u16)(COM1 + 5u)) & 0x20u) != 0u) {
            outb((u16)COM1, (u8)ch);
            return;
        }
    }
}

static void serial_write(const char *text)
{
    while (*text != '\0') {
        serial_putc(*text);
        ++text;
    }
}

static void halt_forever(void)
{
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

void kernel_main(u32 multiboot_magic, u32 multiboot_info)
{
    (void)multiboot_info;
    serial_init();
    serial_write("\r\nMichael OC: kernel entry reached.\r\n");

    vga_clear();
    vga_write_at(0u, 0u, " MICHAEL OC - KERNEL BOOT ", 0xF0u);
    vga_write_at(2u, 2u, "Independent operating system / early bootstrap", 0x0Fu);

    if (multiboot_magic != MULTIBOOT_BOOTLOADER_MAGIC) {
        serial_write("ERROR: invalid Multiboot magic.\r\n");
        vga_write_at(4u, 2u, "BOOT STATUS : ERROR", 0x0Fu);
        vga_write_at(6u, 2u, "Invalid Multiboot hand-off value.", 0x0Fu);
        halt_forever();
    }

    serial_write("MICHAEL_OC_BOOT_OK\r\n");

    vga_write_at(4u, 2u, "BOOT STATUS : OK", 0x0Fu);
    vga_write_at(5u, 2u, "ARCHITECTURE: x86, 32-bit protected mode", 0x0Fu);
    vga_write_at(6u, 2u, "LOADER      : GRUB Multiboot v1", 0x0Fu);
    vga_write_at(7u, 2u, "DISPLAY     : VGA text, black and white only", 0x0Fu);

    vga_write_at(10u, 2u, "EARLY BOOTSTRAP - NOT THE FINISHED OPERATING SYSTEM", 0x0Fu);
    vga_write_at(12u, 2u, "NEXT: 1-BIT FRAMEBUFFER, INPUT, AND GRAPHICAL DESKTOP", 0x0Fu);
    vga_write_at(13u, 2u, "THEN: PERSISTENT STORAGE, FILESYSTEM, AND REAL APPS", 0x0Fu);

    vga_write_at(16u, 2u, "Serial test marker: MICHAEL_OC_BOOT_OK", 0x0Fu);
    vga_write_at(18u, 2u, "No persistent filesystem or general app loader yet.", 0x0Fu);
    vga_write_at(20u, 2u, "Kernel idles so the boot state remains visible.", 0x0Fu);
    vga_write_at(23u, 2u, "Michael OC | development build", 0x0Fu);

    halt_forever();
}
