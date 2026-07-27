/*
 * Software iWARP device driver for Linux
 *
 * Authors: Bernard Metzler <bmt@zurich.ibm.com>
 *          Fredy Neeser <nfd@zurich.ibm.com>
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

#include <linux/errno.h>
#include <linux/types.h>
#include <linux/net.h>
#include <linux/inetdevice.h>
#include <linux/workqueue.h>
#include <net/tcp.h>
#include <net/sock.h>
#include <net/addrconf.h>
#include <linux/tcp.h>


#include <rdma/iw_cm.h>
#include <rdma/ib_verbs.h>
#include <rdma/ib_smi.h>
#include <rdma/ib_user_verbs.h>

#include "siw.h"
#include "siw_cm.h"
#include "siw_obj.h"
#include "siw_str.h"
#include <linux/list.h>

static bool mpa_crc_strict = 1;
module_param(mpa_crc_strict, bool, 0644);
static bool mpa_crc_required;
module_param(mpa_crc_required, bool, 0644);
static bool tcp_nodelay = 1;
module_param(tcp_nodelay, bool, 0644);
static bool tcp_quickack = 1;
module_param(tcp_quickack, bool, 0644);
static unsigned sock_buff_sz = 65536;
module_param(sock_buff_sz, int, 0644);
static unsigned comp_vector_cpu0 = 0;
module_param(comp_vector_cpu0, int, 0644);

MODULE_PARM_DESC(mpa_crc_required, "MPA CRC required");
MODULE_PARM_DESC(mpa_crc_strict, "MPA CRC off enforced");
MODULE_PARM_DESC(tcp_nodelay, "Set TCP NODELAY");
MODULE_PARM_DESC(tcp_quickack, "Set TCP QUICKACK");
MODULE_PARM_DESC(sock_buff_sz, "Socket Buffers Size");

bool connect_non_block = 1;
module_param(connect_non_block, bool, 0644);
MODULE_PARM_DESC(connect_non_block, "Connect non-blocking");

bool use_so_incoming_cpu = 1;
module_param(use_so_incoming_cpu, bool, 0644);
MODULE_PARM_DESC(use_so_incoming_cpu, "Set the RX CPU of socket to RCQ's comp-vector index (after connect/accept)");

atomic64_t siw_cm_wq_work_idx = ATOMIC_INIT(0);

#define SIW_CM_NON_LISTENING_CEP_WORKS 5

/* cm_id provider data for SIW listeners (common) */
struct siw_listener_cm_id_provider_data_common
{
	/* List of CEPs that contain the listening socket(s) and state */
	struct list_head	cep_list_head;
	bool listen_any_addr;
	int bound_dev_if;
	int addr_family;
};

/* Extension of above for dynamically updating listeners on INADDR_ANY */
struct siw_listener_cm_id_provider_data_any_addr {
	struct siw_listener_cm_id_provider_data_common common;
	/* Entry in Global listen_any_list */
	struct list_head 	listen_any_entry;
	struct net_device	*netdev;
	struct iw_cm_id		*cm_id;
	__be16			port;
	int 			backlog;
};

/* Global list of all INADDR_ANY listeners for dynamically updating on address change */
static LIST_HEAD(listen_any_list);
static DEFINE_MUTEX(listen_any_guard);

static void siw_get_work_ref(struct siw_cm_work *work);
static void siw_put_work(struct siw_cm_work *work, bool already_locked);

#ifndef for_ifa
#define for_ifa(in_dev)	{ struct in_ifaddr *ifa; \
  for (ifa = (in_dev)->ifa_list; ifa; ifa = ifa->ifa_next)
#define endfor_ifa(in_dev) }
#endif
	  
#if KS_HAS_SOCK_SETSOCKOPT && KS_SOCK_SETSOCKOPT_TAKES_SOCKPTR_T
#define sock_setsockopt_val_t sockptr_t
#define SOCK_SETSOCKOPT_VAL_PTR KERNEL_SOCKPTR
#else
#define sock_setsockopt_val_t char __user *
static inline char __user *SOCK_SETSOCKOPT_VAL_PTR(void *ptr) {
	return (char __user *)ptr;
}
#endif

#if KS_HAS_TCP_SETSOCKOPT && KS_TCP_SETSOCKOPT_TAKES_SOCKPTR_T
#define tcp_setsockopt_val_t sockptr_t
#define TCP_SETSOCKOPT_VAL_PTR KERNEL_SOCKPTR
#else
#define tcp_setsockopt_val_t char __user *
static inline char __user *TCP_SETSOCKOPT_VAL_PTR(void *ptr) {
	return (char __user *)ptr;
}
#endif

#define tcp_getsockopt_len_t int __user *
static inline int __user *TCP_GETSOCKOPT_LEN_PTR(int *ptr) {
	return (int __user *)ptr;
}
#define tcp_getsockopt_val_t char __user *
static inline char __user *TCP_GETSOCKOPT_VAL_PTR(void *ptr) {
	return (char __user *)ptr;
}

static inline const char * siw_cep_state_str(enum siw_cep_state state)
{
	switch (state) {
		case SIW_EPSTATE_IDLE				: return "IDLE";
		case SIW_EPSTATE_LISTENING			: return "LISTENING";
		case SIW_EPSTATE_CONNECTING			: return "CONNECTING";
		case SIW_EPSTATE_AWAIT_MPAREQ		: return "AWAIT_MPAREQ";
		case SIW_EPSTATE_RECVD_MPAREQ		: return "RECVD_MPAREQ";
		case SIW_EPSTATE_AWAIT_MPAREP		: return "AWAIT_MPAREP";
		case SIW_EPSTATE_RDMA_MODE			: return "RDMA_MODE";
		case SIW_EPSTATE_CLOSED				: return "CLOSED";
		case SIW_EPSTATE_REJECTING			: return "REJECTING";
		case SIW_EPSTATE_REJECTED			: return "REJECTED";
		default								: return "???";
	}	
}

static inline const char * siw_cm_work_type_str(enum siw_work_type work)
{
	switch (work) {
		case SIW_CM_WORK_ACCEPT				: return "ACCEPT";
		case SIW_CM_WORK_READ_MPAHDR		: return "READ_MPAHDR";
		case SIW_CM_WORK_CLOSE_LLP			: return "CLOSE_LLP";
		case SIW_CM_WORK_PEER_CLOSE			: return "PEER_CLOSE";
		case SIW_CM_WORK_MPATIMEOUT			: return "MPATIMEOUT";
		case SIW_CM_WORK_CONNECT_TIMEOUT	: return "CONNECT_TIMEOUT";
		case SIW_CM_WORK_CONNECT			: return "CONNECT";
		case SIW_CM_WORK_CONNECT_CANCEL		: return "CONNECT_CANCEL";
		case SIW_CM_WORK_REJECT_CONNECTION	: return "REJECT_CONNECTION";
		default								: return "???";
	}
}

static inline const char * iw_cm_event_type_str(enum iw_cm_event_type event)
{
	switch (event) {
		case IW_CM_EVENT_CONNECT_REQUEST	: return "CONNECT_REQUEST";	/* connect request received */          
		case IW_CM_EVENT_CONNECT_REPLY 		: return "CONNECT_REPLY";	/* reply from active connect request */ 
		case IW_CM_EVENT_ESTABLISHED	   	: return "ESTABLISHED";     /* passive side accept successful */    
		case IW_CM_EVENT_DISCONNECT	    	: return "DISCONNECT";      /* orderly shutdown */                  
		case IW_CM_EVENT_CLOSE          	: return "CLOSE";      		/* close complete */                    
		default								: return "???";
	}
}

static void siw_cep_state_change(struct siw_cep *cep, enum siw_cep_state new_state)
{
	dprint(DBG_CM, "(): cep=" dprint_ptr_str() ", state: %s(%d) -> %s(%d) (in_use/pid=%d/%d)\n",
		   cep, siw_cep_state_str(cep->state), cep->state,
		   siw_cep_state_str(new_state), new_state, cep->in_use, cep->in_use_pid);

	cep->state = new_state;
}

/*
 * siw_sock_nodelay() - Disable Nagle algorithm
 */
static int siw_sock_nodelay(struct socket *sock, char *orig_ca, int orig_ca_len)
{
	int rv, val = 1;
#if KS_HAS_SET_FS && defined(KERNEL_DS)
	mm_segment_t oldfs;
#endif

#if !KS_HAS_TCP_SOCK_SET_NODELAY || !KS_HAS_TCP_SOCK_SET_QUICKACK
	tcp_setsockopt_val_t optval = TCP_SETSOCKOPT_VAL_PTR(&val);
#endif
	val = tcp_nodelay ? 1 : 0;

#if KS_HAS_SET_FS && defined(KERNEL_DS)
	oldfs = get_fs();
	set_fs(KERNEL_DS);
#endif

#if !KS_HAS_TCP_SOCK_SET_NODELAY
#	if !KS_HAS_TCP_SETSOCKOPT
	rv = sock->ops->setsockopt(sock, SOL_TCP, TCP_NODELAY,
				   optval, sizeof(val));
#	else
	rv = tcp_setsockopt(sock->sk, SOL_TCP, TCP_NODELAY,
			     optval, sizeof(val));
#	endif
#else
	if (tcp_nodelay)
		tcp_sock_set_nodelay(sock->sk);
	rv = 0;
#endif
	if (!rv) {
		val = tcp_quickack ? 1 : 0;
#if !KS_HAS_TCP_SOCK_SET_QUICKACK
#	if !KS_HAS_TCP_SETSOCKOPT
		rv = sock->ops->setsockopt(sock, SOL_TCP, TCP_QUICKACK,
					   optval, sizeof(val));
#	else
		rv = tcp_setsockopt(sock->sk, SOL_TCP, TCP_QUICKACK,
				     optval, sizeof(val));
#	endif
#else
		tcp_sock_set_quickack(sock->sk, val);
		rv = 0;
#endif
	}
	
#ifdef SIW_TX_COMP_WAIT_ACK
	if (!rv) {
		char siw_tcp_cong_ctrl_name[] = SIW_TCP_CONG_CTRL_NAME;
		tcp_getsockopt_len_t get_cong_optlen = TCP_GETSOCKOPT_LEN_PTR(&orig_ca_len);
		tcp_getsockopt_val_t get_cong_optval = TCP_GETSOCKOPT_VAL_PTR(orig_ca);
		tcp_setsockopt_val_t set_cong_optval = TCP_SETSOCKOPT_VAL_PTR(siw_tcp_cong_ctrl_name);

#	if !KS_HAS_TCP_SETSOCKOPT
		rv = sock->ops->getsockopt(sock, SOL_TCP, TCP_CONGESTION,
					   get_cong_optval, get_cong_optlen);
#	else
		rv = tcp_getsockopt(sock->sk, SOL_TCP, TCP_CONGESTION,
				    get_cong_optval, get_cong_optlen);
#	endif
		if (rv < 0) {
			/* [NVMESH-5470]: On newer kernels, it is no longer possible to call tcp_getsockopt on kernel addresses.
			 * It is not a must to restore the previous congestion algorithm on shutdown, as long as a non-SIW one can be set
			 * to release the SIW refcount. 'reno' seems to be always available so that can be set instead. */
			dprint(DBG_CM, "(sock=" dprint_ptr_str() "): Failed (%d) to get TCP_CONGESTION sockopt. Assuming 'reno' is available\n",
			       sock, rv);
			strlcpy(orig_ca, "reno", orig_ca_len);
		}

#	if !KS_HAS_TCP_SETSOCKOPT
		rv = sock->ops->setsockopt(sock, SOL_TCP, TCP_CONGESTION,
					set_cong_optval, sizeof(siw_tcp_cong_ctrl_name));
#	else
		rv = tcp_setsockopt(sock->sk, SOL_TCP, TCP_CONGESTION,
				set_cong_optval, sizeof(siw_tcp_cong_ctrl_name));
#	endif
		if (rv < 0) {
			dprint(DBG_CM, "(sock=" dprint_ptr_str() "): Failed (%d) to set TCP_CONGESTION sockopt to %s\n", 
			       sock, rv, siw_tcp_cong_ctrl_name);
			goto out;
		}

		dprint(DBG_CM, "(sock=" dprint_ptr_str() "): Set TCP_CONGESTION sockopt to %s (originally %s)\n",
			sock, siw_tcp_cong_ctrl_name, orig_ca);
	}
#endif

out:

#if KS_HAS_SET_FS && defined(KERNEL_DS)
	set_fs(oldfs);
#endif
	return rv;
}

/*
 * siw_sock_nodelay() - Disable Nagle algorithm
 */
static int siw_cep_socket_restore_ca(struct siw_cep *cep)
{
	struct socket *sock = cep->llp.sock;
	int rv;
	tcp_setsockopt_val_t set_cong_optval = TCP_SETSOCKOPT_VAL_PTR(cep->orig_ca_name);
	size_t optval_len = sizeof(cep->orig_ca_name);

#if KS_HAS_SET_FS && defined(KERNEL_DS)
	mm_segment_t oldfs;
#endif

#if KS_HAS_SET_FS && defined(KERNEL_DS)
	oldfs = get_fs();
	set_fs(KERNEL_DS);
#endif
	if (!cep->orig_ca_name[0]) {
		rv = -EALREADY;
		goto out;
	}

#	if !KS_HAS_TCP_SETSOCKOPT
	rv = sock->ops->setsockopt(sock, SOL_TCP, TCP_CONGESTION,
				   set_cong_optval, optval_len);
#	else
	rv = tcp_setsockopt(sock->sk, SOL_TCP, TCP_CONGESTION,
			set_cong_optval, optval_len);
#	endif

	if (rv < 0) {
		dprint_cep(DBG_CM, cep, "Failed (%d) to restore original congestion algorithm %s\n",
			   rv, cep->orig_ca_name);
		goto out;
	}

	dprint_cep(DBG_CM, cep, "Restored original TCP_CONGESTION sockopt value - %s\n", cep->orig_ca_name);
	cep->orig_ca_name[0] = 0;

out:

#if KS_HAS_SET_FS && defined(KERNEL_DS)
	set_fs(oldfs);
#endif
	return rv;
}

static void siw_cm_llp_state_change(struct sock *);
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 15, 0)
static void siw_cm_llp_data_ready(struct sock *sk, int flags);
#else
static void siw_cm_llp_data_ready(struct sock *sk);
#endif
static void siw_cm_llp_write_space(struct sock *);
static void siw_cm_llp_error_report(struct sock *);

static void siw_sk_assign_cm_upcalls(struct sock *sk)
{
	write_lock_bh(&sk->sk_callback_lock);
	sk->sk_state_change = siw_cm_llp_state_change;
	sk->sk_data_ready   = siw_cm_llp_data_ready;
	sk->sk_write_space  = siw_cm_llp_write_space;
	sk->sk_error_report = siw_cm_llp_error_report;
	write_unlock_bh(&sk->sk_callback_lock);
}

static void siw_sk_save_upcalls(struct sock *sk)
{
	struct siw_cep *cep = sk_to_cep(sk);
	BUG_ON(!cep);

	write_lock_bh(&sk->sk_callback_lock);
	cep->sk_state_change = sk->sk_state_change;
	cep->sk_data_ready   = sk->sk_data_ready;
	cep->sk_write_space  = sk->sk_write_space;
	cep->sk_error_report = sk->sk_error_report;
	write_unlock_bh(&sk->sk_callback_lock);
}

static void siw_sk_restore_upcalls(struct sock *sk, struct siw_cep *cep)
{
	sk->sk_state_change	= cep->sk_state_change;
	sk->sk_data_ready	= cep->sk_data_ready;
	sk->sk_write_space	= cep->sk_write_space;
	sk->sk_error_report	= cep->sk_error_report;
	sk->sk_user_data	= NULL;
}

static void siw_socket_disassoc(struct socket *s)
{
	struct sock	*sk = s->sk;
	struct siw_cep	*cep;

	if (sk) {
		write_lock_bh(&sk->sk_callback_lock);
		cep = sk_to_cep(sk);
		if (cep) {
			siw_sk_restore_upcalls(sk, cep);
			siw_cep_put(cep);
		} else
			pr_warn("cannot restore sk callbacks: no ep\n");
		write_unlock_bh(&sk->sk_callback_lock);
	} else
		pr_warn("cannot restore sk callbacks: no sk\n");
}

static void siw_cep_socket_assoc(struct siw_cep *cep, struct socket *s)
{
	cep->llp.sock = s;
	siw_cep_get(cep);
	s->sk->sk_user_data = cep;

	siw_sk_save_upcalls(s->sk);
	siw_sk_assign_cm_upcalls(s->sk);
}

void siw_cep_log_print(struct siw_cep *cep, const void *fmt, ...)
{
	int len, i;

	va_list valist;

	va_start(valist, fmt);
	/* Get the size, by passing NULL, 0 to vsprintf */
	len = vsnprintf(cep->log_printf_buf, SIW_CEP_LOG_PRINTF_BUF_SIZE, fmt, valist);
	va_end(valist);

	if (len <= 0) {
		return;
	} else if (len >= SIW_CEP_LOG_PRINTF_BUF_SIZE) {
		/* vsnprintf returns the size that would have been printed */
		len = SIW_CEP_LOG_PRINTF_BUF_SIZE - 1;
	}

	/* Copy to ring buffer (adding at the tail) */
	for (i = 0; i < len; i++) {
		cep->log_ring_buf[cep->ring_buf_tail] = cep->log_printf_buf[i];
		cep->ring_buf_tail = (cep->ring_buf_tail + 1) % SIW_CEP_LOG_RING_BUF_SIZE;
		/* Update the head if necessary so we know where to start printing from */
		if (cep->ring_buf_tail == cep->ring_buf_head) {
			cep->ring_buf_head = (cep->ring_buf_head + 1) % SIW_CEP_LOG_RING_BUF_SIZE;
		}
	}
}

