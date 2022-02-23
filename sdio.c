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

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "sdio.h"
#include "hardware/pio.h"
#include "hardware/gpio.h"
#include "hardware/dma.h"
#include "sdio.pio.h"
#include "crc7.h"
#include "crc-itu-t.h"
#include "pico/binary_info.h"
#include "pins.h"
#include "mmc_defs.h"
#include "sd.h"

#define SD_CLK_SM 0u
#define SD_CMD_SM 1u
#define SD_DAT_SM 2u
const int sd_cmd_dma_channel = 11;
const int sd_data_dma_channel = 10;
const int sd_chain_dma_channel = 9;
const int sd_pio_dma_channel = 8;

#if 0
#define sd_debug(format, args...) printf(format, ##args)
#else
#define sd_debug(format, args...) (void)0
#endif

static inline uint32_t sd_pio_cmd(uint cmd, uint32_t param)
{
	assert(cmd <= sd_cmd_or_dat_program.length);
	assert(param <= 0xffff);
	return (pio_encode_jmp(cmd) << 16u) | param;
}

PIO sd_pio = pio1;

static uint8_t rca_high, rca_low;
uint8_t cid_raw[16];
uint8_t csd_raw[16];

static uint32_t crcs[SDIO_MAX_BLOCK_COUNT * 2];
static uint32_t ctrl_words[(SDIO_MAX_BLOCK_COUNT + 1) * 4];
static uint32_t pio_cmd_buf[SDIO_MAX_BLOCK_COUNT * 3];
uint32_t zeroes;
uint32_t start_bit = 0xfffffffe;

inline static int safe_wait_tx_empty(pio_hw_t *pio, uint sm)
{
	int wooble = 0;
	while (!pio_sm_is_tx_fifo_empty(pio, sm))
	{
		wooble++;
		if (wooble > 1000000)
		{
			printf("stuck %d @ %d\n", sm, (int)pio->sm[sm].addr);
			__breakpoint();
			return SD_ERR_STUCK;
		}
	}
	return SD_OK;
}

inline static int safe_wait_tx_not_full(pio_hw_t *pio, uint sm)
{
	int wooble = 0;
	while (pio_sm_is_tx_fifo_full(pio, sm))
	{
		wooble++;
		if (wooble > 1000000)
		{
			printf("stuck %d @ %d\n", sm, (int)pio->sm[sm].addr);
			__breakpoint();
			return SD_ERR_STUCK;
		}
	}
	return SD_OK;
}

inline static int safe_dma_wait_for_finish(pio_hw_t *pio, uint sm, uint chan)
{
	int wooble = 0;
	while (dma_channel_is_busy(chan))
	{
		wooble++;
		if (wooble > 8000000)
		{
			printf("stuck dma channel %d rem %08x %d @ %d\n", chan, (uint)dma_hw->ch[chan].transfer_count, sm, (int)pio->sm[sm].addr);
			__breakpoint();
			return SD_ERR_STUCK;
		}
	}
	return SD_OK;
}

static inline int acquiesce_sm(int sm)
{
	int rc = safe_wait_tx_empty(sd_pio, sm);
	if (rc)
		return rc;

	uint32_t foo = 0;
	uint32_t timeout = 1000000;
	while (--timeout)
	{
		uint32_t addr = sd_pio->sm[sm].addr;
		foo |= 1 << addr;
		if (addr == sd_cmd_or_dat_offset_no_arg_state_waiting_for_cmd)
		{
			break;
		}
		// todo not forever
	}
	if (!timeout)
		return SD_ERR_STUCK;

	return SD_OK;
}

static int __time_critical_func(start_single_dma)(uint dma_channel, uint sm, uint32_t *buf, uint byte_length, bool bswap, bool sniff)
{
	gpio_set_mask(1);
	uint word_length = (byte_length + 3) / 4;
	dma_channel_config c = dma_channel_get_default_config(dma_channel);
	channel_config_set_bswap(&c, bswap);
	channel_config_set_read_increment(&c, false);
	channel_config_set_write_increment(&c, true);
	channel_config_set_dreq(&c, DREQ_PIO1_RX0 + sm);
	dma_channel_configure(
		dma_channel,
		&c,
		buf,			  // dest
		&sd_pio->rxf[sm], // src
		word_length,
		false);
	if (sniff)
	{
		assert(sm == SD_DAT_SM);
		dma_sniffer_enable(dma_channel, DMA_SNIFF_CTRL_CALC_VALUE_CRC16, true);
		dma_hw->sniff_data = 0;
	}
	dma_channel_start(dma_channel);
	gpio_clr_mask(1);
	return SD_OK;
}

