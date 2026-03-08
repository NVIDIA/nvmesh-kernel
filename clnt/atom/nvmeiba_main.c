/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeiba_main.h"
#include "nvmeiba_nvmesh_api.h"
#include "common/compat/kr_incs_time.h"
#include "utils/nvmeib_jdr/nvmeib_jdr.h"

MODULE_AUTHOR("NVIDIA CORPORATION");
MODULE_DESCRIPTION("nvmesh client hot upgrade core");
MODULE_LICENSE("GPL and additional rights");

/********** List of all active OS/API's, including unsafely detached *********/
static struct nvmeiba_all_os_apis all;

/******************************* Proc files ***********************************/
#define PROCFS_ATOM_STR "nvmeiba"
#define VERSION_PROC_FRMT_VER 1
#define __char_const(_s) ((char const *)(_s))
#define __const_stringfy(_s) __char_const(__stringify(_s))
static ssize_t fill_version_json(void *a, char *buffer, size_t len)
{
	struct charvec buf = {.base = buffer, .len = len};
	struct jdr jdr;
	struct charvec json;
	(void)a;

	jdr = jdr_make(buf);
	jdr_write_var(&jdr, module, (char const *)("atom"));
	jdr.ops.ascii_format(&jdr, "commit", "%llx", (u64)COMMIT_ID);
	jdr_write_var(&jdr, release, __const_stringfy(NVMESH_RELEASE));
	jdr_write_var(&jdr, version, __const_stringfy(NVMESH_VERSION));
	jdr_write_var(&jdr, build_number, __const_stringfy(BUILD_NUMBER));
	jdr_write_var(&jdr, distro, __const_stringfy(BUILD_DISTRO));
	json = jdr_finalize(&jdr);

	return json.len;
}

static ssize_t fill_users(void *a, char *buf, size_t len)
{
	ssize_t pos = 0;
	struct nvmeiba_atom_os_api *atom;
	(void)a;
	list_for_each_entry(atom, &all.list, list_all_os_apis) {
		pos += scnprintf(buf + pos, len - pos, "%s:", atom->dev_name);
		pos += nvmeiba_atom_users_to_string((void *)atom, buf + pos, len - pos);
	}
	return pos;
}


static int nvmeiba_os_apis_tostring(struct nvmeiba_all_os_apis* A, char *buf, int buf_len, char fmt);

static ssize_t fill_status_h(void *A, char *buffer, size_t len)
{
	#define BUF_ADD(...) count += scnprintf(buffer+count, len-count, __VA_ARGS__)
	int count = 0;
	count += nvmeiba_os_apis_tostring(A, buffer+count, len-count, 'H');
	return count;
}
static ssize_t fill_status_j(void *A, char *buffer, size_t len)
{
	int count = 0;
	count += nvmeiba_os_apis_tostring(A, buffer+count, len-count, 'J');
	#undef BUF_ADD
	return count;
}

#define RM_TINY_PROC_FILE(procfs_entry) ({	\
	if (procfs_entry) {						\
		nvmeiba_proc_remove(procfs_entry);	\
		(procfs_entry) = NULL;				\
	}										\
})

static void nvmeiba_procs_destroy(struct nvmeiba_all_os_apis* A)
{
	RM_TINY_PROC_FILE(A->proc.version_file);
	RM_TINY_PROC_FILE(A->proc.users_file);
	RM_TINY_PROC_FILE(A->proc.status_file);
	RM_TINY_PROC_FILE(A->proc.status_json);
	if (A->proc.dir) {
		remove_proc_entry(PROCFS_ATOM_STR, NULL);
		A->proc.dir = NULL;
	}
}

