#include "kr_incs.h"
#include "ib_incs.h"
#include "nvmeibp_trace.h"

#include "nvmeib_public_siw_imp.c"
#include "nvmeib_public_siwp.h"

MODULE_AUTHOR("Excelero");
MODULE_DESCRIPTION("NVMeIB Public siw");
MODULE_LICENSE("GPL and additional rights");

#define DEBUG_LEVEL (int)0
static int (*debug_level_f)(void);

static __attribute__ ((__unused__)) int nvmeib_public_siw_debug_level(void)
{
	return debug_level_f ? debug_level_f() : DEBUG_LEVEL;
}


static struct nvmeib_device_public_ops siw_ops = {
	.module = THIS_MODULE,
	.alloc_n_map = siw_alloc_n_map,
	.unmapn_n_free = siw_unmapn_n_free,
	.map_mr = siw_map_mr,

	.query_device = ib_query_device,
	.post_send_atomic = nvmeib_public_generic_post_send_atomic,
};

static struct nvmeib_device_public_ops siw_odp = {
	.module = THIS_MODULE,
	.alloc_n_map = siw_alloc_n_map,
	.unmapn_n_free = siw_unmapn_n_free,
	.map_mr = siw_map_mr,

	.query_device = ib_query_device,
	.post_send_atomic = nvmeib_public_generic_post_send_atomic,
};

#define DEV_MODNAME "siw"

static int __init nvmeib_public_siw_init(void)
{
	int rv;

	if ((rv = nvmeib_public_set_pops(DT_siw, DEV_MODNAME, &siw_ops, &siw_odp)) < 0)
		goto out;

	siw_redirect_dprint_init();

out:
	return rv;
}

static void __exit nvmeib_public_siw_exit(void)
{
	siw_redirect_dprint_free();

	nvmeib_public_clear_pops(DT_siw);
}

module_init(nvmeib_public_siw_init);
module_exit(nvmeib_public_siw_exit);
