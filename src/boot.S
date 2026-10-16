.section .multiboot,"a"
.align 4
.long 0x1BADB002
.long 0x00000004
.long -(0x1BADB002 + 0x00000004)

/*
 * Multiboot v1 keeps graphics fields at byte offset 32. The five words
 * below occupy the reserved/address-field slots at offsets 12..28; because
 * flag 16 is clear, GRUB does not interpret them as load addresses.
 */
.long 0
.long 0
.long 0
.long 0
.long 0

/* Request a common linear 800x600x32 mode. */
.long 0
.long 800
.long 600
.long 32

.section .bss
.align 16
stack_bottom:
.skip 16384
stack_top:

.section .text
.global _start
.type _start, @function
_start:
    cli
    mov $stack_top, %esp
    and $-16, %esp
    sub $8, %esp
    xor %ebp, %ebp

    /*
     * Install the exception IDT before entering C kernel code. Preserve both
     * Multiboot arguments on the stack because the initializer may clobber
     * caller-saved registers. IF remains disabled until hardware IRQs exist.
     */
    push %eax
    push %ebx
    call interrupts_init
    mov 4(%esp), %eax
    mov (%esp), %ebx
    add $8, %esp

    /* C uses cdecl: push arguments right-to-left (info, then magic). */
    push %ebx
    push %eax
    call kernel_main

halt:
    cli
    hlt
    jmp halt

.size _start, . - _start

.section .note.GNU-stack,"",@progbits
