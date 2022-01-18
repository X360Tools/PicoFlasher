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

#include "tusb.h"

//--------------------------------------------------------------------+
// Device Descriptors
//--------------------------------------------------------------------+
tusb_desc_device_t const desc_device =
	{
		.bLength = sizeof(tusb_desc_device_t),
		.bDescriptorType = TUSB_DESC_DEVICE,
		.bcdUSB = 0x0200,

		// Use Interface Association Descriptor (IAD) for CDC
		// As required by USB Specs IAD's subclass must be common class (2) and protocol must be IAD (1)
		.bDeviceClass = TUSB_CLASS_MISC,
		.bDeviceSubClass = MISC_SUBCLASS_COMMON,
		.bDeviceProtocol = MISC_PROTOCOL_IAD,

		.bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,

		.idVendor = 0x600D,
		.idProduct = 0x7001,
		.bcdDevice = 0x0100,

		.iManufacturer = 0x01,
		.iProduct = 0x02,
		.iSerialNumber = 0x03,

		.bNumConfigurations = 0x01};

// Invoked when received GET DEVICE DESCRIPTOR
// Application return pointer to descriptor
uint8_t const *tud_descriptor_device_cb(void)
{
	return (uint8_t const *)&desc_device;
}

//--------------------------------------------------------------------+
// Configuration Descriptor
//--------------------------------------------------------------------+

enum
{
	ITF_NUM_CDC = 0,
	ITF_NUM_CDC_DATA,
	ITF_NUM_TOTAL
};

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)

#define EPNUM_CDC_NOTIF 0x81
#define EPNUM_CDC_OUT 0x02
#define EPNUM_CDC_IN 0x82

uint8_t const desc_fs_configuration[] =
{
	// Config number, interface count, string index, total length, attribute, power in mA
	TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),

	// Interface number, string index, EP notification address and size, EP data address (out, in) and size.
	// TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),

	/* Interface Associate */
	8, TUSB_DESC_INTERFACE_ASSOCIATION, ITF_NUM_CDC, 2, TUSB_CLASS_CDC, CDC_COMM_SUBCLASS_ABSTRACT_CONTROL_MODEL, CDC_COMM_PROTOCOL_NONE, 0,
	/* CDC Control Interface */
	9, TUSB_DESC_INTERFACE, ITF_NUM_CDC, 0, 1, TUSB_CLASS_CDC, CDC_COMM_SUBCLASS_ABSTRACT_CONTROL_MODEL, CDC_COMM_PROTOCOL_NONE, 4,
	/* CDC Header */
	5, TUSB_DESC_CS_INTERFACE, CDC_FUNC_DESC_HEADER, U16_TO_U8S_LE(0x0120),
	/* CDC Call */
	5, TUSB_DESC_CS_INTERFACE, CDC_FUNC_DESC_CALL_MANAGEMENT, 0, ITF_NUM_CDC_DATA,
	/* CDC ACM: support line request */
	4, TUSB_DESC_CS_INTERFACE, CDC_FUNC_DESC_ABSTRACT_CONTROL_MANAGEMENT, 2,
	/* CDC Union */
	5, TUSB_DESC_CS_INTERFACE, CDC_FUNC_DESC_UNION, ITF_NUM_CDC, ITF_NUM_CDC_DATA,
	/* Endpoint Notification */
	7, TUSB_DESC_ENDPOINT, EPNUM_CDC_NOTIF, TUSB_XFER_INTERRUPT, U16_TO_U8S_LE(8), 16,
	/* CDC Data Interface */
	9, TUSB_DESC_INTERFACE, ITF_NUM_CDC_DATA, 0, 2, TUSB_CLASS_CDC_DATA, 0, 0, 0,
	/* Endpoint Out */
	7, TUSB_DESC_ENDPOINT, EPNUM_CDC_OUT, TUSB_XFER_BULK, U16_TO_U8S_LE(64), 0,
	/* Endpoint In */
	7, TUSB_DESC_ENDPOINT, EPNUM_CDC_IN, TUSB_XFER_BULK, U16_TO_U8S_LE(64), 0,
};

// Invoked when received GET CONFIGURATION DESCRIPTOR
// Application return pointer to descriptor
// Descriptor contents must exist long enough for transfer to complete
uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
	(void)index; // for multiple configurations

	return desc_fs_configuration;
}

//--------------------------------------------------------------------+
// String Descriptors
//--------------------------------------------------------------------+

// array of pointer to string descriptors
char const *string_desc_arr[] =
	{
		(const char[]){0x09, 0x04}, // 0: is supported language is English (0x0409)
		"PicoFlasher",				// 1: Manufacturer
		"PicoFlasher Device",		// 2: Product
		"123456",					// 3: Serials, should use chip ID
		"PicoFlasher CDC",			// 4: CDC Interface
};

static uint16_t _desc_str[32];

// Invoked when received GET STRING DESCRIPTOR request
// Application return pointer to descriptor, whose contents must exist long enough for transfer to complete
uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
	(void)langid;

	uint8_t chr_count;

	if (index == 0)
	{
		memcpy(&_desc_str[1], string_desc_arr[0], 2);
		chr_count = 1;
	}
	else
	{
		// Note: the 0xEE index string is a Microsoft OS 1.0 Descriptors.
		// https://docs.microsoft.com/en-us/windows-hardware/drivers/usbcon/microsoft-defined-usb-descriptors

		if (!(index < sizeof(string_desc_arr) / sizeof(string_desc_arr[0])))
			return NULL;

		const char *str = string_desc_arr[index];

		// Cap at max char
		chr_count = strlen(str);
		if (chr_count > 31)
			chr_count = 31;

		// Convert ASCII string into UTF-16
		for (uint8_t i = 0; i < chr_count; i++)
		{
			_desc_str[1 + i] = str[i];
		}
	}

	// first byte is length (including header), second byte is string type
	_desc_str[0] = (TUSB_DESC_STRING << 8) | (2 * chr_count + 2);

	return _desc_str;
}
