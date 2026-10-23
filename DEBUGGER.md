# Debugger

Milestone M10. Source-level debugging of a PIC18F27Q10 with the Nucleo as the probe: halt
(at a breakpoint or on request from the PC), read and change RAM, SFRs and variables by
name, show the call stack, resume, reset, and a build-program-run loop, all from the PC.
Page numbers below are DS40002043F (data sheet) unless another document is named.

## Why a software agent, not the on-chip debugger

- The part has on-chip debug: "In-Circuit Debug (ICD) with Three Breakpoints via Two Pins"
  (p.4), on ICSPCLK = RB6 and ICSPDAT = RB7 (Table 3, p.5), enabled by CONFIG2 `DEBUG`
  (p.31). No document on disk describes its protocol or the debug executive; the
  programming spec DS40001874H covers programming only. The Nucleo cannot drive it.
- Errata DS80000832G 1.8.1: "Software breakpoints are not available" on this silicon even
  with Microchip's own debugger, so the hardware route is limited to three breakpoints
  anyway.
- MDB (MPLAB X v6.35) only attaches to Microchip tools, so it cannot use the Nucleo. It is
  used here as a simulator (see "Testing").

So the debugger is an agent library linked into the target program, talking to the PC over
EUSART1 through the Nucleo's existing `BRIDGE`. The cost: it uses one EUSART, one
interrupt and some flash and RAM; peripherals and timers keep running while the CPU is
halted; and it cannot debug code that runs with interrupts off or that breaks the agent.

## Facts the design depends on

1. **Pins (28-pin SPDIP/SOIC/SSOP, Table 3 p.5-6):** RC6 pin 17, RC7 pin 18, RB6 pin 27
   (ICSPCLK), RB7 pin 28 (ICSPDAT), RE3 pin 1 (MCLR), VDD pin 20, VSS pins 8 and 19.
2. **EUSART1 through PPS (p.286-287):** RX1PPS resets to RC7 (0x17), so RX needs no PPS
   write. TX does: RC6PPS = 0x09 (EUSART1 TX/CK). EUSART2 defaults to RB6/RB7, the ICSP
   pins, so the agent uses EUSART1.
3. **Baud rate (Table 29-1 p.511):** SYNC = 0, BRG16 = 1, BRGH = 1 gives
   FOSC / [4 (n+1)]; at 64 MHz, n = 138 gives 115 108 baud (-0.08 %).
4. **Clock accuracy: auto-baud is required, not optional.** Both ends run from RC
   oscillators. The baud budget at 115 200 baud on a bench at 0 to 60 C:
   - PIC HFINTOSC: ±2 % in that band (Figure 39-5 p.701; up to ±5 % outside it, and errata
     DS80000832G 1.7.1 says up to 5 % over the whole range). These bands come from the text
     of the figure, which loses the exact VDD and temperature edges; read them off the drawing.
   - Nucleo HSI16 (DS12232 Table 41): 15.88 to 16.08 MHz at 3.0 V and 30 C (-0.75 % to
     +0.5 %), plus ±1 % from 0 to 85 C and -0.1 % to +0.05 % over VDD: -1.85 % to +1.55 %.
   - Quantisation: both ends land on 115 108 baud (PIC n = 138; STM32 BRR = 556, from
     `target_uart.c`), so it cancels.
   - Nucleo receiver tolerance (RM0444 33.5.8, Table 189: BRR[3:0] = 0xC, 8 data bits,
     oversampling by 16, ONEBIT = 0 as `target_uart.c` leaves it): 3.33 %; 3.88 % with
     ONEBIT = 1.

   Worst case is 2 % + 1.85 % = 3.85 %, above 3.33 %. Bytes from the PIC can be lost even
   at room temperature. The PIC data sheet gives no receiver tolerance for the other
   direction. The fix is auto-baud detect on the PIC (29.2.1 p.514): the PC sends 0x55,
   ABD times eight bit periods at FOSC/32 and leaves the bit time in FOSC/4 counts
   (subtract 1 for SPxBRG). That cancels both oscillators at the moment of sync, leaving
   one BRG count, 1 in 139 (about 0.7 %), at 115 200 baud. The agent syncs at every
   `HELLO` and after a reset, and again when receive framing errors appear. A crystal on
   the PIC (the errata work-around) would do instead.
