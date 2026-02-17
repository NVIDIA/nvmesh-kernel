/*
 * Software iWARP device driver for Linux
 *
 * Authors: Bernard Metzler <bmt@zurich.ibm.com>
 *
 * Copyright (c) 2008-2016, IBM Corporation
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *   Redistribution and use in source and binary forms, with or
 *   without modification, are permitted provided that the following
 *   conditions are met:
 *
 *   - Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 *   - Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 *   - Neither the name of IBM nor the names of its contributors may be
 *     used to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <linux/init.h>
#include <linux/errno.h>
#include <linux/netdevice.h>
#include <linux/inetdevice.h>
#include <net/net_namespace.h>
#include <linux/rtnetlink.h>
#include <linux/if_arp.h>
#include <linux/list.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <uapi/linux/sched/types.h>

#include <rdma/ib_verbs.h>
#include <rdma/ib_smi.h>
#include <rdma/ib_user_verbs.h>

#include <net/tcp.h>
#include <net/addrconf.h>

#include "siw.h"
#include "siw_obj.h"
#include "siw_cm.h"
#include "siw_verbs.h"
#ifdef USE_SQ_KTHREAD
#include <linux/kthread.h>
#endif
#include "siw_str.h"

#if KS_IB_DEVICE_HAS_IWCM
#define __iwcm_op(dev, op) (dev)->iwcm->op
#define __iwcm_data(dev, data) (dev)->iwcm->data
#else
#define __iwcm_op(dev, op) (dev)->ops.iw_ ## op
#define __iwcm_data(dev, data) (dev)->iw_ ## data
#endif

#ifndef KERNEL_VERSION_LT
#define KERNEL_VERSION_LT(a,b,c) (LINUX_VERSION_CODE < KERNEL_VERSION(a,b,c))
#endif
#ifndef KS_SCHED_SETSCHEDULER_EXPORTED
#define KS_SCHED_SETSCHEDULER_EXPORTED KERNEL_VERSION_LT(5,9,0)
#endif

MODULE_AUTHOR("Bernard Metzler");
MODULE_DESCRIPTION("Software iWARP Driver");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_VERSION("0.2");

#define SIW_MAX_IF 12
static char *iface_list[SIW_MAX_IF];
module_param_array(iface_list, charp, NULL, 0444);
MODULE_PARM_DESC(iface_list, "Interface list SIW attaches to if present (array of characters).");

static bool loopback_enabled = 1;
module_param(loopback_enabled, bool, 0644);
MODULE_PARM_DESC(loopback_enabled, "Enable loopback (bool).");

#if SIW_ENABLE_PANIC_REMOTE_ON_RX_ERR
bool panic_remote_on_rx_err = 0;
module_param(panic_remote_on_rx_err, bool, 0644);
MODULE_PARM_DESC(panic_remote_on_rx_err, "Panic remote on RX Error (bool) using TCP OOB.");
#endif

#if SIW_CQ_NOTIFY_WORK_QP_INDEPENDENT
bool cq_notify_tasklet = true;
module_param(cq_notify_tasklet, bool, 0444);
MODULE_PARM_DESC(cq_notify_tasklet, "Use tasklet (instead of WQ) for CQ notify (bool).");
#else
bool cq_notify_tasklet = false;
#endif

LIST_HEAD(siw_devlist);
DECLARE_RWSEM(siw_dev_lock);

#ifdef USE_SQ_KTHREAD
static char tx_cpu_list[1024] = "";
module_param_string(tx_cpu_list,tx_cpu_list, 1024, 0444);
MODULE_PARM_DESC(tx_cpu_list, "List of CPUs siw TX thread shall be bound to (format: comma separated no spaces) (string).");

int default_tx_cpu = -1;
static int tx_on_all_cpus = 1;
extern int siw_run_sq(void *);
struct task_struct *qp_tx_thread[NR_CPUS];
int num_tx_vector = 0;
int qp_tx_vector_cpu[NR_CPUS] = {};
#endif

int *siw_panic_on_warn = NULL;

struct workqueue_struct *notify_wq = NULL;

struct workqueue_struct *net_event_wq = NULL;

struct workqueue_struct *flush_wq = NULL;

struct workqueue_struct *siw_rx_wq = NULL;

#ifdef SIW_DB_SYSCALL
extern long siw_doorbell(u32, u32, u32);
long (*db_orig_call) (u32, u32, u32);
#endif

static ssize_t show_sw_version(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct siw_dev *sdev = container_of(dev, struct siw_dev, ofa_dev.dev);

	return sprintf(buf, "%x\n", sdev->attrs.version);
}

static ssize_t show_if_type(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct siw_dev *sdev = container_of(dev, struct siw_dev, ofa_dev.dev);

	return sprintf(buf, "%d\n", sdev->attrs.iftype);
}

static DEVICE_ATTR(sw_version, S_IRUGO, show_sw_version, NULL);
static DEVICE_ATTR(if_type, S_IRUGO, show_if_type, NULL);

static struct device_attribute *siw_dev_attributes[] = {
	&dev_attr_sw_version,
	&dev_attr_if_type
};

static void siw_device_release(struct device *dev)
{
	dprint(DBG_KEYP, "%s device released\n", dev_name(dev));
}

#if KS_IB_REGISTER_DEVICE_HAS_DEVICE && !KS_HAS_VIRT_DMA_SUPPORT
struct device_dma_parameters siw_dma_parms;
#endif

static struct device siw_generic_dma_device = {
#if KS_DEV_ARCHDATA_HAS_DMA_OPS
	.archdata.rh_reserved_dma_ops	= &siw_dma_generic_ops,
#else
	.dma_ops		= &siw_dma_generic_ops,
#endif

	.init_name		= "software-rdma-v2",
	.release		= siw_device_release,

#if KS_IB_REGISTER_DEVICE_HAS_DEVICE && !KS_HAS_VIRT_DMA_SUPPORT
	.dma_parms 	= &siw_dma_parms,
#endif
};

static struct bus_type siw_bus = {
	.name	= "siw",
};

static int siw_modify_port(struct ib_device *ofa_dev, t_ib_port port, int mask,
			   struct ib_port_modify *props)
{
	return -EOPNOTSUPP;
}

static void siw_device_register(struct siw_dev *sdev)
{
	struct ib_device *ofa_dev = &sdev->ofa_dev;
	int rv, i;
	static int dev_id = 1;

	dprint(DBG_DM, " ofa_dev=" dprint_ptr_str() ", ofa_dev->dev.parent=" dprint_ptr_str() "\n",
		   ofa_dev, ofa_dev->dev.parent);

#if KS_IB_REGISTER_DEVICE_HAS_NAME && KS_IB_REGISTER_DEVICE_HAS_KOBJECT
	rv = ib_register_device(ofa_dev, ofa_dev->name, NULL);
#elif !KS_IB_REGISTER_DEVICE_HAS_NAME && KS_IB_REGISTER_DEVICE_HAS_KOBJECT
	rv = ib_register_device(ofa_dev, NULL);
#elif !KS_IB_REGISTER_DEVICE_HAS_NAME && !KS_IB_REGISTER_DEVICE_HAS_KOBJECT
	rv = ib_register_device(ofa_dev);
#elif KS_HAS_VIRT_DMA_SUPPORT
	ofa_dev->dma_device = NULL;
	rv = ib_register_device(ofa_dev, ofa_dev->name, NULL);
#elif KS_IB_REGISTER_DEVICE_HAS_DEVICE
	rv = ib_register_device(ofa_dev, ofa_dev->name, &siw_generic_dma_device);
#elif KS_IB_REGISTER_DEVICE_HAS_IB_DEVICE && KS_IB_REGISTER_DEVICE_HAS_NAME
	rv = ib_register_device(ofa_dev, ofa_dev->name);
#else
#error unexpected ib_register_device() function signature
#endif
	if (rv) {
		dprint(DBG_DM|DBG_ON, "(dev=%s): "
		       "ib_register_device failed: rv=%d\n", ofa_dev->name, rv);
		return;
	}

	//ib_register_device calls ib_device_register_sysfs which does:
	//struct device *class_dev = &device->dev;
	//class_dev->parent        = device->dma_device; --> parent is NULLified
	dprint(DBG_DM, " ofa_dev=" dprint_ptr_str() ", ofa_dev->dev.parent=" dprint_ptr_str() "\n",
		   ofa_dev, ofa_dev->dev.parent);

#if !KS_HAS_VIRT_DMA_SUPPORT
	// override dma device
	ofa_dev->dma_device = &siw_generic_dma_device;
#endif

	for (i = 0; i < ARRAY_SIZE(siw_dev_attributes); ++i) {
		rv = device_create_file(&ofa_dev->dev, siw_dev_attributes[i]);
		if (rv) {
			dprint(DBG_DM|DBG_ON, "(dev=%s): "
				"device_create_file failed: i=%d, rv=%d\n",
				ofa_dev->name, i, rv);
			ib_unregister_device(ofa_dev);
			return;
		}
	}
	siw_debugfs_add_device(sdev);

	sdev->attrs.vendor_part_id = dev_id++;

	dprint(DBG_DM, ": Registered '%s' for interface '%s', "
		"HWaddr=%02x.%02x.%02x.%02x.%02x.%02x\n",
		ofa_dev->name, sdev->netdev->name,
		*(u8 *)sdev->netdev->dev_addr,
		*((u8 *)sdev->netdev->dev_addr + 1),
		*((u8 *)sdev->netdev->dev_addr + 2),
		*((u8 *)sdev->netdev->dev_addr + 3),
		*((u8 *)sdev->netdev->dev_addr + 4),
		*((u8 *)sdev->netdev->dev_addr + 5));

	sdev->is_registered = 1;

	dprint(DBG_DM, " ofa_dev=" dprint_ptr_str() ", ofa_dev->dev.parent=" dprint_ptr_str() "\n",
		   ofa_dev, ofa_dev->dev.parent);
}

static void siw_device_deregister(struct siw_dev *sdev)
{
	int i;

	siw_debugfs_del_device(sdev);

	if (sdev->is_registered) {

		dprint(DBG_DM, ": deregister %s at %s\n", sdev->ofa_dev.name,
			sdev->netdev->name);

		for (i = 0; i < ARRAY_SIZE(siw_dev_attributes); ++i)
			device_remove_file(&sdev->ofa_dev.dev,
					   siw_dev_attributes[i]);

		ib_unregister_device(&sdev->ofa_dev);
	}

	#define WARN_MEMBER_ATOMIC_NZ(sdev, __atmc) \
	do { \
		int __val = atomic_read(&sdev->__atmc); \
		if (__val) { \
			dprint(DBG_ON, ": %s " #__atmc "=%d sedv=" dprint_ptr_str() "\n", sdev->ofa_dev.name, __val, sdev); \
			WARN_ON(1); \
		} \
	} while (0)

	#define BUG_MEMBER_ATOMIC_NZ(sdev, __atmc) \
	do { \
		int __val = atomic_read(&sdev->__atmc); \
		if (__val) { \
			dprint(DBG_ON, ": %s " #__atmc "=%d sedv=" dprint_ptr_str() "\n", sdev->ofa_dev.name, __val, sdev); \
			BUG(); \
		} \
	} while (0)

#if defined(NVMESH_IS_PRODUCTION_COMPILATION) && (NVMESH_IS_PRODUCTION_COMPILATION==1)
	WARN_MEMBER_ATOMIC_NZ(sdev, num_ctx);
	WARN_MEMBER_ATOMIC_NZ(sdev, num_srq);
	WARN_MEMBER_ATOMIC_NZ(sdev, num_qp);
	WARN_MEMBER_ATOMIC_NZ(sdev, num_cq);
	WARN_MEMBER_ATOMIC_NZ(sdev, num_mem);
	WARN_MEMBER_ATOMIC_NZ(sdev, num_pd);
	WARN_MEMBER_ATOMIC_NZ(sdev, num_cep);
#else
	BUG_MEMBER_ATOMIC_NZ(sdev, num_ctx);
	BUG_MEMBER_ATOMIC_NZ(sdev, num_srq);
	BUG_MEMBER_ATOMIC_NZ(sdev, num_qp);
	BUG_MEMBER_ATOMIC_NZ(sdev, num_cq);
	/* num_mem still a known bug */
	WARN_MEMBER_ATOMIC_NZ(sdev, num_mem);
	BUG_MEMBER_ATOMIC_NZ(sdev, num_pd);
	BUG_MEMBER_ATOMIC_NZ(sdev, num_cep);
