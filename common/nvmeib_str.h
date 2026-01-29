/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIB_STR_H
#define NVMEIB_STR_H
#ifndef __KERNEL__
	#include <string.h>
	#include <stdlib.h>
#	define unsafe_memcpy(_dst, _src, _len, _just) memcpy(_dst, _src, _len)
#else
#	include <linux/string.h>
#	ifndef unsafe_memcpy
#		define unsafe_memcpy(_dst, _src, _len, _just) memcpy(_dst, _src, _len) 
#	endif
#endif

/*
 * Copy src to string dst of size size.  At most size-1 characters
 * will be copied.  Always NUL terminates (unless size == 0).
 * Returns strlen(src); if retval >= size, truncation occurred.
 */
static inline size_t nvmeib_strlcpy(char *dst, const char *src, size_t max_len_unsigned)
{
	int max_len = (int)max_len_unsigned;
	size_t src_len = src ? strlen(src) : 0;
	if (max_len <= 0) {
		// Do nothing
	} else if ((int)src_len + 1 <= max_len) {	// Copy entire source
		if (src) {
			unsafe_memcpy(dst, src, src_len + 1, "nvmeib_strlcpy");
		} else
			dst[0] = '\0';
	} else {									// truncation
		unsafe_memcpy(dst, src, max_len - 1, "nvmeib_strlcpy");
		dst[max_len - 1] = '\0';
	}
	return src_len;
}

#ifndef strlcpy
	#define strlcpy(dst, src, size) nvmeib_strlcpy(dst, src, size)
#endif
#ifndef scnprintf
	#define scnprintf(buf,len, ...)	({ int _x = snprintf(buf, len, __VA_ARGS__); ((_x >= (int)len-1) ? (int)len-1 : _x); })
#endif	// Note some kernels do not have this function

/* Daniel: In some kernels strchrnul() function does not appear, also in user space for mac os */
static inline char *my_strchrnul(const char *s, int c)
{
	while (*s && *s != (char)c)
		s++;
	return (char *)s;
}

// include/linux/stringfiy.h
#ifndef __stringify
	#define __stringify_1(x...)	#x
	#define __stringify(x...)	__stringify_1(x)
#endif

#ifndef __KERNEL__
	#include <errno.h> // for EIO / EINVAL in userspce

	// lib/hexdump.c
	static inline int hex_to_bin(char c) {
		if (     c >= '0' && c <= '9')		return c - '0';
		else if (c >= 'a' && c <= 'f')		return c - 'a' + 10;
		else if (c >= 'A' && c <= 'F')		return c - 'A' + 10;
		return -1;
	}

	static inline bool isxdigit_imp(char c) {
		return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
	}

	static inline int kstrtol(const char *s, unsigned int base, long *res){ *res = strtol(s, NULL, base); return 0; }
	static inline int kstrtoull(const char *s, unsigned int base, unsigned long long *res) {
		if (*s > '9' || *s < '0') {
			return -EINVAL;
		}
		*res = strtoull(s, NULL, base);
		return 0;
	}
	// return the last part (extract the filename) of a pathname.
	static inline const char *kbasename(const char *path){ const char *tail = strrchr(path, '/'); return tail ? tail + 1 : path; }
#endif // Kernel

#endif  //NVMEIB_STR_H
