/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/string.h>
#include <linux/version.h>
#include <linux/vmalloc.h>
#include <linux/moduleparam.h>
#include <linux/atomic.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/parser.h>
#include <linux/dma-direction.h>
#include <linux/err.h>
#include <linux/random.h>
#include <linux/bug.h>
#include <linux/rwsem.h>

#include <linux/delay.h>

#include "../../common/ib_incs.h"

#define __FILE_LITERAL__ atom_test.c
/* We need to use the workaround for the mlx5 atomics */
#include "../../common_public/nvmeib_public.h"
#include "rdma/ib_verbs.h"
#if !HAS_IB_QUERY_GID
#include "rdma/ib_cache.h"
#endif

#define DEBUG_LEVEL 2

#define trace_inf(x, y, fmt, ...) do { pr_info("(%d/%d)" x "[%s](%d): " fmt, current->pid, raw_smp_processor_id(), y, __FUNCTION__, __LINE__, ## __VA_ARGS__); } while (0)
#define trace_wrn(x, y, fmt, ...) do { pr_warn("(%d/%d)" x "[%s](%d): " fmt, current->pid, raw_smp_processor_id(), y, __FUNCTION__, __LINE__, ## __VA_ARGS__); } while (0)
#define trace_err(x, y, fmt, ...) do { pr_err("(%d/%d)" x "[%s](%d): " fmt, current->pid, raw_smp_processor_id(), y, __FUNCTION__, __LINE__, ## __VA_ARGS__); } while (0)
#define trace_dbg(x, y, fmt, ...) do { printk("(%d/%d)" x "[%s](%d): " fmt, current->pid, raw_smp_processor_id(), y, __FUNCTION__, __LINE__, ## __VA_ARGS__); } while (0)

/* as those are already used by tracer will undef them */
#undef _I
#undef _W
#undef _E
#undef _D
#undef FIN
#undef FOUT
#undef LINE

#define FILENAME kbasename(__FILE__)
#define _I(fmt, ...) trace_inf("%s", FILENAME, fmt, ## __VA_ARGS__)
#define _W(fmt, ...) trace_wrn("%s", FILENAME, fmt, ## __VA_ARGS__)
#define _E(fmt, ...) trace_err("%s", FILENAME, fmt, ## __VA_ARGS__)
#define _D(fmt, ...) if (!DEBUG_LEVEL); else if (DEBUG_LEVEL == 1) trace_dbg("%s", FILENAME, fmt, ## __VA_ARGS__); else trace_inf("%s", FILENAME, fmt, ## __VA_ARGS__)

#define FIN _D("-->\n")
#define FOUT _D("<--\n")
#define LINE _D("---\n")
#define IFIN _I("-->\n")
#define IFOUT _I("<--\n")

MODULE_AUTHOR("NVIDIA CORPORATION");
MODULE_DESCRIPTION("Tests if RDMA Atomic operations use PCIE Atomics");
MODULE_LICENSE("GPL and additional rights");

#define NUM_OPS 16

struct atom_test_data {
	/* Data for Atomic Counter */
	struct page *page;
	atomic_t *atom_ctr;
	/* Data for runtime */
	int run_secs;
	unsigned long start_jiffies;
	unsigned long end_jiffies;
	bool running;
};

struct atom_loop_con {
	struct ib_qp *active_qp, *passive_qp;
	struct ib_cq *cq;
	u8 port_num;
	u16 port_lid;
	enum rdma_link_layer port_link_layer;
	struct nvmeib_send_wr wr[NUM_OPS];
	struct ib_wc wc;
	struct ib_sge sg[NUM_OPS];
};

struct atom_dev {
	struct ib_device *dev;
	struct ib_pd *pd;
	struct ib_mr *mr;
	struct list_head link;
	int selected_port;
	struct atom_loop_con loop_con;
	int op_ctr;
	struct task_struct *thread;
	struct nvmeib_alloc_n_map atom_ctr_map;
	void *sg_buf_virt;
	u64 sg_buf_dma_addr;
};

struct rdma_dev {
	struct ib_device *dev;
	struct list_head link;
};