static int nvmeiba_procs_create(struct nvmeiba_all_os_apis* A)
{
	int rv = 0;
	if (!rv)
		rv = (A->proc.dir = proc_mkdir(PROCFS_ATOM_STR, NULL)) ? 0 : -1 ;
	if (!rv)
		rv = (A->proc.version_file = nvmeiba_proc_create("version", A->proc.dir, &fill_version_json, A)) ? 0 : -1;
	if (!rv)
		rv = (A->proc.users_file = nvmeiba_proc_create("users", A->proc.dir, &fill_users, A)) ? 0 : -1;
	if (!rv)
		rv = (A->proc.status_file = nvmeiba_proc_create("status", A->proc.dir, &fill_status_h, A)) ? 0 : -1;
	if (!rv)
		rv = (A->proc.status_json = nvmeiba_proc_create("status.json", A->proc.dir, &fill_status_j, A)) ? 0 : -1;
	if (rv)
		nvmeiba_procs_destroy(A);
	return rv;
}


static void __print_hooray(bool is_start, const char* mod_name)
{	/* Hooray */
	const char *ur = ((is_start) ? "registered" : "unregistered");		// Consider up/down
	struct timeval time;
	unsigned long local_time;
	struct rtc_time tm;
	do_gettimeofday(&time);
	local_time = (u32)(time.tv_sec - (sys_tz.tz_minuteswest * 60));
	rtc_time_to_tm(local_time, &tm);
	_NI_to_user(t_01_atom, "Module %s. Module: %s. Timestamp: (%04d-%02d-%02d %02d:%02d:%02d).\n", ur, mod_name, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec); // Hooray //. Error code: 0
}

static int __init nvmeiba_all_os_apis_init(void)
{
	struct nvmeiba_all_os_apis* A = &all;
	int rv;
	memset(A, 0, sizeof(*A));
	INIT_LIST_HEAD(&A->list);
	spin_lock_init(&A->lock);
	A->n.sub_osapi = A->n.nvmeibc = A->n.orphan_osapi = A->n.osapi = 0;
	rv = nvmeiba_procs_create(A);
	nvmeiba_os_apis_set_default_fops(&A->default_fops);
#if !KS_REQUEST_QUEUE_HAS_REQUEST_FN
	nvmeiba_os_apis_set_upgrade_fops(&A->upgrade_fops);
	nvmeiba_os_apis_set_detaching_fops(&A->detaching_fops);
#endif
	__print_hooray(true, PROCFS_ATOM_STR);
	return rv;
}
module_init(nvmeiba_all_os_apis_init);

static void __exit nvmeiba_all_os_apis_exit(void) /* Destructor */
{
	struct nvmeiba_all_os_apis* A = &all;
	const int n_lives = A->n.osapi, n_nvmeibc = A->n.nvmeibc;
	if ((n_lives != 0)||(n_nvmeibc != 0)) {
		const int buf_size = 4*PAGE_SIZE;
		char *printbuf = kzalloc(buf_size, GFP_KERNEL);	// Take one page
		WARN(true, A_DMESG_PREFIX "Unexpected internal error, operating system may become unstable.  Error code: 1007. Internal info (%d, %d)", n_lives, n_nvmeibc);
		/* Daniel, kernel should prevent service stop/ yum remove, etc so it is
		   impossible fo this if to occur. If it occurs on older kernels
		   disable os_api volume f_ops callbacks, coz nvmeiba code will unload
		   shortely */
		if (printbuf) {
			fill_status_h(A, printbuf, buf_size);
			_NE_to_user(t_02_atom, "Unexpected internal error,  Error code: 1008. Internal info %s.\n", printbuf);
			kfree(printbuf);
		}
	}
	nvmeiba_procs_destroy(A);
	__print_hooray(false, PROCFS_ATOM_STR);
}
module_exit(nvmeiba_all_os_apis_exit);

