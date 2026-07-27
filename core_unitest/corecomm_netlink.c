/**
 * This files provide corecomm library interface implementaion over netlink
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/netlink.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifdef SYS_gettid
#define __gettid() syscall(SYS_gettid);
#else
#error "SYS_gettid unavailable on this system"
#endif
#define CONTROL_PROC "/proc/nvmeibc/corecomm"
#define INVALID_HANDLE ((corecomm_handle)0)

/* Interface we are going to implement */
#include "corecomm.h"
#include "corecomm_netlink_if.h"

static unsigned __msg_seq = 0;
#define GET_MSG_SEQ() __atomic_add_fetch(&__msg_seq, 1, __ATOMIC_SEQ_CST)
static __thread long __local_errno;
#define __set_local_errno(val)                                                 \
	({                                                                         \
		errno         = val;                                                   \
		__local_errno = val;                                                   \
	})
#define __set_local_errno_return_put(__self, rv)                               \
	{                                                                          \
		__set_local_errno(errno);                                              \
		put_self(__self);                                                      \
		return rv;                                                             \
	}

/** Client side RPC declaration
 * @TODO: Move to a separate compilation unit
 * No body is required, automatic.
 * @param api Literal, api function name
 * @param rsptype Response type, will be the last argument
 * @param rspname Response variable name, important to generation of right debug
 * @param ... Arguments list of the form (type,name),...
 * symbols
 */
