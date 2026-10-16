/*
 * Michael OC CPU exception / IDT foundation.
 *
 * This first interrupt milestone installs gates for CPU exception vectors
 * 0..31. External hardware IRQs remain masked/disabled and are not routed yet.
 */
#include "interrupts.h"
#include "multiboot.h"

#define IDT_EXCEPTION_COUNT 32u
#define COM1 0x3F8u

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

static idt_entry_t idt[IDT_EXCEPTION_COUNT];
static idt_register_t idt_register;

extern void (*isr_stub_table[IDT_EXCEPTION_COUNT])(void);

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

void interrupts_init(void)
{
    for (u32 vector = 0u; vector < IDT_EXCEPTION_COUNT; ++vector) {
        idt_set_gate(vector, (u32)isr_stub_table[vector]);
    }

    idt_register.limit = (u16)(sizeof(idt) - 1u);
    idt_register.base = (u32)idt;
    __asm__ volatile ("lidt %0" : : "m"(idt_register));

    /* Confirm that the CPU accepted the descriptor-table base and limit. */
    idt_register_t observed;
    __asm__ volatile ("sidt %0" : "=m"(observed));
    if (observed.limit != idt_register.limit ||
        observed.base != idt_register.base) {
        serial_write("MICHAEL_OC_IDT_FAIL\r\n");
        halt_forever();
    }

    serial_write("MICHAEL_OC_IDT_OK\r\n");
}

void interrupt_exception_handler(u32 vector, u32 error_code)
{
    __asm__ volatile ("cli");
    serial_write("MICHAEL_OC_EXCEPTION_VECTOR=");
    serial_write_hex(vector);
    serial_write(" ERROR=");
    serial_write_hex(error_code);
    serial_write("\r\n");
    halt_forever();
}
