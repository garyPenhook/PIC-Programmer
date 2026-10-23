#ifndef PDBG_PORT_H
#define PDBG_PORT_H

#include <stdint.h>

/*
 * What the agent core needs from the part. The target build implements these with
 * EUSART1, TBLRD and the saved context; the tests implement them with arrays.
 */

typedef struct {
    uint32_t pc;  /* interrupted program counter */
    uint32_t tblptr;
    uint16_t fsr0;
    uint16_t fsr1;
    uint16_t fsr2;
    uint16_t prod;
    uint8_t wreg;
    uint8_t status;
    uint8_t bsr;
    uint8_t tablat;
    uint8_t stkptr;
} pdbg_regs_t;

typedef struct {
    uint16_t device_id;
    uint16_t revision;
    uint8_t reset_cause;  /* PCON0 as found at start-up */
} pdbg_info_t;

int pdbg_port_rx(uint8_t *byte);   /* a received byte if there is one (returns 1), else 0 */
void pdbg_port_tx(uint8_t byte);   /* blocks until sent */
void pdbg_port_service(void);      /* called while halted: keep the watchdog serviced */

uint8_t pdbg_port_irq_off(void);   /* returns the previous state for pdbg_port_irq_restore */
void pdbg_port_irq_restore(uint8_t state);

uint8_t pdbg_port_rd(uint16_t addr);   /* data memory; an unmapped address reads 0 */
void pdbg_port_wr(uint16_t addr, uint8_t value);
uint8_t pdbg_port_rdpfm(uint32_t addr);

void pdbg_port_regs(pdbg_regs_t *regs);   /* the context saved on entry */
uint8_t pdbg_port_stack_depth(void);
uint32_t pdbg_port_stack_entry(uint8_t index);  /* 0 is the top of the return stack */
void pdbg_port_info(pdbg_info_t *info);
void pdbg_port_reset(void);
#endif
