#ifndef TRACE_H
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>

#define GUID_SIZE sizeof("ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff")

#define trace(fmt, ...) fprintf(stderr, "%s[%d]-" fmt, __FUNCTION__, __LINE__, ## __VA_ARGS__)
#define FIN trace("-->\n")
#define FOUT trace("<--\n")
#define LINE trace("---\n")

// if x is NON-ZERO, error is printed
#define TEST_NZ(x, y) do { if ((x)) die(y); } while (0)

// if x is ZERO, error is printed
#define TEST_Z(x, y) do { if (!(x)) die(y); } while (0)

// if x is NEGATIVE, error is printed
#define TEST_N(x, y) do { if ((x) < 0) die(y); } while(0)

static inline void format_gid_raw(uint8_t raw[16], char *buf) {
	int i, n;

	for (n = 0, i = 0; i < 8; ++i) {
		n += sprintf(buf + n, "%04x", htons(((uint16_t *)raw)[i]));
		if (i < 7)
			buf[n++] = ':';
	}
}

static inline void format_gid(union ibv_gid *gid, char *buf) {
	format_gid_raw(gid->raw, buf);
}
 
static inline int die(const char *reason) {
	trace("Err: %s\n ", reason);
	exit(EXIT_FAILURE);
	return -1;
}

#endif