static void __time_critical_func(start_chain_dma_read_with_address_size_only)(uint sm, uint32_t *buf, bool bswap, bool sniff)
{
	assert(!sniff); // for now
	dma_channel_config c = dma_channel_get_default_config(sd_data_dma_channel);
	channel_config_set_bswap(&c, bswap);
	channel_config_set_read_increment(&c, false);
	channel_config_set_write_increment(&c, true);
	channel_config_set_dreq(&c, DREQ_PIO1_RX0 + sm);
	channel_config_set_chain_to(&c, sd_chain_dma_channel); // individual buffers chain back to master
	channel_config_set_irq_quiet(&c, true);

	dma_channel_configure(
		sd_data_dma_channel,
		&c,
		0,				  // dest
		&sd_pio->rxf[sm], // src
		0,
		false);

	c = dma_channel_get_default_config(sd_chain_dma_channel);
	channel_config_set_read_increment(&c, true);
	channel_config_set_write_increment(&c, true);
	channel_config_set_ring(&c, 1, 3); // wrap the write at 8 bytes (so each transfer writes the same 2 word ctrl registers)
	dma_channel_configure(
		sd_chain_dma_channel,
		&c,
		&dma_channel_hw_addr(sd_data_dma_channel)->al1_write_addr, // dest
		buf,													   // src
		2,														   // send 2 words to ctrl block of data chain per transfer
		false);

	gpio_set_mask(1);
	//    if (sniff)
	//    {
	//        dma_enable_sniffer(sd_data_dma_channel, DMA_SNIFF_CTRL_CALC_VALUE_CRC16);
	//        dma_hw->sniff_data = 0;
	//    }
	dma_channel_start(sd_chain_dma_channel);
	gpio_clr_mask(1);
}

static __attribute__((used)) __noinline void spoop()
{
	int dma_channel = 3;
	dma_channel_config config = dma_channel_get_default_config(dma_channel);
	channel_config_set_read_increment(&config, true);
	channel_config_set_write_increment(&config, true);
	channel_config_set_dreq(&config, DREQ_SPI0_RX);
	channel_config_set_transfer_data_size(&config, DMA_SIZE_8);
	dma_channel_set_config(dma_channel, &config, false);

	*(volatile uint32_t *)(DMA_BASE + DMA_CH3_AL1_CTRL_OFFSET) = 0x00089831;
}

static int __time_critical_func(start_read)(int sm, uint32_t *buf, uint byte_length, bool enable)
{
	spoop();
	int rc;
	gpio_set_mask(1);
	assert(!(3u & (uintptr_t)buf)); // in all fairness we should receive into a buffer from the pool
	uint bit_length = byte_length * 8;
	if (sm == SD_DAT_SM)
	{
		assert(!(bit_length & 31u));
		bit_length += 16;
	}
	rc = safe_wait_tx_not_full(sd_pio, sm);
	if (rc)
		return rc;

	pio_sm_put(sd_pio, sm, sd_pio_cmd(sd_cmd_or_dat_offset_state_receive_bits, bit_length - 1));
	pio_sm_set_wrap(sd_pio, sm, sd_cmd_or_dat_wrap_target, sd_cmd_or_dat_wrap);
	gpio_clr_mask(1);
	gpio_set_mask(1);
	if (enable)
		pio_sm_set_enabled(sd_pio, sm, true);
	if (bit_length & 31u)
	{
		rc = safe_wait_tx_not_full(sd_pio, sm);
		if (rc)
			return rc;
		pio_sm_put(sd_pio, sm, sd_pio_cmd(sd_cmd_or_dat_offset_state_inline_instruction, pio_encode_in(pio_null, 32 - (bit_length & 31u))));
	}
	// now go back to wait state
	rc = safe_wait_tx_not_full(sd_pio, sm);
	if (rc)
		return rc;
	pio_sm_put(sd_pio, sm, sd_pio_cmd(sd_cmd_or_dat_offset_state_inline_instruction, pio_encode_jmp(sm == SD_DAT_SM ? sd_cmd_or_dat_offset_no_arg_state_waiting_for_cmd : sd_cmd_or_dat_offset_no_arg_state_wait_high)));
	gpio_clr_mask(1);
	return SD_OK;
}

static int __time_critical_func(finish_read)(uint dma_channel, int sm, uint16_t *suffixed_crc, uint16_t *sniffed_crc)
{
	gpio_set_mask(1);
	int rc = safe_dma_wait_for_finish(sd_pio, sm, dma_channel);
	if (rc)
		return rc;
	if (sniffed_crc)
	{
		*sniffed_crc = (uint16_t)dma_hw->sniff_data;
	}
	if (sm == SD_DAT_SM)
	{
		// todo not forever
		while (pio_sm_is_rx_fifo_empty(sd_pio, SD_DAT_SM))
			;
		uint32_t w = sd_pio->rxf[SD_DAT_SM];
		if (suffixed_crc)
			*suffixed_crc = w >> 16u;
	}
	assert(pio_sm_is_rx_fifo_empty(sd_pio, sm));
	gpio_clr_mask(1);
	return SD_OK;
}

int __time_critical_func(sd_response_dma)(uint dma_channel, uint sm, uint8_t *buf, uint byte_length, bool bswap, uint16_t *suffixed_crc, uint16_t *sniffed_crc, bool first, bool last, bool enable)
{
	int rc = SD_OK;
	if (first)
	{
		rc = start_single_dma(dma_channel, sm, (uint32_t *)buf, byte_length, bswap, sniffed_crc != 0);
		if (!rc)
			rc = start_read(sm, (uint32_t *)buf, byte_length, enable);
	}

	if (!rc)
		rc = finish_read(dma_channel, sm, suffixed_crc, sniffed_crc);

	if (!last && !rc)
	{
		rc = start_single_dma(dma_channel, sm, (uint32_t *)buf, byte_length, bswap, sniffed_crc != 0);
		if (!rc)
			rc = start_read(sm, (uint32_t *)buf, byte_length, enable);
	}
	return rc;
}

