#include "nvmeib_ib_driver.h"
#include "nvmeib_public.h"
#include "mlx/nvmeib_mlx.h"
#include "siw/nvmeib_siw.h"
#include "bnxt_re/nvmeib_bnxt_re.h"
#include "nvmeib_utils.h"
#include "nvmeib_public.h"
#include "nvmeibm_trace.h"

struct nvmeib_hwdev {
	struct list_head list;

#if KS_HAS_MODULE_MUTEX
	struct module *owner;
#else
	/* find_module is not available, must match by name */
	const char *owner;
#endif
	const char *name;
	enum nvmeib_dev_type type;
	unsigned long dev_caps;
	int max_rdda_sq_sz;

	struct mutex ops_guard;
	struct nvmeib_device_ops *ops;
	struct nvmeib_device_public_ops *pops;
};

static LIST_HEAD(hwdevs);
static DEFINE_MUTEX(hwdevs_guard);

static struct nvmeib_hwdev *nvmeib_ibdr_hwdev_get_by_module(struct module *m)
{
	struct nvmeib_hwdev *dev, *ret = NULL;

	mutex_lock(&hwdevs_guard);
	list_for_each_entry(dev, &hwdevs, list) {
#if KS_HAS_MODULE_MUTEX
		if (dev->owner == m) {
			ret = dev;
			goto unlock;
		}
#else
		/* find_module is not available, must match by module name */
		if (!dev->owner)
			continue;
		if (strncmp(dev->owner, m->name, MODULE_NAME_LEN) == 0) {
			ret = dev;
			goto unlock;
		}
#endif
	}

unlock:
	mutex_unlock(&hwdevs_guard);
	return ret;
}

static struct nvmeib_hwdev *nvmeib_ibdr_hwdev_get_by_type(enum nvmeib_dev_type dev_type)
{
	struct nvmeib_hwdev *dev, *ret = NULL;

	mutex_lock(&hwdevs_guard);
	list_for_each_entry(dev, &hwdevs, list) {
		if (dev->type == dev_type) {
			ret = dev;
			break;
		}
	}
	mutex_unlock(&hwdevs_guard);
	return ret;
}

int nvmeib_ibdr_dev_init(bool paging_enabled)
{
	int ret;

	ret = nvmeib_mlx4_init();
	if (ret)
		return ret;

	ret = nvmeib_mlx5_init(paging_enabled);
	if (ret) {
		nvmeib_mlx4_cleanup();
		return ret;
	}

	ret = nvmeib_siw_init();
	if (ret) {
		nvmeib_mlx5_cleanup();
		nvmeib_mlx4_cleanup();
		return ret;
	}

#if BNXT_RE
	ret = nvmeib_bnxt_re_init();
	if (ret) {
		nvmeib_siw_cleanup();
		nvmeib_mlx5_cleanup();
		nvmeib_mlx4_cleanup();
	}
#endif

	return ret;
}
EXPORT_SYMBOL(nvmeib_ibdr_dev_init);

static int nvmeib_ibdr_hwdev_pops_clear(struct nvmeib_hwdev *dev);

void nvmeib_ibdr_dev_cleanup(void)
{
	struct nvmeib_hwdev *dev;

	mutex_lock(&hwdevs_guard);
	list_for_each_entry(dev, &hwdevs, list) {
		nvmeib_ibdr_hwdev_pops_clear(dev);
	}
	mutex_unlock(&hwdevs_guard);
	
#if BNXT_RE
	nvmeib_bnxt_re_cleanup();
#endif
	nvmeib_siw_cleanup();
	nvmeib_mlx5_cleanup();
	nvmeib_mlx4_cleanup();
}
EXPORT_SYMBOL(nvmeib_ibdr_dev_cleanup);

int nvmeib_ibdr_hwdev_register(enum nvmeib_dev_type dev_type,
			       const char *name,
			       struct nvmeib_device_ops *ops,
			       unsigned long dev_caps,
			       int max_rdda_sq_sz)
{
	struct nvmeib_hwdev *dev = nvmeib_ibdr_hwdev_get_by_type(dev_type);

	if (dev) {
		_NI(trace_nvmeib_ib_driver_nvmeib_ibdr_hwdev_register, "Reregistering device type");
		return -EEXIST;
	}

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->ops = ops;
	dev->type = dev_type;
	dev->name = name;
	dev->dev_caps = dev_caps;
	dev->max_rdda_sq_sz = max_rdda_sq_sz;
	mutex_init(&dev->ops_guard);
	
	mutex_lock(&hwdevs_guard);
	list_add(&dev->list, &hwdevs);
	mutex_unlock(&hwdevs_guard);

	return 0;
}
EXPORT_SYMBOL(nvmeib_ibdr_hwdev_register);

