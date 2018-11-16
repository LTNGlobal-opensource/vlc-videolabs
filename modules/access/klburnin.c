/*
 * Copyright (c) 2016-2018 Kernel Labs Inc. All Rights Reserved
 *
 * Address: Kernel Labs Inc., PO Box 745, St James, NY. 11780
 * Contact: sales@kernellabs.com
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "klburnin.h"

#define  y_white 0x3ff
#define  y_black 0x000
#define cr_white 0x200
#define cb_white 0x200

/* Six pixels */
static uint32_t white[] = {
	cr_white << 20 |  y_white << 10 | cb_white,
	y_white << 20 | cb_white << 10 |  y_white,
	cb_white << 20 |  y_white << 10 | cr_white,
	y_white << 20 | cr_white << 10 |  y_white,
};

static uint32_t black[] = {
	cr_white << 20 |  y_black << 10 | cb_white,
	y_black << 20 | cb_white << 10 |  y_black,
	cb_white << 20 |  y_black << 10 | cr_white,
	y_black << 20 | cr_white << 10 |  y_black,
};

/* KL paint 6 pixels in a single point */
__inline__ void V210_draw_6_pixels(uint32_t *addr, uint32_t *coloring)
{
	for (int i = 0; i < 5; i++) {
		addr[0] = coloring[0];
		addr[1] = coloring[1];
		addr[2] = coloring[2];
		addr[3] = coloring[3];
		addr += 4;
	}
}

__inline__ void V210_draw_box(uint32_t *frame_addr, uint32_t stride, int color)
{
	uint32_t *coloring;
	if (color == 1)
		coloring = white;
	else
		coloring = black;

	for (uint32_t l = 0; l < 30; l++) {
		uint32_t *addr = frame_addr + (l * (stride / 4));
		V210_draw_6_pixels(addr, coloring);
	}
}

__inline__ void V210_draw_box_at(uint32_t *frame_addr, uint32_t stride, int color, int x, int y)
{
	uint32_t *addr = frame_addr + (y * (stride / 4));
	addr += ((x / 6) * 4);
	V210_draw_box(addr, stride, color);
}

void klburnin_V210_write_32bit_value(void *frame_bytes, uint32_t stride, uint32_t value, uint32_t lineNr)
{
	for (int p = 31, sh = 0; p >= 0; p--, sh++) {
		V210_draw_box_at(((uint32_t *)frame_bytes), stride,
				 (value & (1 << sh)) == (uint32_t)(1 << sh), p * 30, lineNr);
	}
}

uint32_t klburnin_V210_read_32bit_value(void *frame_bytes, uint32_t stride, uint32_t lineNr)
{
	int xpos = 0;
	uint32_t bits = 0;
	for (int i = 0; i < 32; i++) {
		xpos = (i * 30) + 8;
		/* Sample the pixel eight lines deeper than the initial line, and eight pixels in from the left */
		uint32_t *addr = ((uint32_t *)frame_bytes) + ((lineNr + 8) * (stride / 4));
		addr += ((xpos / 6) * 4);

		bits <<= 1;

		/* Sample the pixel.... Compressor will decimate, we'll need a luma threshold for production. */
		if ((addr[1] & 0x3ff) > 0x080)
			bits |= 1;
	}
	return bits;
}
