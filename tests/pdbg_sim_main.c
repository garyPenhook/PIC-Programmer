/*
 * The debug agent core (target/pdbg) on the PC: stdin is the UART receive, stdout the
 * transmit, and the part is the fake of pdbg_fake_target.c. host/tests/test_pdbg.py will drive
 * it with picprog.debug.pdbg.
 *
 * Usage: pdbg_sim [--hit SITE@FRAMES]
 *   --hit   the "application" reaches PDBG_BREAK(SITE) once FRAMES frames have been sent,
 *           so a test can see a breakpoint stop a running program.
 * Fake state: RAM byte i is i & 0xFF, return stack 0x001234, 0x000456, 0x000789.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pdbg.h"
#include "pdbg_fake_target.h"

static unsigned frames_sent;
static int hit_site = -1;
static unsigned hit_after;

int pdbg_port_rx(uint8_t *byte)
{
    const int c = getchar();

    if (c == EOF) {
        exit(0);
    }
    *byte = (uint8_t)c;
    return 1;
}

void pdbg_port_tx(uint8_t byte)
{
    putchar(byte);
    if (byte == 0u) {
        frames_sent++;
        fflush(stdout);
    }
}

void pdbg_port_service(void)
{
}

int main(int argc, char **argv)
{
    uint8_t byte;
    unsigned i;

    if (argc == 3 && strcmp(argv[1], "--hit") == 0) {
        hit_site = atoi(argv[2]);
        hit_after = (unsigned)atoi(strchr(argv[2], '@') + 1);
    }
    fake_target_reset();
    for (i = 0; i < FAKE_RAM_SIZE; i++) {
        fake_ram[i] = (uint8_t)i;
    }
    fake_regs.pc = 0x1002u;
    fake_regs.wreg = 0x11u;
    fake_regs.status = 0x04u;
    fake_regs.bsr = 0x01u;
    fake_regs.fsr0 = 0x0210u;
    fake_regs.fsr1 = 0x0320u;
    fake_regs.fsr2 = 0x0430u;
    fake_regs.prod = 0xBEEFu;
    fake_regs.tblptr = 0x012345u;
    fake_regs.tablat = 0x99u;
    fake_stack[0] = 0x001234u;
    fake_stack[1] = 0x000456u;
    fake_stack[2] = 0x000789u;
    fake_stack_depth = 3;
    fake_regs.stkptr = 3;
    pdbg_init();
    for (;;) {
        (void)pdbg_port_rx(&byte);
        if (pdbg_feed(byte) == PDBG_HALT) {
            pdbg_enter(PDBG_REASON_REQUEST, 0u);
        }
        if (hit_site >= 0 && frames_sent >= hit_after) {
            const int site = hit_site;

            hit_site = -1;
            pdbg_break((uint8_t)site);
        }
    }
}
