/* The keeper module is used to hold resources for the client/target during restart,
 * speeding up the restart and reducing the time that IO is paused (hiatus time).
 * 
 * Currently it is only used to hold MRs for the client fast-reg-pool, but it is extendable
 * to hold anything.
 * 
 * The keeper and public module (for GPL reasons) rendezvous without static dependencies
 * using symbol_get()/symbol_put().
 * 
 * Because the keeper holds IB resources, it is also registers as an IB client in order
 * to get add_one()/remove_one() events.
 * 
 * CLIENT SHUTDOWN FLOW
 * - In nvmeibc_core_ibdev.inc.c remove_ib() - Checks if shutting down for upgrade and calls nvmeib_public_load_keeper()
 * - nvmeib_public_load_keeper() - Calls request_module() to load the keeper module
 * - nvmeib_keeper_init() - Initialises the keeper and calls public nvmeib_register_keeper() with keeper ops via symbol_get()
 * - Keeper has now registered with client and ops can be accessed with refcnt using nvmeib_public_get_keeper()/nvmeib_public_put_keeper()
 * - In nvmeibc_core_ibdev.inc.c free_fmr():
 *	 - If keeper registered: 
 * 		- Pass mr array to nvmeib_destroy_fast_reg_pool() to fill with pool MRs
 *	 	- Store mrs, dma_mr and pd in nvmeib_dev->keeper_frs_info
 * - In nvmeib.c nvmeib_free():
 * 	- If keeper registered:
 * 		- Call keeper push_frs_op_fn() through push_frs ops ptr.
 * 
 * CLIENT STARTUP FLOW (Keeper is already loaded by shutdown flow):
 * - When public is loaded, nvmeib_public_module_init() calls init_keeper_iface()
 * - init_keeper_iface() - Calls request_keeper_to_register()
 * - request_keeper_to_register() - looks up nvmeib_keeper_request_register with symbol_get() and calls it with ptr to nvmeib_register_keeper()
 * - nvmeib_keeper_request_register() - Triggers keeper rendezvous with public module via nvmeib_register_keeper fn ptr.
 * - Keeper has now registered with client
 * - In nvmeib.c nvmeib_init():
 * 	- If keeper registered:
 * 		- Calls pop_frs_op_fn() through pop_frs ops ptr.
 * 		- Results of pop_frs are stored in nvmeib_dev->keeper_frs_info
 * 		- dev->pd and dev->mr are taken from keeper_frs_info instead of allocating them
 * - In nvmeib.c nvmeib_alloc_fast_reg_pool():
 * 	- If dev->keeper_frs_info has stored mrs - create_fr_pool() is called with stored mr_arr
 * 		- create_fr_pool() - Initialises FR pool using MRs from stored mr_arr
 * - In nvmeibc_core_common.inc.c t_core_clnt_globals_create() after clnt_start_ib_work_fn() runs on Main WQ:
 * 	- Calls nvmeib_public_unload_keeper() - Unloads keeper module using call_usermodehelper_exec to call modprobe
 * 
 * KEEPER EXIT FLOW (Keeper unloads before public):
 * - nvmeib_keeper_exit() calls public nvmeib_unregister_keeper() via symbol_get()
 * - nvmeib_unregister_keeper() calls nvmeib_ref_release_start() on nvmeib_keeper_refcnt
 * - nvmeib_keeper_ops is set to NULL (All future calls to nvmeib_public_get_keeper() return NULL)
 * - If keeper reconnects, nvmeib_register_keeper() calls nvmeib_ref_release_wait() on nvmeib_keeper_refcnt
 *
 * KEEPER EXIT FLOW (Public unloads before Keeper):
 * - nvmeib_public_module_exit() calls nvmeib_public_keeper_fini()
 * - nvmeib_public_keeper_fini() - Calls nvmeib_public_get_keeper() to see if keeper registered
 * - If keeper registered:
 * 	- Calls close_cb_fn() via keeper ops with ptr to nvmeib_unregister_keeper()
 * 	- close_cb_fn() calls nvmeib_unregister_keeper() via fn ptr
 * 	- nvmeib_unregister_keeper() calls nvmeib_ref_release_start() on nvmeib_keeper_refcnt
 * 	- nvmeib_keeper_ops is set to NULL (All future calls to nvmeib_public_get_keeper() return NULL)
 * 	- nvmeib_public_keeper_fini() then: 
 * 		- Calls nvmeib_public_put_keeper() to return reference
 * 		- Calls nvmeib_unregister_keeper_wait_done() to wait for refcnt to return to 0
 * 
 * FREEING FRS FLOW (Keeper is unloaded after Client-Shutdown and before Client-Start)
 * - nvmeib_keeper_exit() calls ib_unregister_client() which triggers nvmeib_keeper_remove_one() callbacks
 * - nvmeib_keeper_remove_one():
 *	- Finds ib_device in radix tree
 *	- Loops over all instance nodes:
 *		- Loops over all FRs in tree:
 *			- Frees FRs MRs
 *		 	- Frees FRs PD
 * 
 * NOTE: The keeper must stay up while nvmesh is brought down for upgrade.
 * Therefore it cannot have any hard dependencies on any of the other nvmesh modules.
 * This also means the tracer cannot be used */

