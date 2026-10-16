/*
 * Michael OC CPU exceptions, PIC routing and PS/2 keyboard IRQ handling.
 *
 * CPU exception vectors 0..31 and PIC IRQ vectors 32..47 have IDT gates.
 * IRQ1 is the only unmasked hardware interrupt at this milestone. The
 * keyboard IRQ queues scan codes; UI state and rendering remain in the
 * kernel event loop, not inside interrupt context.
 */
#include "interrupts.h"

#define IDT_EXCEPTION_COUNT 32u
#define IDT_IRQ_COUNT       16u
#define IDT_ENTRY_COUNT     (IDT_EXCEPTION_COUNT + IDT_IRQ_COUNT)

#define COM1            0x3F8u
#define PIC_MASTER_CMD  0x20u
#define PIC_MASTER_DATA 0x21u
#define PIC_SLAVE_CMD   0xA0u
#define PIC_SLAVE_DATA  0xA1u
#define PIC_EOI         0x20u

#define PIT_CHANNEL0    0x40u
#define PIT_COMMAND     0x43u
#define PIT_INPUT_HZ    1193182u
#define PIT_TICK_HZ     100u

#define PS2_STATUS      0x64u
#define PS2_DATA        0x60u
#define PS2_OUTPUT_FULL 0x01u
#define PS2_INPUT_FULL  0x02u
#define PS2_AUX_DATA    0x20u

#define KEYBOARD_QUEUE_SIZE 64u
#define KEYBOARD_QUEUE_MASK (KEYBOARD_QUEUE_SIZE - 1u)
#define MOUSE_QUEUE_SIZE 16u
#define MOUSE_QUEUE_MASK (MOUSE_QUEUE_SIZE - 1u)
#define PS2_MOUSE_ACK 0xFAu
#define PS2_MOUSE_RESEND 0xFEu

typedef struct __attribute__((packed)) {
    u16 offset_low;
    u16 selector;
    u8 zero;
    u8 type_attribute;
    u16 offset_high;
} idt_entry_t;

typedef struct __attribute__((packed)) {
    u16 limit;
    u32 base;
} idt_register_t;

static idt_entry_t idt[IDT_ENTRY_COUNT];
static idt_register_t idt_register;

/* Single-producer (IRQ1), single-consumer (kernel event loop) ring buffer. */
static volatile u8 keyboard_queue[KEYBOARD_QUEUE_SIZE];
static volatile u8 keyboard_queue_read;
static volatile u8 keyboard_queue_write;
static volatile u32 keyboard_queue_dropped;

static volatile int mouse_queue_dx[MOUSE_QUEUE_SIZE];
static volatile int mouse_queue_dy[MOUSE_QUEUE_SIZE];
static volatile u8 mouse_queue_buttons[MOUSE_QUEUE_SIZE];
static volatile u8 mouse_queue_read;
static volatile u8 mouse_queue_write;
static volatile u32 mouse_queue_dropped;
static volatile u8 mouse_packet[3];
static volatile u8 mouse_packet_phase;
static int mouse_available;
static volatile u32 timer_tick_count;

extern void (*isr_stub_table[IDT_EXCEPTION_COUNT])(void);
extern void (*irq_stub_table[IDT_IRQ_COUNT])(void);

static void keyboard_queue_push(u8 scancode);

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

static inline void io_wait(void)
{
    outb(0x80u, 0u);
}

static void serial_putc(char ch)
{
    for (u32 wait = 0u; wait < 100000u; ++wait) {
        if ((inb((u16)(COM1 + 5u)) & 0x20u) != 0u) {
            outb((u16)COM1, (u8)ch);
            return;
        }
    }
}

