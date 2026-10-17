#ifndef SIM_P18_Q10_H
#define SIM_P18_Q10_H

#include <stddef.h>
#include <stdint.h>

/*
 * A software model of a PIC18F2X/4XQ10 device on the ICSP pins. It implements the icsp.h API, so
 * the p18_q10 protocol can be built and run on a PC against it.
 *
 * It decodes the bit stream the way the programming spec describes (key, 8-bit commands,
 * 24-clock payloads, falling-edge latching) and checks that the driver waits long enough
 * after each command. Time is virtual: only icsp_delay_us() advances it.
 *
 * Select the part to model by device ID with sim_select_device() before sim_reset(). Its
 * memory sizes are the model's own, from DS40001874H and DS40002043, not from the device table.
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

/* Direct access to the model's memory, for setting up and checking tests. */
uint16_t sim_flash_word(uint32_t word_index);
void sim_set_flash_word(uint32_t word_index, uint16_t value);
uint16_t sim_userid_word(uint32_t index);
uint8_t sim_eeprom_byte(uint32_t index);
void sim_set_eeprom_byte(uint32_t index, uint8_t value);
uint16_t sim_config_word(uint32_t index);
uint32_t sim_bulk_erases(void); /* Bulk Erase commands seen */
void sim_set_revision_id(uint16_t value); /* default 0xA000: revision A0 (Register 2-2) */
void sim_set_code_protect(int enabled); /* 1 = CP and CPD set: Bulk Erase also takes the EEPROM */

#endif