int nvmeib_ibdr_hwdev_unregister(enum nvmeib_dev_type type)
{
	struct nvmeib_hwdev *dev;

	dev = nvmeib_ibdr_hwdev_get_by_type(type);
	if (!dev) {
		_NI(trace_nvmeib_ib_driver_nvmeib_ibdr_hwdev_unregister, "Unregistering an unknown device type");
		return -EINVAL;
	}

	mutex_lock(&hwdevs_guard);
	list_del(&dev->list);
	mutex_unlock(&hwdevs_guard);
	
	/* Needs to either have not been set or been cleared previously */
	BUG_ON(dev->pops);
	kfree(dev);

	return 0;
}
EXPORT_SYMBOL(nvmeib_ibdr_hwdev_unregister);

static inline struct module *ib_dev_owner(struct ib_device *ib_dev)
{
#if defined(IB_DEVICE_OPS_HAS_MODULE_OWNER) && IB_DEVICE_OPS_HAS_MODULE_OWNER
	return ib_dev->ops.owner;
#else
	return ib_dev->owner;
#endif
}

static struct nvmeib_hwdev *nvmeib_ibdr_hwdev_get(struct ib_device *ib_dev)
{
	struct module *owner = NULL;

	if (!ib_dev || !(owner = ib_dev_owner(ib_dev)))
		return NULL;

	return nvmeib_ibdr_hwdev_get_by_module(owner);
}
// EXPORT_SYMBOL(nvmeib_ibdr_hwdev_get);

enum nvmeib_dev_type nvmeib_get_device_type(struct ib_device *ib_dev)
{
	struct nvmeib_hwdev *dev = nvmeib_ibdr_hwdev_get(ib_dev);

	return dev ? dev->type : DT_uknown;
}
EXPORT_SYMBOL(nvmeib_get_device_type);

const char *nvmeib_ib_driver_dev_type(enum nvmeib_dev_type t)
{
	struct nvmeib_hwdev *dev = nvmeib_ibdr_hwdev_get_by_type(t);

	return dev ? dev->name : NULL;
}
EXPORT_SYMBOL(nvmeib_ib_driver_dev_type);

#if KS_HAS_MODULE_MUTEX
#define MODULE_PARAM_NAME(m) m->name
int nvmeib_ibdr_hwdev_pops_set(enum nvmeib_dev_type t,
                               struct module *m,
                               struct nvmeib_device_public_ops *pops)
#else
#define MODULE_PARAM_NAME(m) m
int nvmeib_ibdr_hwdev_pops_set(enum nvmeib_dev_type t,
                               const char *m,
                               struct nvmeib_device_public_ops *pops)
#endif
{
	struct nvmeib_hwdev *dev;
	int rv;

	dev = nvmeib_ibdr_hwdev_get_by_type(t);
	if (!dev) {
		rv = -EINVAL;
		goto out;
	}

	mutex_lock(&dev->ops_guard);

	if (dev->pops) {
		_NE(error_nvmeib_ib_driver_nvmeib_ibdr_hwdev_pops_set, "Device type @TOPOLOGY_INT already has pops set", t);
		rv = -EALREADY;
		goto unlock;
	}

	if (!try_module_get(pops->module)) {
		_NE(err_ibdr_hwdev_pops_set_ref_fail, 
		    "Failed to get reference count for @MODULE_NAME", MODULE_PARAM_NAME(m));
		rv = -EBUSY;
		goto unlock;
	}

	dev->owner = m;
	dev->pops = pops;
	rv = 0;

unlock:
	mutex_unlock(&dev->ops_guard);

out:
	return rv;
}
EXPORT_SYMBOL(nvmeib_ibdr_hwdev_pops_set);

static int nvmeib_ibdr_hwdev_pops_clear(struct nvmeib_hwdev *dev)
{
	mutex_lock(&dev->ops_guard);
	if (dev->pops) {
		module_put(dev->pops->module);
		dev->pops = NULL;
	}
	mutex_unlock(&dev->ops_guard);

	return 0;
}

