.syntax unified
.cpu cortex-m0plus
.thumb

.global g_pfnVectors
.global Reset_Handler

.extern main
.extern TIM16_IRQHandler
.extern SystemInit
.extern _sidata
.extern _sdata
.extern _edata
.extern _sbss
.extern _ebss

.section .isr_vector,"a",%progbits
.type g_pfnVectors, %object
g_pfnVectors:
    .word _estack
    .word Reset_Handler
    .word Default_Handler
    .word Default_Handler
    .word Default_Handler
    .word Default_Handler
    .word Default_Handler
    .word 0
    .word 0
    .word 0
    .word 0
    .word Default_Handler
    .word Default_Handler
    .word 0
    .word Default_Handler
    .word Default_Handler
    /* IRQ 0..20 (0x40..0x90), then IRQ 21 = TIM16 / FDCAN_IT0 at 0x94 */
    .rept 21
    .word Default_Handler
    .endr
    .word TIM16_IRQHandler
.size g_pfnVectors, .-g_pfnVectors

.section .text.Reset_Handler
.type Reset_Handler, %function
Reset_Handler:
    ldr r0, =_sidata
    ldr r1, =_sdata
    ldr r2, =_edata
CopyData:
    cmp r1, r2
    bcc CopyDataWord
    b ZeroBssStart
CopyDataWord:
    ldr r3, [r0]
    str r3, [r1]
    adds r0, r0, #4
    adds r1, r1, #4
    b CopyData

ZeroBssStart:
    ldr r1, =_sbss
    ldr r2, =_ebss
    movs r3, #0
ZeroBss:
    cmp r1, r2
    bcc ZeroBssWord
    b CallMain
ZeroBssWord:
    str r3, [r1]
    adds r1, r1, #4
    b ZeroBss

CallMain:
    bl SystemInit
    bl main
    b .
.size Reset_Handler, .-Reset_Handler

.section .text.Default_Handler
.type Default_Handler, %function
Default_Handler:
    b .
.size Default_Handler, .-Default_Handler