static LIST_HEAD(dev_list);
static DEFINE_MUTEX(dev_list_guard);

static struct atom_test_data test;
static struct atom_dev devs[2];
DECLARE_RWSEM(run_sem);

#if KS_IB_CLIENT_ADD_RV_IS_INT
static int add_one(struct ib_device *device);
#else
static void add_one(struct ib_device *device);
#endif

#if IB_REMOVE_EXTRA_ARG
static void remove_one(struct ib_device *device, void *);
#else
static void remove_one(struct ib_device *device);
#endif

static struct ib_client client = {
	.name   = "atom_test_ib",
	.add    = add_one,
	.remove = remove_one
};

static ssize_t dev_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf);
static ssize_t dev_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count);
static ssize_t run_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf);
static ssize_t run_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count);

static struct kobject *atom_test_kobj;
static struct kobj_attribute dev1_attr = __ATTR(dev1, 0660, dev_show, dev_store);
static struct kobj_attribute dev2_attr = __ATTR(dev2, 0660, dev_show, dev_store);
static struct kobj_attribute run_attr = __ATTR(run, 0660, run_show, run_store);

static void cq_handler(struct ib_cq *cq, void *ctx)
{
	struct atom_dev *dev = ctx;
	wake_up_process(dev->thread);
}

static void cq_event_handler(struct ib_event *event, void *ctx)
{
	struct atom_dev *dev = ctx;
	_I("Got CQ Event %d on Device %s\n", event->event, dev->dev->name);
	wake_up_process(dev->thread);
}

static int create_qps(struct atom_dev *dev)
{
	struct ib_device *ib_dev = dev->dev;
	struct atom_loop_con *con = &dev->loop_con;
	struct ib_qp_init_attr qp_init_attr = {0};
	struct ib_qp_attr qp_attr = {0};
	int rv = 0;

	/* Create and CQ and QPs */

	con->cq = nvmeib_create_cq(ib_dev, cq_handler, cq_event_handler, dev, NUM_OPS, 0);
	if (IS_ERR(con->cq)) {
		rv = PTR_ERR(con->cq);
		_E("nvmeib_create_cq failed for %s (%d)\n", ib_dev->name, rv);
		goto out;
	}
	ib_req_notify_cq(con->cq, IB_CQ_NEXT_COMP);
	_D("Created CQ %p\n", con->cq);

	qp_init_attr.send_cq = con->cq;
	qp_init_attr.recv_cq = con->cq;
	qp_init_attr.cap.max_send_wr = NUM_OPS;
	qp_init_attr.cap.max_send_sge = 1;
	qp_init_attr.cap.max_recv_wr = 1;
	qp_init_attr.cap.max_recv_sge = 1;
	qp_init_attr.sq_sig_type = IB_SIGNAL_REQ_WR;
	qp_init_attr.qp_type = IB_QPT_RC;

	con->active_qp = ib_create_qp(dev->pd, &qp_init_attr);
	if (IS_ERR(con->active_qp)) {
		rv = PTR_ERR(con->active_qp);
		_E("ib_create_qp failed for %s (%d)\n", ib_dev->name, rv);
		goto free_cq;
	}
	_D("Created Active QP %x\n", con->active_qp->qp_num);

	qp_init_attr.cap.max_send_wr = 1;
	con->passive_qp = ib_create_qp(dev->pd, &qp_init_attr);
	if (IS_ERR(con->passive_qp)) {
		rv = PTR_ERR(con->passive_qp);
		_E("ib_create_qp failed for %s (%d)\n", ib_dev->name, rv);
		goto free_active_qp;
	}
	_D("Created Passive QP %x\n", con->passive_qp->qp_num);

	/* Init QPs */
	qp_attr.qp_state = IB_QPS_INIT;
	qp_attr.port_num = con->port_num;
	qp_attr.qp_access_flags = IB_ACCESS_LOCAL_WRITE | IB_ACCESS_REMOTE_ATOMIC | IB_ACCESS_REMOTE_WRITE;

	if ((rv = ib_modify_qp(con->active_qp, &qp_attr,
			  IB_QP_STATE              |
			  IB_QP_PKEY_INDEX         |
			  IB_QP_PORT               |
			  IB_QP_ACCESS_FLAGS)) < 0) {
		_E("Error %d modifying Active QP to Init\n", rv);
		goto out;
	}

	_D("Modified Active QP %x to Init State\n", con->active_qp->qp_num);

	if ((rv = ib_modify_qp(con->passive_qp, &qp_attr,
			  IB_QP_STATE              |
			  IB_QP_PKEY_INDEX         |
			  IB_QP_PORT               |
			  IB_QP_ACCESS_FLAGS)) < 0) {
		_E("Error %d modifying Passive QP to Init\n", rv);
		goto out;
	}

	_D("Modified Passive QP %x to Init State\n", con->passive_qp->qp_num);

	goto out;

free_active_qp:
	ib_destroy_qp(con->active_qp);
	con->active_qp = NULL;


free_cq:
	ib_destroy_cq(con->cq);
	con->cq = NULL;

out:
	return rv;
}

