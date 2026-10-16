/*
 * Common x86 exception entry.
 *
 * Exceptions which do not push an error code get a synthetic zero, keeping
 * the common stack layout: vector, error code, then the CPU's saved frame.
 */
.section .text
.code32

.extern interrupt_exception_handler

.macro ISR_NO_ERROR number
.global isr\number
.type isr\number, @function
isr\number:
    pushl $0
    pushl $\number
    jmp isr_common
.endm

.macro ISR_ERROR number
.global isr\number
.type isr\number, @function
isr\number:
    pushl $\number
    jmp isr_common
.endm

isr_common:
    cli
    cld
    pushal
    movl %esp, %eax
    /* Use the saved-frame base so the first push cannot shift the next read. */
    pushl 36(%eax)       /* CPU/synthetic error code */
    pushl 32(%eax)       /* exception vector */
    call interrupt_exception_handler
1:
    cli
    hlt
    jmp 1b

ISR_NO_ERROR 0
ISR_NO_ERROR 1
ISR_NO_ERROR 2
ISR_NO_ERROR 3
ISR_NO_ERROR 4
ISR_NO_ERROR 5
ISR_NO_ERROR 6
ISR_NO_ERROR 7
ISR_ERROR     8
ISR_NO_ERROR 9
ISR_ERROR     10
ISR_ERROR     11
ISR_ERROR     12
ISR_ERROR     13
ISR_ERROR     14
ISR_NO_ERROR 15
ISR_NO_ERROR 16
ISR_ERROR     17
ISR_NO_ERROR 18
ISR_NO_ERROR 19
ISR_NO_ERROR 20
ISR_ERROR     21
ISR_NO_ERROR 22
ISR_NO_ERROR 23
ISR_NO_ERROR 24
ISR_NO_ERROR 25
ISR_NO_ERROR 26
ISR_NO_ERROR 27
ISR_NO_ERROR 28
ISR_ERROR     29
ISR_ERROR     30
ISR_NO_ERROR 31

.section .rodata
.align 4
.global isr_stub_table
isr_stub_table:
    .long isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7
    .long isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15
    .long isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23
    .long isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31

.section .note.GNU-stack,"",@progbits