#endif

	/* Check that idrs are empty */
	#define WARN_IDR_NOT_EMPTY(sdev, objname) \
	do { \
		struct siw_objhdr *hdr;\
		u32 id;\
		struct idr *idr = &sdev->objname ## _idr;\
		idr_for_each_entry(idr, hdr, id) {\
			struct siw_##objname *obj = container_of(hdr, struct siw_##objname, hdr);\
			dprint(DBG_DM | DBG_ON, #objname "(%d/" dprint_ptr_str() "): not freed (ref %d)\n",\
			id, obj, kref_read(&hdr->ref));\
		}\
	} while(0);

	WARN_IDR_NOT_EMPTY(sdev, qp);
	WARN_IDR_NOT_EMPTY(sdev, cq);
	WARN_IDR_NOT_EMPTY(sdev, pd);
	WARN_IDR_NOT_EMPTY(sdev, mem);

	i = 0;
	while (!list_empty(&sdev->cep_list)) {
		struct siw_cep *cep = list_entry(sdev->cep_list.next,
						 struct siw_cep, devq);
		list_del(&cep->devq);
		siw_cep_printk_log(cep, "Outstanding");
		siw_cep_force_free(cep);
		i++;
	}
	if (i)
		pr_warn("siw_device_deregister: free'd %d CEPs\n", i);

	sdev->is_registered = 0;
}

static void siw_device_destroy(struct siw_dev *sdev)
{
	dprint(DBG_DM, ": destroy siw device at %s\n", sdev->netdev->name);

	siw_idr_release(sdev);
#if KS_IB_DEVICE_HAS_IWCM
	kfree(sdev->ofa_dev.iwcm);
#endif
	dev_put(sdev->netdev);
	ib_dealloc_device(&sdev->ofa_dev);
}


static int siw_match_iflist(struct net_device *dev)
{
	int i = 0, found = *iface_list ? 0 : 1;

	while (iface_list[i]) {
		if (!strcmp(iface_list[i++], dev->name)) {
			found = 1;
			break;
		}
	}
	return found;
}