static int connect_qps(struct atom_dev *dev)
{
	int rv = 1;
	struct atom_loop_con *loop_qp = &dev->loop_con;

	struct ib_qp_attr qp_connect_attr = {
		.qp_state               = IB_QPS_RTR,
		.path_mtu               = IB_MTU_512, /* Don't Care as long as it connects */
		.rq_psn                 = 0,
		.sq_psn			= 0,
		.dest_qp_num            = loop_qp->passive_qp->qp_num,
		.ah_attr                = {
			/*.is_global      = 0,*/
			.ah_flags	= 0,
	//		.dlid           = loop_qp->port_lid,
			.sl             = 0,
	//		.src_path_bits  = 0,
			.port_num       = loop_qp->port_num,
		},
		.max_rd_atomic		= NUM_OPS,
		.max_dest_rd_atomic     = NUM_OPS,
		.min_rnr_timer          = 12,
		.port_num		= loop_qp->port_num,
		.timeout		= 14,
		.retry_cnt		= 7,
		.rnr_retry		= 7,
	};

	NVMEIB_AH_DLID(qp_connect_attr.ah_attr) = loop_qp->port_lid;
	rdma_ah_set_path_bits(&qp_connect_attr.ah_attr, 0);

	FIN;

	if (loop_qp->port_link_layer == IB_LINK_LAYER_ETHERNET) {
		qp_connect_attr.ah_attr.grh.sgid_index = 0;
		#if IB_HAS_RDMA_AH_ATTR_TYPE
			qp_connect_attr.ah_attr.type = RDMA_AH_ATTR_TYPE_ROCE;
			if(!(dev->dev->get_netdev)) {
				_E("get_netdev is null, can't fill mac");
				goto out;
			}
			memcpy(qp_connect_attr.ah_attr.roce.dmac,
			 dev->dev->get_netdev(dev->dev, loop_qp->port_num)->dev_addr, ETH_ALEN);
		#endif
		/* For RoCE, we need to fill GRH part of AH (SGID Index and DGID) */
		qp_connect_attr.ah_attr.ah_flags = IB_AH_GRH;
		#if HAS_IB_QUERY_GID
		#if IB_HAS_GID_ATTR
			rv = ib_query_gid(dev->dev, loop_qp->port_num, qp_connect_attr.ah_attr.grh.sgid_index,
				&qp_connect_attr.ah_attr.grh.dgid, NULL);
		#else
			rv = ib_query_gid(dev->dev, loop_qp->port_num, qp_connect_attr.ah_attr.grh.sgid_index,
				&qp_connect_attr.ah_attr.grh.dgid);
		#endif
		#else
			rv = rdma_query_gid(dev->dev, loop_qp->port_num, qp_connect_attr.ah_attr.grh.sgid_index,
							&qp_connect_attr.ah_attr.grh.dgid);
		#endif

		if (rv) {
			_E("ib_query_gid failed with error %d\n", rv);
			goto out;
		}

		_D("Got DGID %pI6 for SGID Index %d\n", &qp_connect_attr.ah_attr.grh.dgid.raw,
			qp_connect_attr.ah_attr.grh.sgid_index);
	} else {
	#if IB_HAS_RDMA_AH_ATTR_TYPE
		qp_connect_attr.ah_attr.type = RDMA_AH_ATTR_TYPE_IB;
	#endif
	}

	if ((rv = ib_modify_qp(loop_qp->active_qp, &qp_connect_attr,
			  IB_QP_STATE              |
			  IB_QP_AV                 |
			  IB_QP_PATH_MTU           |
			  IB_QP_DEST_QPN           |
			  IB_QP_RQ_PSN             |
			  IB_QP_MAX_DEST_RD_ATOMIC |
			  IB_QP_MIN_RNR_TIMER)) < 0) {
		_E("Error %d modifying Active QP to RTR\n", rv);
		goto out;
	}

	_D("Modified Active QP %x to RTR\n", loop_qp->active_qp->qp_num);

	qp_connect_attr.dest_qp_num = loop_qp->active_qp->qp_num;

	if ((rv = ib_modify_qp(loop_qp->passive_qp, &qp_connect_attr,
			  IB_QP_STATE              |
			  IB_QP_AV                 |
			  IB_QP_PATH_MTU           |
			  IB_QP_DEST_QPN           |
			  IB_QP_RQ_PSN             |
			  IB_QP_MAX_DEST_RD_ATOMIC |
			  IB_QP_MIN_RNR_TIMER)) < 0) {
		_E("Error %d modifying Passive QP to RTR\n", rv);
		goto out;
	}

	_D("Modified Passive QP %x to RTR\n", loop_qp->passive_qp->qp_num);

	qp_connect_attr.qp_state = IB_QPS_RTS;

	if ((rv = ib_modify_qp(loop_qp->active_qp, &qp_connect_attr,
			       IB_QP_STATE              |
			       IB_QP_TIMEOUT            |
			       IB_QP_RETRY_CNT          |
			       IB_QP_RNR_RETRY          |
			       IB_QP_SQ_PSN             |
			       IB_QP_MAX_QP_RD_ATOMIC)) < 0) {
		_E("Error %d modifying Active QP to RTS\n", rv);
		goto out;
	}

	_D("Modified Active QP %x to RTS\n", loop_qp->active_qp->qp_num);

	if ((rv = ib_modify_qp(loop_qp->passive_qp, &qp_connect_attr,
			       IB_QP_STATE              |
			       IB_QP_TIMEOUT            |
			       IB_QP_RETRY_CNT          |
			       IB_QP_RNR_RETRY          |
			       IB_QP_SQ_PSN             |
			       IB_QP_MAX_QP_RD_ATOMIC)) < 0) {
		_E("Error %d modifying Passive QP to RTS\n", rv);
	}

	_D("Modified Passive QP %x to RTS\n", loop_qp->passive_qp->qp_num);

out:
	FOUT;
	return rv;
}

