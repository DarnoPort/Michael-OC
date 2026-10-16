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
#include "fs.h"
#include "paging.h"

#define VGA_WIDTH  80u
#define VGA_HEIGHT 25u
#define VGA_MEMORY ((volatile u16 *)0xB8000u)

#define COM1 0x3F8u

#define UI_LOG_LINES 64u
#define UI_LOG_WIDTH 100u
#define UI_CONSOLE_VISIBLE_LINES 17u

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
    UI_SCREEN_FRAMEBUFFER,
    UI_SCREEN_FILES,
    UI_SCREEN_EDITOR,
    UI_SCREEN_APP,
    UI_SCREEN_CONSOLE
};
static enum ui_screen active_screen = UI_SCREEN_HOME;
static u8 last_key_scancode;
static char ui_log_lines[UI_LOG_LINES][UI_LOG_WIDTH];
static u32 ui_log_next;
static u32 ui_log_count = 1u;
static u32 ui_log_column;
static u32 ui_console_scroll;
static u32 ui_mouse_x = MONO_WIDTH / 2u;
static u32 ui_mouse_y = MONO_HEIGHT / 2u;
static u8 ui_mouse_buttons;

#define UI_EDITOR_CAPACITY 16384u
#define UI_EDITOR_LINE_WIDTH 100u
#define UI_EDITOR_VISIBLE_LINES 18u
#define UI_FILE_VISIBLE_ENTRIES 12u

enum ui_file_dialog_mode {
    UI_FILE_DIALOG_NONE = 0,
    UI_FILE_DIALOG_NEW_FILE,
    UI_FILE_DIALOG_NEW_DIRECTORY,
    UI_FILE_DIALOG_RENAME,
    UI_FILE_DIALOG_DELETE_CONFIRM
};

static u16 ui_directory_stack[FS_DIRECTORY_LIMIT];
static u32 ui_directory_depth;
static fs_entry_t ui_file_entries[FS_DIRECTORY_LIMIT];
static u32 ui_file_count;
static u32 ui_file_selection;
static enum ui_file_dialog_mode ui_file_dialog;
static char ui_file_dialog_buffer[32];
static u32 ui_file_dialog_length;
static char ui_status_message[48];

static u8 editor_buffer[UI_EDITOR_CAPACITY];
static u32 editor_size;
static u32 editor_cursor;
static u32 editor_first_line;
static u16 editor_file_index;
static enum ui_screen editor_return_screen = UI_SCREEN_FILES;
static char editor_filename[32];
static int editor_dirty;
static int editor_exit_confirm;
static int timer_tick_marker_reported;

#define MOCAPP_MAX_LINES 256u
#define MOCAPP_OUTPUT_LINES 14u
#define MOCAPP_OUTPUT_WIDTH 96u
static char mocapp_program[UI_EDITOR_CAPACITY + 1u];
static char *mocapp_lines[MOCAPP_MAX_LINES];
static u32 mocapp_line_count;
static u32 mocapp_pc;
static char mocapp_output[MOCAPP_OUTPUT_LINES][MOCAPP_OUTPUT_WIDTH];
static u32 mocapp_output_count;
static int mocapp_running;
static int mocapp_waiting_key;
static char mocapp_last_key;
static int mocapp_register;
static enum ui_screen mocapp_return_screen = UI_SCREEN_FILES;
static char mocapp_filename[32];
static fs_entry_t mocapp_list_entries[FS_DIRECTORY_LIMIT];

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

static void ui_log_capture_char(char ch)
{
    if (ch == '\r') return;

    u32 slot = ui_log_next;
    if (ch == '\n') {
        ui_log_lines[slot][ui_log_column] = '\0';
        ui_log_next = (ui_log_next + 1u) % UI_LOG_LINES;
        ui_log_lines[ui_log_next][0] = '\0';
        ui_log_column = 0u;
        if (ui_log_count < UI_LOG_LINES) ++ui_log_count;
        ui_console_scroll = 0u;
        return;
    }

    if ((u8)ch < 32u || (u8)ch > 126u) ch = '?';
    if (ui_log_column + 1u < UI_LOG_WIDTH) {
        ui_log_lines[slot][ui_log_column++] = ch;
        ui_log_lines[slot][ui_log_column] = '\0';
    }
}