static void siw_cep_init(struct siw_dev  *sdev, struct siw_cep *cep)
{
	unsigned long flags;

	INIT_LIST_HEAD(&cep->listenq);
	INIT_LIST_HEAD(&cep->devq);
	INIT_LIST_HEAD(&cep->work_freelist);
	INIT_LIST_HEAD(&cep->work_outstanding);

	kref_init(&cep->ref);
	cep->state = SIW_EPSTATE_IDLE;
	init_waitqueue_head(&cep->waitq);
	spin_lock_init(&cep->lock);
	cep->sdev = sdev;

	spin_lock_irqsave(&sdev->idr_lock, flags);
	list_add_tail(&cep->devq, &sdev->cep_list);
	spin_unlock_irqrestore(&sdev->idr_lock, flags);
	atomic_inc(&sdev->num_cep);
	
#if SIW_CEP_LOG_RING_BUF_SIZE > 0
	if (!(cep->log_ring_buf = kzalloc(SIW_CEP_LOG_RING_BUF_SIZE, GFP_KERNEL)) ||
		!(cep->log_printf_buf = kzalloc(SIW_CEP_LOG_PRINTF_BUF_SIZE, GFP_KERNEL)))
	{
		kfree(cep->log_ring_buf);
		cep->log_ring_buf = NULL;
		kfree(cep->log_printf_buf);
		cep->log_printf_buf = NULL;
	}
#endif
}

static struct siw_cep *siw_cep_alloc(struct siw_dev *sdev)
{
	struct siw_cep *cep = kzalloc(sizeof *cep, GFP_KERNEL);	
	if (cep) {
		siw_cep_init(sdev, cep);
		dprint_cep(DBG_OBJ | DBG_CM, cep, "New Object\n");
	}
	return cep;
}

static void siw_cm_free_work(struct siw_cep *cep)
{
	struct list_head	*w, *tmp;
	struct siw_cm_work	*work;

	BUG_ON(!list_empty(&cep->work_outstanding));

	list_for_each_safe(w, tmp, &cep->work_freelist) {
		work = list_entry(w, struct siw_cm_work, list);
		list_del(&work->list);
		kfree(work);
	}
}

static void siw_cancel_mpatimer(struct siw_cep *cep)
{
	siw_cep_get(cep);
	spin_lock_bh(&cep->lock);
	if (cep->mpa_timer) {
		bool pending_and_canceld = cancel_delayed_work(&cep->mpa_timer->work);
		dprint_cep(DBG_CM , cep, "work " dprint_ptr_str() " cancel_delayed_work returned %d\n", 
		       cep->mpa_timer, pending_and_canceld);
		if (pending_and_canceld) {
			/* Puts the reference matched with siw_get_work */
			siw_put_work(cep->mpa_timer, true);
			//siw_cep_put(cep); Moved to siw_get_work / siw_put_work
		}
		/* Puts the reference matched with assigning to cep->mpa_timer */
		siw_put_work(cep->mpa_timer, true);
		cep->mpa_timer = NULL;
	}
	spin_unlock_bh(&cep->lock);
	siw_cep_put(cep);
}

static void siw_clear_mpatimer(struct siw_cep *cep, struct siw_cm_work *work)
{
	siw_cep_get(cep);
	spin_lock_bh(&cep->lock);
	if (cep->mpa_timer) {
		dprint_cep(DBG_CM , cep, "clearing mpa_timer " dprint_ptr_str() "\n", cep->mpa_timer);
		/* MPA timer cannot be scheduled more than once */
		BUG_ON(cep->mpa_timer != work);
		/* Puts the reference matched with assigning to cep->mpa_timer */
		siw_put_work(cep->mpa_timer, true);
		cep->mpa_timer = NULL;
	}
	spin_unlock_bh(&cep->lock);
	siw_cep_put(cep);
}

static void siw_cancel_connect_timer(struct siw_cep *cep)
{
	siw_cep_get(cep);
	spin_lock_bh(&cep->lock);
	if (cep->connect_timer) {
		bool pending_and_canceld = cancel_delayed_work(&cep->connect_timer->work);
		dprint_cep(DBG_CM , cep, "work " dprint_ptr_str() " cancel_delayed_work returned %d\n", 
		       cep->connect_timer, pending_and_canceld);
		if (pending_and_canceld) {
			/* Puts the reference matched with siw_get_work */
			siw_put_work(cep->connect_timer, true);
			//siw_cep_put(cep); Moved to siw_get_work / siw_put_work
		}
		/* Puts the reference matched with assigning to cep->connect_timer */
		siw_put_work(cep->connect_timer, true);
		cep->connect_timer = NULL;
	}
	spin_unlock_bh(&cep->lock);
	siw_cep_put(cep);
}

static void siw_clear_connect_timer(struct siw_cep *cep, struct siw_cm_work *work)
{
	siw_cep_get(cep);
	spin_lock_bh(&cep->lock);
	if (cep->connect_timer) {
		dprint_cep(DBG_CM , cep, "clearing connect_timer " dprint_ptr_str() "\n", cep->connect_timer);
		/* Connect timer cannot be scheduled more than once */
		BUG_ON(cep->connect_timer != work);
		/* Puts the reference matched with assigning to cep->connect_timer */
		siw_put_work(cep->connect_timer, true);
		cep->connect_timer = NULL;
	}
	spin_unlock_bh(&cep->lock);
	siw_cep_put(cep);
}

static void siw_cancel_read_mpa_hdr(struct siw_cep *cep)
{
	siw_cep_get(cep);
	spin_lock_bh(&cep->lock);
	if (cep->read_mpa_hdr_work) {
		bool pending_and_canceld = cancel_delayed_work(&cep->read_mpa_hdr_work->work);
		dprint_cep(DBG_CM , cep, "work " dprint_ptr_str() " cancel_delayed_work returned %d\n", 
		       cep->read_mpa_hdr_work, pending_and_canceld);
		if (pending_and_canceld) {
			/* Puts the reference matched with siw_get_work */
			siw_put_work(cep->read_mpa_hdr_work, true);
			//siw_cep_put(cep); Moved to siw_get_work / siw_put_work
		}
		/* Puts the reference matched with assigning to cep->read_mpa_hdr_work */
		siw_put_work(cep->read_mpa_hdr_work, true);
		cep->read_mpa_hdr_work = NULL;
	}
	spin_unlock_bh(&cep->lock);
	siw_cep_put(cep);
}

static void siw_clear_read_mpa_hdr(struct siw_cep *cep, struct siw_cm_work *work)
{
	siw_cep_get(cep);
	spin_lock_bh(&cep->lock);
	/* Read MPA Header can be scheduled more than once */
	if (cep->read_mpa_hdr_work && cep->read_mpa_hdr_work == work) {
		dprint_cep(DBG_CM , cep, "clearing read_mpa_hdr_work " dprint_ptr_str() "\n", cep->read_mpa_hdr_work);
		/* Puts the reference matched with assigning to cep->read_mpa_hdr_work */
		siw_put_work(cep->read_mpa_hdr_work, true);
		cep->read_mpa_hdr_work = NULL;
	}
	spin_unlock_bh(&cep->lock);
	siw_cep_put(cep);
}

static void siw_cancel_peer_close(struct siw_cep *cep)
{
	siw_cep_get(cep);
	spin_lock_bh(&cep->lock);
	if (cep->peer_close_work) {
		bool pending_and_canceld = cancel_delayed_work(&cep->peer_close_work->work);
		dprint_cep(DBG_CM, cep, "work " dprint_ptr_str() " cancel_delayed_work returned %d\n", 
		       cep->peer_close_work, pending_and_canceld);
		if (pending_and_canceld) {
			/* Puts the reference matched with siw_get_work */
			siw_put_work(cep->peer_close_work, true);
			//siw_cep_put(cep); Moved to siw_get_work / siw_put_work
		}
		/* Puts the reference matched with assigning to cep->peer_close_work */
		siw_put_work(cep->peer_close_work, true);
		cep->peer_close_work = NULL;
	}
	spin_unlock_bh(&cep->lock);
	siw_cep_put(cep);
}

static void siw_clear_peer_close(struct siw_cep *cep, struct siw_cm_work *work)
{
	siw_cep_get(cep);
	spin_lock_bh(&cep->lock);
	/* Not a BUG, siw_cancel_peer_close may have cleared it even
	 * though it was unable to cancel it
	 * BUG_ON(!cep->peer_close_work);
	 */
	if (cep->peer_close_work) {
		dprint_cep(DBG_CM , cep, "clearing peer_close_work " dprint_ptr_str() "\n", cep->peer_close_work);
		/* Peer close cannot be scheduled more than once */
		BUG_ON(cep->peer_close_work != work);
		/* Puts the reference matched with assigning to cep->peer_close_work */
		siw_put_work(cep->peer_close_work, true);
		cep->peer_close_work = NULL;
	}
	spin_unlock_bh(&cep->lock);
	siw_cep_put(cep);
}

static void __siw_put_work(struct kref *ref)
{
	struct siw_cm_work *work = container_of(ref, struct siw_cm_work, ref);
	struct siw_cep *cep = work->cep;
	/* Sanity Check */
	BUG_ON(delayed_work_pending(&work->work));
	/* Parent has already locked cep->lock */
	list_del(&work->list);
	list_add(&work->list, &work->cep->work_freelist);
	if (cep->all_work_done && list_empty(&cep->work_outstanding)) {
		/* Used by siw_cep_force_free */
		complete(cep->all_work_done);
		cep->all_work_done = NULL;
	}
	/* Put cep ref matched with siw_get_work */
	siw_cep_put_locked(work->cep);
}

static void siw_get_work_ref(struct siw_cm_work *work)
{
	kref_get(&work->ref);
	dprint_cep(DBG_OBJ|DBG_CM, work->cep, "work: " dprint_ptr_str() " siw_get_work_ref New refcount: %d\n", 
	       work, kref_read(&work->ref));
}

static void siw_put_work(struct siw_cm_work *work, bool already_locked)
{
	unsigned long flags;
	int refcount = kref_read(&work->ref);
	dprint_cep(DBG_OBJ|DBG_CM, work->cep, "work: " dprint_ptr_str() " siw_put_work New refcount: %d\n", 
	       work, refcount - 1);
	BUG_ON(refcount < 1);
	if (!already_locked) {
		/* Stop cep being freed before we can unlock */
		siw_cep_get(work->cep);
		spin_lock_irqsave(&work->cep->lock, flags);
	}
	kref_put(&work->ref, __siw_put_work);
	if (!already_locked) {
		spin_unlock_irqrestore(&work->cep->lock, flags);
		/* Put reference matched with above */
		siw_cep_put(work->cep);
	}
}

static void siw_cep_set_inuse(struct siw_cep *cep)
{
	unsigned long flags;
	int rv __attribute__((unused));
retry:
	dprint_cep(DBG_CM, cep, "try use %d, pid=%d\n",
		cep->in_use, cep->in_use_pid);

	spin_lock_irqsave(&cep->lock, flags);

	if (cep->in_use) {
		spin_unlock_irqrestore(&cep->lock, flags);
		rv = wait_event_interruptible(cep->waitq, !cep->in_use);
		if (signal_pending(current))
			flush_signals(current);
		goto retry;
	} else {
		cep->in_use = 1;
		cep->in_use_pid = current->pid;
		spin_unlock_irqrestore(&cep->lock, flags);
	}
}

static void siw_cep_set_free(struct siw_cep *cep)
{
	unsigned long flags;

	dprint_cep(DBG_CM, cep, "use %d, pid=%d\n",
		cep->in_use, cep->in_use_pid);

	spin_lock_irqsave(&cep->lock, flags);

#if defined(NVMESH_IS_PRODUCTION_COMPILATION) && (NVMESH_IS_PRODUCTION_COMPILATION==1)
	WARN_ON(!cep->in_use);
	WARN_ON(cep->in_use_pid != current->pid);
#else
	BUG_ON(!cep->in_use);
	BUG_ON(cep->in_use_pid != current->pid);
#endif

	cep->in_use = 0;
	cep->in_use_pid = -1;
	spin_unlock_irqrestore(&cep->lock, flags);

	wake_up(&cep->waitq);
}


static void __siw_cep_dealloc(struct siw_cep *cep, bool already_locked)
{
	struct siw_dev *sdev = cep->sdev;
	unsigned long flags;

	dprint_cep(DBG_OBJ|DBG_CM, cep, "Free Object\n");

	WARN_ON(cep->listen_cep);

	kfree(cep->log_ring_buf);
	kfree(cep->log_printf_buf);
	kfree(cep->reject_pdata);

	/* kfree(NULL) is save */
	kfree(cep->mpa.pdata);
	if (!already_locked)
		spin_lock_bh(&cep->lock);
	if (!list_empty(&cep->work_freelist))
		siw_cm_free_work(cep);
	if (!already_locked)
		spin_unlock_bh(&cep->lock);

	spin_lock_irqsave(&sdev->idr_lock, flags);
	list_del(&cep->devq);
	spin_unlock_irqrestore(&sdev->idr_lock, flags);
	atomic_dec(&sdev->num_cep);
	kfree(cep);
}

void siw_cep_dealloc_already_locked(struct kref *ref)
{
	struct siw_cep *cep = container_of(ref, struct siw_cep, ref);
	__siw_cep_dealloc(cep, true);
}

void siw_cep_dealloc(struct kref *ref)
{
	struct siw_cep *cep = container_of(ref, struct siw_cep, ref);
	__siw_cep_dealloc(cep, false);
}

static struct siw_cm_work *siw_get_work(struct siw_cep *cep)
{
	struct siw_cm_work	*work = NULL;
	bool disable_irqs = !irqs_disabled();

	if (disable_irqs)
		spin_lock_bh(&cep->lock);
	else
		spin_lock(&cep->lock);
	if (!list_empty(&cep->work_freelist)) {
		work = list_entry(cep->work_freelist.next, struct siw_cm_work,
				  list);
		list_del(&work->list);
		list_add_tail(&work->list, &cep->work_outstanding);
		kref_init(&work->ref);
		siw_cep_get(cep);
	}
	if (disable_irqs)
		spin_unlock_bh(&cep->lock);
	else
		spin_unlock(&cep->lock);
	return work;
}

static int siw_cm_alloc_work(struct siw_cep *cep, int num)
{
	struct siw_cm_work	*work;

	BUG_ON(!list_empty(&cep->work_freelist));
	BUG_ON(!list_empty(&cep->work_outstanding));

	while (num--) {
		work = kmalloc(sizeof *work, GFP_KERNEL);
		if (!work) {
			if (!(list_empty(&cep->work_freelist)))
				siw_cm_free_work(cep);
			dprint(DBG_ON, " Failed\n");
			return -ENOMEM;
		}
		work->cep = cep;
		INIT_LIST_HEAD(&work->list);
		list_add(&work->list, &cep->work_freelist);
	}
	return 0;
}

/*
 * siw_cm_upcall()
 *
 * Upcall to IWCM to inform about async connection events
 */
static int siw_cm_upcall(struct siw_cep *cep, enum iw_cm_event_type reason,
			 int status)
{
	struct iw_cm_event	event;
	struct iw_cm_id		*cm_id;

	memset(&event, 0, sizeof event);
	event.status = status;
	event.event = reason;

	if (reason == IW_CM_EVENT_CONNECT_REQUEST ||
	    reason == IW_CM_EVENT_CONNECT_REPLY) {
		u16 pd_len = be16_to_cpu(cep->mpa.hdr.params.pd_len);

		if (pd_len) {
			/*
			 * hand over MPA private data
			 */
			event.private_data_len = pd_len;
			event.private_data = cep->mpa.pdata;
		}
		getname_local(cep->llp.sock, &event.local_addr);
		getname_peer(cep->llp.sock, &event.remote_addr);
	}
	if (reason == IW_CM_EVENT_CONNECT_REQUEST) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 2, 0)
		event.ird = cep->sdev->attrs.max_ird;
		event.ord = cep->sdev->attrs.max_ord;
#endif
		event.provider_data = cep;
		cm_id = cep->listen_cep->cm_id;
	} else
		cm_id = cep->cm_id;

	dprint_cep(DBG_CM, cep, "(QP%d): id=" dprint_ptr_str() ", dev(id)=%s, "
		"reason/event=%s(%d), status=%d\n",
		cep->qp ? QP_ID(cep->qp) : -1, cm_id,
		cm_id->device->name, iw_cm_event_type_str(reason), reason, status);

	return cm_id->event_handler(cm_id, &event);
}
/*
 * siw_qp_cm_drop()
 *
 * Drops established LLP connection if present and not already
 * scheduled for dropping. Called from user context, SQ workqueue
 * or receive IRQ. Caller signals if socket can be immediately
 * closed (basically, if not in IRQ).
 */
