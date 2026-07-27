#ifndef __NVMEIB_KEEPER_IFACE__
#define __NVMEIB_KEEPER_IFACE__

struct nvmeib_keeper_ops;
struct ib_device;
struct ib_pd;
struct ib_mr;

/* Common functions for registering / unregistering keeper. Resolved when keeper loads */
#define DEFINE_COMMON_REGISTER_KEEPER_FN(_fn) int _fn(struct nvmeib_keeper_ops *ops)
#define DEFINE_COMMON_UNREGISTER_KEEPER_FN(_fn) void _fn(struct nvmeib_keeper_ops *ops)

/* Keeper functions for requesting register to common. Resolved when common loads */
#define DEFINE_KEEPER_REQUEST_REGISTER_FN(_fn) int _fn(\
DEFINE_COMMON_REGISTER_KEEPER_FN((*reg_keeper_fn)))

/* Interface for the keeper */

#define NVMEIB_KEEPER_FRS_INFO_VERSION 1

struct nvmeib_keeper_frs_info {
	unsigned version;
	struct ib_pd *pd;
	struct ib_mr *dma_mr;
	struct ib_mr **mr_arr;
	int n_mr;
	u64 mr_page_mask;
	int mr_page_size;
	int mr_max_size;
	int max_pages_per_mr;
};

#define DEFINE_KEEPER_PUSH_FRS_FN(_fn) int _fn(const char *inst_name, \
	const struct nvmeib_keeper_frs_info *nvmeib_keeper_frs_info)
#define DEFINE_KEEPER_POP_FRS_FN(_fn) int _fn(const char *mod_name, const char *inst_name, struct ib_device *ib_dev, \
	struct nvmeib_keeper_frs_info *nvmeib_keeper_frs_info)

#define DEFINE_KEEPER_CLOSE_CB_FN(_fn) void _fn(DEFINE_COMMON_UNREGISTER_KEEPER_FN((*unreg_keeper_fn)))

struct nvmeib_keeper_ops {
	DEFINE_KEEPER_PUSH_FRS_FN((*push_frs));
	DEFINE_KEEPER_POP_FRS_FN((*pop_frs));
	DEFINE_KEEPER_CLOSE_CB_FN((*close_cb));
};

#define CALL_KEEPER_OP(_ops, _fn) (*_ops->_fn)

#endif
