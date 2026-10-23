#include "pdbg_fake_target.h"

#include <string.h>

uint8_t fake_ram[FAKE_RAM_SIZE];
uint8_t fake_pfm[FAKE_PFM_SIZE];
pdbg_regs_t fake_regs;
uint32_t fake_stack[FAKE_STACK_MAX];
uint8_t fake_stack_depth;
pdbg_info_t fake_info;
unsigned fake_resets;
uint8_t fake_irq_enabled;

void fake_target_reset(void)
{
    unsigned i;

    memset(fake_ram, 0, sizeof fake_ram);
    for (i = 0; i < FAKE_PFM_SIZE; i++) {
        fake_pfm[i] = (uint8_t)(i * 7u + 3u);
    }
    memset(&fake_regs, 0, sizeof fake_regs);
    memset(fake_stack, 0, sizeof fake_stack);
    fake_stack_depth = 0;
    fake_info.device_id = 0x7100u;
    fake_info.revision = 0xA042u;
    fake_info.reset_cause = 0x3Cu;
    fake_resets = 0;
    fake_irq_enabled = 1;
}

uint8_t pdbg_port_irq_off(void)
{
    const uint8_t was = fake_irq_enabled;

    fake_irq_enabled = 0;
    return was;
}

void pdbg_port_irq_restore(uint8_t state)
{
    fake_irq_enabled = state;
}

uint8_t pdbg_port_rd(uint16_t addr)
{
    return addr < FAKE_RAM_SIZE ? fake_ram[addr] : 0;
}

void pdbg_port_wr(uint16_t addr, uint8_t value)
{
    if (addr < FAKE_RAM_SIZE) {
        fake_ram[addr] = value;
    }
}

uint8_t pdbg_port_rdpfm(uint32_t addr)
{
    return addr < FAKE_PFM_SIZE ? fake_pfm[addr] : 0xFF;
}

void pdbg_port_regs(pdbg_regs_t *regs)
{
    *regs = fake_regs;
}

uint8_t pdbg_port_stack_depth(void)
{
    return fake_stack_depth;
}

uint32_t pdbg_port_stack_entry(uint8_t index)
{
    return fake_stack[index];
}

void pdbg_port_info(pdbg_info_t *info)
{
    *info = fake_info;
}

void pdbg_port_reset(void)
{
    fake_resets++;
}
