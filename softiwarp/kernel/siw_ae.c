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

#include <linux/errno.h>
#include <linux/types.h>
#include <linux/net.h>
#include <linux/scatterlist.h>
#include <linux/highmem.h>
#include <net/sock.h>
#include <net/tcp_states.h>
#include <net/tcp.h>

#include <rdma/iw_cm.h>
#include <rdma/ib_verbs.h>
#include <rdma/ib_smi.h>
#include <rdma/ib_user_verbs.h>

#include "siw.h"
#include "siw_obj.h"
#include "siw_cm.h"


/* QP state lock is held before calling this function */
void siw_qp_event(struct siw_qp *qp, enum ib_event_type etype)
{
	struct ib_event event;
	struct ib_qp	*ofa_qp = &qp->ofa_qp;

	event.event = etype;
	event.device = ofa_qp->device;
	event.element.qp = ofa_qp;

	if (!(qp->attrs.flags & SIW_QP_IN_DESTROY) && ofa_qp->event_handler) {
		dprint(DBG_EH, ": reporting %d\n", etype);
		(*ofa_qp->event_handler)(&event, ofa_qp->qp_context);
	}
}

void siw_cq_event(struct siw_cq *cq, enum ib_event_type etype)
{
	struct ib_event event;
	struct ib_cq	*ofa_cq = &cq->ofa_cq;

	event.event = etype;
	event.device = ofa_cq->device;
	event.element.cq = ofa_cq;

	if (ofa_cq->event_handler) {
		dprint(DBG_EH, ": reporting %d\n", etype);
		(*ofa_cq->event_handler)(&event, ofa_cq->cq_context);
	}
}

void siw_srq_event_work(struct work_struct *work)
{
	struct siw_srq *srq = container_of(work, struct siw_srq, event_work);
	siw_srq_event(srq, srq->event_work_type, false);
	atomic_dec(&srq->event_work_sched);
}

void siw_srq_event(struct siw_srq *srq, enum ib_event_type etype, bool schedule)
{
	if (!schedule) {
		struct ib_event event;
		struct ib_srq	*ofa_srq = &srq->ofa_srq;

		event.event = etype;
		event.device = ofa_srq->device;
		event.element.srq = ofa_srq;

		if (ofa_srq->event_handler) {
			dprint(DBG_EH | DBG_ON, "(SRQ " dprint_ptr_str() "): reporting %d\n", srq, etype);
			(*ofa_srq->event_handler)(&event, ofa_srq->srq_context);
		}
	} else {
		if (atomic_inc_return(&srq->event_work_sched) == 1) {
			dprint(DBG_EH | DBG_ON, "(SRQ " dprint_ptr_str() "): Scheduling event %d on system WQ\n", srq, etype);
			srq->event_work_type = etype;
			if (!schedule_work(&srq->event_work)) {
				dprint(DBG_EH | DBG_ON, "(SRQ " dprint_ptr_str() "): Failed to schedule event %d work\n", srq, etype);
				atomic_dec(&srq->event_work_sched);
			}
		} else {
			atomic_dec(&srq->event_work_sched);
			dprint(DBG_EH | DBG_ON, "(SRQ " dprint_ptr_str() "): Event already scheduled\n", srq);
		}
	}
}

void siw_port_event(struct siw_dev *sdev, u8 port, enum ib_event_type etype)
{
	struct ib_event event;

	event.event = etype;
	event.device = &sdev->ofa_dev;
	event.element.port_num = port;

	dprint(DBG_EH, ": reporting %d\n", etype);
	ib_dispatch_event(&event);

#if 0
//#if KS_IB_DEVICE_HAS_EVENT_HADNLER_RWSEM
	/* WA to the fact the IB_EVENT_GID_CHANGE is not published for non roce cap devices 
	   In newer kernels */
	if (etype == IB_EVENT_GID_CHANGE) {
		dprint(DBG_EH | DBG_ON,
		 "reporting IB_EVENT_GID_CHANGE directly for %s\n", sdev->ofa_dev.name);
		down_read(&event.device->event_handler_rwsem);
		list_for_each_entry(handler, &event.device->event_handler_list, list)
			handler->handler(handler, &event);
		up_read(&event.device->event_handler_rwsem);
	}
#endif
}
