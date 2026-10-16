/*
 * Michael OC protected-mode interrupt descriptor table and exception reports.
 * This establishes exception handlers only; hardware IRQ routing is a later
 * milestone. The IF flag stays clear until a PIC/PIT/keyboard IRQ setup exists.
 */
#include "interrupts.h"
#include "multiboot.h"

#define COM1 0x3F8u
#define EXCEPTION_COUNT 32u
#define IRQ_BASE 32u
#define IRQ_COUNT 16u
#define IDT_ENTRIES (EXCEPTION_COUNT + IRQ_COUNT)
#define IDT_INTERRUPT_GATE 0x8Eu
#define PIC_MASTER_COMMAND 0x20u
#define PIC_MASTER_DATA 0x21u
#define PIC_SLAVE_COMMAND 0xA0u
#define PIC_SLAVE_DATA 0xA1u
#define PIC_EOI 0x20u
#define PS2_DATA 0x60u
#define PS2_STATUS_COMMAND 0x64u
#define PIT_CHANNEL0_DATA 0x40u
#define PIT_COMMAND 0x43u
#define PIT_FREQUENCY_HZ 100u
#define KEYBOARD_IRQ 1u
#define SCANCODE_QUEUE_SIZE 128u
#define SCANCODE_QUEUE_MASK (SCANCODE_QUEUE_SIZE - 1u)

typedef struct __attribute__((packed)) {
    u16 offset_low;
    u16 selector;
    u8 zero;
    u8 type_attr;
    u16 offset_high;
} idt_gate_t;

typedef struct __attribute__((packed)) {
    u16 limit;
    u32 base;
} idtr_t;

typedef struct {
    u32 edi;
    u32 esi;
    u32 ebp;
    u32 saved_esp;
    u32 ebx;
    u32 edx;
    u32 ecx;
    u32 eax;
    u32 vector;
    u32 error_code;
    u32 eip;
    u32 cs;
    u32 eflags;
} interrupt_frame_t;

static idt_gate_t idt[IDT_ENTRIES];
static idtr_t idtr;
static volatile u8 scancode_queue[SCANCODE_QUEUE_SIZE];
static volatile u8 scancode_head;
static volatile u8 scancode_tail;
static volatile u32 pit_tick_count;

extern u32 isr_stub_table[IDT_ENTRIES];

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