5. **Interrupts (15.1-15.3 p.193-194):** high priority at 0008h, low at 0018h; with IPEN =
   0 everything goes to 0008h. RC1IF/RC1IE/RC1IP are bit 5 of PIR3/PIE3/IPR3 (p.197). The
   agent's receive interrupt is high priority so a halt request can stop low-priority ISRs.
6. **Context (15.11 p.195, 11.1.2.5 p.112):** WREG, STATUS and BSR go to the fast register
   stack on interrupt, which "is neither readable nor writable". The agent must save them
   itself, before any compiler prologue, to report the interrupted values.
7. **Return stack (11.1.2 p.110):** 31 levels; STKPTR and TOSU:TOSH:TOSL are readable and
   writable, with GIE off while touching them. The agent walks the whole stack by stepping
   STKPTR down, reading TOS and putting STKPTR back: a full backtrace on real silicon.
8. **Flash reads:** TBLRD with TBLPTR (12.1.1, p.142-145), so the agent can read PFM, User
   ID and config without ICSP. Self-writes have errata 1.9.1 (the next instruction may not
   execute; two NOPs, which `<xc.h>` adds) and 1.6.2 (no PFM writes above 85 C).
9. **Watchdog (p.88, 10.5.1 p.99):** the WWDT resets the part without CLRWDT, and in
   windowed mode it must be armed first. A halted agent must keep it serviced, or
   debugging requires WDTE off.
10. **Silicon revisions (errata Table 1):** device ID 0x7100, revision A4 = 0xA004 or B2 =
    0xA042. `picprog id` tells which one is on the bench.

Checked with the installed tools (2026-10-22):

- XC8 v4.00 (`-mcpu=18F27Q10 -mdfp=.../PIC18F-Q_DFP/1.30.487/xc8 -gdwarf-3`) writes an ELF
  with `.debug_info` and `.debug_line`, plus a `.sym` file (`_counter 1 0 COMRAM 1`). The
  host gets PC to file:line and variable addresses and types from these.
- XC8 rejects `#pragma config DEBUG=...` (error 1363), so XC8 builds keep `DEBUG` = 1.
  A cleared `DEBUG` comes from MPLAB debug builds or hand-edited hex.
- The MDB simulator ran such an ELF on PIC18F27Q10, wrote EUSART1 output to a file
  (`set uart1io.uartioenabled true`, `uart1io.output file`), printed a variable by name and
  gave a source-level backtrace.
- The simulator also feeds EUSART1 *input*, from an SCL stimulus file attached with `stim`.
  `packetin(s, RC1REG, true)` queues bytes that the simulated UART delivers at the baud
  rate, setting RC1IF each time. A test that echoed from a high-priority RC1 interrupt
  received 41 42 00 03 7E and sent the same five bytes out. Three traps: the string must
  be assigned in the process body, because an initialiser in its declaration stays empty
  and `packetin()` throws (E0108); the field is `PIR3.RCIF`, since SCL rejects `RC1IF`; and
  writing `RC1REG` from SCL does not fill the receive FIFO. The simulator's UART I/O
  options are output only. A test is a scripted run, not a live session: the commands go
  in the SCL file, and the replies are read from the output file afterwards.
- pyelftools 0.33 reads the XC8 DWARF (machine `EM_MCHP_PIC`, DWARF 3, 4-byte addresses).
  The line table maps PC to `file:line`, ISR at 0008h included. Globals carry
  `DW_OP_addr` data-space addresses that match the `.sym` file (`rxcount` at 0x0D). The
  types come through, arrays included (`unsigned char[8]`). Functions have absolute
  `low_pc`/`high_pc` (`main` 0x1002 to 0x102D). XC8 gives locals fixed addresses too (a
  local `b` at 0x10), so the host prints locals without unwinding frames. SFRs from the
  device header come with addresses as well (`RC1REG` at 0x0F98, as in the pack's
  `PIC18F27Q10.PIC`).

## Pieces

**Target agent (`target/pdbg/`, C for XC8, new directory):**

