/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include "nvmeibt_srvr_proc.h"
#include "nvmeibt_common.h"
#include "utils/nvmeibt_str.h"

static ssize_t __nvmeibt_pwrite_atomic(int fd, const void *vptr, size_t size, int OK_err_1, int OK_err_2)
{
	const ssize_t n_bytes_written = pwrite(fd, vptr, size, 0 /*offset*/);
	if (n_bytes_written < 0) {
		const ssize_t rv = -errno;
		if (errno == OK_err_1 || errno == OK_err_2) {
			N_Tf(6sjhk20, "Failed pwrite(fd=@FD vptr=@PTR size=@SIZEOF) (@AUTO_ERRNO))", fd, vptr, size);
		} else {
			N_Wf(35s83jm, "Failed pwrite(fd=@FD vptr=@PTR size=@SIZEOF) (@AUTO_ERRNO))", fd, vptr, size);
		}
		errno = 0;
		return rv;
	} else if ((size_t) n_bytes_written != size) {
		N_Tf(rvsx83j, "Partial pwrite(fd=@FD vptr=@PTR size=@SIZEOF) (@AUTO_ERRNO) wrote rv=@ZX", fd, vptr, size, n_bytes_written);
		errno = 0;
		return -1;
	}
	return 0;		// Success
}

#define NNVMEIBT_PWRITE_ATOMIC(name, __fd, __buf, __n, _OK_err_1, _OK_err_2) ({					\
	ssize_t		__rv__;																						\
	__MEASURE_TOOK_INIT();																					\
	__rv__ = __nvmeibt_pwrite_atomic((__fd), (__buf), (__n), (_OK_err_1), (_OK_err_2));			\
	__MEASURE_TOOK(N_IMf(name, "pwrite(@FD) Took @LLD ms", (__fd), NSEC_TO_MSEC(__measure_took_time_took_nsec)));	\
	__rv__;																									\
})

/***************************** mmap shared memory (server /proc/.../toma_status/files & IO locks table) *******************************/
#include <sys/mman.h>
/* mmap wrapper that creates a protected page before and after the allocation. Must be freed using nvmeibt_munmap(),
	This interface should seem as if the original mmap was used but with the added protection given by the extra protected pages  */
static inline size_t padded_mmap_length(size_t length) { return length + 2 * PAGE_SIZE; }

#define PADDED_MMAP_MAGIC_NUM 0x726f656568657265LLU		// MAGIC cookie ("roeehere") to protect against buffer overrun in the first page
struct padded_mmap_magic_number {
	uint64_t magic_num;
	void *addr;
	size_t length;
};

static inline void init_padded_mmap_magic_number_struct(struct padded_mmap_magic_number *me, size_t length)
{
	me->addr = (void*)me;
	me->length = length;
	me->magic_num = PADDED_MMAP_MAGIC_NUM;
}

static void *nvmeibt_mmap(size_t length, int fd, uint64_t offset, bool allow_write)
{
	const size_t padded_length = padded_mmap_length(length);
	void *mapped_padded = mmap(NULL, padded_length, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);	// Allocating 2 pages more than length requested with no read/write access
	void *mapped = MAP_FAILED;

	N_Tf(salddbmmfr, "length=@ZX, fd=@FD, offset=@ZX, write=@BOOL_YN", length, fd, offset, allow_write);
	if (mapped_padded == MAP_FAILED) {
		N_Ef(salddbmmf0, "(length=@ZX, fd=@FD) failed on mmap().", length, fd);
		return MAP_FAILED;
	}

	if (mprotect(mapped_padded, PAGE_SIZE, PROT_WRITE) >= 0) {		// Modify permissions for the first guard page only
		init_padded_mmap_magic_number_struct(mapped_padded, padded_length);			// Writing to the first page details about the allocation and setting it back to no access permissions
		if (mprotect(mapped_padded, PAGE_SIZE, PROT_NONE) >= 0) {
			const int permission = PROT_READ | (allow_write ? PROT_WRITE : 0);
			const int flags = MAP_FIXED | MAP_SHARED;
			mapped = mmap(mapped_padded + PAGE_SIZE, length, permission, flags, fd, offset);	// Override the last mmap (except for the first and last pages)
		}
	}
	if (mapped == MAP_FAILED) {		// Have to unmap the +2 pages larger arre
		int save_errno = errno;
		int unmap_rv;
		N_Ef(salddbmmf1, "(length=@ZX, fd=@FD) failed, unmapping.", length, fd);
		memset(mapped_padded, 0, sizeof(struct padded_mmap_magic_number));
		unmap_rv = munmap(mapped_padded, padded_length);
		NTOMA_ASSERT(salddbmmf2, unmap_rv == 0, "munmap failed, probably bad args passed. @AUTO_ERRNO");
		mapped_padded = NULL;
		errno = save_errno;
	}
	return mapped;
}

static int nvmeibt_munmap(void *addr, size_t length)
{
	int rv = -1;
	void *mapped_padded = addr - PAGE_SIZE;
	const size_t length_padded = padded_mmap_length(length);
	const struct padded_mmap_magic_number *me = mapped_padded;

	NFIN;
	if ((((uintptr_t)addr % PAGE_SIZE) != 0) || (uintptr_t)addr < 2*PAGE_SIZE) {
		NTOMA_ASSERT(salddbmmf5, false, "Invalid addr=@PTR, length=@ZX", addr, length);
		errno = EINVAL;
		goto out;
	}
	mprotect(mapped_padded, PAGE_SIZE, PROT_READ);
	if ((me->addr != (void*)me) || (me->length != length_padded) || (me->magic_num != PADDED_MMAP_MAGIC_NUM)) {
		NTOMA_ASSERT(salddbmmf6, false, "Magic number mismatch, expected={@PTR, len=@ZX, magic=@LLX}, found={@PTR, len=@ZX, magic=@LLX}",
					   mapped_padded, length_padded, PADDED_MMAP_MAGIC_NUM,
					   me->addr, me->length, me->magic_num);
		errno = EINVAL;
	} else {
		mprotect(mapped_padded, PAGE_SIZE, PROT_WRITE);
		memset(mapped_padded, 0, sizeof(struct padded_mmap_magic_number));
		rv = munmap(mapped_padded, length_padded);
		NTOMA_ASSERT(salddbmmf8, rv == 0, "munmap failed, probably bad args passed. @AUTO_ERRNO");
	}
out:
	NFOUT;
	return rv;
}

int nvmeib_srvr_api_lib_disk_nvmeof_sata_bind(const char *dev_file_name, const char*model, const char*serial, u16 vendor, const bool is_stock_to_nvmeibs)
{
	const char* path = TOMA_ROOT_DIR "proc/nvmeibs/nvmeof_disks";
	char val[512];
	int n_bytes, n_bytes_written, fd = NNVMEIBT_OPEN(ttsrspfsh, path, O_WRONLY);
	if (fd < 0) {
		N_Ef(ttsrspfsi, "Cannot open @STR @AUTO_ERRNO", path);
		return -ENOENT;
	}
	if (is_stock_to_nvmeibs)
		n_bytes = snprintf(val, sizeof(val), "%s,%s/%s,%d", dev_file_name, model, serial, vendor);
	else
		n_bytes = snprintf(val, sizeof(val), "%s", dev_file_name);
	n_bytes_written = write(fd, val, n_bytes);
	NNVMEIBT_CLOSE(ttsrspfsj, fd);
	if (n_bytes_written < 0) {
		N_Ef(ttsrspfsk, "Cannot write @STR to path @STR @AUTO_ERRNO", val, path);
		if (errno == EEXIST && is_stock_to_nvmeibs)
			N_Tf(ttsrspfsl, "Apperantly @STR already belong to nvmeibs(Only Toma closed?)", val);
	}
	N_Tf(ttsrspfsm, "Bind @STR len=@INT[b] written=@INT[b]", val, n_bytes, n_bytes_written);
	return 0;
}