static struct siw_dev *siw_dev_from_netdev(struct net_device *dev)
{
	if (!list_empty(&siw_devlist)) {
		struct list_head *pos;
		list_for_each(pos, &siw_devlist) {
			struct siw_dev *sdev =
				list_entry(pos, struct siw_dev, list);
			if (sdev->netdev == dev)
				return sdev;
		}
	}
	return NULL;
}

#ifdef USE_SQ_KTHREAD
static int siw_tx_qualified(int cpu)
{
	static char __tx_cpu_list[sizeof(tx_cpu_list) + 1];
	int i = 0;

	if (tx_on_all_cpus)
		return 1;

	scnprintf(__tx_cpu_list, sizeof(__tx_cpu_list), "%s,", tx_cpu_list);

	for (i = 0; i < NR_CPUS; i++) {
		char cpustr[32];
		scnprintf(cpustr, sizeof(cpustr), "%u,", cpu);
		if (!strncmp(cpustr, __tx_cpu_list, strlen(cpustr))) return 1;
		scnprintf(cpustr, sizeof(cpustr), ",%u,", cpu);
		if (strstr(__tx_cpu_list, cpustr)) return 1;
	}
	return 0;
}

static ulong tx_thread_high_prio_bmp = 0;
module_param(tx_thread_high_prio_bmp, ulong, 0644);
MODULE_PARM_DESC(tx_thread_high_prio_bmp, "A bitmap of CPU Tx Threads to set to high priority.");

#ifndef BITS_PER_TYPE
#define BITS_PER_TYPE(type)	(sizeof(type) * BITS_PER_BYTE)
#endif

static bool siw_high_prio_tx_thread(int cpu)
{
	if (cpu < 0 || cpu >= BITS_PER_TYPE(tx_thread_high_prio_bmp))
		return false;
	return !!(tx_thread_high_prio_bmp & (1ULL << cpu));
}

static int siw_create_tx_threads(int max_threads, int check_qualified)
{
	int cpu, rv __attribute__((unused)), assigned = 0;
	if (max_threads < 0 || max_threads > NR_CPUS)
		return 0;

	for_each_online_cpu(cpu) {
		if (check_qualified == 0 || siw_tx_qualified(cpu)) {
			qp_tx_thread[cpu] =
				kthread_create(siw_run_sq,
					(unsigned long *)(long)cpu,
					"qp_tx_thread/%d", cpu);
			kthread_bind(qp_tx_thread[cpu], cpu);
			if (IS_ERR(qp_tx_thread)) {
				rv = PTR_ERR(qp_tx_thread);
				qp_tx_thread[cpu] = NULL;
				dprint(DBG_KEYP, "Binding TX thread to CPU %d failed",
					cpu);
				break;
			}
			if (!siw_low_delay_tx_cpu(cpu) && siw_high_prio_tx_thread(cpu)) {
				/* Set threads to high-priority */
#if KS_SCHED_SETSCHEDULER_EXPORTED
				do {
					struct sched_param param = {
						.sched_priority = 1,
					};
					sched_setscheduler(qp_tx_thread[cpu], SCHED_FIFO, &param);
				} while(0);
#else
				sched_set_fifo_low(qp_tx_thread[cpu]);
#endif
			}
			wake_up_process(qp_tx_thread[cpu]);
			assigned++;
			qp_tx_vector_cpu[num_tx_vector++] = cpu;
			if (default_tx_cpu < 0)
				default_tx_cpu = cpu;
			if (assigned >= max_threads)
				break;
		}
	}
	return assigned;
}
#endif

static int siw_dev_qualified(struct net_device *netdev)
{
	if (!siw_match_iflist(netdev)) {
		dprint(DBG_DM|DBG_ON, ": %s (not selected)\n",
			netdev->name);
		return 0;
	}
	/*
	 * Additional hardware support can be added here
	 * (e.g. ARPHRD_FDDI, ARPHRD_ATM, ...) - see
	 * <linux/if_arp.h> for type identifiers.
	 */
	if (netdev->type == ARPHRD_ETHER ||
	    netdev->type == ARPHRD_IEEE802 ||
	    netdev->type == ARPHRD_INFINIBAND ||
	    (netdev->type == ARPHRD_LOOPBACK && loopback_enabled))
		return 1;

	return 0;
}

static void siw_sq_flush_ofa(struct ib_qp *ofa_qp) {
	 siw_sq_flush(siw_qp_ofa2siw(ofa_qp));
}

static void siw_rq_flush_ofa(struct ib_qp *ofa_qp) {
	 siw_rq_flush(siw_qp_ofa2siw(ofa_qp));
}

#if KS_IB_DEVICE_HAS_DEVICE_OPS == 0
static inline void siw_init_ofa_dev_ops(struct ib_device *ofa_dev)
{
	ofa_dev->query_device = siw_query_device;
	ofa_dev->query_port = siw_query_port;
	ofa_dev->get_netdev = siw_get_netdev;
#if KS_IB_DEVICE_ATTR_HAS_GET_PORT_IMMUTABLE
	ofa_dev->get_port_immutable = siw_get_port_immutable;
#endif
	ofa_dev->query_qp = siw_query_qp;
	ofa_dev->modify_port = siw_modify_port;
	ofa_dev->query_pkey = siw_query_pkey;
	ofa_dev->query_gid = siw_query_gid;
	ofa_dev->alloc_ucontext = siw_alloc_ucontext;
	ofa_dev->dealloc_ucontext = siw_dealloc_ucontext;
	ofa_dev->mmap = siw_mmap;
	ofa_dev->alloc_pd = siw_alloc_pd;
	ofa_dev->dealloc_pd = siw_dealloc_pd;
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 4, 0)
	ofa_dev->create_ah = siw_create_ah;
	ofa_dev->destroy_ah = siw_destroy_ah;
#else
	ofa_dev->create_ah = NULL;
