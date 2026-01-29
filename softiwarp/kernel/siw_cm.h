/*
 * Software iWARP device driver for Linux
 *
 * Authors: Bernard Metzler <bmt@zurich.ibm.com>
 *
 * Copyright (c) 2008-2016, IBM Corporation
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *   Redistribution and use in source and binary forms, with or
 *   without modification, are permitted provided that the following
 *   conditions are met:
 *
 *   - Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 *   - Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 *   - Neither the name of IBM nor the names of its contributors may be
 *     used to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef _SIW_CM_H
#define _SIW_CM_H

#include <net/tcp.h>
#include <net/sock.h>
#include <linux/tcp.h>

#include <rdma/iw_cm.h>

enum siw_cep_state {
	SIW_EPSTATE_IDLE = 1,
	SIW_EPSTATE_LISTENING,
	SIW_EPSTATE_CONNECTING,
	SIW_EPSTATE_AWAIT_MPAREQ,
	SIW_EPSTATE_RECVD_MPAREQ,
	SIW_EPSTATE_AWAIT_MPAREP,
	SIW_EPSTATE_RDMA_MODE,
	SIW_EPSTATE_CLOSED,
	SIW_EPSTATE_REJECTING,
	SIW_EPSTATE_REJECTED
};

struct siw_mpa_info {
	struct mpa_rr	hdr;	/* peer mpa hdr in host byte order */
	char		*pdata;
	int		bytes_rcvd;
};

struct siw_llp_info {
	struct socket		*sock;
	struct siw_sk_upcalls	sk_def_upcalls;
	struct sockaddr_storage	listen_addr;
};

struct siw_dev;

struct siw_cep {
	struct iw_cm_id		*cm_id;
	struct siw_dev		*sdev;

	struct list_head	devq;
	/*
	 * The provider_data element of a listener IWCM ID
	 * refers to a list of one or more listener CEPs
	 */
	struct list_head	listenq;
	struct siw_cep		*listen_cep;
	struct siw_qp		*qp;
	spinlock_t		lock;
	wait_queue_head_t	waitq;
	struct kref		ref;
	enum siw_cep_state	state;
	short			in_use;
	int				in_use_pid;
	struct siw_cm_work	*connect_timer;
	struct siw_cm_work	*mpa_timer;
	struct siw_cm_work 	*read_mpa_hdr_work;
	struct siw_cm_work 	*peer_close_work;
	struct list_head	work_freelist;
	struct list_head	work_outstanding;
	struct siw_llp_info	llp;
	struct siw_mpa_info	mpa;
	int			ord;
	int			ird;
	int			sk_error; /* not (yet) used XXX */

	/* Saved upcalls of socket llp.sock */
	void    (*sk_state_change)(struct sock *sk);
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 15, 0)
	void    (*sk_data_ready)(struct sock *sk, int bytes);
#else
	void    (*sk_data_ready)(struct sock *sk);
#endif
	void    (*sk_write_space)(struct sock *sk);
	void    (*sk_error_report)(struct sock *sk);

	/* Saved congestion-algorithm of socket llp.sock */
	char orig_ca_name[TCP_CA_NAME_MAX];

	const void *reject_pdata;
	u8 reject_plen;

	char *log_ring_buf;
	char *log_printf_buf;
	int ring_buf_head;
	int ring_buf_tail;
	
	struct completion *all_work_done;
};

#define MPAREQ_TIMEOUT	(HZ*10)
#define MPAREP_TIMEOUT	(HZ*5)
#define CONNECT_TIMEOUT (HZ*5)
#define ALL_WORK_DONE_TIMEOUT (HZ * 30)

enum siw_work_type {
	SIW_CM_WORK_ACCEPT	= 1,
	SIW_CM_WORK_READ_MPAHDR,
	SIW_CM_WORK_CLOSE_LLP,		/* close socket */
	SIW_CM_WORK_PEER_CLOSE,		/* socket indicated peer close */
	SIW_CM_WORK_MPATIMEOUT,
	SIW_CM_WORK_CONNECT_TIMEOUT,
	SIW_CM_WORK_CONNECT,
	SIW_CM_WORK_CONNECT_CANCEL,
	SIW_CM_WORK_REJECT_CONNECTION,
};

struct siw_cm_work {
	struct delayed_work	work;
	struct list_head	list;
	enum siw_work_type	type;
	struct siw_cep	*cep;
	u64 work_idx;
	unsigned long q_jif;
	struct kref ref;
};

