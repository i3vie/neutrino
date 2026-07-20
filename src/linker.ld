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
        PROVIDE(kernel_text_start = .);
        *(.text .text.*)
        PROVIDE(kernel_text_end = .);
    } :text

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .rodata : {
        PROVIDE(kernel_rodata_start = .);
        *(.rodata .rodata.*)
        . = ALIGN(8);
        PROVIDE(__kernel_modules_start = .);
        KEEP(*(.kernel_modules))
        KEEP(*(.kernel_modules.*))
        PROVIDE(__kernel_modules_end = .);
        PROVIDE(kernel_rodata_end = .);
    } :rodata

    .note.gnu.build-id : {
        *(.note.gnu.build-id)
    } :rodata

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .data : {
        PROVIDE(kernel_data_start = .);
        *(.data .data.*)
    } :data

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
        PROVIDE(kernel_data_end = .);
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
