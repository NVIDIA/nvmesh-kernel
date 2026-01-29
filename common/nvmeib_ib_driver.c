/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeib_ib_driver.h"
#include "nvmeib_public.h"
#include "mlx/nvmeib_mlx.h"
#include "siw/nvmeib_siw.h"
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
int nvmeib_ibdr_hwdev_pops_set(enum nvmeib_dev_type t,
                               struct module *m,
                               struct nvmeib_device_public_ops *pops)
#else
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
		    "Failed to get reference count for @MODULE_NAME", m);
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

