/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

int nvmeib_remove_unsafe_symbols(char* dst, const char* src)
{
	const char *dst_start = dst;
	__attribute__((nonstring)) const char lut[128] = { // All printable ASCII's simplification lookup table
	//"          \n                      !"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\ ]^_`abcdefghijklmnopqrstuvwxyz{|}~ ";
	 "\0         \n                      !_#$%&_()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^__abcdefghijklmnopqrstuvwxyz{|}~ "};
	#define __transform(src) lut[(((int)(*(src)))&(0x7F))]
	while (__transform(src) == ' ')		// Clean leading spaces
		src++;
	for (; *src; src++, dst++) {
		*dst = __transform(src);
		if (*dst == ' ') {
			while (__transform(src+1) == ' ')
				src++;					// Compact series of spaces to ' '
		}
	}
	if ((dst > dst_start) && (dst[-1] == ' '))
		dst--;							// Remove trailing space(es)
	*dst = '\0';						// Add null terminator
	return (int)(dst-dst_start);
}
