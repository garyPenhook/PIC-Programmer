/*
 * Startup code and interrupt vector table for the STM32G071RB (Cortex-M0+).
 *
 * On reset the core reads two words from the start of flash: the initial stack
 * pointer (word 0) and the address of Reset_Handler (word 1), then jumps there.
 * That is why the vector table must sit at 0x08000000 (see the linker script).
 */
.syntax unified
.cpu cortex-m0plus
.thumb

.global g_pfnVectors
.global Reset_Handler

/* Symbols defined elsewhere: C functions, and addresses the linker script computes. */
.extern main
.extern TIM16_IRQHandler
.extern SystemInit
.extern _sidata        /* flash address where the initial values of .data are stored */
.extern _sdata         /* start of .data in RAM */
.extern _edata         /* end of .data in RAM */
.extern _sbss          /* start of .bss in RAM */
.extern _ebss          /* end of .bss in RAM */

/*
 * Vector table (RM0444 Table 61). Each entry is one 32-bit word, and entry N sits at
 * address N * 4. Entries 0..15 are the Cortex-M core exceptions; entry 16 onwards are
 * the peripheral interrupts, so IRQ number n is at entry 16 + n (address 0x40 + 4n).
 * Reserved slots hold 0 or a harmless handler.
 */
.section .isr_vector,"a",%progbits
.type g_pfnVectors, %object
g_pfnVectors:
    .word _estack          /* 0x00: initial stack pointer (top of RAM, from the linker) */
    .word Reset_Handler    /* 0x04: Reset */
    .word Default_Handler  /* 0x08: NMI */
    .word Default_Handler  /* 0x0C: HardFault */
    .word Default_Handler  /* 0x10..0x18: reserved */
    .word Default_Handler
    .word Default_Handler
    .word 0                /* 0x1C..0x28: reserved */
    .word 0
    .word 0
    .word 0
    .word Default_Handler  /* 0x2C: SVCall */
    .word Default_Handler  /* 0x30: reserved */
    .word 0                /* 0x34: reserved */
    .word Default_Handler  /* 0x38: PendSV */
    .word Default_Handler  /* 0x3C: SysTick (not used; the LED uses TIM16) */
    /*
     * Peripheral interrupts. IRQ 0..20 (0x40..0x90) are unused and trap in
     * Default_Handler. IRQ 21 (0x94) is "TIM16 / FDCAN_IT0".
     * To use another interrupt, add its handler at entry 16 + IRQ number.
     * Note the table is only complete up to IRQ 21: enabling a higher IRQ without
     * extending the table would fetch a vector from beyond it.
     */
    .rept 21
    .word Default_Handler
    .endr
    .word TIM16_IRQHandler
.size g_pfnVectors, .-g_pfnVectors

/*
 * Reset_Handler runs first. C expects initialised globals to hold their values and
 * zero-initialised globals to be 0, but RAM is random at power-up, so we set it up
 * by hand before calling any C code:
 *   1. copy the initial values of .data from flash into RAM,
 *   2. zero .bss.
 */
.section .text.Reset_Handler
.type Reset_Handler, %function
Reset_Handler:
    ldr r0, =_sidata       /* r0 = source: .data initial values in flash */
    ldr r1, =_sdata        /* r1 = destination: start of .data in RAM */
    ldr r2, =_edata        /* r2 = end of .data in RAM */
CopyData:
    cmp r1, r2             /* done when destination reaches the end */
    bcc CopyDataWord       /* "carry clear" = unsigned r1 < r2: more to copy */
    b ZeroBssStart
CopyDataWord:
    ldr r3, [r0]           /* load a word from flash. Needs a 4-byte-aligned address: */
    str r3, [r1]           /* an unaligned one HardFaults, so the linker aligns _sidata. */
    adds r0, r0, #4        /* advance both pointers by one word */
    adds r1, r1, #4
    b CopyData

ZeroBssStart:
    ldr r1, =_sbss         /* r1 = start of .bss */
    ldr r2, =_ebss         /* r2 = end of .bss */
    movs r3, #0            /* value to store */
ZeroBss:
    cmp r1, r2
    bcc ZeroBssWord        /* still below the end: clear another word */
    b CallMain
ZeroBssWord:
    str r3, [r1]
    adds r1, r1, #4
    b ZeroBss

CallMain:
    bl SystemInit          /* clock setup hook (empty here), then the application */
    bl main
    b .                    /* main() should never return; if it does, stop here */
.size Reset_Handler, .-Reset_Handler

/*
 * Catch-all for every interrupt or fault without its own handler. An endless loop
 * makes an unexpected exception easy to find: halt the debugger and look at the PC.
 */
.section .text.Default_Handler
.type Default_Handler, %function
Default_Handler:
    b .
.size Default_Handler, .-Default_Handler
