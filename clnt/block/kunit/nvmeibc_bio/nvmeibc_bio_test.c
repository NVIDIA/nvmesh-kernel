#include <kunit/test.h>

/* Define the test cases. */

static void nvmeibc_bio_add_test_basic(struct kunit *test) {
  KUNIT_EXPECT_EQ(test, 1, 1); //test should be ok
}

static void nvmeibc_bio_test_failure(struct kunit *test) {
  KUNIT_FAIL(test, "This test never passes.");
}

static struct kunit_case nvmeibc_bio_test_cases[] = {
    KUNIT_CASE(nvmeibc_bio_add_test_basic),
    KUNIT_CASE(nvmeibc_bio_test_failure),
    {}};

static struct kunit_suite nvmeibc_bio_test_suite = {
    .name = "nvmeibc_bio",
    .test_cases = nvmeibc_bio_test_cases,
};

kunit_test_suite(nvmeibc_bio_test_suite);

MODULE_LICENSE("GPL");
