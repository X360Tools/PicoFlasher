/*
 * Copyright (c) 2022 Balázs Triszka <balika011@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __ISD1200_H__
#define __ISD1200_H__

enum ISD2100_DEV_ID
{
	ISD2110 = 0x01, // 44KB
	ISD2115 = 0x10, // 64KB
	ISD2130 = 0x11, // 1MB
};

enum ISD2100_VOICE_INDEX
{
	POWER = 5,
	EJECT = 6,
};

#define STATUS_CMD_BSY (1 << 0)
#define STATUS_CBUF_FUL (1 << 1)
#define STATUS_VM_BSY (1 << 2)
#define STATUS_INT (1 << 5)
#define STATUS_DBUF_RDY (1 << 6)
#define STATUS_PD (1 << 7)

#define INTERRUPT_STATUS_CMD_FIN (1 << 2)
#define INTERRUPT_STATUS_OVF_ERR (1 << 3)
#define INTERRUPT_STATUS_CMD_ERR (1 << 4)
#define INTERRUPT_STATUS_WR_FIN (1 << 5)
#define INTERRUPT_STATUS_MPT_ERR (1 << 6)

bool isd1200_init();
void isd1200_deinit();
uint8_t isd1200_read_status();
uint8_t isd1200_read_interrupt_status();
void isd1200_power_up();
void isd1200_power_down();
void isd1200_reset();
uint8_t isd1200_read_id();
void isd1200_play_vp(uint16_t index);
void isd1200_exe_vm(uint16_t index);
void isd1200_flash_read(uint32_t page, uint8_t *buffer);
void isd1200_chip_erase();
void isd1200_flash_write(uint32_t page, uint8_t *buffer);

void isd1200_test();

#endif