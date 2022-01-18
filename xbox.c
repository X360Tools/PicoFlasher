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

#include "pico/stdlib.h"
#include "pins.h"
#include "spiex.h"
#include "pio_spi.h"

void xbox_init()
{
	gpio_init(SMC_DBG_EN);
	gpio_put(SMC_DBG_EN, 1);
	gpio_set_dir(SMC_DBG_EN, GPIO_OUT);

	gpio_init(SMC_RST_XDK_N);
	gpio_put(SMC_RST_XDK_N, 1);
	gpio_set_dir(SMC_RST_XDK_N, GPIO_OUT);

	gpio_init(SPI_SS_N);
	gpio_put(SPI_SS_N, 1);
	gpio_set_dir(SPI_SS_N, GPIO_OUT);
}

void xbox_start_smc()
{
	spiex_deinit();

	gpio_put(SMC_DBG_EN, 0);
	gpio_put(SMC_RST_XDK_N, 0);

	sleep_ms(50);

	gpio_put(SMC_RST_XDK_N, 1);
}

void xbox_stop_smc()
{
	gpio_put(SMC_DBG_EN, 0);

	sleep_ms(50);

	gpio_put(SPI_SS_N, 0);
	gpio_put(SMC_RST_XDK_N, 0);

	sleep_ms(50);

	gpio_put(SMC_DBG_EN, 1);
	gpio_put(SMC_RST_XDK_N, 1);

	sleep_ms(50);

	gpio_put(SPI_SS_N, 1);

	sleep_ms(50);

	spiex_init();
}

uint32_t xbox_get_flash_config()
{
	static uint32_t flash_config = 0;
	if (!flash_config)
		flash_config = spiex_read_reg(0);

	return flash_config;
}

uint16_t xbox_nand_get_status()
{
	return spiex_read_reg(0x04);
}

void xbox_nand_clear_status()
{
	spiex_write_reg(0x04, spiex_read_reg(0x04));
}

int xbox_nand_wait_ready(uint16_t timeout)
{
	do
	{
		if (!(xbox_nand_get_status() & 0x01))
			return 0;
	} while (timeout--);

	return 1;
}

int xbox_nand_read_block(uint32_t lba, uint8_t *buffer, uint8_t *spare)
{
	xbox_nand_clear_status();

	spiex_write_reg(0x0C, lba << 9);

	spiex_write_reg(0x08, 0x03);

	if (xbox_nand_wait_ready(0x1000))
		return 0x8000 | xbox_nand_get_status();

	spiex_write_reg(0x0C, 0);

	uint8_t *end = buffer + 0x200;
	while (buffer < end)
	{
		spiex_write_reg(0x08, 0x00);

		*(uint32_t *) buffer = spiex_read_reg(0x10);
		buffer += 4;
	}

	end = spare + 0x10;
	while (spare < end)
	{
		spiex_write_reg(0x08, 0x00);

		*(uint32_t *)spare = spiex_read_reg(0x10);
		spare += 4;
	}

	return 0;
}

int xbox_nand_erase_block(uint32_t lba)
{
	xbox_nand_clear_status();

	spiex_write_reg(0x00, spiex_read_reg(0x00) | 0x08);

	spiex_write_reg(0x0C, lba << 9);

	spiex_write_reg(0x08, 0xAA);
	spiex_write_reg(0x08, 0x55);
	spiex_write_reg(0x08, 0x05);

	if (xbox_nand_wait_ready(0x1000))
		return 0x8000 | xbox_nand_get_status();

	return 0;
}

int xbox_nand_write_block(uint32_t lba, uint8_t *buffer, uint8_t *spare)
{
	// erase ereases 0x4000 bytes
	if (lba % 0x20 == 0)
	{
		int ret = xbox_nand_erase_block(lba);
		if (ret)
			return ret;
	}

	xbox_nand_clear_status();

	spiex_write_reg(0x0C, 0);

	uint8_t *end = buffer + 0x200;
	while (buffer < end)
	{
		spiex_write_reg(0x10, *(uint32_t *)buffer);

		spiex_write_reg(0x08, 0x01);

		buffer += 4;
	}

	end = spare + 0x10;
	while (spare < end)
	{
		spiex_write_reg(0x10, *(uint32_t *)spare);

		spiex_write_reg(0x08, 0x01);

		spare += 4;
	}

	if (xbox_nand_wait_ready(0x1000))
		return 0x8000 | xbox_nand_get_status();

	spiex_write_reg(0x0C, lba << 9);

	if (xbox_nand_wait_ready(0x1000))
		return 0x8000 | xbox_nand_get_status();

	spiex_write_reg(0x08, 0x55);
	spiex_write_reg(0x08, 0xAA);
	spiex_write_reg(0x08, 0x04);

	if (xbox_nand_wait_ready(0x1000))
		return 0x8000 | xbox_nand_get_status();

	return 0;
}
