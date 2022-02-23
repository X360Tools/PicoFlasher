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

#ifndef _SDIO_H
#define _SDIO_H

#ifdef __cplusplus
extern "C" {
#endif

#include "pico.h"

#define SD_OK (0)
#define SD_ERR_STUCK (-1)
#define SD_ERR_BAD_RESPONSE (-2)
#define SD_ERR_CRC (-3)
#define SD_ERR_BAD_PARAM (-4)

#define SDIO_MAX_BLOCK_COUNT 32
#define SD_SECTOR_SIZE 512

// todo buffer pool
int sd_init();
int sd_readblocks_sync(void *buf, uint32_t block, uint block_count);
int sd_readblocks_async(void *buf, uint32_t block, uint block_count);
bool sd_scatter_read_complete(int *status);
int sd_writeblocks_async(const void *data, uint32_t sector_num, uint sector_count);
int sd_writeblocks_sync(const void *data, uint32_t sector_num, uint sector_count);
bool sd_write_complete(int *status);
void sd_read_cid(void *cid);
void sd_read_csd(void *csd);
int sd_read_ext_csd(void *ext_csd);

#endif

#ifdef __cplusplus
}
#endif