/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include "nvmeibt_debug.h"
#include "nvmeibt_common.h"
#include <signal.h>
#include <sys/signalfd.h>
#include <sys/select.h>
#include "nvmeibt_os_signal.h"
#include <sys/un.h>
#include <sys/uio.h>
#include <sys/ucontext.h>
#include <sys/socket.h>
#include "nvmeibt_ds.h"
#include "../common/nvmeib_hash.h"

bool nvmeibt_toma_is_main_thread(void);
static int g_log_file_fd = -1;
static const char *priv_exe_name;

static void open_syslog_socket(const char *exe_name)
{
	struct sockaddr_un sys_log_addr;

	priv_exe_name = exe_name;
	sys_log_addr.sun_family = AF_UNIX;
	nvmeibt_strlcpy(sys_log_addr.sun_path, _PATH_LOG, sizeof(sys_log_addr.sun_path));
	g_log_file_fd =
		NNVMEIBT_SOCKET(trace_toma_open_syslog_socket, AF_UNIX, SOCK_DGRAM, 0);
	if (g_log_file_fd < 0) {
		return;
	}

	if (connect(g_log_file_fd, &sys_log_addr, sizeof(sys_log_addr)) == -1) {
		NNVMEIBT_CLOSE(trace_1_toma_open_syslog_socket, g_log_file_fd);
		return;
	}
}


static sigset_t block_sig_mask;

void handle_sig_fd(int signals_fd, void (*sig_handler)(int32_t n, uint64_t addr))
{
	struct signalfd_siginfo si;
	ssize_t res;

	NFIN;
	res = read(signals_fd, &si, sizeof(si));
	if (res < 0) {
		if (errno == EAGAIN) {
			goto out;
		}
		N_Ef(trace_toma_handle_sig_fd, "failed reading signals_fd   @ERRNO @AUTO_ERRNO", errno);
		goto out;
	}
	if (res != sizeof(si)) {
		N_Ef(trace_1_toma_handle_sig_fd, "read() signals_fd returned unexpected value @RES_SSIZET", res);
		goto out;
	}
	sig_handler(si.ssi_signo, si.ssi_addr);

out:
	NFOUT;
}

// Write a string to syslog, writing each '\n'-separated part separately so it
// appears on a different syslog line.
// The name of the executable is also prefixed before each emitted part.
// This function is async-safe and can be safely called from a signal handler.
static void write_buf_to_syslog(const char* buf, int buf_size)
{
	struct iovec iovecs[3];
	const char* p = buf;
	const char* pend = buf + buf_size;

	if (g_log_file_fd < 0)
		return;

	iovecs[0].iov_base = (void*)priv_exe_name;
	iovecs[0].iov_len = strlen(iovecs[0].iov_base);
	iovecs[1].iov_base = ": ";
	iovecs[1].iov_len = strlen(iovecs[1].iov_base);

	// break prints at every '\n' characters as syslog will ignore them
	while (p < pend) {
		const char* first_newline = strchr(p, '\n');
		int nbytes_until_newline;
		if (!first_newline) {
			first_newline = pend;
		}
		nbytes_until_newline = first_newline - p;
		iovecs[2].iov_base = (void*)p;
		iovecs[2].iov_len = nbytes_until_newline;
		writev(g_log_file_fd, iovecs, ARRAY_SIZE(iovecs));
		p = first_newline + 1;
	}
}

#define MAX_STACK_LEVELS 20
typedef unsigned long long ull_t;

// Make sure backtrace() is called once when the process starts so it doesn't
// need to malloc() memory on it's initial execution (when dlopen()ing the
// gcc unwind library).
static void __attribute__ ((constructor)) init_backtrace(void)
{
	void *bt[MAX_STACK_LEVELS];
	backtrace(bt, MAX_STACK_LEVELS);
}

