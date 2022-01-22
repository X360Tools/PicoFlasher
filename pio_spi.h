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

#ifndef _PIO_SPI_H
#define _PIO_SPI_H

#include "hardware/pio.h"
#include "spi.pio.h"

typedef struct pio_spi_inst
{
	PIO pio;
	uint sm;
	uint prog;
	uint order;
} pio_spi_inst_t;

typedef enum pio_spi_order
{
	SPI_LSB_FIRST = 0,
	SPI_MSB_FIRST
} pio_spi_order_t;

void pio_spi_init(pio_spi_inst_t *spi, PIO pio, uint sm, float freq, uint n_bits, pio_spi_order_t order, bool cpha, bool cpol, uint pin_ss, uint pin_mosi, uint pin_miso);

void pio_spi_write8_blocking(const pio_spi_inst_t *spi, const uint8_t *src, size_t len);

void pio_spi_read8_blocking(const pio_spi_inst_t *spi, uint8_t *dst, size_t len);

void pio_spi_write8_read8_blocking(const pio_spi_inst_t *spi, uint8_t *src, uint8_t *dst, size_t len);

#endif