int __noinline sd_command(uint8_t cmd, uint32_t arg, void *response)
{
	int rc = acquiesce_sm(SD_CMD_SM);
	if (rc)
		return rc;

	sd_debug("SD command %d ARG: %x\n", cmd, arg);

	uint8_t b0 = arg >> 24;
	uint8_t b1 = arg >> 16;
	uint8_t b2 = arg >> 8;
	uint8_t b3 = arg;

	cmd |= 0x40;
	uint8_t crc = 0;
	crc = crc7_table[crc ^ cmd];
	crc = crc7_table[crc ^ b0];
	crc = crc7_table[crc ^ b1];
	crc = crc7_table[crc ^ b2];
	crc = crc7_table[crc ^ b3];
	crc |= 1;

	uint32_t high = (cmd << 24) | (b0 << 16) | (b1 << 8) | b2;
	uint32_t low = (b3 << 24) | (crc << 16);

	uint64_t packed_command = (((uint64_t)high) << 32) | low;

	cmd &= 0x3f;

	// disable SM so we don't have a race on us filling the FIFO - we must not stall or we will lose sync with clock
	pio_sm_set_enabled(sd_pio, SD_CMD_SM, false);
	pio_sm_put(sd_pio, SD_CMD_SM, sd_pio_cmd(sd_cmd_or_dat_offset_state_send_bits, 48 - 1));
	pio_sm_put(sd_pio, SD_CMD_SM, (uint32_t)(packed_command >> 32));
	pio_sm_put(sd_pio, SD_CMD_SM, (uint32_t)packed_command);

	uint byte_length = 6;
	switch (cmd)
	{
	// R0
	case MMC_GO_IDLE_STATE:
	case MMC_SET_DSR:
	case MMC_GO_INACTIVE_STATE:
		byte_length = 0;
		break;
	// R2
	case MMC_SEND_CSD:
	case MMC_ALL_SEND_CID:
	case MMC_SEND_CID:
		byte_length = 17;
		break;
	}

	if (byte_length)
	{
		uint8_t receive_buf[20];
		rc = sd_response_dma(sd_cmd_dma_channel, SD_CMD_SM, receive_buf, byte_length, false, NULL, NULL, true, true, true);
		if (!rc)
		{
			((uint32_t *)receive_buf)[4] = __builtin_bswap32((((uint32_t *)receive_buf)[4] >> 1) | (((uint32_t *)receive_buf)[3] << 31));
			((uint32_t *)receive_buf)[3] = __builtin_bswap32((((uint32_t *)receive_buf)[3] >> 1) | (((uint32_t *)receive_buf)[2] << 31));
			((uint32_t *)receive_buf)[2] = __builtin_bswap32((((uint32_t *)receive_buf)[2] >> 1) | (((uint32_t *)receive_buf)[1] << 31));
			((uint32_t *)receive_buf)[1] = __builtin_bswap32((((uint32_t *)receive_buf)[1] >> 1) | (((uint32_t *)receive_buf)[0] << 31));
			((uint32_t *)receive_buf)[0] = __builtin_bswap32(((uint32_t *)receive_buf)[0] >> 1);

			bool ok = true;
			switch (cmd)
			{
			// R2:
			case MMC_SEND_CSD:
			case MMC_ALL_SEND_CID:
			case MMC_SEND_CID:
				if (response)
					memcpy(response, &receive_buf[1], 16);
				break;
			// R3:
			case SD_APP_OP_COND:
			case MMC_SEND_OP_COND:
				ok = receive_buf[0] == 0x3f && (receive_buf[1] & 0x1e) == 0;
				if (response)
					*(uint32_t *)response = __builtin_bswap32(*(uint32_t *)&receive_buf[1]);
				break;
			// R1:
			default:
			{
				if (cmd != receive_buf[0])
				{
					printf("tsk\n");
				}

				uint8_t crc = crc7_table[receive_buf[0]];
				crc = crc7_table[crc ^ receive_buf[1]];
				crc = crc7_table[crc ^ receive_buf[2]];
				crc = crc7_table[crc ^ receive_buf[3]];
				crc = crc7_table[crc ^ receive_buf[4]];
				if ((crc | 1u) != receive_buf[5])
				{
					panic("bad crc %02x != %02x\n", crc | 1u, receive_buf[5]);
					ok = false;
				}

				if (response)
					*(uint32_t *)response = __builtin_bswap32(*(uint32_t *)&receive_buf[1]);
			}
			}
			if (!ok)
			{
				printf("bad response from card\n");
				return SD_ERR_BAD_RESPONSE;
			}
		}
	}
	else
		pio_sm_set_enabled(sd_pio, SD_CMD_SM, true);

	sd_debug("SD command done %d\n", cmd);
	return SD_OK;
}

int sd_wait()
{
	int rc = acquiesce_sm(SD_DAT_SM);
	if (!rc)
	{
		pio_sm_put(sd_pio, SD_DAT_SM, sd_pio_cmd(sd_cmd_or_dat_offset_state_inline_instruction, pio_encode_set(pio_pindirs, 0)));
		pio_sm_put(sd_pio, SD_DAT_SM,
				   sd_pio_cmd(sd_cmd_or_dat_offset_state_inline_instruction, pio_encode_wait_pin(true, 0)));
		pio_sm_put(sd_pio, SD_DAT_SM, sd_pio_cmd(sd_cmd_or_dat_offset_state_inline_instruction, pio_encode_jmp(sd_cmd_or_dat_offset_no_arg_state_wait_high)));
		rc = acquiesce_sm(SD_DAT_SM);
	}
	return rc;
}