void dev_cleanup(struct atom_dev *dev)
{
	if (dev->loop_con.active_qp)
		ib_destroy_qp(dev->loop_con.active_qp);
	if (dev->loop_con.passive_qp)
		ib_destroy_qp(dev->loop_con.passive_qp);
	if (dev->loop_con.cq)
		ib_destroy_cq(dev->loop_con.cq);
	if (dev->atom_ctr_map.mr) {
		nvmeib_mem_unmapn_n_free(&dev->atom_ctr_map);
	}
	if (dev->sg_buf_virt) {
		ib_dma_free_coherent(dev->dev, sizeof(u64) * NUM_OPS,
				     dev->sg_buf_virt, dev->sg_buf_dma_addr);
		dev->sg_buf_virt = NULL;
	}
#if HAS_IB_GET_DMA_MR
	if (dev->mr)
		ib_dereg_mr(dev->mr);
#endif
	if (dev->pd)
		ib_dealloc_pd(dev->pd);

	dev->loop_con.active_qp = NULL;
	dev->loop_con.passive_qp = NULL;
	dev->loop_con.cq = NULL;
	dev->mr = NULL;
	dev->pd = NULL;
	dev->dev = NULL;
}

int dev_init(struct atom_dev *dev, struct ib_device *ib_dev, int port)
{
	int rv = 0;
	struct ib_port_attr port_attr;
	int selected_port = -1;
	int i = 0;

	_D("Init device %s\n", ib_dev->name);
	/* Check port is valid or find first valid port */
	dev->dev = ib_dev;
	if (port >= 0) {
		_D("Port specified %d\n", port);
		if ((rv = ib_query_port(ib_dev, port, &port_attr))) {
			_E("Invalid port %d for device %s\n", port, ib_dev->name);
			goto err;
		}
		if (port_attr.state != IB_PORT_ACTIVE) {
			_E("Port %d of device %s is inactive\n", port, ib_dev->name);
			goto err;
		}
		selected_port = port;
	} else {
		for (port = 0; port < 256; port++) {
			_D("Searching port %d\n", port);
			if ((rv = ib_query_port(ib_dev, port, &port_attr))) {
				_D("Invalid port %d for device %s\n", port, ib_dev->name);
				continue;
			}
			if (port_attr.state == IB_PORT_ACTIVE) {
				_I("Using active port %d of device %s\n", port, ib_dev->name);
				selected_port = port;
				break;
			}
			_D("Port %d of device %s inactive\n", i, ib_dev->name);
		}
	}

	if (selected_port < 0) {
		_E("Didn't find an active valid port of device %s\n", ib_dev->name);
		rv = -ENOENT;
		goto err;
	}

	dev->loop_con.port_num = selected_port;
	dev->loop_con.port_lid = port_attr.lid;
	dev->loop_con.port_link_layer = rdma_port_get_link_layer(ib_dev, selected_port);
	dev->pd = ib_alloc_pd(ib_dev);
	if (IS_ERR(dev->pd)) {
		rv = PTR_ERR(dev->pd);
		_E("ib_alloc_pd failed for %s (%d)\n", ib_dev->name, rv);
		goto err;
	}

#if HAS_IB_GET_DMA_MR
	dev->mr = ib_get_dma_mr(dev->pd, IB_ACCESS_LOCAL_WRITE |
					IB_ACCESS_REMOTE_READ |
					IB_ACCESS_REMOTE_WRITE |
					IB_ACCESS_REMOTE_ATOMIC);
	if (IS_ERR(dev->mr)) {
		rv = PTR_ERR(dev->mr);
		_E("ib_get_dma_mr failed for %s (%d)\n", ib_dev->name, rv);
		goto err;
	}
#endif

	/* Allocate memory for Atomic Op Return value */
	if (!(dev->sg_buf_virt = ib_dma_alloc_coherent(dev->dev, sizeof(u64) * NUM_OPS, &dev->sg_buf_dma_addr, GFP_KERNEL))) {
		_E("DMA Memory allocation error\n");
		goto err;
	}

	dev->atom_ctr_map.pd = dev->pd;
	dev->atom_ctr_map.n_pages = 1;
	dev->atom_ctr_map.pages = &test.page;
	dev->atom_ctr_map.ioaddr = 0;
	dev->atom_ctr_map.access_flags = IB_ACCESS_LOCAL_WRITE | IB_ACCESS_REMOTE_READ |
		IB_ACCESS_REMOTE_WRITE | IB_ACCESS_REMOTE_ATOMIC;

	if ((rv = nvmeib_mem_alloc_n_map(&dev->atom_ctr_map))) {
		_E("nvmeib_mem_alloc_n_map failed (%d)\n", rv);
		goto err;
	}

	/* Create QPs */
	rv = create_qps(dev);
	if (rv)
		goto err;

	/* Connect QPs */
	rv = connect_qps(dev);
	if (rv)
		goto err;

	/* Register MRs */


	/* Init WRs */
	for (i = 0; i < NUM_OPS; i++) {
		nvmeib_send_wr_common(dev->loop_con.wr[i]).opcode = IB_WR_ATOMIC_FETCH_AND_ADD;
		nvmeib_send_wr_common(dev->loop_con.wr[i]).wr_id = i;
		if (i < NUM_OPS - 1)
			nvmeib_send_wr_set_next(dev->loop_con.wr[i], &dev->loop_con.wr[i + 1]);
		else {
			nvmeib_send_wr_clear_next(dev->loop_con.wr[i]);
			nvmeib_send_wr_common(dev->loop_con.wr[i]).send_flags = IB_SEND_SIGNALED;
		}
		nvmeib_send_wr_common(dev->loop_con.wr[i]).num_sge = 1;
		nvmeib_send_wr_common(dev->loop_con.wr[i]).sg_list = &dev->loop_con.sg[i];

		nvmeib_send_wr_atomic(dev->loop_con.wr[i]).remote_addr = dev->atom_ctr_map.ioaddr;
		nvmeib_send_wr_atomic(dev->loop_con.wr[i]).rkey = dev->atom_ctr_map.rkey;
		nvmeib_send_wr_atomic(dev->loop_con.wr[i]).compare_add = 1;

		/* The Atomic Op stores the original value in the SGEs */
		dev->loop_con.sg[i].addr = dev->sg_buf_dma_addr + i * sizeof(u64);
		dev->loop_con.sg[i].length = sizeof(u64);
		dev->loop_con.sg[i].lkey = nvmeib_get_lkey(dev);

		_D("WR[%d] op %d wr_id %llu flg %d next %p raddr %llx rkey %x add %llu\n",
		   i, nvmeib_send_wr_common(dev->loop_con.wr[i]).opcode,
		   nvmeib_send_wr_common(dev->loop_con.wr[i]).wr_id,
		   nvmeib_send_wr_common(dev->loop_con.wr[i]).send_flags,
		   nvmeib_send_wr_common(dev->loop_con.wr[i]).next,
		   nvmeib_send_wr_atomic(dev->loop_con.wr[i]).remote_addr,
		   nvmeib_send_wr_atomic(dev->loop_con.wr[i]).rkey,
		   nvmeib_send_wr_atomic(dev->loop_con.wr[i]).compare_add);
	}

	goto out;

err:
	dev_cleanup(dev);
out:
	return rv;
}