#define NLRPC_CLNT(api, rsptype, rspname, ...)                                       \
	struct nlrpc_inp(api){                                                           \
	    nlrpc_foreach(nlrpc_unwrap_tuple_semi, ##__VA_ARGS__)};                      \
	static int api(corecomm_handle handle,                                           \
	               rsptype *rspname nlrpc_foreach(                                   \
	                   nlrpc_unwrap_tuple_comma_const, ##__VA_ARGS__)) {             \
		struct corecomm_nl *__self = get_self_(handle);                              \
		struct nlmsghdr *__nlh;                                                      \
		if (!__self) {                                                               \
			errno = EBADF;                                                           \
			__set_local_errno(errno);                                                \
			return -1;                                                               \
		}                                                                            \
		__nlh = calloc(NLMSG_SPACE(sizeof(struct nlrpc_inp(api))), 1);               \
		if (__nlh) {                                                                 \
			struct iovec __iov;                                                      \
			struct msghdr __msg;                                                     \
			struct nlrpc_inp(api) * data;                                            \
			__nlh->nlmsg_len  = NLMSG_SPACE(sizeof(struct nlrpc_inp(api)));          \
			__nlh->nlmsg_pid  = getpid();                                            \
			__nlh->nlmsg_type = nlrpc_enum_name(api);                                \
			__nlh->nlmsg_seq  = GET_MSG_SEQ();                                       \
			__iov             = (struct iovec){__nlh, __nlh->nlmsg_len, /*0*/};      \
			__msg             = (struct msghdr){&__self->dst_addr,                   \
                                    sizeof(__self->dst_addr), &__iov, 1, \
                                    /*0*/};                              \
			data              = NLMSG_DATA(__nlh);                                   \
			nlrpc_foreach(nlrpc_unwrap_tuple_memcpy, __VA_ARGS__);                   \
			if (sendmsg(__self->sock_fd, &__msg, 0) < 0) {                           \
				int __errno = errno;                                                 \
				free(__nlh);                                                         \
				errno = __errno;                                                     \
				__set_local_errno_return_put(__self, -1);                            \
			}                                                                        \
			free(__nlh);                                                             \
			{                                                                        \
				size_t __len;                                                        \
				char __rcv_buf[4 * 4096];                                            \
				struct iovec __rcv_iov =                                             \
				    (struct iovec){__rcv_buf, sizeof(__rcv_buf), /*0*/};             \
				struct msghdr __rcv_msg =                                            \
				    (struct msghdr){&__self->src_addr,                               \
				                    sizeof(__self->src_addr), &__rcv_iov, 1,         \
				                    /*0*/};                                          \
				struct nlmsghdr *__nlh_rcv = (struct nlmsghdr *)__rcv_buf;           \
				if ((__len = recvmsg(__self->sock_fd, &__rcv_msg, 0)) < 0)           \
					__set_local_errno_return_put(__self, -1);                        \
				if (!NLMSG_OK(__nlh_rcv, __len)) {                                   \
					errno = EIO;                                                     \
					__set_local_errno_return_put(__self, -1);                        \
				}                                                                    \
				if (__nlh_rcv->nlmsg_type ==                                         \
				    NLMSG_ERROR) { /* Error from kernel */                           \
					int *__rsp = NLMSG_DATA(__nlh_rcv);                              \
					errno      = *__rsp;                                             \
					__set_local_errno_return_put(__self, -1);                        \
				} else { /* Command was sent, here is the reply */                   \
					memcpy(rspname, NLMSG_DATA(__nlh_rcv), sizeof(rsptype));         \
					__set_local_errno_return_put(__self, 0);                         \
				}                                                                    \
			}                                                                        \
		} else {                                                                     \
			__set_local_errno_return_put(__self, -1); /*errno=ENOMEM*/               \
		}                                                                            \
	}

#define CORECOMM_MAX_RECV_BUFFER_SIZE 4096 /* For now */

/**
 * Netlink specific definition of struct corecomm. Use typedef for implementaion
 * specific operations, or struct corecomm for type agnostic.
 */
typedef struct corecomm_nl {
	int sock_fd;
	int control_proc_fd;
	int is_dead;
	pthread_t monitor_thread;
	pthread_mutex_t in_use_mutex;
	struct sockaddr_nl src_addr, dst_addr;
} corecomm_nl_t;

/** Utility get object from handle */
static struct corecomm_nl *get_self_(corecomm_handle h) {
	struct corecomm_nl *self = (void *)h;
	if (!self) return NULL;
	pthread_mutex_lock(&self->in_use_mutex);
	if (self->is_dead) {
		/* Already closed */
		pthread_mutex_unlock(&self->in_use_mutex);
		return NULL;
	}
	return self;
}
void put_self(struct corecomm_nl *self) {
	if (!self) return;
	pthread_mutex_unlock(&self->in_use_mutex);
}

/******************* Global vaiables *******************/

/******************* Implementation *******************/

long corecomm_errno(void) { return __local_errno; }

void *__control_proc_monitor_thread(void *param) {
	struct corecomm_nl *self = param;
	/* We spin until either closed by kernel or by user */
	while (!self->is_dead && !read(self->control_proc_fd, NULL, 0)) {}
	printf("Terminating\n");
	pthread_mutex_lock(&self->in_use_mutex);
	self->is_dead = 1;
	pthread_mutex_unlock(&self->in_use_mutex);
	/* From this point, nobody can use the handle's file destricptors, so we can
	 * free them. We cannot free the memory yet. The handle is stale, awaiting
	 * final destruction.
	 */
	close(self->control_proc_fd);
	close(self->sock_fd);

	return NULL;
}

corecomm_handle corecomm_create() {
	/* calloc will also memset 0, it is important */
	struct corecomm_nl *self = NULL;
	int rv;

	self = calloc(sizeof(struct corecomm_nl), 1);
	if (!self) goto err;

	/* Start by opening the control proc */
	if ((self->control_proc_fd = open(CONTROL_PROC, O_RDONLY)) < 0) goto err;

	if ((self->sock_fd = socket(PF_NETLINK, SOCK_RAW, NETLINK_CORECOMM)) < 0)
		goto err;

	self->src_addr.nl_family = AF_NETLINK;
	self->src_addr.nl_pid    = getpid(); /* self pid */

	if (bind(self->sock_fd, (struct sockaddr *)&self->src_addr,
	         sizeof(self->src_addr)) < 0)
		goto err;

	self->dst_addr.nl_family = AF_NETLINK;
	self->dst_addr.nl_pid    = 0; /* 0 = kernel */
	self->dst_addr.nl_groups = 0; /* 0 = unicast */

	if ((rv = pthread_mutex_init(&self->in_use_mutex, NULL))) goto err;

	/* Last create monitor thread */
	if ((rv = pthread_create(&self->monitor_thread, NULL,
	                         __control_proc_monitor_thread, self))) {
		int _errno = errno;
		pthread_mutex_destroy(&self->in_use_mutex);
		errno = _errno;
		goto err;
	}

	return (corecomm_handle)self;

err:
	do { /* We are here only if we failed before monitor thread is created */
		int _errno = errno;
		if (self) {
			if (self->control_proc_fd) close(self->control_proc_fd);
			if (self->sock_fd) close(self->sock_fd);
			free(self);
		}
		errno = _errno;
	} while (0);
	__set_local_errno(errno);
	return INVALID_HANDLE;
}

void corecomm_destroy(corecomm_handle handle) {
	struct corecomm_nl *self = get_self_(handle);
	if (self) {
		self->is_dead = 1;
		put_self(self);
		/* At this point self aways exists, but not necessarily alive. It is
		 * safe to access its members. */
		pthread_join(self->monitor_thread, NULL);
		pthread_mutex_destroy(&self->in_use_mutex);
		free(self);
	}
}

NLRPC_CLNT(corecomm_format_local_disk_, struct corecomm_new_format_info, rsp,
           (struct corecomm_format_disk, fd));
value_or_minus_1
corecomm_format_local_disk(corecomm_handle handle,
                           struct corecomm_format_disk fd,
                           struct corecomm_new_format_info *output) {
	return corecomm_format_local_disk_(handle, output, fd);
}

NLRPC_CLNT(corecomm_set_symbol_, int, dummy, (long_name_t, sym_name),
           (int, size), (struct page_container, value), (int, deref));
value_or_minus_1 corecomm_set_symbol(corecomm_handle handle,
                                     const char *sym_name, int size,
                                     struct page_container *value, int deref) {
	int dummy;
	return corecomm_set_symbol_(handle, &dummy, sym_name, size, *value, deref);
}

NLRPC_CLNT(corecomm_freeze_, int, dummy, (name_t, disk));
value_or_minus_1 corecomm_freeze(corecomm_handle handle,
                                 const char *disk_name) {
	int dummy;
	return corecomm_freeze_(handle, &dummy, disk_name);
}

NLRPC_CLNT(corecomm_unfreeze_, int, dummy, (name_t, disk));
value_or_minus_1 corecomm_unfreeze(corecomm_handle handle,
                                   const char *disk_name) {
	int dummy;
	return corecomm_unfreeze_(handle, &dummy, disk_name);
}

NLRPC_CLNT(corecomm_read_symbol_, struct page_container, output,
           (long_name_t, sym_name), (int, size), (int, deref));
value_or_minus_1 corecomm_read_symbol(corecomm_handle handle,
                                      const char *sym_name, int size, int deref,
                                      struct page_container *output) {
	return corecomm_read_symbol_(handle, output, sym_name, size, deref);
}

NLRPC_CLNT(corecomm_register_arnic_, int, dummy, (name_t, node_id),
           (name_t, gid), (unsigned short, pkey), (enum rdma_type, type));
value_or_minus_1 corecomm_register_arnic(corecomm_handle handle,
                                         const char *node_id, const char *gid,
                                         unsigned short pkey,
                                         enum rdma_type type) {
	int dummy;
	return corecomm_register_arnic_(handle, &dummy, node_id, gid, pkey, type);
}

NLRPC_CLNT(corecomm_discover_, cdisk_handle, output, (name_t, disk_name),
           (name_t, node_id));
cdisk_handle corecomm_discover(corecomm_handle handle, const char *disk_name,
                               const char *node_id) {
	cdisk_handle output;
	if (corecomm_discover_(handle, &output, disk_name, node_id)) return -1;
	return output;
}

NLRPC_CLNT(corecomm_disk_remove_, int, dummy, (cdisk_handle, disk));
value_or_minus_1 corecomm_disk_remove(corecomm_handle handle,
                                      cdisk_handle disk) {
	int dummy;
	return corecomm_disk_remove_(handle, &dummy, disk);
}

NLRPC_CLNT(corecomm_alloc_ndb_, ndb_handle, rsp, (unsigned int, n_pages));
ndb_handle corecomm_alloc_ndb(corecomm_handle handle, unsigned long n_pages) {
	ndb_handle rsp;
	if (corecomm_alloc_ndb_(handle, &rsp, n_pages)) return -1;
	return rsp;
}

NLRPC_CLNT(corecomm_free_ndb_, int, dummy, (ndb_handle, ndb));
value_or_minus_1 corecomm_free_ndb(corecomm_handle handle, ndb_handle ndb) {
	int dummy;
	return corecomm_free_ndb_(handle, &dummy, ndb);
}

NLRPC_CLNT(corecomm_stamp_ndb_, int, dummy, (ndb_handle, ndb),
           (data_stamps_arr_t, stamps_arr), (unsigned int, n_stamps),
           (unsigned int, offset_page), (unsigned int, offset_bytes));

NLRPC_CLNT(corecomm_stamp_ndb_md_, int, dummy, (ndb_handle, ndb),
           (data_stamps_arr_t, stamps_arr), (unsigned int, n_stamps),
           (unsigned int, offset_page));

value_or_minus_1 corecomm_stamp_ndb(corecomm_handle handle, ndb_handle ndb,
                                    unsigned long *stamp_array,
                                    unsigned int n_stamps,
                                    unsigned int offset_page,
                                    unsigned int offset_bytes, int is_md) {
	int dummy;
	data_stamps_arr_t arr;
	if (n_stamps * sizeof(unsigned long) > sizeof(arr)) {
		errno = EINVAL;
		__set_local_errno(errno);
		return -1;
	};
	memcpy(arr, stamp_array, n_stamps * sizeof(unsigned long));
	if (is_md) {
		return corecomm_stamp_ndb_md_(handle, &dummy, ndb, arr, n_stamps,
		                              offset_page);
	} else {
		return corecomm_stamp_ndb_(handle, &dummy, ndb, arr, n_stamps,
		                           offset_page, offset_bytes);
	}
}

NLRPC_CLNT(corecomm_read_stamp_ndb_, struct data_stamps_arr_container, rsp,
           (ndb_handle, ndb), (unsigned int, n_stamps),
           (unsigned int, offset_page), (unsigned int, offset_bytes));

NLRPC_CLNT(corecomm_read_stamp_ndb_md_, struct data_stamps_arr_container, rsp,
           (ndb_handle, ndb), (unsigned int, n_stamps),
           (unsigned int, offset_page));

value_or_minus_1
corecomm_read_stamps_ndb(corecomm_handle handle, ndb_handle ndb,
                         unsigned long *stamp_array, unsigned int n_stamps,
                         unsigned int offset_page, unsigned int offset_bytes,
                         int is_md) {
	struct data_stamps_arr_container rsp;
	int rv;
	if (n_stamps * sizeof(unsigned long) > sizeof(rsp)) {
		errno = EINVAL;
		__set_local_errno(errno);
		return -1;
	};
	if (is_md) {
		rv = corecomm_read_stamp_ndb_md_(handle, &rsp, ndb, n_stamps,
		                                 offset_page);
	} else {
		rv = corecomm_read_stamp_ndb_(handle, &rsp, ndb, n_stamps, offset_page,
		                              offset_bytes);
	}
	if (!rv) memcpy(stamp_array, rsp.data, n_stamps * sizeof(unsigned long));
	return rv;
}

NLRPC_CLNT(corecomm_pd_cmpxchg_, struct lock_data, output, (cdisk_handle, disk),
           (unsigned long long, addr), (unsigned long long, compare),
           (unsigned long long, exchange));

value_or_minus_1 corecomm_pd_cmpxchg(corecomm_handle handle, cdisk_handle disk,
                                     unsigned long long addr,
                                     unsigned long long compare,
                                     unsigned long long exchange,
                                     struct lock_data *output) {
	return corecomm_pd_cmpxchg_(handle, output, disk, addr, compare, exchange);
}

NLRPC_CLNT(corecomm_pd_read_lock_, struct lock_data, output,
           (cdisk_handle, disk), (unsigned long long, addr));

value_or_minus_1 corecomm_pd_read_lock(corecomm_handle handle,
                                       cdisk_handle disk,
                                       unsigned long long addr,
                                       struct lock_data *output) {
	return corecomm_pd_read_lock_(handle, output, disk, addr);
}

NLRPC_CLNT(corecomm_pd_write_blkset_info_, struct lock_data, output,
           (cdisk_handle, disk), (unsigned long long, addr),
           (unsigned long long, bi));
value_or_minus_1 corecomm_pd_write_blkset_info(corecomm_handle handle,
                                               cdisk_handle disk,
                                               unsigned long long addr,
                                               unsigned long long bi,
                                               struct lock_data *output) {
	return corecomm_pd_write_blkset_info_(handle, output, disk, addr, bi);
}

NLRPC_CLNT(corecomm_direct_read_lock_, struct lock_data, output,
           (name_t, disk_name), (unsigned long long, addr));

value_or_minus_1 corecomm_direct_read_lock(corecomm_handle handle,
                                           const char *disk_name,
                                           unsigned long long addr,
                                           struct lock_data *output) {
	return corecomm_direct_read_lock_(handle, output, disk_name, addr);
}

NLRPC_CLNT(corecomm_nvmeibc_pd_io_, struct lock_data, output,
           (cdisk_handle, cdisk), (ndb_handle, ndb), (unsigned long long, addr),
           (unsigned long long, len), (enum io_type, io_type),
           (int, block /*block = 1, journal = 0*/), (int, has_piggy_back),
           (unsigned long long, pb_addr), (int, sub_block));

value_or_minus_1
corecomm_pd_execute_io_blocks(corecomm_handle handle, cdisk_handle disk,
                              ndb_handle ndb, unsigned long long addr,
                              unsigned long long len, enum io_type io_type,
                              int has_piggy_back, unsigned long long pb_addr,
                              int sub_block, struct lock_data *output) {
	return corecomm_nvmeibc_pd_io_(handle, output, disk, ndb, addr, len,
	                               io_type, 1, has_piggy_back, pb_addr,
	                               sub_block);
}

value_or_minus_1 corecomm_pd_execute_io_jour_blocks(
    corecomm_handle handle, cdisk_handle disk, ndb_handle ndb,
    unsigned long long addr, unsigned long long len, enum io_type io_type,
    int has_piggy_back, unsigned long long pb_addr, int sub_block,
    struct lock_data *output) {
	return corecomm_nvmeibc_pd_io_(handle, output, disk, ndb, addr, len,
	                               io_type, 0, has_piggy_back, pb_addr,
	                               sub_block);
}

NLRPC_CLNT(corecomm_pd_get_blkset_problems_, union problems_report_container,
           output, (cdisk_handle, cdisk), (unsigned long long, start),
           (unsigned long long, len), (int, get_dbits), (int, get_stales));

value_or_minus_1 corecomm_pd_get_blkset_problems(
    corecomm_handle handle, cdisk_handle disk, unsigned long long start,
    unsigned long long len, int get_dbits, int get_stales,
    union problems_report_container *output) {
	return corecomm_pd_get_blkset_problems_(handle, output, disk, start, len,
	                                        get_dbits, get_stales);
}

NLRPC_CLNT(corecomm_pd_jmdc_read_, int, dummy, (cdisk_handle, cdisk),
           (unsigned int, start_rng), (unsigned int, num_rng),
           (int, dirty_only), (corecomm_userspace_ptr, output));
value_or_minus_1 corecomm_pd_jmdc_read(corecomm_handle handle,
                                       cdisk_handle disk,
                                       unsigned int start_rng,
                                       unsigned int num_rng, int dirty_only,
                                       struct corecomm_jmdc_container *output) {
	int dummy;
	int rv = corecomm_pd_jmdc_read_(handle, &dummy, disk, start_rng, num_rng,
	                                dirty_only, (corecomm_userspace_ptr)output);
	return rv;
}

NLRPC_CLNT(corecomm_gen_blkset_recovered_, int, rsp, (cdisk_handle, cdisk),
           (name_t, client_uuid), (name_t, sgmnt_uuid),
           (unsigned int, slice_size), (unsigned long, jrange),
           (unsigned long, jentry), (int, pass2toma));

value_or_minus_1
corecomm_gen_blkset_recovered(corecomm_handle handle, cdisk_handle disk,
                              const char *client_uuid, const char *sgmnt_uuid,
                              unsigned int slice_size, unsigned long jrange,
                              unsigned long jentry, int pass2toma) {
	int dummy;
	return corecomm_gen_blkset_recovered_(handle, &dummy, disk, client_uuid,
	                                      sgmnt_uuid, slice_size, jrange,
	                                      jentry, pass2toma);
}

NLRPC_CLNT(corecomm_gen_get_uuid_jour_, struct corecomm_jmdc_range, output,
           (cdisk_handle, cdisk), (name_t, client_uuid), (name_t, sgmnt_uuid));

value_or_minus_1
corecomm_gen_get_uuid_jour(corecomm_handle handle, cdisk_handle disk,
                           name_t client_uuid, name_t sgmnt_uuid,
                           struct corecomm_jmdc_range *output) {
	return corecomm_gen_get_uuid_jour_(handle, output, disk, client_uuid,
	                                   sgmnt_uuid);
}

NLRPC_CLNT(corecomm_gen_jentry_erase_, int, output, (cdisk_handle, cdisk),
           (unsigned short, jentry), (unsigned char, jentry_gen_id));

value_or_minus_1 corecomm_gen_jentry_erase(corecomm_handle handle,
                                           cdisk_handle disk,
                                           unsigned short jentry,
                                           unsigned char jentry_gen_id,
                                           int *output) {
	return corecomm_gen_jentry_erase_(handle, output, disk, jentry,
	                                  jentry_gen_id);
}

NLRPC_CLNT(corecomm_pd_free_jrnl_ents_, int, output, (cdisk_handle, cdisk),
           (name_t, seg_uuid), (unsigned long long, start_blkset_lba),
           (unsigned long long, len_blksets), (int, pass2toma),
           (unsigned long long, lock_entry_raw), (name_t, serjio_boot_id),
           (unsigned int, jrange), (unsigned int, jentry),
           (unsigned char, jentry_gen_id), (int, num_ents));

value_or_minus_1 corecomm_pd_free_jrnl_ents(
    corecomm_handle handle, cdisk_handle disk, const char *seg_uuid,
    unsigned long long start_blkset_lba, unsigned long long len_blksets,
    int pass2toma, unsigned long long lock_entry_raw,
    const char *serjio_boot_id, unsigned int jrange, unsigned int jentry,
    unsigned char jentry_gen_id, int num_ents, int *output) {
	return corecomm_pd_free_jrnl_ents_(handle, output, disk, seg_uuid,
	                                   start_blkset_lba, len_blksets, pass2toma,
	                                   lock_entry_raw, serjio_boot_id, jrange,
	                                   jentry, jentry_gen_id, num_ents);
}

NLRPC_CLNT(corecomm_pd_please_kill_yourself_, int, output,
           (cdisk_handle, cdisk), (unsigned int, rsc_id),
           (unsigned long long, dlba));

value_or_minus_1 corecomm_pd_dbg_please_kill_yourself(corecomm_handle handle,
                                                      cdisk_handle disk,
													  unsigned int rsc_id,
                                                      unsigned long long dlba) {
	int dummy;
	return corecomm_pd_please_kill_yourself_(handle, &dummy, disk, rsc_id, dlba);
}

NLRPC_CLNT(corecomm_alloc_jrnls_, struct corecomm_lbas_set, output,
           (int, n_disks), (struct corecomm_disks_set, disks), (int, txid),
           (struct corecomm_lbas_set, dlbas));
value_or_minus_1 corecomm_alloc_jrnls(corecomm_handle handle, int n_disks,
                                      struct corecomm_disks_set *disks,
                                      int txid, struct corecomm_lbas_set *dlbas,
                                      struct corecomm_lbas_set *output) {
	return corecomm_alloc_jrnls_(handle, output, n_disks, *disks, txid, *dlbas);
}

NLRPC_CLNT(corecomm_free_jrnls_, int, output, (int, n_disks),
           (struct corecomm_disks_set, disks),
           (struct corecomm_lbas_set, jlbas), (unsigned int, wr_sts_bm));
value_or_minus_1 corecomm_free_jrnls(corecomm_handle handle, int n_disks,
                                     struct corecomm_disks_set *disks,
                                     struct corecomm_lbas_set *jlbas,
                                     unsigned int wr_sts_bm) {
	int dummy;
	return corecomm_free_jrnls_(handle, &dummy, n_disks, *disks, *jlbas,
	                            wr_sts_bm);
}

NLRPC_CLNT(corecomm_jam_lba_2_idx_, int, output, (cdisk_handle, cdisk),
           (unsigned long long, lba));
value_or_minus_1 corecomm_jam_lba_2_idx(corecomm_handle handle,
                                       cdisk_handle cdisk,
                                       unsigned long long lba, int *output) {
	return corecomm_jam_lba_2_idx_(handle, output, cdisk, lba);
}

/* This is only needed in order for the compiler not to optimize out
   corecomm_jblock_md from debug symbols. Didn't find a pragma for that. */
void corecomm_dummy__1(union corecomm_jblock_md dummy) {
	(void)dummy;
}
