#include "kr_incs.h"
#include "ib_incs.h"
#include "nvmeibp_trace.h"

#include "nvmeib_public_mlx4_imp.c"
#include "nvmeib_public_mlx4p.h"

MODULE_AUTHOR("Excelero");
MODULE_DESCRIPTION("NVMeIB Public mlx4");
MODULE_LICENSE("GPL and additional rights");

#define DEBUG_LEVEL (int)0
static int (*debug_level_f)(void);

static int __attribute__ ((__unused__)) nvmeib_public_mlx4_debug_level(void)
{
	return debug_level_f ? debug_level_f() : DEBUG_LEVEL;
}

static void nvmeib_public_mlx4_set_debug_level(int (*dlf)(void))
{
	debug_level_f = dlf;
}

static int
nvmeib_public_mlx4_alloc_n_map(struct nvmeib_alloc_n_map *mem)
{
	return mlx4_alloc_n_map(mem);
}

static int
nvmeib_public_mlx4_unmapn_n_free(struct nvmeib_alloc_n_map *mem)
{
	return mlx4_unmapn_n_free(mem);
}

static int
nvmeib_public_mlx4_map_mr(struct ib_device *ibdev, struct ib_mr *mr,
	phys_addr_t *pages, int n_pages)
{
	return mlx4_map_mr(ibdev, mr, pages, n_pages);
}

static int nvmeib_public_mlx4_peek_cq(struct ib_cq *ibcq, int max)
{
	return mlx4_peek_cq(ibcq, max);
}

static struct nvmeib_device_public_ops mlx4 = {
	.module = THIS_MODULE,
	.alloc_n_map = nvmeib_public_mlx4_alloc_n_map,
	.unmapn_n_free = nvmeib_public_mlx4_unmapn_n_free,
	.map_mr = nvmeib_public_mlx4_map_mr,

	.query_device = ib_query_device,
	.post_send_atomic = nvmeib_public_generic_post_send_atomic,
	.set_debug_level = nvmeib_public_mlx4_set_debug_level,
	.peek_cq = nvmeib_public_mlx4_peek_cq,
	.create_rdda_qp = ib_create_qp,
	.destroy_rdda_qp = ib_destroy_qp,
};

static struct nvmeib_device_public_ops mlx4_odp = {
	.module = THIS_MODULE,
	.alloc_n_map = nvmeib_public_mlx4p_alloc_n_map,
	.unmapn_n_free = nvmeib_public_mlx4p_unmapn_n_free,
	.map_mr = nvmeib_public_mlx4p_map_mr,

	.query_device = ib_query_device,
	.post_send_atomic = nvmeib_public_generic_post_send_atomic,
	.set_debug_level = nvmeib_public_mlx4_set_debug_level,
	.peek_cq = nvmeib_public_mlx4_peek_cq,
	.create_rdda_qp = ib_create_qp,
	.destroy_rdda_qp = ib_destroy_qp,
};

#define DEV_MODNAME "mlx4_ib"

static int __init nvmeib_public_mlx4_init(void)
{
	return nvmeib_public_set_pops(DT_mlx4, DEV_MODNAME, &mlx4, &mlx4_odp);
}

static void __exit nvmeib_public_mlx4_exit(void)
{
	nvmeib_public_clear_pops(DT_mlx4);
}

module_init(nvmeib_public_mlx4_init);
module_exit(nvmeib_public_mlx4_exit);
