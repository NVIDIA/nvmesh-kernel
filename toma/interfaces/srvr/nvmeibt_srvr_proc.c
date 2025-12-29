#include "nvmeibt_srvr_proc.h"
#include "nvmeibt_common.h"
#include "utils/nvmeibt_str.h"

/***************************** Generic API Toma->Server ***********************/
static int fd_toma2srvr = -1;
static int fd_srvr2toma = -1;
static int fd_toma2clnt = -1;

int nvmeib_srvr_api_lib_get_fd_for_epoll(void) { return fd_srvr2toma; }

int nvmeib_srvr_api_lib_create(void)
{
	const char *proc_path_toma2srvr = TOMA_ROOT_DIR "proc/nvmeibs/toma_server";		// Toma->Srvr, See server nvmeibs_toma_create()
	const char *proc_path_srvr2toma = TOMA_ROOT_DIR "proc/nvmeibs/toma_server_events";	// Srvr->Toma
	const char *proc_path_toma2clnt = TOMA_ROOT_DIR "proc/nvmeibs/toma_clients";		// Toma->Clnt
	NTOMA_ASSERT(nsalc0, (fd_srvr2toma | fd_toma2clnt | fd_toma2srvr) == -1, "Wrong call, already initialized");
	fd_srvr2toma = NNVMEIBT_OPEN(nsalc1, proc_path_srvr2toma, O_RDWR);
	fd_toma2clnt = NNVMEIBT_OPEN(nsalc2, proc_path_toma2clnt, O_RDWR);
	fd_toma2srvr = NNVMEIBT_OPEN(nsalc3, proc_path_toma2srvr, O_RDWR);
	if ((fd_srvr2toma < 0) || (fd_toma2clnt < 0) || (fd_toma2srvr < 0)) {
		N_Ef(nsalc7, "Failed: @STR=@FD, @STR=@FD, @STR=@FD, @AUTO_ERRNO, FATAL: Without server toma will not live", proc_path_srvr2toma, fd_srvr2toma, proc_path_toma2clnt, fd_toma2clnt, proc_path_toma2srvr, fd_toma2srvr);
		exit(-1);
	}
	return 0;
}

static ssize_t __nvmeibt_pwrite_atomic(int fd, const void *vptr, size_t size, int OK_err_1, int OK_err_2)
{
	ssize_t rv = pwrite(fd, vptr, size, 0 /*offset*/);
	if (rv < 0) {
		if (errno == OK_err_1 || errno == OK_err_2) {
			N_Tf(6sjhk20, "Failed pwrite(fd=@FD vptr=@PTR size=@SIZEOF) (@AUTO_ERRNO))", fd, vptr, size);
		} else {
			N_Wf(35s83jm, "Failed pwrite(fd=@FD vptr=@PTR size=@SIZEOF) (@AUTO_ERRNO))", fd, vptr, size);
		}
	} else if ((size_t) rv != size) {
		N_Tf(rvsx83j, "Partial pwrite(fd=@FD, size=@SIZEOF) wrote rv=@ZX",	fd, size, rv);
		errno = 0;	// No ERRNO, since not an error
		return -1;
	}
	return rv;
}

#define NNVMEIBT_PWRITE_ATOMIC(name, __fd, __buf, __n, _OK_err_1, _OK_err_2) ({					\
	ssize_t		__rv__;																						\
	__MEASURE_TOOK_INIT();																					\
	__rv__ = __nvmeibt_pwrite_atomic((__fd), (__buf), (__n), (_OK_err_1), (_OK_err_2));			\
	__MEASURE_TOOK(N_IMf(name, "pwrite(@FD) Took @LLD ms", (__fd), NSEC_TO_MSEC(__measure_took_time_took_nsec)));	\
	__rv__;																									\
})

static int nvmeibt_toma_announce_ready(bool is_login)
{
	struct nvmeibs_toma_server_proc_buf buf;
	int rv;
	N_Tf(nsalcq, "is_login=@BOOL_YN", is_login);
	memset(&buf, 0, sizeof(buf));
	buf.type = (is_login ? NVMEIBS_TOMA_LOGIN : NVMEIBS_TOMA_LOGOUT);
	rv = NNVMEIBT_PWRITE_ATOMIC(nsalca, fd_toma2srvr, &buf, sizeof(buf), 0, 0);
	if (rv < 0) {
		N_Ef(nsalcb, "OOPS! Failed, fd=@FD of size @SIZEOF, rv=@RV", fd_toma2srvr, sizeof(buf), rv);
	}
	if (!is_login && 0) {	// Todo, properly close me
		NNVMEIBT_CLOSE(nsalcc, fd_srvr2toma);
		NNVMEIBT_CLOSE(nsalcd, fd_toma2clnt);
		NNVMEIBT_CLOSE(nsalce, fd_toma2srvr);
	}
	N_Tf(nsalcw, "Done");
	return 0;
}

