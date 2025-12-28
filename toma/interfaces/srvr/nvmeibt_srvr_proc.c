#include "nvmeibt_srvr_proc.h"
#include "nvmeibt_common.h"
#include "utils/nvmeibt_str.h"

/***************************** Generic API Toma->Server ***********************/
static const char proc_path_toma2srvr[] = TOMA_ROOT_DIR "proc/nvmeibs/toma_server";			// Toma->Srvr
static const char proc_path_srvr2toma[] = TOMA_ROOT_DIR "proc/nvmeibs/toma_server_events";	// Srvr->Toma
static const char proc_path_toma2clnt[] = TOMA_ROOT_DIR "proc/nvmeibs/toma_clients";		// Toma->Clnt
static int fd_toma2srvr = -1;
static int fd_srvr2toma = -1;
static int fd_toma2clnt = -1;

int nvmeibt_toma_get_local_server_fd(void) { return fd_toma2srvr;}
int nvmeibt_toma_get_local_server_fd_events(void) { return fd_srvr2toma; }

static int __open_fd_toma2srvr(void)
{
	int rv = 0;
	fd_toma2srvr = NNVMEIBT_OPEN(trace_toma_open_local_server_fd, proc_path_toma2srvr, O_RDWR);
	N_Tf(t_07_nvmeibt_open_fd_comm, "open('@STR')=@LOCAL_SERVER_FD, errno=@AUTO_ERRNO", proc_path_toma2srvr, fd_toma2srvr);
	if (fd_toma2srvr == -1)
		rv = -1;
	return rv;
}

int nvmeibt_open_fd_clnt_and_local_srvr(void)
{
	int	rv = 0;

	NFIN;
	if (fd_srvr2toma == -1) {
		fd_srvr2toma = NNVMEIBT_OPEN(t_01_nvmeibt_open_fd_comm, proc_path_srvr2toma, O_RDWR);
		if (fd_srvr2toma == -1) {
			N_Ef(t_02_nvmeibt_open_fd_comm, "open('@STR') failed, @AUTO_ERRNO, FATAL: Without server toma will not live", proc_path_srvr2toma);
			exit(-1); //without server toma should not live
		} else {
			N_Tf(t_03_nvmeibt_open_fd_comm, "opened('@STR')   fd_srvr2toma=@LOCAL_SERVER_EVENTS_FD", proc_path_srvr2toma, fd_srvr2toma);
		}
	}
	if (fd_toma2clnt == -1) {
		fd_toma2clnt = NNVMEIBT_OPEN(t_04_nvmeibt_open_fd_comm, proc_path_toma2clnt, O_RDWR);
		if (fd_toma2clnt == -1) {
			N_Ef(t_05_nvmeibt_open_fd_comm, "open('@STR') failed, @AUTO_ERRNO", proc_path_toma2clnt);
			rv = -1;
		} else {
			N_Tf(t_06_nvmeibt_open_fd_comm, "opened('@STR')   fd_toma2clnt=@CLIENTS_FD", proc_path_toma2clnt, fd_toma2clnt);
		}
	}
	if (fd_toma2srvr == -1) {
		rv = __open_fd_toma2srvr();
	}
	NFOUT;
	return rv;
}

static int __login_into_server(int is_login)
{
	struct nvmeibs_toma_server_proc_buf buf;
	int srv_fd = nvmeibt_toma_get_local_server_fd();
	int rv = 0;

	NFIN;
	memset(&buf, 0, sizeof(buf));
	if (srv_fd < 0) {
		goto out;	// Too early for login/logout
	}

	buf.type = (is_login ? NVMEIBS_TOMA_LOGIN : NVMEIBS_TOMA_LOGOUT);
	if (NNVMEIBT_PWRITE_ATOMIC(trace_toma_login_into_server, srv_fd, &buf, sizeof(buf), 0, 0, 0) < 0) {
		N_Ef(trace_1_toma_login_into_server, "OOPS! Failed pwrite(fd_toma2srvr,...) of size @SIZEOF)", sizeof(buf));
		if (!is_login) {
			N_Tf(trace_2_toma_login_into_server, "For now patching using close() and open");
			NNVMEIBT_CLOSE(trace_3_toma_login_into_server, srv_fd);
			if (__open_fd_toma2srvr() == 0) {
				goto out;
			}
		}
		rv = -1;
		exit(-1);
	}
out:
	NFOUT;
	return rv;
}

