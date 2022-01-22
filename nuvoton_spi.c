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

pio_spi_inst_t nuvoton_spi;

void nuvoton_spi_init()
{
	pio_spi_init(&nuvoton_spi, pio1, 0, 1.f, 8, SPI_MSB_FIRST, true, true, NUVOTON_SPI_SS_N, NUVOTON_SPI_MOSI, NUVOTON_SPI_MISO);
}

void nuvoton_spi_deinit()
{
	pio_remove_program(nuvoton_spi.pio, &spi_cpha1_cs_program, nuvoton_spi.prog);
}

void nuvoton_spi_transfer(uint8_t *buffer, uint32_t length)
{
	pio_spi_write8_read8_blocking(&nuvoton_spi, buffer, buffer, length);
}