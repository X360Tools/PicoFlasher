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
#include "pio_spi.h"
#include "hardware/clocks.h"

pio_spi_inst_t spi = {
	.pio = pio0,
	.sm = 0,
};

void spiex_init()
{
	spi.prog = pio_add_program(spi.pio, &spi_cpha0_cs_program);
	// pio_spi_cs_init(spi.pio, spi.sm, spi.prog, 8, 3.0f, 0, 0, SPI_SS_N, SPI_MOSI, SPI_MISO, 0);

	float freq = clock_get_hz(clk_sys);
	freq /= 4000000.f;

	// freq /= 12.f;
	freq /= 18.f;

	pio_spi_cs_init(spi.pio, spi.sm, spi.prog, 8, freq, 0, 0, SPI_SS_N, SPI_MOSI, SPI_MISO, 0);
}

void spiex_deinit()
{
	pio_remove_program(spi.pio, &spi_cpha0_cs_program, spi.prog);
}

uint32_t spiex_read_reg(uint8_t reg)
{
	uint8_t txbuf[] = {(reg << 2) | 1, 0xFF, 0x00, 0x00, 0x00, 0x00};
	uint8_t rxbuf[sizeof(txbuf)];

	pio_spi_write8_read8_blocking(&spi, txbuf, rxbuf, sizeof(txbuf));

	return *(uint32_t *)&rxbuf[2];
}

void spiex_write_reg(uint8_t reg, uint32_t val)
{
	uint8_t txbuf[] = {(reg << 2) | 2, 0x00, 0x00, 0x00, 0x00};

	*(uint32_t *)&txbuf[1] = val;

	pio_spi_write8_blocking(&spi, txbuf, sizeof(txbuf));
}