static ssize_t dev_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	ssize_t rv = 0;
	struct atom_dev *dev = NULL;

	FIN;
	if (attr == &dev1_attr) {
		dev = &devs[0];
	}
	else if (attr == &dev2_attr) {
		dev = &devs[1];
	}
	else {
		_E("Invalid attribute pointer %p\n", attr);
		rv = -EINVAL;
		goto out;
	}

	if (dev->dev)
		rv = sprintf(buf, "%s:%d\n", dev->dev->name, dev->loop_con.port_num);
	else
		rv = sprintf(buf, "cpu\n");

out:
	FOUT;
	return rv;
}

static ssize_t dev_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	ssize_t rv = -ENODEV;
	char *colptr = NULL;
	char dev_name[IB_DEVICE_NAME_MAX];
	struct atom_dev *dev;
	struct rdma_dev *d_itr = NULL;
	int port = -1;

	FIN;
	if (attr == &dev1_attr) {
		dev = &devs[0];
	}
	else if (attr == &dev2_attr) {
		dev = &devs[1];
	}
	else {
		_E("Invalid attribute pointer %p\n", attr);
		rv = -EINVAL;
		goto out;
	}

	if (dev->dev)
		dev_cleanup(dev);

	/* Check if they are requesting the CPU device */
	if (strncmp(buf, "cpu", count) == 0) {
		_I("Using CPU device\n");
		goto out;
	}

	if ((colptr = strstr(buf, ":"))) {
		/* Specifies the port as well */
		int max_str_len = min((int)(colptr - buf), (int)count);
		strncpy(dev_name, buf, max_str_len);
		dev_name[max_str_len] = 0;
		if (kstrtoint(colptr + 1, 10, &port) != 0) {
			_E("Failed to parse device string %s\n", buf);
			rv = -EINVAL;
			goto out;
		}
	} else if ((colptr = strstr(buf, "\n")))  {
		/* Remove the EOL character */
		int max_str_len = min((int)(colptr - buf), (int)count);
		strncpy(dev_name, buf, max_str_len);
		dev_name[max_str_len] = 0;
	} else {
		int max_str_len = min((int)IB_DEVICE_NAME_MAX, (int)count);
		strncpy(dev_name, buf, max_str_len);
		dev_name[max_str_len] = 0;
		_D("Looking for device %s\n", dev_name);
	}

	/* Look for device in list */
	mutex_lock(&dev_list_guard);
	list_for_each_entry(d_itr, &dev_list, link) {
		_D("Device in list %s\n", d_itr->dev->name);
		if (strncmp(d_itr->dev->name, dev_name, IB_DEVICE_NAME_MAX) == 0) {
			/* Found it! */
			rv = dev_init(dev, d_itr->dev, port);
			if (!rv)
				rv = count;
			break;
		}
	}
	mutex_unlock(&dev_list_guard);

