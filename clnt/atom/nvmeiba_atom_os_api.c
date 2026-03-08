#include "nvmeiba_main.h"
#include "nvmeiba_nvmesh_api.h"
#include "nvmeib_str.h"

/*************** Descriptor of process using kernel API ***********************/
/* an element of a single process that has an open handle to the module */
struct nvmeiba_pid_owner {
	struct list_head  chain;
	/* process group ID is the same for all threads of a process. hence,
	 * matching open/close from different threads of a process
	 * this does NOT handle mount which leaves the handle open & held by kernel,
	 * while user-space application terminates
	 * Warning: Do not save struct task_struct*, It may not exist! */
	char task_name[TASK_COMM_LEN];			// Todo: Save also the stack
	pid_t			pgid;
	u32				mode;
	bool			is_excl;
};
/* getting process group id seems to depend on kernel version.
 * as we move through newer versions, lets see what works.
 * Options:
 * 1) pinfo->pgid = pid_vnr(task_pgrp(current));
 * 2) pinfo->pgid = current->pgrp;
 * 3) on some its tgid rather than pgid
 */
static pid_t __get_user_pid(void)
{
	return task_tgid_nr(current);
}

static struct nvmeiba_pid_owner * __pid_owner_create(pid_t pgid, const char *name, BLK_MODE_T mode)
{
	struct nvmeiba_pid_owner *rv = kmalloc(sizeof(*rv), GFP_KERNEL);
	if (rv) {
		rv->pgid = pgid;
		strlcpy(rv->task_name, name, TASK_COMM_LEN);
		rv->mode = (u32)mode;
		rv->is_excl = (mode & BLK_MODE_EXCL);
	}
	return rv;
}

// Special macros when kernel takes reference open(), close() instead of user space
#define SELF_REF_PID			((pid_t)2)

/******************* Implement kernel mounting of atom ************************/
#define gendisk_get_atom_api_os(disk)	((struct nvmeiba_atom_os_api*)((disk)->private_data))

static int nvmeiba_bdev_open(struct gendisk *disk,
							   BLK_MODE_T mode, pid_t pgid, const char	*name)
{
	struct nvmeiba_atom_os_api *atom = gendisk_get_atom_api_os(disk);
	struct nvmeiba_pid_owner *pinfo;
	unsigned long flags;
	int	counter, rv = 0;

	pinfo = __pid_owner_create(pgid, name, mode);
	if (!pinfo) {
		_NE_to_user(t_0b_atom, "%s: Failed to open block device due to out of memory. Error code: 1005.\n", atom->dev_name);
		rv = -ENOMEM;
		goto out;
	}
	counter = atomic_inc_return(&atom->users.n_opens);
	_NT(t_0c_atom, "%s: mode=%#x, pid=%7u, pgid=%7u disk=%p, atm=%p opens=%d\n",
	   atom->dev_name, mode, current->pid,
	   pinfo->pgid, disk, atom, counter);
	spin_lock_irqsave(&atom->users.lock, flags);
	list_add_tail(&pinfo->chain, &atom->users.pids);
	spin_unlock_irqrestore(&atom->users.lock, flags);
	_NT(t_0d_atom, "%s: %spen, %s mode=%#x %d\n", atom->dev_name,
		(mode & BLK_MODE_EXCL		) ? "Exclusive o" : "O",
		(mode & MODE_WRITES_ALLOWED ) ? "Read/Write"  : "ReadOnly",
		mode, counter);
out:
	return rv;
}

static void __dec_ref_and_destroy_if_needed(struct nvmeiba_atom_os_api *atom,
							struct gendisk *disk, fmode_t mode, pid_t pgid)
{
	char dev_name[sizeof(atom->dev_name)];	/* Copy to stack for print */
	int counter;
	strlcpy(dev_name, atom->dev_name, sizeof(dev_name));
	counter = atomic_dec_return(&atom->users.n_opens);
	/* Daniel: Warning if (counter != 0) 'atom' might already got kfree() */
	_NT(t_0e_atom, "%s: mode=%#x, pgid=%7u, gendisk=%p, atm=%p, opens=%d\n", &dev_name[0],
	   mode, pgid, disk, atom, counter);
	if ((counter == 0) && (atom->disk == NULL)) { // LKJ: use enum nvmeiba_status_detaching, but make sure it is aligned with spinlock as gendisk does
		/* Last open handle being closed on a destroyed block atom */
		nvmeiba_os_api_destructor(atom);
	}
}