int nvmeibt_toma_announce_ready(int is_on)
{
	const int srv_fd = nvmeibt_toma_get_local_server_fd();
	int rv = 0;

	NFIN;
	if ((srv_fd < 0) || __login_into_server(is_on) != 0) {
		N_Tf(trace_toma_nvmeibt_toma_announce_ready, "Failed: is_on=@RV, proc: @STR", is_on, proc_path_toma2srvr);
		rv = -1;
	}
	NFOUT;
	return rv;
}

/***************************** Status proc reply messages *******************************/
#include <sys/mman.h>

struct status_str_ctx {					// Write status to mmap proc file in response to server request
	char 	*buf;
	size_t 	max_len;
	size_t 	cur_len;
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
	if (len_needed >= (size_t)avail_len) {
		N_Tf(ttsrspfs1, "Buffer overflow, max_len=@SIZE_T cur_len=@SIZE_T len_needed=@SIZE_T", str_ctx->max_len, str_ctx->cur_len, len_needed);
		str_ctx->is_overflow = 1;
	}
	str_ctx->cur_len += min((size_t)avail_len - 1, len_needed);
	return 0;
}

int nvmeib_srvr_api_lib_fill_and_send_status_reply(const struct nvmeibs_msg_s2t_toma_status_req *req,
	void (*your_print_status_fn)(enum nvmeibs_toma_status_type, int (*printf_fn)(void *ctx, const char *fmt, ...), void *ctx))
{
	int mmap_fd = -1;
	char mmap_fname[PATH_MAX];
	struct status_str_ctx status_str_ctx = {.buf = NULL, .max_len = req->max_length, .cur_len = 0, .is_overflow = 0	};
	struct nvmeibs_toma_server_proc_buf write_resp = {.type = NVMEIBS_TOMA_WRITE_STATUS_RESP};
	struct nvmeibs_msg_t2s_toma_status_resp *pl = &write_resp.status_resp_msg;

	// Open the mmap proc file
	#define TOMA_STATUS_PROC_PATH TOMA_ROOT_DIR "proc/nvmeibs/" TOMA_STATUS_PROC_DIR
	snprintf(mmap_fname, sizeof(mmap_fname), "%s/%s", TOMA_STATUS_PROC_PATH, req->fname);
	N_Tf(ttsrspfs4, "mmap file @MMAP_FNAME", mmap_fname);
	if ((mmap_fd = NNVMEIBT_OPEN(ttsrspfs5, mmap_fname, O_RDWR)) < 0) {
		N_Wf(ttsrspfs6, "Failed to open mmap file @MMAP_FNAME (@ERRNO - '@AUTO_ERRNO')", mmap_fname, errno);
		return -1;
	}


	status_str_ctx.buf = nvmeibt_mmap(req->max_length, mmap_fd);		// memory map the proc file
	if (status_str_ctx.buf == MAP_FAILED) {
		N_Wf(ttsrspfs7, "Failed to mmap file @MMAP_FNAME. (@ERRNO - '@AUTO_ERRNO')", mmap_fname, errno);
		NNVMEIBT_CLOSE(ttsrspfs8, mmap_fd);
		return -1;
	}

	your_print_status_fn(req->type, &__status_str_printf, &status_str_ctx);	// Print the status to the proc file
	nvmeibt_munmap(status_str_ctx.buf, req->max_length);
	NNVMEIBT_CLOSE(ttsrspfs9, mmap_fd);

	pl->handle = req->handle;
	pl->length = status_str_ctx.cur_len;
	pl->is_overflow = status_str_ctx.is_overflow;
	pl->handle_req = req->handle_req;
	if (nvmeibt_toma_send_msg_to_local_server(&write_resp) < 0) {
		N_Wf(ttsrspfsa, "Failed to send response to server (@ERRNO - '@AUTO_ERRNO')", errno);
		return -1;
	}
	return 0;
}

