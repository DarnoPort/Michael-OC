.section .multiboot
.align 4
.long 0x1BADB002
.long 0x00000000
.long -(0x1BADB002 + 0x00000000)

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
    xor %ebp, %ebp

    /*
     * Multiboot v1 enters with EAX=magic and EBX=info pointer.
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
