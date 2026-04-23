/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#pragma once
/* Emulation of operating system (system calls & data structures), Used by Toma */

#include <pthread.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>

#undef TOMA_ROOT_DIR
#define TOMA_ROOT_DIR "_root/"	// Toma is running in sandbox (current directory in file system)

/*********************************** system Calls *****************************/
#define _SYS_SOCKET_H 		//	#include <sys/socket.h>
enum { PF_UNIX = 1, AF_UNIX = 1, AF_INET	= 2, PF_NETLINK = 16, AF_NETLINK = 16, MSG_PEEK = 0x02, MSG_DONTWAIT = 0x40, MSG_MORE = 0x8000,
		SOL_SOCKET = 1, SO_REUSEADDR = 2,
};
enum __socket_type { SOCK_STREAM = 1, SOCK_DGRAM = 2, SOCK_RAW = 3, SOCK_PACKET = 10, SOCK_CLOEXEC = 02000000, SOCK_NONBLOCK = 00004000, };
struct sockaddr_un;
struct sockaddr {};

int socket(    int __domain, int __type, int __protocol);		// Does not return fd but index pointer to fd storage
int socketpair(int __domain, int __type, int __protocol, int __fds[2]);
int __connect(int fd, const struct sockaddr_un * addr, unsigned int len);
int __bind(   int fd, const void*              __addr, unsigned int len);
#define connect(fd, addr, len) ({ fd = __connect(fd, addr, len); 0; })
#define bind(   fd, addr, len) ({ fd = __bind(   fd, addr, len); 0; })	// Todo: Solve this ugliness
int setsockopt(int __fd, int __level, int __optname, const void *__optval, unsigned int __optlen);
struct msghdr {
	void	*	msg_name;	/* Socket name			*/
	int		msg_namelen;	/* Length of name		*/
	void *	msg_iov;	/* Data blocks			*/
	unsigned int	msg_iovlen;	/* Number of blocks		*/
	void 	*	msg_control;	/* Per protocol magic (eg BSD file descriptor passing) */
	unsigned int	msg_controllen;	/* Length of cmsg list */
	unsigned int	msg_flags;
};
ssize_t sendmsg(int __fd, const struct msghdr *__message, int __flags);
ssize_t recvmsg(int __fd,       struct msghdr *__message, int __flags);
ssize_t send(   int __fd, const void *__buf, size_t __n , int __flags);
ssize_t recv(   int __fd,       void *__buf, size_t __n , int __flags);
int     listen( int __fd, int __n);
int     accept( int __fd, struct sockaddr* __addr, unsigned int *__addr_len);

#define	_ARPA_INET_H 1		//#include <arpa/inet.h>

#define _NETINET_IN_H		// #include <netinet/in.h>
#define _LINUX_IF_H			// #include <linux/if.h>

#if (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
	#define ntohl(x)	__builtin_bswap32 (x)
	#define ntohs(x)	__builtin_bswap16 (x)
	#define htonl(x)	__builtin_bswap32 (x)
	#define htons(x)	__builtin_bswap16 (x)
#else
	#define ntohl(x)	__uint32_identity (x)
	#define ntohs(x)	__uint16_identity (x)
	#define htonl(x)	__uint32_identity (x)
	#define htons(x)	__uint16_identity (x)
#endif