/***************************** Generic messages *******************************/
/* Original shell code:
* 		pcidrivers_base_path=/sys/bus/pci/drivers
* 		pcifd=$pcidrivers_base_path/nvme/$nvmepci
* 		#check if pci binded to nvme driver
* 		if [ ! -e "$pcifd" ]; then
* 			echo "Error locating pcifd for dev=$dev_id"; exit 0;
* 		fi
* 		#unbind pci from nvme driver
* 		echo -n "$nvmepci" > $pcidrivers_base_path/nvme/unbind
* 		if [ "$?" -eq "0" ] && [ -d "$pcidrivers_base_path/nvmeibs" ]; then
* 			#bind to nvmeibs
* 			echo -n "$nvmepci" > $pcidrivers_base_path/nvmeibs/bind
* 		fi
*/
static int __nvmeib_srvr_api_lib_disk_do_bind_unbind(const char *disk_bdf, bool is_nvmesh, bool do_bind)
{
	char path[128];	// Build kernel path of bind/unbind
	int fd;
	snprintf(path, sizeof(path), TOMA_ROOT_DIR "sys/bus/pci/drivers/%s/%sbind", (is_nvmesh ? "nvmeibs" : "nvme"), (do_bind ? "" : "un"));
	if (!disk_bdf[0])
		return -EINVAL;
	fd = NNVMEIBT_OPEN(salddbu0, path, O_WRONLY);
	if (fd >= 0) {
		const int n_written = NNVMEIBT_PWRITE(salddbu1, fd, disk_bdf, strlen(disk_bdf), 0, 0);		// write() // Ronen: I suspect that this takes 1/2 sec for mvmeibs
		NNVMEIBT_CLOSE(salddbu2, fd);
		if (n_written > 0)
			return 0;
		return -EIO;
	}
	return -EACCES;
}

int nvmeib_srvr_api_lib_disk_dobind(const char *disk_bdf, bool is_nvmesh)
{
	return __nvmeib_srvr_api_lib_disk_do_bind_unbind(disk_bdf, is_nvmesh, true);
}

int nvmeib_srvr_api_lib_disk_unbind(const char *disk_bdf, bool is_nvmesh)
{
	return __nvmeib_srvr_api_lib_disk_do_bind_unbind(disk_bdf, is_nvmesh, false);
}

/************************* NEW ntelink API **********************/
/************************* NEW ntelink API **********************/
/************************* NEW ntelink API **********************/
#include <sys/socket.h>
#include <linux/netlink.h>
#include <sys/select.h>
#include "nvmeibt_ds.h"

struct srv_comm_msg {						// In air message awaiting for server reply
	void (*on_done)(void *ctx, int ok, struct nvmeib_nl_uk_comm_rep *msg);
	void *ctx;								// ctx for on_done()
	struct xdlist link;						// Link to reside in msg lists or in progress list
	struct nvmeib_nl_uk_comm_msg msg;
};

static void msg_free(struct srv_comm_msg *msg) {
	if (msg->on_done)
		msg->on_done(msg->ctx, false, NULL);
	NNVMEIBT_BM_FREE(ttkmcmf0, msg);		// Did not get any reply
}

struct disk_info {
	struct nvmeib_disk_info disk;
	struct nvmeib_remove_disk rm_disk;
	bool remove_in_progress;
	unsigned long ack_id;
	struct xdlist link;
};

bool disk_info_is_equal(const struct disk_info *di, const char* disk_name) {
	return !memcmp(di->disk.disk_id, disk_name, sizeof(di->disk.disk_id));
}

typedef XDLIST_DECLARE(msgs_list, struct srv_comm_msg,   link) msgs_list_t;
typedef XDLIST_DECLARE(disk_list, struct disk_info,      link) disk_list_t;
struct nvmeibt_km_comm {
	struct nvmeibt_km_comm_params params;
	msgs_list_t msgs1, msgs2;		// Double buffering, 1 list is draining, other getting new requests
	msgs_list_t *msgs;				// Points to current list of added entries (1 of the 2 above) ????
	msgs_list_t in_progress_msgs;	// In air messages, sent to server and awaiting reply, accessed only from main thread, or when it is dead, so no need for locks
	disk_list_t disks;
	pthread_mutex_t guard;			// Serialize Toma thread access
	atomic64_t unique_id_generator;	// Ever increasing counter for msg id and others
	int spair[2];					// Wakeup socket-pair: Toma sends msgs to spair[0], our main thread selects on spair[1]. Read from spair[1] and passes msg to kernel or dispatch internally
	pthread_t comm_thread;			// main thread which processes messages
	//struct async_server_msg_api {
		struct nlmsghdr *nlh;			// 1 preallocated Linux netlink msg, to avoid mallocs during send/recv
		struct iovec iov;				// 1 preallocated iovec
		struct sockaddr_nl dest_addr;	// Netlink address to send msgs to
		int nl_sock_fd;					// Socket to which send/recv message to/from server (kernel netlink, UDS in user-space)
		int fd_srvr2toma;				// fd' for toma receive large messages from server (registrant clients topology).
	//} async;
	struct blocking_server_api {	// Blocking messaging API (large messages) - implemented towards kernel server, 2 fd's proc files
		int fd_toma2srvr, fd_toma2clnt;	// 2 fd's for toma send
	} kernel;						// In user space API those 2 fd's are redirected to 'nl_sock_fd'
	struct {
		int error_occured;			// if != 0: Object is not operational, closing due to error. Stores error code
		bool stop_uplink;			// Prevent Toma messages from being sent to
		bool use_async_api_and_sema_for_blocking_msgs;
		bool use_async_api_instead_of_proc_files;
	} state_flags;
	struct resource_usage_counters_t {
		int n_lock_maps;
	} resource;
	struct {						// For debug, maximal message sizes. Defined at compile time
		int proc_recv;
		int proc_send;
		int nlink;					// Maximal size of msg that can be sent/recv to/from kernel. Known at compile time
	} max_msg_size;
} *_singleton;

static unsigned long get_guid(struct nvmeibt_km_comm *p)
{
	return atomic64_inc_return(&p->unique_id_generator);
}

static void nvmeibt_km_comm_lock(struct nvmeibt_km_comm *p)
{
	const int rv = pthread_mutex_lock(&p->guard);
	if (rv != 0) N_Ef(kmtscl0, "Failed to lock srv comm guard rv=@RV", rv);
}

static void nvmeibt_km_comm_unlock(struct nvmeibt_km_comm *p)
{
	const int rv = pthread_mutex_unlock(&p->guard);
	if (rv != 0) N_Ef(kmtscl1, "Failed to unlock srv comm guard rv=@RV", rv);
}

static int start_netlink_socket(struct nvmeibt_km_comm *p)
{
	int sock_fd = NNVMEIBT_SOCKET(tscnlss0, PF_NETLINK, SOCK_RAW, NETLINK_SRV_COMM);
	struct sockaddr_nl src_addr;

	if (sock_fd < 0) {
		N_ETf(tscnlss1, "Fail to create netlink socket - @AUTO_ERRNO");
		return -1;
	}
	memset(&src_addr, 0, sizeof(src_addr));
	src_addr.nl_family = AF_NETLINK;
	src_addr.nl_pid = /*pthread_self() << 16 | */getpid();

	if (bind(sock_fd, (struct sockaddr*)&src_addr, sizeof(src_addr)) < 0) {
		N_ETf(tscnlss3, "Fail to bind netlink socket - @AUTO_ERRNO");
		NNVMEIBT_CLOSE(tscnlss4, sock_fd);
		return -2;
	} else {
		memset(&p->dest_addr, 0, sizeof(p->dest_addr));
		p->dest_addr.nl_family = AF_NETLINK;
		p->dest_addr.nl_pid = 0; /* For Linux Kernel */
		p->dest_addr.nl_groups = 0; /* unicast */
		p->nl_sock_fd = sock_fd;
		return 0;
	}
}