static struct nvmeib_device_ops *nvmeib_ibdr_hwdev_ops_get(struct nvmeib_hwdev *dev, const char *call_fn)
{
	struct nvmeib_device_ops *ops = ERR_PTR(-EINVAL);

	if (!dev || !dev->ops) {
		_NE(error_nvmeib_ib_driver_nvmeib_ibdr_hwdev_ops_get, "Invalid device ptr @DEV", dev);
		goto out;
	}
	
	mutex_lock(&dev->ops_guard);
	ops = dev->ops;

	if (!try_module_get(ops->module)) {
		_NE(error_1_nvmeib_ib_driver_nvmeib_ibdr_hwdev_ops_get, "try_module_get failed for module @MODULE_NAME", ops->module->name);
		ops = ERR_PTR(-EBUSY);
	}
	else
		_ND(trace_nvmeib_ib_driver_nvmeib_ibdr_hwdev_ops_get, "module @MODULE_NAME call_fn @CALL_FN ref-cnt +1", ops->module->name, call_fn);
	mutex_unlock(&dev->ops_guard);

out:
	return ops;
}

static struct nvmeib_device_ops *nvmeib_ibdr_hwdev_ops_get_ib(struct ib_device *ib_dev, const char *call_fn)
{
	struct nvmeib_hwdev *dev = nvmeib_ibdr_hwdev_get(ib_dev);
	return nvmeib_ibdr_hwdev_ops_get(dev, call_fn);
}

static int nvmeib_ibdr_hwdev_ops_put(struct nvmeib_device_ops *ops, const char *call_fn)
{
	int rv = 0;

	if (IS_ERR_OR_NULL(ops)) {
		rv = -EINVAL;
		goto out;
	}

	module_put(ops->module);
	_ND(trace_nvmeib_ib_driver_nvmeib_ibdr_hwdev_ops_put, "module @MODULE_NAME call_fn @CALL_FN ref-cnt -1", ops->module->name, call_fn);

out:
	return rv;
}

struct nvmeib_device_public_ops *nvmeib_ibdr_hwdev_pops_get(struct ib_device *ib_dev)
{
	struct nvmeib_hwdev *dev = nvmeib_ibdr_hwdev_get(ib_dev);
	struct nvmeib_device_public_ops *pops = ERR_PTR(-ENODEV);
  
	if (!dev || !dev->pops) {
		_NE(error_nvmeib_ib_driver_nvmeib_ibdr_hwdev_pops_get, "Device @IB_DEV_NAME has no public ops registered", ib_dev->name);
		goto out;
	}

	mutex_lock(&dev->ops_guard);
	pops = dev->pops;

	if (!try_module_get(pops->module)) {
		_NE(error_1_nvmeib_ib_driver_nvmeib_ibdr_hwdev_pops_get, "try_module_get failed for module @MODULE_NAME", pops->module->name);
		pops = ERR_PTR(-EBUSY);
	}
	else
		_ND(trace_nvmeib_ib_driver_nvmeib_ibdr_hwdev_pops_get, "module @MODULE_NAME ref-cnt +1", pops->module->name);
	mutex_unlock(&dev->ops_guard);

out:
	return pops;
}
EXPORT_SYMBOL(nvmeib_ibdr_hwdev_pops_get);

int nvmeib_ibdr_hwdev_pops_put(struct nvmeib_device_public_ops *pops)
{
	module_put(pops->module);
	_ND(trace_nvmeib_ib_driver_nvmeib_ibdr_hwdev_pops_put, "module @MODULE_NAME ref-cnt -1", pops->module->name);
	return 0;
}
EXPORT_SYMBOL(nvmeib_ibdr_hwdev_pops_put);

