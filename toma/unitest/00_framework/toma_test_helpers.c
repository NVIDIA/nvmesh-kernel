/**
 * toma_test_helpers.c - Test environment initialization
 */

#include "nvmeibt_global.h"
#include "utils/nvmeibt_bm.h"

void TEST_init(void)
{
	nvmeibt_global_ctx_alloc();
	nvmeibt_bm_create();
}