static void * run(void *v);
static int start_thread(struct nvmeibt_km_comm *p)
{
	pthread_attr_t attr;
	int pt_err;
	p->comm_thread = 0;
	if ((pt_err = pthread_attr_init(&attr)) != 0 ||
		(pt_err = pthread_create(&p->comm_thread, &attr, run, p)) != 0) {
		p->comm_thread = 0;
		errno = pt_err;
		N_Ef(tscnlss6, "Fail to create srv comm thread @AUTO_ERRNO");
		return -1;
	}
	pthread_setname_np(p->comm_thread, "km_comm_srv");
	return 0;
}

void __calc_max_msg_size(struct nvmeibt_km_comm *p) {
	const size_t server_nlink = sizeof(struct nvmeib_nl_uk_comm_msg) + max((sizeof(struct nvmeib_nl_msg_to_toma) + 256 /*nvmeib_push_extended_msg payload?*/), sizeof(union nvmeib_nl_msg_to_srvr_payload));
	const size_t lserver_proc = sizeof(struct nvmeibs_toma_server_proc_buf);
	const size_t clients_topo = NVMEIB_TOMA_REQ_MAX_LEN + (sizeof(struct nvmeibs_toma_client_proc_buf) - sizeof(struct nvmeibt_client_msg));
	const size_t srvr_max = max(server_nlink, lserver_proc);
	const size_t total_max = max(srvr_max, clients_topo);
	p->max_msg_size.proc_recv = max(lserver_proc, clients_topo);
	p->max_msg_size.proc_send = clients_topo;
	if (p->params.use_user_space_api) {
		p->state_flags.use_async_api_and_sema_for_blocking_msgs = true;									// Kernel has blocking /proc. User space does not have them. Toma Simulator supports both
		p->state_flags.use_async_api_instead_of_proc_files = true;			// Todo: Do not enable yet. Toma simulator does not support it yet
		p->max_msg_size.nlink = NLMSG_SPACE(total_max);			// All messages via async api
	} else {
		p->max_msg_size.nlink = NLMSG_SPACE(server_nlink);		// Large messages use /proc sync api
	}
}

static int __blocking_msg_api_create(struct nvmeibt_km_comm *p)
{
	const char *path_toma2srvr = TOMA_ROOT_DIR "proc/nvmeibs/toma_server";			// Toma->Srvr, See server nvmeibs_toma_create()
	const char *path_srvr2toma = TOMA_ROOT_DIR "proc/nvmeibs/toma_server_events";	// Srvr->Toma
	const char *path_toma2clnt = TOMA_ROOT_DIR "proc/nvmeibs/toma_clients";			// Toma->Clnt
	int rv = 0;

	p->fd_srvr2toma = p->kernel.fd_toma2clnt = p->kernel.fd_toma2srvr = -1;
	if (true) {		// We separate it from async API for QOS reason: faster response time
		p->fd_srvr2toma = NNVMEIBT_OPEN(nsalc1, path_srvr2toma, O_RDWR);
		if (p->fd_srvr2toma < 0)
			rv = -1;
	}
	if (!p->params.use_user_space_api) {
		p->kernel.fd_toma2clnt = NNVMEIBT_OPEN(nsalc2, path_toma2clnt, O_RDWR);
		p->kernel.fd_toma2srvr = NNVMEIBT_OPEN(nsalc3, path_toma2srvr, O_RDWR);
		if ((p->kernel.fd_toma2clnt < 0) || (p->kernel.fd_toma2srvr < 0))
			rv = -1;
	}
	if (rv < 0)
		N_Ef(nsalc7, "Failed: @STR=@FD, @STR=@FD, @STR=@FD, @AUTO_ERRNO, FATAL: Without server toma will not live", path_srvr2toma, p->fd_srvr2toma, path_toma2clnt, p->kernel.fd_toma2clnt, path_toma2srvr, p->kernel.fd_toma2srvr);
	return rv;
}

static void __blocking_msg_api_destroy(struct nvmeibt_km_comm *p)
{
	if (true)
		NNVMEIBT_CLOSE(nsalcc, p->fd_srvr2toma);
	if (!p->params.use_user_space_api) {
		NNVMEIBT_CLOSE(nsalcd, p->kernel.fd_toma2clnt);
		NNVMEIBT_CLOSE(nsalce, p->kernel.fd_toma2srvr);
	}
}

int nvmeib_srvr_api_lib_create(const struct nvmeibt_km_comm_params* params)
{
	struct nvmeibt_km_comm *p;
	int rv = 0;

	NFIN;
	if (_singleton) {											rv = -__LINE__; goto init_fail; }
	p = _singleton = NNVMEIBT_TOMA_CALLOC(tscnlssa, 1, sizeof(*p));
	if (!p) { 													rv = -__LINE__; goto init_fail; }
	p->params = *params;
	if (params->use_only_passive_util_mode) {	/* Skip active initialization*/ goto done; }
	if (__blocking_msg_api_create(p) < 0) {						rv = -__LINE__; goto free_p; }
	if (!params->print_status_fn) { 							rv = -__LINE__; goto free_p; }			// The only one which is mandatory for Toma
	if (pthread_mutex_init(&p->guard, NULL) != 0) { 				rv = -__LINE__; goto free_p; }
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, p->spair) < 0) { 	rv = -__LINE__; goto free_guard; }
	if (start_netlink_socket(p))  { 							rv = -__LINE__; goto free_spair; }
	if (nvmeibt_nonblock_fd(p->spair[1]) < 0) {					rv = -__LINE__; goto free_netlink; }
	__calc_max_msg_size(p);
	if (p->max_msg_size.nlink > 4096) {							rv = -__LINE__; goto free_netlink; }	// Allocate 1 aligned page for faster access
	NNVMEIBT_TOMA_POSIX_MEMALIGN(tscnlssb, &p->nlh, 4096, 4096);
	if (!p->nlh) {												rv = -__LINE__; goto free_netlink; }
	p->iov.iov_base = (void *)p->nlh;
	p->iov.iov_len = p->max_msg_size.nlink;
	XDLIST_HEAD_INIT(&p->in_progress_msgs);
	XDLIST_HEAD_INIT(&p->disks);
	XDLIST_HEAD_INIT(&p->msgs1);
	XDLIST_HEAD_INIT(&p->msgs2);
	p->msgs = &p->msgs1;
	if (start_thread(p) < 0) {									rv = -__LINE__; goto free_nl_buffer;}
	{		// Connect
		struct nvmeibs_toma_server_proc_buf buf;
		N_Tf(__AUTOID__, "Logging in to server");
		memset(&buf, 0, sizeof(buf));
		buf.type = NVMEIBS_TOMA_LOGIN;
		(void)nvmeib_srvr_api_lib_send_block_msg_to_server(&buf);
	}
done:
	N_Tf(__AUTOID__, "Done. Active=@BOOL_YN", !params->use_only_passive_util_mode);
	return 0;

free_nl_buffer:	NNVMEIBT_TOMA_FREE(tscnlssc, p->nlh);
free_netlink:	NNVMEIBT_CLOSE(tscnlssd, p->nl_sock_fd);
free_spair:		NNVMEIBT_CLOSE(tscnlsse, p->spair[0]);
				NNVMEIBT_CLOSE(tscnlssf, p->spair[1]);
free_guard:		pthread_mutex_destroy(&p->guard);
free_p:			NNVMEIBT_TOMA_FREE(tscnlssg, p);
				_singleton = NULL;
init_fail:		N_Ef(tscnlssh, "Failed on line=@INT, aborting. @AUTO_ERRNO", rv);
	return _singleton ? -EEXIST : -ENODEV;
}

static void __remove_disk_and_free(struct nvmeibt_km_comm *p, struct disk_info *disk)
{
	nvmeibt_km_comm_lock(p);
	XDLIST_DEL(&disk->link);
	nvmeibt_km_comm_unlock(p);
	NNVMEIBT_TOMA_FREE(tscnlssn, disk);
}