/*********************** API for List of all ATOMS ****************************/
#define BUF_ADD(...) pos += scnprintf(buf + pos, buf_len - pos, __VA_ARGS__)
static void _atom_attr_to_string(const struct nvmeiba_atom_os_api *atom, char *buf, int buf_len, bool is_fmt_human)
{
	const union nvmeiba_part_flags *f = &atom->sub.flags;
	const char *line_fmt = NULL;
	int pos = 0;

	line_fmt = (is_fmt_human) ? "flags=0x%x " : "\"flags\" :\"0x%x\", \"str_flags\" : [";
	BUF_ADD(line_fmt, f->all);
	if (f->is_sub_atom) {
		BUF_ADD("\"sub\"");
		if (f->is_sub_auto_resize)
			BUF_ADD(", \"alias\"");			// Nick name, mimics the carrier in every way, auto resizable with the carrier
	} else if (!list_empty(&atom->sub.part_list)) {
		BUF_ADD("\"car\"");	// is carrier
	} else {
		if (is_fmt_human) {
			buf[0] = (char)0; //BUF_ADD("");	// Empty description for regular volumes
		}
	}
	BUF_ADD((is_fmt_human) ? "" : "]");
}

/* Print live attached OS API's,
   If 'verbose' also print for each volume its opens and user processes*/
static int nvmeiba_os_apis_tostring(struct nvmeiba_all_os_apis* A, char *buf, int buf_len, char fmt)
{
	unsigned long flags;
	/*const*/ struct nvmeiba_atom_os_api *atom;
	int pos = 0, i = 0, n_total_opens = 0;
	const bool is_fmt_human = (fmt == 'H');
	const char *line_fmt = NULL;

	BUF_ADD((is_fmt_human) ? "" : "{\n");				// Add file prolog
	BUF_ADD((is_fmt_human) ? "" : "\t\"atoms\" : [\n");	// Add atoms array prolog

	spin_lock_irqsave(&A->lock, flags);
	list_for_each_entry(atom, &A->list, list_all_os_apis) {
		const int n_opens = atomic_read(&atom->users.n_opens);
		char attr[64];
		const char *disk_name;
		_atom_attr_to_string(atom, attr, sizeof(attr), is_fmt_human);
		spin_lock(&atom->disk_lock);					// Prevent atom->disk becoming NULL during detach. Note: disk always exists when (atom->status == nvmeiba_status_orphan)
		disk_name = ((atom->disk) ? atom->disk->disk_name : "null");
		line_fmt = (is_fmt_human) ?
			"%3d) %-32s |%-8s| opens=%4d, path=%-32s(0x%llx), atom_ptr=0x%llx, n_pend_bio=%u|%s" :
			"\t\t{\"index\" :%d, \"name\" :\"%s\", \"status\" :\"%s\", \"num_opens\" :%d, \"path\" :\"%s\", \"gendisk\" : \"0x%llx\", \"atom_ptr\" : \"0x%llx\", \"pend\" :{\"n_bio\" :%u},\n\t\t\t\"attr\" : {%s}";
		BUF_ADD(line_fmt,
				i++, atom->dev_name, nvmeiba_atom_get_string_status(atom),
				n_opens, disk_name, (u64)atom->disk, (u64)atom, atom->pender.n_bios, attr);
		if (atom->sub.flags.is_sub_atom) {
			ulong len_4kb = 0;					// Optional: user knows the length via 'lsblk | grep nvmesh'. Need to show only the offset
			len_4kb = (ulong)((atom->disk) ? (get_capacity(atom->disk) >> 3) : 0);
			line_fmt = (is_fmt_human) ?
				", offset=%lu[4k] len=%lu[4k], carrier_disk=0x%llx" :
				", \"offset_4k\" :%lu, \"len_4k\" :%lu, \"carrier_gendisk\" : \"0x%llx\"";
			BUF_ADD(line_fmt, (atom->sub.offset >> 12), len_4kb, (u64)atom->sub.parent->disk);
		}
		spin_unlock(&atom->disk_lock);
		BUF_ADD((is_fmt_human) ? "\n" : "},\n");
		n_total_opens += n_opens;
	}

	if (!is_fmt_human) {
		if (i > 0)
			pos -= 2;				// Remove prev ",\n"
		BUF_ADD("\n\t],\n");      	// Add atoms array epilog
	}

	line_fmt = (is_fmt_human) ?
		"nvmeiba_ptr=%p, num:{atoms=%d, orphans=%d, sub=%d, opens=%d, nvmeibc=%d}\n" :
		 "\t\"nvmeiba_ptr\": \"%p\",\n\t\"counters\": {\"atoms\": %d, \"orphans\": %d, \"sub\": %d, \"opens\" :%d, \"nvmeibc\" :%d},\n" ;
	BUF_ADD(line_fmt, A, A->n.osapi, A->n.orphan_osapi, A->n.sub_osapi, n_total_opens, A->n.nvmeibc);

	if (is_fmt_human) {
		list_for_each_entry(atom, &A->list, list_all_os_apis) {
			BUF_ADD("%s:", atom->dev_name);
			pos += nvmeiba_atom_users_to_string((void*)atom, buf+pos, buf_len-pos);
		}
	}
	spin_unlock_irqrestore(&A->lock, flags);

	if (!is_fmt_human) {
		pos -= 2;					// Remove prev ",\n"
		BUF_ADD("\n}\n");    	  	// Add atoms file epilog
	}
	WARN((pos >= buf_len), A_DMESG_PREFIX "Unexpected internal error, buffer too short. Some volumes may not report attachment correctly.  Error code: 1003.");  // Acts as _NE_to_user()
	#undef BUF_ADD
	return pos;
}

