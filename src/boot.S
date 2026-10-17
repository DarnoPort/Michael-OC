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
     * Multiboot leaves segment-selector values undefined and GDTR may be
     * unusable. Preserve the boot arguments, install our flat GDT, then load
     * the selectors used by the kernel's IDT (code=0x08, data=0x10).
     */
    pushl %eax
    pushl %ebx
    lgdt gdt_descriptor
    ljmp $0x08, $gdt_flush

gdt_flush:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss
    popl %ebx
    popl %eax

    /*
     * Keep 16-byte call-site alignment for the compiled C kernel.
     * C uses cdecl: push arguments right-to-left.
     */
    push %ebx
    push %eax
    call kernel_main

halt:
    cli
    hlt
    jmp halt

.size _start, . - _start

.section .note.GNU-stack,"",@progbits

.section .rodata
.align 8
gdt_start:
    .quad 0x0000000000000000
    .quad 0x00CF9A000000FFFF
    .quad 0x00CF92000000FFFF
gdt_end:

gdt_descriptor:
    .word gdt_end - gdt_start - 1
    .long gdt_start