static void serial_write(const char *text)
{
    while (*text != '\0') {
        ui_log_capture_char(*text);
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

static void halt_forever(void) __attribute__((noreturn));
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
    mono_draw_text(80u, 188u, "1 SYSTEM STATUS", 2u, 0);
    mono_draw_text(80u, 226u, "2 KEYBOARD TEST", 2u, 0);
    mono_draw_text(80u, 264u, "3 DISPLAY TEST", 2u, 0);
    mono_draw_text(405u, 188u, "4 FILE MANAGER", 2u, 0);
    mono_draw_text(405u, 226u, "5 TEXT EDITOR", 2u, 0);
    mono_draw_text(405u, 264u, "6 DEBUG CONSOLE", 2u, 0);
    mono_fill_rect(80u, 315u, 640u, 2u, 1);
    mono_draw_text(80u, 345u, "PRESS A NUMBER TO LAUNCH AN APP", 1u, 0);
    mono_draw_text(80u, 370u, "USE ESC TO RETURN HOME", 1u, 0);
    mono_draw_text(80u, 485u, fs_is_mounted() ? "STORAGE: PERSISTENT FS READY" : "NO VIRTUAL DISK DETECTED", 1u, 0);
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
    mono_draw_text(80u, 418u, mouse_is_available() ? "INPUT: PS2 KEY/MOUSE" : "INPUT: PS/2 KEY ONLY", 2u, 0);
    mono_draw_text(80u, 454u, pmm_is_initialized() ? "PMM: READY" : "PMM: NOT READY", 2u, 0);
    mono_draw_text(250u, 454u, "PIT: 100 HZ", 1u, 0);
    mono_draw_text(80u, 475u, "UPTIME SEC:", 1u, 0);
    mono_draw_uint(150u, 475u, timer_ticks() / 100u, 1u);
    mono_draw_text(80u, 490u, "FREE PAGES:", 1u, 0);
    mono_draw_text(80u, 515u, fs_is_mounted() ? "FS: MOCFS1 READY" : "FS: UNAVAILABLE", 1u, 0);
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


static void ui_set_message(const char *message)
{
    u32 i = 0u;
    while (i < sizeof(ui_status_message) - 1u && message != 0 && message[i] != '\0') {
        ui_status_message[i] = message[i];
        ++i;
    }
    ui_status_message[i] = '\0';
    while (++i < sizeof(ui_status_message)) ui_status_message[i] = '\0';
}

static void ui_copy_text(char *destination, u32 capacity, const char *source)
{
    if (capacity == 0u) return;
    u32 i = 0u;
    while (i + 1u < capacity && source != 0 && source[i] != '\0') {
        destination[i] = source[i];
        ++i;
    }
    destination[i] = '\0';
    while (++i < capacity) destination[i] = '\0';
}

static u32 ui_text_length(const char *text)
{
    u32 length = 0u;
    if (text == 0) return 0u;
    while (text[length] != '\0' && length < 4096u) ++length;
    return length;
}

static int ui_text_equal(const char *left, const char *right)
{
    if (left == 0 || right == 0) return 0;
    u32 i = 0u;
    while (left[i] != '\0' && right[i] != '\0') {
        if (left[i] != right[i]) return 0;
        ++i;
    }
    return left[i] == right[i];
}

static u16 ui_current_directory(void)
{
    return ui_directory_depth == 0u ? FS_ROOT : ui_directory_stack[ui_directory_depth - 1u];
}

static void ui_refresh_file_list(void)
{
    ui_file_count = 0u;
    if (!fs_is_mounted()) {
        ui_set_message("NO DISK AVAILABLE");
        return;
    }
    u32 count = 0u;
    if (!fs_list(ui_current_directory(), ui_file_entries, FS_DIRECTORY_LIMIT, &count)) {
        ui_set_message("DIRECTORY READ FAILED");
        return;
    }
    ui_file_count = count;
    if (ui_file_count == 0u) ui_file_selection = 0u;
    else if (ui_file_selection >= ui_file_count) ui_file_selection = ui_file_count - 1u;
}

static void ui_select_name(const char *name)
{
    for (u32 i = 0u; i < ui_file_count; ++i) {
        if (ui_text_equal(ui_file_entries[i].name, name)) {
            ui_file_selection = i;
            return;
        }
    }
}

static int editor_open_file(u16 index, const char *name, enum ui_screen return_screen)
{
    u32 size = 0u;
    if (!fs_read_file(index, editor_buffer, UI_EDITOR_CAPACITY, &size)) {
        ui_set_message("OPEN FAILED OR FILE TOO LARGE");
        return 0;
    }
    editor_file_index = index;
    editor_size = size;
    editor_cursor = size;
    editor_first_line = 0u;
    editor_return_screen = return_screen;
    editor_dirty = 0;
    editor_exit_confirm = 0;
    ui_copy_text(editor_filename, sizeof(editor_filename), name);
    ui_set_message("FILE OPENED");
    active_screen = UI_SCREEN_EDITOR;
    serial_write("MICHAEL_OC_EDITOR_OPENED=");
    serial_write(editor_filename);
    serial_write(" SIZE=");
    serial_write_hex(editor_size);
    serial_write("\r\n");
    return 1;
}

static int editor_open_welcome(void)
{
    static const u8 welcome[] =
        "WELCOME TO MICHAEL OC\n"
        "Edit this document and press F2 to save.\n"
        "Use the file manager to create and organize files.\n";
    if (!fs_is_mounted()) {
        ui_set_message("NO DISK AVAILABLE");
        return 0;
    }
    u16 index = 0u;
    if (!fs_lookup(FS_ROOT, "welcome.txt", &index)) {
        if (!fs_create_file(FS_ROOT, "welcome.txt", &index) ||
            !fs_write_file(index, welcome, (u32)sizeof(welcome) - 1u)) {
            ui_set_message("COULD NOT CREATE WELCOME FILE");
            return 0;
        }
    }
    return editor_open_file(index, "welcome.txt", UI_SCREEN_HOME);
}

static char ui_scancode_to_ascii(u8 scancode)
{
    switch (scancode) {
    case 0x02: return '1'; case 0x03: return '2'; case 0x04: return '3';
    case 0x05: return '4'; case 0x06: return '5'; case 0x07: return '6';
    case 0x08: return '7'; case 0x09: return '8'; case 0x0A: return '9';
    case 0x0B: return '0'; case 0x0C: return '-';
    case 0x10: return 'q'; case 0x11: return 'w'; case 0x12: return 'e';
    case 0x13: return 'r'; case 0x14: return 't'; case 0x15: return 'y';
    case 0x16: return 'u'; case 0x17: return 'i'; case 0x18: return 'o';
    case 0x19: return 'p'; case 0x1E: return 'a'; case 0x1F: return 's';
    case 0x20: return 'd'; case 0x21: return 'f'; case 0x22: return 'g';
    case 0x23: return 'h'; case 0x24: return 'j'; case 0x25: return 'k';
    case 0x26: return 'l'; case 0x2C: return 'z'; case 0x2D: return 'x';
    case 0x2E: return 'c'; case 0x2F: return 'v'; case 0x30: return 'b';
    case 0x31: return 'n'; case 0x32: return 'm'; case 0x33: return ',';
    case 0x34: return '.'; case 0x39: return ' ';
    default: return '\0';
    }
}


static char *mocapp_skip_spaces(char *text)
{
    while (*text == ' ' || *text == '\t') ++text;
    return text;
}
static int mocapp_token_equal(const char *left, const char *right)
{
    u32 i = 0u;
    while (left[i] != '\0' && right[i] != '\0') {
        char a = left[i], b = right[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
        if (b >= 'a' && b <= 'z') b = (char)(b - 'a' + 'A');
        if (a != b) return 0;
        ++i;
    }
    return left[i] == '\0' && right[i] == '\0';
}
static int mocapp_parse_integer(const char *text, int *value_out)
{
    if (text == 0 || value_out == 0) return 0;
    while (*text == ' ' || *text == '\t') ++text;
    int negative = 0;
    if (*text == '-') { negative = 1; ++text; }
    if (*text < '0' || *text > '9') return 0;
    int value = 0;
    while (*text >= '0' && *text <= '9') {
        const int digit = *text - '0';
        if (value > 200000000 || (value == 200000000 && digit > 0)) return 0;
        value = value * 10 + digit;
        ++text;
    }
    while (*text == ' ' || *text == '\t') ++text;
    if (*text != '\0') return 0;
    *value_out = negative ? -value : value;
    return 1;
}
static void mocapp_clear_output(void)
{
    mocapp_output_count = 0u;
    for (u32 i = 0u; i < MOCAPP_OUTPUT_LINES; ++i)
        mocapp_output[i][0] = '\0';
}
static void mocapp_print_line(const char *message)
{
    if (mocapp_output_count == MOCAPP_OUTPUT_LINES) {
        for (u32 i = 1u; i < MOCAPP_OUTPUT_LINES; ++i)
            ui_copy_text(mocapp_output[i - 1u], MOCAPP_OUTPUT_WIDTH, mocapp_output[i]);
        --mocapp_output_count;
    }
    ui_copy_text(mocapp_output[mocapp_output_count], MOCAPP_OUTPUT_WIDTH, message);
    ++mocapp_output_count;
    serial_write("MICHAEL_OC_APP_PRINT=");
    serial_write(message);
    serial_write("\r\n");
}
static void mocapp_print_integer(int value)
{
    char output[16], digits[12];
    u32 length = 0u, count = 0u;
    unsigned int magnitude;
    if (value < 0) {
        output[length++] = '-';
        magnitude = (unsigned int)(-(value + 1)) + 1u;
    } else magnitude = (unsigned int)value;
    do {
        digits[count++] = (char)('0' + magnitude % 10u);
        magnitude /= 10u;
    } while (magnitude != 0u && count < sizeof(digits));
    while (count > 0u) output[length++] = digits[--count];
    output[length] = '\0';
    mocapp_print_line(output);
}
static int mocapp_is_script(const char *name)
{
    const u32 length = ui_text_length(name);
    if (length < 5u) return 0;
    const char *suffix = name + length - 5u;
    return suffix[0] == '.' &&
           (suffix[1] == 'm' || suffix[1] == 'M') &&
           (suffix[2] == 'a' || suffix[2] == 'A') &&
           (suffix[3] == 'p' || suffix[3] == 'P') &&
           (suffix[4] == 'p' || suffix[4] == 'P');
}
static void mocapp_close(const char *reason)
{
    mocapp_running = 0;
    mocapp_waiting_key = 0;
    active_screen = mocapp_return_screen;
    ui_set_message(reason);
    serial_write("MICHAEL_OC_APP_EXIT\r\n");
}
static void mocapp_execute(void)
{
    u32 budget = 128u;
    while (mocapp_running && !mocapp_waiting_key &&
           mocapp_pc < mocapp_line_count && budget > 0u) {
        --budget;
        char *line = mocapp_skip_spaces(mocapp_lines[mocapp_pc]);
        if (*line == '\0' || *line == '#') {
            ++mocapp_pc;
            continue;
        }
        char *argument = line;
        while (*argument != '\0' && *argument != ' ' && *argument != '\t') ++argument;
        if (*argument != '\0') {
            *argument++ = '\0';
            argument = mocapp_skip_spaces(argument);
        }
        if (mocapp_token_equal(line, "PRINT")) {
            mocapp_print_line(argument);
            ++mocapp_pc;
        } else if (mocapp_token_equal(line, "CLEAR")) {
            mocapp_clear_output();
            ++mocapp_pc;
        } else if (mocapp_token_equal(line, "WAITKEY")) {
            ++mocapp_pc;
            mocapp_waiting_key = 1;
            serial_write("MICHAEL_OC_APP_WAITKEY\r\n");
        } else if (mocapp_token_equal(line, "JUMP")) {
            int target = 0;
            if (!mocapp_parse_integer(argument, &target) ||
                target < 0 || (u32)target >= mocapp_line_count) {
                mocapp_print_line("VM ERROR: BAD JUMP TARGET");
                mocapp_close("APP ERROR");
                return;
            }
            mocapp_pc = (u32)target;
        } else if (mocapp_token_equal(line, "IFKEY")) {
            const char expected = argument[0];
            char *target_text = argument;
            if (*target_text != '\0') ++target_text;
            while (*target_text == ' ' || *target_text == '\t') ++target_text;
            int target = 0;
            if (expected == '\0' || !mocapp_parse_integer(target_text, &target) ||
                target < 0 || (u32)target >= mocapp_line_count) {
                mocapp_print_line("VM ERROR: USE IFKEY CHAR LINE");
                mocapp_close("APP ERROR");
                return;
            }
            if (mocapp_last_key == expected) mocapp_pc = (u32)target;
            else ++mocapp_pc;
        } else if (mocapp_token_equal(line, "SET") ||
                   mocapp_token_equal(line, "ADD") ||
                   mocapp_token_equal(line, "SUB")) {
            int value = 0;
            if (!mocapp_parse_integer(argument, &value)) {
                mocapp_print_line("VM ERROR: EXPECTED INTEGER");
                mocapp_close("APP ERROR");
                return;
            }
            if (mocapp_token_equal(line, "SET")) mocapp_register = value;
            else if (mocapp_token_equal(line, "ADD")) mocapp_register += value;
            else mocapp_register -= value;
            ++mocapp_pc;
        } else if (mocapp_token_equal(line, "PRINTNUM")) {
            mocapp_print_integer(mocapp_register);
            ++mocapp_pc;
        } else if (mocapp_token_equal(line, "LS")) {
            u32 count = 0u;
            if (!fs_list(ui_current_directory(), mocapp_list_entries,
                         FS_DIRECTORY_LIMIT, &count)) {
                mocapp_print_line("LS FAILED");
            } else {
                for (u32 i = 0u; i < count; ++i)
                    mocapp_print_line(mocapp_list_entries[i].name);
            }
            ++mocapp_pc;
        } else if (mocapp_token_equal(line, "READ")) {
            u16 index = 0u;
            u8 data[512];
            u32 size = 0u;
            if (!fs_lookup(ui_current_directory(), argument, &index) ||
                !fs_read_file(index, data, sizeof(data) - 1u, &size)) {
                mocapp_print_line("READ FAILED (MISSING OR OVER 511 BYTES)");
            } else {
                data[size] = '\0';
                char *start = (char *)data;
                for (u32 i = 0u; i <= size; ++i) {
                    if (data[i] == '\n' || data[i] == '\0') {
                        const u8 saved = data[i];
                        data[i] = '\0';
                        mocapp_print_line(start);
                        start = (char *)data + i + 1u;
                        data[i] = saved;
                    }
                }
            }
            ++mocapp_pc;
        } else if (mocapp_token_equal(line, "WRITE")) {
            char *name = argument;
            char *content = name;
            while (*content != '\0' && *content != ' ' && *content != '\t') ++content;
            char *content_separator = content;
            const char content_separator_character = *content;
            if (*content != '\0') *content++ = '\0';
            content = mocapp_skip_spaces(content);
            u16 index = 0u;
            if (*name == '\0') {
                mocapp_print_line("WRITE FAILED: MISSING NAME");
            } else if ((!fs_lookup(ui_current_directory(), name, &index) &&
                        !fs_create_file(ui_current_directory(), name, &index)) ||
                       !fs_write_file(index, (const u8 *)content, ui_text_length(content))) {
                mocapp_print_line("WRITE FAILED");
            } else {
                mocapp_print_line("WRITE OK");
            }
            if (content_separator_character != '\0')
                *content_separator = content_separator_character;
            ++mocapp_pc;
        } else if (mocapp_token_equal(line, "EXIT")) {
            mocapp_close("APP FINISHED");
            return;
        } else {
            mocapp_print_line("VM ERROR: UNKNOWN COMMAND");
            mocapp_close("APP ERROR");
            return;
        }
    }
    if (mocapp_running && !mocapp_waiting_key && mocapp_pc >= mocapp_line_count) {
        mocapp_close("APP FINISHED");
    } else if (mocapp_running && !mocapp_waiting_key && budget == 0u) {
        mocapp_print_line("VM ERROR: INSTRUCTION BUDGET EXHAUSTED");
        mocapp_close("APP ERROR");
    }
}
static int mocapp_start_file(u16 index, const char *name, enum ui_screen return_screen)
{
    u32 size = 0u;
    if (!fs_read_file(index, (u8 *)mocapp_program, UI_EDITOR_CAPACITY, &size)) {
        ui_set_message("APP LOAD FAILED OR FILE TOO LARGE");
        return 0;
    }
    mocapp_program[size] = '\0';
    mocapp_line_count = 0u;
    mocapp_pc = 0u;
    mocapp_clear_output();
    mocapp_waiting_key = 0;
    mocapp_running = 1;
    mocapp_last_key = '\0';
    mocapp_register = 0;
    mocapp_return_screen = return_screen;
    ui_copy_text(mocapp_filename, sizeof(mocapp_filename), name);
    if (size != 0u) mocapp_lines[mocapp_line_count++] = mocapp_program;
    for (u32 i = 0u; i < size; ++i) {
        if (mocapp_program[i] == '\r') mocapp_program[i] = '\0';
        if (mocapp_program[i] == '\n') {
            mocapp_program[i] = '\0';
            if (i + 1u < size) {
                if (mocapp_line_count >= MOCAPP_MAX_LINES) {
                    ui_set_message("APP HAS TOO MANY LINES");
                    mocapp_running = 0;
                    return 0;
                }
                mocapp_lines[mocapp_line_count++] = &mocapp_program[i + 1u];
            }
        }
    }
    active_screen = UI_SCREEN_APP;
    ui_set_message("APP LOADED");
    serial_write("MICHAEL_OC_APP_STARTED=");
    serial_write(mocapp_filename);
    serial_write("\r\n");
    mocapp_execute();
    return 1;
}
static void mocapp_handle_key(u8 scancode)
{
    if (scancode == 0x3Bu) {
        mocapp_close("APP STOPPED");
        return;
    }
    if (!mocapp_waiting_key) return;
    mocapp_last_key = ui_scancode_to_ascii(scancode);
    serial_write("MICHAEL_OC_APP_KEY=");
    if (mocapp_last_key != '\0') {
        char text[2] = {mocapp_last_key, '\0'};
        serial_write(text);
    }
    serial_write("\r\n");
    mocapp_waiting_key = 0;
    mocapp_execute();
}
static void draw_mocapp_screen(void)
{
    draw_window_frame("MOCAPP RUNTIME");
    mono_draw_text(80u, 118u, mocapp_filename, 1u, 0);
    for (u32 i = 0u; i < mocapp_output_count; ++i)
        mono_draw_text(80u, 145u + i * 20u, mocapp_output[i], 1u, 0);
    mono_draw_text(80u, 458u, mocapp_waiting_key ? "APP WAITING FOR A KEY" : "APP RUNNING", 1u, 0);
    mono_draw_text(80u, 482u, "ESC RETURN TO FILE MANAGER  F1 STOP", 1u, 0);
}

static void draw_file_manager(void)
{
    draw_window_frame("FILE MANAGER");
    ui_refresh_file_list();
    mono_draw_text(80u, 118u, ui_directory_depth == 0u ? "ROOT" : "SUBDIRECTORY", 1u, 0);
    mono_draw_text(250u, 118u, "N NEW D DIR R RENAME X DELETE", 1u, 0);
    mono_draw_text(80u, 132u, "ENTER OPEN BACKSPACE PARENT", 1u, 0);
    if (ui_file_count == 0u) mono_draw_text(100u, 165u, "DIRECTORY IS EMPTY", 1u, 0);

    const u32 count = ui_file_count < UI_FILE_VISIBLE_ENTRIES ?
                      ui_file_count : UI_FILE_VISIBLE_ENTRIES;
    for (u32 i = 0u; i < count; ++i) {
        const u32 y = 155u + i * 25u;
        const int selected = i == ui_file_selection;
        mono_fill_rect(80u, y - 2u, 640u, 18u, selected);
        mono_draw_text(92u, y, ui_file_entries[i].name, 1u, selected);
        mono_draw_text(610u, y,
                       ui_file_entries[i].type == FS_ENTRY_DIRECTORY ? "DIR" : "FILE",
                       1u, selected);
    }
    mono_draw_text(80u, 468u, ui_status_message, 1u, 0);
    mono_draw_text(80u, 492u, "F2 REFRESH  N FILE  D DIRECTORY  R RENAME", 1u, 0);

    if (ui_file_dialog != UI_FILE_DIALOG_NONE) {
        mono_fill_rect(125u, 215u, 550u, 112u, 1);
        mono_outline_rect(125u, 215u, 550u, 112u, 0);
        if (ui_file_dialog == UI_FILE_DIALOG_NEW_FILE)
            mono_draw_text(155u, 238u, "NEW FILE NAME", 1u, 1);
        else if (ui_file_dialog == UI_FILE_DIALOG_NEW_DIRECTORY)
            mono_draw_text(155u, 238u, "NEW DIRECTORY NAME", 1u, 1);
        else if (ui_file_dialog == UI_FILE_DIALOG_RENAME)
            mono_draw_text(155u, 238u, "RENAME TO", 1u, 1);
        else {
            mono_draw_text(155u, 238u, "DELETE SELECTED ITEM?", 1u, 1);
            mono_draw_text(155u, 265u, "PRESS Y TO DELETE OR N TO CANCEL", 1u, 1);
        }
        if (ui_file_dialog != UI_FILE_DIALOG_DELETE_CONFIRM) {
            mono_draw_text(155u, 270u, ui_file_dialog_buffer, 1u, 1);
            mono_fill_rect(155u + ui_file_dialog_length * 6u, 281u, 5u, 1u, 0);
        }
    }
}

static void editor_cursor_visual_position(u32 *line_out, u32 *column_out)
{
    u32 line = 0u, column = 0u;
    const u32 stop = editor_cursor < editor_size ? editor_cursor : editor_size;
    for (u32 i = 0u; i < stop; ++i) {
        if (editor_buffer[i] == '\n') {
            ++line;
            column = 0u;
        } else {
            if (column >= UI_EDITOR_LINE_WIDTH) {
                ++line;
                column = 0u;
            }
            ++column;
        }
    }
    if (column >= UI_EDITOR_LINE_WIDTH &&
        (editor_cursor >= editor_size || editor_buffer[editor_cursor] != '\n')) {
        ++line;
        column = 0u;
    }
    *line_out = line;
    *column_out = column;
}

static int editor_find_visual_line(u32 wanted, u32 *start_out, u32 *end_out)
{
    u32 line = 0u, column = 0u, start = 0u;
    for (u32 pos = 0u; pos < editor_size; ++pos) {
        const u8 ch = editor_buffer[pos];
        if (ch == '\n') {
            if (line == wanted) {
                *start_out = start;
                *end_out = pos;
                return 1;
            }
            ++line;
            column = 0u;
            start = pos + 1u;
            continue;
        }
        if (column >= UI_EDITOR_LINE_WIDTH) {
            if (line == wanted) {
                *start_out = start;
                *end_out = pos;
                return 1;
            }
            ++line;
            column = 0u;
            start = pos;
        }
        ++column;
    }
    if (line == wanted) {
        *start_out = start;
        *end_out = editor_size;
        return 1;
    }
    return 0;
}

static void editor_move_vertical(int down)
{
    u32 line = 0u, column = 0u;
    editor_cursor_visual_position(&line, &column);
    if (!down && line == 0u) return;
    const u32 target = down ? line + 1u : line - 1u;
    u32 start = 0u, end = 0u;
    if (!editor_find_visual_line(target, &start, &end)) return;
    const u32 length = end - start;
    editor_cursor = start + (column < length ? column : length);
}

static void draw_text_editor(void)
{
    draw_window_frame("TEXT EDITOR");
    mono_draw_text(80u, 118u, editor_filename, 1u, 0);
    if (editor_dirty) mono_draw_text(480u, 118u, "UNSAVED", 1u, 0);

    u32 cursor_line = 0u, cursor_column = 0u;
    editor_cursor_visual_position(&cursor_line, &cursor_column);
    if (cursor_line < editor_first_line) editor_first_line = cursor_line;
    else if (cursor_line >= editor_first_line + UI_EDITOR_VISIBLE_LINES)
        editor_first_line = cursor_line - UI_EDITOR_VISIBLE_LINES + 1u;

    u32 line = 0u, column = 0u;
    for (u32 pos = 0u; pos < editor_size; ++pos) {
        const u8 ch = editor_buffer[pos];
        if (ch != '\n' && column >= UI_EDITOR_LINE_WIDTH) {
            ++line;
            column = 0u;
        }
        if (pos == editor_cursor && line >= editor_first_line &&
            line < editor_first_line + UI_EDITOR_VISIBLE_LINES) {
            mono_fill_rect(80u + column * 6u,
                           142u + (line - editor_first_line) * 18u + 8u,
                           5u, 1u, 1);
        }
        if (ch == '\n') {
            ++line;
            column = 0u;
            continue;
        }
        if (line >= editor_first_line &&
            line < editor_first_line + UI_EDITOR_VISIBLE_LINES) {
            mono_draw_char(80u + column * 6u,
                           142u + (line - editor_first_line) * 18u,
                           (char)ch, 1u, 0);
        }
        ++column;
    }
    if (editor_cursor == editor_size) {
        if (column >= UI_EDITOR_LINE_WIDTH) { ++line; column = 0u; }
        if (line >= editor_first_line &&
            line < editor_first_line + UI_EDITOR_VISIBLE_LINES) {
            mono_fill_rect(80u + column * 6u,
                           142u + (line - editor_first_line) * 18u + 8u,
                           5u, 1u, 1);
        }
    }
    mono_draw_text(80u, 475u, ui_status_message, 1u, 0);
    mono_draw_text(80u, 496u, "F2 SAVE  ARROWS MOVE  BACKSPACE DELETE", 1u, 0);
    if (editor_exit_confirm) {
        mono_fill_rect(135u, 225u, 530u, 80u, 1);
        mono_outline_rect(135u, 225u, 530u, 80u, 0);
        mono_draw_text(165u, 245u, "UNSAVED CHANGES", 1u, 1);
        mono_draw_text(165u, 270u, "F2 SAVE OR PRESS ESC AGAIN TO DISCARD", 1u, 1);
    }
}

static void draw_debug_console(void)
{
    draw_window_frame("DEBUG CONSOLE");
    mono_draw_text(80u, 118u, "KERNEL SERIAL LOG  UP OLDER  DOWN NEWER", 1u, 0);

    u32 scroll_limit = ui_log_count > UI_CONSOLE_VISIBLE_LINES ?
                       ui_log_count - UI_CONSOLE_VISIBLE_LINES : 0u;
    if (ui_console_scroll > scroll_limit) ui_console_scroll = scroll_limit;

    for (u32 row = 0u; row < UI_CONSOLE_VISIBLE_LINES; ++row) {
        const u32 age = ui_console_scroll + (UI_CONSOLE_VISIBLE_LINES - 1u - row);
        if (age >= ui_log_count) continue;
        const u32 newest = ui_log_next;
        const u32 index = (newest + UI_LOG_LINES - age) % UI_LOG_LINES;
        mono_draw_text(80u, 140u + row * 20u, ui_log_lines[index], 1u, 0);
    }

    mono_draw_text(80u, 486u, "R RETURN TO LATEST LOGS", 1u, 0);
    mono_draw_text(80u, 506u, "ESC HOME", 1u, 0);
}

static void draw_mouse_cursor(void)
{
    if (!mouse_is_available()) return;

    /* A black-edged, white-filled pointer stays visible on either background. */
    for (u32 row = 0u; row < 14u; ++row) {
        const u32 width = row / 2u + 1u;
        for (u32 col = 0u; col < width; ++col) {
            const u32 x = ui_mouse_x + col;
            const u32 y = ui_mouse_y + row;
            mono_put_pixel(x, y, 0);
            if (row >= 3u && col > 0u && col + 1u < width)
                mono_put_pixel(x, y, 1);
        }
    }
}

static void render_active_screen(void)
{
    switch (active_screen) {
    case UI_SCREEN_SYSTEM: draw_system_screen(); break;
    case UI_SCREEN_KEYBOARD: draw_keyboard_screen(); break;
    case UI_SCREEN_FRAMEBUFFER: draw_framebuffer_screen(); break;
    case UI_SCREEN_FILES: draw_file_manager(); break;
    case UI_SCREEN_EDITOR: draw_text_editor(); break;
    case UI_SCREEN_APP: draw_mocapp_screen(); break;
    case UI_SCREEN_CONSOLE: draw_debug_console(); break;
    case UI_SCREEN_HOME:
    default: draw_home_screen(); break;
    }
    draw_mouse_cursor();
}

static void ui_start_file_dialog(enum ui_file_dialog_mode mode)
{
    ui_file_dialog = mode;
    ui_file_dialog_length = 0u;
    for (u32 i = 0u; i < sizeof(ui_file_dialog_buffer); ++i)
        ui_file_dialog_buffer[i] = '\0';

    if (mode == UI_FILE_DIALOG_RENAME && ui_file_selection < ui_file_count) {
        ui_copy_text(ui_file_dialog_buffer, sizeof(ui_file_dialog_buffer),
                     ui_file_entries[ui_file_selection].name);
        ui_file_dialog_length = ui_text_length(ui_file_dialog_buffer);
    }
    if (mode == UI_FILE_DIALOG_NEW_FILE)
        serial_write("MICHAEL_OC_FILE_DIALOG=NEW_FILE\r\n");
    else if (mode == UI_FILE_DIALOG_NEW_DIRECTORY)
        serial_write("MICHAEL_OC_FILE_DIALOG=NEW_DIRECTORY\r\n");
    else if (mode == UI_FILE_DIALOG_RENAME)
        serial_write("MICHAEL_OC_FILE_DIALOG=RENAME\r\n");
    else if (mode == UI_FILE_DIALOG_DELETE_CONFIRM)
        serial_write("MICHAEL_OC_FILE_DIALOG=DELETE_CONFIRM\r\n");
}

static void ui_finish_file_dialog(void)
{
    const enum ui_file_dialog_mode mode = ui_file_dialog;
    ui_file_dialog = UI_FILE_DIALOG_NONE;

    if (mode == UI_FILE_DIALOG_DELETE_CONFIRM) {
        if (ui_file_selection >= ui_file_count) {
            ui_set_message("NOTHING SELECTED");
            return;
        }
        u16 index = 0u;
        if (!fs_lookup(ui_current_directory(), ui_file_entries[ui_file_selection].name, &index) ||
            !fs_delete(index)) {
            ui_set_message("DELETE FAILED");
            return;
        }
        ui_set_message("ITEM DELETED");
        ui_file_selection = 0u;
        ui_refresh_file_list();
        return;
    }

    if (ui_file_dialog_length == 0u) {
        ui_set_message("NAME CANNOT BE EMPTY");
        return;
    }

    if (mode == UI_FILE_DIALOG_NEW_FILE) {
        char name[32];
        ui_copy_text(name, sizeof(name), ui_file_dialog_buffer);
        u16 index = 0u;
        if (!fs_create_file(ui_current_directory(), name, &index)) {
            ui_set_message("CREATE FILE FAILED");
            return;
        }
        serial_write("MICHAEL_OC_FILE_CREATED\r\n");
        (void)editor_open_file(index, name, UI_SCREEN_FILES);
        return;
    }

    if (mode == UI_FILE_DIALOG_NEW_DIRECTORY) {
        char name[32];
        ui_copy_text(name, sizeof(name), ui_file_dialog_buffer);
        u16 index = 0u;
        if (!fs_make_directory(ui_current_directory(), name, &index)) {
            ui_set_message("CREATE DIRECTORY FAILED");
            return;
        }
        ui_set_message("DIRECTORY CREATED");
        ui_file_selection = 0u;
        ui_refresh_file_list();
        ui_select_name(name);
        serial_write("MICHAEL_OC_DIRECTORY_CREATED\r\n");
        return;
    }

    if (mode == UI_FILE_DIALOG_RENAME) {
        char name[32];
        ui_copy_text(name, sizeof(name), ui_file_dialog_buffer);
        if (ui_file_selection >= ui_file_count) {
            ui_set_message("NOTHING SELECTED");
            return;
        }
        u16 index = 0u;
        if (!fs_lookup(ui_current_directory(), ui_file_entries[ui_file_selection].name, &index) ||
            !fs_rename(index, name)) {
            ui_set_message("RENAME FAILED");
            return;
        }
        ui_set_message("ITEM RENAMED");
        ui_refresh_file_list();
        ui_select_name(name);
        serial_write("MICHAEL_OC_ITEM_RENAMED\r\n");
    }
}

static int ui_handle_file_manager_scancode(u8 scancode)
{
    if (ui_file_dialog != UI_FILE_DIALOG_NONE) {
        if (ui_file_dialog == UI_FILE_DIALOG_DELETE_CONFIRM) {
            if (scancode == 0x15u) {
                ui_finish_file_dialog();
                return 1;
            }
            if (scancode == 0x31u) {
                ui_file_dialog = UI_FILE_DIALOG_NONE;
                ui_set_message("DELETE CANCELLED");
                return 1;
            }
            return 0;
        }
        if (scancode == 0x1Cu) {
            ui_finish_file_dialog();
            return 1;
        }
        if (scancode == 0x0Eu) {
            if (ui_file_dialog_length > 0u)
                ui_file_dialog_buffer[--ui_file_dialog_length] = '\0';
            return 1;
        }
        const char ch = ui_scancode_to_ascii(scancode);
        if (ch != '\0' && ui_file_dialog_length < FS_NAME_MAX) {
            ui_file_dialog_buffer[ui_file_dialog_length++] = ch;
            ui_file_dialog_buffer[ui_file_dialog_length] = '\0';
            return 1;
        }
        return 0;
    }

    if (scancode == 0x48u) {
        if (ui_file_count != 0u && ui_file_selection > 0u) --ui_file_selection;
        return 1;
    }
    if (scancode == 0x50u) {
        if (ui_file_count != 0u && ui_file_selection + 1u < ui_file_count) ++ui_file_selection;
        return 1;
    }
    if (scancode == 0x0Eu) {
        if (ui_directory_depth > 0u) {
            --ui_directory_depth;
            ui_file_selection = 0u;
            ui_set_message("PARENT DIRECTORY");
        } else ui_set_message("ALREADY AT ROOT");
        return 1;
    }
    if (scancode == 0x3Cu) {
        ui_set_message("DIRECTORY REFRESHED");
        ui_refresh_file_list();
        return 1;
    }
    if (scancode == 0x1Cu) {
        if (ui_file_selection >= ui_file_count) return 0;
        u16 index = 0u;
        const fs_entry_t *entry = &ui_file_entries[ui_file_selection];
        if (!fs_lookup(ui_current_directory(), entry->name, &index)) {
            ui_set_message("ENTRY NOT FOUND");
            return 1;
        }
        if (entry->type == FS_ENTRY_DIRECTORY) {
            if (ui_directory_depth >= FS_DIRECTORY_LIMIT) {
                ui_set_message("DIRECTORY DEPTH LIMIT");
                return 1;
            }
            ui_directory_stack[ui_directory_depth++] = index;
            ui_file_selection = 0u;
            ui_set_message("DIRECTORY OPENED");
        } else if (mocapp_is_script(entry->name)) {
            (void)mocapp_start_file(index, entry->name, UI_SCREEN_FILES);
        } else {
            (void)editor_open_file(index, entry->name, UI_SCREEN_FILES);
        }
        return 1;
    }
    if (scancode == 0x31u) {
        ui_start_file_dialog(UI_FILE_DIALOG_NEW_FILE);
        return 1;
    }
    if (scancode == 0x20u) {
        ui_start_file_dialog(UI_FILE_DIALOG_NEW_DIRECTORY);
        return 1;
    }
    if (scancode == 0x13u) {
        if (ui_file_selection < ui_file_count) ui_start_file_dialog(UI_FILE_DIALOG_RENAME);
        else ui_set_message("SELECT AN ITEM FIRST");
        return 1;
    }
    if (scancode == 0x2Du) {
        if (ui_file_selection < ui_file_count) ui_start_file_dialog(UI_FILE_DIALOG_DELETE_CONFIRM);
        else ui_set_message("SELECT AN ITEM FIRST");
        return 1;
    }
    return 0;
}

static void editor_insert_byte(u8 value)
{
    if (editor_size >= UI_EDITOR_CAPACITY) {
        ui_set_message("EDITOR BUFFER FULL");
        return;
    }
    for (u32 i = editor_size; i > editor_cursor; --i)
        editor_buffer[i] = editor_buffer[i - 1u];
    editor_buffer[editor_cursor++] = value;
    ++editor_size;
    editor_dirty = 1;
    editor_exit_confirm = 0;
    ui_set_message("UNSAVED CHANGES");
}

static void editor_save(void)
{
    if (!fs_write_file(editor_file_index, editor_buffer, editor_size)) {
        ui_set_message("SAVE FAILED");
        return;
    }
    editor_dirty = 0;
    editor_exit_confirm = 0;
    ui_set_message("SAVED TO DISK");
    serial_write("MICHAEL_OC_EDITOR_SAVED\r\n");
    serial_write("MICHAEL_OC_EDITOR_SIZE=");
    serial_write_hex(editor_size);
    serial_write("\r\n");
}

static int ui_handle_editor_scancode(u8 scancode)
{
    if (scancode == 0x3Cu) { editor_save(); return 1; }
    if (scancode == 0x4Bu) {
        if (editor_cursor > 0u) --editor_cursor;
        editor_exit_confirm = 0;
        return 1;
    }
    if (scancode == 0x4Du) {
        if (editor_cursor < editor_size) ++editor_cursor;
        editor_exit_confirm = 0;
        return 1;
    }
    if (scancode == 0x48u || scancode == 0x50u) {
        editor_move_vertical(scancode == 0x50u);
        editor_exit_confirm = 0;
        return 1;
    }
    if (scancode == 0x47u) {
        while (editor_cursor > 0u && editor_buffer[editor_cursor - 1u] != '\n')
            --editor_cursor;
        return 1;
    }
    if (scancode == 0x4Fu) {
        while (editor_cursor < editor_size && editor_buffer[editor_cursor] != '\n')
            ++editor_cursor;
        return 1;
    }
    if (scancode == 0x0Eu) {
        if (editor_cursor > 0u) {
            for (u32 i = editor_cursor - 1u; i + 1u < editor_size; ++i)
                editor_buffer[i] = editor_buffer[i + 1u];
            --editor_cursor;
            --editor_size;
            editor_dirty = 1;
            editor_exit_confirm = 0;
            ui_set_message("UNSAVED CHANGES");
        }
        return 1;
    }
    if (scancode == 0x53u) {
        if (editor_cursor < editor_size) {
            for (u32 i = editor_cursor; i + 1u < editor_size; ++i)
                editor_buffer[i] = editor_buffer[i + 1u];
            --editor_size;
            editor_dirty = 1;
            editor_exit_confirm = 0;
            ui_set_message("UNSAVED CHANGES");
        }
        return 1;
    }
    if (scancode == 0x1Cu) {
        editor_insert_byte((u8)'\n');
        return 1;
    }
    const char ch = ui_scancode_to_ascii(scancode);
    if (ch != '\0') {
        editor_insert_byte((u8)ch);
        return 1;
    }
    return 0;
}

static int handle_ui_scancode(u8 scancode)
{
    if (scancode == 0xE0u || scancode == 0xE1u ||
        (scancode & 0x80u) != 0u) return 0;

    if (scancode == 0x01u) {
        if (active_screen == UI_SCREEN_APP) {
            mocapp_close("APP STOPPED");
            return 1;
        }
        if (active_screen == UI_SCREEN_EDITOR) {
            if (editor_dirty && !editor_exit_confirm) {
                editor_exit_confirm = 1;
                ui_set_message("UNSAVED CHANGES - F2 SAVE OR ESC AGAIN");
                return 1;
            }
            active_screen = editor_return_screen;
            editor_dirty = 0;
            editor_exit_confirm = 0;
            ui_set_message("EDITOR CLOSED");
            return 1;
        }
        if (active_screen == UI_SCREEN_FILES && ui_file_dialog != UI_FILE_DIALOG_NONE) {
            ui_file_dialog = UI_FILE_DIALOG_NONE;
            ui_set_message("DIALOG CANCELLED");
            return 1;
        }
        if (active_screen != UI_SCREEN_HOME) {
            active_screen = UI_SCREEN_HOME;
            return 1;
        }
        return 0;
    }

    if (active_screen == UI_SCREEN_FILES) return ui_handle_file_manager_scancode(scancode);
    if (active_screen == UI_SCREEN_EDITOR) return ui_handle_editor_scancode(scancode);
    if (active_screen == UI_SCREEN_APP) {
        mocapp_handle_key(scancode);
        return 1;
    }

    if (active_screen == UI_SCREEN_CONSOLE) {
        if (scancode == 0x48u) {
            if (ui_console_scroll < UI_LOG_LINES - UI_CONSOLE_VISIBLE_LINES)
                ++ui_console_scroll;
            return 1;
        }
        if (scancode == 0x50u) {
            if (ui_console_scroll > 0u) --ui_console_scroll;
            return 1;
        }
        if (scancode == 0x13u) {
            ui_console_scroll = 0u;
            return 1;
        }
        return 0;
    }

    if (active_screen == UI_SCREEN_HOME) {
        if (scancode == 0x02u) { active_screen = UI_SCREEN_SYSTEM; return 1; }
        if (scancode == 0x03u) { active_screen = UI_SCREEN_KEYBOARD; last_key_scancode = 0u; return 1; }
        if (scancode == 0x04u) { active_screen = UI_SCREEN_FRAMEBUFFER; return 1; }
        if (scancode == 0x05u) {
            ui_directory_depth = 0u;
            ui_file_selection = 0u;
            ui_file_dialog = UI_FILE_DIALOG_NONE;
            ui_set_message("READY");
            active_screen = UI_SCREEN_FILES;
            return 1;
        }
        if (scancode == 0x06u) {
            (void)editor_open_welcome();
            return 1;
        }
        if (scancode == 0x07u) {
            ui_console_scroll = 0u;
            active_screen = UI_SCREEN_CONSOLE;
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
    case UI_SCREEN_SYSTEM: serial_write("MICHAEL_OC_UI_SCREEN=SYSTEM\r\n"); break;
    case UI_SCREEN_KEYBOARD: serial_write("MICHAEL_OC_UI_SCREEN=KEYBOARD\r\n"); break;
    case UI_SCREEN_FRAMEBUFFER: serial_write("MICHAEL_OC_UI_SCREEN=FRAMEBUFFER\r\n"); break;
    case UI_SCREEN_FILES: serial_write("MICHAEL_OC_UI_SCREEN=FILES\r\n"); break;
    case UI_SCREEN_EDITOR: serial_write("MICHAEL_OC_UI_SCREEN=EDITOR\r\n"); break;
    case UI_SCREEN_APP: serial_write("MICHAEL_OC_UI_SCREEN=APP\r\n"); break;
    case UI_SCREEN_CONSOLE: serial_write("MICHAEL_OC_UI_SCREEN=CONSOLE\r\n"); break;
    case UI_SCREEN_HOME:
    default: serial_write("MICHAEL_OC_UI_SCREEN=HOME\r\n"); break;
    }
}

static void ui_mouse_click(u32 x, u32 y)
{
    serial_write("MICHAEL_OC_MOUSE_CLICK\r\n");

    if (active_screen == UI_SCREEN_HOME) {
        if (x >= 65u && x <= 280u && y >= 175u && y <= 212u) {
            (void)handle_ui_scancode(0x02u);
        } else if (x >= 65u && x <= 280u && y >= 215u && y <= 250u) {
            (void)handle_ui_scancode(0x03u);
        } else if (x >= 65u && x <= 280u && y >= 252u && y <= 292u) {
            (void)handle_ui_scancode(0x04u);
        } else if (x >= 395u && x <= 610u && y >= 175u && y <= 212u) {
            (void)handle_ui_scancode(0x05u);
        } else if (x >= 395u && x <= 610u && y >= 215u && y <= 250u) {
            (void)handle_ui_scancode(0x06u);
        } else if (x >= 395u && x <= 610u && y >= 252u && y <= 292u) {
            (void)handle_ui_scancode(0x07u);
        }
        return;
    }

    if (active_screen == UI_SCREEN_FILES) {
        if (ui_file_dialog != UI_FILE_DIALOG_NONE) {
            if (ui_file_dialog == UI_FILE_DIALOG_DELETE_CONFIRM &&
                y >= 260u && y <= 300u) {
                (void)handle_ui_scancode(x < 400u ? 0x15u : 0x31u);
            }
            return; /* File names are still typed with the keyboard. */
        }
        if (y >= 153u && y < 453u && x >= 80u && x <= 720u) {
            const u32 selected = (y - 153u) / 25u;
            if (selected < ui_file_count && selected < UI_FILE_VISIBLE_ENTRIES) {
                ui_file_selection = selected;
                (void)handle_ui_scancode(0x1Cu); /* Single-click opens an entry. */
            }
            return;
        }
        if (y >= 112u && y <= 128u) {
            if (x >= 250u && x < 282u) (void)handle_ui_scancode(0x31u);      /* N */
            else if (x >= 282u && x < 310u) (void)handle_ui_scancode(0x20u); /* D */
            else if (x >= 310u && x < 360u) (void)handle_ui_scancode(0x13u); /* R */
            else if (x >= 360u && x < 430u) (void)handle_ui_scancode(0x2Du); /* X */
        } else if (y >= 485u && y <= 510u) {
            if (x < 210u) (void)handle_ui_scancode(0x3Cu);                  /* F2 */
            else if (x < 290u) (void)handle_ui_scancode(0x31u);
            else if (x < 370u) (void)handle_ui_scancode(0x20u);
            else if (x < 445u) (void)handle_ui_scancode(0x13u);
        } else if (y >= 520u && y <= 550u && x < 190u) {
            (void)handle_ui_scancode(0x01u);
        }
        return;
    }

    if (active_screen == UI_SCREEN_EDITOR) {
        if (editor_exit_confirm && y >= 235u && y <= 300u) {
            (void)handle_ui_scancode(x < 400u ? 0x3Cu : 0x01u);
        } else if (y >= 138u && y < 466u && x >= 80u && x < 680u) {
            const u32 visual_line = editor_first_line + (y - 142u) / 18u;
            const u32 visual_column = (x - 80u) / 6u;
            u32 line_start = 0u, line_end = 0u;
            if (editor_find_visual_line(visual_line, &line_start, &line_end)) {
                const u32 line_length = line_end - line_start;
                editor_cursor = line_start +
                    (visual_column < line_length ? visual_column : line_length);
                editor_exit_confirm = 0;
            }
        } else if (y >= 485u && y <= 510u && x < 200u) {
            (void)handle_ui_scancode(0x3Cu); /* F2 */
        } else if (y >= 520u && y <= 550u && x < 190u) {
            (void)handle_ui_scancode(0x01u);
        }
        return;
    }

    if (active_screen == UI_SCREEN_APP) {
        if (y >= 470u && y <= 510u) {
            if (x < 290u) (void)handle_ui_scancode(0x01u);
            else if (x >= 560u) (void)handle_ui_scancode(0x3Bu); /* F1 */
        }
        return;
    }

    if (y >= 520u && y <= 550u && x >= 75u && x <= 200u)
        (void)handle_ui_scancode(0x01u);
}

static void ui_handle_mouse_event(const mouse_event_t *event)
{
    const int next_x = (int)ui_mouse_x + event->dx;
    const int next_y = (int)ui_mouse_y + event->dy;
    ui_mouse_x = next_x < 0 ? 0u :
                 (next_x >= (int)canvas_width ? canvas_width - 1u : (u32)next_x);
    ui_mouse_y = next_y < 0 ? 0u :
                 (next_y >= (int)canvas_height ? canvas_height - 1u : (u32)next_y);

    const int clicked = (event->buttons & 0x01u) != 0u &&
                        (ui_mouse_buttons & 0x01u) == 0u;
    ui_mouse_buttons = event->buttons;
    serial_write("MICHAEL_OC_MOUSE_EVENT\r\n");
    serial_write("MICHAEL_OC_MOUSE_POSITION=");
    serial_write_hex(ui_mouse_x);
    serial_write(",");
    serial_write_hex(ui_mouse_y);
    serial_write("\r\n");

    if (clicked) ui_mouse_click(ui_mouse_x, ui_mouse_y);
}

static void keyboard_event_loop(void)
{
    serial_write("MICHAEL_OC_KEYBOARD_EVENT_LOOP_OK\r\n");

    for (;;) {
        if (!timer_tick_marker_reported && timer_ticks() != 0u) {
            timer_tick_marker_reported = 1;
            serial_write("MICHAEL_OC_TIMER_TICK_OK\r\n");
        }

        mouse_event_t mouse_event;
        if (mouse_try_get_event(&mouse_event)) {
            const enum ui_screen previous_screen = active_screen;
            ui_handle_mouse_event(&mouse_event);
            if (active_screen != previous_screen) log_ui_screen_event();
            render_active_screen();
            mono_flush_framebuffer();
            continue;
        }

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


#ifdef MICHAEL_OC_STORAGE_SELF_TEST
static int kernel_bytes_equal(const u8 *left, const u8 *right, u32 size)
{
    for (u32 i = 0u; i < size; ++i)
        if (left[i] != right[i]) return 0;
    return 1;
}
static void storage_test_fail(const char *reason) __attribute__((noreturn));
static void storage_test_fail(const char *reason)
{
    serial_write("MICHAEL_OC_STORAGE_TEST_FAIL=");
    serial_write(reason);
    serial_write("\r\n");
    halt_forever();
}
static void storage_self_test(void) __attribute__((noreturn));
static void storage_self_test(void)
{
    static const u8 persistent_payload[] =
        "Michael OC disk persistence survives a cold QEMU restart.";
    static const u8 crud_payload[] = "file read/write/rename/delete";
    u8 verify[128];
    u32 size = 0u;
    u16 file_index = 0u;

    if (!fs_mount()) storage_test_fail("MOUNT");
    serial_write("MICHAEL_OC_STORAGE_FS_OK\r\n");

    const u32 payload_size = (u32)sizeof(persistent_payload);
    if (!fs_lookup(FS_ROOT, "ci-persist.txt", &file_index)) {
        if (!fs_create_file(FS_ROOT, "ci-persist.txt", &file_index))
            storage_test_fail("CREATE_PERSISTENT_FILE");
        if (!fs_write_file(file_index, persistent_payload, payload_size))
            storage_test_fail("WRITE_PERSISTENT_FILE");
        if (!fs_read_file(file_index, verify, sizeof(verify), &size) ||
            size != payload_size ||
            !kernel_bytes_equal(verify, persistent_payload, payload_size))
            storage_test_fail("READ_AFTER_WRITE");
        serial_write("MICHAEL_OC_STORAGE_PHASE=WRITE\r\n");
        halt_forever();
    }

    if (!fs_read_file(file_index, verify, sizeof(verify), &size) ||
        size != payload_size ||
        !kernel_bytes_equal(verify, persistent_payload, payload_size))
        storage_test_fail("PERSISTENCE_VERIFY");
    serial_write("MICHAEL_OC_STORAGE_PHASE=VERIFY\r\n");

    u16 folder_index = 0u;
    u16 nested_index = 0u;
    if (!fs_make_directory(FS_ROOT, "ci-folder", &folder_index))
        storage_test_fail("MKDIR");
    if (!fs_create_file(folder_index, "inside.txt", &nested_index))
        storage_test_fail("CREATE_NESTED_FILE");
    if (!fs_write_file(nested_index, crud_payload, (u32)sizeof(crud_payload)))
        storage_test_fail("WRITE_NESTED_FILE");

    fs_entry_t entries[4];
    u32 entry_count = 0u;
    if (!fs_list(folder_index, entries, 4u, &entry_count) ||
        entry_count != 1u || entries[0].type != FS_ENTRY_FILE)
        storage_test_fail("LIST_DIRECTORY");
    if (!fs_rename(nested_index, "renamed.txt"))
        storage_test_fail("RENAME_FILE");
    u16 renamed_index = 0u;
    if (fs_lookup(folder_index, "inside.txt", &renamed_index) ||
        !fs_lookup(folder_index, "renamed.txt", &renamed_index) ||
        renamed_index != nested_index)
        storage_test_fail("LOOKUP_AFTER_RENAME");
    if (!fs_read_file(renamed_index, verify, sizeof(verify), &size) ||
        size != sizeof(crud_payload) ||
        !kernel_bytes_equal(verify, crud_payload, (u32)sizeof(crud_payload)))
        storage_test_fail("READ_RENAMED_FILE");
    if (!fs_delete(renamed_index) || fs_lookup(folder_index, "renamed.txt", &renamed_index))
        storage_test_fail("DELETE_FILE");
    if (!fs_delete(folder_index)) storage_test_fail("DELETE_EMPTY_DIRECTORY");

    serial_write("MICHAEL_OC_STORAGE_CRUD_OK\r\n");
    halt_forever();
}

#endif /* MICHAEL_OC_STORAGE_SELF_TEST */

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

    if (!paging_init((const multiboot_info_t *)multiboot_info)) {
        serial_write("MICHAEL_OC_PAGING_FAIL\r\n");
        halt_forever();
    }
    serial_write("MICHAEL_OC_PAGING_OK\r\n");
    if (pmm_is_initialized()) {
        if (!paging_self_test()) {
            serial_write("MICHAEL_OC_PAGING_SELF_TEST_FAIL\r\n");
            halt_forever();
        }
        serial_write("MICHAEL_OC_PAGING_SELF_TEST_OK\r\n");
    } else {
        serial_write("MICHAEL_OC_PAGING_SELF_TEST_SKIPPED\r\n");
    }

#ifdef MICHAEL_OC_STORAGE_SELF_TEST
    storage_self_test();
#endif
    if (fs_mount()) {
        serial_write("MICHAEL_OC_FS_OK\r\n");
    } else {
        serial_write("MICHAEL_OC_FS_UNAVAILABLE\r\n");
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
#ifdef MICHAEL_OC_PAGE_FAULT_SELF_TEST
    const u32 protected_target = (u32)&kernel_main;
    serial_write("MICHAEL_OC_PROTECTED_WRITE_TARGET=");
    serial_write_hex(protected_target);
    serial_write("\r\nMICHAEL_OC_PAGE_FAULT_SELF_TEST_TRIGGER\r\n");
    *(volatile u8 *)protected_target = 0x90u;
    serial_write("MICHAEL_OC_PAGE_FAULT_TEST_FAILED\r\n");
    halt_forever();
#endif
    interrupts_enable();
    keyboard_event_loop();
}
