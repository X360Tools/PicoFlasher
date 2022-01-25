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

#include <string.h>
#include "pico/stdlib.h"
#include "isd1200.h"
#include "nuvuton_spi.h"

#define CMD_PLAY_VP 0xA6
#define CMD_PLAY_VP_RN 0xAE
#define CMD_PLAY_VP_LP 0xA4
#define CMD_PLAY_VP_LP_RN 0xB2
#define CMD_STOP_LP 0x2E
#define CMD_EXE_VM 0xB0
#define CMD_EXE_VM_RN 0xBC
#define CMD_PLAY_SIL 0xA8
#define CMD_STOP 0x2A
#define CMD_SPI_PCM_READ 0xAC
#define CMD_SPI_SND_DEC 0xC0
#define CMD_READ_STATUS 0x40
#define CMD_READ_INT 0x46
#define CMD_READ_ID 0x48
#define CMD_DIG_READ 0xA2
#define CMD_DIG_WRITE 0xA0
#define CMD_ERASE_MEM 0x24
#define CMD_CHIP_ERASE 0x26
#define CMD_CHECKSUM 0xF2
#define CMD_PWR_UP 0x10
#define CMD_PWR_DN 0x12
#define CMD_SET_CLK_CFG 0xB4
#define CMD_RD_CLK_CFG 0xB6
#define CMD_WR_CFG_REG 0xB8
#define CMD_RD_CFG_REG 0xBA
#define CMD_RESET 0x14

uint8_t dev_id = 0;

bool isd1200_init()
{
	nuvoton_spi_init();

	isd1200_power_up();

	uint8_t buf[] = {CMD_READ_ID, 0x00, 0x00, 0x00, 0x00};

	nuvoton_spi_transfer(buf, sizeof(buf));

	if (buf[1] != 0x03) // PART_ID
		return false;

	if (buf[2] != 0xEF) // MAN_ID
		return false;

	if (buf[3] != 0x20) // MEM_TYPE
		return false;

	dev_id = buf[4]; // DEV_ID

	return true;
}

void isd1200_deinit()
{
	isd1200_power_down();

	nuvoton_spi_deinit();
}

void isd1200_power_up()
{
	uint8_t buf[] = {CMD_PWR_UP};

	nuvoton_spi_transfer(buf, sizeof(buf));

	while (!(isd1200_read_status() & STATUS_DBUF_RDY))
		;
	while (isd1200_read_status() & STATUS_VM_BSY)
		;
}

uint8_t isd1200_read_status()
{
	uint8_t buf[] = {CMD_READ_STATUS, 0x00};

	nuvoton_spi_transfer(buf, sizeof(buf));

	return buf[0]; // buf[1] is interrupt status
}

uint8_t isd1200_read_interrupt_status()
{
	uint8_t buf[] = {CMD_READ_INT, 0x00};

	nuvoton_spi_transfer(buf, sizeof(buf));

	return buf[1];
}

void isd1200_power_down()
{
	uint8_t buf[] = {CMD_PWR_DN};

	nuvoton_spi_transfer(buf, sizeof(buf));
}

void isd1200_reset()
{
	uint8_t buf[] = {CMD_RESET};

	nuvoton_spi_transfer(buf, sizeof(buf));
}

uint8_t isd1200_read_id()
{
	return dev_id;
}

void isd1200_play_vp(uint16_t index)
{
	uint8_t buf[] = {CMD_PLAY_VP, 0x00, 0x00};

	*(uint16_t *)&buf[1] = __builtin_bswap16(index);

	nuvoton_spi_transfer(buf, sizeof(buf));
}

void isd1200_exe_vm(uint16_t index)
{
	uint8_t buf[] = {CMD_EXE_VM, 0x00, 0x00};

	*(uint16_t *)&buf[1] = __builtin_bswap16(index);

	nuvoton_spi_transfer(buf, sizeof(buf));
}

void isd1200_flash_read(uint32_t page, uint8_t *buffer)
{
	uint8_t buf[1 + 4 + 512] = {CMD_DIG_READ, 0x00, 0x00, 0x00};

	uint32_t offset = page * 512;

	*(uint32_t *)&buf[1] = __builtin_bswap32(offset << 8);

	nuvoton_spi_transfer(buf, sizeof(buf));

	memcpy(buffer, &buf[5], 512);
}

void isd1200_chip_erase()
{
	uint8_t buf[] = {CMD_CHIP_ERASE, 0x01};

	nuvoton_spi_transfer(buf, sizeof(buf));

	while (isd1200_read_status() & STATUS_CMD_BSY)
		;
}

void isd1200_flash_write(uint32_t page, uint8_t *buffer)
{
	uint8_t buf[1 + 3 + 16] = {CMD_DIG_WRITE, 0x00, 0x00, 0x00};

	uint32_t offset = page * 16;

	*(uint32_t *)&buf[1] = __builtin_bswap32(offset << 8);

	memcpy(&buf[4], buffer, 16);

	nuvoton_spi_transfer(buf, sizeof(buf));

	while (isd1200_read_status() & STATUS_CMD_BSY)
		;

	while (!(isd1200_read_interrupt_status() & INTERRUPT_STATUS_WR_FIN))
		;
}