- Owns EUSART1 on RC6/RC7 and its receive interrupt. Halt entry points, all reaching one
  agent loop:
  - `PDBG_BREAK(n)`: a compiled breakpoint site. A RAM bitmap enables each site, so the PC
    turns breakpoints on and off without reflashing; a disabled site costs a call and a
    bit test.
  - Halt request: the PC sends a break byte; the RC1 interrupt enters the agent with the
    interrupted PC on TOS.
  - `PDBG_ASSERT(cond)`, and a report of the reset cause (PCON0, stack overflow and
    underflow, p.110) on the first contact after reset.
- Entry is a short assembly stub that saves WREG, STATUS, BSR, FSR0-2, PROD, TBLPTR and
  TABLAT before C runs (fact 6). The first prototype in the simulator must settle how this
  stub sits in front of an application's own XC8 high-priority ISR: the agent owns 0008h
  and calls the application's handler, or the application calls `pdbg_isr()` first.
- Halted: interrupts off, polls EUSART1, keeps the WWDT serviced (fact 9), serves
  commands, then restores the context and returns with RETFIE or RETURN.
- Running: serves read requests from the interrupt without halting. This gives a live
  watch of variables at a few updates a second.
- Console: `putch()` for `printf` goes through the agent, so application text and debug
  replies share the one UART.
- Budget, to be measured with the XC8 memory summary: aim for under 1.5 KB of flash and
  under 100 bytes of RAM plus the receive buffer.

**Wire protocol (PC to agent, through `BRIDGE`):** frames are COBS-encoded with a 0x00
delimiter and end in CRC-16/CCITT-FALSE, the same CRC as `crc16()` in `host/picprog/link.py`
and `src/cmd.c`. COBS resynchronises after a lost byte, which matters on a link between
two RC oscillators (fact 4). An empty frame (0x00 0x00) means halt: COBS never puts 0x00 inside a frame,
so the receive interrupt can spot it without decoding. Target to PC frames have a channel:
console text, reply, or async event (halted, with reason and PC). The bridge closes on
`BRIDGE_EXIT` anywhere in the PC's bytes, so `pdbg.py` must refuse, or split, a `WR` whose
encoded frame would contain it. Commands:

| Command | Does |
| --- | --- |
| `HELLO` | agent version, buffer size, device and revision ID (TBLRD), reset cause |
| `HALT` / `CONT` | stop, resume |
| `REGS` | saved context, STKPTR |
| `STACK` | every return address (fact 7) |
| `RD addr len` / `WR addr data` | data memory, SFRs included |
| `RDPFM addr len` | program flash, User ID, config (fact 8) |
| `BP n on/off` | compiled breakpoint sites |
| `RESET` | RESET instruction; the Nucleo's `RESET` (below) pulses MCLR instead |

**Nucleo firmware:** `BRIDGE` already runs the PC at 921600 baud and USART1 at the target
rate with a 2048-byte buffer (`include/bridge.h`). One new command: `RESET`, which pulses
MCLR (PB5, D4) low and releases it, with the pulse width taken from the data sheet's reset
timing. Today the only way to reset the target is the side effect of entering and leaving
programming mode (`ID`). Auto-baud needs nothing from the Nucleo: the PC's 0x55 passes
through the bridge. Later, only if the round trip proves too slow or fragile: a reset
inside the bridge.

**Host (`host/picprog/debug/`):**

- `pdbg.py`: framing and commands, against any byte stream, like `link.py`.
- `elfinfo.py`: symbols, DWARF line table, variable types and addresses from the XC8 ELF,
  with pyelftools as a new dependency in `host/pyproject.toml` (checked above). The `.sym`
  file is a cross-check for addresses.
- `picprog debug app.elf`: programs the `.hex` that XC8 writes next to the ELF (existing
  `write` and verify),
  releases MCLR, opens the bridge, sends `HELLO`, then an interactive shell: `break
  file:line` (picks the `PDBG_BREAK` site there), `cont`, `halt`, `bt`, `print var`,
  `set var = v`, `x addr`, `regs`, `watch var`, `reset`, with console output shown inline.
- Later, a Debug Adapter Protocol (DAP) server over the same classes, so VS Code and Zed
  (both already set up in `.vscode/` and `.zed/`) get breakpoints, a call stack and
  variables in the editor. A GDB remote stub is not planned: GDB has no PIC18 target to
  drive it.