static void nvmeiba_bdev_close(struct gendisk *disk, BLK_MODE_T mode, pid_t pgid)
{
	struct nvmeiba_atom_os_api *atom = gendisk_get_atom_api_os(disk);
	int is_process_found = 0;
	struct nvmeiba_pid_owner *pinfo;
	unsigned long flags;
	spin_lock_irqsave(&atom->users.lock, flags);
	list_for_each_entry(pinfo, &atom->users.pids, chain) {
		if (pinfo->pgid == pgid) {		// a process with multiple entries deletes only 1 (first found)
			list_del(&pinfo->chain);
			kfree(pinfo);
			is_process_found = true;
			break;
		}
	}
	if (unlikely(!is_process_found && (mode & BLK_MODE_EXCL))) {
		list_for_each_entry(pinfo, &atom->users.pids, chain) {		// not found, checking for EXCL open (e.g.: mount)
			if (pinfo->is_excl) {		// a process with multiple entries deletes only 1 (first found)
				_NT(t_0f_atom, "%s: user pid=%u not found, removing EXCL %u\n", atom->dev_name, pgid, pinfo->pgid);
				list_del(&pinfo->chain);
				kfree(pinfo);
				is_process_found = true;
				break;
			}
		}
	}
	spin_unlock_irqrestore(&atom->users.lock, flags);
	if (unlikely(!is_process_found)) { // In simulator this often happens bcz pid is of a pthread
		_NT(t_0g_atom, "%s: user %i (%u) [%.16s], not found\n", atom->dev_name, current->pid, pgid, current->comm);
	}
	__dec_ref_and_destroy_if_needed(atom, disk, mode, pgid);
}

void nvmeiba_atom_part_add(struct nvmeiba_atom_os_api *atom)
{
	const int counter = atomic_inc_return(&atom->users.n_opens);
	_NT(t_0g_atom, "%s: add_sub to atm=%p opens=%d\n", atom->dev_name, atom, counter);
}
EXPORT_SYMBOL(nvmeiba_atom_part_add);

void nvmeiba_atom_part_del(struct nvmeiba_atom_os_api *atom)
{
	__dec_ref_and_destroy_if_needed(atom, NULL, FMODE_LSEEK, 0);
}
EXPORT_SYMBOL(nvmeiba_atom_part_del);

/* Remove leaking client process information.*/
static void __clean_leaking_users(struct nvmeiba_atom_os_api *atom)
{
	struct nvmeiba_users *us = &atom->users;
	unsigned long flags;
	int num_clients = 0;
	struct nvmeiba_pid_owner *pinfo;
	_NT(t_0h_atom, "%s: is removed with client process\n", atom->dev_name);
	spin_lock_irqsave(&atom->users.lock, flags);
	while ((pinfo = list_first_entry_or_null(&us->pids,
									struct nvmeiba_pid_owner, chain))) {
		_NT(t_0i_atom, "%3d) %u [%.16s] mode=0x%x\n", num_clients, pinfo->pgid, &pinfo->task_name[0], pinfo->mode);
		list_del(&pinfo->chain);
		kfree(pinfo);
		num_clients++;
	}
	spin_unlock_irqrestore(&atom->users.lock, flags);
	_NT(t_0j_atom, "%s: removed with %u client process\n", atom->dev_name, num_clients);
}

static int __fops_interface_open(struct BLK_MODE_OPEN_OBJ_T *obj, BLK_MODE_T mode)
{
	return nvmeiba_bdev_open(BLK_MODE_GENDISK(obj), mode, __get_user_pid(), current->comm);
}

#ifndef FMODE_EXCL
static void __fops_interface_close(struct gendisk *disk)
{
	nvmeiba_bdev_close(disk, disk->open_mode, __get_user_pid());
}
#elif KS_BLOCK_DEV_DEVICE_CLOSE_VOID
static void __fops_interface_close(struct gendisk *disk, BLK_MODE_T mode)
{
	nvmeiba_bdev_close(disk, mode, __get_user_pid());
}
#else // KS_BLOCK_DEV_DEVICE_CLOSE_VOID
static int  __fops_interface_close(struct gendisk *disk, BLK_MODE_T mode)
{
	nvmeiba_bdev_close(disk, mode, __get_user_pid());
	return KS_BLOCK_DEV_ZERO;
}
#endif // KS_BLOCK_DEV_DEVICE_CLOSE_VOID