const char *states[] = { "idle", "ready", "ident", "stby", "tran", "data", "rcv", "prg", "dis" };

void print_status(uint32_t response)
{
	if (response & R1_OUT_OF_RANGE)
		printf(" ORANGE");
	if (response & R1_ADDRESS_ERROR)
		printf(" ADDRESS");
	if (response & R1_BLOCK_LEN_ERROR)
		printf(" BLEN");
	if (response & R1_ERASE_SEQ_ERROR)
		printf(" ESEQ");
	if (response & R1_ERASE_PARAM)
		printf(" EPARM");
	if (response & R1_WP_VIOLATION)
		printf(" WPV");
	if (response & R1_CARD_IS_LOCKED)
		printf(" LOCKED");
	if (response & R1_LOCK_UNLOCK_FAILED)
		printf(" UNLOCK");
	if (response & R1_COM_CRC_ERROR)
		printf(" CRC");
	if (response & R1_ILLEGAL_COMMAND)
		printf(" ILLEGAL");
	if (response & R1_CARD_ECC_FAILED)
		printf(" ECC");
	if (response & R1_CC_ERROR)
		printf(" INTERNAL");
	if (response & R1_ERROR)
		printf(" << ERRORS: ");
	if (response & R1_UNDERRUN)
		printf(" under_run");
	if (response & R1_OVERRUN)
		printf(" over_run");
	if (response & R1_CID_CSD_OVERWRITE)
		printf(" c_overwrite");
	if (response & R1_WP_ERASE_SKIP)
		printf(" era_skip");
	if (response & R1_CARD_ECC_DISABLED)
		printf(" ecc_dis");
	if (response & R1_ERASE_RESET)
		printf(" era_reset");
	if (response & R1_SWITCH_ERROR)
		printf(" switch");
	if (response & R1_EXCEPTION_EVENT)
		printf(" exception");
	printf(" %s", states[R1_CURRENT_STATE(response)]);
	printf((response & R1_READY_FOR_DATA) ? " ready" : " not-ready");
	if (response & R1_APP_CMD)
		printf(" ACMD...");
	printf("\n");
}

void read_status(bool dump)
{
	uint32_t response;
	int not_ready_retries = 3;
	while (not_ready_retries--)
	{
		// let's see the status
		sd_command(MMC_SEND_STATUS, (rca_high << 24) | (rca_low << 16), &response);
		if (dump)
			print_status(response);

		// Break if ready
		if (response & R1_READY_FOR_DATA)
			break;

		// Wait if not ready and try again
		sleep_ms(1);
	}
}

int sd_set_clock_divider(uint div)
{
	pio_sm_set_clkdiv_int_frac(sd_pio, SD_CLK_SM, div, 0);
	pio_sm_set_clkdiv_int_frac(sd_pio, SD_CMD_SM, div, 0);
	pio_sm_set_clkdiv_int_frac(sd_pio, SD_DAT_SM, div, 0);
	pio_clkdiv_restart_sm_mask(sd_pio, (1u << SD_CMD_SM) | (1u << SD_CLK_SM) | (1u << SD_DAT_SM));
	return SD_OK;
}

