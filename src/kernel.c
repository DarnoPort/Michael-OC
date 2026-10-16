/*
 * Michael OC early kernel bootstrap.
 *
 * The software canvas is packed at one bit per pixel. A Multiboot-provided
 * RGB framebuffer is only the scan-out target; every output pixel is either
 * black or full white. VGA text mode remains a diagnostic fallback.
 */

#include "multiboot.h"
#include "memory.h"
#include "interrupts.h"

#define VGA_WIDTH  80u
#define VGA_HEIGHT 25u
#define VGA_MEMORY ((volatile u16 *)0xB8000u)

#define COM1 0x3F8u

#define MONO_WIDTH 800u
#define MONO_HEIGHT 600u
#define MONO_BUFFER_BYTES ((MONO_WIDTH * MONO_HEIGHT + 7u) / 8u)

static u8 mono_buffer[MONO_BUFFER_BYTES];
static u32 canvas_width;
static u32 canvas_height;

static volatile u8 *active_framebuffer;
static u32 active_framebuffer_pitch;
static u32 active_bytes_per_pixel;
static u32 active_white_pixel;
static u32 active_fb_width;
static u32 active_fb_height;
static u32 active_fb_bpp;

enum ui_screen {
    UI_SCREEN_HOME = 0,
    UI_SCREEN_SYSTEM,
    UI_SCREEN_KEYBOARD,
    UI_SCREEN_FRAMEBUFFER
};
static enum ui_screen active_screen = UI_SCREEN_HOME;
static u8 last_key_scancode;

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

static void serial_write_hex(u32 value)
{
    static const char digits[] = "0123456789ABCDEF";
    serial_write("0x");
    for (int shift = 28; shift >= 0; shift -= 4) {
        serial_putc(digits[(value >> (u32)shift) & 0x0Fu]);
    }
}