u64 nvmeiba_os_apis_get_version(void)
{
	return (u64)COMMIT_ID;
}
EXPORT_SYMBOL(nvmeiba_os_apis_get_version);

int nvmeiba_os_apis_get_num(unsigned char req_type)
{
	unsigned long flags;
	int res;
	spin_lock_irqsave(&all.lock, flags);
	switch (req_type) {
		case 'O' : res = all.n.orphan_osapi; break;
		case 'A' : res = all.n.osapi;        break;
		case 'C' : res = all.n.nvmeibc;      break;
		case 'S' : res = all.n.sub_osapi;    break;
		default  : res = -EINVAL;            break;
	}
	spin_unlock_irqrestore(&all.lock, flags);
	return res;
}
EXPORT_SYMBOL(nvmeiba_os_apis_get_num);

void nvmeiba_os_apis_add(struct nvmeiba_atom_os_api *os)
{
	struct nvmeiba_all_os_apis* A = &all;
	unsigned long flags;
	INIT_LIST_HEAD(&os->list_all_os_apis);
	spin_lock_irqsave(&A->lock, flags);
	if (A->n.osapi == 0) {							// Self reference on first attached atom, even if user space does not hold reference
		 __module_get(A->default_fops.owner);	// Can use THIS_MODULE as well. Daniel: Should use try_module_get(me), but here we know that module cannot be unloaded in this context
	}
	A->n.osapi++;
	if (os->sub.flags.is_sub_atom)
		A->n.sub_osapi++;
	list_add_tail(&os->list_all_os_apis, &A->list);
	spin_unlock_irqrestore(&A->lock, flags);
}

#define WARN_INCORRECT_COUNTERS(cond)       WARN(cond, A_DMESG_PREFIX     "Unexpected Internal error during hot upgrade. Machine reboot might be required. Error code: 1003. Internal information {%d,%d}.", A->n.osapi, A->n.orphan_osapi);  // Acts as _NE_to_user()
#define WARN_INCORRECT_STATUS(  cond, atom) WARN(cond, A_DMESG_PREFIX "%s: Unexpected Internal error during hot upgrade. Machine reboot might be required. Error code: 1007. Internal information {%d}.", atom->dev_name, atom->status);  // Acts as _NE_to_user()
void nvmeiba_os_apis_del(struct nvmeiba_atom_os_api *os)
{
	struct nvmeiba_all_os_apis* A = &all;
	unsigned long flags;
	spin_lock_irqsave(&A->lock, flags);
	if (os->status == nvmeiba_status_orphan)
		A->n.orphan_osapi--;
	if (os->sub.flags.is_sub_atom)
		A->n.sub_osapi--;
	A->n.osapi--;
	if (A->n.osapi == 0) {							// Put self reference on last detached atom
		module_put(A->default_fops.owner);		// Can use THIS_MODULE as well
	}
	list_del(&os->list_all_os_apis);
	WARN_INCORRECT_COUNTERS(A->n.osapi < A->n.orphan_osapi);
	spin_unlock_irqrestore(&A->lock, flags);
}