int sd_init()
{
	int sd_clk_pin = MMC_CLK_PIN;
	int sd_cmd_pin = MMC_CMD_PIN;
	int sd_dat_pin_base = MMC_DAT0_PIN;

	gpio_set_function(sd_clk_pin, GPIO_FUNC_PIO1);
	gpio_set_slew_rate(sd_clk_pin, GPIO_SLEW_RATE_FAST);
	gpio_set_drive_strength(sd_clk_pin, GPIO_DRIVE_STRENGTH_12MA);
	gpio_set_function(sd_cmd_pin, GPIO_FUNC_PIO1);
	gpio_set_function(sd_dat_pin_base, GPIO_FUNC_PIO1);
	gpio_set_pulls(sd_clk_pin, false, true);
	gpio_set_pulls(sd_cmd_pin, true, false);
	gpio_set_pulls(sd_dat_pin_base, true, false);

	static bool added; // todo this is a temporary hack as we don't free
	static uint cmd_or_dat_offset;
	static uint clk_program_offset;

	if (!added)
	{
		cmd_or_dat_offset = pio_add_program(sd_pio, &sd_cmd_or_dat_program);
		assert(!cmd_or_dat_offset); // we don't add this later because it is assumed to be 0
		clk_program_offset = pio_add_program(sd_pio, &sd_clk_program);
		added = true;
	}

	pio_sm_config c = sd_clk_program_get_default_config(clk_program_offset);
	sm_config_set_sideset_pins(&c, sd_clk_pin);
	pio_sm_init(sd_pio, SD_CLK_SM, clk_program_offset, &c);

	c = sd_cmd_or_dat_program_get_default_config(cmd_or_dat_offset);
	sm_config_set_out_pins(&c, sd_cmd_pin, 1);
	sm_config_set_set_pins(&c, sd_cmd_pin, 1);
	sm_config_set_in_pins(&c, sd_cmd_pin);
	sm_config_set_in_shift(&c, false, true, 32);
	sm_config_set_out_shift(&c, false, true, 32);
	pio_sm_init(sd_pio, SD_CMD_SM, cmd_or_dat_offset, &c);

	c = sd_cmd_or_dat_program_get_default_config(cmd_or_dat_offset);
	sm_config_set_out_pins(&c, sd_dat_pin_base, 1);
	sm_config_set_set_pins(&c, sd_dat_pin_base, 1);
	sm_config_set_in_pins(&c, sd_dat_pin_base);
	sm_config_set_in_shift(&c, false, true, 32);
	sm_config_set_out_shift(&c, false, true, 32);
	pio_sm_init(sd_pio, SD_DAT_SM, cmd_or_dat_offset, &c);

	int rc = sd_set_clock_divider(355); // 375KHz
	if (rc)
		return rc;

	pio_sm_exec(sd_pio, SD_CMD_SM, pio_encode_jmp(sd_cmd_or_dat_offset_no_arg_state_wait_high));
	pio_sm_exec(sd_pio, SD_DAT_SM, pio_encode_jmp(sd_cmd_or_dat_offset_no_arg_state_waiting_for_cmd));

	uint32_t all_pin_mask = (1 << sd_dat_pin_base) | (1u << sd_cmd_pin) | (1u << sd_clk_pin);
	pio_sm_set_pindirs_with_mask(sd_pio, SD_CLK_SM, all_pin_mask, all_pin_mask);
	pio_sm_exec(sd_pio, SD_DAT_SM, pio_encode_set(pio_pins, 1));

	// Reset hack for xbox
	gpio_init(MMC_RST_PIN);
	gpio_set_dir(MMC_RST_PIN, GPIO_OUT);
	gpio_put(MMC_RST_PIN, 0);
	sleep_ms(50);
	gpio_put(MMC_RST_PIN, 1);
	sleep_ms(50);

	pio_sm_put(sd_pio, SD_CMD_SM, sd_pio_cmd(sd_cmd_or_dat_offset_state_send_bits, 80 - 1));
	pio_sm_put(sd_pio, SD_CMD_SM, 0xffffffff);
	pio_sm_put(sd_pio, SD_CMD_SM, 0xffffffff);

	pio_sm_put(sd_pio, SD_CMD_SM, 0xffff0000 | pio_encode_jmp(sd_cmd_or_dat_offset_no_arg_state_wait_high));
	pio_enable_sm_mask_in_sync(sd_pio, (1u << SD_CMD_SM) | (1u << SD_CLK_SM) | (1u << SD_DAT_SM));

	sd_command(MMC_GO_IDLE_STATE, 0, 0);

	while (true)
	{
		uint32_t response;
		sd_command(MMC_SEND_OP_COND, 0x40FF8000, &response);
		if ((response & 0xFF000000) == (MMC_CARD_BUSY | SD_OCR_CCS))
			break;
	}
	sd_debug("Card ready\r\n");

	sd_command(MMC_ALL_SEND_CID, 0, cid_raw);
	sd_debug("CID: ");
	for (int i = 0; i < 16; ++i)
	{
		sd_debug("%02X ", cid_raw[i]);
	}
	sd_debug("\n");

	uint32_t response;
	sd_command(MMC_SET_RELATIVE_ADDR, 0, &response);
	rca_high = response >> 24;
	rca_low = response >> 16;

	sd_command(MMC_SEND_CSD, (rca_high << 24) | (rca_low << 16), csd_raw);
	sd_debug("CSD: ");
	for (int i = 0; i < 16; ++i)
	{
		sd_debug("%02X ", csd_raw[i]);
	}
	sd_debug("\n");

	sd_command(MMC_SELECT_CARD, (rca_high << 24) | (rca_low << 16), &response);
	sd_wait();

	return sd_set_clock_divider(4);
}

static uint32_t *start_read_to_buf(int sm, uint32_t *buf, uint byte_length, bool first)
{
	uint bit_length = byte_length * 8;
	if (sm == SD_DAT_SM)
	{
		assert(!(bit_length & 31u));
		bit_length += 16;
	}

	*buf++ = sd_pio_cmd(sd_cmd_or_dat_offset_state_receive_bits, bit_length - 1);
	if (first)
		pio_sm_set_wrap(sd_pio, sm, sd_cmd_or_dat_wrap_target, sd_cmd_or_dat_wrap);

	// add zero padding to word boundary if necessary
	if (bit_length & 31u)
	{
		*buf++ = sd_pio_cmd(sd_cmd_or_dat_offset_state_inline_instruction, pio_encode_in(pio_null, 32 - (bit_length & 31u)));
	}
	*buf++ = sd_pio_cmd(sd_cmd_or_dat_offset_state_inline_instruction, pio_encode_jmp(sm == SD_DAT_SM ? sd_cmd_or_dat_offset_no_arg_state_waiting_for_cmd : sd_cmd_or_dat_offset_no_arg_state_wait_high));
	return buf;
}