out:
	FOUT;
	return rv;
}

static void end_thread_fn(void)
{
	if (down_write_trylock(&run_sem)) {
		int atom_ctr_val = atomic_read(test.atom_ctr);
		bool atom_fail = devs[0].op_ctr + devs[1].op_ctr != atom_ctr_val;

		test.end_jiffies = jiffies;
		test.run_secs = (test.end_jiffies - test.start_jiffies) / HZ;
		test.running = false;

		_I("Test finished - Runtime: %d secs Count 1: %d Count 2: %d Total Count: %d Atomic Failure: %s\n",
			test.run_secs, devs[0].op_ctr, devs[1].op_ctr, atom_ctr_val, (atom_fail ? "TRUE" : "FALSE"));
		up_write(&run_sem);
	}
}

static int dev_thread_fn(void *data)
{
	struct atom_dev *dev = data;
	struct nvmeib_send_wr *bad_wr;
	int rv = 0;
	down_read(&run_sem);
	dev->op_ctr = 0;

	while (!kthread_should_stop() && jiffies < test.end_jiffies) {
		if ((rv = nvmeib_post_send_atomic(dev->loop_con.active_qp,
				  &dev->loop_con.wr[0], &bad_wr))) {
			_E("ib_post_send failed (%d)\n", rv);
			break;
		}
		do {
			schedule();
			rv = ib_poll_cq(dev->loop_con.cq, 1, &dev->loop_con.wc);
		} while (rv == 0 && !kthread_should_stop());
		if (rv < 0) {
			_E("ib_poll_cq failed (%d)\n", rv);
			break;
		}
		dev->op_ctr += NUM_OPS;
		if (dev->loop_con.wc.status != IB_WC_SUCCESS) {
			_E("Send WR failed with status %d and vendor error %d\n", dev->loop_con.wc.status, dev->loop_con.wc.vendor_err);
			rv = dev->loop_con.wc.status;
			break;
		}
	}
	up_read(&run_sem);
	end_thread_fn();
	return rv;
}