static bool __is_atom_in_dir(struct nvmeiba_atom_os_api *atom, const char* dev_dir, int dev_dir_len)
{
	if (atom->disk) {	// Otherwise Atom is detaching. Does not belong to any dir
		const char* name = atom->disk->disk_name;
		return (!dev_dir_len || // No directory
			(!strncmp(name, dev_dir, dev_dir_len) && (name[dev_dir_len] == '/')));
	}
	return false;
}

static bool __is_atom_matching(struct nvmeiba_atom_os_api *atom, const char* dev_dir, int dev_dir_len, const char *dev_name)
{
	if (atom->status != nvmeiba_status_orphan) return false;		// else atom->disk always exists, no need to take atom->disk_lock to guarantee that
	if (strcmp(atom->dev_name, dev_name)) return false;				// Compare full name without directory
	return __is_atom_in_dir(atom, dev_dir, dev_dir_len);			// Not found, Correct volume in different directory (different client instance), Daniel: Comparison of only disk_name is not good enough because it can get truncated
}

void nvmeiba_os_api_exec_for_each_atom(const char* dev_dir, void (*fn)(const struct nvmeiba_atom_os_api *atom, void *ctx), void* ctx)
{
	struct nvmeiba_all_os_apis* A = &all;
	struct nvmeiba_atom_os_api *atom;
	unsigned long flags;
	const int dev_dir_len = (dev_dir ? strlen(dev_dir) : 0);
	spin_lock_irqsave(&A->lock, flags);
	list_for_each_entry(atom, &A->list, list_all_os_apis) {
		const bool does_belong_to_nvmeibc = __is_atom_in_dir(atom, dev_dir, dev_dir_len);
		const bool should_expose_detaching = true && (atom->status == nvmeiba_status_detaching);	// nvmeibc cannot attach to those atoms, but maybe it wants to report them to mgmt so at least make them visible
		if (does_belong_to_nvmeibc || should_expose_detaching)
			fn(atom, ctx);
	}
	spin_unlock_irqrestore(&A->lock, flags);
}
EXPORT_SYMBOL(nvmeiba_os_api_exec_for_each_atom);

struct nvmeiba_atom_os_api *nvmeiba_os_apis_adopt_by(const char* dev_dir, const char *dev_name)
{
	struct nvmeiba_all_os_apis* A = &all;
	struct nvmeiba_atom_os_api *atom, *rv = NULL;
	unsigned long flags;
	const int dev_dir_len = (dev_dir ? strlen(dev_dir) : 0);

	spin_lock_irqsave(&A->lock, flags);
	list_for_each_entry(atom, &A->list, list_all_os_apis) {
		if (__is_atom_matching(atom, dev_dir, dev_dir_len, dev_name)) {
			rv = atom;
			goto _out;
		}
	}
_out:
	if (rv) {
		A->n.orphan_osapi--;
		if (atom->sub.flags.is_sub_atom) {
			/* Adopting sub volume as a result of adoption of volume*/
		}
		WARN((rv->status != nvmeiba_status_orphan), A_DMESG_PREFIX "%s: Unexpected Internal error during hot upgrade. Machine reboot might be required. Error code: 1002. Internal information {%s,%d}.", rv->dev_name, (dev_dir ? dev_dir : "/"), rv->status);  // Acts as _NE_to_user()
	}
	WARN_INCORRECT_COUNTERS(A->n.orphan_osapi < 0);
	spin_unlock_irqrestore(&A->lock, flags);
	return rv;
}