// note caller must make space for CRC (2 word) in 4 bit mode
int sd_readblocks_scatter_async(uint32_t *control_words, uint32_t block, uint block_count)
{
	assert(pio_sm_is_rx_fifo_empty(sd_pio, SD_DAT_SM));
	uint32_t total = 0;
	uint32_t *p = control_words;
	while (p[0])
	{
		//        printf("%p %08x %08x\n", p, (uint)p[0], (uint)p[1]);
		assert(p[1]);
		total += p[1];
		p += 2;
	}

	// todo further state checks
	while (sd_pio->sm[SD_DAT_SM].addr != sd_cmd_or_dat_offset_no_arg_state_waiting_for_cmd)
	{
		printf("oops %d\n", (uint)sd_pio->sm[SD_DAT_SM].addr);
	}
	assert(sd_pio->sm[SD_DAT_SM].addr == sd_cmd_or_dat_offset_no_arg_state_waiting_for_cmd);
	assert(pio_sm_is_rx_fifo_empty(sd_pio, SD_DAT_SM));
	assert(block_count <= SDIO_MAX_BLOCK_COUNT);

	assert(total == block_count * (128 + 1));
	start_chain_dma_read_with_address_size_only(SD_DAT_SM, control_words, true, false);
	uint32_t *buf = pio_cmd_buf;
	for (int i = 0; i < block_count; i++)
	{
		buf = start_read_to_buf(SD_DAT_SM, buf, 512, !i);
	}

	dma_channel_config c = dma_channel_get_default_config(sd_pio_dma_channel);
	channel_config_set_read_increment(&c, true);
	channel_config_set_write_increment(&c, false);
	channel_config_set_dreq(&c, DREQ_PIO1_TX0 + SD_DAT_SM);
	dma_channel_configure(
		sd_pio_dma_channel,
		&c,
		&sd_pio->txf[SD_DAT_SM], // dest
		pio_cmd_buf,			 // src
		buf - pio_cmd_buf,
		true);
	// todo decide timing of this - as long as dat lines are hi, this is fine. (note this comment now applies to the trigger true in the dma_channel_configure)
	// dma_channel_start(sd_pio_dma_channel);
	assert(block_count);
	int rc;
	if (block_count == 1)
	{
		uint32_t response;
		rc = sd_command(MMC_READ_SINGLE_BLOCK, block, &response);
		if (!rc)
			rc = response & R1_OUT_OF_RANGE;
	}
	else
	{
		rc = sd_command(MMC_SET_BLOCK_COUNT, block_count, 0);
		if (!rc)
		{
			uint32_t response;
			rc = sd_command(MMC_READ_MULTIPLE_BLOCK, block, &response);
			if (!rc)
				rc = response & R1_OUT_OF_RANGE;
		}
	}
	return rc;
}

bool sd_scatter_read_complete(int *status)
{
	// printf("%d:%d %d:%d %d:%d %d\n", dma_channel_is_busy(sd_chain_dma_channel), (uint)dma_hw->ch[sd_chain_dma_channel].transfer_count,
	//	   dma_channel_is_busy(sd_data_dma_channel), (uint)dma_hw->ch[sd_data_dma_channel].transfer_count,
	//	   dma_channel_is_busy(sd_pio_dma_channel), (uint)dma_hw->ch[sd_pio_dma_channel].transfer_count, (uint)sd_pio->sm[SD_DAT_SM].addr);
	// this is a bit half arsed atm
	bool rc;
	if (dma_channel_is_busy(sd_chain_dma_channel) || dma_channel_is_busy(sd_data_dma_channel) || dma_channel_is_busy(sd_pio_dma_channel))
	{
		rc = false;
	}
	else
	{
		rc = (sd_pio->sm[SD_DAT_SM].addr == sd_cmd_or_dat_offset_no_arg_state_waiting_for_cmd &&
			  pio_sm_is_tx_fifo_empty(sd_pio, SD_DAT_SM));
	}
	if (status)
		*status = SD_OK;
	return rc;
}

int sd_readblocks_async(void *buf, uint32_t block, uint block_count)
{
	assert(block_count <= SDIO_MAX_BLOCK_COUNT);

	uint32_t *p = ctrl_words;
	uint crc_words = 1;
	for (int i = 0; i < block_count; i++)
	{
		*p++ = (uintptr_t)((uint8_t *)buf + i * 512);
		*p++ = 128;
		// for now we read the CRCs also
		*p++ = (uintptr_t)(crcs + i * crc_words);
		*p++ = crc_words;
	}
	*p++ = 0;
	*p++ = 0;
	return sd_readblocks_scatter_async(ctrl_words, block, block_count);
}

int sd_readblocks_sync(void *buf, uint32_t block, uint block_count)
{
	assert(block_count <= SDIO_MAX_BLOCK_COUNT);

	uint32_t *p = ctrl_words;
	uint crc_words = 1;
	for (int i = 0; i < block_count; i++)
	{
		*p++ = (uintptr_t)((uint8_t *)buf + i * 512);
		*p++ = 128;
		// for now we read the CRCs also
		*p++ = (uintptr_t)(crcs + i * crc_words);
		//        printf("%08x\n", (uint)(uint32_t)(crcs + i * crc_words));
		*p++ = crc_words;
	}
	*p++ = 0;
	*p++ = 0;
	int rc = sd_readblocks_scatter_async(ctrl_words, block, block_count);
	if (!rc)
	{
		while (!sd_scatter_read_complete(&rc))
		{
			tight_loop_contents();
		}
	}
	return rc;
}

static void __time_critical_func(start_chain_dma_write)(uint sm, uint32_t *buf)
{
	dma_channel_config c = dma_get_channel_config(sd_data_dma_channel);
	channel_config_set_chain_to(&c, sd_chain_dma_channel);
	channel_config_set_irq_quiet(&c, true);
	dma_channel_set_config(sd_data_dma_channel, &c, false);

	c = dma_channel_get_default_config(sd_chain_dma_channel);
	channel_config_set_read_increment(&c, true);
	channel_config_set_write_increment(&c, true);
	channel_config_set_ring(&c, 1, 4); // wrap the write at 16 bytes
	dma_channel_configure(
		sd_chain_dma_channel,
		&c,
		&dma_channel_hw_addr(sd_data_dma_channel)->read_addr, // ch DMA config (target "ring" buffer size 16) - this is (read_addr, write_addr, transfer_count, ctrl),                    // dest
		buf,												  // src
		4,													  // send 4 words to ctrl block of data chain per transfer
		false);
	gpio_set_mask(1);
	dma_channel_start(sd_chain_dma_channel);
	gpio_clr_mask(1);
}

