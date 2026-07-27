#ifndef TESTS_CONF
#define TESTS_CONF

struct test_config;

extern struct unitest_config *unitest_global_cfg;

/**
 * Read unitest config file, create unitest_config object and
 * return
 * @param path to config file to read from
 * @return NULL or new config object
 */
struct unitest_config *unitest_init_config(const char *path);

/**
 * Release resources used by unitest_config
 * @param unitest_config CI context object
 */
void unitest_destroy_config(struct unitest_config *self);

/**
 * check whether a given test is excluded in the current context
 * and if included return the number of repetitions to run test.
 * @param unitest_config
 * @param tname Test name
 * @return number of repetitions, to run test.
 */
int unitest_get_test_num_of_rep(struct unitest_config *self, const char *tname);


#endif /* TESTS_CONF */