void siw_qp_cm_drop(struct siw_qp *qp, int schedule)
{
	struct siw_cep *cep = qp->cep;
	unsigned long flags;

	dprint_cep(DBG_CM|DBG_ON, cep, "(QP%d): SIW QP state=%d flags=%x, suspend rx and tx\n",
		QP_ID(qp), qp->attrs.state, qp->attrs.flags);

	lock_rq_rxsave(qp, flags);
	qp->rx_ctx.rx_suspend = 1;
	unlock_rq_rxsave(qp, flags);

	lock_sq_rxsave(qp, flags);
	qp->tx_ctx.tx_suspend = 1;
	unlock_sq_rxsave(qp, flags);

	if (!qp->cep)
		return;

	if (schedule)
		siw_cm_queue_work(cep, SIW_CM_WORK_CLOSE_LLP);
	else {
		siw_cep_get(cep);
		siw_cep_set_inuse(cep);

		if (cep->state == SIW_EPSTATE_CLOSED) {
			dprint_cep(DBG_CM, cep, "already closed\n");
			goto out;
		}
		/*
		 * Immediately close socket
		 */
		dprint_cep(DBG_CM, cep, "immediate close, state=%d, id=" dprint_ptr_str() ", sock=" dprint_ptr_str() ", QP%d\n", 
			   cep->state, cep->cm_id, cep->llp.sock, cep->qp ? QP_ID(cep->qp) : -1);

		if (cep->cm_id) {
			switch (cep->state) {

			case SIW_EPSTATE_CONNECTING:
				BUG_ON(!connect_non_block);
				siw_cancel_connect_timer(cep);
				siw_cm_upcall(cep, IW_CM_EVENT_CONNECT_REPLY,
					      -ECANCELED);
				break;
			case SIW_EPSTATE_AWAIT_MPAREP:
				/* Prevent race with MPA header arrival */
				siw_cancel_read_mpa_hdr(cep);
				siw_cancel_mpatimer(cep);
				siw_cm_upcall(cep, IW_CM_EVENT_CONNECT_REPLY,
					      -ECANCELED);
				break;

			case SIW_EPSTATE_RDMA_MODE:
				siw_cm_upcall(cep, IW_CM_EVENT_CLOSE, 0);

				break;

			case SIW_EPSTATE_AWAIT_MPAREQ:
				/* Prevent race with MPA header arrival */
				siw_cancel_read_mpa_hdr(cep);
				break;
			case SIW_EPSTATE_IDLE:
			case SIW_EPSTATE_LISTENING:
			case SIW_EPSTATE_RECVD_MPAREQ:
			case SIW_EPSTATE_CLOSED:
			case SIW_EPSTATE_REJECTING:
			case SIW_EPSTATE_REJECTED:
			default:

				break;
			}
			cep->cm_id->rem_ref(cep->cm_id);
			cep->cm_id = NULL;
			siw_cep_put(cep);
		}
		/* Prevent race with TCP_CLOSE event */
		siw_cep_state_change(cep, SIW_EPSTATE_CLOSED); /* qp-cm-drop */
		siw_cancel_peer_close(cep);

		if (cep->llp.sock) {
			siw_cep_socket_restore_ca(cep);
			siw_socket_disassoc(cep->llp.sock);
			sock_release(cep->llp.sock);
			cep->llp.sock = NULL;
		}

		/* Cancel any work scheduled by data-ready callback */
		siw_rx_cancel_work(qp);

		if (cep->qp) {
			BUG_ON(qp != cep->qp);
			cep->qp = NULL;
			siw_qp_put(qp);
		}
out:
		siw_cep_set_free(cep);
		siw_cep_put(cep);
	}
}

static inline int ksock_recv(struct socket *sock, char *buf, size_t size,
			     int flags)
{
	struct kvec iov = {buf, size};
	struct msghdr msg = {.msg_name = NULL, .msg_flags = flags};

	return kernel_recvmsg(sock, &msg, &iov, 1, size, flags);
}

/*
 * Expects params->pd_len in host byte order
 *
 * TODO: We might want to combine the arguments params and pdata to a single
 * pointer to a struct siw_mpa_info as defined in siw_cm.h.
 * This way, all private data parameters would be in a common struct.
 */
static int siw_send_mpareqrep(struct siw_cep *cep, const void *pdata,
			      u8 pd_len)
{
	struct socket	*s = cep->llp.sock;
	struct mpa_rr	*rr = &cep->mpa.hdr;
	struct kvec	iov[2];
	struct msghdr	msg;
	int		rv;

	memset(&msg, 0, sizeof(msg));

	rr->params.pd_len = cpu_to_be16(pd_len);

	iov[0].iov_base = rr;
	iov[0].iov_len = sizeof *rr;

	if (pd_len) {
		iov[1].iov_base = (char *)pdata;
		iov[1].iov_len = pd_len;

		rv =  kernel_sendmsg(s, &msg, iov, 2, pd_len + sizeof *rr);
	} else
		rv =  kernel_sendmsg(s, &msg, iov, 1, sizeof *rr);

	return rv < 0 ? rv : 0;
}

/*
 * Receive MPA Request/Reply header.
 *
 * Returns 0 if complete MPA Request/Reply haeder including
 * eventual private data was received. Returns -EAGAIN if
 * header was partially received or negative error code otherwise.
 *
 * Context: May be called in process context only
 */
static int siw_recv_mpa_rr(struct siw_cep *cep)
{
	struct mpa_rr	*hdr = &cep->mpa.hdr;
	struct socket	*s = cep->llp.sock;
	u16		pd_len;
	int		rcvd, to_rcv;

	if (cep->mpa.bytes_rcvd < sizeof(struct mpa_rr)) {

		rcvd = ksock_recv(s, (char *)hdr + cep->mpa.bytes_rcvd,
				  sizeof(struct mpa_rr) -
				  cep->mpa.bytes_rcvd, MSG_DONTWAIT);

		if (rcvd <= 0)
			return -ECONNABORTED;

		cep->mpa.bytes_rcvd += rcvd;

		if (cep->mpa.bytes_rcvd < sizeof(struct mpa_rr))
			return -EAGAIN;

		if (be16_to_cpu(hdr->params.pd_len) > MPA_MAX_PRIVDATA)
			return -EPROTO;
	}
	pd_len = be16_to_cpu(hdr->params.pd_len);

	/*
	 * At least the MPA Request/Reply header (frame not including
	 * private data) has been received.
	 * Receive (or continue receiving) any private data.
	 */
	to_rcv = pd_len - (cep->mpa.bytes_rcvd - sizeof(struct mpa_rr));

	if (!to_rcv) {
		/*
		 * We must have hdr->params.pd_len == 0 and thus received a
		 * complete MPA Request/Reply frame.
		 * Check against peer protocol violation.
		 */
		u32 word;

		rcvd = ksock_recv(s, (char *)&word, sizeof word, MSG_DONTWAIT);
		if (rcvd == -EAGAIN)
			return 0;

		if (rcvd == 0) {
			dprint_cep(DBG_CM, cep, "peer EOF\n");
			return -EPIPE;
		}
		if (rcvd < 0) {
			dprint_cep(DBG_CM, cep, "ERROR: %d:\n", rcvd);
			return rcvd;
		}
		dprint_cep(DBG_CM, cep, "peer sent extra data: %d\n", rcvd);
		return -EPROTO;
	}

	/*
	 * At this point, we must have hdr->params.pd_len != 0.
	 * A private data buffer gets allocated if hdr->params.pd_len != 0.
	 */
	if (!cep->mpa.pdata) {
		cep->mpa.pdata = kmalloc(pd_len + 4, GFP_KERNEL);
		if (!cep->mpa.pdata) {
			dprint_cep(DBG_CM | DBG_ON, cep, "OOM allocating private_data\n");
			return -ENOMEM;
		}
	}
	rcvd = ksock_recv(s, cep->mpa.pdata + cep->mpa.bytes_rcvd
			  - sizeof(struct mpa_rr), to_rcv + 4, MSG_DONTWAIT);

	if (rcvd < 0)
		return rcvd;

	if (rcvd > to_rcv)
		return -EPROTO;

	cep->mpa.bytes_rcvd += rcvd;

	if (to_rcv == rcvd) {
		dprint_cep(DBG_CM, cep, "%d bytes private_data received: %*ph\n", 
		       pd_len, pd_len, cep->mpa.pdata);

		return 0;
	}
	return -EAGAIN;
}


/*
 * siw_proc_mpareq()
 *
 * Read MPA Request from socket and signal new connection to IWCM
 * if success. Caller must hold lock on corresponding listening CEP.
 */
static int siw_proc_mpareq(struct siw_cep *cep)
{
	struct mpa_rr	*req;
	int		rv;

	dprint_cep(DBG_CM|DBG_ON, cep, "\n");

	rv = siw_recv_mpa_rr(cep);
	if (rv)
		goto out;

	req = &cep->mpa.hdr;

	if (__mpa_rr_revision(req->params.bits) > MPA_REVISION_1) {
		/* allow for 0 and 1 only */
		rv = -EPROTO;
		goto out;
	}
	if (memcmp(req->key, MPA_KEY_REQ, 16)) {
		rv = -EPROTO;
		goto out;
	}
	/*
	 * Prepare for sending MPA reply
	 */
	memcpy(req->key, MPA_KEY_REP, 16);

	if (req->params.bits & MPA_RR_FLAG_MARKERS
		|| (req->params.bits & MPA_RR_FLAG_CRC
			&& !mpa_crc_required && mpa_crc_strict)) {
		/*
		 * MPA Markers: currently not supported. Marker TX to be added.
		 *
		 * CRC:
		 *    RFC 5044, page 27: CRC MUST be used if peer requests it.
		 *    siw specific: 'mpa_crc_strict' parameter to reject
		 *    connection with CRC if local CRC off enforced by
		 *    'mpa_crc_strict' module parameter.
		 */
		dprint(DBG_CM|DBG_ON, " Reject: CRC %d:%d:%d, M %d:%d\n",
			req->params.bits & MPA_RR_FLAG_CRC ? 1 : 0,
			mpa_crc_required, mpa_crc_strict,
			req->params.bits & MPA_RR_FLAG_MARKERS ? 1 : 0, 0);

		req->params.bits &= ~MPA_RR_FLAG_MARKERS;
		req->params.bits |= MPA_RR_FLAG_REJECT; /* reject */

		if (!mpa_crc_required && mpa_crc_strict)
			req->params.bits &= ~MPA_RR_FLAG_CRC;

		kfree(cep->mpa.pdata);
		cep->mpa.pdata = NULL;

		(void)siw_send_mpareqrep(cep, NULL, 0);
		rv = -EOPNOTSUPP;
		goto out;
	}
	/*
	 * Enable CRC if requested by module initialization
	 */
	if (!(req->params.bits & MPA_RR_FLAG_CRC) && mpa_crc_required)
		req->params.bits |= MPA_RR_FLAG_CRC;
	
	/* Let the initiator know we support write acknowledgements */
	req->params.bits |= MPA_RR_FLAG_WR_ACK;
#if 0
	if (!cep->mpa.hdr.params.c && mpa_crc_required)
		cep->mpa.hdr.params.c = 1;
#endif

	siw_cep_state_change(cep, SIW_EPSTATE_RECVD_MPAREQ); /* process mpa-req -> connect request */

	/* Keep reference until IWCM accepts/rejects */
	siw_cep_get(cep);
	rv = siw_cm_upcall(cep, IW_CM_EVENT_CONNECT_REQUEST, 0);
	if (rv)
		siw_cep_put(cep);
out:
	return rv;
}


static int siw_proc_mpareply(struct siw_cep *cep)
{
	struct siw_qp_attrs	qp_attrs;
	struct siw_qp		*qp = cep->qp;
	struct mpa_rr		*rep;
	int			rv;
	bool			qp_locked;

	rv = siw_recv_mpa_rr(cep);
	if (rv != -EAGAIN)
		siw_cancel_mpatimer(cep);
	if (rv)
		goto out_err;

	rep = &cep->mpa.hdr;

	if (__mpa_rr_revision(rep->params.bits) > MPA_REVISION_1) {
		/* allow for 0 and 1 only */
		rv = -EPROTO;
		goto out_err;
	}
	if (memcmp(rep->key, MPA_KEY_REP, 16)) {
		rv = -EPROTO;
		goto out_err;
	}
	if (rep->params.bits & MPA_RR_FLAG_REJECT) {
		dprint_cep(DBG_CM, cep, "Got MPA reject with %d bytes of pdata: %*ph\n",
		       cep->mpa.bytes_rcvd, cep->mpa.bytes_rcvd, cep->mpa.pdata);
		(void)siw_cm_upcall(cep, IW_CM_EVENT_CONNECT_REPLY,
				    -ECONNREFUSED);

		rv = -ECONNREFUSED;
		goto out;
	}
	if ((rep->params.bits & MPA_RR_FLAG_MARKERS)
		|| (mpa_crc_required && !(rep->params.bits & MPA_RR_FLAG_CRC))
		|| (mpa_crc_strict && !mpa_crc_required
			&& (rep->params.bits & MPA_RR_FLAG_CRC))) {

		dprint_cep(DBG_CM|DBG_ON, cep, "Reply unsupp: CRC %d:%d:%d, M %d:%d\n",
			rep->params.bits & MPA_RR_FLAG_CRC ? 1 : 0,
			mpa_crc_required, mpa_crc_strict,
			rep->params.bits & MPA_RR_FLAG_MARKERS ? 1 : 0, 0);

		(void)siw_cm_upcall(cep, IW_CM_EVENT_CONNECT_REPLY,
				    -EPROTO);
		rv = -EINVAL;
		goto out;
	}
	memset(&qp_attrs, 0, sizeof qp_attrs);
	qp_attrs.mpa.marker_rcv = 0;
	qp_attrs.mpa.marker_snd = 0;
	qp_attrs.mpa.crc = cep->mpa.hdr.params.bits & MPA_RR_FLAG_CRC ? 1 : 0;
	qp_attrs.mpa.wr_ack = cep->mpa.hdr.params.bits & MPA_RR_FLAG_WR_ACK ? 1 : 0;
	qp_attrs.irq_size = cep->ird;
	qp_attrs.orq_size = cep->ord;
	qp_attrs.llp_stream_handle = cep->llp.sock;
	qp_attrs.state = SIW_QP_STATE_RTS;

	/* Move socket RX/TX under QP control */
	if (!(qp_locked = try_write_lock_qp(qp)) || qp->attrs.state > SIW_QP_STATE_RTR) {
		rv = -EBUSY;
		do {
			int state_lock_failed;
			if ((state_lock_failed = atomic_xchg(&qp->state_lock_failed, 0))) {
				dprint(DBG_CM|DBG_ON, "QP(%d): state_lock_failed %d for siw_qp " dprint_ptr_str() "",
						QP_ID(qp), state_lock_failed, qp);
				WARN_ON(1);
			}
		} while(0);
		if (qp_locked)
			write_unlock_qp(qp);
		goto out_err;
	}
	rv = siw_qp_modify(qp, &qp_attrs, SIW_QP_ATTR_STATE|
					       SIW_QP_ATTR_LLP_HANDLE|
					       SIW_QP_ATTR_ORD|
					       SIW_QP_ATTR_IRD|
					       SIW_QP_ATTR_MPA);

	do {
		int state_lock_failed;
		if ((state_lock_failed = atomic_xchg(&qp->state_lock_failed, 0))) {
			dprint(DBG_CM|DBG_ON, "QP(%d): state_lock_failed %d for siw_qp " dprint_ptr_str() "",
					QP_ID(qp), state_lock_failed, qp);
			WARN_ON(1);
		}
	} while(0);
	write_unlock_qp(qp);

	if (!rv) {
		rv = siw_cm_upcall(cep, IW_CM_EVENT_CONNECT_REPLY, 0);
		if (!rv)
			siw_cep_state_change(cep, SIW_EPSTATE_RDMA_MODE); /* process mpa-reply -> connect-reply */

		goto out;
	}

out_err:
	if (rv != -EAGAIN)
		(void)siw_cm_upcall(cep, IW_CM_EVENT_CONNECT_REPLY, -EINVAL);
out:
	return rv;
}

/*
 * siw_connect_newconn - finish connection
 * 
 */