static void halt_forever(void)
{
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

/* Only this packed buffer stores the user-facing graphics: one bit/pixel. */
static void mono_clear(void)
{
    for (u32 i = 0; i < MONO_BUFFER_BYTES; ++i) {
        mono_buffer[i] = 0u;
    }
}

static void mono_put_pixel(u32 x, u32 y, int white)
{
    if (x >= canvas_width || y >= canvas_height) {
        return;
    }

    const u32 bit_index = y * MONO_WIDTH + x;
    const u32 byte_index = bit_index >> 3;
    const u8 mask = (u8)(1u << (7u - (bit_index & 7u)));

    if (white != 0) {
        mono_buffer[byte_index] |= mask;
    } else {
        mono_buffer[byte_index] &= (u8)~mask;
    }
}

static int mono_get_pixel(u32 x, u32 y)
{
    if (x >= canvas_width || y >= canvas_height) {
        return 0;
    }

    const u32 bit_index = y * MONO_WIDTH + x;
    const u32 byte_index = bit_index >> 3;
    const u8 mask = (u8)(1u << (7u - (bit_index & 7u)));
    return (mono_buffer[byte_index] & mask) != 0u;
}

static void mono_fill_rect(u32 x, u32 y, u32 width, u32 height, int white)
{
    for (u32 row = 0; row < height; ++row) {
        for (u32 col = 0; col < width; ++col) {
            mono_put_pixel(x + col, y + row, white);
        }
    }
}

static void mono_outline_rect(u32 x, u32 y, u32 width, u32 height, int white)
{
    if (width == 0u || height == 0u) {
        return;
    }

    mono_fill_rect(x, y, width, 1u, white);
    mono_fill_rect(x, y + height - 1u, width, 1u, white);
    mono_fill_rect(x, y, 1u, height, white);
    mono_fill_rect(x + width - 1u, y, 1u, height, white);
}

static const u8 font_letters[26][7] = {
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, /* A */
    {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, /* B */
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, /* C */
    {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}, /* D */
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, /* E */
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, /* F */
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, /* G */
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, /* H */
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, /* I */
    {0x07,0x02,0x02,0x02,0x12,0x12,0x0C}, /* J */
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, /* K */
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, /* L */
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, /* M */
    {0x11,0x19,0x15,0x13,0x11,0x11,0x11}, /* N */
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, /* O */
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, /* P */
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, /* Q */
    {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, /* R */
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, /* S */
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, /* T */
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, /* U */
    {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}, /* V */
    {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}, /* W */
    {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, /* X */
    {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, /* Y */
    {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}  /* Z */
};

static const u8 font_digits[10][7] = {
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, /* 0 */
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, /* 1 */
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, /* 2 */
    {0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E}, /* 3 */
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, /* 4 */
    {0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E}, /* 5 */
    {0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E}, /* 6 */
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, /* 7 */
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, /* 8 */
    {0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E}  /* 9 */
};

static u8 font_row(char ch, u32 row)
{
    if (row >= 7u) {
        return 0u;
    }

    if (ch >= 'a' && ch <= 'z') {
        ch = (char)(ch - 'a' + 'A');
    }

    if (ch >= 'A' && ch <= 'Z') {
        return font_letters[(u32)(ch - 'A')][row];
    }

    if (ch >= '0' && ch <= '9') {
        return font_digits[(u32)(ch - '0')][row];
    }

    switch (ch) {
    case '-': return row == 3u ? 0x1Fu : 0u;
    case ':': return (row == 2u || row == 5u) ? 0x04u : 0u;
    case '.': return row == 6u ? 0x04u : 0u;
    case '/':
        switch (row) {
        case 0u: return 0x01u;
        case 1u: return 0x02u;
        case 2u: return 0x02u;
        case 3u: return 0x04u;
        case 4u: return 0x08u;
        case 5u: return 0x08u;
        default: return 0x10u;
        }
    case '|': return 0x04u;
    case '_': return row == 6u ? 0x1Fu : 0u;
    default: return 0u;
    }
}

static void mono_draw_char(u32 x, u32 y, char ch, u32 scale, int invert)
{
    for (u32 row = 0; row < 7u; ++row) {
        const u8 bits = font_row(ch, row);
        for (u32 col = 0; col < 5u; ++col) {
            const int lit = (bits & (u8)(1u << (4u - col))) != 0u;
            const int white = invert != 0 ? !lit : lit;

            for (u32 sy = 0; sy < scale; ++sy) {
                for (u32 sx = 0; sx < scale; ++sx) {
                    mono_put_pixel(x + col * scale + sx,
                                   y + row * scale + sy, white);
                }
            }
        }
    }
}

static void mono_draw_text(u32 x, u32 y, const char *text,
                           u32 scale, int invert)
{
    while (*text != '\0') {
        mono_draw_char(x, y, *text, scale, invert);
        x += 6u * scale;
        ++text;
    }
}

static int mono_buffer_self_test(void)
{
    mono_clear();

    if (mono_get_pixel(0u, 0u) != 0) {
        return 0;
    }

    mono_put_pixel(0u, 0u, 1);
    if (mono_get_pixel(0u, 0u) == 0) {
        return 0;
    }

    mono_put_pixel(0u, 0u, 0);
    if (mono_get_pixel(0u, 0u) != 0) {
        return 0;
    }

    mono_put_pixel(canvas_width - 1u, canvas_height - 1u, 1);
    if (mono_get_pixel(canvas_width - 1u, canvas_height - 1u) == 0) {
        return 0;
    }

    mono_clear();
    return 1;
}

static void mono_draw_uint(u32 x, u32 y, u32 value, u32 scale)
{
    char digits[10];
    u32 count = 0u;

    do {
        digits[count++] = (char)('0' + (value % 10u));
        value /= 10u;
    } while (value != 0u && count < (u32)sizeof(digits));

    while (count > 0u) {
        mono_draw_char(x, y, digits[--count], scale, 0);
        x += 6u * scale;
    }
}

static void mono_draw_hex_byte(u32 x, u32 y, u8 value, u32 scale)
{
    static const char digits[] = "0123456789ABCDEF";

    mono_draw_char(x, y, '0', scale, 0);
    mono_draw_char(x + 6u * scale, y, 'X', scale, 0);
    mono_draw_char(x + 12u * scale, y, digits[(value >> 4) & 0x0Fu], scale, 0);
    mono_draw_char(x + 18u * scale, y, digits[value & 0x0Fu], scale, 0);
}

static void mono_flush_framebuffer(void)
{
    if (active_framebuffer == 0 || active_bytes_per_pixel == 0u) {
        return;
    }

    for (u32 y = 0; y < canvas_height; ++y) {
        const u32 row_offset = y * active_framebuffer_pitch;
        for (u32 x = 0; x < canvas_width; ++x) {
            /* The one-bit software canvas emits only black or full RGB white. */
            const u32 value = mono_get_pixel(x, y) != 0 ? active_white_pixel : 0u;
            const u32 offset = row_offset + x * active_bytes_per_pixel;

            for (u32 byte = 0; byte < active_bytes_per_pixel; ++byte) {
                active_framebuffer[offset + byte] = (u8)(value >> (8u * byte));
            }
        }
    }
}

static void draw_window_frame(const char *title)
{
    mono_clear();
    mono_outline_rect(24u, 24u, 752u, 552u, 1);
    mono_fill_rect(32u, 32u, 736u, 64u, 1);
    mono_draw_text(56u, 50u, title, 3u, 1);
    mono_fill_rect(80u, 112u, 640u, 2u, 1);
    mono_draw_text(80u, 530u, "ESC HOME", 1u, 0);
}

static void draw_home_screen(void)
{
    draw_window_frame("MICHAEL OC");
    mono_draw_text(80u, 140u, "MONOCHROME SYSTEM READY", 2u, 0);
    mono_draw_text(80u, 222u, "1 SYSTEM STATUS", 2u, 0);
    mono_draw_text(80u, 266u, "2 KEYBOARD TEST", 2u, 0);
    mono_draw_text(80u, 310u, "3 FRAMEBUFFER TEST", 2u, 0);
    mono_fill_rect(80u, 370u, 640u, 2u, 1);
    mono_draw_text(80u, 405u, "PRESS 1 2 OR 3 TO OPEN A SCREEN", 1u, 0);
    mono_draw_text(80u, 430u, "USE ESC TO RETURN HOME", 1u, 0);
    mono_draw_text(80u, 485u, "NO FILESYSTEM OR USER APPS YET", 1u, 0);
}

static void draw_system_screen(void)
{
    draw_window_frame("SYSTEM STATUS");
    mono_draw_text(80u, 140u, "FRAMEBUFFER ACTIVE", 2u, 0);

    mono_draw_text(80u, 198u, "WIDTH:", 2u, 0);
    mono_draw_uint(190u, 198u, active_fb_width, 2u);

    mono_draw_text(80u, 234u, "HEIGHT:", 2u, 0);
    mono_draw_uint(190u, 234u, active_fb_height, 2u);

    mono_draw_text(80u, 270u, "BPP:", 2u, 0);
    mono_draw_uint(190u, 270u, active_fb_bpp, 2u);

    mono_draw_text(80u, 322u, "CANVAS: 1 BIT / PIXEL", 2u, 0);
    mono_draw_text(80u, 370u, "DISPLAY: BLACK / WHITE", 2u, 0);
    mono_draw_text(80u, 418u, "INPUT: PS/2 IRQ1", 2u, 0);
    mono_draw_text(80u, 454u, pmm_is_initialized() ? "PMM: READY" : "PMM: NOT READY", 2u, 0);
    mono_draw_text(80u, 490u, "FREE PAGES:", 1u, 0);
    mono_draw_uint(170u, 490u, pmm_free_page_count(), 1u);
}

static void draw_keyboard_screen(void)
{
    draw_window_frame("KEYBOARD TEST");
    mono_draw_text(80u, 145u, "PRESS ANY KEY", 2u, 0);
    mono_draw_text(80u, 205u, "LAST SCAN CODE:", 2u, 0);
    mono_draw_hex_byte(290u, 205u, last_key_scancode, 2u);
    mono_draw_text(80u, 270u, "THIS PAGE SHOWS MAKE SCANCODES", 1u, 0);
    mono_draw_text(80u, 295u, "KEY RELEASE CODES ARE IGNORED", 1u, 0);
}

static void draw_framebuffer_screen(void)
{
    draw_window_frame("FRAMEBUFFER TEST");
    mono_draw_text(80u, 140u, "TWO SOLID PIXEL VALUES", 2u, 0);

    mono_outline_rect(78u, 185u, 260u, 150u, 1);
    mono_fill_rect(80u, 187u, 256u, 146u, 1);
    mono_draw_text(105u, 246u, "WHITE", 3u, 1);

    mono_outline_rect(398u, 185u, 260u, 150u, 1);
    mono_draw_text(430u, 246u, "BLACK", 3u, 0);

    mono_draw_text(80u, 380u, "THE SOFTWARE CANVAS STORES ONE BIT", 1u, 0);
    mono_draw_text(80u, 405u, "PER PIXEL; NO GREY VALUE EXISTS", 1u, 0);
}

static void render_active_screen(void)
{
    switch (active_screen) {
    case UI_SCREEN_SYSTEM:
        draw_system_screen();
        break;
    case UI_SCREEN_KEYBOARD:
        draw_keyboard_screen();
        break;
    case UI_SCREEN_FRAMEBUFFER:
        draw_framebuffer_screen();
        break;
    case UI_SCREEN_HOME:
    default:
        draw_home_screen();
        break;
    }
}

static int handle_ui_scancode(u8 scancode)
{
    /* Ignore extended prefixes and key-release events in this initial driver. */
    if (scancode == 0xE0u || scancode == 0xE1u || (scancode & 0x80u) != 0u) {
        return 0;
    }

    if (scancode == 0x01u) {
        if (active_screen != UI_SCREEN_HOME) {
            active_screen = UI_SCREEN_HOME;
            return 1;
        }
        return 0;
    }

    if (active_screen == UI_SCREEN_HOME) {
        if (scancode == 0x02u) {
            active_screen = UI_SCREEN_SYSTEM;
            return 1;
        }
        if (scancode == 0x03u) {
            active_screen = UI_SCREEN_KEYBOARD;
            last_key_scancode = 0u;
            return 1;
        }
        if (scancode == 0x04u) {
            active_screen = UI_SCREEN_FRAMEBUFFER;
            return 1;
        }
        return 0;
    }

    if (active_screen == UI_SCREEN_KEYBOARD) {
        last_key_scancode = scancode;
        return 1;
    }

    return 0;
}

static int ui_input_self_test(void)
{
    active_screen = UI_SCREEN_HOME;
    last_key_scancode = 0u;

    if (!handle_ui_scancode(0x03u) || active_screen != UI_SCREEN_KEYBOARD) {
        return 0;
    }
    if (!handle_ui_scancode(0x1Eu) || last_key_scancode != 0x1Eu) {
        return 0;
    }
    if (handle_ui_scancode(0x9Eu) || last_key_scancode != 0x1Eu) {
        return 0;
    }
    if (!handle_ui_scancode(0x01u) || active_screen != UI_SCREEN_HOME) {
        return 0;
    }
    if (!handle_ui_scancode(0x02u) || active_screen != UI_SCREEN_SYSTEM) {
        return 0;
    }
    if (!handle_ui_scancode(0x01u) || active_screen != UI_SCREEN_HOME) {
        return 0;
    }
    if (!handle_ui_scancode(0x04u) || active_screen != UI_SCREEN_FRAMEBUFFER) {
        return 0;
    }
    if (!handle_ui_scancode(0x01u) || active_screen != UI_SCREEN_HOME) {
        return 0;
    }

    return 1;
}

static u32 channel_mask(u8 position, u8 size, u8 bpp)
{
    if (size == 0u || size > 8u || (u32)position + (u32)size > (u32)bpp) {
        return 0u;
    }

    return ((1u << size) - 1u) << position;
}

static int initialize_graphics(u32 multiboot_info_address)
{
    if (multiboot_info_address == 0u) {
        return 0;
    }

    const multiboot_info_t *info =
        (const multiboot_info_t *)multiboot_info_address;

    serial_write("MB_FLAGS="); serial_write_hex(info->flags); serial_write("\r\n");
    serial_write("FB_ADDR_LO="); serial_write_hex((u32)info->framebuffer_addr); serial_write("\r\n");
    serial_write("FB_ADDR_HI="); serial_write_hex((u32)(info->framebuffer_addr >> 32)); serial_write("\r\n");
    serial_write("FB_PITCH="); serial_write_hex(info->framebuffer_pitch); serial_write("\r\n");
    serial_write("FB_WIDTH="); serial_write_hex(info->framebuffer_width); serial_write("\r\n");
    serial_write("FB_HEIGHT="); serial_write_hex(info->framebuffer_height); serial_write("\r\n");
    serial_write("FB_BPP="); serial_write_hex((u32)info->framebuffer_bpp); serial_write("\r\n");
    serial_write("FB_TYPE="); serial_write_hex((u32)info->framebuffer_type); serial_write("\r\n");
    serial_write("FB_RED_POS="); serial_write_hex((u32)info->color.red_field_position); serial_write("\r\n");
    serial_write("FB_RED_SIZE="); serial_write_hex((u32)info->color.red_mask_size); serial_write("\r\n");
    serial_write("FB_GREEN_POS="); serial_write_hex((u32)info->color.green_field_position); serial_write("\r\n");
    serial_write("FB_GREEN_SIZE="); serial_write_hex((u32)info->color.green_mask_size); serial_write("\r\n");
    serial_write("FB_BLUE_POS="); serial_write_hex((u32)info->color.blue_field_position); serial_write("\r\n");
    serial_write("FB_BLUE_SIZE="); serial_write_hex((u32)info->color.blue_mask_size); serial_write("\r\n");

    if ((info->flags & MULTIBOOT_INFO_FRAMEBUFFER) == 0u) {
        serial_write("FB_DIAG_FLAGS_MISSING\r\n");
        return 0;
    }
    if (info->framebuffer_type != 1u) {
        serial_write("FB_DIAG_NOT_RGB\r\n");
        return 0;
    }

    if (info->framebuffer_addr > 0xFFFFFFFFull) {
        return 0;
    }

    const u32 width = info->framebuffer_width;
    const u32 height = info->framebuffer_height;
    const u32 bpp = (u32)info->framebuffer_bpp;

    if (width < 320u || height < 200u ||
        width > 4096u || height > 2160u ||
        (bpp != 24u && bpp != 32u)) {
        return 0;
    }

    const u32 bytes_per_pixel = bpp / 8u;
    if ((u64)info->framebuffer_pitch < (u64)width * bytes_per_pixel) {
        return 0;
    }

    const u64 framebuffer_end = info->framebuffer_addr +
        (u64)info->framebuffer_pitch * (u64)height;
    if (framebuffer_end > 0x100000000ull) {
        return 0;
    }

    u32 red_mask = channel_mask(info->color.red_field_position,
                                  info->color.red_mask_size, (u8)bpp);
    u32 green_mask = channel_mask(info->color.green_field_position,
                                  info->color.green_mask_size, (u8)bpp);
    u32 blue_mask = channel_mask(info->color.blue_field_position,
                                 info->color.blue_mask_size, (u8)bpp);

    const int standard_masks_valid =
        red_mask != 0u && green_mask != 0u && blue_mask != 0u &&
        (red_mask & green_mask) == 0u &&
        (red_mask & blue_mask) == 0u &&
        (green_mask & blue_mask) == 0u;

    if (!standard_masks_valid) {
        /*
         * The GRUB 2.12 QEMU hand-off observed during bring-up aligns the
         * RGB union to four bytes, leaving two zero bytes after framebuffer
         * type. Multiboot describes RGB bytes at offsets 110..115; tolerate
         * the aligned form at 112..117 only when the standard interpretation
         * is invalid. Both interpretations are range-checked and must not
         * contain overlapping channels.
         */
        const u8 *aligned_rgb = (const u8 *)info + 112u;
        const u32 alt_red = channel_mask(aligned_rgb[0], aligned_rgb[1], (u8)bpp);
        const u32 alt_green = channel_mask(aligned_rgb[2], aligned_rgb[3], (u8)bpp);
        const u32 alt_blue = channel_mask(aligned_rgb[4], aligned_rgb[5], (u8)bpp);

        const int aligned_masks_valid =
            alt_red != 0u && alt_green != 0u && alt_blue != 0u &&
            (alt_red & alt_green) == 0u &&
            (alt_red & alt_blue) == 0u &&
            (alt_green & alt_blue) == 0u;

        if (!aligned_masks_valid) {
            serial_write("FB_DIAG_INVALID_RGB_MASKS\\r\\n");
            return 0;
        }

        red_mask = alt_red;
        green_mask = alt_green;
        blue_mask = alt_blue;
        serial_write("FB_COLOR_LAYOUT_ALIGNED\\r\\n");
    } else {
        serial_write("FB_COLOR_LAYOUT_STANDARD\\r\\n");
    }

    const u32 white_pixel = red_mask | green_mask | blue_mask;

    canvas_width = width < MONO_WIDTH ? width : MONO_WIDTH;
    canvas_height = height < MONO_HEIGHT ? height : MONO_HEIGHT;
    active_framebuffer = (volatile u8 *)(u32)info->framebuffer_addr;
    active_framebuffer_pitch = info->framebuffer_pitch;
    active_bytes_per_pixel = bytes_per_pixel;
    active_white_pixel = white_pixel;
    active_fb_width = width;
    active_fb_height = height;
    active_fb_bpp = bpp;

    if (canvas_width < 320u || canvas_height < 200u ||
        !mono_buffer_self_test()) {
        return 0;
    }

    serial_write("MICHAEL_OC_MONO_TEST_OK\r\n");

    active_screen = UI_SCREEN_HOME;
    last_key_scancode = 0u;
    if (!ui_input_self_test()) {
        serial_write("MICHAEL_OC_INPUT_TEST_FAILED\r\n");
        return 0;
    }
    serial_write("MICHAEL_OC_INPUT_TEST_OK\r\n");

    active_screen = UI_SCREEN_HOME;
    last_key_scancode = 0u;
    render_active_screen();
    mono_flush_framebuffer();

    serial_write("MICHAEL_OC_FB_OK\r\n");
    return 1;
}

static int keyboard_try_read_scancode(u8 *scancode)
{
    return keyboard_try_get_scancode(scancode);
}

static void log_ui_screen_event(void)
{
    switch (active_screen) {
    case UI_SCREEN_SYSTEM:
        serial_write("MICHAEL_OC_UI_SCREEN=SYSTEM\r\n");
        break;
    case UI_SCREEN_KEYBOARD:
        serial_write("MICHAEL_OC_UI_SCREEN=KEYBOARD\r\n");
        break;
    case UI_SCREEN_FRAMEBUFFER:
        serial_write("MICHAEL_OC_UI_SCREEN=FRAMEBUFFER\r\n");
        break;
    case UI_SCREEN_HOME:
    default:
        serial_write("MICHAEL_OC_UI_SCREEN=HOME\r\n");
        break;
    }
}

static void keyboard_event_loop(void)
{
    serial_write("MICHAEL_OC_KEYBOARD_POLL_OK\r\n");

    for (;;) {
        u8 scancode;
        if (keyboard_try_read_scancode(&scancode)) {
            const enum ui_screen previous_screen = active_screen;
            if (handle_ui_scancode(scancode)) {
                if (active_screen != previous_screen) {
                    log_ui_screen_event();
                } else if (active_screen == UI_SCREEN_KEYBOARD) {
                    serial_write("MICHAEL_OC_UI_KEY_SCANCODE=");
                    serial_write_hex((u32)scancode);
                    serial_write("\r\n");
                }
                render_active_screen();
                mono_flush_framebuffer();
            }
        } else {
            __asm__ volatile ("pause");
        }
    }
}

static void show_vga_fallback(void)
{
    vga_clear();
    vga_write_at(0u, 0u, " MICHAEL OC - KERNEL BOOT ", 0xF0u);
    vga_write_at(2u, 2u, "Independent operating system / early bootstrap", 0x0Fu);
    vga_write_at(4u, 2u, "BOOT STATUS : OK", 0x0Fu);
    vga_write_at(5u, 2u, "ARCHITECTURE: x86, 32-bit protected mode", 0x0Fu);
    vga_write_at(6u, 2u, "LOADER      : GRUB Multiboot v1", 0x0Fu);
    vga_write_at(7u, 2u, "DISPLAY     : framebuffer unavailable", 0x0Fu);
    vga_write_at(10u, 2u, "GRAPHICAL BOOT TEST DID NOT PASS", 0x0Fu);
    vga_write_at(12u, 2u, "Check GRUB video mode and Multiboot framebuffer data.", 0x0Fu);
    vga_write_at(15u, 2u, "MICHAEL_OC_FB_UNAVAILABLE", 0x0Fu);
}

void kernel_main(u32 multiboot_magic, u32 multiboot_info)
{
    serial_init();
    interrupts_init();
    serial_write("\r\nMichael OC: kernel entry reached.\r\n");

    if (multiboot_magic != MULTIBOOT_BOOTLOADER_MAGIC) {
        serial_write("ERROR: invalid Multiboot magic.\r\n");
        vga_clear();
        vga_write_at(0u, 0u, "MICHAEL OC - BOOT ERROR", 0xF0u);
        vga_write_at(2u, 2u, "Invalid Multiboot hand-off value.", 0x0Fu);
        halt_forever();
    }

    serial_write("MICHAEL_OC_BOOT_OK\r\n");

    if (pmm_init((const multiboot_info_t *)multiboot_info) && pmm_self_test()) {
        serial_write("MICHAEL_OC_PMM_OK\r\n");
    } else {
        serial_write("MICHAEL_OC_PMM_FAIL\r\n");
    }

    if (!initialize_graphics(multiboot_info)) {
        serial_write("MICHAEL_OC_FB_UNAVAILABLE\r\n");
        show_vga_fallback();
        halt_forever();
    }

#ifdef MICHAEL_OC_EXCEPTION_SELF_TEST
    serial_write("MICHAEL_OC_EXCEPTION_SELF_TEST_TRIGGER\r\n");
    __asm__ volatile ("int $3");
    serial_write("MICHAEL_OC_EXCEPTION_TEST_FAILED\r\n");
#endif
    interrupts_enable();
    keyboard_event_loop();
}
