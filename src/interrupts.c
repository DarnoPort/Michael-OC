/*
 * Michael OC protected-mode interrupt descriptor table and exception reports.
 * This establishes exception handlers only; hardware IRQ routing is a later
 * milestone. The IF flag stays clear until a PIC/PIT/keyboard IRQ setup exists.
 */
#include "interrupts.h"
#include "multiboot.h"

#define COM1 0x3F8u
#define IDT_ENTRIES 32u
#define IDT_INTERRUPT_GATE 0x8Eu

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
    serial_write("\\r\\n");

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

    serial_write("MICHAEL_OC_IDT_OK\r\n");

#ifdef MICHAEL_OC_EXCEPTION_TEST
    /* This build is only used by make test-exception, never by the normal ISO. */
    serial_write("MICHAEL_OC_EXCEPTION_TEST_TRIGGER\r\n");
    __asm__ volatile ("int3");
    stop_with_init_error("MICHAEL_OC_EXCEPTION_TEST_FAILED: INT3 returned");
#endif
}

static const char *exception_name(u32 vector)
{
    static const char *const names[IDT_ENTRIES] = {
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

    return vector < IDT_ENTRIES ? names[vector] : "unknown exception";
}

void interrupt_dispatch(const interrupt_frame_t *frame)
{
    if (frame == 0) {
        stop_with_init_error("MICHAEL_OC_EXCEPTION: missing register frame");
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
