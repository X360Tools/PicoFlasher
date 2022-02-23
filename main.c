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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "bsp/board.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"
#include "pico/stdlib.h"
#include "pico/bootrom.h"

#include "tusb.h"
#include "xbox.h"
#include "isd1200.h"
#include "sdio.h"
#include "pins.h"
#include "mmc_defs.h"

#define LED_PIN 25

// Invoked when device is mounted
void tud_mount_cb(void)
{
	xbox_stop_smc();

	uint32_t flash_config = xbox_get_flash_config();

	printf("flash_config: %x\n", flash_config);
}

// Invoked when device is unmounted
void tud_umount_cb(void)
{
	xbox_start_smc();

	printf("Bye!\n");
}

// Invoked when usb bus is suspended
// remote_wakeup_en : if host allow us  to perform remote wakeup
// Within 7ms, device must draw an average of current less than 2.5 mA from bus
void tud_suspend_cb(bool remote_wakeup_en)
{
	(void)remote_wakeup_en;

	xbox_start_smc();

	printf("Bye!\n");
}

// Invoked when usb bus is resumed
void tud_resume_cb(void)
{
	xbox_stop_smc();

	uint32_t flash_config = xbox_get_flash_config();

	printf("flash_config: %x\n", flash_config);
}

void led_blink(void)
{
	static uint32_t start_ms = 0;
	static bool led_state = false;

	uint32_t now = board_millis();

	if (now - start_ms < 50)
		return;

	start_ms = now;

	gpio_put(LED_PIN, led_state);
	led_state = 1 - led_state;
}

#define GET_VERSION 0x00
#define GET_FLASH_CONFIG 0x01
#define READ_FLASH 0x02
#define WRITE_FLASH 0x03
#define READ_FLASH_STREAM 0x04

#define EMMC_DETECT 0x50
#define EMMC_INIT 0x51
#define EMMC_GET_CID 0x52
#define EMMC_GET_CSD 0x53
#define EMMC_GET_EXT_CSD 0x54
#define EMMC_READ 0x55
#define EMMC_READ_STREAM 0x56
#define EMMC_WRITE 0x57

#define ISD1200_INIT 0xA0
#define ISD1200_DEINIT 0xA1
#define ISD1200_READ_ID 0xA2
#define ISD1200_READ_FLASH 0xA3
#define ISD1200_ERASE_FLASH 0xA4
#define ISD1200_WRITE_FLASH 0xA5
#define ISD1200_PLAY_VOICE 0xA6
#define ISD1200_EXEC_MACRO 0xA7
#define ISD1200_RESET 0xA8

#define REBOOT_TO_BOOTLOADER 0xFE

#pragma pack(push, 1)
struct cmd
{
	uint8_t cmd;
	uint32_t lba;
};
#pragma pack(pop)

bool emmc_detected = false;
bool stream_emmc = false;
bool do_stream = false;
uint32_t stream_offset = 0;
uint32_t stream_end = 0;
void stream()
{
	if (do_stream)
	{
		if (stream_offset >= stream_end)
		{
			do_stream = false;
			return;
		}

		if (tud_cdc_write_available() < 4 + (stream_emmc ? 0x200 : 0x210))
			return;

		if (!stream_emmc)
		{
			static uint8_t buffer[4 + 0x210];
			uint32_t ret = xbox_nand_read_block(stream_offset, &buffer[4], &buffer[4 + 0x200]);
			*(uint32_t *)buffer = ret;
			if (ret == 0)
			{
				tud_cdc_write(buffer, sizeof(buffer));
				++stream_offset;
			}
			else
			{
				tud_cdc_write(&ret, 4);
				do_stream = false;
			}
		}
		else
		{
			static uint8_t buffer[4 + 0x200];
			int ret = sd_readblocks_sync(&buffer[4], stream_offset, 1);
			*(uint32_t *)buffer = ret;
			if (ret == 0)
			{
				tud_cdc_write(buffer, sizeof(buffer));
				++stream_offset;
			}
			else
			{
				tud_cdc_write(&ret, 4);
				do_stream = false;
			}
		}
	}
}