static void __drain_msg_list(msgs_list_t *l)
{
	while (!XDLIST_EMPTY(l)) {
		struct srv_comm_msg *msg = XDLIST_FIRST(l);
		XDLIST_DEL(&msg->link);
		msg_free(msg);
	}
}

static void __stop_main_thread(struct nvmeibt_km_comm *p)
{
	int pt_err;

	if (p->comm_thread) {		// Block until main thread is stopped and join it
		const struct km_comm_msg_hdr msg = {.len = 0, .opcode = csc_internal_suicide, .on_done = NULL };
		N_Tf(tscnlsst, "Send internal suicide message, to main thread");
		nvmeib_srvr_api_lib_send_async_msg_to_server(&msg);	// Issue suicide request to be handled in main thread context
		pt_err = pthread_join(p->comm_thread, NULL);
		if (pt_err != 0) {
			errno = pt_err;
			N_Ef(tscnlssk, "join failed @PTHREAD, @AUTO_ERRNO", p->comm_thread);
		}
		p->comm_thread = 0;
	}
	N_Tf(tscnlssu, "Main thread down");
	__drain_msg_list(&p->msgs1);
	__drain_msg_list(&p->msgs2);		// One of those 2 is p->msgs
	__drain_msg_list(&p->in_progress_msgs);
	while (!XDLIST_EMPTY(&p->disks)) {
		__remove_disk_and_free(p, XDLIST_FIRST(&p->disks));
	}
	NNVMEIBT_TOMA_FREE(tscnlsso, p->nlh);
	pthread_mutex_destroy(&p->guard);
	NNVMEIBT_CLOSE(tscnlssp, p->spair[0]);
	NNVMEIBT_CLOSE(tscnlssq, p->spair[1]);
	NNVMEIBT_CLOSE(tscnlssr, p->nl_sock_fd);
}

void nvmeib_srvr_api_lib_destroy(void)
{
	struct nvmeibt_km_comm *p = _singleton;
	if (!p)
		return;
	if (!p->params.use_only_passive_util_mode) {
		struct nvmeibs_toma_server_proc_buf buf;
		int rv;
		memset(&buf, 0, sizeof(buf));
		buf.type = NVMEIBS_TOMA_LOGOUT;
		rv = nvmeib_srvr_api_lib_send_block_msg_to_server(&buf);
		(void)rv; // Nothing to do with this
		__stop_main_thread(p);
		__blocking_msg_api_destroy(p);
	}
	if (p->resource.n_lock_maps != 0)
		N_Ef(tscnlssv, "Leaking resources: lock_maps=@INT", p->resource.n_lock_maps);
	NNVMEIBT_TOMA_FREE(tscnlsss, _singleton);
}

static void read_toma_wakeup_event(struct nvmeibt_km_comm *p)
{
	char c;
	NFIN;
	while (read(p->spair[1], &c, 1) == 1);		// If toma send a few messages, drain all wakeups
	NFOUT;
}

static void __fill_netlink_hdr(struct nvmeibt_km_comm *p, struct msghdr* hdr, struct sockaddr_nl *addr, size_t buf_len)
{
	memset(hdr, 0, sizeof(*hdr));
	hdr->msg_name = (void *)addr;
	hdr->msg_namelen = sizeof(*addr);
	hdr->msg_iov = &p->iov;
	hdr->msg_iovlen = 1;
	p->iov.iov_len = buf_len;						// Minimal init for recv msg. For sending, more init is required
}

static void send_msg_to_kernel(struct nvmeibt_km_comm *p, struct srv_comm_msg *msg, bool is_toma_explicit_msg)
{
	struct msghdr hdr;
	struct nlmsghdr *nlh = p->nlh;
	const size_t send_len = NLMSG_LENGTH(msg->msg.len);	// Transmit not NLMSG_SPACE() but the exact length to avoid alignment padding of a few bytes, even though server code should consume with optional alignment

	NFIN;
	if (send_len > (size_t)p->max_msg_size.nlink) {
		N_Ef(tkmcsmtk0, "msg[@INT].id=@ID size @LEN[b] > max netlink msg @LEN[b]", msg->msg.opcode, msg->msg.id, msg->msg.len, p->max_msg_size.nlink);
		msg_free(msg);
		goto out;
	}
	msg->msg.caller_type = TOMA_CALLER;
	__fill_netlink_hdr(p, &hdr, &p->dest_addr, send_len);
	nlh->nlmsg_len = send_len; 							// Parsed by server: static void recv_msg_(struct sk_buff *skb)
	nlh->nlmsg_pid = getpid();							// Important, server uses this pid to pin pages in memory
	nlh->nlmsg_flags = 0;								// We dont use lh->nlmsg_seq currently.
	nlh->nlmsg_type = NVMESH_NL_MSG_TYPE;
	memcpy(NLMSG_DATA(nlh), &msg->msg, msg->msg.len);
	N_Tf(tkmcsmtk1, "msg[@INT].id=@ID to kernel pid=@PID", msg->msg.opcode, msg->msg.id, nlh->nlmsg_pid);
	if (msg->on_done)
		XDLIST_ADD_TAIL(&p->in_progress_msgs, msg);		// Important: Insert before calling send, as reply can come fast and not find the in progress message
	sendmsg(p->nl_sock_fd, &hdr, 0);					// Todo: Check for error
	if (!msg->on_done && is_toma_explicit_msg)			// Internally generated messages are always on stack and dont have on_done() (for simplicity of code)
		msg_free(msg);
out:
	NFOUT;
}

static bool __handle_incomming_msg_from_toma(struct nvmeibt_km_comm *p)
{
	msgs_list_t *msgs;
	bool is_alive = true;

	NFIN;
	nvmeibt_km_comm_lock(p);
	msgs = p->msgs;
	p->msgs = (msgs == &p->msgs1) ? &p->msgs2 : &p->msgs1;
	nvmeibt_km_comm_unlock(p);
	while (!XDLIST_EMPTY(msgs)) {
		struct srv_comm_msg *msg = XDLIST_FIRST(msgs);
		XDLIST_DEL(&msg->link);
		N_Tf(tkmcsmtk2, "msg[@INT].id=@ID", msg->msg.opcode, msg->msg.id);
		if (unlikely(msg->msg.opcode < csc_start)) {
			p->state_flags.stop_uplink = true;
			if (msg->msg.opcode == csc_internal_stop_callbacks) {
				p->params.on_add_disk = NULL;
				p->params.on_remove_disk = NULL;
				p->params.process_extend_msg = NULL;
				p->params.process_local_srvr_msg = NULL;
				p->params.process_disk_info = NULL;
			} else if (msg->msg.opcode <= csc_internal_suicide) {
				is_alive = false;									// Stop the main thread
			}
		}
		if (!p->state_flags.stop_uplink) {
			send_msg_to_kernel(p, msg, true);						// Dont free msg, it is added to a different queue or freed inside
			continue;
		}
		if (msg->msg.opcode == csc_t2s_blocking_msg_other) {		// Proc messages, autofail all except logout msg
			const struct nvmeibs_toma_server_proc_buf *pb = (struct nvmeibs_toma_server_proc_buf *)&msg->msg.data[0];
			if (pb->type == NVMEIBS_TOMA_LOGOUT) {
				send_msg_to_kernel(p, msg, true);
				continue;
			}
		}
		N_Tf(tkmcsmtk3, "msg[@INT].id=@ID, not sent to server", msg->msg.opcode, msg->msg.id);
		msg_free(msg);											// Autofail msg
	}
	NFOUT;
	return is_alive;
}

static struct srv_comm_msg *find_in_progress_msg_waiting_for_reply(struct nvmeibt_km_comm *p, unsigned long id)
{
	struct srv_comm_msg *msg;
	XDLIST_FOREACH_SAFE(msg, &p->in_progress_msgs) {
		if (msg->msg.id == id) {
			XDLIST_DEL(&msg->link);
			return msg;
		}
	}
	return NULL;
}

