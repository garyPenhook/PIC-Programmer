#ifndef SIM_PIC16F171_H
#define SIM_PIC16F171_H

#include <stddef.h>
#include <stdint.h>

/*
 * A software model of a PIC16F171XX device on the ICSP pins, built from the programming
 * specification DS40002266B. It implements the icsp.h API so pic16f171.c can run on a PC.
 *
 * It decodes the bit stream as the spec describes (key, 8-bit commands, 24-clock
 * payloads, falling-edge latching), models the 32-word write latches, and checks that the
 * driver waits long enough after each command. Time is virtual: only icsp_delay_us()
 * advances it.
 *
 * Select which family member to model with sim_select_device() before sim_reset():
 *   0x30DC = PIC16F17124 (4 KW flash, 128 byte EEPROM)
 *   0x30E1 = PIC16F17146 (16 KW flash, 256 byte EEPROM)
 */

void sim_select_device(uint16_t device_id);
void sim_reset(void);
void sim_set_lvp(int enabled);              /* 0 = device ignores the low-voltage key */

int sim_in_program_mode(void);
uint32_t sim_violations(void);              /* waits that were too short, or bus contention */
uint32_t sim_bad_commands(void);            /* command bytes the model does not know */
uint64_t sim_time_us(void);

uint16_t sim_flash_word(uint32_t index);
void sim_set_flash_word(uint32_t index, uint16_t value);
uint16_t sim_userid_word(uint32_t index);
uint16_t sim_config_word(uint32_t index);
uint8_t sim_eeprom_byte(uint32_t index);

#endif