#endif
	ofa_dev->create_qp = siw_create_qp;
	ofa_dev->modify_qp = siw_ofed_modify_qp;
	ofa_dev->destroy_qp = siw_destroy_qp;
	ofa_dev->create_cq = siw_create_cq;
	ofa_dev->destroy_cq = siw_destroy_cq;
	ofa_dev->resize_cq = NULL;
	ofa_dev->poll_cq = siw_poll_cq;
	ofa_dev->get_dma_mr = siw_get_dma_mr;
	ofa_dev->reg_user_mr = siw_reg_user_mr;
	ofa_dev->dereg_mr = siw_dereg_mr;
	ofa_dev->alloc_mr = siw_alloc_mr;
	ofa_dev->map_mr_sg = siw_map_mr_sg;
	ofa_dev->dealloc_mw = NULL;

	ofa_dev->create_srq = siw_create_srq;
	ofa_dev->modify_srq = siw_modify_srq;
	ofa_dev->query_srq = siw_query_srq;
	ofa_dev->destroy_srq = siw_destroy_srq;
	ofa_dev->post_srq_recv = siw_post_srq_recv;

	ofa_dev->attach_mcast = NULL;
	ofa_dev->detach_mcast = NULL;
	ofa_dev->process_mad = siw_no_mad;

	ofa_dev->req_notify_cq = siw_req_notify_cq;
	ofa_dev->post_send = siw_post_send;
	ofa_dev->post_recv = siw_post_receive;

	ofa_dev->drain_sq = siw_sq_flush_ofa;
	ofa_dev->drain_rq = siw_rq_flush_ofa;
}
#else // KS_IB_DEVICE_HAS_DEVICE_OPS
static inline void siw_init_ofa_dev_ops(struct ib_device *ofa_dev)
{
	struct ib_device_ops *ofa_devops = &ofa_dev->ops;
	int xlro = RDMA_DRIVER_SIW_XLRO;

#if KS_IB_DEVICE_OPS_HAS_SIZES
	*ofa_devops = (struct ib_device_ops) {
		INIT_RDMA_OBJ_SIZE(ib_cq, siw_cq, ofa_cq),
		INIT_RDMA_OBJ_SIZE(ib_pd, siw_pd, ofa_pd),
		INIT_RDMA_OBJ_SIZE(ib_srq, siw_srq, ofa_srq),
		INIT_RDMA_OBJ_SIZE(ib_ucontext, siw_ucontext, ib_ucontext),
	#if KS_IB_DEVICE_OPS_HAS_QP_SIZE
		INIT_RDMA_OBJ_SIZE(ib_qp, siw_qp, ofa_qp),
	#endif
	};
#endif
	/* For user-space driver */
	ofa_devops->uverbs_abi_ver = VERSION_ID_SOFTIWARP,
#if KS_IB_DEVICE_OPS_HAS_DRIVER_ID
	// Workaround for assignment of enum outside boundaries
	ofa_devops->driver_id = *(&xlro),
#endif

	ofa_devops->query_device = siw_query_device;
	ofa_devops->query_port = siw_query_port;
	ofa_devops->get_netdev = siw_get_netdev;
#if KS_IB_DEVICE_ATTR_HAS_GET_PORT_IMMUTABLE
	ofa_devops->get_port_immutable = siw_get_port_immutable;
#endif
	ofa_devops->query_qp = siw_query_qp;
	ofa_devops->modify_port = siw_modify_port;
	ofa_devops->query_pkey = siw_query_pkey;
	ofa_devops->query_gid = siw_query_gid;
	ofa_devops->alloc_ucontext = siw_alloc_ucontext;
	ofa_devops->dealloc_ucontext = siw_dealloc_ucontext;
	ofa_devops->mmap = siw_mmap;
	ofa_devops->alloc_pd = siw_alloc_pd;
	ofa_devops->dealloc_pd = siw_dealloc_pd;
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 4, 0)
	ofa_devops->create_ah = siw_create_ah;
	ofa_devops->destroy_ah = siw_destroy_ah;
#else
	ofa_devops->create_ah = NULL;
#endif
	ofa_devops->create_qp = siw_create_qp;
	ofa_devops->modify_qp = siw_ofed_modify_qp;
	ofa_devops->destroy_qp = siw_destroy_qp;
	ofa_devops->create_cq = siw_create_cq;
	ofa_devops->destroy_cq = siw_destroy_cq;
	ofa_devops->resize_cq = NULL;
	ofa_devops->poll_cq = siw_poll_cq;
	ofa_devops->get_dma_mr = siw_get_dma_mr;
	ofa_devops->reg_user_mr = siw_reg_user_mr;
	ofa_devops->dereg_mr = siw_dereg_mr;
	ofa_devops->alloc_mr = siw_alloc_mr;
	ofa_devops->map_mr_sg = siw_map_mr_sg;
	ofa_devops->dealloc_mw = NULL;

	ofa_devops->create_srq = siw_create_srq;
	ofa_devops->modify_srq = siw_modify_srq;
	ofa_devops->query_srq = siw_query_srq;
	ofa_devops->destroy_srq = siw_destroy_srq;
	ofa_devops->post_srq_recv = siw_post_srq_recv;

	ofa_devops->attach_mcast = NULL;
	ofa_devops->detach_mcast = NULL;
	ofa_devops->process_mad = siw_no_mad;

	ofa_devops->req_notify_cq = siw_req_notify_cq;
	ofa_devops->post_send = siw_post_send;
	ofa_devops->post_recv = siw_post_receive;

	ofa_devops->drain_sq = siw_sq_flush_ofa;
	ofa_devops->drain_rq = siw_rq_flush_ofa;
}
#endif // KS_IB_DEVICE_HAS_DEVICE_OPS
static struct siw_dev *siw_device_create(struct net_device *netdev)
{
	struct siw_dev *sdev;
	struct ib_device *ofa_dev;

#if !KS_IB_DEVICE_HAS_DEVICE_OPS
	sdev = (struct siw_dev *)ib_alloc_device(sizeof *sdev);
#else
	sdev = ib_alloc_device(siw_dev, ofa_dev);
#endif
	if (!sdev) {
		__WARN();
		goto out;
	}

	ofa_dev = &sdev->ofa_dev;

#if KS_IB_DEVICE_HAS_IWCM
	ofa_dev->iwcm = kmalloc(sizeof(struct iw_cm_verbs), GFP_KERNEL);
	if (!ofa_dev->iwcm) {
		__WARN();
		ib_dealloc_device(ofa_dev);
		sdev = NULL;
		goto out;
	}
#endif

	sdev->netdev = netdev;
	list_add_tail(&sdev->list, &siw_devlist);

	strcpy(ofa_dev->name, SIW_IBDEV_PREFIX);
	strlcpy(ofa_dev->name + strlen(SIW_IBDEV_PREFIX), netdev->name,
		IB_DEVICE_NAME_MAX - strlen(SIW_IBDEV_PREFIX));

	memset(&ofa_dev->node_guid, 0, sizeof(ofa_dev->node_guid));
	if (netdev->type != ARPHRD_LOOPBACK)
		memcpy(&ofa_dev->node_guid, netdev->dev_addr, 6);
	else {
		/*
		 * The loopback device does not have a HW address,
		 * but connection mangagement lib expects gid != 0
		 */
		size_t gidlen = min(strlen(ofa_dev->name), (size_t)6);
		memcpy(&ofa_dev->node_guid, ofa_dev->name, gidlen);
	}

	/*Shall come before owner assigment*/
	siw_init_ofa_dev_ops(ofa_dev);

#if KS_IB_DEVICE_HAS_OWNER
	ofa_dev->owner = THIS_MODULE;
#else
	ofa_dev->ops.owner = THIS_MODULE;
#endif

#if !KS_IB_DEVICE_HAS_DEVICE_OPS
	/* For user-space driver */
	ofa_dev->uverbs_abi_ver = VERSION_ID_SOFTIWARP;
#if KS_IB_DEVICE_HAS_DRIVER_ID
	ofa_dev->driver_id = RDMA_DRIVER_SIW_XLRO;
#endif
#endif