static void remove_disk_ack(struct nvmeibt_km_comm *p, const struct nvmeib_disk_info *di);
static void __process_disk(struct nvmeibt_km_comm *p, const struct nvmeib_nl_uk_comm_msg *rcv_msg)
{
	struct nvmeib_nl_uk_comm_rep *rep = (struct nvmeib_nl_uk_comm_rep *)rcv_msg->data;
	struct disk_info *disk;

	NFIN;
	if (rep->error == csce_ok) {
		struct nvmeib_disk_info_reply *disk_rep = container_of(rep, struct nvmeib_disk_info_reply, base);
		const struct nvmeib_disk_info *di = &disk_rep->dinfo.disk;
		switch (disk_rep->selector) {
		case nvmeib_disk_info_reply_dummy:
			// Obsolete. Need to be removed from the ENUM in order to avoid compilation warning
			break;
		case nvmeib_disk_info_reply_serjio_state: {
			const struct nvmeib_disk_info_rep_sej_state_t *ssc = &disk_rep->serjio_state_change;
			if (p->params.process_disk_info)
				p->params.process_disk_info(ssc->disk_id, ssc->vendor_id, ssc->model_str, ssc->serjio_status);
			break;
		}
		case nvmeib_disk_info_reply_dinfo:
			if (di->n_blocks > 0) {
				disk = NNVMEIBT_TOMA_CALLOC(t2cnlpd1, 1, sizeof(*disk));
				if (disk) {
					N_Tf(t2cnlpd2, "Adding disk=@STR, has_cb=@BOOL_YN", di->disk_id, !!p->params.on_add_disk);
					disk->disk = *di;
					nvmeibt_km_comm_lock(p);
					XDLIST_ADD_TAIL(&p->disks, disk);
					nvmeibt_km_comm_unlock(p);
					if (p->params.on_add_disk)
						p->params.on_add_disk(&disk->disk);
					if (p->params.process_disk_info)
						p->params.process_disk_info(di->disk_id, di->vendor_id, di->model_str, disk_rep->dinfo.serjio_status);
				} else {
					N_Ef(t2cnlpd3, "Failed to allocate memory for disk=@STR", di->disk_id);
				}
			} else {
				bool found_disk = false;
				XDLIST_FOREACH(disk, &p->disks) {
					if (disk_info_is_equal(disk, di->disk_id)) {
						found_disk = true;
						disk->remove_in_progress = true;
						disk->ack_id = get_guid(p);
						memcpy(disk->rm_disk.disk_id, disk->disk.disk_id, sizeof(disk->rm_disk.disk_id));
						disk->rm_disk.vendor_id = disk->disk.vendor_id;
						disk->rm_disk.ack_id = disk->ack_id;
						N_Tf(t2cnlpd4, "Removing disk=@STR, has_cb=@BOOL_YN", di->disk_id, !!p->params.on_remove_disk);
						if (p->params.on_remove_disk)
							p->params.on_remove_disk(&disk->rm_disk);
						break;
					}
				}
				// Ack the remove. When the WQ is finalized we will unmap the lock table, releasing the disk.
				remove_disk_ack(p, &disk_rep->dinfo.disk);
				if (found_disk) {
					__remove_disk_and_free(p, disk);
				}
			}
			break;
		}
	}
	NFOUT;
}

static bool __handle_new_srvr_msg(struct nvmeibt_km_comm *p)
{
	struct sockaddr_nl src_addr;
	struct msghdr hdr;
	ssize_t n;
	bool is_alive = true;

	NFIN;
	__fill_netlink_hdr(p, &hdr, &src_addr, p->max_msg_size.nlink);	// Ensure recvmsg() can fill the entire preallocated buffer
	n = recvmsg(p->nl_sock_fd, &hdr, 0);
	if (n < (ssize_t)(NLMSG_LENGTH(sizeof(struct nvmeib_nl_uk_comm_msg)))) {
		N_Ef(t2shnnm0, "Failed to recieve message from kernel, bytes=@INT", (int)n);
		is_alive = false;
	} else {
		struct nvmeib_nl_uk_comm_msg *rcv_msg = NLMSG_DATA(p->nlh);
		struct srv_comm_msg *msg = find_in_progress_msg_waiting_for_reply(p, rcv_msg->id);
		N_Tf(t2shnnm1, "rcv_msg[@INT].id=@ID, @LEN[b], was_blocking=@BOOL_YN, n=@ZU[b]", rcv_msg->opcode, rcv_msg->id, rcv_msg->len, !!msg, n);
		if (msg) {
			if (msg->on_done) {
				struct nvmeib_nl_uk_comm_rep *rep = (struct nvmeib_nl_uk_comm_rep *)rcv_msg->data;
				N_Tf(t2shnnm3, "Calling callback on rep_msg[@INT].id=@ID rp_rv=@RV, waiting_msg[@INT]", rep->opcode, msg->msg.id, rep->error, msg->msg.opcode);
				msg->on_done(msg->ctx, (rep->error == csce_ok), rep);
				msg->on_done = NULL;
			}
			msg_free(msg);
		} else if (rcv_msg->opcode == csc_get_disks) {			// Reply to internal periodic get disks message
			__process_disk(p, rcv_msg);
		} else if (rcv_msg->opcode == csc_msg_to_process) {		// Server initiated extended msg
			const struct nvmeib_nl_msg_to_toma *tm = (void*)rcv_msg->data;
			if (p->params.process_extend_msg)
				p->params.process_extend_msg(&tm->payload.extended_msg);
		} else {
			N_Ef(t2shnnm5, "Got a message from kernel that no one was waiting for");
		}
	}
	NFOUT;
	return is_alive;
}

static void __release_msg_queues_on_error(struct nvmeibt_km_comm *p, const char *reason)
{
	msgs_list_t *msgs;
	bool is_alive = true;

	N_Ef(t2srmqon0, "Error: @STR, @AUTO_ERRNO", reason);
	nvmeibt_km_comm_lock(p);
	read_toma_wakeup_event(p);
	p->state_flags.error_occured = -1;				// No need to deferntiate by 'reason', we have it in logs
	msgs = p->msgs;
	p->msgs = msgs == &p->msgs1 ? &p->msgs2 : &p->msgs1;
	while (!XDLIST_EMPTY(msgs)) {
		struct srv_comm_msg *msg = XDLIST_FIRST(msgs);
		XDLIST_DEL(&msg->link);
 		if (msg->msg.opcode <= csc_internal_suicide)
			is_alive = false;
		msg_free(msg);
	}
	__drain_msg_list(&p->in_progress_msgs);
	nvmeibt_km_comm_unlock(p);
	if (is_alive) {
		char c;
		N_Tf(t2scqrl1, "Wait for wakeup to terminate main loop");
		if (nvmeibt_fd_set_blocking(p->spair[1], 1))
			read(p->spair[1], &c, 1);
		N_Tf(t2scqrl2, "Wakeup arrived");
	}
	NFOUT;
}

static void remove_disk_ack(struct nvmeibt_km_comm *p, const struct nvmeib_disk_info *di)
{
	char buf[sizeof(struct srv_comm_msg) + sizeof(struct nvmeib_remove_disk)];
	struct srv_comm_msg *kmsg = (void *)buf;
	struct nvmeib_remove_disk *rd = (void *)kmsg->msg.data;
	memset(buf, 0, sizeof(buf));
	kmsg->msg.opcode = csc_remove_disk_ack;
	kmsg->msg.len = sizeof(kmsg->msg) + sizeof(struct nvmeib_remove_disk);
	memcpy(rd->disk_id, di->disk_id, sizeof(di->disk_id));
	send_msg_to_kernel(p, kmsg, false);
}