static int cpu_thread_fn(void *data)
{
	struct atom_dev *dev = data;
	down_read(&run_sem);
	dev->op_ctr = 0;
	while (!kthread_should_stop() && jiffies < test.end_jiffies) {
		int i;
		for (i = 0; i < NUM_OPS; i++) {
			atomic_inc(test.atom_ctr);
			dev->op_ctr++;
		}
		schedule();
	}
	up_read(&run_sem);
	end_thread_fn();
	return 0;
}

void start_run(void)
{
	int i;

	atomic_set(test.atom_ctr, 0);
	test.start_jiffies = jiffies;
	test.end_jiffies = test.start_jiffies + (test.run_secs * HZ);
	test.running = true;
	for (i = 0; i < 2; i++) {
		if (devs[i].dev)
			devs[i].thread = kthread_run(dev_thread_fn, &devs[i], "atom_dev%d", i);
		else
			devs[i].thread = kthread_run(cpu_thread_fn, &devs[i], "atom_cpu%d", i);

		if (IS_ERR(devs[i].thread)) {
			_E("kthread_create failed (%ld)\n", PTR_ERR(devs[i].thread));
			goto err;
		}
	}
	return;

err:
	for (i = 0; i < 2; i++) {
		if (!(IS_ERR(devs[i].thread)))
			kthread_stop(devs[i].thread);
	}
}

void stop_run(void)
{
	int i;
	for (i = 0; i < 2; i++) {
		kthread_stop(devs[i].thread);
	}
}