#define NVMEIB_DEV_OPS_CALL(_ibdev, cb, ...) \
({ \
	 struct nvmeib_device_ops *ops; \
	 int rv; \
	 \
	 ops = nvmeib_ibdr_hwdev_ops_get_ib(_ibdev, #cb); \
	 if (!IS_ERR_OR_NULL(ops)) {\
		 if (ops->cb) \
			rv = ops->cb(__VA_ARGS__); \
		else\
			rv = -ENOSYS;\
		nvmeib_ibdr_hwdev_ops_put(ops, #cb); \
	 } \
	 else \
		 rv = ops ? PTR_ERR(ops) : -ENOSYS;\
	 rv; \
 })

#define NVMEIB_DEV_OPS_CALL_RET_PTR(_ibdev, cb, ...) \
({ \
	 struct nvmeib_device_ops *ops; \
	 void *rv = NULL; \
	 \
	 ops = nvmeib_ibdr_hwdev_ops_get_ib(_ibdev, #cb); \
	 if (!IS_ERR_OR_NULL(ops)) {\
		 if (ops->cb) \
			rv = ops->cb(__VA_ARGS__); \
		nvmeib_ibdr_hwdev_ops_put(ops, #cb); \
	 }\
	 rv; \
 })

void nvmeib_ibdr_hwdev_pops_call_all(void (*cb)(struct nvmeib_device_public_ops *pops, void *param), void *param)
{
	struct nvmeib_hwdev *dev;

	mutex_lock(&hwdevs_guard);
	list_for_each_entry(dev, &hwdevs, list) {
		if (dev->pops)
			(*cb)(dev->pops, param);
	}
	mutex_unlock(&hwdevs_guard);
}
EXPORT_SYMBOL(nvmeib_ibdr_hwdev_pops_call_all);

void nvmeib_ib_driver_enable_cap(enum nvmeib_dev_type t, enum nvmeib_dev_cap cap, bool enable)
{
	struct nvmeib_hwdev *dev = nvmeib_ibdr_hwdev_get_by_type(t);

	NFIN;
	if (dev)
		if (enable)
			dev->dev_caps |= cap;
		else
			dev->dev_caps &= ~cap;
	else
		_NW(warn_nvmeib_ib_driver_nvmeib_ib_driver_enable_cap, "Trying to set rdda support for non existing device type(@TOPOLOGY_INT)", t);
	NFOUT;
}
EXPORT_SYMBOL(nvmeib_ib_driver_enable_cap);

bool nvmeib_device_sup_cap(enum nvmeib_dev_type t, enum nvmeib_dev_cap cap)
{
	struct nvmeib_hwdev *dev = nvmeib_ibdr_hwdev_get_by_type(t);
	bool rv = false;

	NFIN;
	if (dev)
		rv = !!(dev->dev_caps & cap);
	else
		_NW(warn_nvmeib_ib_driver_nvmeib_device_sup_cap, "Querying rdda support of non existing device type (@TOPOLOGY_INT)", t);
	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_device_sup_cap);

int nvmeib_device_get_max_rd_atom_on_wire(enum nvmeib_dev_type t)
{
	struct nvmeib_hwdev *dev = nvmeib_ibdr_hwdev_get_by_type(t);
	int rv = NVMEIB_DFLT_MAX_READ_ATOM_ON_WIRE;
	
	NFIN;
	if (dev) {
		if (dev->dev_caps & NVMEIB_DEVCAP_RD_ATOM_64)
			rv = 64;
		else if (dev->dev_caps & NVMEIB_DEVCAP_RD_ATOM_32)
			rv = 32;
		else if (dev->dev_caps & NVMEIB_DEVCAP_RD_ATOM_16)
			rv = 16;
		else if (dev->dev_caps & NVMEIB_DEVCAP_RD_ATOM_8)
			rv = 8;
	} else
		_NW(warn_nvmeib_device_get_max_rd_atom_on_wire, 
			"Querying rdda support of non existing device type (@TOPOLOGY_INT)", t);
	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_device_get_max_rd_atom_on_wire);

/* get qp send_q resources */
int nvmeib_ibdr_get_qp_sqr(struct ib_device *ib_dev, struct ib_qp *ib_qp,
	struct nvmeib_sq_rsc *sqr)
{
	return NVMEIB_DEV_OPS_CALL(ib_dev, get_qp_sqr, ib_dev, ib_qp, sqr);
}
EXPORT_SYMBOL(nvmeib_ibdr_get_qp_sqr);

/* get qp completion queue resources */
int nvmeib_ibdr_get_qp_cqr(struct ib_device *ib_dev, struct ib_cq *ib_cq,
	struct nvmeib_cq_rsc *cqr)
{
	return NVMEIB_DEV_OPS_CALL(ib_dev, get_qp_cqr, ib_dev, ib_cq, cqr);
}
EXPORT_SYMBOL(nvmeib_ibdr_get_qp_cqr);

int nvmeib_ibdr_init_qp(struct ib_device *ib_dev, struct ib_qp *ib_qp,
	u64 wr_id, u32 *opcode, struct nvmeib_sq_rsc *sqr)
{
	return NVMEIB_DEV_OPS_CALL(ib_dev, init_qp, ib_dev, ib_qp, wr_id, opcode, sqr);
}
EXPORT_SYMBOL(nvmeib_ibdr_init_qp);

int nvmeib_ibdr_dump_sq(struct ib_device *ib_dev, struct ib_qp *ib_qp)
{
	return NVMEIB_DEV_OPS_CALL(ib_dev, dump_sq, ib_dev, ib_qp);
}
EXPORT_SYMBOL(nvmeib_ibdr_dump_sq);

ssize_t nvmeib_ibdr_get_qp_usage(struct ib_device *ib_dev, struct ib_qp *ib_qp,
								 enum nvmeib_cnt_mem_type mem_type)
{
	return NVMEIB_DEV_OPS_CALL(ib_dev, get_qp_usage, ib_dev, ib_qp, mem_type);
}
EXPORT_SYMBOL(nvmeib_ibdr_get_qp_usage);

ssize_t nvmeib_ibdr_get_srq_usage(struct ib_device *ib_dev, struct ib_srq *ib_srq,
								 enum nvmeib_cnt_mem_type mem_type)
{
	return NVMEIB_DEV_OPS_CALL(ib_dev, get_srq_usage, ib_dev, ib_srq, mem_type);
}
EXPORT_SYMBOL(nvmeib_ibdr_get_srq_usage);

ssize_t nvmeib_ibdr_get_cq_usage(struct ib_device *ib_dev, struct ib_cq *ib_cq,
								 enum nvmeib_cnt_mem_type mem_type)
{
	return NVMEIB_DEV_OPS_CALL(ib_dev, get_cq_usage, ib_dev, ib_cq, mem_type);
}
EXPORT_SYMBOL(nvmeib_ibdr_get_cq_usage);

ssize_t nvmeib_ibdr_get_mr_usage(struct ib_device *ib_dev, struct ib_mr *ib_mr,
								 enum nvmeib_cnt_mem_type mem_type)
{
	return NVMEIB_DEV_OPS_CALL(ib_dev, get_mr_usage, ib_dev, ib_mr, mem_type);
}
EXPORT_SYMBOL(nvmeib_ibdr_get_mr_usage);

int nvmeib_ibdr_check_rdda_fw(struct ib_device *ib_dev)
{
	return NVMEIB_DEV_OPS_CALL(ib_dev, check_rdda_fw, ib_dev);
}
EXPORT_SYMBOL(nvmeib_ibdr_check_rdda_fw);

int nvmeib_ibdr_max_sq_sz(struct ib_device *ib_dev)
{
	struct nvmeib_hwdev *dev = nvmeib_ibdr_hwdev_get(ib_dev);
	int rv = 0;

	NFIN;
	if (dev)
		rv = dev->max_rdda_sq_sz;
	else
		_NW(warn_nvmeib_ib_driver_nvmeib_ibdr_max_sq_sz, "Querying max rdda send-queue size of unsupported device @IB_DEV_NAME", ib_dev->name);
	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_ibdr_max_sq_sz);

struct nvmeib_shadow_qp {
	void *priv;
	struct nvmeib_device_ops *ops;
	int (*init)(struct nvmeibc_remote_net *rnet, void **priv);
	int (*clear)(struct nvmeibc_remote_net *rnet, void *priv);
	int (*send)(struct nvmeibc_remote_net *rnet, void *priv, struct nvmeib_send_wr *wr);
	int (*free)(struct nvmeibc_remote_net *rnet, void *priv);
	int (*dump_sq)(struct nvmeibc_remote_net *rnet, void *priv);
};

int nvmeib_ibdr_init_remote_qp_shadow(enum nvmeib_dev_type type,
	struct nvmeibc_remote_net *rnet)
{
	struct nvmeib_shadow_qp *sqp;
	int rv = 0;

	NFIN;
	if ((sqp = kzalloc(sizeof(*sqp), GFP_KERNEL))) {
		struct nvmeib_hwdev *dev = nvmeib_ibdr_hwdev_get_by_type(type);
		sqp->ops = nvmeib_ibdr_hwdev_ops_get(dev, __func__);

		if (!IS_ERR_OR_NULL(sqp->ops)) {
			sqp->init = sqp->ops->init_remote_qp_shadow;
			sqp->clear = sqp->ops->clear_remote_qp_shadow;
			sqp->send = sqp->ops->send_remote_qp_shadow;
			sqp->free = sqp->ops->free_remote_qp_shadow;
			sqp->dump_sq = sqp->ops->dump_remote_sq_shadow;

			if ((rv = sqp->init(rnet, &sqp->priv))) {
				nvmeib_ibdr_hwdev_ops_put(sqp->ops, __func__);
				sqp->ops = NULL;
			}
		} else 
			rv = -ENOSYS;
		if (!rv) {
			rnet->priv = sqp;
			rnet->type = type;
		}
		else
			kfree(sqp);
	}
	else
		rv = -ENOMEM;
	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_ibdr_init_remote_qp_shadow);

int nvmeib_ibdr_clear_remote_qp_shadow(struct nvmeibc_remote_net *rnet)
{
	struct nvmeib_shadow_qp *qps = rnet->priv;
	int rv = 0;

	NFIN;
	rv = qps->clear(rnet, qps->priv);
	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_ibdr_clear_remote_qp_shadow);

int nvmeib_ibdr_free_remote_qp_shadow(struct nvmeibc_remote_net *rnet)
{
	struct nvmeib_shadow_qp *qps = rnet->priv;
	int rv = 0;

	NFIN;
	if (!qps || IS_ERR_OR_NULL(qps->ops)) {
		_NT(trace_nvmeib_ib_driver_nvmeib_ibdr_free_remote_qp_shadow, "Invalid ops. qps @QPS qps->ops @OPS_PTR", qps, qps->ops);
		rv = -EINVAL;
		goto out;
	}

	if (qps->free)
		rv = qps->free(rnet, qps->priv);
	nvmeib_ibdr_hwdev_ops_put(qps->ops, __func__);
	qps->ops = NULL;
	kfree(qps);

out:
	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_ibdr_free_remote_qp_shadow);

int nvmeib_ibdr_dump_remote_sq_shadow(struct nvmeibc_remote_net *rnet)
{
	struct nvmeib_shadow_qp *qps = rnet->priv;
	int rv = 0;
	
	NFIN;
	if (!qps)
		rv = -EINVAL;
	else if (!qps->dump_sq)
		rv = -ENOSYS;
	else
		rv = qps->dump_sq(rnet, qps->priv);
	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_ibdr_dump_remote_sq_shadow);

int nvmeib_ibdr_read_remote_qp_shadow(struct nvmeibc_remote_net *rnet,
	struct nvmeib_rdma_iu *ariu, struct nvmeib_iu *iu,
	struct nvmeib_rdma_iu *pbiu, u32 immediate)
{
	struct nvmeib_shadow_qp *qps = rnet->priv;
	struct nvmeib_send_wr *wr;
	struct nvmeib_rdma_iu *riu;
	int o1 = ariu ? 1 : 0;
	int o2 = pbiu ? 1 : 0; /* read-lock piggyback iu */
	int len = o1 + o2 + iu->n_rdma_iu + 1;
	int i, rv;

	NFIN;
	if (!(wr = kzalloc(len * sizeof(*wr), GFP_ATOMIC))) {
		NFOUT;
		return -ENOMEM;
	}

	/* add a possible first riu - (probably disk completion queue */
	if (o1) {
		nvmeib_send_wr_common(wr[0]).opcode = IB_WR_RDMA_WRITE;
		nvmeib_send_wr_rdma(wr[0]).remote_addr = ariu->raddr;
		nvmeib_send_wr_rdma(wr[0]).rkey = ariu->rkey;
		nvmeib_send_wr_common(wr[0]).num_sge = ariu->sge_cnt;
		nvmeib_send_wr_common(wr[0]).sg_list = ariu->sge;
		nvmeib_send_wr_set_next(wr[0], &wr[1]);
	}
	riu = iu->rius;
	if (!o2) {
		for (i = 0; i < iu->n_rdma_iu - 1; ++i, ++riu) {
			nvmeib_send_wr_common(wr[o1 + i]).opcode = IB_WR_RDMA_WRITE;
			nvmeib_send_wr_rdma(wr[o1 + i]).remote_addr = riu->raddr;
			nvmeib_send_wr_rdma(wr[o1 + i]).rkey = riu->rkey;
			nvmeib_send_wr_common(wr[o1 + i]).num_sge = riu->sge_cnt;
			nvmeib_send_wr_common(wr[o1 + i]).sg_list = riu->sge;
			nvmeib_send_wr_set_next(wr[o1 + i], &wr[o1 + i + 1]);
		}
		nvmeib_send_wr_common(wr[o1 + i]).opcode = IB_WR_RDMA_WRITE_WITH_IMM;
		nvmeib_send_wr_ex(wr[o1 + i]).imm_data = immediate;
		nvmeib_send_wr_rdma(wr[o1 + i]).remote_addr = riu->raddr;
		nvmeib_send_wr_rdma(wr[o1 + i]).rkey = riu->rkey;
		nvmeib_send_wr_common(wr[o1 + i]).num_sge = riu->sge_cnt;
		nvmeib_send_wr_common(wr[o1 + i]).sg_list = riu->sge;
		nvmeib_send_wr_clear_next(wr[o1 + i]);
	}
	else {
		for (i = 0; i < iu->n_rdma_iu; ++i, ++riu) {
			nvmeib_send_wr_common(wr[o1 + i]).opcode = IB_WR_RDMA_WRITE;
			nvmeib_send_wr_rdma(wr[o1 + i]).remote_addr = riu->raddr;
			nvmeib_send_wr_rdma(wr[o1 + i]).rkey = riu->rkey;
			nvmeib_send_wr_common(wr[o1 + i]).num_sge = riu->sge_cnt;
			nvmeib_send_wr_common(wr[o1 + i]).sg_list = riu->sge;
			nvmeib_send_wr_set_next(wr[o1 + i], &wr[o1 + i + 1]);
		}
		nvmeib_send_wr_common(wr[o1 + i]).opcode = IB_WR_RDMA_WRITE_WITH_IMM;
		nvmeib_send_wr_ex(wr[o1 + i]).imm_data = immediate;
		nvmeib_send_wr_rdma(wr[o1 + i]).remote_addr = pbiu->raddr;
		nvmeib_send_wr_rdma(wr[o1 + i]).rkey = pbiu->rkey;
		nvmeib_send_wr_common(wr[o1 + i]).num_sge = pbiu->sge_cnt;
		nvmeib_send_wr_common(wr[o1 + i]).sg_list = pbiu->sge;
		nvmeib_send_wr_clear_next(wr[o1 + i]);
	}

	/* nvmeib_mlx5_send_remote_qp_shadow */
	rv = qps->send(rnet, qps->priv, wr);
	kfree(wr);
	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_ibdr_read_remote_qp_shadow);

int nvmeib_ibdr_write_remote_qp_shadow(struct nvmeibc_remote_net *rnet,
	struct nvmeib_rdma_iu *ariu, u32 immediate)
{
	struct nvmeib_shadow_qp *qps = rnet->priv;
	struct nvmeib_send_wr wr;
	int rv;

	NFIN;
	memset(&wr, 0, sizeof(wr));
	if (ariu) {
#ifdef USE_RDMA_POLLING
		nvmeib_send_wr_common(wr).opcode = IB_WR_RDMA_WRITE;
#else
		nvmeib_send_wr_common(wr).opcode= IB_WR_RDMA_WRITE_WITH_IMM;
		nvmeib_send_wr_ex(wr).imm_data = immediate;
#endif
		nvmeib_send_wr_rdma(wr).remote_addr = ariu->raddr;
		nvmeib_send_wr_rdma(wr).rkey = ariu->rkey;
		nvmeib_send_wr_common(wr).num_sge = ariu->sge_cnt;
		nvmeib_send_wr_common(wr).sg_list = ariu->sge;
	}
	else {
		nvmeib_send_wr_common(wr).opcode = IB_WR_SEND_WITH_IMM;
		nvmeib_send_wr_ex(wr).imm_data = immediate;
	}

	/* nvmeib_mlx5_send_remote_qp_shadow */
	rv = qps->send(rnet, qps->priv, &wr);
	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_ibdr_write_remote_qp_shadow);