static int siw_connect_newconn(struct siw_cep *cep)
{
	struct socket		*s = cep->llp.sock;
	struct siw_qp	*qp = cep->qp;
	int rv;

	siw_cancel_connect_timer(cep);

	rv = siw_sock_nodelay(s, cep->orig_ca_name, sizeof(cep->orig_ca_name));
	if (rv < 0) {
		dprint_cep(DBG_CM, cep, "(QP%d): siw_sock_nodelay(): rv=%d\n",
			   QP_ID(qp), rv);
		goto out;
	}

#if KS_HAS_SO_INCOMING_CPU
	/* Set the RX CPU from the RCQ interrupt vector */
	if (use_so_incoming_cpu && qp->rcq) {
		int rx_cpu = qp->rcq->comp_vector + comp_vector_cpu0;
		sock_setsockopt_val_t optval = SOCK_SETSOCKOPT_VAL_PTR(&rx_cpu);
		rv = sock_setsockopt(s, SOL_SOCKET, SO_INCOMING_CPU,
				     optval, sizeof(rx_cpu));
		if (rv < 0)
			pr_err("Error %d setting SO_INCOMING_CPU\n", rv);
		else
			dprint_cep(DBG_CM|DBG_ON, cep, "(QP%d): set SO_INCOMING_CPU to %d\n", QP_ID(qp), rx_cpu);
	}
#endif

	siw_cep_state_change(cep, SIW_EPSTATE_AWAIT_MPAREP); /* send mpa-req-rep */

	/*
	 * Set MPA Request bits: CRC if required, no MPA Markers,
	 * MPA Rev. 1, Key 'Request'.
	 */
	cep->mpa.hdr.params.bits = 0;
	__mpa_rr_set_revision(&cep->mpa.hdr.params.bits, MPA_REVISION_1);
	
	if (mpa_crc_required)
		cep->mpa.hdr.params.bits |= MPA_RR_FLAG_CRC;
	
	memcpy(cep->mpa.hdr.key, MPA_KEY_REQ, 16);
	
	/* [NVMESH-3371]: Schedule the work before we send the response
	 * in case the reply arrives and the data-ready callback is called
	 * before we exit the function and queue the work. If the send fails,
	 * cancel the delayed work. */
	rv = siw_cm_queue_work(cep, SIW_CM_WORK_MPATIMEOUT);
	if (rv < 0) {
		dprint_cep(DBG_CM, cep, "siw_cm_queue_work Failed: rv=%d\n", rv);
		goto out;
	}

	rv = siw_send_mpareqrep(cep, cep->mpa.pdata, cep->mpa.hdr.params.pd_len);
	if (rv < 0) {
		siw_cancel_mpatimer(cep);
		dprint_cep(DBG_CM, cep, "siw_send_mpareqrep Failed: rv=%d\n", rv);
		goto out;
	}

	/* Free private data */
	kfree(cep->mpa.pdata);
	cep->mpa.pdata = NULL;
	cep->mpa.hdr.params.pd_len = 0;

out:
	if (rv < 0)
		dprint_cep(DBG_CM, cep, "connect failed (%d)", rv);

	return rv;
}

/*
 * siw_accept_newconn - accept an incoming pending connection
 *
 */
static void siw_accept_newconn(struct siw_cep *cep)
{
	struct socket		*s = cep->llp.sock;
	struct socket		*new_s = NULL;
	struct siw_cep		*new_cep = NULL;
	int			rv = 0, val; /* debug only. should disappear */
	sock_setsockopt_val_t optval = SOCK_SETSOCKOPT_VAL_PTR(&val);

	if (cep->state != SIW_EPSTATE_LISTENING)
		goto error;

	new_cep = siw_cep_alloc(cep->sdev);
	if (!new_cep)
		goto error;

	if (siw_cm_alloc_work(new_cep, SIW_CM_NON_LISTENING_CEP_WORKS) != 0)
		goto error;

	/*
	 * Copy saved socket callbacks from listening CEP
	 * and assign new socket with new CEP
	 */
	new_cep->sk_state_change = cep->sk_state_change;
	new_cep->sk_data_ready   = cep->sk_data_ready;
	new_cep->sk_write_space  = cep->sk_write_space;
	new_cep->sk_error_report = cep->sk_error_report;
	
	/* [NVMESH-3371]: Queue the delayed work before we call accept
	 * in case we get state-change callback with TCP_CLOSE before the
	 * delayed work is queued. If the kernel_accep fails, we cancel the work */
	rv = siw_cm_queue_work(new_cep, SIW_CM_WORK_MPATIMEOUT);
	if (rv < 0) {
		dprint_cep(DBG_CM, cep, "siw_cm_queue_work Failed: rv=%d\n", rv);
		goto error;
	}
	rv = kernel_accept(s, &new_s, O_NONBLOCK);
	if (rv != 0) {
		/*
		 * TODO: Already aborted by peer?
		 * Is there anything we should do?
		 */
		dprint_cep(DBG_CM|DBG_ON, cep, "ERROR: "
			"kernel_accept(): rv=%d\n", rv);
		goto error;
	}

#if KS_HAS_SOCK_NOT_OWNED_BY_ME
	{
		struct sock 	*sk = new_s->sk;
		struct net      *net = sock_net(sk);

		/* Prevent inet_csk_clear_xmit_timers_sync being called from tcp_close()
		 * Fixes [NVMESH-5532] */
		sk->sk_net_refcnt = 1;
		get_net(net);
	}
#endif

	val = (int)sock_buff_sz;
	rv = sock_setsockopt(new_s, SOL_SOCKET, SO_SNDBUF,
			     optval, sizeof(val));
	if (rv < 0) {
		pr_err("Error %d setting SO_SNDBUF\n", rv);
		goto error;
	}
	rv = sock_setsockopt(new_s, SOL_SOCKET, SO_RCVBUF,
			     optval, sizeof(val));
	if (rv < 0) {
		pr_err("Error %d setting SO_SNDBUF\n", rv);
		goto error;
	}

	new_cep->llp.sock = new_s;
	siw_cep_get(new_cep);
	new_s->sk->sk_user_data = new_cep;

	dprint_cep(DBG_CM, cep, "s=" dprint_ptr_str() ", new_s=" dprint_ptr_str() "): "
		"New LLP connection accepted\n", s, new_s);

	rv = siw_sock_nodelay(new_s, new_cep->orig_ca_name, sizeof(new_cep->orig_ca_name));
	if (rv != 0) {
		dprint_cep(DBG_CM|DBG_ON, new_cep, "ERROR: "
			"siw_sock_nodelay(): rv=%d\n", rv);
		goto error;
	}

	siw_cep_state_change(new_cep, SIW_EPSTATE_AWAIT_MPAREQ); /* siw-accept-newconn */

	/*
	 * See siw_proc_mpareq() etc. for the use of new_cep->listen_cep.
	 */
	new_cep->listen_cep = cep;
	siw_cep_get(cep);

	if (atomic_read(&new_s->sk->sk_rmem_alloc)) {
		/*
		 * MPA REQ already queued
		 */
		dprint_cep(DBG_CM, new_cep, "Immediate MPA req.\n");

		siw_cep_set_inuse(new_cep);
		rv = siw_proc_mpareq(new_cep);
		siw_cep_set_free(new_cep);

		if (rv != -EAGAIN) {
			/* rv is either 0 (succesful read or a critical error).
			 * Either way, we are finished with the listener and can cancel any work put by llp_data_ready */
			siw_cancel_read_mpa_hdr(new_cep);
			/* Remove reference to listener */
			siw_cep_put(cep);
			new_cep->listen_cep = NULL;
			if (rv) {
				/* Remove reference to sock */
				new_cep->llp.sock = NULL;
				dprint_cep(DBG_CM|DBG_ON, new_cep, "Immediate MPA req. ERROR: rv=%d\n", rv);
				goto error;
			}
		}
	}
	return;

error:
	if (new_cep) {
		/* Prevents TCP_CLOSE event from trying to run SIW_CM_WORK_PEER_CLOSE
		 * after we have already released the socket */
		siw_cep_state_change(new_cep, SIW_EPSTATE_CLOSED);
		siw_cancel_peer_close(new_cep);
		siw_cancel_mpatimer(new_cep);
		siw_cep_socket_restore_ca(new_cep);
		siw_cep_put(new_cep);
	}

	if (new_s) {
		siw_socket_disassoc(new_s);
		sock_release(new_s);
	}
	dprint_cep(DBG_CM|DBG_ON, cep, "ERROR: rv=%d\n", rv);
}

static int siw_reject_work(struct siw_cep *cep)
{
	int rv;

	if (__mpa_rr_revision(cep->mpa.hdr.params.bits) != MPA_REVISION_1) {
		dprint_cep(DBG_CM|DBG_ON, cep, "Invalid MPA Revision\n");
		rv = -EPROTO;
		goto out;
	}

	dprint_cep(DBG_CM, cep, "Sending reject with %d bytes of private_data: %*ph\n",
	       cep->reject_plen, cep->reject_plen, cep->reject_pdata);

	cep->mpa.hdr.params.bits |= MPA_RR_FLAG_REJECT; /* reject */
	rv = siw_send_mpareqrep(cep, cep->reject_pdata, cep->reject_plen);
	if (rv < 0) {
		dprint_cep(DBG_CM | DBG_ON, cep, 
		       "Sending reject failed (%d)\n", rv);
		goto out;
	}

	siw_cep_state_change(cep, SIW_EPSTATE_REJECTED);

out:
	return rv;
}

static void siw_cm_work_handler(struct work_struct *w)
{
	struct siw_cm_work	*work;
	struct siw_cep		*cep;
	int release_cep = 0, rv = 0;
	unsigned long jif = jiffies;
	unsigned long end_jif, pre_jif, post_jif;

	work = container_of(w, struct siw_cm_work, work.work);
	cep = work->cep;

	dprint_cep(DBG_CM, cep, "(QP%d): WORK type: %s(%d), work_idx=%llu, state: %s(%d), LQ=%d, LC=%d\n",
		cep->qp ? QP_ID(cep->qp) : -1, siw_cm_work_type_str(work->type), work->type, work->work_idx,
		siw_cep_state_str(cep->state), cep->state, !list_empty(&cep->listenq), !!cep->listen_cep);
	if (jif - work->q_jif > HZ/5) {
		dprint_cep(DBG_CM | DBG_ON, cep, "(QP%d): WORK type: %d, state: %d delayed by %lu ms\n",
			cep->qp ? QP_ID(cep->qp) : -1, work->type, 
			cep->state, (1000 * (jif - work->q_jif)) / HZ);
	}

	siw_cep_set_inuse(cep);

	switch (work->type) {

	case SIW_CM_WORK_ACCEPT:
		siw_accept_newconn(cep);
		break;

	case SIW_CM_WORK_READ_MPAHDR:
		
		siw_clear_read_mpa_hdr(cep, work);

		switch (cep->state) {

		case SIW_EPSTATE_AWAIT_MPAREQ:

			if (cep->listen_cep) {
				siw_cep_set_inuse(cep->listen_cep);

				if (cep->listen_cep->state ==
				    SIW_EPSTATE_LISTENING)
					rv = siw_proc_mpareq(cep);
				else
					rv = -EFAULT;

				siw_cep_set_free(cep->listen_cep);

				if (rv != -EAGAIN) {
					siw_cep_put(cep->listen_cep);
					cep->listen_cep = NULL;
					if (rv) {
						dprint_cep(DBG_CM, cep, "mark release\n");
						release_cep = 1;
					}
				}
			}
			break;

		case SIW_EPSTATE_AWAIT_MPAREP:
			if ((rv = siw_proc_mpareply(cep)) < 0) {
				if (rv != -EAGAIN) {
					dprint_cep(DBG_CM, cep, "mark release\n");
					release_cep = 1;
				}
			} else {
				siw_rx_queue_work(cep->qp, 0);
			}
			break;

		default:
			/*
			 * CEP already moved out of MPA handshake.
			 * any connection management already done.
			 * silently ignore the mpa packet.
			 */
			dprint_cep(DBG_CM, cep, "CEP not in MPA "
				"handshake state: %d\n", cep->state);
			if (cep->state == SIW_EPSTATE_RDMA_MODE) {
				siw_rx_queue_work(cep->qp, 0);

				dprint_cep(DBG_ON, cep, "cep already in RDMA mode");
			} else
				dprint_cep(DBG_ON, cep, "cep out of state: %d\n", cep->state);
				
		}
		if (rv && rv != EAGAIN) {
			dprint_cep(DBG_CM, cep, "mark release\n");
			release_cep = 1;
		}

		break;

	case SIW_CM_WORK_CLOSE_LLP:
		/*
		 * QP scheduled LLP close
		 */
		dprint_cep(DBG_CM, cep, "SIW_CM_WORK_CLOSE_LLP, cep->state=%d\n",
			cep->state);

		if (cep->cm_id) {
			switch (cep->state) {
			case SIW_EPSTATE_CONNECTING:
			case SIW_EPSTATE_AWAIT_MPAREP:
				/*
				 * MPA reply not received, but connection drop
				 */
				siw_cm_upcall(cep, IW_CM_EVENT_CONNECT_REPLY,
							  -ECONNRESET);
				break;
			case SIW_EPSTATE_RDMA_MODE:
				siw_cm_upcall(cep, IW_CM_EVENT_CLOSE, 0);
				break;
			default:
				break;
			}
		}

		if (cep->state != SIW_EPSTATE_CLOSED) {
			dprint_cep(DBG_CM, cep, "mark release\n");
			release_cep = 1;
		}

		break;

	case SIW_CM_WORK_PEER_CLOSE:

		dprint_cep(DBG_CM, cep, "SIW_CM_WORK_PEER_CLOSE, "
			"cep->state=%d\n", cep->state);

		siw_clear_peer_close(cep, work);

		if (cep->cm_id) {
			switch (cep->state) {
			case SIW_EPSTATE_CONNECTING:
			case SIW_EPSTATE_AWAIT_MPAREP:
				/*
				 * MPA reply not received, but connection drop
				 */
				siw_cm_upcall(cep, IW_CM_EVENT_CONNECT_REPLY,
					      -ECONNRESET);
				break;

			case SIW_EPSTATE_RDMA_MODE:
				/*
				 * NOTE: IW_CM_EVENT_DISCONNECT is given just
				 *       to transition IWCM into CLOSING.
				 *       FIXME: is that needed?
				 */
				pre_jif = jiffies;
				siw_cm_upcall(cep, IW_CM_EVENT_DISCONNECT, 0);
				siw_cm_upcall(cep, IW_CM_EVENT_CLOSE, 0);
				post_jif = jiffies;
				if (post_jif - pre_jif > HZ / 5) {
					dprint_cep(DBG_CM | DBG_ON, cep, "QP: %d/" dprint_ptr_str() " upcalls took %lu ms",
						cep->qp ? QP_ID(cep->qp) : -1, cep->qp,
						(1000UL * (post_jif - pre_jif)) / HZ);
				}

				break;
			case SIW_EPSTATE_REJECTING:
			case SIW_EPSTATE_REJECTED:
				/* ULP has already rejected connection so we don't need to inform it about the close */
				break;

			default:

				break;
				/*
				 * for these states there is no connection
				 * known to the IWCM.
				 */
			}
		} else {
			switch (cep->state) {
			case SIW_EPSTATE_RECVD_MPAREQ:
				/*
				 * Wait for the CM to call its accept/reject
				 */
				dprint_cep(DBG_CM, cep, "STATE_RECVD_MPAREQ: "
					"wait for CM:\n");
				break;
			case SIW_EPSTATE_AWAIT_MPAREQ:
				/*
				 * Socket close before MPA request received.
				 */
				dprint_cep(DBG_CM, cep,
					"STATE_AWAIT_MPAREQ: "
					"unlink from Listener\n");
				siw_cep_put(cep->listen_cep);
				cep->listen_cep = NULL;

				break;
			case SIW_EPSTATE_REJECTING:
			case SIW_EPSTATE_REJECTED:
				/* Nothing to do. ULP has already rejected connection */
				break;

			default:
				break;
			}
		}
		if (cep->state != SIW_EPSTATE_CLOSED) {
			dprint_cep(DBG_CM, cep, "mark release\n");
			release_cep = 1;
		}

		break;

	case SIW_CM_WORK_MPATIMEOUT:

		siw_clear_mpatimer(cep, work);

		if (cep->state == SIW_EPSTATE_AWAIT_MPAREP) {
			/*
			 * MPA request timed out:
			 * Hide any partially received private data and signal
			 * timeout
			 */
			cep->mpa.hdr.params.pd_len = 0;

			if (cep->cm_id)
				siw_cm_upcall(cep, IW_CM_EVENT_CONNECT_REPLY,
					      -ETIMEDOUT);

			dprint_cep(DBG_CM, cep, "mark release\n");
			release_cep = 1;

		} else if (cep->state == SIW_EPSTATE_AWAIT_MPAREQ) {
			/*
			 * No MPA request received after peer TCP stream setup.
			 */
			siw_cep_put(cep->listen_cep);
			cep->listen_cep = NULL;
			dprint_cep(DBG_CM, cep, "mark release\n");
			release_cep = 1;
		}
		break;
	case SIW_CM_WORK_CONNECT:
	{
		int rv;
		BUG_ON(!connect_non_block);
		if (cep->state == SIW_EPSTATE_CONNECTING) {
			if ((rv = siw_connect_newconn(cep)) < 0) {
				siw_cm_upcall(cep, IW_CM_EVENT_CONNECT_REPLY, rv);
				dprint_cep(DBG_CM, cep, "mark release\n");
				release_cep = 1;
			}
		}
		break;
	}
	case SIW_CM_WORK_CONNECT_TIMEOUT:
		BUG_ON(!connect_non_block);
		siw_clear_connect_timer(cep, work);
		if (cep->state == SIW_EPSTATE_CONNECTING) {
			/* Free private data */
			kfree(cep->mpa.pdata);
			cep->mpa.pdata = NULL;
			cep->mpa.hdr.params.pd_len = 0;
			if (cep->cm_id)
				siw_cm_upcall(cep, IW_CM_EVENT_CONNECT_REPLY,
							  -ETIMEDOUT);

			dprint_cep(DBG_CM, cep, "mark release\n");
			release_cep = 1;
		}
		break;
	case SIW_CM_WORK_CONNECT_CANCEL:
		BUG_ON(!connect_non_block);
		switch (cep->state) {
		case SIW_EPSTATE_CONNECTING:
		case SIW_EPSTATE_AWAIT_MPAREP:
			/* Free private data */
			kfree(cep->mpa.pdata);
			cep->mpa.pdata = NULL;
			cep->mpa.hdr.params.pd_len = 0;
			dprint_cep(DBG_CM, cep, "mark release\n");
			release_cep = 1;
			break;
		default:
			dprint_cep(DBG_CM, cep, "(QP%d): ignoring connect cancel in invalid state %d\n",
				   cep->qp ? QP_ID(cep->qp) : -1, cep->state);
		}
		break;
	case SIW_CM_WORK_REJECT_CONNECTION:
		switch (cep->state) {
		case SIW_EPSTATE_REJECTING:
			if ((rv = siw_reject_work(cep)) < 0) {
				dprint_cep(DBG_CM | DBG_ON, cep, "siw_reject_work failed (%d)\n", rv);
				release_cep = 1;
			}
			break;
		default:
			dprint_cep(DBG_CM, cep, "(QP%d): ignoring reject in invalid state %d\n",
			       cep->qp ? QP_ID(cep->qp) : -1, cep->state);
		}
		break;

	default:
		BUG();
	}

	if (release_cep) {

		dprint_cep(DBG_CM, cep, "Release: "
			"mpa_timer=%s, sock=" dprint_ptr_str() ", QP%d, id=" dprint_ptr_str() "\n",
			cep->mpa_timer ? "y" : "n", cep->llp.sock,
			cep->qp ? QP_ID(cep->qp) : -1, cep->cm_id);

		siw_cancel_connect_timer(cep);
		siw_cancel_mpatimer(cep);
		siw_cancel_read_mpa_hdr(cep);

		siw_cep_state_change(cep, SIW_EPSTATE_CLOSED); /* release-cep, cm-work */
		siw_cancel_peer_close(cep);

		if (cep->qp) {
			/* Bring down the QP - Part #1 (before siw_socket_disassoc()) */
			struct siw_qp *qp = cep->qp;
			/*
			 * Serialize a potential race with application
			 * closing the QP and calling siw_qp_cm_drop()
			 */
			siw_qp_get(qp);
			siw_cep_set_free(cep);

			pre_jif = jiffies;
			siw_qp_llp_close(qp);
			post_jif = jiffies;
			if (post_jif - pre_jif > HZ / 5) {
				dprint_cep(DBG_CM | DBG_ON, cep, "QP: %d/" dprint_ptr_str() " siw_qp_llp_close took %lu ms",
					cep->qp ? QP_ID(cep->qp) : -1, cep->qp,
					(1000UL * (post_jif - pre_jif)) / HZ);
			}
		}
		if (cep->llp.sock) {
			pre_jif = jiffies;
			siw_cep_socket_restore_ca(cep);
			siw_socket_disassoc(cep->llp.sock);
			sock_release(cep->llp.sock);
			cep->llp.sock = NULL;
			post_jif = jiffies;
			if (post_jif - pre_jif > HZ / 5) {
				dprint_cep(DBG_CM | DBG_ON, cep, "QP: %d/" dprint_ptr_str() " sock_release took %lu ms",
					cep->qp ? QP_ID(cep->qp) : -1, cep->qp,
					(1000UL * (post_jif - pre_jif)) / HZ);
			}
		}
		if (cep->qp) {
			/* Bring down the QP - Part #2 (after siw_socket_disassoc()) */
			struct siw_qp *qp = cep->qp;

			/* Cancel any rx_work scheduled by data-ready callback */
			pre_jif = jiffies;
			siw_rx_cancel_work(qp);
			post_jif = jiffies;
			if (post_jif - pre_jif > HZ / 5) {
				dprint_cep(DBG_CM | DBG_ON, cep, "QP: %d/" dprint_ptr_str() " cancel_delayed_work_sync took %lu ms",
					   cep->qp ? QP_ID(cep->qp) : -1, cep->qp,
					   (1000UL * (post_jif - pre_jif)) / HZ);
			}

			/* Ref put for the cep->qp pointer */
			siw_qp_put(qp);

			siw_cep_set_inuse(cep);
			cep->qp = NULL; /* Used by socket callbacks (sk_to_qp), must be after siw_socket_disassoc() */

			/* Ref put for kref_init */
			siw_qp_put(qp);
		}
		if (cep->cm_id) {
			cep->cm_id->rem_ref(cep->cm_id);
			cep->cm_id = NULL;
		}
		siw_cep_put(cep); /* This is the needed put for kref_init call */
	}

	siw_cep_set_free(cep);

	dprint_cep(DBG_CM, cep, "(Exit): WORK type: %s(%d), QP: %d\n", 
		siw_cm_work_type_str(work->type), work->type,
		cep->qp ? QP_ID(cep->qp) : -1);

	end_jif = jiffies;
	if (end_jif - jif > HZ / 5) {
		dprint_cep(DBG_CM | DBG_ON, cep, "WORK type: %d, QP: %d/" dprint_ptr_str() " took %lu ms",
			work->type, cep->qp ? QP_ID(cep->qp) : -1, cep->qp,
			(1000UL * (end_jif - jif)) / HZ);
	}

	siw_put_work(work, false);
	//siw_cep_put(cep); Moved to siw_get_work / siw_put_work
}