int nvmeib_srvr_api_lib_handshake_server(void) { 	return nvmeibt_toma_announce_ready(true); }
int nvmeib_srvr_api_lib_destroy(void){ 				return nvmeibt_toma_announce_ready(false); }

/***************************** mmap shared memory (server /proc files, disk locks file) *******************************/
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

	if (mprotect(mapped_padded, 1, PROT_WRITE) >= 0) {		// mprotect() __len == 1 so we only modify permissions for a single page. In case the struct is bigger than the size of one page we crash immediately after when we try to write to the 2nd page
		init_padded_mmap_magic_number_struct(mapped_padded, padded_length);			// Writing to the first page details about the allocation and setting it back to no access permissions
		if (mprotect(mapped_padded, 1, PROT_NONE) >= 0) {
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
	mprotect(mapped_padded, 1, PROT_READ);
	if ((me->addr != (void*)me) || (me->length != length_padded) || (me->magic_num != PADDED_MMAP_MAGIC_NUM)) {
		NTOMA_ASSERT(salddbmmf6, false, "Magic number mismatch, expected={@PTR, len=@ZX, magic=@LLX}, found={@PTR, len=@ZX, magic=@LLX}",
					   mapped_padded, length_padded, PADDED_MMAP_MAGIC_NUM,
					   me->addr, me->length, me->magic_num);
		errno = EINVAL;
	} else {
		mprotect(mapped_padded, 1, PROT_WRITE);
		memset(mapped_padded, 0, sizeof(struct padded_mmap_magic_number));
		rv = munmap(mapped_padded, length_padded);
		NTOMA_ASSERT(salddbmmf8, rv == 0, "munmap failed, probably bad args passed. @AUTO_ERRNO");
	}
out:
	NFOUT;
	return rv;
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

int nvmeib_srvr_api_lib_fill_and_send_status_reply(const struct nvmeibs_msg_s2t_toma_status_req *req,
	void (*your_print_status_fn)(enum nvmeibs_toma_status_type, int (*printf_fn)(void *ctx, const char *fmt, ...), void *ctx))
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

	your_print_status_fn(req->type, &__status_str_printf, &ctx);	// Print the status to the proc file
	nvmeibt_munmap(ctx.buf, req->max_length);
	NNVMEIBT_CLOSE(ttsrspfs9, fd);

	if (ctx.is_overflow)
		N_Tf(ttsrspfsc, "Output truncated from @SIZE_T to @SIZE_T characters", ctx.total_needed_len, ctx.cur_len);

	pl->handle = req->handle;
	pl->length = ctx.cur_len;
	pl->is_overflow = ctx.is_overflow;
	pl->handle_req = req->handle_req;
	if (nvmeibt_toma_send_msg_to_local_server(&write_resp) < 0) {
		N_Wf(ttsrspfsa, "Failed to send response to server (@ERRNO - '@AUTO_ERRNO')", errno);
		return -__LINE__;
	}
	return 0;
}

/***************************** Generic messages *******************************/
int nvmeibt_toma_send_msg_to_local_server(const struct nvmeibs_toma_server_proc_buf *msg)
{
	int	rv = 0;
	if (msg->type != NVMEIBS_TOMA_CLEAN_JOURNAL_FOR_DISK_RANGE) {
		if (NNVMEIBT_PWRITE_ATOMIC(tsmtls0, fd_toma2srvr, msg, sizeof(*msg), 0, 0) < 0) {
			N_Tf(tsmtls1, "pwrite(@FD) failed, @AUTO_ERRNO", fd_toma2srvr);
			rv = -1;
		}
	} else {
		// RonenHod: Write: our kernel API is weird - write() will return error anyway, where certain errno values indicate success... sigh.
		rv = NNVMEIBT_PWRITE_ATOMIC(tsmtls3, fd_toma2srvr, &msg, sizeof(*msg), EALREADY, EINPROGRESS);
		if (rv >= 0) {
			rv = 0;
		} else if ((rv < 0) && (errno == EALREADY || errno == EINPROGRESS)) {
			// Either a cleanup was already active, or a new "job" started
			N_Tf(tsmtls4, "cleanup request success: @STR", (errno == EALREADY) ? "already active" : "started");
			rv = EINPROGRESS;
		} else {
			N_Ef(tsmtls5, "pwrite(@FD) failed, wr_cnt=@RV @AUTO_ERRNO", fd_toma2srvr, rv);
			rv = -1;
		}
	}
	return rv;
}

int nvmeibt_toma_get_msg_from_local_server(struct nvmeibs_toma_server_proc_buf *msg, int max_len, bool *is_server_event)
{
	const int rv = read(fd_srvr2toma, msg, max_len);
	if (rv < (int)sizeof(msg->handle)) {
		N_Ef(tsmtls8, "Failed read fd=@FD rv=@RV @AUTO_ERRNO", fd_srvr2toma, rv);
		return -1;
	}
	*is_server_event = (msg->zero == 0);			// The first u64 decides between server event or client message to TOMA: For server event, the zero member must be 0, and then the type member indicates the server event type. For client messages, the client-uid (cid) - which occupies the higher half of the handle- may not be zero. (see also common/nvmeib_shared.h)
	return rv;
}

int nvmeibt_toma_send_buf_to_client(const char *buf, int buf_len, const char *clnt_host)
{
	int rv = 0;
	if (NNVMEIBT_PWRITE_ATOMIC(tsb2cp0, fd_toma2clnt, buf, buf_len, ENXIO, 0) < 0) {
		if (errno == ENXIO) {
			N_Tf(tsb2cp1, "write(@FD, handle=@PTR, len=@LEN) failed because the client=@MY_HOSTNAME already disconnected", fd_toma2clnt, buf, buf_len, clnt_host);
		} else {
			N_Tf(tsb2cp2, "write(@FD, handle=@PTR, len=@LEN) failed, @AUTO_ERRNO", fd_toma2clnt, buf, buf_len);
			rv = -1;
		}
	}
	return rv;
}

static int __get_srvr_csv(struct nvmeibt_Str *str, bool is_disks)
{
	const char *path = (is_disks ? TOMA_ROOT_DIR "proc/nvmeibs/disks.csv" : TOMA_ROOT_DIR "proc/nvmeibs/nics.csv");
	int rv = 0, fd = NNVMEIBT_OPEN_READ(salgcd0, path, 1);
	if (fd > 0) {
		const int n_recv_bytes = NNVMEIBT_STR_FREAD_ATOMIC(salgcd2, str, fd);
		if (n_recv_bytes <= 0) {
			rv = -__LINE__;
		}
	} else {
		rv = -__LINE__;
	}
	NNVMEIBT_CLOSE(salgcd4, fd);
	return rv;
}

int nvmeib_srvr_api_lib_get_csv_disks(struct nvmeibt_Str *str) { return __get_srvr_csv(str, true); }
int nvmeib_srvr_api_lib_get_csv_nics( struct nvmeibt_Str *str) { return __get_srvr_csv(str, false); }

/* Original shell code:
* 		pcidrivers_base_path=/sys/bus/pci/drivers
* 		pcifd=$pcidrivers_base_path/nvme/$nvmepci
* 		#check if pci binded to nvme driver
* 		if [ ! -e "$pcifd" ]; then
* 			echo "Error locating pcifd for dev=$dev_id"
* 			exit 0
* 			fi
* 		#unbind pci from nvme driver
* 		echo -n "$nvmepci" > $pcidrivers_base_path/nvme/unbind
* 		if [ "$?" -eq "0" ] && [ -d "$pcidrivers_base_path/nvmeibs" ]; then
* 			#bind to nvmeibs
* 			echo -n "$nvmepci" > $pcidrivers_base_path/nvmeibs/bind
* 			fi
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
	}
	NNVMEIBT_CLOSE(salddbmm6, fd); /* closing file descriptor does not unmap the region */
	return rv;
}

int nvmeib_srvr_api_lib_locks_map_put(const char *disk_name, struct mmap_tbl m)
{
	if (m.addr && (nvmeibt_munmap(m.addr, m.length) < 0)) {
		N_Wf(vjs9o39, "Failed to munmap disk=@STR locks table @AUTO_ERRNO", disk_name);
		return -1;
	}
	return 0;
}
