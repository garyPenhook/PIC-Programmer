#ifndef ICSP_H
#define ICSP_H

#include <stdint.h>

/*
 * Pin-level access to a target PIC's ICSP interface. This is the only layer that
 * touches hardware, so the protocol code above it can also be built on a PC and run
 * against a simulated PIC (see tests/).
 *
 * Nucleo-G071RB wiring (Arduino header, UM2324 Table 12):
 *
 *     Nucleo        MCU pin   PIC pin
 *     D3  (CN9-4)   PB3       ICSPCLK   (driven by us)
 *     D5  (CN9-6)   PB4       ICSPDAT   (bidirectional)
 *     D4  (CN9-5)   PB5       MCLR      (driven by us)
 *     3V3           -         VDD       (target power)
 *     GND           -         VSS
 *
 * Target pin numbers for the two first-target devices (PIC16F171XX programming spec
 * DS40002266B Table 6-1; the signals are RA3 = MCLR, RA1 = ICSPCLK, RA0 = ICSPDAT):
 *
 *     device / package                     VDD  VSS  MCLR  ICSPCLK  ICSPDAT
 *     PIC16F17124, 14-pin SOIC/TSSOP/PDIP    1   14     4       12       13
 *     PIC16F17124, 16-pin QFN/VQFN           16  13     3       11       12
 *     PIC16F17146, 20-pin SSOP/SOIC/PDIP      1  20     4       18       19
 *     PIC16F17146, 20-pin QFN/VQFN-S         18  17     1       15       16
 *
 * All levels are 0 or 1. The target is powered from the Nucleo 3.3 V, within the
 * 1.8 to 5.5 V programming range in the programming specs.
 */

/*
 * Configure the three pins and the delay timer. Leaves the target running:
 * MCLR high (not in reset), ICSPCLK low, ICSPDAT an input with a weak pull-down
 * so a missing target reads as 0.
 */
void icsp_init(void);

void icsp_set_mclr(uint8_t level); /* drive MCLR (push-pull) */
void icsp_set_clk(uint8_t level);  /* drive ICSPCLK */
void icsp_set_dat(uint8_t level);  /* drive ICSPDAT as an output at this level */
void icsp_release_dat(void);       /* make ICSPDAT an input (target may drive it) */
uint8_t icsp_get_dat(void);        /* read ICSPDAT */

/* Busy-wait for at least this many microseconds. */
void icsp_delay_us(uint32_t us);

#endif