static void get_disks(struct nvmeibt_km_comm *p)
{
	struct srv_comm_msg kmsg;
	memset(&kmsg, 0, sizeof(kmsg));
	kmsg.msg.opcode = csc_get_disks;
	kmsg.msg.len = sizeof(kmsg.msg);
	send_msg_to_kernel(p, &kmsg, false);
}

static void _send_keep_alive_to_server(struct nvmeibt_km_comm *p)
{
	struct srv_comm_msg kmsg;
	memset(&kmsg, 0, sizeof(kmsg));
	kmsg.msg.opcode = csc_keep_alive;
	kmsg.msg.len = sizeof(kmsg.msg);
	send_msg_to_kernel(p, &kmsg, false);
}

static bool _recv_msg_from_local_server(struct nvmeibt_km_comm *p)
{
	const int max_len = p->max_msg_size.proc_recv;
	struct nvmeibs_toma_server_proc_buf *msg = NNVMEIBT_BM_CALLOC(tthlsd0, max_len);
	const int rv = read(p->fd_srvr2toma, msg, max_len);
	if (rv < (int)sizeof(msg->handle)) {
		N_Ef(tthlsd1, "Failed read fd=@FD rv=@RV @AUTO_ERRNO", p->fd_srvr2toma, rv);
		NNVMEIBT_BM_FREE(tthlsd2, msg);
	} else if (p->params.process_local_srvr_msg) {
		p->params.process_local_srvr_msg(msg, rv);	// Callback will free the message
	} else {
		N_Tf(tthlsd4, "Dropping msg.type=@INT, @INT[bytes], no callback", msg->type, rv);
		NNVMEIBT_BM_FREE(tthlsd3, msg);
	}
	return true;
}

static void * run(void *v)
{
	struct nvmeibt_km_comm *p = v;
	fd_set read_fds, except_fds;
	const int _max_fd0 = max(p->spair[1], p->nl_sock_fd);
	const int n_fds = max(_max_fd0, p->fd_srvr2toma) + 1;
	int n;

	NFIN;
	get_disks(p);
	while (true) {
		struct timeval tv = {.tv_sec = TOMA_SILENCE_MAX_PERIOD_SECS, .tv_usec = 0};
		FD_ZERO(&read_fds);
		FD_SET(p->spair[1],     &read_fds);
		FD_SET(p->nl_sock_fd,   &read_fds);
		FD_SET(p->fd_srvr2toma, &read_fds);
		except_fds = read_fds;
		n = select(n_fds, &read_fds, NULL /*No writes*/, &except_fds, &tv);	// Wakeup on incomming msg from server or from toma
		if (n > 0) {
			if (FD_ISSET(p->spair[1], &read_fds)) {
				read_toma_wakeup_event(p);				if (!__handle_incomming_msg_from_toma(p)) 	   break; }
			if (FD_ISSET(p->nl_sock_fd,   &read_fds)) {	if (!__handle_new_srvr_msg(p)) 				   break; }
			if (FD_ISSET(p->fd_srvr2toma, &read_fds)) {	if (!_recv_msg_from_local_server(p)) 		   break; }
			if (FD_ISSET(p->spair[1],     &except_fds)) { __release_msg_queues_on_error(p, "toma sock"); break; }
			if (FD_ISSET(p->nl_sock_fd,   &except_fds)) { __release_msg_queues_on_error(p, "srvr sock"); break; }
			if (FD_ISSET(p->fd_srvr2toma, &except_fds)) { __release_msg_queues_on_error(p, "fd_s2toma"); break; }
		} else if (n == 0) {	// timeout
			N_Df(trace_km_comm_run, "Timeout");		  { if (!__handle_incomming_msg_from_toma(p)) 	   break; }	// Try for the chance we missed an event
			XDLIST_EMPTY(&p->disks) ? get_disks(p) : _send_keep_alive_to_server(p);
		} else {										__release_msg_queues_on_error(p, "select_-1"); break; }
	}
	NFOUT;
	return NULL;
}

static bool __submit_toma_msg(struct nvmeibt_km_comm *p, struct srv_comm_msg *m, const void* buf, size_t buf_len)
{
	bool was_sent;
	m->msg.len = sizeof(m->msg) + buf_len;
	if (buf_len) memcpy(m->msg.data, buf, buf_len);
	m->msg.id = get_guid(p);
	N_Tf(__AUTOID__, "msg[@INT].id=@ID, base=@INT[b]", m->msg.opcode, m->msg.id, m->msg.len-(int)sizeof(struct nvmeib_nl_uk_comm_msg));
	nvmeibt_km_comm_lock(p);
	was_sent = !p->state_flags.error_occured;					// Reading is syncronize with setting it from main thread via lock
	if (was_sent) {
		int wakeup_rv;
		char c = 1;
		XDLIST_ADD_TAIL(p->msgs, m);
		wakeup_rv = write(p->spair[0], &c, 1);					// Wakeup our main thread to handle the message, dont care if wakeup fails
		if (wakeup_rv != 1) N_Ef(__AUTOID__, "Unable wakeup msg2srv submit thread");
	}															// Else msg lists already drained, dont add anything to it
	nvmeibt_km_comm_unlock(p);
	if (unlikely(!was_sent)) {
		N_Tf(__AUTOID__, "msg[@INT].id=@ID cannot be sent", m->msg.opcode, m->msg.id);
		m->on_done = NULL; 										// Agreement in case of syncronous send error, callback will not be given
		msg_free(m);
	}
	return was_sent;
}

int nvmeib_srvr_api_lib_send_async_msg_to_server(const struct km_comm_msg_hdr *hdr)
{
	struct nvmeibt_km_comm *p = _singleton;
	struct srv_comm_msg *m = NNVMEIBT_BM_CALLOC(__AUTOID__, sizeof(*m) + sizeof(m->msg) + hdr->len);
	int rv;
	NTOMA_ASSERT(__AUTOID__, (hdr->opcode != csc_start || hdr->opcode < csc_end), "msg[@INT] Invalid type", hdr->opcode);
	if (!m) {
		N_Ef(stkmcnl2, "Fail to allocate nvmeibt_km_comm msg");
		return -ENOMEM;
	}
	m->msg.opcode = hdr->opcode;
	m->on_done = hdr->on_done;
	m->ctx =  hdr->ctx;
	rv = __submit_toma_msg(p, m, hdr->data, hdr->len) ? 0 : -EPERM;
	NFOUT;
	return rv;
}

#include <semaphore.h>
struct completion {
	sem_t s;	// reset(){=0}, done(){change 0->1}, wait(){block until == 1, then reset()}
};
static inline void init_completion(    struct completion *x){ NTOMA_ASSERT(__AUTOID__, sem_init(&x->s, 0, 0) == 0, "comp=@PTR", x); }
static inline void wait_for_completion(struct completion *x){ NTOMA_ASSERT(__AUTOID__, sem_wait(&x->s)       == 0, "comp=@PTR", x); }
static inline void complete(           struct completion *x){ NTOMA_ASSERT(__AUTOID__, sem_post(&x->s)       == 0, "comp=@PTR", x); }
static inline void destroy_completion( struct completion *x){ NTOMA_ASSERT(__AUTOID__, sem_destroy(&x->s)    == 0, "comp=@PTR", x); }
static inline bool completion_is_done( struct completion *x){ return sem_trywait(&x->s); }

struct blocking_wait_context {
	struct completion comp;				// For blocking implementation
	struct nvmeibt_Str *reply_str;		// Not needed if waiting for ACK, but if need to process a response store it in a string
	int rv;								// Netlink main thread will wakeup send-request thread when reply/ack arrives
};

