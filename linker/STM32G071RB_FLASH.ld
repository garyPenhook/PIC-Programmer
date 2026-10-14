/*
 * Linker script for the STM32G071RB: 128 KB flash at 0x08000000, 36 KB SRAM at
 * 0x20000000 (datasheet Table 2; 36 KB assumes RAM parity is not used).
 * The link step prints usage against these sizes, so keep them in step with the part.
 */
ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 128K
    RAM (xrw) : ORIGIN = 0x20000000, LENGTH = 36K
}

/* Initial stack pointer: the stack grows down from the top of RAM. */
_estack = ORIGIN(RAM) + LENGTH(RAM);

SECTIONS
{
    /* Vector table first: the core reads it from the start of flash at reset. */
    .isr_vector :
    {
        KEEP(*(.isr_vector))   /* KEEP stops --gc-sections discarding it (nothing calls it) */
    } > FLASH

    /* Code and read-only data. */
    .text :
    {
        *(.text*)
        *(.rodata*)
        /*
         * Align the end of the section. _etext is where the .data initial values are
         * stored, and Reset_Handler copies them with 32-bit loads. If this address were
         * not a multiple of 4 the copy would HardFault on Cortex-M0+.
         */
        . = ALIGN(4);
        _etext = .;
    } > FLASH

    _sidata = LOADADDR(.data);  /* where .data's initial values live in flash */

    /*
     * Initialised globals. They run from RAM (> RAM) but are stored in flash right
     * after .text (AT(_etext)); Reset_Handler copies them across at startup.
     */
    .data : AT(_etext)
    {
        _sdata = .;
        *(.data*)
        . = ALIGN(4);          /* keeps the word-by-word copy loop on whole words */
        _edata = .;
    } > RAM

    /* Zero-initialised globals: no flash image, just cleared by Reset_Handler. */
    .bss :
    {
        _sbss = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM

    . = ALIGN(8);
    _end = .;
}
