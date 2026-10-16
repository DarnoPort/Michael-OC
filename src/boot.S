.section .multiboot,"a"
.align 4
.long 0x1BADB002
.long 0x00000004
.long -(0x1BADB002 + 0x00000004)
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
     * Multiboot v1 enters with EAX=magic and EBX=info pointer.
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