static uint32_t dma_ctrl_for(enum dma_channel_transfer_size size, bool src_incr, bool dst_incr, uint dreq,
							 uint chain_to, bool ring_sel, uint ring_size, bool enable)
{
	dma_channel_config c = dma_channel_get_default_config(0); // channel doesn't matter as we set chain_to later (it is just use to pre-populate that)
	channel_config_set_transfer_data_size(&c, size);
	channel_config_set_read_increment(&c, src_incr);
	channel_config_set_write_increment(&c, dst_incr);
	channel_config_set_dreq(&c, dreq);
	channel_config_set_chain_to(&c, chain_to);
	channel_config_set_ring(&c, ring_sel, ring_size);
	channel_config_set_enable(&c, enable);
	return c.ctrl;
}

int sd_writeblocks_async(const void *data, uint32_t sector_num, uint sector_count)
{
	uint32_t *buf = pio_cmd_buf;
	for (int i = 0; i < sector_count; i++)
	{
		// we send an extra word even though the CRC is only 16 bits to make life easy... the receiver doesn't care
		// todo that would need to work anyway for inline CRC (which can't include a pio_cmd)
		*buf++ = sd_pio_cmd(sd_cmd_or_dat_offset_state_send_bits, 512 * 8 + 32 + 32 - 1);
	}
	*buf++ = sd_pio_cmd(sd_cmd_or_dat_offset_state_inline_instruction, pio_encode_jmp(sd_cmd_or_dat_offset_no_arg_state_wait_high));

	if (sector_count > (SDIO_MAX_BLOCK_COUNT - 1) / 4)
	{
		panic("too many blocks for now");
	}

	assert(pio_sm_is_tx_fifo_empty(sd_pio, SD_DAT_SM));

	uint32_t *p = ctrl_words;
#define build_transfer(src, words, size, flags)  \
	*p++ = (uintptr_t)(src);                     \
	*p++ = (uintptr_t)(&sd_pio->txf[SD_DAT_SM]); \
	*p++ = words;                                \
	*p++ = dma_ctrl_for(size, true, false, DREQ_PIO1_TX0 + SD_DAT_SM, sd_chain_dma_channel, 0, 0, true) | (flags);

	for (int i = 0; i < sector_count; i++)
	{
		// first cb - zero out sniff data
		*p++ = (uintptr_t)&zeroes;
		*p++ = (uintptr_t)(&dma_hw->sniff_data);
		*p++ = 1;
		*p++ = dma_ctrl_for(DMA_SIZE_32, false, false, DREQ_FORCE, sd_chain_dma_channel, 0, 0, true);
		// second cb - send bits command
		build_transfer(pio_cmd_buf + i, 1, DMA_SIZE_32, 0);
		build_transfer(&start_bit, 1, DMA_SIZE_32, 0);
		// third cb - 128 words of sector data
		build_transfer((uint8_t *)data + i * 512, 128, DMA_SIZE_32, DMA_CH0_CTRL_TRIG_BSWAP_BITS | DMA_CH0_CTRL_TRIG_SNIFF_EN_BITS);
        // fourth cb - transfer sniff
		// note offset of 2, since we bswap the data
		build_transfer((uintptr_t)&dma_hw->sniff_data, 1, DMA_SIZE_16, 0); // DMA_CH0_CTRL_TRIG_BSWAP_BITS);
	}
	// final cb - return to wait state
	build_transfer(pio_cmd_buf + sector_count, 1, DMA_SIZE_32, 0);
	*p++ = 0;
	*p++ = 0;

	// todo further state checks
	while (sd_pio->sm[SD_DAT_SM].addr != sd_cmd_or_dat_offset_no_arg_state_waiting_for_cmd)
	{
		printf("oops %d\n", (uint)sd_pio->sm[SD_DAT_SM].addr);
	}
	assert(sd_pio->sm[SD_DAT_SM].addr == sd_cmd_or_dat_offset_no_arg_state_waiting_for_cmd);
	assert(pio_sm_is_tx_fifo_empty(sd_pio, SD_DAT_SM));
	pio_sm_put(sd_pio, SD_DAT_SM, sd_pio_cmd(sd_cmd_or_dat_offset_state_inline_instruction, pio_encode_jmp(sd_cmd_or_dat_offset_no_arg_state_wait_high)));
	while (sd_pio->sm[SD_DAT_SM].addr != sd_cmd_or_dat_offset_no_arg_state_waiting_for_cmd)
	{
	}

	int rc = 0;

	assert(sector_count);

	if (sector_count == 1)
	{
		rc = sd_command(MMC_WRITE_BLOCK, sector_num, 0);
	}
	else
	{
		// todo this is only writing the first sector on SanDisk EDGE 16G right now - probably need a delay between sectors... works fine on a SAMSUNG EVO 32G

		rc = sd_command(MMC_SET_BLOCK_COUNT, sector_count, 0);
		if (!rc)
			rc = sd_command(MMC_WRITE_MULTIPLE_BLOCK, sector_num, 0);
	}

	if (!rc)
	{
		pio_sm_set_enabled(sd_pio, SD_DAT_SM, false);
		dma_sniffer_enable(sd_data_dma_channel, DMA_SNIFF_CTRL_CALC_VALUE_CRC16, true);
		dma_sniffer_set_byte_swap_enabled(true);
		start_chain_dma_write(SD_DAT_SM, ctrl_words);
		pio_sm_set_enabled(sd_pio, SD_DAT_SM, true);
		// printf("dma chain data (rem %04x @ %08x) data (rem %04x @ %08x) pio data (rem %04x @ %08x) datsm @ %d\n",
		// 	   (uint)dma_hw->ch[sd_chain_dma_channel].transfer_count,
		// 	   (uint)dma_hw->ch[sd_chain_dma_channel].read_addr,
		// 	   (uint)dma_hw->ch[sd_data_dma_channel].transfer_count, (uint)dma_hw->ch[sd_data_dma_channel].read_addr,
		// 	   (uint)dma_hw->ch[sd_pio_dma_channel].transfer_count, (uint)dma_hw->ch[sd_pio_dma_channel].read_addr,
		// 	   (int)sd_pio->sm[SD_DAT_SM].addr);
	}
	return rc;
}

