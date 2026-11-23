/* SMC-Mixer probe: runs in place from flash as the application behind fw/boot (app area at
 * flash 0x9000, app.bin at 0x9120 = CPU 0x1E08120, which the bootloader jumps to). RAM
 * 0x12000..0x30000 like the bootloader's DFU mode: clear of the ROM's RAM (0x31B60 up) and of
 * what must survive a reset (0x316E0..0x31AE8: diagnostics, DFU magic, boot counter). */
MEMORY
{
    code (rx)  : ORIGIN = 0x1E08120, LENGTH = 0x77000 - 0x9120
    ram  (rwx) : ORIGIN = 0x12000,   LENGTH = 0x1E000
}

ENTRY(probe_entry)

SECTIONS
{
    .text :
    {
        KEEP(*(.entry))
        ASSERT(probe_entry == ORIGIN(code), "probe_entry must be the entry point");
        *(.text .text.*)
        *(.rodata .rodata.*)
        . = ALIGN(4);
        text_end = .;
    } > code

    .data :
    {
        data_begin = .;
        *(.data .data.*)
        . = ALIGN(4);
        data_end = .;
    } > ram AT > code
    data_load = LOADADDR(.data);

    .bss (NOLOAD) :
    {
        bss_begin = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        bss_end = .;
    } > ram

    stack_top = ORIGIN(ram) + LENGTH(ram);

    /* probe.bin is .text then .data, back to back */
    ASSERT(data_load == text_end, ".data is not stored right after .text")
}