static void output_backtrace(void)
{
	void *buffer[MAX_STACK_LEVELS];
	int levels = backtrace(buffer, MAX_STACK_LEVELS);
	int i;
	char output[256];

	int length = snprintf(output, sizeof(output), "Backtrace");
	write_buf_to_syslog(output, length);
	length = snprintf(output, sizeof(output), "---------");
	write_buf_to_syslog(output, length);
	for (i = 0; i < levels; i++) {
		length = snprintf(output, sizeof(output), "%02d: %016llx\n", i, (ull_t)buffer[i]);
		// write to syslog
		write_buf_to_syslog(output, length);
	}
	length = snprintf(output, sizeof(output), "End Trace");
	write_buf_to_syslog(output, length);
}

static void sig_segv_func(int n, __attribute__((__unused__)) siginfo_t *info,
	__attribute__((__unused__))void *vcontext)
{
#if __x86_64__
	static char out1[] __attribute__ ((unused)) = "SIGSEGV on main thread - waiting for logger to finish...\n";
	static char out2[] __attribute__ ((unused)) = "SIGSEGV on main thread - logger is done\n";
	static char out3[] __attribute__ ((unused)) = "SIGSEGV on other (not main) thread\n";
	char output[1024];
	const greg_t* gregs = ((ucontext_t*)vcontext)->uc_mcontext.gregs;

	size_t reg_output_bytes = snprintf(output, sizeof(output),
			 "RIP: %016llx RSP: %016llx EFLAGS: %08llx\n"
			 "RAX: %016llx RBX: %016llx RCX: %016llx\n"
			 "RDX: %016llx RSI: %016llx RDI: %016llx\n"
			 "RBP: %016llx R08: %016llx R09: %016llx\n"
			 "R10: %016llx R11: %016llx R12: %016llx\n"
			 "R13: %016llx R14: %016llx R15: %016llx\n"
			 "CR2: %016llx signum: %02d\n"
			 , (ull_t)gregs[REG_RIP], (ull_t)gregs[REG_RSP], (ull_t)gregs[REG_EFL]
			 , (ull_t)gregs[REG_RAX], (ull_t)gregs[REG_RBX], (ull_t)gregs[REG_RCX]
			 , (ull_t)gregs[REG_RDX], (ull_t)gregs[REG_RSI], (ull_t)gregs[REG_RDI]
			 , (ull_t)gregs[REG_RBP], (ull_t)gregs[REG_R8],  (ull_t)gregs[REG_R9]
			 , (ull_t)gregs[REG_R10], (ull_t)gregs[REG_R11], (ull_t)gregs[REG_R12]
			 , (ull_t)gregs[REG_R13], (ull_t)gregs[REG_R14], (ull_t)gregs[REG_R15]
			 , (ull_t)gregs[REG_CR2], n);
	fprintf(stderr, "%s[%d]:%s(): n=%d\n",__FILE__, __LINE__, __FUNCTION__, n);
	if (reg_output_bytes > sizeof(output)) {
		reg_output_bytes = sizeof(output);
		output[sizeof(output) - 1] = '\n';
	}

	// write to syslog
	write_buf_to_syslog(output, reg_output_bytes);

	/* we are in a sigsegv handler - probably memory is corrupted.
	   there are only few syscalls we may call in current context
	   and non of them is pthread_???.
	   however we would like to dump the pending logs to ease the crash
	   handling.  if the crash is on the main thraed we can try and gracefuly
	   shutdown the logger.  if on the other hand the crash is on the logger
	   thread then we will have the core file.
	   so we check for the thread and try to stop with using any pthread API.
	*/
	if (nvmeibt_toma_is_main_thread()) {
		nvmeibt_write(STDERR_FILENO, out1, sizeof(out1));
		nvmeibt_write(STDERR_FILENO, output, reg_output_bytes);
		nvmeibt_write(STDERR_FILENO, out2, sizeof(out2));
	}
	else {
		nvmeibt_write(STDERR_FILENO, out3, sizeof(out3));
	}

#endif //  __x86_64__
	output_backtrace();

	nvmeibt_flush_all_and_terminate();

	signal(n, SIG_DFL);
}