	ofa_dev->uverbs_cmd_mask =
	    (1ull << IB_USER_VERBS_CMD_GET_CONTEXT) |
	    (1ull << IB_USER_VERBS_CMD_QUERY_DEVICE) |
	    (1ull << IB_USER_VERBS_CMD_QUERY_PORT) |
	    (1ull << IB_USER_VERBS_CMD_ALLOC_PD) |
	    (1ull << IB_USER_VERBS_CMD_DEALLOC_PD) |
	    (1ull << IB_USER_VERBS_CMD_REG_MR) |
	    (1ull << IB_USER_VERBS_CMD_DEREG_MR) |
	    (1ull << IB_USER_VERBS_CMD_CREATE_COMP_CHANNEL) |
	    (1ull << IB_USER_VERBS_CMD_CREATE_CQ) |
	    (1ull << IB_USER_VERBS_CMD_POLL_CQ) |
	    (1ull << IB_USER_VERBS_CMD_REQ_NOTIFY_CQ) |
	    (1ull << IB_USER_VERBS_CMD_DESTROY_CQ) |
	    (1ull << IB_USER_VERBS_CMD_CREATE_QP) |
	    (1ull << IB_USER_VERBS_CMD_QUERY_QP) |
	    (1ull << IB_USER_VERBS_CMD_MODIFY_QP) |
	    (1ull << IB_USER_VERBS_CMD_DESTROY_QP) |
	    (1ull << IB_USER_VERBS_CMD_POST_SEND) |
	    (1ull << IB_USER_VERBS_CMD_POST_RECV) |
	    (1ull << IB_USER_VERBS_CMD_CREATE_SRQ) |
	    (1ull << IB_USER_VERBS_CMD_MODIFY_SRQ) |
	    (1ull << IB_USER_VERBS_CMD_QUERY_SRQ) |
	    (1ull << IB_USER_VERBS_CMD_DESTROY_SRQ) |
	    (1ull << IB_USER_VERBS_CMD_REG_MR) |
	    (1ull << IB_USER_VERBS_CMD_DEREG_MR) |
	    (1ull << IB_USER_VERBS_CMD_POST_SRQ_RECV);

	ofa_dev->node_type = RDMA_NODE_RNIC;
	memcpy(ofa_dev->node_desc, SIW_NODE_DESC_COMMON, sizeof(SIW_NODE_DESC_COMMON));

	/*
	 * Current model (one-to-one device association):
	 * One Softiwarp device per net_device or, equivalently,
	 * per physical port.
	 */
	ofa_dev->phys_port_cnt = 1;

#if KS_IB_SET_NETDEV_HAS_IB_DEVICE
	{
		// struct device *parent = netdev->dev.parent;
		struct device *parent = &netdev->dev;
		int rv;

		// if (!parent) {
		// 	/*
		// 	* The loopback device has no parent device,
		// 	* so it appears as a top-level device. To support
		// 	* loopback device connectivity, take this device
		// 	* as the parent device. Skip all other devices
		// 	* w/o parent device.
		// 	*/
		// 	if (netdev->type != ARPHRD_LOOPBACK) {
		// 		pr_warn("siw: device %s error: no parent device\n",
		// 			netdev->name);
		// 		ib_dealloc_device(ofa_dev);
		// 		return NULL;
		// 	}
		// 	parent = &netdev->dev;
		// }
		ofa_dev->dev.parent = parent;
		dprint(DBG_DM, " ofa_dev=" dprint_ptr_str() ", ofa_dev->dev.parent is " dprint_ptr_str() ", set to parent=" dprint_ptr_str() " &netdev->dev=" dprint_ptr_str() "\n",
			ofa_dev, ofa_dev->dev.parent, parent, &netdev->dev);

		rv = ib_device_set_netdev(ofa_dev, netdev, 1);
		if (rv) {
			pr_warn("ib_device_set_netdev returned error (%d)\n", rv);
			ib_dealloc_device(ofa_dev);
				return NULL;
		}
	}
#else
	dprint(DBG_DM, " ofa_dev=" dprint_ptr_str() ", ofa_dev->dev.parent is " dprint_ptr_str() ", set to &netdev->dev=" dprint_ptr_str() "\n",
		   ofa_dev, ofa_dev->dev.parent, &netdev->dev);
	ofa_dev->dev.parent = &netdev->dev;
#endif

#ifdef USE_SQ_KTHREAD
	ofa_dev->num_comp_vectors = num_tx_vector;
#else
	ofa_dev->num_comp_vectors = num_online_cpus();
#endif

	// ib_register_device expects ofa_dev->dma_device == NULL
	// ofa_dev->dma_device = &siw_generic_dma_device;
	ofa_dev->dma_device = NULL;

#if KS_IB_VERBS_HAS_DMA_MAPPING_OPS
	ofa_dev->dma_ops = &siw_dma_mapping_ops;
#endif

	__iwcm_op(ofa_dev, connect) = siw_connect;
	__iwcm_op(ofa_dev, accept) = siw_accept;
	__iwcm_op(ofa_dev, reject) = siw_reject;
	__iwcm_op(ofa_dev, create_listen) = siw_create_listen;
	__iwcm_op(ofa_dev, destroy_listen) = siw_destroy_listen;
	__iwcm_op(ofa_dev, add_ref) = siw_qp_get_ref;
	__iwcm_op(ofa_dev, rem_ref) = siw_qp_put_ref;
	__iwcm_op(ofa_dev, get_qp) = siw_get_ofaqp;


#if KS_IW_CM_HAS_IFNAME || !KS_IB_DEVICE_HAS_IWCM
	strlcpy(__iwcm_data(ofa_dev, ifname), ofa_dev->name, sizeof(__iwcm_data(ofa_dev, ifname)));
#endif

	/*
	 * set and register sw version + user if type
	 */
	sdev->attrs.version = VERSION_ID_SOFTIWARP;
	sdev->attrs.iftype  = SIW_IF_MAPPED;

	sdev->attrs.vendor_id = SIW_VENDOR_ID;
	sdev->attrs.vendor_part_id = SIW_VENDORT_PART_ID;
	sdev->attrs.sw_version = VERSION_ID_SOFTIWARP;
	sdev->attrs.max_qp = SIW_MAX_QP;
	sdev->attrs.max_qp_wr = SIW_MAX_QP_WR;
	sdev->attrs.max_ord = SIW_MAX_ORD;
	sdev->attrs.max_ird = SIW_MAX_IRD;
	sdev->attrs.cap_flags = IB_DEVICE_MEM_MGT_EXTENSIONS;
	sdev->attrs.max_sge = SIW_MAX_SGE;
	sdev->attrs.max_sge_rd = SIW_MAX_SGE_RD;
	sdev->attrs.max_cq = SIW_MAX_CQ;
	sdev->attrs.max_cqe = SIW_MAX_CQE;
	sdev->attrs.max_mr = SIW_MAX_MR;
	sdev->attrs.max_mr_size = RLIM_INFINITY; // Gregory: do not limit kernel device
	sdev->attrs.max_pd = SIW_MAX_PD;
	sdev->attrs.max_mw = SIW_MAX_MW;
	sdev->attrs.max_fmr = SIW_MAX_FMR;
	sdev->attrs.max_srq = SIW_MAX_SRQ;
	sdev->attrs.max_srq_wr = SIW_MAX_SRQ_WR;
	sdev->attrs.max_srq_sge = SIW_MAX_SGE;

	siw_idr_init(sdev);
	INIT_LIST_HEAD(&sdev->cep_list);
	INIT_LIST_HEAD(&sdev->qp_list);

	atomic_set(&sdev->num_ctx, 0);
	atomic_set(&sdev->num_srq, 0);
	atomic_set(&sdev->num_qp, 0);
	atomic_set(&sdev->num_cq, 0);
	atomic_set(&sdev->num_mem, 0);
	atomic_set(&sdev->num_pd, 0);
	atomic_set(&sdev->num_cep, 0);

