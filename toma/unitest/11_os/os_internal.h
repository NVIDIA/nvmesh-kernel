/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#pragma once
/* Emulation of operating system backend which implements the public API */
#include "os_public.h"
#include <sys/un.h>
#include "../sandbox_util.h"

#define OFFSET_NONE ((off_t) -1)				// Offset used to indicate a non-random-access operation like send()/recv() or read()/write(), rather than a random access operation like pread()/pwrite().
struct TSB_fd_otherside {		// Every file descriptor (file, socket, ...) implementation must derive from this sub class. Sandbox injects data to Toma via those functions
	// The send()/recv() operations act as a generic I/O interface that is common to both: seekable (block device, local file), and non-seekable (pipe, socket, FIFO).
	// Use offset == OFFSET_NONE to indicate a non-random I/O operation like read()/write() or send()/recv().
	// When offset != OFFSET_NONE (i.e. > 0) this indicates a pread()/pwrite() operation.
	ssize_t (*send)(int fd, const void *buf, size_t n, off_t offset, int flags);	// Toma sends data to simulator
	ssize_t (*recv)(int fd,       void *buf, size_t n, off_t offset, int flags);	// Toma receives data from simulator
	bool    (*has_data)(void);									// epoll()/select() on this socket/file-descriptor
	struct TSB_fd_impl *sock;									// Pointer to the file descriptor structure which uses me
};

static inline ssize_t fd_otherside_read_only_illegal_send(int fd, const void *buf, size_t n, off_t offset, int flags) {
	BUG_ON(true || (fd < 2) || (n == 0) || (buf == NULL) || (offset != OFFSET_NONE) || (flags != 0));
	return 0;
}

static inline ssize_t fd_otherside_write_only_illegal_recv(int fd, void *buf, size_t n, off_t offset, int flags) {
	BUG_ON(true || (fd < 2) || (n == 0) || (buf == NULL) || (offset != OFFSET_NONE) || (flags != 0));
	return 0;
}

/*****************************************************************************/
struct TSB_fd_impl {			// Implementation of a single file descriptor (file/bdev/socket/etc... used by Toma)
	FILE *f;			// Sometimes we need a backend file emulating this file
	int fd;				// Real system file descriptor emulating this socket/fd
	int dom;
	int type;
	int proto;
	u32 len;
	int ref_cnt;								// Same fd' is sometimes use multiple times by the simulator. accept(). Todo, clean this
	struct sockaddr_un addr;
	struct TSB_fd_otherside *other_side;		// Here sandbox connects to socket from the other side
};

struct TSB_os_mmap_impl {						// Intercept file mmap by toma to be able to inject values
	void *addr;
	size_t len;
};
void TSB_os_mmap_impl_clear(struct TSB_os_mmap_impl *mi, size_t length);

/*****************************************************************************/
struct TSB_all_fds_tbl {					// Operating system, list of all file descriptors used by Toma
	int n_fds;								// Number of file descriptors currently opened by Toma
	int debug_offset;						// Prevent confusion between real descriptors and emulated
	pthread_mutex_t mutex;					// Guard against Toma multi-threaded open()/close()
	struct TSB_fd_impl fd_arr[32];			// Max amount of file descriptors used by toma
};

int TSB_all_fds_tbl_create_fd(const char *file_name, int flags);		// Production code uses override_open. Simulators use this one to avoid confusion

/*****************************************************************************/
struct TSB_netlink_mock {
	struct TSB_fd_otherside o;				// Here server simulator will connect as other side
	unsigned n_recv_msgs;
	pthread_mutex_t mutex;					// Thread-safe message queue for netlink access (by server-lib Toma thread and by server simulator )
	#define TSB_NL_QUEUE_SIZE 8				// Simple fixed-size queue of messages
	#define TSB_NL_MSG_SIZE 512
	struct {
		char data[TSB_NL_MSG_SIZE];
		size_t len;
	} queue[TSB_NL_QUEUE_SIZE];	// Outgoing messages to Toma
	int queue_head;			// Next position to dequeue from
	int queue_tail;			// Next position to enqueue to
	int queue_count;		// Number of messages in queue
};

/*****************************************************************************/
struct TSB_operating_system_impl {				// Sandbox for all services Toma needs from the operating system
	struct TSB_all_fds_tbl fs;					// File system (files/sockets) descriptors
	struct TSB_signals_queue {					// Signaling/Logging mechanism to toma
		struct TSB_fd_otherside o;
		int cur_sig;							// Current signal to send, 0 if nothing to send. We schedule 1 signal at a time so use a ring buffer of size == 1.
		int n_sigs_sent;						// statistics, amount of signals sent
		int fd;
	} TSB_signal;
	struct TSB_syslog_impl {					// Syslog
		struct TSB_fd_otherside o;
		int fd;									// fd assigned to syslog
	} TSB_syslog;
	struct TSB_wakeup_pipe_impl {				// Operating system pipe, for communication between toma threads
		struct TSB_fd_otherside o[2];			// 1 read, 1 write file descriptor
		pthread_mutex_t mutex;					// Naturally acceesed from multiple threads
		#define TSB_WU_PIPE_QUEUE_SIZE 32		// Simple fixed-size circular buffer queue of up to 32 wakeup messages
		#define TSB_WU_PIPE_MSG_SIZE 16			// Toma write wakeup messages of exactly 16[b]
		struct {
			char data[TSB_WU_PIPE_MSG_SIZE];
		} queue[TSB_WU_PIPE_QUEUE_SIZE];		// Wakeup message from toma other threads to toma main thread
		int q_head, q_tail, q_count;			// Next position to dequeue from, Next position to enqueue to, Number of messages in queue
	} TSB_wake_pip;
	struct TSB_globa_epoll_impl {				// Implementation of epoll mechanism
		struct TSB_fd_otherside o;
		struct epoll_event evs[16];
		uint64_t n_calls_to_wait;
		int n_fds;
	} TSB_epoll;
	struct TSB_netlink_mock TSB_netlink;
	struct TSB_server_comm_wakeup_mock {
		struct TSB_fd_otherside o[2];
		long n_wakeup_msgs __attribute__((aligned(sizeof(long))));
	} TSB_km_sock_pair;
};												// Emulates operating system.
void os_sim_init(   struct TSB_operating_system_impl *os);
void os_sim_destroy(struct TSB_operating_system_impl *os, bool do_verify_used);

void os_sim_send_signal_to_toma(int sig_number);