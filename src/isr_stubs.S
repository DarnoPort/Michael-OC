/*
 * x86 CPU exception and legacy PIC IRQ entry stubs.
 * Exception stack layout is normalized to vector + error code. IRQs push
 * only their PIC line number. C dispatch runs with interrupts disabled.
 */
.section .text
.code32

.extern interrupt_exception_handler
.extern interrupt_irq_handler

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
    pushl 36(%eax)
    pushl 32(%eax)
    call interrupt_exception_handler
1:
    cli
    hlt
    jmp 1b

.macro IRQ_STUB number
.global irq_stub\number
.type irq_stub\number, @function
irq_stub\number:
    pushl $\number
    jmp irq_common
.endm

irq_common:
    cli
    cld
    pushal
    pushl 32(%esp)
    call interrupt_irq_handler
    addl $4, %esp
    popal
    addl $4, %esp
    iret

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

IRQ_STUB 0
IRQ_STUB 1
IRQ_STUB 2
IRQ_STUB 3
IRQ_STUB 4
IRQ_STUB 5
IRQ_STUB 6
IRQ_STUB 7
IRQ_STUB 8
IRQ_STUB 9
IRQ_STUB 10
IRQ_STUB 11
IRQ_STUB 12
IRQ_STUB 13
IRQ_STUB 14
IRQ_STUB 15

.section .rodata
.align 4
.global isr_stub_table
isr_stub_table:
    .long isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7
    .long isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15
    .long isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23
    .long isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31

.align 4
.global irq_stub_table
irq_stub_table:
    .long irq_stub0, irq_stub1, irq_stub2, irq_stub3
    .long irq_stub4, irq_stub5, irq_stub6, irq_stub7
    .long irq_stub8, irq_stub9, irq_stub10, irq_stub11
    .long irq_stub12, irq_stub13, irq_stub14, irq_stub15

.section .note.GNU-stack,"",@progbits