	#if KS_HAS_IDR_INIT
		/* This prevent WARN_ON_ONCE on newer kernels */
		idr_init(&sdev->qp_idr);
		idr_init(&sdev->cq_idr);
		idr_init(&sdev->pd_idr);
		idr_init(&sdev->mem_idr);
		idr_init(&sdev->srq_idr);
	#endif

	sdev->is_registered = 0;
out:
	if (sdev)
		dev_hold(netdev);

	return sdev;
}

enum siw_net_event_type {
	NET_EVT = 0,
	INADDR_EVT,
	IN6ADDR_EVT,
};

struct siw_net_event_work {
	struct work_struct work;
	struct net_device *netdev;
	unsigned long net_event;
	unsigned long addr_event;
	enum siw_net_event_type net_event_type;
	union {
		__be32 		ifa_addr;
		struct in6_addr	in6_addr;
	};
};

static void siw_net_event_work(struct work_struct *work);

static int siw_net_event(struct net_device	*netdev, unsigned long net_event, unsigned long addr_event,  
			 enum siw_net_event_type net_event_type, void *net_event_ctx)
{
	struct siw_net_event_work *event_work;

	switch (net_event_type) {
	case NET_EVT:
		dprint(DBG_DM, " (dev=%s): Net Event %lu\n", netdev->name, net_event);
		break;
	case INADDR_EVT:
		dprint(DBG_DM, " (dev=%s): Net Event %lu Addr Event %lu Addr " dprint_ip4_str() "\n",
		       netdev->name, net_event, addr_event, dprint_ip4_param(*(__be32 *)net_event_ctx));
		break;
	case IN6ADDR_EVT:
		dprint(DBG_DM, " (dev=%s): Net Event %lu Addr Event %lu Addr " dprint_ip6_str() "\n",
		       netdev->name, net_event, addr_event, dprint_ip6_param(*(struct in6_addr *)net_event_ctx));
		break;
	default:
		BUG();
	}

	if (!siw_dev_qualified(netdev)) {
		dprint(DBG_DM, "(dev=%s): not supported or enabled for SIW", netdev->name);
		goto done;
	}

	if (dev_net(netdev) != &init_net) {
		dprint(DBG_DM, "(dev=%s): dev_net(netdev) != init_net", netdev->name);
		WARN_ON_ONCE(1);
		goto done;
	}

	if (!down_read_trylock(&siw_dev_lock)) {
		/* The module is being removed */
		dprint(DBG_DM, "(dev=%s): Could not lock siw_dev_lock", netdev->name);
		goto done;
	}

	/* TBD: Switch to pool of event_work */
	if (!(event_work = kzalloc(sizeof(*event_work), GFP_ATOMIC))) {
		dprint(DBG_DM | DBG_ON, " (dev=%s): OOM processing Event %lu\n", netdev->name, net_event);
		WARN_ON_ONCE(1);
		goto unlock;
	}

	dev_hold(netdev);

	INIT_WORK(&event_work->work, siw_net_event_work);
	event_work->netdev = netdev;
	event_work->net_event = net_event;
	event_work->addr_event = addr_event;
	event_work->net_event_type = net_event_type;
	if (net_event_type == INADDR_EVT)
		memcpy(&event_work->ifa_addr, net_event_ctx, sizeof(event_work->ifa_addr));
	else if (net_event_type == IN6ADDR_EVT)
		memcpy(&event_work->in6_addr, net_event_ctx, sizeof(event_work->in6_addr));

	if (!queue_work(net_event_wq, &event_work->work)) {
		dprint(DBG_DM | DBG_ON, 
		       " (dev=%s): Failed to schedule work processing Event %lu\n", 
		       netdev->name, net_event);
		WARN_ON_ONCE(1);
		dev_put(netdev);
		kfree(event_work);
	}
	
unlock:
	up_read(&siw_dev_lock);
done:
	return NOTIFY_OK;
}

static void siw_net_event_work(struct work_struct *work)
{
	struct siw_net_event_work *event_work = container_of(work, struct siw_net_event_work, work);
	struct net_device *netdev = event_work->netdev;
	unsigned long net_event = event_work->net_event;
	struct siw_dev *sdev;

	dprint(DBG_DM, " (dev=%s): Event %lu\n", netdev->name, net_event);

	if (!down_read_trylock(&siw_dev_lock)) {
		/* The module is being removed */
		dprint(DBG_DM, "(dev=%s): Could not lock siw_dev_lock", netdev->name);
		goto done;
	}

	sdev = siw_dev_from_netdev(netdev);
	if (!sdev) {
		dprint(DBG_ON, "OOPS, no sdev for dev=%s (event=%lu)\n",
			   netdev->name, net_event);
		if (false && net_event != NETDEV_UP && net_event != NETDEV_CHANGEADDR)
			goto unlock;
	}
	else {
		dprint(DBG_DM, " (dev=%s): Event %lu, state=%d\n", netdev->name, net_event, sdev->state);
	}

	switch (net_event) {

	case NETDEV_CHANGEADDR:
	case NETDEV_UP:
		{
			struct in_device *in_dev = in_dev_get(netdev);
			struct in_ifaddr *in_ifa_list = in_dev ? in_dev->ifa_list : NULL;
			struct inet6_dev *in6_dev = in6_dev_get(netdev);
			LIST_HEAD(empty_addr_list);
			struct list_head *in6_addr_list = in6_dev ? &in6_dev->addr_list : &empty_addr_list;
			
			if (!in_dev && !in6_dev) {
				dprint(DBG_DM, ": %s: has no IPv4 or IPv6 support\n", netdev->name);
				if (sdev) 
					sdev->state = IB_PORT_INIT;
				break;
			}

			if (!in_dev) {
				dprint(DBG_DM, ": %s: has no IPv4 support\n", netdev->name);
			}

			if (!in6_dev) {
				dprint(DBG_DM, ": %s: has no IPv6 support\n", netdev->name);
			}

			if (in_ifa_list || !list_empty(in6_addr_list)) {
				if (!sdev) {
					if (!siw_dev_qualified(netdev))
						goto dev_put;

					sdev = siw_device_create(netdev);
					if (sdev) {
						sdev->state = IB_PORT_INIT;
						dprint(DBG_DM, ": new siw device for %s\n",
							netdev->name);
					}
				}

				if (!sdev)
					goto dev_put;

				if (sdev->is_registered) {
					if (netdev->flags & IFF_UP)
						/* NETDEV_CHANGEADDR can also happen on down netdev */
						sdev->state = IB_PORT_ACTIVE;
					siw_port_event(sdev, 1, net_event == NETDEV_UP? IB_EVENT_PORT_ACTIVE : IB_EVENT_GID_CHANGE);
					goto dev_put;
				}

				if (in_ifa_list) {
					struct in_ifaddr *in_ifa_list_iter;

					dprint(DBG_DM, ": %s: found ifa-list\n", netdev->name);

					for (in_ifa_list_iter = in_ifa_list; in_ifa_list_iter != NULL; in_ifa_list_iter = in_ifa_list_iter->ifa_next) {
						dprint(DBG_DM, ": %s: ifa-list: ifa_address=" dprint_ip4_str() " ifa_local=" dprint_ip4_str() "\n",
						netdev->name, dprint_ip4_param(in_ifa_list->ifa_address), dprint_ip4_param(in_ifa_list->ifa_local));
					}
				}
				if (!list_empty(in6_addr_list)) {
					struct inet6_ifaddr *ifp;
					list_for_each_entry(ifp, in6_addr_list, if_list) {
						dprint(DBG_DM, ": %s: in6 addr_list: flags=%x addr=%pI6\n", netdev->name, ifp->flags, &ifp->addr);
					}
				}

				sdev->state = IB_PORT_ACTIVE;
				siw_device_register(sdev);
			} else {
				dprint(DBG_DM, ": %s: no ifa_list and empty in6 addr_list\n", netdev->name);
				if (sdev) {
					if (net_event == NETDEV_CHANGEADDR) {
						/* publish zgid event as ip got deleted*/
						siw_port_event(sdev, 1, IB_EVENT_GID_CHANGE);
					}
					sdev->state = IB_PORT_INIT;
				}
			}
dev_put:
			if (in6_dev)
				in6_dev_put(in6_dev);
			if (in_dev)
				in_dev_put(in_dev);
		}
		break;
	case NETDEV_DOWN:
		if (sdev && sdev->is_registered) {
			sdev->state = IB_PORT_DOWN;
			siw_port_event(sdev, 1, IB_EVENT_PORT_ERR);
			break;
		}
		break;

	case NETDEV_REGISTER:
		break;

	case NETDEV_UNREGISTER:
		if (sdev) {
			if (sdev->is_registered)
				siw_device_deregister(sdev);
			list_del(&sdev->list);
			siw_device_destroy(sdev);
		}
		break;
#if 0
	/* Changed to use same code path as NETDEV_UP */
	case NETDEV_CHANGEADDR:
		if (sdev && sdev->is_registered)
			siw_port_event(sdev, 1, IB_EVENT_GID_CHANGE);

		break;
#endif
	/*
	 * Todo: Below netdev events are currently not handled.
	 */
	case NETDEV_CHANGEMTU:
	case NETDEV_GOING_DOWN:
	case NETDEV_CHANGE:
		break;

	default:
		break;
	}

	if (net_event == NETDEV_CHANGEADDR) {
		if (event_work->net_event_type == INADDR_EVT)
			siw_cm_any_listeners_update(event_work->netdev, AF_INET, event_work->ifa_addr, NULL, event_work->addr_event == NETDEV_UP);
		else if (event_work->net_event_type == IN6ADDR_EVT)
			siw_cm_any_listeners_update(event_work->netdev, AF_INET6, 0, &event_work->in6_addr, event_work->addr_event == NETDEV_UP);
	}

unlock:
	up_read(&siw_dev_lock);
done:
	dev_put(event_work->netdev);
	kfree(event_work);
}