void nvmeiba_os_apis_set_default_fops(struct block_device_operations *fops)
{
	fops->owner	=   THIS_MODULE;
	fops->open =    __fops_interface_open;
	fops->release = __fops_interface_close;
	fops->ioctl =   NULL;						// No default ioctls. Should be?
#if !KS_REQUEST_QUEUE_HAS_REQUEST_FN
	fops->submit_bio = NULL;					// Default ATOM submit_bio can be reject all bio.
#endif
}

#if !KS_REQUEST_QUEUE_HAS_REQUEST_FN
static REQ_RET nvmeiba_b_req_push(struct bio *bio);
void nvmeiba_os_apis_set_upgrade_fops(struct block_device_operations *fops)
{
	fops->owner	=   THIS_MODULE;
	fops->open =    __fops_interface_open;
	fops->release = __fops_interface_close;
	fops->ioctl =   NULL;						// No default ioctls. Should be?
	fops->submit_bio = nvmeiba_b_req_push;		// Upgrade ATOM submit_bio should store all bio.
}
static REQ_RET nvmeiba_b_req_reject(struct bio *bio);
void nvmeiba_os_apis_set_detaching_fops(struct block_device_operations *fops)
{
	fops->owner	=   THIS_MODULE;
	fops->open =    __fops_interface_open;
	fops->release = __fops_interface_close;
	fops->ioctl =   NULL;
	fops->submit_bio = nvmeiba_b_req_reject;		// Detaching ATOM submit_bio should reject all bio.
}
#endif

int nvmeiba_atom_open(struct BLK_MODE_OPEN_OBJ_T *obj, const char *name)
{
	return nvmeiba_bdev_open(BLK_MODE_GENDISK(obj), SELF_REF_MODE, SELF_REF_PID, name);
}
EXPORT_SYMBOL(nvmeiba_atom_open);

void nvmeiba_atom_close(struct gendisk *disk)
{
	nvmeiba_bdev_close(disk, SELF_REF_MODE, SELF_REF_PID);
}
EXPORT_SYMBOL(nvmeiba_atom_close);

/************************ External API for ATOM *******************************/
/* format all client processes as output for procfs file */
#define CLIENT_PROCESSES_PROC_FRMT_VER 1
ssize_t nvmeiba_atom_users_to_string(void *_atom, char *buf, size_t len)
{
#define BUF_ADD(...) count += scnprintf(buf+count, len-count, __VA_ARGS__)
	struct nvmeiba_atom_os_api *atom = _atom;
	const struct nvmeiba_users *us = &atom->users;
	struct nvmeiba_pid_owner 	*pinfo;
	unsigned long flags;
	ssize_t count   				= 0;
	u32  num_clients = 0, num_opens	= 0;

	BUF_ADD("Processes holding handle:\n");
	spin_lock_irqsave(&atom->users.lock, flags);
	num_opens = (u32)atomic_read(&us->n_opens);
	list_for_each_entry(pinfo, &us->pids, chain) {
		BUF_ADD("%3d) %u [%.16s], mode=0x%x\n", num_clients, pinfo->pgid,
				&pinfo->task_name[0], pinfo->mode);
		num_clients++;
	}
	spin_unlock_irqrestore(&atom->users.lock, flags);
	BUF_ADD("Num Processes=%u, opens=%u\n", num_clients, num_opens);
	return count;
#undef BUF_ADD
}
EXPORT_SYMBOL(nvmeiba_atom_users_to_string);

char *nvmeiba_atom_get_string_status(const struct nvmeiba_atom_os_api *a)
{
	switch (a->status) {
	case (nvmeiba_status_hidden)   : return "Hidden";
	case (nvmeiba_status_live)     : return "LiveIO";
	case (nvmeiba_status_orphan)   : return "Upgrde";
	case (nvmeiba_status_detaching): return "Detach";
	default                        : return "Uknown";
	}
}

void nvmeiba_os_api_constructor(struct nvmeiba_atom_os_api *atom)
{
#if NVMEIBA_HACK_DETACHING_DYNAMIC_EXPORT
	atom->reserved[0] = (u64)&nvmeiba_os_api_set_detaching;
#endif
	nvmeiba_os_apis_add(atom);
}
EXPORT_SYMBOL(nvmeiba_os_api_constructor);

void nvmeiba_os_api_destructor(struct nvmeiba_atom_os_api *atom)
{
	const struct nvmeiba_users *us = &atom->users;
	const int num_opens = (u32)atomic_read(&us->n_opens);
	_NT(t_0k_atom, "%s: freeing os_api=%p\n", atom->dev_name, atom);
	WARN(num_opens != 0, A_DMESG_PREFIX "%s: Unexpected error of device being free while it is in use by %d handles, Contact NVidia support. Error code: 1004.", atom->dev_name, num_opens); // Acts as _NE_to_user()
	nvmeiba_os_apis_del(atom);
	if (!list_empty(&us->pids)) { // Close of last handle frees the os_api so list should already be empty. But maybe some processes were not removed if process A did open() and B did its close(). A will still be in the list
		__clean_leaking_users(atom);
	}
	kfree(atom);
}
EXPORT_SYMBOL(nvmeiba_os_api_destructor);