static struct workqueue_struct *siw_cm_wq;

int siw_cm_queue_work(struct siw_cep *cep, enum siw_work_type type)
{
	struct siw_cm_work *work = siw_get_work(cep);
	unsigned long delay = 0;

	if (!work) {
		dprint_cep(DBG_ON, cep, "Failed\n");
		pr_warn("(CEP:" dprint_ptr_str() ") - Failed to get work from pool\n", cep);
		WARN_ON_ONCE(1);
		return -ENOMEM;
	}
	work->type = type;
	work->cep = cep;
	work->work_idx = atomic64_inc_return(&siw_cm_wq_work_idx);

	//siw_cep_get(cep); Moved to siw_get_work / siw_put_work

	INIT_DELAYED_WORK(&work->work, siw_cm_work_handler);

	if (type == SIW_CM_WORK_MPATIMEOUT) {
		/* Get a reference for assigning to cep->mpa_timer */
		siw_get_work_ref(work);
		cep->mpa_timer = work;

		if (cep->state == SIW_EPSTATE_AWAIT_MPAREP)
			delay = MPAREQ_TIMEOUT;
		else
			delay = MPAREP_TIMEOUT;
	} else if (type == SIW_CM_WORK_CONNECT_TIMEOUT) {
		BUG_ON(cep->state != SIW_EPSTATE_CONNECTING);

		/* Get a reference for assigning to cep->connect_timer */
		siw_get_work_ref(work);
		cep->connect_timer = work;
		delay = CONNECT_TIMEOUT;
	} else if (type == SIW_CM_WORK_READ_MPAHDR) {
		spin_lock_bh(&cep->lock);
		if (cep->state != SIW_EPSTATE_AWAIT_MPAREQ && cep->state != SIW_EPSTATE_AWAIT_MPAREP) {
			spin_unlock_bh(&cep->lock);
			dprint_cep(DBG_CM, cep, "(QP%d): Not scheduling READ_MPAHDR, CEP in state %s (%d)\n",
					cep->qp ? QP_ID(cep->qp) : -1, siw_cep_state_str(cep->state), cep->state);
			siw_put_work(work, false);
			//siw_cep_put(cep); Moved to siw_get_work / siw_put_work
			goto out;
		} else if (cep->read_mpa_hdr_work) {
			//[NVMESH-3837]: Get trace fields before unlock
			struct siw_cm_work *mpa_work = cep->read_mpa_hdr_work;
			u64 mpa_work_idx = mpa_work->work_idx;
			spin_unlock_bh(&cep->lock);
			dprint_cep(DBG_CM, cep, "(QP%d): Already scheduled READ_MPAHDR, (work " dprint_ptr_str() " work_idx=%llu)\n",
					cep->qp ? QP_ID(cep->qp) : -1, mpa_work, mpa_work_idx);
			siw_put_work(work, false);
			//siw_cep_put(cep); Moved to siw_get_work / siw_put_work
			goto out;
		}
		/* Get a reference for assigning to cep->read_mpa_hdr_work */
		siw_get_work_ref(work);
		cep->read_mpa_hdr_work = work;
		spin_unlock_bh(&cep->lock);
	} else if (type == SIW_CM_WORK_PEER_CLOSE) {
		spin_lock_bh(&cep->lock);
		if (cep->peer_close_work) {
			//[NVMESH-3837]: Get trace fields before unlock
			struct siw_cm_work *pc_work = cep->peer_close_work;
			u64 pc_work_idx = pc_work->work_idx;
			spin_unlock_bh(&cep->lock);
			dprint_cep(DBG_CM, cep, "(QP%d): Already scheduled PEER_CLOSE (work " dprint_ptr_str() " work_idx=%llu)\n",
			       cep->qp ? QP_ID(cep->qp) : -1, pc_work, pc_work_idx);
			siw_put_work(work, false);
			//siw_cep_put(cep); Moved to siw_get_work / siw_put_work
			goto out;
		}
		/* Get a reference for assigning to cep->peer_close_work */
		siw_get_work_ref(work);
		cep->peer_close_work = work;
		spin_unlock_bh(&cep->lock);
	}
	dprint_cep(DBG_CM, cep,"(QP%d): WORK type: %s(%d), LQ=%d, LC=%d, work " dprint_ptr_str() " work_idx=%llu, "
		"timeout %lu\n",
		cep->qp ? QP_ID(cep->qp) : -1, siw_cm_work_type_str(type), type, !list_empty(&cep->listenq), !!cep->listen_cep, work, work->work_idx, delay);

	work->q_jif = jiffies + delay;
	if (!queue_delayed_work(siw_cm_wq, &work->work, delay)) {
		/* API failed which means someone managed to set the pending bit of the work before us, this should 
		   only happen if someone called cancel before queue_delayed_work change the work to pending by itself
		   SEE: https://elixir.bootlin.com/linux/v4.20.17/source/kernel/workqueue.c#L1248
		   As the job never run will reset the needed fields and put the work + cep back.
		*/
		dprint_cep(DBG_CM, cep, "work " dprint_ptr_str() " work_idx=%llu failed to queue\n", work, work->work_idx);

		spin_lock_bh(&cep->lock);
		switch (type) {
		case SIW_CM_WORK_CONNECT_TIMEOUT:
			if (cep->connect_timer) {
				/* Connect timer is only scheduled once */
				BUG_ON(cep->connect_timer != work);
				cep->connect_timer = NULL;
				/* Puts the reference matched with assigning to cep->connect_timer */
				siw_put_work(work, true);
			} else {
				dprint_cep(DBG_CM, cep, "work " dprint_ptr_str() " work_idx=%llu already cleared by cancel routine\n", 
				       work, work->work_idx);
			}
			break;
		case SIW_CM_WORK_MPATIMEOUT:
			if (cep->mpa_timer) {
				/* MPA timer is only scheduled once */
				BUG_ON(cep->mpa_timer != work);
				cep->mpa_timer = NULL;
				/* Puts the reference matched with assigning to cep->mpa_timer */
				siw_put_work(work, true);
			} else {
				dprint_cep(DBG_CM, cep, "work " dprint_ptr_str() " work_idx=%llu already cleared by cancel routine\n", 
				       work, work->work_idx);
			}
			break;
		case SIW_CM_WORK_READ_MPAHDR:
			/* Read MPA Header can be scheduled more than once */
			if (cep->read_mpa_hdr_work && cep->read_mpa_hdr_work == work) {
				cep->read_mpa_hdr_work = NULL;
				/* Puts the reference matched with assigning to cep->read_mpa_hdr_work */
				siw_put_work(work, true);
			} else {
				dprint_cep(DBG_CM, cep, "work " dprint_ptr_str() " work_idx=%llu already cleared by cancel routine\n", 
				       work, work->work_idx);
			}
			break;
		case SIW_CM_WORK_PEER_CLOSE:
			if (cep->peer_close_work) {
				/* Peer close is only scheduled once */
				BUG_ON(cep->peer_close_work != work);
				cep->peer_close_work = NULL;
				/* Puts the reference matched with assigning to cep->peer_close_work */
				siw_put_work(work, true);
			} else {
				dprint_cep(DBG_CM, cep, "work " dprint_ptr_str() " work_idx=%llu already cleared by cancel routine\n", 
					   work, work->work_idx);
			}
			break;
		default:
			break;
		}
		/* Puts the reference matched with siw_get_work */
		siw_put_work(work, true);
		spin_unlock_bh(&cep->lock);
		//siw_cep_put(cep); Moved to siw_get_work / siw_put_work
		goto out;
	}

out:
	return 0;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 15, 0)
static void siw_cm_llp_data_ready(struct sock *sk, int flags)
#else
static void siw_cm_llp_data_ready(struct sock *sk)
#endif
{
	struct siw_cep	*cep;

	read_lock(&sk->sk_callback_lock);

	cep = sk_to_cep(sk);
	if (!cep) {
		dprint(DBG_CM, "(): sk " dprint_ptr_str() " (sk_state: %d) has no cep\n", sk, sk->sk_state);

		/* Warning disabled. It is not present in upstream */
		/* WARN_ON(1); */
		goto out;
	}

	dprint_cep(DBG_CM, cep, "state: %s(%d), LQ=%d, LC=%d\n",
		   siw_cep_state_str(cep->state), cep->state,
		   !list_empty(&cep->listenq), !!cep->listen_cep);

	switch (cep->state) {

	case SIW_EPSTATE_RDMA_MODE:
	case SIW_EPSTATE_LISTENING:

		break;

	case SIW_EPSTATE_AWAIT_MPAREQ:
	case SIW_EPSTATE_AWAIT_MPAREP:
		if (sk->sk_state == TCP_CLOSE || sk->sk_state == TCP_CLOSE_WAIT) {
			dprint_cep(DBG_CM, cep, "already got TCP_CLOSE, skipping SIW_CM_WORK_READ_MPAHDR");
		} else {
			siw_cm_queue_work(cep, SIW_CM_WORK_READ_MPAHDR);
		}
		break;

	default:
		dprint_cep(DBG_CM, cep, "Unexpected DATA, state %d\n", cep->state);
		break;
	}
out:

	//end print
	read_unlock(&sk->sk_callback_lock);
}

static void siw_cm_llp_write_space(struct sock *sk)
{
	struct siw_cep	*cep = sk_to_cep(sk);

	if (cep)
		dprint_cep(DBG_CM, cep, "state: %d\n", cep->state);
}

static void siw_cm_llp_error_report(struct sock *sk)
{
	struct siw_cep	*cep = sk_to_cep(sk);

	dprint(DBG_CM, "(): error: %d, state: %d\n", sk->sk_err, sk->sk_state);

	if (cep) {
		cep->sk_error = sk->sk_err;
		dprint_cep(DBG_CM, cep, "Got sock error: %d,  cep->state: %d\n", sk->sk_err, cep->state);
		cep->sk_error_report(sk);
	}
}

static void siw_cm_llp_state_change(struct sock *sk)
{
	struct siw_cep	*cep;
	struct socket	*s __attribute__((unused));
	void (*orig_state_change)(struct sock *);


	read_lock(&sk->sk_callback_lock);

	cep = sk_to_cep(sk);
	if (!cep) {
		/* Warning disabled. It is not present in upstream */
		/* WARN_ON(1); */
		read_unlock(&sk->sk_callback_lock);
		return;
	}
	orig_state_change = cep->sk_state_change;

	s = sk->sk_socket;

	dprint_cep(DBG_CM, cep, "state: %s(%d), LQ=%d, LC=%d, sk: " dprint_ptr_str() ", sk_state: %d\n",
		   siw_cep_state_str(cep->state), cep->state,
		   !list_empty(&cep->listenq), !!cep->listen_cep, sk, sk->sk_state);

	switch (sk->sk_state) {

	case TCP_ESTABLISHED:
		if (cep->state == SIW_EPSTATE_CONNECTING) {
			/* [NVMESH-4440]: Avoid cancel_delayed_work in int ctx
			 * to prevent racing with queue_delayed_work.
			 * siw_cancel_connect_timer(cep); */
			siw_cm_queue_work(cep, SIW_CM_WORK_CONNECT);
		} else {
			/*
			* handle accepting socket as special case where only
			* new connection is possible
			*/
			siw_cm_queue_work(cep, SIW_CM_WORK_ACCEPT);
		}

		break;

	case TCP_CLOSE:
	case TCP_CLOSE_WAIT:
		/* Other side sent a FIN before we managed to accept.
		   Let later socket operations(accept/send/rcv) to handle the it the right way 
		   As there is no reason to close the listening socket itself(SIW_CM_WORK_PEER_CLOSE) */
		if (cep->state == SIW_EPSTATE_LISTENING) {
			dprint_cep(DBG_CM|DBG_ON, cep, "Ignore Listen socket state TCP_CLOSE_WAIT, sk:" dprint_ptr_str() " sk_udata:" dprint_ptr_str() " sk_state:%d\n",
					sk, sk->sk_user_data, sk->sk_state);
		}
		else {
			/* Opportunistically try to suspend TX in case this is a peer close */
			if (cep->qp) {
				struct siw_qp *qp = cep->qp;
				if (down_read_trylock(&qp->state_lock)) {
					unsigned long flags;
					/* We need to disable interrupts to prevent hard-lockup on the target
					 * (It calls siw_post_send from NVME IRQ context) */
					lock_sq_rxsave(qp, flags);
					qp->tx_ctx.tx_suspend = 1;
					unlock_sq_rxsave(qp, flags);
					up_read(&qp->state_lock);
				}
			}
			/* [NVMESH-4440]: Avoid cancel_delayed_work in int ctx
			 * to prevent racing with queue_delayed_work.
			 * siw_cancel_connect_timer(cep);
			 * siw_cancel_mpatimer(cep);
			 * siw_cancel_read_mpa_hdr(cep);*/
			siw_cm_queue_work(cep, SIW_CM_WORK_PEER_CLOSE);
		}

		break;

	default:
		dprint_cep(DBG_CM, cep, "Unexpected sock state %d\n", sk->sk_state);
	}
	read_unlock(&sk->sk_callback_lock);
	orig_state_change(sk);
}


