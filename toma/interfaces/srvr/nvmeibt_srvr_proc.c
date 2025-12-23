#include "nvmeibt_srvr_proc.h"
#include "nvmeibt_common.h"

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