**Programmer guard:** add a `warn` entry for `DEBUG` = 0 to the `PIC18F??Q10` row of
`data/pic_config_bits.csv`, so `picprog write` warns through the existing
`config_warnings()` path. The data sheet says the bit "needs to be maintained as a '1'" for
normal operation (p.26).

## Testing

1. **On the PC:** the agent's protocol and command code are kept apart from its register
   access, as `src/proto/` is kept apart from `icsp.c`, and built with gcc against a fake
   memory and UART (`tests/test_pdbg.c` in `run_tests.sh`). pytest drives that build
   through pipes with `pdbg.py`, as `host/tests` drives `host_sim` today.
2. **In the MDB simulator:** the XC8 build of the agent on PIC18F27Q10: entry stub,
   context save, stack walk, resume. The simulator backtrace (shown working above) is the
   reference for `STACK`. Commands go in through an SCL `packetin()` script and replies
   come out in the UART output file (checked above). A Python helper writes the script
   with `pdbg.py`'s encoder and decodes the output file with its decoder, so the same
   framing code is tested end to end. The 0x55 auto-baud sync can be tested the same way,
   though the simulator's oscillator is exact.
3. **On silicon:** listed under the steps below.

## Steps

Each step ends with something that works and is tested.

| Step | Work | Done when |
| --- | --- | --- |
| D0 | `DEBUG` = 0 warning in the config table, plus a test (done: `test_q10_warns_when_debug_is_cleared`) | `picprog write` warns on such a hex |
| D1 | Program a Q10 on silicon: `p18_q10` is simulator-tested only (M5), so this comes first. Hookup for the Q10 in `docs/hookup.md` (ICSP: D3 to pin 27, D5 to pin 28, D4 to pin 1; UART: D1 (PC4, TX) to RC7 pin 18, D0 (PC5, RX) from RC6 pin 17) | `id`, `write`, `verify` pass on a real PIC18F27Q10, revision recorded |
| D2 | A plain `printf` program over `BRIDGE` at 115 200 baud, then the same with ABD on a 0x55 from the PC; the Nucleo `RESET` command | a long run counts framing errors and bad bytes with and without ABD (fact 4 says without it can fail); `RESET` restarts it |
| D3 | Agent core, protocol and `pdbg.py`, tested on the PC (done: the C agent core in `target/pdbg`, `tests/test_pdbg.c` and the `pdbg_sim` stdin/stdout build; to do: `picprog.debug.pdbg` and `host/tests/test_pdbg.py`) | `run_tests.sh` and pytest pass |
| D4 | Entry stub, context save, stack walk on XC8, in the MDB simulator | halt, `REGS`, `STACK`, `CONT` match the simulator's own view |
| D5 | The same on silicon, with ABD sync at `HELLO` and after reset: halt request, `PDBG_BREAK`, resume | a counting program halts, shows a correct backtrace, resumes without losing count or resetting |
| D6 | `picprog debug` with ELF symbols, console channel, live watch | debug session by file:line and variable name |
| D7 | WWDT handling while halted; ABD re-sync on framing errors | a halt longer than the WWDT period does not reset the part; the link recovers after the PIC is warmed or cooled |
| D8 | DAP server | breakpoints and variables in VS Code |
| D9 (optional) | Address breakpoints and single step by patching flash (a CALL into the agent in place of an instruction): costs an erase of the 128-word sector each time (Q10 row in `data/pic_devices.csv`), needs the flash endurance from the data sheet's electrical specs and errata 1.9.1 | decided after D8 |

Not covered: single step and breakpoints at arbitrary addresses before D9, freezing
peripherals while halted, and code that runs with interrupts disabled (it can only stop
at `PDBG_BREAK` sites).

## Open questions

- Which revision (A4 or B2) is on the bench; check after D1.
- Does the application need EUSART1 itself? If so, the agent moves to EUSART2 through PPS
  on non-ICSP pins.
- The VDD and temperature edges of the ±2 % band in Figure 39-5 (p.701), read off the
  drawing. If the bench sits outside that band, the budget in fact 4 gets worse, not better.

Settled (2026-10-22, details above): the MDB simulator does feed EUSART1 input (SCL
`packetin`); pyelftools reads XC8's DWARF; the baud budget fails at worst case without
auto-baud (fact 4), so ABD is in the agent from D5.