static int siw_inetaddr_event(struct notifier_block *nb, unsigned long event,
			    void *arg)
{
	struct net_device	*netdev;
	struct in_ifaddr	*ifa = arg;

	netdev = ifa->ifa_dev->dev;
	/* This is an "address" event and not a device event,
	 so we only get NETDEV_DOWN = Address Delete, and
	 NETDEV_UP = Address Add. Both are translated to NETDEV_CHANGEADDR for siw_net_event
	 * and then to IB_EVENT_GID_CHANGE for ib_dispatch_event */
	if (event != NETDEV_UP && event != NETDEV_DOWN) {
		pr_warn("siw_inetaddr_event - netdev %s unexpected event %lu", netdev->name, event);
		WARN_ON_ONCE(1);
		goto out;
	}
	dprint(DBG_DM, "(dev=%s): Event %lu, Addr " dprint_ip4_str() "\n", 
	       netdev->name, event, dprint_ip4_param(ifa->ifa_address));

	siw_net_event(netdev, NETDEV_CHANGEADDR, event, INADDR_EVT, &ifa->ifa_address);

out:
	return NOTIFY_OK;
}

static int siw_inet6addr_event(struct notifier_block *nb, unsigned long event,
			      void *arg)
{
	struct net_device	*netdev;
	struct inet6_ifaddr	*ifa6 = arg;
	
	netdev = ifa6->idev->dev;
	/* This is an "address" event and not a device event,
	 s o we only get NETDEV_DOWN = Address Delete, *and
	 NETDEV_UP = Address Add. Both are translated to NETDEV_CHANGEADDR for siw_net_event
	 * and then to IB_EVENT_GID_CHANGE for ib_dispatch_event */
	if (event != NETDEV_UP && event != NETDEV_DOWN) {
		pr_warn("siw_inet6addr_event - netdev %s unexpected event %lu", netdev->name, event);
		WARN_ON_ONCE(1);
		goto out;
	}
	dprint(DBG_DM, "(dev=%s): Event %lu, Addr " dprint_ip6_str() "\n", 
	       netdev->name, event, dprint_ip6_param(ifa6->addr));

	siw_net_event(netdev, NETDEV_CHANGEADDR, event, IN6ADDR_EVT, &ifa6->addr);

out:
	return NOTIFY_OK;
}

static int siw_netdev_event(struct notifier_block *nb, unsigned long event,
			    void *arg)
{
#if 0
	struct net_device	*netdev = arg;
#else
	struct net_device	*netdev = netdev_notifier_info_to_dev(arg);
#endif
	return siw_net_event(netdev, event, 0, NET_EVT, NULL);
}

static struct notifier_block siw_inetaddr_nb = {
	.notifier_call = siw_inetaddr_event,
};

static struct notifier_block siw_inet6addr_nb = {
	.notifier_call = siw_inet6addr_event,
};

static struct notifier_block siw_netdev_nb = {
	.notifier_call = siw_netdev_event,
};
#ifdef SIW_DB_SYSCALL
extern long (*doorbell_call)(u32, u32, u32);
#endif

#ifdef SIW_TX_COMP_WAIT_ACK
/* TBD: Optimize this for SIW */
static struct tcp_congestion_ops siw_tcp_cong_ops __read_mostly = {
	//.init = siw_tcp_cong_init,
	.ssthresh = tcp_reno_ssthresh,
	.cong_avoid = tcp_reno_cong_avoid,
#	if KS_HAS_TCP_RENO_UNDO_CWND
	.undo_cwnd = tcp_reno_undo_cwnd,
#	endif
	.in_ack_event = siw_tcp_cong_ack_event,
	//.get_info = siw_tcp_cong_info,
	//.pkts_acked = siw_tcp_cong_pkts_acked,
	
	.owner = THIS_MODULE,
	.name = SIW_TCP_CONG_CTRL_NAME,
};

#endif

#if !NVMESH_IS_PRODUCTION_COMPILATION
#include <linux/kprobes.h>
static void _set_panic_on_warn(void) {
	static struct kprobe kp = {.symbol_name = "panic_on_warn"};

	register_kprobe(&kp);
	siw_panic_on_warn = (int *) kp.addr;
	if (!siw_panic_on_warn) {
		printk(KERN_INFO "Could not find panic_on_warn symbols");
	}
	unregister_kprobe(&kp);
}
#endif

/*
 * siw_init_module - Initialize Softiwarp module and register with netdev
 *                   subsystem to create Softiwarp devices per net_device
 */
