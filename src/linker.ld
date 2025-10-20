OUTPUT_FORMAT(elf64-x86-64)

ENTRY(kernel_main)

PHDRS
{
    limine_requests PT_LOAD;
    text PT_LOAD;
    rodata PT_LOAD;
    data PT_LOAD;
}

SECTIONS
{
    . = 0xffffffff80000000;

    .limine_requests : {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    } :limine_requests

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .text : {
        *(.text .text.*)
    } :text

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .rodata : {
        *(.rodata .rodata.*)
    } :rodata

    .note.gnu.build-id : {
        *(.note.gnu.build-id)
    } :rodata

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .data : {
        *(.data .data.*)
    } :data

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
    }

    KERNEL_VIRT_BASE = 0xffffffff80000000;
    KERNEL_START     = KERNEL_VIRT_BASE;
    KERNEL_END       = .;
    KERNEL_SIZE      = KERNEL_END - KERNEL_START;

    PROVIDE(kernel_virt_base = KERNEL_VIRT_BASE);
    PROVIDE(kernel_start = KERNEL_START);
    PROVIDE(kernel_end = KERNEL_END);
    PROVIDE(kernel_size = KERNEL_SIZE);
}