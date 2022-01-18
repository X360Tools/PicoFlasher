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

#include "tusb.h"
#include "xbox.h"

#define LED_PIN 25

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
		tud_task();

	return 0;
}

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

#define GET_FLASH_CONFIG 0x01
#define READ_FLASH 0x02
#define WRITE_FLASH 0x03

#pragma pack(push, 1)
struct cmd
{
	uint8_t cmd;
	uint32_t lba;
};
#pragma pack(pop)

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
	}

	if (avilable_data >= needed_data)
	{
		struct cmd cmd;

		uint32_t count = tud_cdc_read(&cmd, sizeof(cmd));
		if (count != sizeof(cmd))
			return;

		if (cmd.cmd == GET_FLASH_CONFIG)
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
				tud_cdc_write(buffer, 0x210);
		}
		else if (cmd.cmd == WRITE_FLASH)
		{
			uint8_t buffer[0x210];
			uint32_t count = tud_cdc_read(&buffer, sizeof(buffer));
			if (count != sizeof(buffer))
				return;
			uint32_t ret = xbox_nand_write_block(cmd.lba, buffer, &buffer[0x200]);
			printf("Write ret: %x\n", ret);
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

