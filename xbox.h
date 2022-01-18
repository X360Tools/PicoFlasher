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

#ifndef __XBOX_H__
#define __XBOX_H__

void xbox_init();

void xbox_start_smc();
void xbox_stop_smc();

uint32_t xbox_get_flash_config();
int xbox_nand_read_block(uint32_t lba, uint8_t *buffer, uint8_t *spare);
int xbox_nand_erase_block(uint32_t lba);
int xbox_nand_write_block(uint32_t lba, uint8_t *buffer, uint8_t *spare);

#endif