/*
 * With kernel 3.12, OFA ddressing changed from sockaddr_in to
 * sockaddr_storage
 */
#define to_sockaddr_in(a) (*(struct sockaddr_in *)(&(a)))
#define to_sockaddr_in6(a) (*(struct sockaddr_in6 *)(&(a)))

static inline int getname_peer(struct socket *s, struct sockaddr_storage *a)
{
#if KS_SOCK_OPT_GETNAME_HAS_UADDR_LEN
	int a_len = sizeof(a);
	return s->ops->getname(s, (struct sockaddr *)a, &a_len, 1);
#else
	return s->ops->getname(s, (struct sockaddr *)a, 1);
#endif
}

static inline int getname_local(struct socket *s, struct sockaddr_storage *a)
{
#if KS_SOCK_OPT_GETNAME_HAS_UADDR_LEN
	int a_len = sizeof(a);
	return s->ops->getname(s, (struct sockaddr *)a, &a_len, 0);
#else
	return s->ops->getname(s, (struct sockaddr *)a, 0);
#endif
}

extern int siw_connect(struct iw_cm_id *, struct iw_cm_conn_param *);
extern int siw_accept(struct iw_cm_id *, struct iw_cm_conn_param *);
extern int siw_reject(struct iw_cm_id *, const void *, u8);
extern int siw_create_listen(struct iw_cm_id *, int);
extern int siw_destroy_listen(struct iw_cm_id *);

extern int siw_cm_queue_work(struct siw_cep *, enum siw_work_type);

extern int siw_cm_init(void);
extern void siw_cm_exit(void);

extern void siw_cm_any_listeners_update(struct net_device *, int, __be32, const struct in6_addr *, bool);
/*
 * TCP socket interface
 */
#define sk_to_qp(sk)	(((struct siw_cep *)((sk)->sk_user_data))->qp)
#define sk_to_cep(sk)	((struct siw_cep *)((sk)->sk_user_data))

/*
 * Should we use tcp_current_mss()?
 * But its not exported by kernel.
 */
static inline unsigned int get_tcp_mss(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	u32 mss_cache_align = ALIGN_DOWN(tp->mss_cache, 8); /* Align mss to 8 bytes */

#if !KS_TCP_SOCK_HAS_XMIT_SIZE_GOAL_SEGS
	if (tp->gso_segs)
		return tp->gso_segs * mss_cache_align;
#else
	if (tp->xmit_size_goal_segs)
		return tp->xmit_size_goal_segs * mss_cache_align;
#endif
	else
		return mss_cache_align;
}

void siw_cep_dealloc(struct kref *ref);
void siw_cep_dealloc_already_locked(struct kref *ref);

void siw_cep_log_print(struct siw_cep *cep, const void *fmt, ...);

#define dprint_cep(dbgcat, cep, fmt, args...) \
({\
	dprint(DBG_CM | dbgcat, "(CEP:" dprint_ptr_str() "): " fmt, cep, ##args); \
	if (cep && cep->log_ring_buf)\
		siw_cep_log_print(cep, "%ld: (%5d/%1d)[%s] %s:%d - " fmt, jiffies, \
		current->pid, siw_current_cpu, current->comm, \
		 __func__, __LINE__,  ##args);\
})

#define siw_cep_get(_cep) \
do { \
	kref_get(&(_cep)->ref); \
	dprint_cep(DBG_OBJ|DBG_CM, _cep, "siw_cep_get New refcount: %d\n", kref_read(&((_cep)->ref))); \
} while (0)

#define __siw_cep_put(_cep) \
do { \
	int _refcount = kref_read(&((_cep)->ref)); \
	dprint_cep(DBG_OBJ|DBG_CM, _cep, "siw_cep_put New refcount: %d\n", _refcount - 1); \
	BUG_ON(_refcount < 1); \
} while (0)

#define siw_cep_put_locked(_cep) \
do { \
	__siw_cep_put(_cep);\
	kref_put(&((_cep)->ref), siw_cep_dealloc_already_locked); \
} while (0)

#define siw_cep_put(_cep) \
do { \
	__siw_cep_put(_cep);\
	kref_put(&((_cep)->ref), siw_cep_dealloc); \
} while (0)

void siw_cep_printk_log(struct siw_cep *cep, const char *prefix);
void siw_cep_force_free(struct siw_cep *cep);

#endif