static __init int siw_init_module(void)
{
	int rv;
#ifdef USE_SQ_KTHREAD
	int nr_cpu;
#endif

#if !NVMESH_IS_PRODUCTION_COMPILATION
	_set_panic_on_warn();
#endif

	if (SENDPAGE_THRESH < SIW_MAX_INLINE) {
		dprint(DBG_KEYP, "SENDPAGE_THRESH: %d < SIW_MAX_INLINE: %d"
			" -- check SIW_MAX_SGE (%d)\n",
			(int)SENDPAGE_THRESH, (int)SIW_MAX_INLINE,
			(int)SIW_MAX_SGE);
		rv = EINVAL;
		goto out;
	}
	/*
	 * The xprtrdma module needs at least some rudimentary bus to set
	 * some devices path MTU.
	 */
	rv = bus_register(&siw_bus);
	if (rv)
		goto out_nobus;

	siw_generic_dma_device.bus = &siw_bus;
#if KS_IB_REGISTER_DEVICE_HAS_DEVICE && !KS_HAS_VIRT_DMA_SUPPORT
	dma_set_max_seg_size(&siw_generic_dma_device, UINT_MAX);
	dma_coerce_mask_and_coherent(&siw_generic_dma_device, DMA_BIT_MASK(64));
#endif

	rv = device_register(&siw_generic_dma_device);
	if (rv)
		goto out;

#if KS_DEVICE_HAS_DEVICE_RH
	/* device_rh was allocated in device_register -> device_initialize */
	siw_generic_dma_device.device_rh->dma_ops = &siw_dma_generic_ops;
#endif
	
#ifdef SIW_TX_COMP_WAIT_ACK
	rv = tcp_register_congestion_control(&siw_tcp_cong_ops);
	if (rv)
		goto out_unregister;
#endif

	rv = siw_cm_init();
	if (rv)
		goto out_unregister;

	rv = siw_sq_worker_init();
	if (rv)
		goto out_unregister;

	siw_debug_init();
	
#ifdef USE_SQ_KTHREAD
	if (tx_cpu_list[0])
		tx_on_all_cpus = 0;

	if (siw_create_tx_threads(NR_CPUS, 1) == 0) {
		dprint(DBG_KEYP, "Try starting default TX thread\n");
		if (siw_create_tx_threads(1, 0) == 0) {
			dprint(DBG_KEYP, "Could not start any TX thread\n");
			goto out_unregister;
		}
	}
#endif
	if (!(notify_wq = alloc_workqueue("siw_notify_wq", WQ_CPU_INTENSIVE | WQ_HIGHPRI, 0))) {
		dprint(DBG_KEYP, "Could not create notify WQ\n");
		goto out_unregister;
	}

	if (!(net_event_wq = alloc_ordered_workqueue("siw_net_evt_wq", 0))) {
		dprint(DBG_KEYP, "Could not create net event WQ\n");
		goto out_unregister;
	}

	if (!(flush_wq = alloc_workqueue("siw_flush_wq", WQ_UNBOUND, 0))) {
		dprint(DBG_KEYP, "Could not create flush WQ\n");
		goto out_unregister;
	}

	if (!(siw_rx_wq = alloc_workqueue("siw_rx_wq", WQ_HIGHPRI, 0))) {
		dprint(DBG_KEYP, "Could not create RX WQ\n");
		goto out_unregister;
	}

#if !KS_DEVICE_HAS_DEVICE_RH
	rv = register_netdevice_notifier(&siw_netdev_nb);
#else
	rv = register_netdevice_notifier_rh(&siw_netdev_nb);
#endif
	if (rv) {
		siw_debugfs_delete();
		goto out_unregister;
	}
	register_inetaddr_notifier(&siw_inetaddr_nb);
	register_inet6addr_notifier(&siw_inet6addr_nb);
#ifdef SIW_DB_SYSCALL
	db_orig_call = doorbell_call;
	doorbell_call = siw_doorbell;

	dprint(DBG_KEYP, "SoftiWARP: doorbell call assigned, syscall # %d\n",
		__NR_rdma_db);
#else
	dprint(DBG_KEYP, "SoftiWARP: no doorbell call\n");
#endif

	dprint(DBG_KEYP, "SoftiWARP attached\n");
	return 0;

out_unregister:
	if (net_event_wq)
		destroy_workqueue(net_event_wq);
	net_event_wq = NULL;
	if (notify_wq)
		destroy_workqueue(notify_wq);
	notify_wq = NULL;
	if (siw_rx_wq)
		destroy_workqueue(siw_rx_wq);
	siw_rx_wq = NULL;

#ifdef USE_SQ_KTHREAD
	for (nr_cpu = 0; nr_cpu < NR_CPUS; nr_cpu++) {
		if (qp_tx_thread[nr_cpu]) {
			kthread_stop(qp_tx_thread[nr_cpu]);
			qp_tx_thread[nr_cpu] = NULL;
		}
	}
#endif
	device_unregister(&siw_generic_dma_device);

out:
	bus_unregister(&siw_bus);
out_nobus:
	dprint(DBG_KEYP, "SoftIWARP attach failed. Error: %d\n", rv);
	siw_sq_worker_exit();
	siw_cm_exit();

	return rv;
}


static void __exit siw_exit_module(void)
{
#ifdef USE_SQ_KTHREAD
	int nr_cpu;

	for (nr_cpu = 0; nr_cpu < NR_CPUS; nr_cpu++) {
		if (qp_tx_thread[nr_cpu]) {
			kthread_stop(qp_tx_thread[nr_cpu]);
			qp_tx_thread[nr_cpu] = NULL;
		}
	}
#endif

	if (notify_wq)
		destroy_workqueue(notify_wq);

	down_write(&siw_dev_lock);
	unregister_inetaddr_notifier(&siw_inetaddr_nb);
	unregister_inet6addr_notifier(&siw_inet6addr_nb);
#if !KS_DEVICE_HAS_DEVICE_RH
	unregister_netdevice_notifier(&siw_netdev_nb);
#else
	unregister_netdevice_notifier_rh(&siw_netdev_nb);
#endif

	if (net_event_wq)
		destroy_workqueue(net_event_wq);

	if (flush_wq)
		destroy_workqueue(flush_wq);

	if (siw_rx_wq)
		destroy_workqueue(siw_rx_wq);

	up_write(&siw_dev_lock);

	siw_sq_worker_exit();
	siw_cm_exit();
	
#ifdef SIW_TX_COMP_WAIT_ACK
	tcp_unregister_congestion_control(&siw_tcp_cong_ops);
#endif

#ifdef SIW_DB_SYSCALL
	doorbell_call = db_orig_call;
#endif

	while (!list_empty(&siw_devlist)) {
		struct siw_dev  *sdev =
			list_entry(siw_devlist.next, struct siw_dev, list);
		list_del(&sdev->list);
		if (sdev->is_registered)
			siw_device_deregister(sdev);

		siw_device_destroy(sdev);
	}
	siw_debugfs_delete();

	device_unregister(&siw_generic_dma_device);

	bus_unregister(&siw_bus);

	dprint(DBG_KEYP, "SoftiWARP detached\n");
}

module_init(siw_init_module);
module_exit(siw_exit_module);