int override_open(const char *path, int flags, ... /*int mode*/);
int override_close(int fd);
int override_dup(int fd);
int override_pipe(int fds[2]);
int override_fcntl(int fd, int cmd, ...);
int override_fstat(int fd, struct stat *buf);
ssize_t override_read(  int fd,       void *buf, size_t nbytes);
ssize_t override_write( int fd, const void *buf, size_t count);
ssize_t override_pread( int fd,       void *buf, size_t count, off_t offset);
ssize_t override_pwrite(int fd, const void *buf, size_t count, off_t offset);
int override_select (int __nfds, fd_set *__restrict __readfds, fd_set *__restrict __writefds, fd_set *__restrict __exceptfds, struct timeval *__restrict __timeout);
void *override_mmap(  void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int   override_munmap(void *addr, size_t length);

#ifndef TOMA_SANDBOX_BYPASS_REDIRECTS
#define open    override_open
#define close   override_close
#define dup     override_dup
#define pipe    override_pipe
#define fcntl   override_fcntl
#define read    override_read
#define write   override_write
#define pread   override_pread
#define pwrite  override_pwrite
#define select  override_select
#define mmap    override_mmap
#define munmap  override_munmap
#define fstat  override_fstat
#endif // TOMA_SANDBOX_BYPASS_REDIRECTS

/************************************* netlink *************************************/
#define __LINUX_NETLINK_H	// #include <linux/netlink.h>
struct sockaddr_nl {
	unsigned short	nl_family;	/* AF_NETLINK	*/
	unsigned short	nl_pad;		/* zero		*/
	uint32_t		nl_pid;				/* port ID	*/
	uint32_t		nl_groups;			/* multicast groups mask */
};
struct nlmsghdr {					// 16[bytes] Copied netlink header from linux include
	uint32_t		nlmsg_len;
	uint16_t		nlmsg_type;
	uint16_t		nlmsg_flags;
	uint32_t		nlmsg_seq;		// Unused
	uint32_t		nlmsg_pid;
};
#define NLMSG_ALIGN(len) (((len)+3U) & ~3U)
#define NLMSG_HDRLEN	 ((int) NLMSG_ALIGN(sizeof(struct nlmsghdr)))
#define NLMSG_LENGTH(len) ((len) + NLMSG_HDRLEN)
#define NLMSG_SPACE(len) NLMSG_ALIGN(NLMSG_LENGTH(len))
#define NLMSG_DATA(nlh)  ((void *)(((char *)nlh) + NLMSG_HDRLEN))
#define NLMSG_NEXT(nlh, len) ((len) -= NLMSG_ALIGN((nlh)->nlmsg_len), (struct nlmsghdr *)(((char *)(nlh)) + NLMSG_ALIGN((nlh)->nlmsg_len)))
#define NLMSG_OK(  nlh, len) ((len) >= (int)sizeof(struct nlmsghdr) && (nlh)->nlmsg_len >= sizeof(struct nlmsghdr) && (nlh)->nlmsg_len <= (len))
#define NLMSG_DONE 0x3
#define NLMSG_MIN_TYPE		0x10

/************************************* syslog *************************************/
#define _SYS_SYSLOG_H 1
#include <syslog.h>
enum sys_log_priorities { LOG_DEBUG = 7, LOG_INFO = 6, LOG_NOTICE = 5, LOG_WARNING = 4, LOG_ERR = 3, LOG_CRIT = 2, LOG_ALERT = 1, LOG_EMERG = 0 };
void syslog(int priority, const char *format, ...);
void closelog(void);
#define	_PATH_LOG TOMA_ROOT_DIR "sys_log"

#define	_SYS_IOCTL_H 1
#include <sys/ioctl.h>
int ioctl(int fd, unsigned long int __request, ...);
#define FIONBIO		0x5421
#undef is_block_device_stat
#define is_block_device_stat(s) true

/************************************* signal *************************************/
//#include "./interfaces/os/nvmeibt_os_signal.h"
#define NVMEIBT_OS_SIGNAL
int init_signal_handling(const char *exe_name);
void handle_sig_fd(int signals_fd, void (*sig_handler)(int32_t n, uint64_t addr));

int nvmeibt_nonblock_fd(int fd);

/************************************* Epoll *************************************/
#define	_SYS_EPOLL_H	1		// #include <sys/epoll.h>
enum EPOLL_EVENTS {
	EPOLLIN = 0x001, EPOLLPRI = 0x002, EPOLLOUT = 0x004, EPOLLERR = 0x008, EPOLLHUP = 0x010,
	EPOLL_CLOEXEC = 02000000,
};
enum EPOLL_CTL { EPOLL_CTL_ADD = 1, EPOLL_CTL_DEL = 2, EPOLL_CTL_MOD = 3 };
typedef struct epoll_data {
	void *ptr;
} epoll_data_t;

struct epoll_event {
	epoll_data_t data;
	uint32_t events;
	int __fd;		// Private usage of the sandbox
};
int epoll_create1(int __flags);
int epoll_ctl( int efd, enum EPOLL_CTL __op, int __fd, struct epoll_event *ev_ptr);
int epoll_wait(int efd,                                struct epoll_event *ev_arr, int arr_size, int time_out_ns);

/********************* lib-udev: replaces: #include <libudev.h> ***********************/
#define NVMEIBT_TOMA_LIB_UDEV_API_H // #include "interfaces/nvme/nvmeibt_lib_udev_api.h"
struct udev;
struct udev* udev_new(void);
void udev_unref(struct udev* u);

struct udev_enumerate;
struct udev_enumerate* udev_enumerate_new(struct udev* u);
void udev_enumerate_add_match_subsystem(struct udev_enumerate *e, const char* sub);
void udev_enumerate_add_match_property( struct udev_enumerate *e, const char* key, const char* val);
void udev_enumerate_scan_devices(       struct udev_enumerate *e);
void udev_enumerate_unref(              struct udev_enumerate* e);

struct udev_list_entry;
struct udev_list_entry* udev_enumerate_get_list_entry(struct udev_enumerate *e);
#define udev_list_entry_foreach(list_entry, first_entry) for (list_entry = first_entry; list_entry != NULL; list_entry = udev_list_entry_get_next(list_entry))
const char*             udev_list_entry_get_name( const struct udev_list_entry *);
const char*             udev_list_entry_get_value(const struct udev_list_entry *);
struct udev_list_entry *udev_list_entry_get_next(       struct udev_list_entry *);

struct udev_device;
struct udev_device* udev_device_new_from_syspath(struct udev *u, const char *path);
const char* udev_device_get_devpath(       const struct udev_device*);	// The devpath is the path under /sys to the device. E.g. `/devices/ACPI0004:00/0/host0/block/sda`
const char* udev_device_get_syspath(       const struct udev_device*);
const char* udev_device_get_devtype(       const struct udev_device*);
const char* udev_device_get_devnode(       const struct udev_device*);	// The devnode is the name of the device (full path to the /dev node). E.g. `/dev/sda `
const char* udev_device_get_action(        const struct udev_device*);
const char* udev_device_get_subsystem(     const struct udev_device*);
const char* udev_device_get_property_value(const struct udev_device*, const char *property);
void udev_device_unref(                          struct udev_device*);

struct udev_monitor;
struct udev_monitor *udev_monitor_new_from_netlink( struct udev *u, const char *name);
struct udev_monitor *udev_monitor_unref(            struct udev_monitor *);
int                  udev_monitor_get_fd(     const struct udev_monitor *);
int udev_monitor_filter_add_match_subsystem_devtype(struct udev_monitor *, const char *subsystem, const char *devtype);
int udev_monitor_enable_receiving(                  struct udev_monitor *);
struct udev_device *udev_monitor_receive_device(    struct udev_monitor *);