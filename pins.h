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

#ifndef __PINS_H__
#define __PINS_H__

#define SPI_MISO 16
#define SPI_SS_N 17
#define SPI_CLK 18
#define SPI_MOSI 19
#define SMC_DBG_EN 20
#define SMC_RST_XDK_N 21

#define NUVOTON_SPI_RDY 11 // FT2V4
#define NUVOTON_SPI_MISO 12 // FT2R7
#define NUVOTON_SPI_SS_N 13 // FT2R6
#define NUVOTON_SPI_CLK 14 // FT2T4
#define NUVOTON_SPI_MOSI 15 // FT2T5

#define MMC_RST_PIN 9
#define MMC_CLK_PIN 8
#define MMC_CMD_PIN 7
#define MMC_DAT0_PIN 6

#endif