/***************************** Generic messages *******************************/
int nvmeibt_toma_send_msg_to_local_server(const struct nvmeibs_toma_server_proc_buf *msg)
{
	const int srv_fd = nvmeibt_toma_get_local_server_fd();
	int	rv = 0;

	NFIN;
	if (NNVMEIBT_PWRITE_ATOMIC(trace_toma_nvmeibt_toma_send_msg_to_local_server, srv_fd, msg, sizeof(*msg), 0, 0, 0) < 0) {
		N_Tf(trace_1_toma_nvmeibt_toma_send_msg_to_local_server, "pwrite('@STR') failed, @AUTO_ERRNO", proc_path_toma2srvr);
		rv = -1;
	}
	NFOUT;
	return rv;
}

int nvmeibt_toma_get_msg_from_local_server(struct nvmeibs_toma_server_proc_buf *msg, int max_len, bool *is_server_event)
{
	int rv = 0;

	rv = read(fd_srvr2toma, msg, max_len);
	if (rv < (int)sizeof(msg->handle)) {
		N_Ef(error_topology_nvmeibt_topology_handle_local_server_event, "Failed read(fd_srvr2toma) rv=@RV @AUTO_ERRNO", rv);
		rv = -1;
	} else {
		*is_server_event = (msg->zero == 0);			// The first u64 decides between server event or client message to TOMA: For server event, the zero member must be 0, and then the type member indicates the server event type. For client messages, the client-uid (cid) - which occupies the higher half of the handle- may not be zero. (see also common/nvmeib_shared.h)
	}
	return rv;
}

int nvmeibt_toma_send_buf_to_client(const char *buf, int buf_len, const char *clnt_host)
{
	int rv = 0;
	if (NNVMEIBT_PWRITE_ATOMIC(tsb2cp0, fd_toma2clnt, buf, buf_len, 0, ENXIO, 0) < 0) {
		if (errno == ENXIO) {
			N_Tf(tsb2cp1, "write('@STR', handle=@PTR, len=@LEN) failed because the client=@MY_HOSTNAME already disconnected", proc_path_toma2clnt, buf, buf_len, clnt_host);
		} else {
			N_Tf(tsb2cp2, "write('@STR', handle=@PTR, len=@LEN) failed, @AUTO_ERRNO",    proc_path_toma2clnt, buf, buf_len);
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

struct mmap_tbl nvmeib_srvr_api_lib_locks_map_get(const char *disk_uuid, uint64_t n_blksets, uint64_t offset)
{
	char file_name[256];
	struct mmap_tbl rv = { .addr = NULL, .length = 0};
	size_t n_bytes = n_blksets * NVMEIB_LOCK_BLKSET_ENTRY_SIZE;	// Same calculation as in scan_locks_ec or disk_lock_allocate_(). Each blockset has a ram blockset-entry which we want to access
	int fd = -1;
	n_bytes = roundup(n_bytes, PAGE_SIZE);		// Align to page size to allow toma padding of pages.
	snprintf(file_name, sizeof(file_name), TOMA_ROOT_DIR "proc/nvmeibs/locks.%.*s", 128, disk_uuid);
	N_Tf(salddbmm0, "mmap file @FILE_NAME n_bytes=@LENGTH_SIZET at offset=@OFFSET_INT n_blksets=@UINT64_TX", file_name, n_bytes, offset, n_blksets);
	fd = NNVMEIBT_OPEN(salddbmm1, file_name, O_RDWR);
	if (fd < 0) {
		N_Wf(salddbmm2, "Failed to open @FILE_NAME (@AUTO_ERRNO). Possibly was removed immediatelly", file_name);
		return rv;
	}
	rv.addr = nvmeibt_mmap(n_bytes, fd);
	if (rv.addr == MAP_FAILED) {
		N_Wf(salddbmm3, "Failed to mmap locks table of disk=@STR @AUTO_ERRNO. Possibly was removed immediatelly", disk_uuid);
		rv.addr = NULL;
	} else {
		rv.length = n_bytes;
	}
	NNVMEIBT_CLOSE(salddbmm6, fd); /* closing file descriptor does not unmap the region */
	return rv;
}

int nvmeib_srvr_api_lib_locks_map_put(const char *disk_uuid, struct mmap_tbl m)
{
	if (m.addr && (nvmeibt_munmap(m.addr, m.length) < 0)) {
		N_Wf(vjs9o39, "Failed to munmap disk=@STR locks table @AUTO_ERRNO", disk_uuid);
		return -1;
	}
	return 0;
}
