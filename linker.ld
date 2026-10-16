ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 1M;

    .multiboot ALIGN(4) :
    {
        KEEP(*(.multiboot))
    } :text

    .text ALIGN(16) :
    {
        *(.text .text.*)
    } :text

    .rodata ALIGN(16) :
    {
        *(.rodata .rodata.*)
    } :text

    /* Keep writable data in a separate page-aligned RW segment. */
    . = ALIGN(0x1000);

    .data :
    {
        *(.data .data.*)
    } :data

    .bss ALIGN(16) (NOLOAD) :
    {
        *(COMMON)
        *(.bss .bss.*)
    } :data

    /DISCARD/ :
    {
        *(.comment)
        *(.eh_frame)
        *(.note .note.*)
    }
}
