#ifndef SIW_STR_H
#define SIW_STR_H
#ifndef __KERNEL__ 

#include <string.h>

#endif //__KERNEL__

/*
 * Copy src to string dst of size size.  At most size-1 characters
 * will be copied.  Always NUL terminates (unless size == 0).
 * Returns strlen(src); if retval >= size, truncation occurred.
 */
static inline size_t siw_strlcpy(char *dst, const char *src, size_t max_len_unsigned)
{
	int max_len = (int)max_len_unsigned;

	size_t src_len = src ? strlen(src) : 0;
	if (max_len < 0) {
		// Do nothing
	} else if ((int)src_len + 1 <= max_len) {
		if (src) {
			memcpy(dst, src, src_len + 1);
		}
	} else if (max_len != 0) {
		memcpy(dst, src, max_len - 1);
		dst[max_len - 1] = '\0';
	}
	return src_len;
}

#ifndef strlcpy
#define strlcpy(dst, src, size) siw_strlcpy(dst, src, size)
#endif//strlcpy

#endif//NVMEIB_STR_H