static void __on_done_wakeup_sender(void *ctx, int ok, struct nvmeib_nl_uk_comm_rep *rep) {	// Called from netlink main thread
	struct blocking_wait_context *b = ctx;
	(void)ok;
	if (rep) {
		if (b->reply_str && (rep->error == csce_ok)) {	// Fill senders context with reply
			const struct nvmeib_t2s_request_srvr_info_rep* pl = (typeof(pl))rep;
			N_Tf(__AUTOID__, "Copy reply rep_msg[@INT], @INT[b], truncate=@BOOL_YN", rep->opcode, pl->n_bytes_len, pl->was_truncated);
			nvmeibt_Str_strncat(b->reply_str, &pl->content[0], pl->n_bytes_len);
			if (pl->was_truncated)
				 rep->error = csce_format_oom;
		}
		if (     rep->error == csce_ok)				 b->rv = 0;	// Inverse of uk_comm_err_from_errno
		else if (rep->error == csce_dst_not_exist)	 b->rv = -ENXIO;
		else if (rep->error == csce_in_progress)	 b->rv = -EINPROGRESS;
		else if (rep->error == csce_already_running) b->rv = -EALREADY;
		else 							 			 b->rv = -EIO;
	} else {
		b->rv = -ENOEXEC;				// Sent to server, but did not get a reply
	}
	complete(&b->comp);
}

static int _submit_msg_and_wait_for_reply(struct nvmeibt_km_comm *p, enum uk_comm_opcode op, const void* buf, size_t buf_len, struct nvmeibt_Str *reply_str)
{
	struct blocking_wait_context b;
	struct srv_comm_msg *m = NNVMEIBT_BM_CALLOC(__AUTOID__, sizeof(*m) + sizeof(m->msg) + buf_len);
	struct timespec	t1, t2;
	getnstimeofday_boot(&t1);
	if (!m) {
		N_Ef(__AUTOID__, "Fail to allocate nvmeibt_km_comm msg");
		return -ENOMEM;
	}
	m->msg.opcode = op;
	m->on_done = &__on_done_wakeup_sender;
	m->ctx = (void*)&b;
	init_completion(&b.comp);
	b.reply_str = reply_str;
	b.rv = -EPERM;							// Not sent to server
	if (__submit_toma_msg(p, m, buf, buf_len)) {
		const unsigned long msg_id = m->msg.id;			// Store on stack as after completion, 'm' will be freee
		const enum uk_comm_opcode opcode = m->msg.opcode;
		wait_for_completion(&b.comp);	// Server reply will autofill b.rv
		N_Tf(__AUTOID__, "msg[@INT].id=@ID, wait_done, rv=@RV", opcode, msg_id, b.rv);
	}	// else, beware: 'm' already freed
	getnstimeofday_boot(&t2);
	{
		const int64_t m_sec = NSEC_TO_MSEC(timespec_diff_ns(t2, t1));
		if (m_sec > 20) N_IMf(__AUTOID__, "msg[@INT] Took @LLD[ms]", op, m_sec);
	}
	destroy_completion(&b.comp);
	return b.rv;
}

static int _submit_msg_and_wait_for_ack(struct nvmeibt_km_comm *p, enum uk_comm_opcode op, const void* buf, size_t buf_len)
{
	return _submit_msg_and_wait_for_reply(p, op, buf, buf_len, NULL);
}

void nvmeib_srvr_api_lib_server__detach(void)
{
	struct nvmeibt_km_comm *p = _singleton;
	if (p->comm_thread) {
		const struct km_comm_msg_hdr msg = {.len = 0, .opcode = csc_internal_stop_callbacks, .on_done = NULL };
		N_Tf(tscnlvs0, "Send internal stop callbacks, to main thread");
		(void)_submit_msg_and_wait_for_ack(p, msg.opcode, &msg, sizeof(msg));
	}
}

int	nvmeib_srvr_api_lib_send_block_msg_to_server(const struct nvmeibs_toma_server_proc_buf *msg)
{
	struct nvmeibt_km_comm *p = _singleton;
	int rv = 0, fd = p->kernel.fd_toma2srvr;
	if (p->state_flags.use_async_api_and_sema_for_blocking_msgs)
		rv = _submit_msg_and_wait_for_ack(p, csc_t2s_blocking_msg_other, msg, sizeof(*msg));
	if (msg->type != NVMEIBS_TOMA_CLEAN_JOURNAL_FOR_DISK_RANGE) {
		if (!p->state_flags.use_async_api_and_sema_for_blocking_msgs)
			rv = NNVMEIBT_PWRITE_ATOMIC(tsmtls0, fd, msg, sizeof(*msg), 0, 0);
		return (rv < 0) ? -1 : 0;
	} else { // RonenHod: Write: our kernel API is weird - write() will return error anyway, where certain errno values indicate success... sigh.
		if (!p->state_flags.use_async_api_and_sema_for_blocking_msgs)
			rv = NNVMEIBT_PWRITE_ATOMIC(tsmtls3, fd, msg, sizeof(*msg), EALREADY, EINPROGRESS);
		if (rv >= 0) {
			return 0;
		} else if (rv == -EALREADY || rv == -EINPROGRESS) {	// Either a cleanup was already active, or a new "job" started
			N_Tf(tsmtls4, "cleanup request success: @STR", (rv == -EALREADY) ? "already active" : "started");
			return EINPROGRESS;	// Success
		} else {
			return -1;
		}
	}
}

int nvmeib_srvr_api_lib_send_block_msg_to_client(const struct nvmeibs_toma_client_proc_buf *msg, int buf_len)
{
	struct nvmeibt_km_comm *p = _singleton;
	int rv;
	NTOMA_ASSERT(__AUTOID__, buf_len <= p->max_msg_size.proc_send, "Msg too large @INT[b]", buf_len);
	if (p->state_flags.use_async_api_and_sema_for_blocking_msgs)	{
		rv = _submit_msg_and_wait_for_ack(p, csc_t2s_blocking_msg_to_io_clients, msg, buf_len);
	} else {
		rv = NNVMEIBT_PWRITE_ATOMIC(tsb2cp0, p->kernel.fd_toma2clnt, msg, buf_len, ENXIO, 0);
	}
	return ((rv == 0) || (rv == -ENXIO)) ? 0 : -1;	// Disconenct OK, or client already disconnected
}

int nvmeibt_km_comm_get_disk_info(const char *disk_name, struct nvmeib_disk_info *di)
{
	struct nvmeibt_km_comm *p = _singleton;
	struct disk_info *disk;
	int rv = -1;

	NFIN;
	nvmeibt_km_comm_lock(p);
	XDLIST_FOREACH(disk, &p->disks) {
		if (disk_info_is_equal(disk, disk_name)) {
			if (di)
				*di = disk->disk;
			rv = 0;
			break;
		}
	}
	nvmeibt_km_comm_unlock(p);
	NFOUT;
	return rv;
}

static int __get_srvr_buf_info(struct nvmeibt_Str *str, const char *path)
{
	int rv = -__LINE__, fd = NNVMEIBT_OPEN_READ(salgcd0, path, 1);
	if (fd > 0) {
		const int n_recv_bytes = NNVMEIBT_STR_FREAD_ATOMIC(salgcd2, str, fd);
		rv = ((n_recv_bytes <= 0) ? -__LINE__ : 0);
	}
	NNVMEIBT_CLOSE(salgcd4, fd);
	return rv;
}

int nvmeib_srvr_api_lib_get_csv_disks(struct nvmeibt_Str *str)
{
	struct nvmeibt_km_comm *p = _singleton;
	if (p->state_flags.use_async_api_instead_of_proc_files) {
		const struct nvmeib_t2s_request_srvr_info_req msg = { .opt_arg = 0,
			.type = NVMEIBS_TOMA_REQ_DISKS_CSV, .max_byte_len = (p->max_msg_size.nlink - NLMSG_HDRLEN) };
		return _submit_msg_and_wait_for_reply(p, csc_t2s_blocking_msg_req_info, &msg, sizeof(msg), str);
	}
	return __get_srvr_buf_info(str, TOMA_ROOT_DIR "proc/nvmeibs/disks.csv");
}

