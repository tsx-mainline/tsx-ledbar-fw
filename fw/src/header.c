// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Application header, placed right after the vector table (offset 0x184).
 * The bootloader looks for "CSIGN" and checks the CRC-16 over start..end.
 * tools/tsx-upg.py fills crc, crc_ptr, start and end after the link.
 */
#include "tsx.h"

struct tsx_header {
	uint32_t crc;		/* CRC-16/XMODEM of start..end */
	uint32_t crc_ptr;	/* address of this crc word */
	uint32_t start;		/* first byte of the CRC range */
	uint32_t end;		/* last byte of the CRC range */
	char tag[12];
	uint16_t major;
	uint16_t minor;
	uint16_t build;
	uint16_t product;
};

const struct tsx_header tsx_header __attribute__((section(".tsx_header"), used)) = {
	.crc = 0,
	.crc_ptr = 0,
	.start = 0,
	.end = 0,
	.tag = "CSIGN",
	.major = TSX_VERSION_MAJOR,
	.minor = TSX_VERSION_MINOR,
	.build = TSX_VERSION_BUILD,
	.product = TSX_PRODUCT_CODE,
};