static int kernel_bindconnect(struct socket *s,
			      struct sockaddr *laddr, struct sockaddr *raddr, int flags)
{
	int err; 
#if !KS_HAS_SOCK_SET_REUSEADDR
	int s_val = 1;
#endif
	size_t size = laddr->sa_family == AF_INET ?
		sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);

	/*
	 * XXX
	 * Tentative fix. Should not be needed but sometimes iwcm
	 * chooses ports in use
	 */

#if KS_HAS_SOCK_SET_REUSEADDR
	sock_set_reuseaddr(s->sk);
	err = 0;
#else
	err = kernel_setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char *)&s_val,
				sizeof s_val);
#endif
	if (err < 0) {
		dprint(DBG_CM, "setsockopt reuseaddr failed err=%d\n", err);
		goto done;
	}

	err = s->ops->bind(s, laddr, size);
	if (err < 0) {
		dprint(DBG_CM, "Bind error %d on IPv6 addr %pI6 (type %0x)\n", err,
		       &to_sockaddr_in6(*laddr).sin6_addr, ipv6_addr_type(&to_sockaddr_in6(*laddr).sin6_addr));
		goto done;
	}

	err = s->ops->connect(s, raddr, size, flags); //inet_stream_connect tcp_v4_connect
	if (err == -EINPROGRESS && connect_non_block)
		err = 0;
	if (err < 0) {
		dprint(DBG_CM, "Connect error %d\n", err);
		goto done;
	}

done:
	return err;
}


int siw_connect(struct iw_cm_id *id, struct iw_cm_conn_param *params)
{
	struct siw_dev	*sdev = siw_dev_ofa2siw(id->device);
	struct siw_qp	*qp;
	struct siw_cep	*cep = NULL;
	struct socket	*s = NULL;
	struct sockaddr *laddr = (struct sockaddr *)&id->local_addr, 
		*raddr = (struct sockaddr *)&id->remote_addr;
	bool v4 = true;
	u16		pd_len = params->private_data_len;
	int		rv, val;
	sock_setsockopt_val_t optval = SOCK_SETSOCKOPT_VAL_PTR(&val);
	char orig_ca_name[TCP_CA_NAME_MAX] = {};

	if (pd_len > MPA_MAX_PRIVDATA)
		return -EINVAL;
	
	if (params->ird > sdev->attrs.max_ird || 
		params->ord > sdev->attrs.max_ord)
		return -ENOMEM;
	
	if (laddr->sa_family == AF_INET6)
		v4 = false;
	else if (laddr->sa_family != AF_INET)
		return -EAFNOSUPPORT;

	qp = siw_qp_id2obj(sdev, params->qpn);
	BUG_ON(!qp);

	dprint(DBG_CM, "(id=" dprint_ptr_str() ", QP%d): dev(id)=%s, netdev=%s\n",
		id, QP_ID(qp), sdev->ofa_dev.name, sdev->netdev->name);
	if (v4)
		dprint(DBG_CM, "(id=" dprint_ptr_str() ", QP%d): pd_len %d, laddr %pI4 %d, raddr %pI4 %d\n", 
		       id, QP_ID(qp), pd_len,
		       &((struct sockaddr_in *)(laddr))->sin_addr, 
		       ntohs(((struct sockaddr_in *)(laddr))->sin_port), 
		       &((struct sockaddr_in *)(raddr))->sin_addr, 
		       ntohs(((struct sockaddr_in *)(raddr))->sin_port));
	else {
		dprint(DBG_CM, "(id=" dprint_ptr_str() ", QP%d): pd_len %d, laddr %pI6 %d, raddr %pI6 %d\n",
		       id, QP_ID(qp), pd_len,
		       &((struct sockaddr_in6 *)(laddr))->sin6_addr, 
		       ntohs(((struct sockaddr_in6 *)(laddr))->sin6_port), 
		       &((struct sockaddr_in6 *)(raddr))->sin6_addr, 
		       ntohs(((struct sockaddr_in6 *)(raddr))->sin6_port));
	}

	rv = sock_create(v4 ? AF_INET : AF_INET6, SOCK_STREAM, IPPROTO_TCP, &s);
	if (rv < 0)
		goto error;

#if KS_HAS_SOCK_NOT_OWNED_BY_ME
	{
		struct sock 	*sk = s->sk;
		struct net      *net = sock_net(sk);

		/* Prevent inet_csk_clear_xmit_timers_sync being called from tcp_close()
		 * Fixes [NVMESH-5532] */
		sk->sk_net_refcnt = 1;
		get_net(net);
	}
#endif

	if (qp->kernel_verbs) {
		/* Causes EFAULT if called from user-space via syscall
		 * Why? Because sock_setsockopt assumes that optval is a user-space ptr
		 * because of the context and it isn't. */
		val = (int)sock_buff_sz;
		rv = sock_setsockopt(s, SOL_SOCKET, SO_SNDBUF,
				optval, sizeof(val));
		if (rv < 0) {
			pr_err("Error %d setting SO_SNDBUF\n", rv);
			goto error;
		}
		rv = sock_setsockopt(s, SOL_SOCKET, SO_RCVBUF,
				optval, sizeof(val));
		if (rv < 0) {
			pr_err("Error %d setting SO_SNDBUF\n", rv);
			goto error;
		}
	}

	/*
	 * NOTE: For simplification, connect() is called in blocking
	 * mode. Might be reconsidered for async connection setup at
	 * TCP level.
	 */
	if (!connect_non_block) {
		rv = kernel_bindconnect(s, laddr, raddr, 0);
		if (rv < 0) {
			dprint(DBG_CM, "(id=" dprint_ptr_str() ", QP%d): kernel_bindconnect: rv=%d\n",
				id, QP_ID(qp), rv);
			goto error;
		}

		rv = siw_sock_nodelay(s, orig_ca_name, sizeof(orig_ca_name));
		if (rv < 0) {
			dprint(DBG_CM, "(id=" dprint_ptr_str() ", QP%d): siw_sock_nodelay(): rv=%d\n",
				id, QP_ID(qp), rv);
			goto error;
		}

#if KS_HAS_SO_INCOMING_CPU
		if (qp->kernel_verbs) {
			/* Set the RX CPU from the RCQ interrupt vector */
			if (use_so_incoming_cpu && qp->rcq) {
				int rx_cpu = qp->rcq->comp_vector + comp_vector_cpu0;
				sock_setsockopt_val_t optval = SOCK_SETSOCKOPT_VAL_PTR(&rx_cpu);
				rv = sock_setsockopt(s, SOL_SOCKET, SO_INCOMING_CPU,
						optval, sizeof(rx_cpu));
				if (rv < 0)
					pr_err("Error %d setting SO_INCOMING_CPU\n", rv);
				else
					dprint(DBG_CM|DBG_ON, "(id=" dprint_ptr_str() ", QP%d): set SO_INCOMING_CPU to %d\n", id, QP_ID(qp), rx_cpu);
			}
		}
#endif
	}

	cep = siw_cep_alloc(sdev);
	if (!cep) {
		rv =  -ENOMEM;
		goto error;
	}
	siw_cep_set_inuse(cep);

	/* Associate QP with CEP */
	siw_cep_get(cep);
	qp->cep = cep;

	/* siw_qp_get(qp) already done by QP lookup */
	cep->qp = qp;

	id->add_ref(id);
	cep->cm_id = id;

	dprint_cep(DBG_CM, cep, "(id=" dprint_ptr_str() ", QP%d)\n",
		   id, QP_ID(qp));

	rv = siw_cm_alloc_work(cep, SIW_CM_NON_LISTENING_CEP_WORKS);
	if (rv != 0) {
		rv = -ENOMEM;
		goto error;
	}
	cep->ird = params->ird;
	cep->ord = params->ord;
	siw_cep_state_change(cep, SIW_EPSTATE_CONNECTING); /* siw-connect */

	dprint_cep(DBG_CM, cep, "pd_len = %u\n", pd_len);

	/*
	 * Associate CEP with socket
	 */
	siw_cep_socket_assoc(cep, s);
	
	if (connect_non_block) {
#if SIW_CONNECT_FAIL_TEST
		do {
			static int fail_count = 0;
			if ((++fail_count) % SIW_CONNECT_FAIL_TEST_N == 0) {
				dprint_cep(DBG_CM, cep, "(id=" dprint_ptr_str() ", QP%d): FAIL TEST: fail_count=%d\n",
				id, QP_ID(qp), fail_count);
				rv = -ECONNABORTED;
				goto error;
			}
		} while(0);
#endif

		cep->mpa.pdata = kmemdup(params->private_data, params->private_data_len, GFP_KERNEL);
		if (!cep->mpa.pdata) {
			dprint_cep(DBG_CM, cep, "(id=" dprint_ptr_str() ", QP%d): OOM\n",
				   id, QP_ID(qp));
			rv = -ENOMEM;
			goto error;
		}
		cep->mpa.hdr.params.pd_len = params->private_data_len;

		/* [NVMESH-3371]: Moved this to before we call kernel_bindconnect
		 * in case the state-change callback happens before we finish the function.
		 * If kernel_bindconnect fails, we cancel the delayed work */
		rv = siw_cm_queue_work(cep, SIW_CM_WORK_CONNECT_TIMEOUT);
		if (rv < 0) {
			dprint_cep(DBG_CM, cep, "(id=" dprint_ptr_str() ", QP%d): siw_cm_queue_work Failed: rv=%d\n",
				   id, QP_ID(qp), rv);
			goto error;
		}

		rv = kernel_bindconnect(s, laddr, raddr, O_NONBLOCK);
		if (rv < 0) {
			siw_cancel_connect_timer(cep);
			dprint_cep(DBG_CM, cep, "(id=" dprint_ptr_str() ", QP%d): kernel_bindconnect: rv=%d\n",
				   id, QP_ID(qp), rv);
			goto error;
		}

		if (!rv) {
			dprint_cep(DBG_CM, cep, "(id=" dprint_ptr_str() ", QP%d): Exit\n",
				   id, QP_ID(qp));
			siw_cep_set_free(cep);
			return 0;
		}
		goto error;
	} else {
		/* !connect_non_block */
		memcpy(cep->orig_ca_name, orig_ca_name, sizeof(cep->orig_ca_name));
	}

	siw_cep_state_change(cep, SIW_EPSTATE_AWAIT_MPAREP); /* siw-connect */

	/*
	 * Set MPA Request bits: CRC if required, no MPA Markers,
	 * MPA Rev. 1, Key 'Request'.
	 */
	cep->mpa.hdr.params.bits = 0;
	__mpa_rr_set_revision(&cep->mpa.hdr.params.bits, MPA_REVISION_1);

	if (mpa_crc_required)
		cep->mpa.hdr.params.bits |= MPA_RR_FLAG_CRC;
	
	/* Let the acceptor know we support write acknowledgements */
	cep->mpa.hdr.params.bits |= MPA_RR_FLAG_WR_ACK;

	memcpy(cep->mpa.hdr.key, MPA_KEY_REQ, 16);

	rv = siw_send_mpareqrep(cep, params->private_data, pd_len);

#if SIW_CONNECT_FAIL_TEST
	if (!connect_non_block) {
		static int fail_count = 0;
		if ((++fail_count) % SIW_CONNECT_FAIL_TEST_N == 0) {
			dprint_cep(DBG_CM, cep, "(id=" dprint_ptr_str() ", QP%d): FAIL TEST: fail_count=%d\n",
			       id, QP_ID(qp), fail_count);
			rv = -ENOMEM;
			goto error;
		}
	}
#endif

	/*
	 * Reset private data.
	 */
	cep->mpa.hdr.params.pd_len = 0;

	if (rv >= 0) {
		rv = siw_cm_queue_work(cep, SIW_CM_WORK_MPATIMEOUT);
		if (!rv) {
			dprint_cep(DBG_CM, cep, "(id=" dprint_ptr_str() " QP%d): Exit\n",
				id, QP_ID(qp));
			siw_cep_set_free(cep);
			return 0;
		}
	}
error:
	dprint_cep(DBG_CM, cep, " Failed: %d\n", rv);

	if (cep) {
		siw_cep_socket_restore_ca(cep);
		siw_socket_disassoc(s);
		sock_release(s);
		cep->llp.sock = NULL;

		cep->qp = NULL;
		siw_qp_put(qp);

		cep->cm_id = NULL;
		id->rem_ref(id);

#if 0
		/* [Jared]: This extra put is a bug (tested also with connect_non_block = false), 
		 * but it is in upstream so leaving it here for now */
		siw_cep_put(cep);
#endif

		qp->cep = NULL;
		siw_cep_put(cep);

		siw_cep_state_change(cep, SIW_EPSTATE_CLOSED); /* siw-connect err */

		siw_cep_set_free(cep);

		siw_cep_put(cep);
	} else {
		if (s)
			sock_release(s);

		siw_qp_put(qp);
	}

	return rv;
}

/*
 * siw_accept - Let SoftiWARP accept an RDMA connection request
 *
 * @id:		New connection management id to be used for accepted
 *		connection request
 * @params:	Connection parameters provided by ULP for accepting connection
 *
 * Transition QP to RTS state, associate new CM id @id with accepted CEP
 * and get prepared for TCP input by installing socket callbacks.
 * Then send MPA Reply and generate the "connection established" event.
 * Socket callbacks must be installed before sending MPA Reply, because
 * the latter may cause a first RDMA message to arrive from the RDMA Initiator
 * side very quickly, at which time the socket callbacks must be ready.
 */
