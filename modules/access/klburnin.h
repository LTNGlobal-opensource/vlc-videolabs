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

/*
 * Functions for burning counters into video, and reading those burned
 * counters out of video.  Useful for detecting dropped and/or skipped
 * frames.
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void klburnin_V210_write_32bit_value(void *frame_bytes, uint32_t stride, uint32_t value, uint32_t lineNr);
uint32_t klburnin_V210_read_32bit_value(void *frame_bytes, uint32_t stride, uint32_t lineNr);

#ifdef __cplusplus
};
#endif
