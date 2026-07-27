#include "tests_conf.h"

#include <ctype.h>
#include <assert.h>
#define _GNU_SOURCE
#include <errno.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/***********************/
/* GLOBAL VARS SECTION */
/***********************/
struct unitest_config *unitest_global_cfg;
/***************************/
/* GLOBAL VARS SECTION END */
/***************************/

#define DEBUG_TESTS_CONF 0

#define _error(fmt, ...) fprintf(stderr, "[unitest_filter_conf] ERROR: " fmt "\n", ##__VA_ARGS__)
#define _info(fmt, ...) fprintf(stderr, "[unitest_filter_conf] INFO: " fmt "\n", ##__VA_ARGS__)
#define _debug(fmt, ...) fprintf(stderr, "[unitest_filter_conf] DEBUG: " fmt "\n", ##__VA_ARGS__)

#if !defined(DEBUG_TESTS_CONF) || DEBUG_TESTS_CONF == 0
#undef _debug
#define _debug(...)
#endif

#define errgoto(...)                                                                                                   \
	({                                                                                                                 \
		_error(__VA_ARGS__);                                                                                           \
		goto err;                                                                                                      \
	})

#define MAX_FILTERS 1024
#define INITIAL_LINE_LEN 120

struct test_config {
	char *test;
	int nrep;
};

struct unitest_config {
	struct {
		struct test_config include[MAX_FILTERS];
		char *exclude[MAX_FILTERS];
	} filters;
};

void __trim_end(char *str, int len) {
    int i;
    if (str == NULL || len <= 0) {
        return;
    }

    for (i = len - 1; i >= 0; i--) {
        if (!isspace((unsigned char)str[i])) {
            break;
        }
    }

    str[i + 1] = '\0';
}
/**
 * Get next valid line from file, return 0 on success
 */
char *__get_next_line(FILE *fp) {
	size_t len = INITIAL_LINE_LEN;
	int actlen;
	char *line = malloc(len);
	assert(line);
	while ((actlen = getline(&line, &len, fp)) != -1) {
		if (actlen) {
			if (actlen <= 1 || line[0] == '#') /*Empty*/
				continue;

			__trim_end(line, actlen);
			return line;
		}
	}
	free(line);
	return NULL;
}

enum __read_config_mode { __RCM_IDLE, __RCM_INCLUDE, __RCM_EXCLUDE };

/**
 * Read configuration from an open file
 */
int __read_config(struct unitest_config *self, FILE *fp) {
	char *line;
	int inidx = 0, exidx = 0;
	enum __read_config_mode mode = __RCM_IDLE;
	while ((line = __get_next_line(fp))) {
		if (!strcmp("[include]", line)) {
			mode = __RCM_INCLUDE;
		} else if (!strcmp("[exclude]", line)) {
			mode = __RCM_EXCLUDE;
		} else if (mode == __RCM_INCLUDE) {
			const char space_str[] = " ";
			char *nrep_str;
			self->filters.include[inidx].test = strtok(line, space_str);
			if (self->filters.include[inidx].test) { //happen if line is empty string
				line = NULL;
				nrep_str = strtok(NULL, space_str);

				if (!nrep_str)
					self->filters.include[inidx].nrep = 1;
				else {
					int nrep = atoi(nrep_str);
					if (nrep) {
						self->filters.include[inidx].nrep = nrep;
					} else {
						errgoto("incorrect number of repetitions for function: %s, rep=%s",
							self->filters.include[inidx].test, nrep_str);
					}
				}
				inidx++;
			}
		} else if (mode == __RCM_EXCLUDE) {
			self->filters.exclude[exidx++] = line;
			line = NULL;
		}
		free(line);
	}

	return 0;
err:
	return -1;
}

struct unitest_config *unitest_init_config(const char *path) {
	FILE *fp               = NULL;
	struct unitest_config *self = malloc(sizeof(struct unitest_config));
	if (!self)
		errgoto("malloc (%m)");
	memset(self, 0, sizeof(struct unitest_config));
	{
		fp = fopen(path, "rt");
		if (!fp) {
			errgoto("open %s (%m)", path);
		} else {
			if (__read_config(self, fp))
				goto err;
		}
	}
	goto out;
err:
	unitest_destroy_config(self);
	self = NULL;
out:
	if (fp)
		fclose(fp);
	return self;
}

void unitest_destroy_config(struct unitest_config *self) {
	int i;
	if (!self)
		return;
	for (i = 0; i < MAX_FILTERS; ++i) {
		if (self->filters.include[i].test)
			free(self->filters.include[i].test);
		if (self->filters.exclude[i])
			free(self->filters.exclude[i]);
	}
	free(self);
}


// @return number of requested repetitions
static inline int __test_get_num_of_rep_from_conf(struct unitest_config *self, const char *tname) {
	{ /*Check explicit includes*/
		int i = 0;
		while (self->filters.include[i].test)
			if (!fnmatch(self->filters.include[i++].test, tname, 0)) {
				_debug("tname %s included by the rule %s, num of repetitions: %u", tname, self->filters.include[i - 1].test, self->filters.include[i - 1].nrep);
				return self->filters.include[i - 1].nrep;
			}
	}

	{ /*Check explicit excludes*/
		int i = 0;
		while (self->filters.exclude[i])
			if (!fnmatch(self->filters.exclude[i++], tname, 0)) {
				_info("tname %s excluded by the rule %s", tname, self->filters.exclude[i - 1]);
				return 0;
			}
	}

	_debug("tname %s included by default", tname);

	return 1; /*By default - include and repet once */
}

int unitest_get_test_num_of_rep(struct unitest_config *self, const char *tname) {
	if (!unitest_global_cfg)
		return 1;
	return __test_get_num_of_rep_from_conf(self, tname);
}