int siw_accept(struct iw_cm_id *id, struct iw_cm_conn_param *params)
{
	struct siw_dev		*sdev = siw_dev_ofa2siw(id->device);
	struct siw_cep		*cep = (struct siw_cep *)id->provider_data;
	struct siw_qp		*qp;
	struct siw_qp_attrs	qp_attrs;
	int rv;

	siw_cep_set_inuse(cep);

	/* Free lingering inbound private data */
	if (cep->mpa.hdr.params.pd_len) {
		cep->mpa.hdr.params.pd_len = 0;
		kfree(cep->mpa.pdata);
		cep->mpa.pdata = NULL;
	}
	siw_cancel_mpatimer(cep);

	if (cep->state != SIW_EPSTATE_RECVD_MPAREQ) {
		switch (cep->state) {
		case SIW_EPSTATE_CLOSED:
			dprint_cep(DBG_CM | DBG_ON, cep, "Already closed\n");

			siw_cep_set_free(cep);
			siw_cep_put(cep); /* should be last reference(siw_proc_mpareq) */
			return -ECONNRESET;
		case SIW_EPSTATE_REJECTING:
		case SIW_EPSTATE_REJECTED:
			dprint_cep(DBG_CM | DBG_ON, cep, "Already rejected\n");
			/* No need to do siw_cep_put. Already done by siw_reject */
			siw_cep_set_free(cep);
			return -ECONNABORTED;
		default:
			dprint_cep(DBG_CM | DBG_ON, cep, "Unexpected state: %d\n", cep->state);

			BUG();

			return -EINVAL;
		}
	}

	siw_cep_put(cep); /* siw_proc_mpareq */

	qp = siw_qp_id2obj(sdev, params->qpn);
	BUG_ON(!qp); /* The OFA core should prevent this */

	write_lock_qp(qp);
	if (qp->attrs.state > SIW_QP_STATE_RTR) {
		rv = -EINVAL;
		do {
			int state_lock_failed;
			if ((state_lock_failed = atomic_xchg(&qp->state_lock_failed, 0))) {
				dprint(DBG_CM|DBG_ON, "QP(%d): state_lock_failed %d for siw_qp " dprint_ptr_str() "",
						QP_ID(qp), state_lock_failed, qp);
				WARN_ON(1);
			}
		} while(0);
		write_unlock_qp(qp);
		goto error;
	}

	dprint_cep(DBG_CM, cep, "(id=" dprint_ptr_str() ", QP%d): dev(id)=%s\n",
		id, QP_ID(qp), sdev->ofa_dev.name);

	if (params->ord > sdev->attrs.max_ord ||
	    params->ird > sdev->attrs.max_ord) {
		dprint_cep(DBG_CM|DBG_ON, cep, "(id=" dprint_ptr_str() ", QP%d): "
			"ORD: %d (max: %d), IRD: %d (max: %d)\n",
			id, QP_ID(qp),
			params->ord, qp->attrs.orq_size,
			params->ird, qp->attrs.irq_size);
		rv = -EINVAL;
		do {
			int state_lock_failed;
			if ((state_lock_failed = atomic_xchg(&qp->state_lock_failed, 0))) {
				dprint(DBG_CM|DBG_ON, "QP(%d): state_lock_failed %d for siw_qp " dprint_ptr_str() "",
						QP_ID(qp), state_lock_failed, qp);
				WARN_ON(1);
			}
		} while(0);
		write_unlock_qp(qp);
		goto error;
	}
	if (params->private_data_len > MPA_MAX_PRIVDATA) {
		dprint_cep(DBG_CM|DBG_ON, cep, "(id=" dprint_ptr_str() ", QP%d): "
			"Private data too long: %d (max: %d)\n",
			id, QP_ID(qp),
			params->private_data_len, MPA_MAX_PRIVDATA);
		rv =  -EINVAL;
		do {
			int state_lock_failed;
			if ((state_lock_failed = atomic_xchg(&qp->state_lock_failed, 0))) {
				dprint(DBG_CM|DBG_ON, "QP(%d): state_lock_failed %d for siw_qp " dprint_ptr_str() "",
						QP_ID(qp), state_lock_failed, qp);
				WARN_ON(1);
			}
		} while(0);
		write_unlock_qp(qp);
		goto error;
	}
	cep->cm_id = id;
	id->add_ref(id);

	memset(&qp_attrs, 0, sizeof qp_attrs);
	qp_attrs.orq_size = params->ord;
	qp_attrs.irq_size = params->ird;
	qp_attrs.llp_stream_handle = cep->llp.sock;

	/*
	 * Currently no MPA markers support. Consider adding marker TX path.
	 */
	qp_attrs.mpa.marker_rcv = 0;
	qp_attrs.mpa.marker_snd = 0;
	qp_attrs.mpa.crc = cep->mpa.hdr.params.bits & MPA_RR_FLAG_CRC ? 1 : 0;
	qp_attrs.mpa.wr_ack = cep->mpa.hdr.params.bits & MPA_RR_FLAG_WR_ACK ? 1 : 0;
	qp_attrs.state = SIW_QP_STATE_RTS;

	dprint_cep(DBG_CM, cep, "(id=" dprint_ptr_str() ", QP%d): Moving to RTS\n", id, QP_ID(qp));

	/* Associate QP with CEP */
	siw_cep_get(cep);
	qp->cep = cep;

	/* siw_qp_get(qp) already done by QP lookup */
	cep->qp = qp;

	siw_cep_state_change(cep, SIW_EPSTATE_RDMA_MODE); /* siw-accept */

	/* Move socket RX/TX under QP control */
	rv = siw_qp_modify(qp, &qp_attrs, SIW_QP_ATTR_STATE|
					  SIW_QP_ATTR_LLP_HANDLE|
					  SIW_QP_ATTR_ORD|
					  SIW_QP_ATTR_IRD|
					  SIW_QP_ATTR_MPA);
	do {
		int state_lock_failed;
		if ((state_lock_failed = atomic_xchg(&qp->state_lock_failed, 0))) {
			dprint(DBG_CM|DBG_ON, "QP(%d): state_lock_failed %d for siw_qp " dprint_ptr_str() "",
					QP_ID(qp), state_lock_failed, qp);
			WARN_ON(1);
		}
	} while(0);
	write_unlock_qp(qp);

	if (rv)
		goto error;

#if KS_HAS_SO_INCOMING_CPU
	/* Set the RX CPU from the RCQ interrupt vector */
	if (use_so_incoming_cpu && qp->rcq) {
		int rx_cpu = qp->rcq->comp_vector + comp_vector_cpu0;
		sock_setsockopt_val_t optval = SOCK_SETSOCKOPT_VAL_PTR(&rx_cpu);
		rv = sock_setsockopt(qp->attrs.llp_stream_handle, SOL_SOCKET, SO_INCOMING_CPU,
				     optval, sizeof(rx_cpu));
		if (rv < 0)
			pr_err("Error %d setting SO_INCOMING_CPU\n", rv);
		else
			dprint(DBG_CM|DBG_ON, "(id=" dprint_ptr_str() ", QP%d): set SO_INCOMING_CPU to %d\n", id, QP_ID(qp), rx_cpu);
	}
#endif

	dprint_cep(DBG_CM, cep, "(id=" dprint_ptr_str() ", QP%d): %d bytes private_data\n",
			id, QP_ID(qp), params->private_data_len);

	dprint_cep(DBG_CM, cep, "(id=" dprint_ptr_str() ", QP%d): Sending MPA Reply\n", id, QP_ID(qp));

	rv = siw_send_mpareqrep(cep, params->private_data,
				params->private_data_len);

	if (!rv) {
		rv = siw_cm_upcall(cep, IW_CM_EVENT_ESTABLISHED, 0);
		if (rv)
			goto error;

		siw_cep_set_free(cep);

		dprint_cep(DBG_CM, cep, "(id=" dprint_ptr_str() ", QP%d): Exit\n", id, QP_ID(qp));
		return 0;
	}

error:
	siw_cep_socket_restore_ca(cep);
	siw_socket_disassoc(cep->llp.sock);
	sock_release(cep->llp.sock);
	cep->llp.sock = NULL;

	siw_cep_state_change(cep, SIW_EPSTATE_CLOSED); /* siw-accept err */

	if (cep->cm_id) {
		cep->cm_id->rem_ref(id);
		cep->cm_id = NULL;
	}
	if (qp->cep) {
		siw_cep_put(cep);
		qp->cep = NULL;
	}
	cep->qp = NULL;
	siw_qp_put(qp);

	siw_cep_set_free(cep);
	siw_cep_put(cep);

	return rv;
}

/*
 * siw_reject()
 *
 * Local connection reject case. Send private data back to peer,
 * close connection and dereference connection id.
 */
int siw_reject(struct iw_cm_id *id, const void *pdata, u8 plen)
{
	struct siw_cep	*cep = (struct siw_cep *)id->provider_data;

	siw_cep_set_inuse(cep);

	siw_cancel_mpatimer(cep);

	dprint_cep(DBG_CM, cep, "cep->state=%d\n", cep->state);

	if (cep->state != SIW_EPSTATE_RECVD_MPAREQ) {
		switch (cep->state) {
		case SIW_EPSTATE_CLOSED:

			dprint_cep(DBG_CM, cep, "Already closed\n");

			siw_cep_set_free(cep);
			siw_cep_put(cep); /* should be last reference(siw_proc_mpareq) */

			return -ECONNRESET;
		case SIW_EPSTATE_REJECTING:
		case SIW_EPSTATE_REJECTED:
			dprint_cep(DBG_CM, cep, "Already rejected\n");

			siw_cep_set_free(cep);
			/* No need to do siw_cep_put. Already done by previous call to siw_reject */

			return -ECONNABORTED;
		default:
			dprint_cep(DBG_CM | DBG_ON, cep, "Unexpected state: %d\n", cep->state);
			BUG();
			return -EINVAL;
		}
	}
	siw_cep_put(cep); /* siw_proc_mpareq */

	cep->reject_pdata = kmemdup(pdata, plen, GFP_KERNEL);
	cep->reject_plen = plen;

	if (!cep->reject_pdata) {
		dprint_cep(DBG_CM|DBG_ON, cep, "OOM");
		siw_cep_set_free(cep);
		return -ENOMEM;
	}

	siw_cep_state_change(cep, SIW_EPSTATE_REJECTING);

	dprint_cep(DBG_CM | DBG_ON, cep, "Sending reject with %d bytes of pdata: %*ph",
	       cep->reject_plen, cep->reject_plen, cep->reject_pdata);

	siw_cep_set_free(cep);

	/* serialize the job using the queue to prevent a 
	race with siw_accept_newconn */
	if(siw_cm_queue_work(cep, SIW_CM_WORK_REJECT_CONNECTION))
		WARN_ON(1);

	return 0;
}

static int siw_listen_address(struct iw_cm_id *id, int backlog,
			      struct sockaddr *laddr, int addr_family, int bound_dev_if, int listen_any_addr)
{
	struct socket		*s;
	struct siw_cep		*cep = NULL;
	int			rv = 0, s_val __attribute__((unused));
	struct siw_listener_cm_id_provider_data_any_addr *provider_data;

	rv = sock_create(addr_family, SOCK_STREAM, IPPROTO_TCP, &s);
	if (rv < 0) {
		dprint_cep(DBG_CM|DBG_ON, cep, "(id=" dprint_ptr_str() "): ERROR: "
			"sock_create(): rv=%d\n", id, rv);
		return rv;
	}

	/*
	 * Probably to be removed later. Allows binding
	 * local port when still in TIME_WAIT from last close.
	 */
	s_val = 1;

#if KS_HAS_SOCK_SET_REUSEADDR
	sock_set_reuseaddr(s->sk);
	rv = 0;
#else
	rv = kernel_setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char *)&s_val,
				sizeof s_val);
#endif
	if (rv != 0) {
		dprint_cep(DBG_CM|DBG_ON, cep, "(id=" dprint_ptr_str() "): ERROR: "
			"kernel_setsockopt(): rv=%d\n", id, rv);
		goto error;
	}
	s->sk->sk_bound_dev_if = bound_dev_if;
	rv = s->ops->bind(s, laddr, addr_family == AF_INET ?
		sizeof(struct sockaddr_in) :
		sizeof(struct sockaddr_in6));
	if (rv != 0) {
		dprint_cep(DBG_CM|DBG_ON, cep, "(id=" dprint_ptr_str() "): ERROR: bind(): rv=%d\n",
			id, rv);
		goto error;
	}

	cep = siw_cep_alloc(siw_dev_ofa2siw(id->device));
	if (!cep) {
		rv = -ENOMEM;
		goto error;
	}
	siw_cep_socket_assoc(cep, s);

	rv = siw_cm_alloc_work(cep, backlog);
	if (rv != 0) {
		dprint_cep(DBG_CM|DBG_ON, cep, "(id=" dprint_ptr_str() "): ERROR: "
			"siw_cm_alloc_work(backlog=%d): rv=%d\n",
			id, backlog, rv);
		goto error;
	}

	rv = s->ops->listen(s, backlog);
	if (rv != 0) {
		dprint_cep(DBG_CM|DBG_ON, cep, "(id=" dprint_ptr_str() "): ERROR: listen() rv=%d\n",
			id, rv);
		goto error;
	}

	cep->cm_id = id;
	id->add_ref(id);

	memcpy(&cep->llp.listen_addr, laddr, addr_family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6));

	/*
	 * In case of a wildcard rdma_listen on a multi-homed device,
	 * a listener's IWCM id is associated with more than one listening CEP.
	 *
	 * We currently use id->provider_data in three different ways:
	 *
	 * o For a listener's IWCM id, id->provider_data points to
	 *   the list_head of the list of listening CEPs.
	 *   Uses: siw_create_listen(), siw_destroy_listen()
	 *
	 * o For a passive-side IWCM id, id->provider_data points to
	 *   the CEP itself. This is a consequence of
	 *   - siw_cm_upcall() setting event.provider_data = cep and
	 *   - the IWCM's cm_conn_req_handler() setting provider_data of the
	 *     new passive-side IWCM id equal to event.provider_data
	 *   Uses: siw_accept(), siw_reject()
	 *
	 * o For an active-side IWCM id, id->provider_data is not used at all.
	 *
	 */
	provider_data = id->provider_data;
	if (!provider_data) {
		BUILD_BUG_ON(offsetof(typeof(*provider_data), common) != 0);
		if (!listen_any_addr || bound_dev_if)
			provider_data = kmalloc(sizeof(provider_data->common), GFP_KERNEL);
		else
			provider_data = kmalloc(sizeof(*provider_data), GFP_KERNEL);
		if (!provider_data) {
			rv = -ENOMEM;
			goto error;
		}
		INIT_LIST_HEAD(&provider_data->common.cep_list_head);
		provider_data->common.listen_any_addr = listen_any_addr;
		provider_data->common.bound_dev_if = bound_dev_if;
		provider_data->common.addr_family = addr_family;
		id->provider_data = provider_data;

		if (listen_any_addr && !bound_dev_if) {
			struct net_device *netdev = siw_dev_ofa2siw(id->device)->netdev;
			INIT_LIST_HEAD(&provider_data->listen_any_entry);
			id->add_ref(id);
			provider_data->cm_id = id;
			dev_hold(netdev);
			provider_data->netdev = netdev;
			provider_data->backlog = backlog;
			if (addr_family == AF_INET)
				provider_data->port = to_sockaddr_in(*laddr).sin_port;
			else
				provider_data->port = to_sockaddr_in6(*laddr).sin6_port;
			mutex_lock(&listen_any_guard);
			list_add_tail(&provider_data->listen_any_entry, &listen_any_list);
			mutex_unlock(&listen_any_guard);
		}
	}

	list_add_tail(&cep->listenq, &provider_data->common.cep_list_head);
	siw_cep_state_change(cep, SIW_EPSTATE_LISTENING); /* siw-create-listen */
	
	if (addr_family == AF_INET)
		dprint_cep(DBG_CM, cep, "(id=" dprint_ptr_str() "): dev(id)=%s, netdev=%s, "
			"id->provider_data=" dprint_ptr_str() ",, listen_addr=%08x:%x bound_dev_if=%d listen_any_addr=%d\n",
			id, id->device->name, siw_dev_ofa2siw(id->device)->netdev->name,
			id->provider_data, be32_to_cpu(to_sockaddr_in(*laddr).sin_addr.s_addr), 
			be16_to_cpu(to_sockaddr_in(*laddr).sin_port), bound_dev_if, listen_any_addr);
	else
		dprint_cep(DBG_CM, cep, "(id=" dprint_ptr_str() "): dev(id)=%s, netdev=%s, "
			"id->provider_data=" dprint_ptr_str() ", listen_addr=" dprint_ip6_str() ":%u bound_dev_if=%d listen_any_addr=%d\n",
			id, id->device->name, siw_dev_ofa2siw(id->device)->netdev->name,
			id->provider_data, dprint_ip6_param(to_sockaddr_in6(*laddr).sin6_addr), 
			be16_to_cpu(to_sockaddr_in6(*laddr).sin6_port), bound_dev_if, listen_any_addr);
	return 0;

error:
	dprint_cep(DBG_CM, cep, "Failed: %d\n", rv);

	if (cep) {
		siw_cep_set_inuse(cep);

		if (cep->cm_id) {
			cep->cm_id->rem_ref(cep->cm_id);
			cep->cm_id = NULL;
		}
		siw_cep_socket_restore_ca(cep);
		cep->llp.sock = NULL;
		siw_socket_disassoc(s);
		siw_cep_state_change(cep, SIW_EPSTATE_CLOSED); /* siw-listen err */

		siw_cep_set_free(cep);
		siw_cep_put(cep);
	}
	sock_release(s);

	return rv;
}

static void siw_drop_listener_cep(struct siw_cep *cep)
{
	dprint_cep(DBG_CM, cep, "(id=" dprint_ptr_str() "): drop, state %d\n",
	       cep->cm_id, cep->state);
	siw_cep_set_inuse(cep);
	
	if (cep->cm_id) {
		cep->cm_id->rem_ref(cep->cm_id);
		cep->cm_id = NULL;
	}
	if (cep->llp.sock) {
		siw_cep_socket_restore_ca(cep);
		siw_socket_disassoc(cep->llp.sock);
		sock_release(cep->llp.sock);
		cep->llp.sock = NULL;
	}
	cep->state = SIW_EPSTATE_CLOSED;
	siw_cep_set_free(cep);
	siw_cep_put(cep);
}

static void siw_drop_listeners(struct iw_cm_id *id)
{
	struct list_head	*p, *tmp;
	struct siw_listener_cm_id_provider_data_common *provider_data = id->provider_data;

	/*
	 * In case of a wildcard rdma_listen on a multi-homed device,
	 * a listener's IWCM id is associated with more than one listening CEP.
	 */
	list_for_each_safe(p, tmp, &provider_data->cep_list_head) {

		struct siw_cep *cep = list_entry(p, struct siw_cep, listenq);
		list_del(p);

		siw_drop_listener_cep(cep);
	}
}

/*
 * siw_create_listen - Create resources for a listener's IWCM ID @id
 *
 * Listens on the socket addresses id->local_addr and id->remote_addr.
 *
 * If the listener's @id provides a specific local IP address, at most one
 * listening socket is created and associated with @id.
 *
 * If the listener's @id provides the wildcard (zero) local IP address,
 * a separate listen is performed for each local IP address of the device
 * by creating a listening socket and binding to that local IP address.
 *
 */