int nvmeib_srvr_api_lib_get_csv_nics(struct nvmeibt_Str *str)
{
	struct nvmeibt_km_comm *p = _singleton;
	if (p->state_flags.use_async_api_instead_of_proc_files) {
		const struct nvmeib_t2s_request_srvr_info_req msg = { .opt_arg = 0,
			.type = NVMEIBS_TOMA_REQ_NICS_CSV, .max_byte_len = (p->max_msg_size.nlink - NLMSG_HDRLEN) };
		return _submit_msg_and_wait_for_reply(p, csc_t2s_blocking_msg_req_info, &msg, sizeof(msg), str);
	}
	return __get_srvr_buf_info(str, TOMA_ROOT_DIR "proc/nvmeibs/nics.csv");
}

int nvmeib_srvr_api_lib_get_disk_smart_info(int seq, struct nvmeibt_Str *str)
{
	struct nvmeibt_km_comm *p = _singleton;
	if (seq >= 1000)
		seq = seq - 1000;		// Example: The '2' in /dev/nvme1002n1 -> /proc/nvmeibs/smart2
	if (p->state_flags.use_async_api_instead_of_proc_files) {
		const struct nvmeib_t2s_request_srvr_info_req msg = { .opt_arg = seq,
			.type = NVMEIBS_TOMA_REQ_DISK_SMART_CNT, .max_byte_len = (p->max_msg_size.nlink - NLMSG_HDRLEN) };
		return _submit_msg_and_wait_for_reply(p, csc_t2s_blocking_msg_req_info, &msg, sizeof(msg), str);
	} else {
		char path[256];
		snprintf(path, sizeof(path), TOMA_ROOT_DIR "proc/nvmeibs/smart%d", seq);
		return __get_srvr_buf_info(str, path);
	}
}

/***************************** Status proc reply messages *******************************/
struct status_str_ctx {					// Write status to mmap proc file in response to server request
	char 	*buf;
	size_t 	max_len;
	size_t 	cur_len;
	size_t 	total_needed_len;
	bool	is_overflow;
};

static int __status_str_printf(void *context, const char *format, ...)			// vsnprintf wrapper
{
	struct status_str_ctx 	*str_ctx = context;
	va_list					arglist;
	size_t					len_needed;
	const ssize_t			avail_len = (ssize_t)str_ctx->max_len - (ssize_t)str_ctx->cur_len;
	NTOMA_ASSERT(ttsrspfs0, avail_len > 0, "cur_len >= max_len when writing to buffer (overflow).");
	va_start(arglist, format);
	len_needed = vsnprintf(str_ctx->buf + str_ctx->cur_len, (size_t)avail_len, format, arglist);
	va_end(arglist);
	if ((len_needed >= (size_t)avail_len) && (!str_ctx->is_overflow)) {	// Print only first time on overflow, not for every function call
		N_Tf(ttsrspfs1, "Buffer overflow, max_len=@SIZE_T cur_len=@SIZE_T len_needed=@SIZE_T", str_ctx->max_len, str_ctx->cur_len, len_needed);
		str_ctx->is_overflow = 1;
	}
	str_ctx->total_needed_len += len_needed;
	str_ctx->cur_len += min((size_t)avail_len - 1, len_needed);
	return 0;
}

int nvmeib_srvr_api_lib_send_block_status_reply(const struct nvmeibs_msg_s2t_toma_status_req *req)
{
	int fd;
	char fname[256];
	struct status_str_ctx ctx = {.buf = NULL, .max_len = req->max_length, .cur_len = 0, .total_needed_len = 0, .is_overflow = 0	};
	struct nvmeibs_toma_server_proc_buf write_resp = {.type = NVMEIBS_TOMA_WRITE_STATUS_RESP};
	struct nvmeibs_msg_t2s_toma_status_resp *pl = &write_resp.status_resp_msg;

	// Open the mmap proc file
	snprintf(fname, sizeof(fname), TOMA_ROOT_DIR "proc/nvmeibs/" TOMA_STATUS_PROC_DIR "/%s", req->fname);
	fd = NNVMEIBT_OPEN(ttsrspfs5, fname, O_RDWR);
	if (fd < 0) {
		N_Wf(ttsrspfs6, "Failed to open mmap file @STR (@ERRNO - '@AUTO_ERRNO')", fname, errno);
		return -__LINE__;
	}
	ctx.buf = nvmeibt_mmap(req->max_length, fd, 0, true);		// map writable memory to the start of the proc file
	if (ctx.buf == MAP_FAILED) {
		N_Wf(ttsrspfs7, "Failed to mmap file @STR. (@ERRNO - '@AUTO_ERRNO')", fname, errno);
		NNVMEIBT_CLOSE(ttsrspfs8, fd);
		return -__LINE__;
	}

	_singleton->params.print_status_fn(req->type, &__status_str_printf, &ctx); 	// Print the status to the proc file
	nvmeibt_munmap(ctx.buf, req->max_length);
	NNVMEIBT_CLOSE(ttsrspfs9, fd);

	if (ctx.is_overflow)
		N_Tf(ttsrspfsc, "Output truncated from @SIZE_T to @SIZE_T characters", ctx.total_needed_len, ctx.cur_len);

	pl->handle = req->handle;
	pl->length = ctx.cur_len;
	pl->is_overflow = ctx.is_overflow;
	pl->handle_req = req->handle_req;
	if (nvmeib_srvr_api_lib_send_block_msg_to_server(&write_resp) < 0) {
		N_Wf(ttsrspfsa, "Failed to send response to server (@ERRNO - '@AUTO_ERRNO')", errno);
		return -__LINE__;
	}
	return 0;
}

/***************************** Lock maps API ***********************/
struct mmap_tbl nvmeib_srvr_api_lib_locks_map_get(const char *disk_name, uint64_t n_blksets, uint64_t offset, bool allow_write)
{
	char file_name[256];
	struct mmap_tbl rv = { .addr = NULL, .length = 0};
	size_t n_bytes = n_blksets * NVMEIB_LOCK_BLKSET_ENTRY_SIZE;	// Same calculation as in scan_locks_ec or disk_lock_allocate_(). Each blockset has a ram blockset-entry which we want to access
	int fd = -1;
	n_bytes = roundup(n_bytes, PAGE_SIZE);		// Align to page size to allow toma padding of pages.
	snprintf(file_name, sizeof(file_name), TOMA_ROOT_DIR "proc/nvmeibs/locks.%.*s", 128, disk_name);
	N_Tf(salddbmm0, "mmap file @STR n_bytes=@ZX at offset=@ZX n_blksets=@ZX writable=@BOOL_YN", file_name, n_bytes, offset, n_blksets, allow_write);
	fd = NNVMEIBT_OPEN(salddbmm1, file_name, O_RDWR);
	if (fd < 0) {
		N_Wf(salddbmm2, "Failed to open @STR (@AUTO_ERRNO). Possibly was removed immediatelly", file_name);
		return rv;
	}
	rv.addr = nvmeibt_mmap(n_bytes, fd, offset, allow_write);
	if (rv.addr == MAP_FAILED) {
		N_Wf(salddbmm3, "Failed to mmap locks table of disk=@STR @AUTO_ERRNO. Possibly was removed immediatelly", disk_name);
		rv.addr = NULL;
	} else {
		rv.length = n_bytes;
		_singleton->resource.n_lock_maps++;
	}
	NNVMEIBT_CLOSE(salddbmm6, fd); /* closing file descriptor does not unmap the region */
	return rv;
}

int nvmeib_srvr_api_lib_locks_map_put(const char *disk_name, struct mmap_tbl m)
{
	if (m.addr != NULL) {
		const int u_rv = nvmeibt_munmap(m.addr, m.length);
		if (u_rv == 0) {
			_singleton->resource.n_lock_maps--;
			return 0;
		}
	}
	N_Wf(vjs9o39, "Failed to munmap disk=@STR locks table @AUTO_ERRNO", disk_name);
	return -1;
}