#include <linux/types.h>
#include <linux/list.h>
#include <linux/proc_fs.h>
#include <linux/rtc.h>
#include <linux/module.h>				// For MODULE_AUTHOR()
#include <rdma/ib_verbs.h>
#include <linux/workqueue.h>
#include <linux/time.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include "nvmeib_jdr.h"

#define MODULE_NAME "nvmeib_keeper"
#define MODULE_VERS "1.0"

#if KS_THREAD_INFO_HAS_CPU
#define keeper_current_cpu current_thread_info()->cpu
#else
#define keeper_current_cpu current->cpu
#endif

#define FILENAME kbasename(__FILE__)
static inline const char *basename(const char *path)
{
	const char *tail = strrchr(path, '/');
	return tail ? tail + 1 : path;
}

#define keeper_print(level, fmt, args...) do {\
	if(!in_interrupt())                                                                 		\
		printk(KERN_##level "%s %s: (%5d/%1d)[%s] %s(%s/%d): " fmt,        	\
				#level, MODULE_NAME, current->pid, keeper_current_cpu, current->comm, __func__, FILENAME, __LINE__,	\
				##args);                                                                    	\
	else                                                                                		\
		printk(KERN_##level "%s %s: ( irq /%1d)[%s] %s(%s/%d): " fmt,      	\
				#level, MODULE_NAME, keeper_current_cpu, current->comm, __func__, FILENAME, __LINE__, ##args);  \
} while(0)

#ifdef KR_INCS
#	error "Dont do include any other nvmesh module (common/kr_incs.h)"
#endif

#include "nvmeib_keeper_iface.h"

MODULE_AUTHOR("NVIDIA");
MODULE_DESCRIPTION("Keeps NVMesh resources during upgrade");
MODULE_LICENSE("Dual BSD/GPL");

/* Keeper Ops Functions Signatures */
static DEFINE_KEEPER_PUSH_FRS_FN(push_frs_op_fn);
static DEFINE_KEEPER_POP_FRS_FN(pop_frs_op_fn);
static DEFINE_KEEPER_CLOSE_CB_FN(close_cb_fn);

/* To keep compiler happy */
DEFINE_KEEPER_REQUEST_REGISTER_FN(nvmeib_keeper_request_register);

/* Keeper Ops Table */
static struct nvmeib_keeper_ops nvmeib_keeper_ops = {
	.push_frs = push_frs_op_fn,
	.pop_frs = pop_frs_op_fn,
	.close_cb = close_cb_fn,
};

static bool nvmeib_keeper_registered = false;

struct ib_dev_node {
	struct ib_device *ib_dev;
	struct list_head inst_list;
	struct radix_tree_node node;
};

#define CINST_NAME_LEN	8

struct inst_node {
	char inst_name[CINST_NAME_LEN];
	struct radix_tree_root frs_tree;
	int num_frs;
	struct list_head link;
};

static DEFINE_MUTEX(nvmeib_keeper_guard);
static struct radix_tree_root nvmeib_keeper_ib_dev_tree;
static int num_ib_dev;

struct frs_tree_node {
	struct nvmeib_keeper_frs_info info;
	struct timespec64 push_ts;
	char inst_name[CINST_NAME_LEN];
	struct radix_tree_node node;
	struct work_struct work;
};

static DECLARE_COMPLETION(free_frs_work_comp);
static atomic_t free_frs_work_cnt = ATOMIC_INIT(1);

static atomic_t keeper_dying = ATOMIC_INIT(0);

static DEFINE_MUTEX(keeper_request_register_guard);
static atomic_t keeper_api_entry = ATOMIC_INIT(1);
static DECLARE_COMPLETION(keeper_api_entry_done);

#if KS_IB_CLIENT_ADD_RV_IS_INT
static int nvmeib_keeper_add_one(struct ib_device *device)
#else
static void nvmeib_keeper_add_one(struct ib_device *device)
#endif
{
	struct ib_dev_node *node;
	int rv;
	
	keeper_print(INFO, "add_one called on device %s\n", device->name);

	if (!(node = kzalloc(sizeof(*node), GFP_KERNEL))) {
		keeper_print(ERR, "OOM\n");
		rv = -ENOMEM;
		goto out;
	}

	node->ib_dev = device;
	INIT_LIST_HEAD(&node->inst_list);
	mutex_lock(&nvmeib_keeper_guard);
	if ((rv = radix_tree_insert(&nvmeib_keeper_ib_dev_tree, (unsigned long)device, node)) < 0) {
		keeper_print(ERR, "radix_tree_insert failed (%d)\n", rv);
		goto unlock;
	}
	num_ib_dev++;
	rv = 0;

unlock:
	mutex_unlock(&nvmeib_keeper_guard);

out:
#if KS_IB_CLIENT_ADD_RV_IS_INT
	return rv;
#else
	(void)rv;
#endif
}

static void free_frs_node(struct frs_tree_node *node)
{
	int rv, i;

	keeper_print(DEBUG, "freeing %d FRs from instance %s of device %s\n",
		 node->info.n_mr, node->inst_name, node->info.pd->device->name);

	for (i = 0; i < node->info.n_mr; i++) {
		if (node->info.mr_arr[i]) {
			if ((rv = ib_dereg_mr(node->info.mr_arr[i])) < 0) {
				keeper_print(ERR, "ib_dereg_mr failed (%d)\n", rv);
			}
		}
	}
	if (node->info.dma_mr) {
#if HAS_IB_GET_DMA_MR
		rv = ib_dereg_mr(node->info.dma_mr);
#else
		/* The DMA MR was gotten through the get_dma_mr fn ptr.
		 * Freeing it with ib_dereg_mr or with the dereg_mr fn ptr causes a crash.
		 * So we just skip it.
		 */
		rv = 0;
#endif
		if (rv < 0) {
			keeper_print(ERR, "ib_dereg_mr failed (%d)\n", rv);
		}
	}
	if (node->info.pd) {
		ib_dealloc_pd(node->info.pd);
	}
	
	kfree(node);
}

static bool keeper_has_frs(void)
{
	struct ib_dev_node *dev_node;
	struct radix_tree_iter dev_iter;
	struct inst_node *inst_node;
	struct radix_tree_iter frs_iter;
	void **dev_slot, **frs_slot;
	bool ret = false;

	mutex_lock(&nvmeib_keeper_guard);

	radix_tree_for_each_slot(dev_slot, &nvmeib_keeper_ib_dev_tree, &dev_iter, 0) {
		dev_node = *dev_slot;
		list_for_each_entry(inst_node, &dev_node->inst_list, link) {
			radix_tree_for_each_slot(frs_slot,  &inst_node->frs_tree, &frs_iter, 0) {
				ret = true;
				goto out;
			}
		}
	}

out:
	mutex_unlock(&nvmeib_keeper_guard);
	return ret;
}

#if KS_IB_CLIENT_REMOVE_HAS_CLIENT_DATA
static void nvmeib_keeper_remove_one(struct ib_device *device, void *client_data)
#else
static void nvmeib_keeper_remove_one(struct ib_device *device)
#endif
{
	struct ib_dev_node *dev_node;
	struct inst_node *inst_node;
	struct frs_tree_node *frs_node;
	struct radix_tree_iter frs_iter;
	void **slot;

	mutex_lock(&nvmeib_keeper_guard);

	/* Try and find the device in the radix-tree */
	if (!(dev_node = radix_tree_lookup(&nvmeib_keeper_ib_dev_tree, (unsigned long)device))) {
		keeper_print(WARNING, "device %s not found in tree\n", device->name);
		goto unlock;
	}

	keeper_print(INFO, "remove_one called on device %s\n", dev_node->ib_dev->name);

	/* Found, remove it and free everything */
	radix_tree_delete(&nvmeib_keeper_ib_dev_tree, (unsigned long)device);
	BUG_ON(num_ib_dev <= 0);
	num_ib_dev--;

	while ((inst_node = list_first_entry_or_null(&dev_node->inst_list, struct inst_node, link))) {
		list_del(&inst_node->link);
		radix_tree_for_each_slot(slot, &inst_node->frs_tree, &frs_iter, 0) {
			frs_node = *slot;
			radix_tree_iter_delete(&inst_node->frs_tree, &frs_iter, slot);
			BUG_ON(inst_node->num_frs <= 0);
			inst_node->num_frs--;
			free_frs_node(frs_node);
		}
		kfree(inst_node);
	}

	kfree(dev_node);
	
unlock:
	mutex_unlock(&nvmeib_keeper_guard);
}

static struct ib_client nvmeib_keeper_ib_client = {
	.name   = "nvmeib_keeper_ib",
	.add    = nvmeib_keeper_add_one,
	.remove = nvmeib_keeper_remove_one
};

static void free_frs_work_fn(struct work_struct *work)
{
	struct frs_tree_node *frs_node = container_of(work, struct frs_tree_node, work);
	free_frs_node(frs_node);
	if (atomic_dec_return(&free_frs_work_cnt) == 0)
		complete(&free_frs_work_comp);
}

static void update_frs_module(struct nvmeib_keeper_frs_info *frs_info, const char *mod_name)
{
#if KS_RDMA_HAS_RESTRACK
	int i;
	
	frs_info->pd->res.kern_name = mod_name;
	for (i = 0; i < frs_info->n_mr; i++) {
		frs_info->mr_arr[i]->res.kern_name = mod_name;
	}
#endif
}

static DEFINE_KEEPER_PUSH_FRS_FN(push_frs_op_fn) {
	struct ib_dev_node *dev_node;
	struct inst_node *inst_node = NULL, *inst_iter;
	struct frs_tree_node *frs_node;
	struct ib_pd *pd;
	struct ib_device *ib_dev;
	int rv;
	
	if (atomic_read(&keeper_dying) || !atomic_inc_not_zero(&keeper_api_entry)) {
		keeper_print(INFO, "keeper is going down");
		rv = -EBUSY;
		goto out;
	}
	
	if (strlen(inst_name) >= CINST_NAME_LEN) {
		keeper_print(ERR, "inst_name %s is too long", inst_name);
		rv = -EINVAL;
		goto api_done;
	}
	
	if (!nvmeib_keeper_frs_info->pd || 
		!nvmeib_keeper_frs_info->dma_mr || 
		!nvmeib_keeper_frs_info->mr_arr || 
		!nvmeib_keeper_frs_info->n_mr) 
	{
		keeper_print(ERR, "push_frs called with invalid params -"
			" pd: %p dma_mr: %p mr_arr: %p n_mr: %d\n",
			nvmeib_keeper_frs_info->pd,
			nvmeib_keeper_frs_info->dma_mr,
			nvmeib_keeper_frs_info->mr_arr,
			nvmeib_keeper_frs_info->n_mr);
		rv = -EINVAL;
		goto api_done;
	}

	BUG_ON(nvmeib_keeper_frs_info->mr_arr[0]->pd != nvmeib_keeper_frs_info->pd);

	pd = nvmeib_keeper_frs_info->pd;
	ib_dev = pd->device;
	
	mutex_lock(&nvmeib_keeper_guard);
	
	/* Check dying flag again after potential sleep of mutex_lock */
	if (atomic_read(&keeper_dying)) {
		keeper_print(INFO, "keeper is going down");
		rv = -EBUSY;
		goto unlock;
	}
	
	if (!(dev_node = radix_tree_lookup(&nvmeib_keeper_ib_dev_tree, (unsigned long)ib_dev))) {
		keeper_print(WARNING, "device %s not found in tree\n", ib_dev->name);
		rv = -ENOENT;
		goto unlock;
	}
	
	list_for_each_entry(inst_iter, &dev_node->inst_list, link) {
		if (strncmp(inst_iter->inst_name, inst_name, CINST_NAME_LEN) == 0) {
			keeper_print(DEBUG, "instance %s found for device %s\n", inst_name, ib_dev->name);
			inst_node = inst_iter;
			break;
		}
	}
	
	if (!inst_node) {
		keeper_print(DEBUG, "adding instance %s to device %s\n", inst_name, ib_dev->name);
		/* Instance does not exist yet, add it */	
		if (!(inst_node = kzalloc(sizeof(*inst_node), GFP_KERNEL))) {
			keeper_print(ERR, "OOM\n");
			rv = -ENOMEM;
			goto unlock;
		}
		strscpy(inst_node->inst_name, inst_name, CINST_NAME_LEN);
		INIT_RADIX_TREE(&inst_node->frs_tree, GFP_KERNEL);
		list_add_tail(&inst_node->link, &dev_node->inst_list);
	}

	if ((frs_node = radix_tree_lookup(&inst_node->frs_tree, (unsigned long)pd))) {
		keeper_print(DEBUG, "removing old FRs with PD %p for instance %s and device %s\n",
			 pd, inst_name, ib_dev->name);
		/* Remove old entry and clean-up MRs on system WQ in BG */
		radix_tree_delete(&inst_node->frs_tree, (unsigned long)pd);
		BUG_ON(inst_node->num_frs <= 0);
		inst_node->num_frs--;
		
		INIT_WORK(&frs_node->work, free_frs_work_fn);
		atomic_inc(&free_frs_work_cnt);
		if (!schedule_work(&frs_node->work)) {
			atomic_dec(&free_frs_work_cnt);
			BUG(); /* If the schedule_work() fails, something has gone horribly wrong - Panic */
		}
	}

	if (!(frs_node = kzalloc(sizeof(*frs_node), GFP_KERNEL))) {
		keeper_print(ERR, "ERROR: OOM\n");
		rv = -ENOMEM;
		goto unlock;
	}

	strscpy(frs_node->inst_name, inst_name, CINST_NAME_LEN);
	ktime_get_real_ts64(&frs_node->push_ts);
	frs_node->info = *nvmeib_keeper_frs_info;
	if ((rv = radix_tree_insert(&inst_node->frs_tree, (unsigned long)pd, frs_node)) < 0) {
		keeper_print(ERR, "ERROR: radix_tree_insert failed (%d)\n", rv);
		kfree(frs_node);
		goto unlock;
	}
	inst_node->num_frs++;
	
	keeper_print(DEBUG, "Added %d FRs with PD %p for instance %s and device %s\n",
		 nvmeib_keeper_frs_info->n_mr, pd, inst_name, ib_dev->name);

	update_frs_module(&frs_node->info, KBUILD_MODNAME);
	
unlock:
	mutex_unlock(&nvmeib_keeper_guard);
	
api_done:
	/* Wake up module_exit if waiting for entries to return to 0 */
	if (atomic_dec_return(&keeper_api_entry) == 0) {
		complete(&keeper_api_entry_done);
	}

out:
	return rv;
}

static DEFINE_KEEPER_POP_FRS_FN(pop_frs_op_fn) {
	struct ib_dev_node *dev_node;
	struct inst_node *inst_node = NULL, *inst_iter;
	struct radix_tree_iter frs_iter;
	struct frs_tree_node *frs_node;
	void **slot;
	int rv;

	if (atomic_read(&keeper_dying) || !atomic_inc_not_zero(&keeper_api_entry)) {
		keeper_print(INFO, "keeper is going down");
		rv = -EBUSY;
		goto out;
	}

	mutex_lock(&nvmeib_keeper_guard);
	
	/* Check dying flag again after potential sleep of mutex_lock */
	if (atomic_read(&keeper_dying)) {
		keeper_print(INFO, "keeper is going down");
		rv = -EBUSY;
		goto unlock;
	}

	if (!(dev_node = radix_tree_lookup(&nvmeib_keeper_ib_dev_tree, (unsigned long)ib_dev))) {
		keeper_print(WARNING, "device %s not found in tree\n", ib_dev->name);
		rv = -ENOENT;
		goto unlock;
	}
	
	
	list_for_each_entry(inst_iter, &dev_node->inst_list, link) {
		if (strncmp(inst_iter->inst_name, inst_name, CINST_NAME_LEN) == 0) {
			keeper_print(DEBUG, "instance %s found for device %s\n", inst_name, ib_dev->name);
			inst_node = inst_iter;
			break;
		}
	}
	
	if (!inst_node) {
		keeper_print(DEBUG, "Instance %s not found for device %s\n", inst_name, ib_dev->name);
		rv = -ENOENT;
		goto unlock;
	}

	radix_tree_for_each_slot(slot, &inst_node->frs_tree, &frs_iter, 0) {
		frs_node = *slot;
		radix_tree_iter_delete(&inst_node->frs_tree, &frs_iter, slot);
		BUG_ON(inst_node->num_frs <= 0);
		inst_node->num_frs--;

		*nvmeib_keeper_frs_info = frs_node->info;
		kfree(frs_node);
		
		keeper_print(DEBUG, "Found %d FRs for instance %s for device %s\n",
			 nvmeib_keeper_frs_info->n_mr, inst_name, nvmeib_keeper_frs_info->pd->device->name);

		update_frs_module(nvmeib_keeper_frs_info, mod_name);
		rv = 0;
		goto unlock;
	}

	keeper_print(DEBUG, "MRs not found for instance %s for device %s\n", inst_name, ib_dev->name);
	rv = -ENOENT;
	
unlock:
	mutex_unlock(&nvmeib_keeper_guard);

	/* Wake up module_exit if waiting for entries to return to 0 */
	if (atomic_dec_return(&keeper_api_entry) == 0) {
		complete(&keeper_api_entry_done);
	}
out:
	return rv;
}

static DEFINE_KEEPER_CLOSE_CB_FN(close_cb_fn) {
	if (atomic_read(&keeper_dying) || !atomic_inc_not_zero(&keeper_api_entry)) {
		keeper_print(INFO, "keeper is going down");
		return;
	}

	mutex_lock(&keeper_request_register_guard);

	/* Check dying flag again after potential sleep of mutex_lock */
	if (atomic_read(&keeper_dying)) {
		keeper_print(INFO, "keeper is going down");
		goto unlock;
	}

	/* NVMesh is going down. Call the function to unregister the keeper. */
	(*unreg_keeper_fn)(&nvmeib_keeper_ops);
	nvmeib_keeper_registered = false;

unlock:
	mutex_unlock(&keeper_request_register_guard);

	/* Wake up module_exit if waiting for entries to return to 0 */
	if (atomic_dec_return(&keeper_api_entry) == 0) {
		complete(&keeper_api_entry_done);
	}	
}

static void __print_hooray(bool is_start, const char* mod_name)
{	/* Hooray */
	const char *ur = ((is_start) ? "registered" : "unregistered");		// Consider up/down
	struct timespec64 ts;
	struct tm tm;

	ktime_get_real_ts64(&ts);
	ts.tv_sec -= sys_tz.tz_minuteswest * 60;
	time64_to_tm(ts.tv_sec, 0, &tm);
	keeper_print(INFO, "Module %s. Module: %s. Timestamp: (%04ld-%02d-%02d %02d:%02d:%02d).\n", ur, mod_name, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec); // Hooray //. Error code: 0
}

DEFINE_COMMON_REGISTER_KEEPER_FN(nvmeib_register_keeper);
DEFINE_COMMON_UNREGISTER_KEEPER_FN(nvmeib_unregister_keeper);

#define __NVMEIB_KEEPER_SYMBOL_STR(x) #x
#define NVMEIB_KEEPER_SYMBOL_STR(x) __NVMEIB_KEEPER_SYMBOL_STR(x)

DEFINE_KEEPER_REQUEST_REGISTER_FN(nvmeib_keeper_request_register)
{
	int rv;

	if (atomic_read(&keeper_dying) || !atomic_inc_not_zero(&keeper_api_entry)) {
		keeper_print(INFO, "keeper is going down");
		rv = -EBUSY;
		goto out;
	}
	
	mutex_lock(&keeper_request_register_guard);
	
	/* Check dying flag again after potential sleep of mutex_lock */
	if (atomic_read(&keeper_dying)) {
		keeper_print(INFO, "keeper is going down");
		rv = -EBUSY;
		goto unlock;
	}

	if (nvmeib_keeper_registered) {
		keeper_print(WARNING, "already registered nvmeib_common\n");
		rv = -EALREADY;
		goto unlock;
	}

	keeper_print(INFO, "Requested to register with nvmeib_common\n");

	if ((rv = (*reg_keeper_fn)(&nvmeib_keeper_ops)) < 0) {
		keeper_print(WARNING, "Failed (%d) to register with nvmeib_common\n", rv);
		goto unlock;
	}

	nvmeib_keeper_registered = true;

unlock:
	mutex_unlock(&keeper_request_register_guard);
	
	/* Wake up module_exit if waiting for entries to return to 0 */
	if (atomic_dec_return(&keeper_api_entry) == 0) {
		complete(&keeper_api_entry_done);
	}

out:
	return rv;
}
EXPORT_SYMBOL_GPL(nvmeib_keeper_request_register);

#define FRS_PROC_VERSION	1

/************ Proc stuff **************************/
static int frs_show(struct seq_file *m, void *v)
{
	struct ib_dev_node *dev_node;
	struct radix_tree_iter dev_iter;
	struct inst_node *inst_node;
	struct radix_tree_iter frs_iter;
	struct frs_tree_node *frs_node;
	void **dev_slot, **frs_slot;
	struct timespec64 ts;
	struct tm tm;
	struct jdr jdr = jdr_make_seq(m);
	
	if (atomic_read(&keeper_dying) || !atomic_inc_not_zero(&keeper_api_entry)) {
		jdr_write_var(&jdr, status, -1);
		jdr_write_var(&jdr, error, (char const *)"keeper is going down");
		goto out;
	}
	
	ktime_get_real_ts64(&ts);
	ts.tv_sec -= sys_tz.tz_minuteswest * 60;
	time64_to_tm(ts.tv_sec, 0, &tm);

	mutex_lock(&nvmeib_keeper_guard);
	
	/* Check dying flag again after potential sleep of mutex_lock */
	if (atomic_read(&keeper_dying)) {
		jdr_write_var(&jdr, status, -1);
		jdr_write_var(&jdr, error, (char const *)"keeper is going down");
		goto unlock;
	}

	jdr_write_var(&jdr, format_version, FRS_PROC_VERSION);
	jdr.ops.ascii_format(&jdr, "time", "%04ld-%02d-%02d %02d:%02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
	{
		jdr_array_scope(&jdr, "devices");
		radix_tree_for_each_slot(dev_slot, &nvmeib_keeper_ib_dev_tree, &dev_iter, 0) {
			dev_node = *dev_slot;
			{
				jdr_object_scope(&jdr, NULL);
				jdr_write_var(&jdr, name, (char const *)dev_node->ib_dev->name);
				{
					jdr_array_scope(&jdr, "instances");
					list_for_each_entry(inst_node, &dev_node->inst_list, link) {
						jdr_object_scope(&jdr, NULL);
						jdr_write_var(&jdr, name, (char const *)inst_node->inst_name);
						{
							jdr_array_scope(&jdr, "frs");
							radix_tree_for_each_slot(frs_slot, &inst_node->frs_tree, &frs_iter, 0) {
								frs_node = *frs_slot;
								{
									jdr_object_scope(&jdr, NULL);
									jdr_write_var(&jdr, version, frs_node->info.version);
									ts = frs_node->push_ts;
									ts.tv_sec -= sys_tz.tz_minuteswest * 60;
									time64_to_tm(ts.tv_sec, 0, &tm);
									jdr.ops.ascii_format(&jdr, "push_time", "%04ld-%02d-%02d %02d:%02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
									jdr_write_var(&jdr, num_mrs, frs_node->info.n_mr);
									jdr_write_var(&jdr, mr_page_mask, frs_node->info.mr_page_mask);
									jdr_write_var(&jdr, mr_page_size, frs_node->info.mr_page_size);
									jdr_write_var(&jdr, mr_max_size, frs_node->info.mr_max_size);
									jdr_write_var(&jdr, max_pages_per_mr, frs_node->info.max_pages_per_mr);
								}
							}
						}
					}
				}
			}
		}
	}

unlock:
	mutex_unlock(&nvmeib_keeper_guard);
	
	/* Wake up module_exit if waiting for entries to return to 0 */
	if (atomic_dec_return(&keeper_api_entry) == 0) {
		complete(&keeper_api_entry_done);
	}
	
out:
	jdr_finalize(&jdr);
	return 0;
}

static int frs_open(struct inode *inode, struct file *file)
{
	// single_open() simplifies seq_file setup for one-shot outputs
	return single_open(file, frs_show, NULL);
}

#if !KS_HAS_PROC_FS
static const struct file_operations frs_proc_ops = {
	.open		= frs_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};
#else
static const struct proc_ops frs_proc_ops = {
	.proc_open		= frs_open,
	.proc_read		= seq_read,
	.proc_lseek		= seq_lseek,
	.proc_release	= single_release,
};
#endif

static int __init nvmeib_keeper_init(void)
{
	static DEFINE_COMMON_REGISTER_KEEPER_FN((*reg_keeper_fn));
	int rv;
	/* Cleanup is done using remove_proc_subtree - no need to keep these */
	struct proc_dir_entry *proc_dir, *proc_frs_file;
	bool ib_reg = false;

	INIT_RADIX_TREE(&nvmeib_keeper_ib_dev_tree, GFP_KERNEL);
	
	if (!(proc_dir = proc_mkdir(MODULE_NAME, NULL))) {
		keeper_print(ERR, "Could not create proc dir /proc/%s\n", MODULE_NAME);
		rv = -ENOMEM;
		goto err;
	}
	
	if (!(proc_frs_file = proc_create("frs", 0444, proc_dir, &frs_proc_ops))) {
		keeper_print(ERR, "Could not create proc entry /proc/%s/frs", MODULE_NAME);
		rv = -ENOMEM;
		goto err;
	}

	if ((rv = ib_register_client(&nvmeib_keeper_ib_client))) {
		keeper_print(ERR, "ib_register_client failed (%d)\n", rv);
		goto err;
	}
	ib_reg = true;

	/* Try and get the symbols from common. Fail to load if it cannot be found */
	reg_keeper_fn = symbol_get(nvmeib_register_keeper);

	if (!reg_keeper_fn) {
		keeper_print(WARNING, "Could not find nvmeib_common. Is it loaded?\n");
		rv = -ENODEV;
		goto err;
	}
	
	if ((rv = (*reg_keeper_fn)(&nvmeib_keeper_ops)) < 0) {
		keeper_print(WARNING, "Failed (%d) to register with nvmeib_common\n", rv);
		goto err;
	}

	nvmeib_keeper_registered = true;
	__print_hooray(true, MODULE_NAME);

	rv = 0;

	goto out;
	
err:
	if (ib_reg)
		ib_unregister_client(&nvmeib_keeper_ib_client);

	/* Cleans up the whole proc tree in one go. */
	remove_proc_subtree(MODULE_NAME, NULL);

out:
	if (reg_keeper_fn)
		symbol_put(nvmeib_register_keeper);

	return rv;
}
module_init(nvmeib_keeper_init);

static void __exit nvmeib_keeper_exit(void) /* Destructor */
{
	static DEFINE_COMMON_UNREGISTER_KEEPER_FN((*unreg_keeper_fn));

	/* Prevent API calls from running in parallel with exit */
	atomic_inc(&keeper_dying);
	if (atomic_dec_return(&keeper_api_entry) > 0)
		wait_for_completion(&keeper_api_entry_done);

	if (keeper_has_frs()) {
		keeper_print(WARNING, "Keeper going down while holding FRs");
	}
	
	if (nvmeib_keeper_registered) {
		unreg_keeper_fn = symbol_get(nvmeib_unregister_keeper);
		if (!unreg_keeper_fn) {
			keeper_print(ERR, "Could not find nvmeib_common. Cannot unregister\n");
			return;
		}
		(*unreg_keeper_fn)(&nvmeib_keeper_ops);
		symbol_put(nvmeib_unregister_keeper);
		
		nvmeib_keeper_registered = false;
	}

	ib_unregister_client(&nvmeib_keeper_ib_client);

	if (atomic_dec_return(&free_frs_work_cnt) > 0) {
		keeper_print(INFO, "Waiting for pending works to finish");
		wait_for_completion(&free_frs_work_comp);
	}

	/* Cleans up the whole proc tree in one go. */
	remove_proc_subtree(MODULE_NAME, NULL);

	__print_hooray(false, MODULE_NAME);
}
module_exit(nvmeib_keeper_exit);