static void pic_remap_and_mask(void)
{
    outb(PIC_MASTER_COMMAND, 0x11u);
    io_wait();
    outb(PIC_SLAVE_COMMAND, 0x11u);
    io_wait();
    outb(PIC_MASTER_DATA, (u8)IRQ_BASE);
    io_wait();
    outb(PIC_SLAVE_DATA, (u8)(IRQ_BASE + 8u));
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

static int ps2_wait_input_empty(void)
{
    for (u32 i = 0u; i < 100000u; ++i) {
        if ((inb(PS2_STATUS_COMMAND) & 0x02u) == 0u) return 1;
    }
    return 0;
}

static int ps2_wait_output_full(void)
{
    for (u32 i = 0u; i < 100000u; ++i) {
        if ((inb(PS2_STATUS_COMMAND) & 0x01u) != 0u) return 1;
    }
    return 0;
}

static int ps2_enable_keyboard_irq(void)
{
    if (!ps2_wait_input_empty()) return 0;
    outb(PS2_STATUS_COMMAND, 0x20u);
    if (!ps2_wait_output_full()) return 0;

    u8 command_byte = inb(PS2_DATA);
    command_byte = (u8)((command_byte | 0x01u) & (u8)~0x10u);

    if (!ps2_wait_input_empty()) return 0;
    outb(PS2_STATUS_COMMAND, 0x60u);
    if (!ps2_wait_input_empty()) return 0;
    outb(PS2_DATA, command_byte);
    if (!ps2_wait_input_empty()) return 0;

    while ((inb(PS2_STATUS_COMMAND) & 0x01u) != 0u) {
        (void)inb(PS2_DATA);
    }
    return 1;
}

static void enqueue_scancode(u8 scancode)
{
    const u8 head = scancode_head;
    const u8 next = (u8)((head + 1u) & SCANCODE_QUEUE_MASK);
    if (next == scancode_tail) return;
    scancode_queue[head] = scancode;
    scancode_head = next;
}

int interrupts_read_keyboard_scancode(u8 *scancode)
{
    if (scancode == 0) return 0;
    const u8 tail = scancode_tail;
    if (tail == scancode_head) return 0;
    *scancode = scancode_queue[tail];
    scancode_tail = (u8)((tail + 1u) & SCANCODE_QUEUE_MASK);
    return 1;
}

static void send_pic_eoi(u32 irq)
{
    if (irq >= 8u) outb(PIC_SLAVE_COMMAND, PIC_EOI);
    outb(PIC_MASTER_COMMAND, PIC_EOI);
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
    for (u32 spin = 0u; spin < 100000u; ++spin) {
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

static __attribute__((noreturn)) void stop_with_init_error(const char *message)
{
    serial_write(message);
    serial_write("\r\n");
    __asm__ volatile ("cli");
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

int interrupts_enable_timer_keyboard_irqs(void)
{
    if (!ps2_enable_keyboard_irq()) {
        serial_write("MICHAEL_OC_KEYBOARD_IRQ_FAILED: PS/2 controller timeout\r\n");
        return 0;
    }

    /* Program PIT channel 0 to ~100 Hz before unmasking the timer line. */
    const u16 divisor = (u16)(1193182u / PIT_FREQUENCY_HZ);
    outb(PIT_COMMAND, 0x36u);
    outb(PIT_CHANNEL0_DATA, (u8)(divisor & 0xFFu));
    outb(PIT_CHANNEL0_DATA, (u8)(divisor >> 8));
    serial_write("MICHAEL_OC_PIT_OK\\r\\n");

    /* Only the timer and keyboard IRQs are enabled; all others stay masked. */
    outb(PIC_MASTER_DATA, 0xFCu);
    outb(PIC_SLAVE_DATA, 0xFFu);
    __asm__ volatile ("sti" : : : "memory");
    serial_write("MICHAEL_OC_HARDWARE_IRQ_OK\\r\\n");
    serial_write("MICHAEL_OC_KEYBOARD_IRQ_OK\\r\\n");
    return 1;
}

u32 interrupts_timer_ticks(void)
{
    return pit_tick_count;
}

void interrupts_init(void)
{
    serial_init();

    /*
     * Multiboot guarantees a valid flat code segment but the selector value
     * belongs to the bootloader's GDT, so do not assume it is always 0x08.
     */
    u16 code_selector;
    __asm__ volatile ("mov %%cs, %0" : "=r"(code_selector));
    serial_write("MICHAEL_OC_IDT_CS=");
    serial_write_hex((u32)code_selector);
    serial_write("\r\n");

    for (u32 vector = 0u; vector < IDT_ENTRIES; ++vector) {
        const u32 handler = isr_stub_table[vector];
        if (handler == 0u) {
            stop_with_init_error("MICHAEL_OC_IDT_FAILED: null handler");
        }

        idt[vector].offset_low = (u16)(handler & 0xFFFFu);
        idt[vector].selector = code_selector;
        idt[vector].zero = 0u;
        idt[vector].type_attr = IDT_INTERRUPT_GATE;
        idt[vector].offset_high = (u16)(handler >> 16);
    }

    idtr.limit = (u16)(sizeof(idt) - 1u);
    idtr.base = (u32)&idt[0];
    __asm__ volatile ("lidt %0" : : "m"(idtr) : "memory");

    idtr_t readback;
    __asm__ volatile ("sidt %0" : "=m"(readback));
    if (readback.limit != idtr.limit || readback.base != idtr.base) {
        stop_with_init_error("MICHAEL_OC_IDT_FAILED: SIDT mismatch");
    }

    pic_remap_and_mask();
    scancode_head = 0u;
    scancode_tail = 0u;
    pit_tick_count = 0u;

    serial_write("MICHAEL_OC_IDT_OK\r\n");
    serial_write("MICHAEL_OC_PIC_REMAP_OK\r\n");

#ifdef MICHAEL_OC_EXCEPTION_TEST
    /* This build is only used by make test-exception, never by the normal ISO. */
    serial_write("MICHAEL_OC_EXCEPTION_TEST_TRIGGER\r\n");
    __asm__ volatile ("int3");
    stop_with_init_error("MICHAEL_OC_EXCEPTION_TEST_FAILED: INT3 returned");
#endif
}

static const char *exception_name(u32 vector)
{
    static const char *const names[EXCEPTION_COUNT] = {
        "divide error", "debug", "non-maskable interrupt", "breakpoint",
        "overflow", "bound range", "invalid opcode", "device not available",
        "double fault", "coprocessor segment overrun", "invalid TSS",
        "segment not present", "stack-segment fault", "general protection",
        "page fault", "reserved exception 15", "x87 floating-point",
        "alignment check", "machine check", "SIMD floating-point",
        "virtualization", "control protection", "reserved exception 22",
        "reserved exception 23", "reserved exception 24",
        "reserved exception 25", "reserved exception 26",
        "reserved exception 27", "hypervisor injection",
        "VMM communication", "security exception", "reserved exception 31"
    };

    return vector < EXCEPTION_COUNT ? names[vector] : "unknown exception";
}

void interrupt_dispatch(const interrupt_frame_t *frame)
{
    if (frame == 0) {
        stop_with_init_error("MICHAEL_OC_EXCEPTION: missing register frame");
    }

    if (frame->vector >= IRQ_BASE && frame->vector < IRQ_BASE + IRQ_COUNT) {
        const u32 irq = frame->vector - IRQ_BASE;
        if (irq == 0u) {
            ++pit_tick_count;
        } else if (irq == KEYBOARD_IRQ) {
            const u8 status = inb(PS2_STATUS_COMMAND);
            if ((status & 0x01u) != 0u) {
                const u8 value = inb(PS2_DATA);
                if ((status & 0x20u) == 0u) enqueue_scancode(value);
            }
        }
        send_pic_eoi(irq);
        return;
    }

    serial_write("MICHAEL_OC_FATAL_EXCEPTION vector=");
    serial_write_hex(frame->vector);
    serial_write(" name=");
    serial_write(exception_name(frame->vector));
    serial_write(" error=");
    serial_write_hex(frame->error_code);
    serial_write(" eip=");
    serial_write_hex(frame->eip);
    serial_write(" cs=");
    serial_write_hex(frame->cs);
    serial_write(" eflags=");
    serial_write_hex(frame->eflags);
    serial_write("\r\nSystem halted.\r\n");

    __asm__ volatile ("cli");
    for (;;) {
        __asm__ volatile ("hlt");
    }
}
