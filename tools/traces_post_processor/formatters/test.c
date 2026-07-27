/*clang-format off*/
#include "common/kr_incs.h" /*Must be first*/
/*clang-format on*/

/**
 * This file is used as development time test for custom formatters
 */

#include "formatter_functions.h"
#include "nvmeib_shared.h"
#include <dlfcn.h>

#define TEST_LIB_PATH "./libfmtrs.so"

static union nvmeibc_dbits_entry dbits_1, dbits_2;
static union nvmeib_lock_id lock_id = {
    .bits.idx_in_praid = 3, .bits.lock_id = 8, .bits.is_stale = 0, .bits.is_read = 1, .bits.reserved = 2};

struct test_case {
	const char *fun;
	long *arg;
	const char *exp;
	int datalen;
};

static const struct test_case cases[] = {
    {"fmt_nvmeibc_dbits_entry", (long *)&dbits_1, "(3 , {0,0})", 4},
    {"fmt_nvmeibc_dbits_entry", (long *)&dbits_2, "(5c,3c)", 4},
    {"fmt_nvmeib_lock_id", (long *)&lock_id, "(all=0xa0000083,idx=3,lock_id=0x8,read)", 4},
    {0}};

void init_globals() {
	dbits_1 = nvmeib_dbits_entry_build_for_seg(3);
	dbits_2 = nvmeib_dbits_entry_build_unk(5, 3);
}

int main() {
	int rv = 0;
	printf("Formatters test utility started\n");
	init_globals();
	{
		void *fmtlib = dlopen(TEST_LIB_PATH, RTLD_LAZY);
		if (fmtlib) {
			int i = 0;
			while (cases[i].fun) {
				formatter_function fun = dlsym(fmtlib, cases[i].fun);
				if (!fun) {
					fprintf(stderr, "Error importing function %s, skipping. %m\n", cases[i].fun);
					rv = 1;
				} else {
					char buf[1024];
					fun(buf, sizeof(buf), *cases[i].arg, cases[i].datalen);
					printf("%3u > %-40s: %s\n", i, cases[i].fun, buf);
					if (strcmp(cases[i].exp, buf)) {
						fprintf(stderr, "Error not matching expected %s.\n", cases[i].exp);
						rv = 1;
					}
				}
				++i;
			}
		} else {
			fprintf(stderr, "Error loading " TEST_LIB_PATH " %m\n");
			return 1;
		}
	}
	return rv;
}