void nvmeiba_os_apis_abandon_by(struct nvmeiba_atom_os_api *atom)
{
	struct nvmeiba_all_os_apis* A = &all;
	unsigned long flags;
	spin_lock_irqsave(&A->lock, flags);
	WARN_INCORRECT_STATUS((atom->queue && !nvmeiba_os_api_is_queue_orphan(atom)), atom);		// Verify correctness. sub volumes dont have request queue on atom
	WARN_INCORRECT_STATUS((atom->status == nvmeiba_status_orphan), atom);							// Only abandoning, cannot already be abandoned
	A->n.orphan_osapi++;
	WARN_INCORRECT_COUNTERS(A->n.osapi < A->n.orphan_osapi);
	spin_unlock_irqrestore(&A->lock, flags);
}

struct nvmeiba_to_c_handover nvmeiba_os_do_on_nvmeibc_up(void)
{
	struct nvmeiba_all_os_apis* A = &all;
	unsigned long flags;
	struct nvmeiba_to_c_handover H;
	struct nvmeiba_all_debug_cntrs n;
	spin_lock_irqsave(&A->lock, flags);
	A->n.nvmeibc++;
	H.n_orphan_osapi = A->n.orphan_osapi;
	n = A->n;								// Cache on stack to print outside spinlock
	H.fops = &A->default_fops;				// KERNEL 5.10+ Default fops include NULL submit_bio, nvmeibc will replace it with a functional submit_bio
	spin_unlock_irqrestore(&A->lock, flags);
	H.protocol_version = NVMEIBA_2_C_PROTO_VERSION_V_2_0;
	if (H.n_orphan_osapi)
		_NI_to_user(t_04_atom, "Successful hot upgrade handshake between modules nvmeiba and nvmeibc. Internal information {%u/%u/%d/%d}", H.n_orphan_osapi, n.osapi, n.sub_osapi, n.nvmeibc);   //. Error code: 0
	return H;
}
EXPORT_SYMBOL(nvmeiba_os_do_on_nvmeibc_up);

void nvmeiba_os_do_on_nvmeibc_down(void)
{
	struct nvmeiba_all_os_apis* A = &all;
	const struct nvmeiba_atom_os_api *atom;
	unsigned long flags;
	bool are_all_orphans;
	spin_lock_irqsave(&A->lock, flags);
	are_all_orphans = (A->n.orphan_osapi == A->n.osapi);
	A->n.nvmeibc--;

	if (A->n.orphan_osapi) {
		_NI_to_user(t_05_atom, "Hot upgrade of nvmeibc module started. Internal information {%u/%u/%d/%d}\n", A->n.orphan_osapi, A->n.osapi, A->n.sub_osapi, A->n.nvmeibc);   //. Error code: 0
		if (A->n.nvmeibc == 0) {
			WARN_INCORRECT_COUNTERS(!are_all_orphans);
		}
	}
	if (A->n.osapi && are_all_orphans) {		// Just verify, for debug
		list_for_each_entry(atom, &A->list, list_all_os_apis) {
			WARN_INCORRECT_STATUS((atom->status != nvmeiba_status_orphan), atom);
		}
	}
	spin_unlock_irqrestore(&A->lock, flags);
	_NI_to_user(t_06_atom, "nvmeibc instance successfully disconnected from nvmeiba module. %d instances remaining\n", A->n.nvmeibc);   //. Error code: 0
}
EXPORT_SYMBOL(nvmeiba_os_do_on_nvmeibc_down);

void nvmeiba_os_apis_set_default_pops(const struct block_device_operations **fops)
{
	(*fops) = &all.default_fops;
}
#if !KS_REQUEST_QUEUE_HAS_REQUEST_FN
void nvmeiba_os_apis_set_upgrade_pops(const struct block_device_operations **fops)
{
	(*fops) = &all.upgrade_fops;
}

void nvmeiba_os_apis_set_detaching_pops(const struct block_device_operations **fops)
{
	(*fops) = &all.detaching_fops;
}
#endif
