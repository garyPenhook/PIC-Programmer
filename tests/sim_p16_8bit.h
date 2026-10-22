#ifndef SIM_P16_8BIT_H
#define SIM_P16_8BIT_H

#include <stddef.h>
#include <stdint.h>

/*
 * A software model of a PIC16F171XX-style device on the ICSP pins, built from the
 * programming specification DS40002266B. It implements the icsp.h API so the p16_8bit
 * protocol can run on a PC.
 *
 * It decodes the bit stream as the spec describes (key, 8-bit commands, 24-clock
 * payloads, falling-edge latching), models the 32-word write latches, and checks that the
 * driver waits long enough after each command. Time is virtual: only icsp_delay_us()
 * advances it.
 *
 * Select the part to model by device ID with sim_select_device() before sim_reset(). Its
 * flash and EEPROM sizes are the model's own, from the spec, not from the device table.
 */

void sim_select_device(uint16_t device_id);
void sim_reset(void);
void sim_set_lvp(int enabled); /* 0 = device ignores the low-voltage key */
void sim_set_dat_stuck_high(
    int enabled); /* 1 = released ICSPDAT reads high, whatever the device sends */

int sim_in_program_mode(void);
uint32_t sim_violations(void);   /* waits that were too short, or bus contention */
uint32_t sim_bad_commands(void); /* command bytes the model does not know */
uint64_t sim_time_us(void);

uint16_t sim_flash_word(uint32_t index);
void sim_set_flash_word(uint32_t index, uint16_t value);
uint16_t sim_userid_word(uint32_t index);
uint16_t sim_config_word(uint32_t index);
uint8_t sim_eeprom_byte(uint32_t index);
/* The model's Device Information Area word (8100h + index, 32 words); fixed, as on silicon. */
uint16_t sim_dia_word(uint32_t index);

#endif
