#ifndef SIM_P18_8BIT_H
#define SIM_P18_8BIT_H

#include <stddef.h>
#include <stdint.h>

/*
 * A software model of a PIC18-Q device on the ICSP pins. It implements the icsp.h API, so
 * the p18_8bit protocol can be built and run on a PC against it.
 *
 * It decodes the bit stream the way the programming spec describes (key, 8-bit commands,
 * 24-clock payloads, falling-edge latching) and checks that the driver waits long enough
 * after each command. Time is virtual: only icsp_delay_us() advances it.
 *
 * Select the Q41 part to model by device ID with sim_select_device() before sim_reset(). Its
 * memory sizes are the model's own, from the data sheet, not from the device table.
 */

void sim_select_device(uint16_t device_id);
void sim_reset(void);
void sim_set_lvp(int enabled);              /* 0 = device ignores the low-voltage key */
void sim_set_dat_stuck_high(int enabled);   /* 1 = released ICSPDAT reads high, whatever the device sends */

int sim_in_program_mode(void);
uint32_t sim_violations(void);              /* waits that were too short, or bus contention */
uint32_t sim_bad_commands(void);            /* command bytes the model does not know */
uint64_t sim_time_us(void);

/* Direct access to the model's memory, for setting up and checking tests. */
uint16_t sim_flash_word(uint32_t word_index);
void sim_set_flash_word(uint32_t word_index, uint16_t value);
uint16_t sim_userid_word(uint32_t index);
uint8_t sim_eeprom_byte(uint32_t index);
void sim_set_eeprom_byte(uint32_t index, uint8_t value);
uint8_t sim_config_byte(uint32_t index);

#endif