// Invoked when CDC interface received data from host
void tud_cdc_rx_cb(uint8_t itf)
{
	(void)itf;
	led_blink();

	uint32_t avilable_data = tud_cdc_available();

	uint32_t needed_data = sizeof(struct cmd);
	{
		uint8_t cmd;
		tud_cdc_peek(&cmd);
		if (cmd == WRITE_FLASH)
			needed_data += 0x210;
		if (cmd == ISD1200_WRITE_FLASH)
			needed_data += 16;
	}

	if (avilable_data >= needed_data)
	{
		struct cmd cmd;

		uint32_t count = tud_cdc_read(&cmd, sizeof(cmd));
		if (count != sizeof(cmd))
			return;

		if (cmd.cmd == GET_VERSION)
		{
			uint32_t ver = 3;
			tud_cdc_write(&ver, 4);
		}
		else if (cmd.cmd == GET_FLASH_CONFIG)
		{
			uint32_t fc = xbox_get_flash_config();
			tud_cdc_write(&fc, 4);
		}
		else if (cmd.cmd == READ_FLASH)
		{
			uint8_t buffer[0x210];
			uint32_t ret = xbox_nand_read_block(cmd.lba, buffer, &buffer[0x200]);
			tud_cdc_write(&ret, 4);
			if (ret == 0)
				tud_cdc_write(buffer, sizeof(buffer));
		}
		else if (cmd.cmd == WRITE_FLASH)
		{
			uint8_t buffer[0x210];
			uint32_t count = tud_cdc_read(&buffer, sizeof(buffer));
			if (count != sizeof(buffer))
				return;
			uint32_t ret = xbox_nand_write_block(cmd.lba, buffer, &buffer[0x200]);
			tud_cdc_write(&ret, 4);
		}
		else if (cmd.cmd == READ_FLASH_STREAM)
		{
			stream_emmc = false;
			do_stream = true;
			stream_offset = 0;
			stream_end = cmd.lba;
		}
		if (cmd.cmd == ISD1200_INIT)
		{
			uint8_t ret = isd1200_init() ? 0 : 1;
			tud_cdc_write(&ret, 1);
		}
		if (cmd.cmd == ISD1200_DEINIT)
		{
			isd1200_deinit();
			uint8_t ret = 0;
			tud_cdc_write(&ret, 1);
		}
		else if (cmd.cmd == ISD1200_READ_ID)
		{
			uint8_t dev_id = isd1200_read_id();
			tud_cdc_write(&dev_id, 1);
		}
		else if (cmd.cmd == ISD1200_READ_FLASH)
		{
			uint8_t buffer[512];
			isd1200_flash_read(cmd.lba, buffer);
			tud_cdc_write(buffer, sizeof(buffer));
		}
		if (cmd.cmd == ISD1200_ERASE_FLASH)
		{
			isd1200_chip_erase();
			uint8_t ret = 0;
			tud_cdc_write(&ret, 1);
		}
		else if (cmd.cmd == ISD1200_WRITE_FLASH)
		{
			uint8_t buffer[16];
			uint32_t count = tud_cdc_read(&buffer, sizeof(buffer));
			if (count != sizeof(buffer))
				return;
			isd1200_flash_write(cmd.lba, buffer);
			uint32_t ret = 0;
			tud_cdc_write(&ret, 4);
		}
		else if (cmd.cmd == ISD1200_PLAY_VOICE)
		{
			isd1200_play_vp(cmd.lba);
			uint8_t ret = 0;
			tud_cdc_write(&ret, 1);
		}
		else if (cmd.cmd == ISD1200_EXEC_MACRO)
		{
			isd1200_exe_vm(cmd.lba);
			uint8_t ret = 0;
			tud_cdc_write(&ret, 1);
		}
		if (cmd.cmd == ISD1200_RESET)
		{
			isd1200_reset();
			uint8_t ret = 0;
			tud_cdc_write(&ret, 1);
		}
		else if (cmd.cmd == REBOOT_TO_BOOTLOADER)
		{
			reset_usb_boot(0, 0);
		}
		else if (cmd.cmd == EMMC_DETECT)
		{
			if (!emmc_detected)
			{
				gpio_init(MMC_CLK_PIN);
				gpio_set_dir(MMC_CLK_PIN, GPIO_IN);
				emmc_detected = gpio_get(MMC_CLK_PIN);
			}
			tud_cdc_write(&emmc_detected, 1);
		}
		else if (cmd.cmd == EMMC_INIT)
		{
			// Put SMC into reset
			gpio_init(SMC_RST_XDK_N);
			gpio_set_dir(SMC_RST_XDK_N, GPIO_OUT);
			gpio_put(SMC_RST_XDK_N, 0);

			uint32_t ret = sd_init();
			tud_cdc_write(&ret, 4);
		}
		else if (cmd.cmd == EMMC_GET_CID)
		{
			uint8_t cid_raw[16];
			sd_read_cid(cid_raw);
			tud_cdc_write(cid_raw, sizeof(cid_raw));
		}
		else if (cmd.cmd == EMMC_GET_CSD)
		{
			uint8_t csd_raw[16];
			sd_read_csd(csd_raw);
			tud_cdc_write(csd_raw, sizeof(csd_raw));
		}
		else if (cmd.cmd == EMMC_GET_EXT_CSD)
		{
			uint8_t ext_csd[512];
			sd_read_ext_csd(ext_csd);
			tud_cdc_write(ext_csd, sizeof(ext_csd));
		}
		else if (cmd.cmd == EMMC_READ)
		{
			uint8_t buffer[0x200];
			int ret = sd_readblocks_sync(buffer, cmd.lba, 1);
			tud_cdc_write(&ret, 4);
			if (ret == 0)
				tud_cdc_write(buffer, sizeof(buffer));
		}
		else if (cmd.cmd == EMMC_READ_STREAM)
		{
			stream_emmc = true;
			do_stream = true;
			stream_offset = 0;
			stream_end = cmd.lba;
		}
		else if (cmd.cmd == EMMC_WRITE)
		{
			uint8_t buffer[0x200];
			uint32_t count = tud_cdc_read(&buffer, sizeof(buffer));
			if (count != sizeof(buffer))
				return;
			uint32_t ret = sd_writeblocks_sync(buffer, cmd.lba, 1);
			tud_cdc_write(&ret, 4);
		}

		tud_cdc_write_flush();
	}
}

void tud_cdc_tx_complete_cb(uint8_t itf)
{
	(void)itf;
	led_blink();
}

int main(void)
{
	vreg_set_voltage(VREG_VOLTAGE_1_30);
	set_sys_clock_khz(266000, true);

	uint32_t freq = clock_get_hz(clk_sys);
	clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS, freq, freq);

	stdio_init_all();

	gpio_init(LED_PIN);
	gpio_set_dir(LED_PIN, GPIO_OUT);

	xbox_init();

	tusb_init();

	while (1)
	{
		tud_task();
		stream();
	}

	return 0;
}