int init_signal_handling(const char *exe_name)
{
	sigset_t mask;
	struct sigaction act;
	int signals_fd;
	int rv = 0;
	int pt_err;

	NFIN;

	open_syslog_socket(exe_name);

	/* firstly ignore SIGPIPE so write to close socket (the MCS UDS)
	    wont kill TOMA
	*/
	signal(SIGPIPE, SIG_IGN);
	// this function is called in the main thread.  it blocks all signals and
	// therefore all newly created threads will inherit the blockage...
	// do the signal handling...
	memset(&act, 0, sizeof(act));
	if (sigaction(SIGTERM, &act, 0)) {
		N_Ef(trace_toma_init_signal_handling, "sigaction for SIGTERM failed @ERRNO @AUTO_ERRNO", errno);
		rv = -1;
		goto out;
	}
	if (sigaction(SIGINT, &act, 0)) {
		N_Ef(trace_1_toma_init_signal_handling, "sigaction for SIGINT failed @ERRNO @AUTO_ERRNO", errno);
		rv = -1;
		goto out;
	}
	if (sigaction(SIGCHLD, &act, 0)) {
		N_Ef(trace_2_toma_init_signal_handling, "sigaction for SIGCHLD failed @ERRNO @AUTO_ERRNO", errno);
		rv = -1;
		goto out;
	}
	if (sigaction(SIGUSR1, &act, 0)) {
		N_Ef(trace_3_toma_init_signal_handling, "sigaction for SIGUSR1 failed @ERRNO @AUTO_ERRNO", errno);
		rv = -1;
		goto out;
	}
	if (sigaction(SIGUSR2, &act, 0)) {
		N_Ef(trace_3_1_toma_init_signal_handling, "sigaction for SIGUSR2 failed @ERRNO @AUTO_ERRNO", errno);
		rv = -1;
		goto out;
	}
	if (sigaction(SIGHUP, &act, 0)) {
		N_Ef(tcvahkw, "sigaction for SIGHUP failed @ERRNO @AUTO_ERRNO", errno);
		rv = -1;
		goto out;
	}
	sigemptyset(&mask);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGCHLD);
	sigaddset(&mask, SIGUSR1);
	sigaddset(&mask, SIGUSR2);
	sigaddset(&mask, SIGHUP);
	pt_err = pthread_sigmask(SIG_BLOCK, &mask, &block_sig_mask);
	if (pt_err != 0) {
		errno = pt_err;
		N_Ef(trace_4_toma_init_signal_handling, "pthread_sigmask failed @ERRNO @AUTO_ERRNO", pt_err);
		rv = -1;
		goto out;
	}

	signals_fd = signalfd(-1, &mask, SFD_NONBLOCK);
	if (signals_fd < 0) {
		N_Ef(trace_5_toma_init_signal_handling, "signalfd failed @ERRNO @AUTO_ERRNO", errno);
		rv = -1;
		goto out;
	}

	// bang! we can get SIGTERM or SIGINT at this point, but it will be
	// delivered while we are in the thread that calls pselect(), because now
	// we block SIGTERM & SIGINT...
	memset(&act, 0, sizeof(act));
	act.sa_sigaction = sig_segv_func;
	act.sa_flags = SA_SIGINFO;
	sigaction(SIGSEGV, &act, NULL);
	sigaction(SIGBUS, &act, NULL);
	rv = signals_fd;

out:
	NFOUT;
	return rv;
}

int nvmeibt_nonblock_fd(int fd)
{
	int flags;
	int rv = -1;

	NFIN;
	flags = fcntl(fd, F_GETFL);
	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
		N_Ef(error_common_nvmeibt_nonblock_fd, "fcntl on fd=@FD flags @FLAGS_INT (@AUTO_ERRNO)", fd, flags);
		goto out;
	}

	rv = 0;

out:
	NFOUT;
	return rv;
}