#if KS_REQUEST_QUEUE_HAS_REQUEST_FN
static REQ_RET nvmeiba_b_req_push(struct request_queue *q, struct bio *bio)
#else
static REQ_RET nvmeiba_b_req_push(struct bio *bio)
#endif
{
	struct nvmeiba_atom_os_api *atom = gendisk_get_atom_api_os(bio_gendisk(bio)); // TODO: Ensure that rider IO to carrier never gets here as bio->bi_disk/bi_bdev is NULL and can crash
	struct nvmeiba_bio_pending_list *p = &atom->pender;
	bool is_still_orphan;
	ulong flags;
	spin_lock_irqsave(&p->lock, flags);
	is_still_orphan = nvmeiba_os_api_is_queue_orphan(atom);	// q->make_request_fn / fops->submit_bio haven't changed, bio is bound to go to the list
	if (is_still_orphan) {
		bio_list_add(&p->bio_list, bio);					// Note: Atom can be regular/carrier/sub atom
		p->n_bios++;
	}
	spin_unlock_irqrestore(&p->lock, flags);
	if (!is_still_orphan) {		// Right now being adopted, bio_list will never grow, being emptied right now
#if KS_REQUEST_QUEUE_HAS_REQUEST_FN
		q->make_request_fn(q, bio);	// make_request_fn() changed, call the correct function
#else
		atom->disk->fops->submit_bio(bio); // submit_bio() changed, call the correct function
#endif
	}
	return REQ_RET_ZERO;
}

bool nvmeiba_os_api_is_queue_orphan(const struct nvmeiba_atom_os_api *atom)
{
#if KS_REQUEST_QUEUE_HAS_REQUEST_FN
	return (atom->queue->make_request_fn == nvmeiba_b_req_push);
#else
	return (atom->disk->fops->submit_bio == nvmeiba_b_req_push);
#endif
}
EXPORT_SYMBOL(nvmeiba_os_api_is_queue_orphan);

int nvmeiba_os_api_orphan_abandon(struct nvmeiba_atom_os_api *atom)
{
#if KS_REQUEST_QUEUE_HAS_REQUEST_FN
	nvmeiba_os_apis_set_default_pops(&atom->disk->fops);
	atom->queue->make_request_fn = nvmeiba_b_req_push;	// queue becomes orphan, start buffering new requests
#else
	nvmeiba_os_apis_set_upgrade_pops(&atom->disk->fops);// Replace submit_bio with nvmeiba_b_req_push
#endif
	nvmeiba_os_apis_abandon_by(atom);
	return 0;
}
EXPORT_SYMBOL(nvmeiba_os_api_orphan_abandon);

struct nvmeiba_atom_os_api *nvmeiba_os_api_orphan_adopt(const char* dev_dir, const char *dev_name)
{
	struct nvmeiba_atom_os_api *orphan = nvmeiba_os_apis_adopt_by(dev_dir, dev_name);
	return orphan;
}
EXPORT_SYMBOL(nvmeiba_os_api_orphan_adopt);

#if KS_REQUEST_QUEUE_HAS_REQUEST_FN
static REQ_RET nvmeiba_b_req_reject(struct request_queue *q, struct bio *bio)
#else
static REQ_RET nvmeiba_b_req_reject(struct bio *bio)
#endif
{
#if KS_REQUEST_QUEUE_HAS_REQUEST_FN
	(void)q;
#endif
	bio_io_error(bio);
	return REQ_RET_ZERO;
}

int nvmeiba_os_api_set_detaching(struct nvmeiba_atom_os_api *atom)
{
#if KS_REQUEST_QUEUE_HAS_REQUEST_FN
	nvmeiba_os_apis_set_default_pops(&atom->disk->fops);
	atom->queue->make_request_fn = nvmeiba_b_req_reject;	// start rejecting new requests
#else
	nvmeiba_os_apis_set_detaching_pops(&atom->disk->fops);// Replace submit_bio with nvmeiba_b_req_reject
#endif
	return 0;
}
EXPORT_SYMBOL(nvmeiba_os_api_set_detaching);