static ssize_t run_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	ssize_t rv = 0;
	if (test.running)
		rv = sprintf(buf, "Running. Device 1: %s Device 2: %s Time remaining %ld secs\n",
			     (devs[0].dev ? devs[0].dev->name : "cpu"),
			     (devs[1].dev ? devs[1].dev->name : "cpu"),
			     (test.end_jiffies - jiffies) / HZ);
	else
		rv = sprintf(buf, "Not running\n");

	return rv;
}

static ssize_t run_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	int run = -1;
	ssize_t rv = count;

	if (sscanf(buf, "%d", &run) != 1) {
		_E("Error parsing run command %s\n", buf);
		rv = -EINVAL;
		goto out;
	}

	if (!test.running) {
		if (run < 0) {
			_E("Not running and run value is %d\n", run);
			rv = -EINVAL;
			goto out;
		}
		test.run_secs = run;
		start_run();
	} else {
		if (run) {
			_E("Running and run value is %d\n", run);
			rv = -EINVAL;
			goto out;
		}

		stop_run();
	}

out:
	return rv;
}

#if KS_IB_CLIENT_ADD_RV_IS_INT
static int add_one(struct ib_device *device)
#else
static void add_one(struct ib_device *device)
#endif
{
	struct rdma_dev *new_dev = NULL;

	if (!(new_dev = kzalloc(sizeof(*new_dev), GFP_KERNEL))) {
		_E("Memory Error");
	#if KS_IB_CLIENT_ADD_RV_IS_INT
		return -1;
	#else
		return;
	#endif
	}

	new_dev->dev = device;

	mutex_lock(&dev_list_guard);
	list_add_tail(&new_dev->link, &dev_list);
	mutex_unlock(&dev_list_guard);

#if KS_IB_CLIENT_ADD_RV_IS_INT
	return 0;
#endif
}

#if IB_REMOVE_EXTRA_ARG
static void remove_one(struct ib_device *device, void *client_data)
#else
static void remove_one(struct ib_device *device)
#endif
{
	struct rdma_dev *dev = NULL;

	/*TBD: If test running, wait for it to finish */

	mutex_lock(&dev_list_guard);
	list_for_each_entry(dev, &dev_list, link) {
		if (dev->dev == device) {
			/* Found the device */
			list_del(&dev->link);
			kfree(dev);
			break;
		}
	}
	mutex_unlock(&dev_list_guard);
}

static int __init atom_test_init(void) /* Constructor */
{
	int rv = 0;

	FIN;
	memset(&test, 0, sizeof(test));
	if (!(test.page = alloc_pages(GFP_KERNEL | __GFP_ZERO, 0))) {
		rv = -ENOMEM;
		goto out;
	}
	test.atom_ctr = page_address(test.page);

	atom_test_kobj = kobject_create_and_add("atom_test", kernel_kobj);

	if (!atom_test_kobj) {
		rv = -ENOMEM;
		goto free_mem;
	}

	if ((rv = sysfs_create_file(atom_test_kobj, &dev1_attr.attr))) {
		_E("sysfs_create_file failed (%d)\n", rv);
		goto free_kobj;
	}

	if ((rv = sysfs_create_file(atom_test_kobj, &dev2_attr.attr))) {
		_E("sysfs_create_file failed (%d)\n", rv);
		goto free_kobj;
	}

	if ((rv = sysfs_create_file(atom_test_kobj, &run_attr.attr))) {
		_E("sysfs_create_file failed (%d)\n", rv);
		goto free_kobj;
	}

	if ((rv = ib_register_client(&client)))
		goto free_kobj;

	_I("Hooray: atom_test registered\n");
	goto out;

free_kobj:
	kobject_put(atom_test_kobj);

free_mem:
	__free_pages(test.page, 0);

out:
	FOUT;
	return rv;
}

static void __exit atom_test_exit(void) /* Destructor */
{
	FIN;
	ib_unregister_client(&client);
	kobject_put(atom_test_kobj);
	__free_pages(test.page, 0);

	_I("Hooray: atom_test unregistered\n");
	FOUT;
}

module_init(atom_test_init);
module_exit(atom_test_exit);