static void serial_write(const char *text)
{
    while (*text != '\0') {
        serial_putc(*text++);
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

static void idt_set_gate(u32 vector, u32 handler_address)
{
    idt_entry_t *entry = &idt[vector];
    entry->offset_low = (u16)(handler_address & 0xFFFFu);
    entry->selector = 0x08u;
    entry->zero = 0u;
    entry->type_attribute = 0x8Eu;
    entry->offset_high = (u16)((handler_address >> 16) & 0xFFFFu);
}

static int ps2_wait_input_empty(void)
{
    for (u32 wait = 0u; wait < 1000000u; ++wait) {
        if ((inb(PS2_STATUS) & PS2_INPUT_FULL) == 0u) {
            return 1;
        }
        __asm__ volatile ("pause");
    }
    return 0;
}

static int ps2_wait_output_full(void)
{
    for (u32 wait = 0u; wait < 1000000u; ++wait) {
        if ((inb(PS2_STATUS) & PS2_OUTPUT_FULL) != 0u) {
            return 1;
        }
        __asm__ volatile ("pause");
    }
    return 0;
}

static void pic_remap_and_mask(void)
{
    /* Keep every IRQ masked until the IDT and keyboard controller are ready. */
    outb(PIC_MASTER_DATA, 0xFFu);
    outb(PIC_SLAVE_DATA, 0xFFu);

    outb(PIC_MASTER_CMD, 0x11u);
    io_wait();
    outb(PIC_SLAVE_CMD, 0x11u);
    io_wait();

    outb(PIC_MASTER_DATA, 0x20u);
    io_wait();
    outb(PIC_SLAVE_DATA, 0x28u);
    io_wait();

    outb(PIC_MASTER_DATA, 0x04u);
    io_wait();
    outb(PIC_SLAVE_DATA, 0x02u);
    io_wait();

    outb(PIC_MASTER_DATA, 0x01u);
    io_wait();
    outb(PIC_SLAVE_DATA, 0x01u);
    io_wait();

    outb(PIC_MASTER_DATA, 0xFFu);
    outb(PIC_SLAVE_DATA, 0xFFu);
}

static int ps2_enable_keyboard_irq(void)
{
    /* Discard stale startup bytes before requesting the controller command byte. */
    for (u32 i = 0u; i < 32u; ++i) {
        if ((inb(PS2_STATUS) & PS2_OUTPUT_FULL) == 0u) {
            break;
        }
        (void)inb(PS2_DATA);
    }

    if (!ps2_wait_input_empty()) {
        return 0;
    }
    outb(PS2_STATUS, 0x20u); /* 8042: read command byte */
    if (!ps2_wait_output_full()) {
        return 0;
    }

    u8 command_byte = inb(PS2_DATA);
    command_byte |= 0x01u;       /* Enable first-port IRQ1. */
    command_byte &= (u8)~0x10u;  /* Ensure the first keyboard port is enabled. */

    if (!ps2_wait_input_empty()) {
        return 0;
    }
    outb(PS2_STATUS, 0x60u); /* 8042: write command byte */
    if (!ps2_wait_input_empty()) {
        return 0;
    }
    outb(PS2_DATA, command_byte);

    if (!ps2_wait_input_empty()) {
        return 0;
    }
    outb(PS2_STATUS, 0xAEu); /* Enable the first PS/2 port. */
    return ps2_wait_input_empty();
}

static int ps2_mouse_wait_response(void)
{
    for (u32 wait = 0u; wait < 1000000u; ++wait) {
        const u8 status = inb(PS2_STATUS);
        if ((status & PS2_OUTPUT_FULL) == 0u) continue;
        const u8 value = inb(PS2_DATA);
        if ((status & PS2_AUX_DATA) != 0u) {
            if (value == PS2_MOUSE_ACK) return 1;
            if (value == PS2_MOUSE_RESEND) return 2;
            continue;
        }
        /* Preserve any keyboard byte observed while waiting for the mouse. */
        keyboard_queue_push(value);
    }
    return 0;
}

static int ps2_mouse_command(u8 command)
{
    for (u32 attempt = 0u; attempt < 3u; ++attempt) {
        if (!ps2_wait_input_empty()) return 0;
        outb(PS2_STATUS, 0xD4u); /* Next data byte is for the auxiliary port. */
        if (!ps2_wait_input_empty()) return 0;
        outb(PS2_DATA, command);

        const int response = ps2_mouse_wait_response();
        if (response == 1) return 1;
        if (response != 2) return 0;
    }
    return 0;
}

static void pit_initialize(void)
{
    const u16 divisor = (u16)(PIT_INPUT_HZ / PIT_TICK_HZ);
    outb(PIT_COMMAND, 0x36u); /* Channel 0, low byte/high byte, square wave. */
    outb(PIT_CHANNEL0, (u8)(divisor & 0xFFu));
    outb(PIT_CHANNEL0, (u8)(divisor >> 8));
    timer_tick_count = 0u;
}

u32 timer_ticks(void)
{
    return timer_tick_count;
}

static int ps2_enable_mouse_irq(void)
{
    if (!ps2_wait_input_empty()) return 0;
    outb(PS2_STATUS, 0xA8u); /* Enable the auxiliary/second PS/2 port. */

    if (!ps2_wait_input_empty()) return 0;
    outb(PS2_STATUS, 0x20u); /* Read controller command byte. */
    if (!ps2_wait_output_full()) return 0;
    u8 command_byte = inb(PS2_DATA);
    command_byte |= 0x02u;        /* Enable IRQ12. */
    command_byte &= (u8)~0x20u;   /* Enable the auxiliary port. */

    if (!ps2_wait_input_empty()) return 0;
    outb(PS2_STATUS, 0x60u);
    if (!ps2_wait_input_empty()) return 0;
    outb(PS2_DATA, command_byte);
    if (!ps2_wait_input_empty()) return 0;

    if (!ps2_mouse_command(0xF6u) || /* Set defaults. */
        !ps2_mouse_command(0xF4u)) { /* Enable packet reporting. */
        return 0;
    }
    return 1;
}

void interrupts_init(void)
{
    for (u32 vector = 0u; vector < IDT_EXCEPTION_COUNT; ++vector) {
        idt_set_gate(vector, (u32)isr_stub_table[vector]);
    }
    for (u32 irq = 0u; irq < IDT_IRQ_COUNT; ++irq) {
        idt_set_gate(IDT_EXCEPTION_COUNT + irq, (u32)irq_stub_table[irq]);
    }

    idt_register.limit = (u16)(sizeof(idt) - 1u);
    idt_register.base = (u32)idt;
    __asm__ volatile ("lidt %0" : : "m"(idt_register));

    idt_register_t observed;
    __asm__ volatile ("sidt %0" : "=m"(observed));
    if (observed.limit != idt_register.limit ||
        observed.base != idt_register.base) {
        serial_write("MICHAEL_OC_IDT_FAIL\r\n");
        halt_forever();
    }

    serial_write("MICHAEL_OC_IDT_OK\r\n");

    pic_remap_and_mask();
    if (!ps2_enable_keyboard_irq()) {
        serial_write("MICHAEL_OC_KEYBOARD_IRQ_SETUP_FAIL\r\n");
        halt_forever();
    }

    mouse_available = ps2_enable_mouse_irq();
    pit_initialize();
    if (mouse_available) {
        /* Unmask IRQ0 (PIT), IRQ1 (keyboard), IRQ2 (cascade), and IRQ12 (mouse). */
        outb(PIC_SLAVE_DATA, 0xEFu);
        outb(PIC_MASTER_DATA, 0xF8u);
        serial_write("MICHAEL_OC_MOUSE_IRQ_OK\r\n");
    } else {
        /* The timer and keyboard continue to work without the optional mouse. */
        outb(PIC_SLAVE_DATA, 0xFFu);
        outb(PIC_MASTER_DATA, 0xFCu);
        serial_write("MICHAEL_OC_MOUSE_IRQ_UNAVAILABLE\r\n");
    }
    serial_write("MICHAEL_OC_TIMER_IRQ_OK\r\n");
    serial_write("MICHAEL_OC_KEYBOARD_IRQ_OK\r\n");
}

void interrupts_enable(void)
{
    __asm__ volatile ("sti" : : : "memory");
}

int keyboard_try_get_scancode(u8 *scancode)
{
    if (scancode == 0 || keyboard_queue_read == keyboard_queue_write) {
        return 0;
    }

    const u8 index = keyboard_queue_read;
    *scancode = keyboard_queue[index];
    keyboard_queue_read = (u8)((index + 1u) & KEYBOARD_QUEUE_MASK);
    return 1;
}

static void mouse_queue_push(int dx, int dy, u8 buttons)
{
    const u8 write_index = mouse_queue_write;
    const u8 next = (u8)((write_index + 1u) & MOUSE_QUEUE_MASK);
    if (next == mouse_queue_read) {
        ++mouse_queue_dropped;
        return;
    }

    mouse_queue_dx[write_index] = dx;
    mouse_queue_dy[write_index] = dy;
    mouse_queue_buttons[write_index] = buttons;
    mouse_queue_write = next;
}

static int mouse_sign_extend(u8 value)
{
    return (value & 0x80u) != 0u ? (int)value - 256 : (int)value;
}

static void mouse_packet_push(u8 value)
{
    if (mouse_packet_phase == 0u) {
        /* Bit 3 is always set in the first byte; use it to re-sync packets. */
        if ((value & 0x08u) == 0u) return;
        mouse_packet[0] = value;
        mouse_packet_phase = 1u;
        return;
    }

    mouse_packet[mouse_packet_phase++] = value;
    if (mouse_packet_phase < 3u) return;
    mouse_packet_phase = 0u;

    const u8 flags = mouse_packet[0];
    if ((flags & 0xC0u) != 0u) return; /* Discard overflowed 8-bit deltas. */

    const int dx = mouse_sign_extend(mouse_packet[1]);
    const int dy = -mouse_sign_extend(mouse_packet[2]);
    mouse_queue_push(dx, dy, (u8)(flags & 0x07u));
}

static void keyboard_queue_push(u8 scancode)
{
    const u8 write_index = keyboard_queue_write;
    const u8 next = (u8)((write_index + 1u) & KEYBOARD_QUEUE_MASK);
    if (next == keyboard_queue_read) {
        ++keyboard_queue_dropped;
        return;
    }

    keyboard_queue[write_index] = scancode;
    keyboard_queue_write = next;
}

int mouse_is_available(void)
{
    return mouse_available;
}

int mouse_try_get_event(mouse_event_t *event)
{
    if (event == 0 || mouse_queue_read == mouse_queue_write) return 0;
    const u8 index = mouse_queue_read;
    event->dx = mouse_queue_dx[index];
    event->dy = mouse_queue_dy[index];
    event->buttons = mouse_queue_buttons[index];
    mouse_queue_read = (u8)((index + 1u) & MOUSE_QUEUE_MASK);
    return 1;
}

void interrupt_irq_handler(u32 irq_number)
{
    if (irq_number == 0u) {
        ++timer_tick_count;
    }

    if (irq_number == 1u) {
        const u8 status = inb(PS2_STATUS);
        if ((status & PS2_OUTPUT_FULL) != 0u &&
            (status & PS2_AUX_DATA) == 0u) {
            keyboard_queue_push(inb(PS2_DATA));
        }
    }

    if (irq_number == 12u) {
        const u8 status = inb(PS2_STATUS);
        if ((status & PS2_OUTPUT_FULL) != 0u) {
            const u8 value = inb(PS2_DATA);
            if ((status & PS2_AUX_DATA) != 0u) mouse_packet_push(value);
            else keyboard_queue_push(value);
        }
    }

    if (irq_number >= 8u) {
        outb(PIC_SLAVE_CMD, PIC_EOI);
    }
    outb(PIC_MASTER_CMD, PIC_EOI);
}

void interrupt_exception_handler(u32 vector, u32 error_code)
{
    __asm__ volatile ("cli");
    serial_write("MICHAEL_OC_EXCEPTION_VECTOR=");
    serial_write_hex(vector);
    serial_write(" ERROR=");
    serial_write_hex(error_code);
    serial_write("\r\n");
    if (vector == 14u) {
        u32 fault_address = 0u;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(fault_address));
        serial_write("MICHAEL_OC_PAGE_FAULT_CR2=");
        serial_write_hex(fault_address);
        serial_write("\r\n");
    }
    halt_forever();
}