int siw_create_listen(struct iw_cm_id *id, int backlog)
{
	struct ib_device	*ofa_dev = id->device;
	struct siw_dev		*sdev = siw_dev_ofa2siw(ofa_dev);
	int			rv = 0, listeners = 0;

	dprint(DBG_CM, "(id=" dprint_ptr_str() "): dev(id)=%s, netdev=%s backlog=%d\n",
		id, ofa_dev->name, sdev->netdev->name, backlog);

	/*
	 * IPv4/v6 design differences regarding multi-homing
	 * propagate up to iWARP:
	 * o For IPv4, use sdev->netdev->ip_ptr
	 * o For IPv6, use sdev->netdev->ipv6_ptr
	 */
	if (id->local_addr.ss_family == AF_INET) {
		/* IPv4 */
#if SIW_LISTEN_USE_BOUND_DEV_IF
		struct sockaddr_in	*laddr = &to_sockaddr_in(id->local_addr);
		int bound_dev_if = 0;

		dprint(DBG_CM, "(id=" dprint_ptr_str() "): listen on laddr %pI4:%d\n", id,
			&laddr->sin_addr, ntohs(laddr->sin_port));

		if (ipv4_is_zeronet(laddr->sin_addr.s_addr))
			bound_dev_if = sdev->netdev->ifindex;
		rv = siw_listen_address(id, backlog,
					(struct sockaddr *)laddr,
					AF_INET, bound_dev_if,
					ipv4_is_zeronet(laddr->sin_addr.s_addr));
		if (!rv)
			listeners++;
		else
			dprint(DBG_CM, "(id=" dprint_ptr_str() "): siw_listen_address failed, rv=%d\n", id, rv);
		goto out;
#else
		/* DEPRECATED */
		struct sockaddr_in	laddr = to_sockaddr_in(id->local_addr);
		u8			*l_ip, *r_ip;
		struct in_device	*in_dev;

		l_ip = (u8 *) &to_sockaddr_in(id->local_addr).sin_addr.s_addr;
		r_ip = (u8 *) &to_sockaddr_in(id->remote_addr).sin_addr.s_addr;
		dprint(DBG_CM, "(id=" dprint_ptr_str() "): "
			"laddr(id)  : ipv4=%d.%d.%d.%d, port=%d; "
			"raddr(id)  : ipv4=%d.%d.%d.%d, port=%d\n",
			id,
			l_ip[0], l_ip[1], l_ip[2], l_ip[3],
			ntohs(to_sockaddr_in(id->local_addr).sin_port),
			r_ip[0], r_ip[1], r_ip[2], r_ip[3],
			ntohs(to_sockaddr_in(id->remote_addr).sin_port));
		in_dev = in_dev_get(sdev->netdev);
		if (!in_dev) {
			dprint(DBG_CM|DBG_ON, "(id=" dprint_ptr_str() "): "
				"netdev has no in_device\n", id);
			return -ENODEV;
		}

		for_ifa(in_dev) {
			/*
			 * Create a listening socket if id->local_addr
			 * contains the wildcard IP address OR
			 * the IP address of the interface.
			 */
			if (ipv4_is_zeronet(
			    to_sockaddr_in(id->local_addr).sin_addr.s_addr) ||
			    to_sockaddr_in(id->local_addr).sin_addr.s_addr ==
			    ifa->ifa_address) {
				laddr.sin_addr.s_addr = ifa->ifa_address;

				l_ip = (u8 *) &laddr.sin_addr.s_addr;
				dprint(DBG_CM, "(id=" dprint_ptr_str() "): "
					"laddr(bind): ipv4=%d.%d.%d.%d,"
					" port=%d\n", id,
					l_ip[0], l_ip[1], l_ip[2],
					l_ip[3], ntohs(laddr.sin_port));

				rv = siw_listen_address(id, backlog,
						(struct sockaddr *)&laddr,
						AF_INET, 0,
						ipv4_is_zeronet(to_sockaddr_in(id->local_addr).sin_addr.s_addr));
				if (!rv)
					listeners++;
				else
					dprint(DBG_CM, "(id=" dprint_ptr_str() "): siw_listen_address failed, rv=%d\n", id, rv);
			}
			else
				dprint(DBG_CM, "Not creating listening socket\n");
		}
		endfor_ifa(in_dev);
		in_dev_put(in_dev);
		if (rv && id->provider_data)
			siw_drop_listeners(id);
#endif
	} else if (id->local_addr.ss_family == AF_INET6) {
#if SIW_LISTEN_USE_BOUND_DEV_IF
		struct sockaddr_in6 *laddr = &to_sockaddr_in6(id->local_addr);
		int bound_dev_if = 0;

		dprint(DBG_CM, "(id=" dprint_ptr_str() "): listen on addr %pI6:%d\n", id,
		       &laddr->sin6_addr, ntohs(laddr->sin6_port));

		/* For wildcard addr, limit binding to current device only */
		if (ipv6_addr_any(&laddr->sin6_addr))
			bound_dev_if = sdev->netdev->ifindex;
		rv = siw_listen_address(id, backlog,
					(struct sockaddr *)laddr,
					AF_INET6, bound_dev_if,
					ipv6_addr_any(&laddr->sin6_addr));
		if (!rv)
			listeners++;
		else
			dprint(DBG_CM, "(id=" dprint_ptr_str() "): siw_listen_address on laddr %pI6 failed, rv=%d\n", 
			       id, &laddr->sin6_addr, rv);
		goto out;
#else
		/* DEPRECATED */
		struct inet6_dev *in6_dev = in6_dev_get(sdev->netdev);
		struct inet6_ifaddr *ifp;
		struct sockaddr_in6 *s_laddr = &to_sockaddr_in6(id->local_addr),
		*s_raddr = &to_sockaddr_in6(id->remote_addr);
		
		if (!in6_dev) {
			rv = -ENODEV;
			goto out;
		}
		dprint(DBG_CM, "(id=" dprint_ptr_str() "): laddr %pI6:%d, raddr %pI6:%d\n", id,
			&s_laddr->sin6_addr, ntohs(s_laddr->sin6_port),
			&s_raddr->sin6_addr, ntohs(s_raddr->sin6_port));

		rtnl_lock();
		list_for_each_entry(ifp, &in6_dev->addr_list, if_list) {
			if (ifp->flags & (IFA_F_TENTATIVE | IFA_F_DEPRECATED))
				continue;
			if (ipv6_addr_any(&s_laddr->sin6_addr) ||
				ipv6_addr_equal(&s_laddr->sin6_addr, &ifp->addr)) {
				struct sockaddr_in6 bind_addr  = {
					.sin6_family = AF_INET6,
					.sin6_port = s_laddr->sin6_port,
					.sin6_flowinfo = 0,
					.sin6_addr = ifp->addr,
					.sin6_scope_id = sdev->netdev->ifindex };
					
				rv = siw_listen_address(id, backlog,
							(struct sockaddr *)&bind_addr,
							AF_INET6, 0, ipv6_addr_any(&s_laddr->sin6_addr));
				if (!rv)
					listeners++;
				else
					dprint(DBG_CM, "(id=" dprint_ptr_str() "): siw_listen_address on laddr %pI6 failed, rv=%d\n", 
					       id, &bind_addr.sin6_addr, rv);
			}
		}
		rtnl_unlock();
		in6_dev_put(in6_dev);
#endif
	} else {
		rv = -EAFNOSUPPORT;
		dprint(DBG_CM|DBG_ON, "(id=" dprint_ptr_str() "): Unknown addr family %d\n", id, id->local_addr.ss_family);
	}
out:
	if (listeners)
		rv = 0;
	else if (!rv) {
		dprint(DBG_CM, "(id=" dprint_ptr_str() "): Failed to create any listeners\n", id);
		rv = -EINVAL;
	}

	if (!rv)
		dprint(DBG_CM, "(id=" dprint_ptr_str() "): Success\n", id);
	else
		dprint(DBG_CM, "(id=" dprint_ptr_str() "): Failed (%d)\n", id, rv);

	return rv;
}


int siw_destroy_listen(struct iw_cm_id *id)
{
	struct siw_listener_cm_id_provider_data_common *provider_data = id->provider_data;
	dprint(DBG_CM, "(id=" dprint_ptr_str() "): dev(id)=%s, netdev=%s\n",
		id, id->device->name,
		siw_dev_ofa2siw(id->device)->netdev->name);

	if (!id->provider_data) {
		/*
		 * TODO: See if there's a way to avoid getting any
		 *       listener ids without a list of CEPs
		 */
		dprint(DBG_CM, "(id=" dprint_ptr_str() "): Listener id: no CEP(s)\n", id);
		return 0;
	}
	
	if (provider_data->listen_any_addr && !provider_data->bound_dev_if) {
		struct siw_listener_cm_id_provider_data_any_addr *provider_data_any_addr = id->provider_data;
		mutex_lock(&listen_any_guard);
		list_del(&provider_data_any_addr->listen_any_entry);
		mutex_unlock(&listen_any_guard);
		
		BUG_ON(provider_data_any_addr->cm_id != id);
		id->rem_ref(id);
		provider_data_any_addr->cm_id = NULL;
		dev_put(provider_data_any_addr->netdev);
		provider_data_any_addr->netdev = NULL;
	}

	siw_drop_listeners(id);
	kfree(id->provider_data);
	id->provider_data = NULL;

	return 0;
}

static void sockaddr_addr_set(struct sockaddr *addr, __be32 in_addr, const struct in6_addr *in6_addr)
{
	if (addr->sa_family == AF_INET)
		to_sockaddr_in(*addr).sin_addr.s_addr = in_addr;
	else if (addr->sa_family == AF_INET6)
		to_sockaddr_in6(*addr).sin6_addr = *in6_addr;
	else
		BUG();
}

static void sockaddr_addr_to_str(char *str, int str_len, const struct sockaddr *addr)
{
	if (addr->sa_family == AF_INET)
		scnprintf(str,  str_len, "%pI4", &to_sockaddr_in(*addr).sin_addr.s_addr);
	else if (addr->sa_family == AF_INET6)
		scnprintf(str,  str_len, "%pI6c", &to_sockaddr_in6(*addr).sin6_addr);
	else
		BUG();
}

static void set_sockaddr_port(struct sockaddr *addr, __be16 port)
{
	if (addr->sa_family == AF_INET)
		to_sockaddr_in(*addr).sin_port = port;
	else if (addr->sa_family == AF_INET6)
		to_sockaddr_in6(*addr).sin6_port = port;
	else
		BUG();
}

static bool sockaddr_addr_equal(const struct sockaddr *addr, __be32 in_addr, const struct in6_addr *in6_addr)
{
	if (addr->sa_family == AF_INET)
		return to_sockaddr_in(*addr).sin_addr.s_addr == in_addr;
	else if (addr->sa_family == AF_INET6)
		return ipv6_addr_equal(&(to_sockaddr_in6(*addr).sin6_addr), in6_addr);
	BUG();
	return false;
}

void siw_cm_any_listeners_update(struct net_device *netdev, int addr_family,
				__be32 in_addr /* if addr_family == AF_INET */, 
				const struct in6_addr *in6_addr /* if addr_family == AF_INET6 */, bool add)
{
	struct siw_listener_cm_id_provider_data_any_addr *provider_data;
	LIST_HEAD(del_list_head);
	struct siw_cep *cep;
	int rv;
	struct sockaddr_storage laddr = {
		.ss_family = addr_family,
	};
	char addr_str[64] = "";

	sockaddr_addr_set((struct sockaddr *)&laddr, in_addr, in6_addr);
	sockaddr_addr_to_str(addr_str, sizeof(addr_str), (struct sockaddr *)&laddr);

	mutex_lock(&listen_any_guard);
	list_for_each_entry(provider_data, &listen_any_list, listen_any_entry) {
		if (provider_data->netdev != netdev)
			continue;
		if (provider_data->common.addr_family != addr_family)
			continue;
		if (add) {
			set_sockaddr_port((struct sockaddr *)&laddr, provider_data->port);
			if ((rv = siw_listen_address(provider_data->cm_id, 
				provider_data->backlog, (struct sockaddr *)&laddr, AF_INET, 0, 0)) < 0) {
				dprint(DBG_CM, "(id=" dprint_ptr_str() "): Listener id: failed (%d) to listen on new addr %s:%u\n",
				       provider_data->cm_id, rv, addr_str, be16_to_cpu(provider_data->port));
			}
			dprint(DBG_CM, "(id=" dprint_ptr_str() "): Listener id: listening on new addr %s:%u\n", 
			       provider_data->cm_id, addr_str, be16_to_cpu(provider_data->port));
		} else {
			/* Delete listener on this address */
			list_for_each_entry(cep, &provider_data->common.cep_list_head, listenq) {
				if (sockaddr_addr_equal((struct sockaddr *)&laddr, in_addr, in6_addr)) {
					dprint(DBG_CM, "(id=" dprint_ptr_str() "): Listener id: destroying CEP(" dprint_ptr_str() ") listening on deleted addr %s\n", 
					       provider_data->cm_id, cep, addr_str);
					list_del(&cep->listenq);
					list_add_tail(&cep->listenq, &del_list_head);
					break;
				}
			}
		}
	}
	mutex_unlock(&listen_any_guard);

	while ((cep = list_first_entry_or_null(&del_list_head, struct siw_cep, listenq))) {
		list_del(&cep->listenq);
		siw_drop_listener_cep(cep);
	}
}

int siw_cm_init(void)
{
	/*
	 * create_single_workqueue for strict ordering
	 */
	siw_cm_wq = create_singlethread_workqueue("siw_cm_wq");
	if (!siw_cm_wq)
		return -ENOMEM;

	return 0;
}

void siw_cm_exit(void)
{
	if (siw_cm_wq) {
		flush_workqueue(siw_cm_wq);
		destroy_workqueue(siw_cm_wq);
	}
}

/* Print the CEP internal log using printk.
 * Used for when there is a CEP leak detected by siw_device_deregister
 */
void siw_cep_printk_log(struct siw_cep *cep, const char *prefix)
{
	if (cep->log_ring_buf) {
		char line[256];
		int ring_idx, line_idx;

		pr_info("CEP: " dprint_ptr_str() "): LOG - head: %d tail: %d buf-size: %d\n",
			cep, cep->ring_buf_head, cep->ring_buf_tail, SIW_CEP_LOG_RING_BUF_SIZE);

		/* Loop over the ring buffer from the head to the tail (and accounting for the wrap-around) and 
		 * break it into lines for printing to the console */
		for (line_idx = 0, ring_idx = cep->ring_buf_head; ring_idx != cep->ring_buf_tail; 
		     line_idx++, ring_idx = (ring_idx + 1) % SIW_CEP_LOG_RING_BUF_SIZE)
		{
			if (cep->log_ring_buf[ring_idx] == '\n' || line_idx == sizeof(line) - 1) {
				/* EOL reached, print the line */
				line[line_idx] = 0;
				pr_info("(CEP: " dprint_ptr_str() "): [%d] %s\n", cep, line_idx, line);
				line_idx = 0;
			} else {
				line[line_idx] = cep->log_ring_buf[ring_idx];
			}
		}
		/* Print the last line */
		if (line_idx != 0) {
			line[line_idx] = 0;
			pr_info("(CEP: " dprint_ptr_str() "): [%d] %s\n", cep, line_idx, line);
		}
	}
}

/* Forcefully free the CEP and cancel all work.
 * Used for when there is a CEP leak detected by siw_device_deregister
 */
void siw_cep_force_free(struct siw_cep *cep)
{
	DECLARE_COMPLETION_ONSTACK(work_done);
	bool wait_work_done = false;
	struct siw_cm_work *work, *t_work;
	int rv_w;

	dprint_cep(DBG_CM | DBG_ON, cep, "Attempting to free CEP in state %d\n", 
	       cep->state);

	siw_cep_get(cep);
	siw_cep_set_inuse(cep);
	
	/* Cancel any currently scheduled works (and reduce their refcount) */
	siw_cancel_mpatimer(cep);
	siw_cancel_connect_timer(cep);
	siw_cancel_read_mpa_hdr(cep);
	siw_cancel_peer_close(cep);
	
	spin_lock_bh(&cep->lock);
	/* Try and cancel any outstanding works (They may not cancel if they are currently running) */
	list_for_each_entry_safe(work, t_work, &cep->work_outstanding, list) {
		bool cancelled;

		dprint_cep(DBG_CM | DBG_ON, cep, "has work "
			dprint_ptr_str() " index %llx of type %d outstanding with refcount %d. Cancelling\n",
		       work, work->work_idx, work->type, kref_read(&work->ref));

		/* We can't use cancel_delayed_work_sync because the CEP is currently in-use (Will cause deadlock) */
		if (!(cancelled = cancel_delayed_work(&work->work))) {
			dprint_cep(DBG_CM | DBG_ON, cep, "Failed to cancel work\n");
		} else {
			siw_put_work(work, true);
		}
	}
	if (!list_empty(&cep->work_outstanding)) {
		cep->all_work_done = &work_done;
		wait_work_done = true;
	}
	spin_unlock_bh(&cep->lock);

	if (wait_work_done) {
		/* Free CEP so any works still scheduled can run */
		siw_cep_set_free(cep);
		dprint_cep(DBG_CM | DBG_ON, cep, "Waiting for all works to complete\n");
		if ((rv_w = wait_for_completion_timeout(&work_done, ALL_WORK_DONE_TIMEOUT)) < 0) {
			dprint_cep(DBG_CM | DBG_ON, cep, "Failed (%d) to wait for all work outstanding\n", rv_w);
		} else if (rv_w == 0) {
			dprint_cep(DBG_CM | DBG_ON, cep, "Timeout waiting for all work outstanding\n");
			pr_warn("(CEP: " dprint_ptr_str() "): Timeout waiting for all work outstanding.\n", cep);

#if !defined(NVMESH_IS_PRODUCTION_COMPILATION) || (NVMESH_IS_PRODUCTION_COMPILATION==0)
			BUG_ON(1);
#endif
			/* Wait forever. If we ignore and continue going down, then when the work does run, it will cause kernel panic */ 
			wait_for_completion(&work_done);
		}
		/* Set CEP back to in-use so it can be set-free later */
		siw_cep_set_inuse(cep);
	}

	spin_lock_bh(&cep->lock);
	if (!list_empty(&cep->work_outstanding)) {
		dprint_cep(DBG_CM | DBG_ON, cep, "Still has work outstanding after cancelling and waiting.");
		
		/* Still has works outstanding. Warn, but don't free them otherwise we may cause a kernel panic */
#if defined(NVMESH_IS_PRODUCTION_COMPILATION) && (NVMESH_IS_PRODUCTION_COMPILATION==1)
		WARN_ON(1);
#else
		BUG_ON(1);
#endif
		return;
	}
	spin_unlock_bh(&cep->lock);

	siw_cep_set_free(cep);
	siw_cep_put(cep);
}