bool sd_write_complete(int *status)
{
	// printf("dma chain data (rem %04x @ %08x) data (rem %04x @ %08x) datsm @ %d\n",
	// 	   (uint)dma_hw->ch[sd_chain_dma_channel].transfer_count, (uint)dma_hw->ch[sd_chain_dma_channel].read_addr,
	// 	   (uint)dma_hw->ch[sd_data_dma_channel].transfer_count, (uint)dma_hw->ch[sd_data_dma_channel].read_addr,
	// 	   (int)sd_pio->sm[SD_DAT_SM].addr);
	// this is a bit half arsed atm
	bool rc;
	if (dma_channel_is_busy(sd_chain_dma_channel) || dma_channel_is_busy(sd_data_dma_channel))
		rc = false;
	else
		rc = sd_pio->sm[SD_DAT_SM].addr == sd_cmd_or_dat_offset_no_arg_state_waiting_for_cmd;
	if (rc)
	{
		// The pio finished sending the data, but the sd may still writes the data to the nand.
		// Here we check if it's still in programming state.
		uint32_t response;
		sd_command(MMC_SEND_STATUS, (rca_high << 24) | (rca_low << 16), &response);
		if (R1_CURRENT_STATE(response) == R1_STATE_PRG)
			rc = false;
	}
	if (status)
		*status = SD_OK;
	return rc;
}

int sd_writeblocks_sync(const void *data, uint32_t sector_num, uint sector_count)
{
	int rc = sd_writeblocks_async(data, sector_num, sector_count);
	if (!rc)
	{
		while (!sd_write_complete(&rc))
		{
			tight_loop_contents();
		}
	}
	return rc;
}

void sd_read_cid(void *cid)
{
	memcpy(cid, cid_raw, sizeof(cid_raw));
}

void sd_read_csd(void *csd)
{
	memcpy(csd, csd_raw, sizeof(csd_raw));
}

int sd_read_ext_csd(void *ext_csd)
{
	uint32_t *p = ctrl_words;

	*p++ = (uintptr_t)ext_csd;
	*p++ = 128;

	// for now we read the CRCs also
	*p++ = (uintptr_t)crcs;
	*p++ = 1;

	*p++ = 0;
	*p++ = 0;

	assert(pio_sm_is_rx_fifo_empty(sd_pio, SD_DAT_SM));

	uint32_t total = 0;

	p = ctrl_words;
	while (p[0])
	{
		assert(p[1]);
		total += p[1];
		p += 2;
	}

	// todo further state checks
	while (sd_pio->sm[SD_DAT_SM].addr != sd_cmd_or_dat_offset_no_arg_state_waiting_for_cmd)
	{
		printf("oops %d\n", (uint)sd_pio->sm[SD_DAT_SM].addr);
	}
	assert(sd_pio->sm[SD_DAT_SM].addr == sd_cmd_or_dat_offset_no_arg_state_waiting_for_cmd);
	assert(pio_sm_is_rx_fifo_empty(sd_pio, SD_DAT_SM));

	start_chain_dma_read_with_address_size_only(SD_DAT_SM, ctrl_words, true, false);
	uint32_t *buf = pio_cmd_buf;
	buf = start_read_to_buf(SD_DAT_SM, pio_cmd_buf, 512, true);

	dma_channel_config c = dma_channel_get_default_config(sd_pio_dma_channel);
	channel_config_set_read_increment(&c, true);
	channel_config_set_write_increment(&c, false);
	channel_config_set_dreq(&c, DREQ_PIO1_TX0 + SD_DAT_SM);
	dma_channel_configure(
		sd_pio_dma_channel,
		&c,
		&sd_pio->txf[SD_DAT_SM], // dest
		pio_cmd_buf,			 // src
		buf - pio_cmd_buf,
		true);

	uint32_t response;
	int rc = sd_command(MMC_SEND_EXT_CSD, 0, &response);
	if (!rc)
	{
		while (!sd_scatter_read_complete(&rc))
		{
			tight_loop_contents();
		}
	}

	return rc;
}