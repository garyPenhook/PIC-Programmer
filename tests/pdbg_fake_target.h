#ifndef PDBG_FAKE_TARGET_H
#define PDBG_FAKE_TARGET_H

#include <stdint.h>

#include "pdbg_port.h"

/* The part-facing half of pdbg_port.h, over arrays: data memory, program flash, a saved
 * context and a return stack. The test or simulator supplies pdbg_port_rx, _tx and
 * _service. */

#define FAKE_RAM_SIZE   0x1000u
#define FAKE_PFM_SIZE   0x200u
#define FAKE_STACK_MAX  31u

extern uint8_t fake_ram[FAKE_RAM_SIZE];
extern uint8_t fake_pfm[FAKE_PFM_SIZE];
extern pdbg_regs_t fake_regs;
extern uint32_t fake_stack[FAKE_STACK_MAX];
extern uint8_t fake_stack_depth;
extern pdbg_info_t fake_info;
extern unsigned fake_resets;
extern uint8_t fake_irq_enabled;

void fake_target_reset(void);

#endif
