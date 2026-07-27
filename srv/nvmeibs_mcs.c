/*
 * proc file (msgloop) interface between target module and mcs-app.
 *
 * On proc file re/open, a periodic report-work is scheduled.
 * On proc file close, periodic report-work is canceled.
 *
 */

/*
 * Assumed msgloop behavior:
 * - Exclusive access (single user)
 *
 * - On close, msgloop will do the following in this order:
 *    1. mark the file as closed - new msgs from client/target are rejected.
 *    2. flush the msg queue - new session wont read prev session's msgs.
 *    3. call the on-close callback of the client/target
 *
 * - Throttling: if enabled, when a threshold is reached,
 *   msgloop will fail msg-send from client/target.
 *   Only after user (mcs-app) drains all the queued msgs,
 *   will msgloop allow new msg send.
 *
 */
#include "nvmeibs_mcs.h"

#include "nvmeibs_mcs_stub.h"
#include "nvmeib_mcs.h"
#include "nvmeib_utils.h"
#include "nvmeibs_defs.h"
#include "nvmeibs_main.h"
#include "nvmeibs_disk.h"
#include "nvmeib.h"
#include "nvmeibs_nvme.h"
#include "nvmeib_msgloop.h"
#include "nvmeibs_ib_port.h"
#include "nvmeib_public.h"
#include "nvmeibs_trace.h"

extern char *dummy_id;

#define MCS_REPORT_PERIOD_SEC 	(5)
#define MCS_REPORT_PERIOD_HZ	(HZ * MCS_REPORT_PERIOD_SEC)
#define NVMEIBS_MAX_MSC_MSGS 5000

struct nvmeibs_mcs {
	spinlock_t guard;
	bool dying;
	/* proc */
	struct msgloop_procfs_ent *proc;
	/* marshaller */
	void *marshaller;
	/* periodic report */
//	struct nvmeibs_mcs_report rep;
	/* recv callback */
	nvmeibs_mcs_recv_cb *recv_cb;
};

struct nvmeibs_mcs *s_mcs = NULL;

/* ************************************************************************** */
/* mcs proc                                                                   */
/* ************************************************************************** */
static void mcs_proc_open(void *arg)
{
	//TODO: if common code, arg should be mcs
	NFIN;
	_NT(trace_mcs_mcs_proc_open, "Opening proc-mcs");
	//mcs_report_work_start();
	NFOUT;
}

static void mcs_proc_close(void *arg)
{
	NFIN;
	_NT(trace_mcs_mcs_proc_close, "Closing proc-mcs");
	//mcs_report_work_cancel();
	_NT(trace_1_mcs_mcs_proc_close, "Closed");
	NFOUT;
}

/**
 * if invoked from:
 * - report-work (dying may be true but) proc is still valid.
 * - nvmeibs, it's its responsibility that proc is valid too.
 */
static int mcs_proc_send(void *context, void *data, int len)
{
	struct msgloop_procfs_ent *p = context;
	int rv;
	NFIN;

	if (!p || p != s_mcs->proc) {
		_NE(error_mcs_mcs_proc_send, "Invalid mcs proc entry");
		rv = -EINVAL;
		goto out;
	}

	rv = nvmeib_msgloop_send(p, data, len);

out:
	NFOUT;
	return rv;
}

/**
 * called from msgloop's .write fops; not thread safe.
 */
static int mcs_proc_recv(void *arg, char *buf, size_t len, bool *posted)
{
	void *msg;
	int opcode;
	int rv = len;
	NFIN;

	if (!(msg = nvmeib_mcs_get_msg(arg, buf, len))) {
		_NE(error_mcs_mcs_proc_recv, "Fail mcs unmarsh");
		rv = -EINVAL;
		goto out;
	}

	opcode = nvmeib_mcs_get_opcode(msg);
	if (false /* For future support of mcs2srv msgs */) {
		if (s_mcs->recv_cb)
			s_mcs->recv_cb(buf, len);
	}
	else {
		_NE(error_1_mcs_mcs_proc_recv, "Unknown msg-opcode @OPCODE", opcode);
		rv = -ENOMSG;
	}

out:
	if (rv != len) {
		_NE(error_2_mcs_mcs_proc_recv, "Dump received mcs msg (mcs=@MCS, buf=@BUF, len=@LEN_LONG):", arg, buf, len);
		len = min((int)len, 256);
		_Dbuf(buf, len);
	}

	NFOUT;
	return rv;
}

static int mcs_proc_create(struct proc_dir_entry *dir, void *arg)
{
	int rv = -1;
	NFIN;

	if (s_mcs->proc) {
		_NE(error_mcs_mcs_proc_create, "mcs proc handle already exists (@PROC)", s_mcs->proc);
		goto out;
	}

	if (!(s_mcs->proc = nvmeib_msgloop_create("mcs", dir,
		mcs_proc_recv, mcs_proc_open, mcs_proc_close, arg))) {
		_NE(error_1_mcs_mcs_proc_create, "Fail to create mcs proc file");
		goto out;
	}

	nvmeib_msgloop_set_max(s_mcs->proc, NVMEIBS_MAX_MSC_MSGS);

	rv = 0;

out:
	NFOUT;
	return rv;
}

static void mcs_proc_remove(void)
{
	NFIN;

	if (s_mcs->proc) {
		nvmeib_msgloop_remove(s_mcs->proc);
		s_mcs->proc = NULL;
	}
	else
		_NE(error_mcs_mcs_proc_remove, "No mcs proc handle");

	NFOUT;
}

/* ************************************************************************** */
/* APIs                                                                       */
/* ************************************************************************** */
int nvmeibs_mcs_send(void *buf, int len)
{
	int rv;
	NFIN;

	rv = mcs_proc_send(s_mcs->proc, buf, len);

	NFOUT;
	return rv;
}

int nvmeibs_mcs_create(struct proc_dir_entry *proc_dir,
	nvmeibs_mcs_recv_cb *recv_cb)
{
	int rv = -1;
	NFIN;

	if (s_mcs) {
		_NE(error_mcs_nvmeibs_mcs_create, "Fail to create, s_mcs already exist");
		goto out;
	}

	_NT(trace_mcs_nvmeibs_mcs_create, "Create srv's mcs");
	if (!(s_mcs = kzalloc(sizeof(*s_mcs), GFP_KERNEL))) {
		_NE(error_1_mcs_nvmeibs_mcs_create, "Fail to allocate srv's mcs");
		goto out;
	}
	spin_lock_init(&s_mcs->guard);

	/* allocate marshaller */
	if (!(s_mcs->marshaller = NVMEIB_MCS_INIT))
		goto err;

	/* create mcs proc file */
	s_mcs->recv_cb = recv_cb;
	if (mcs_proc_create(proc_dir, s_mcs) < 0)
		goto err;

	/* when (/if) mcs-app opens the proc, we'll start mcs-reports ... */
	_NT(trace_1_mcs_nvmeibs_mcs_create, "srv's mcs created successfully");
	rv = 0;
	goto out;

err:
	nvmeibs_mcs_destroy();

out:
	NFOUT;
	return rv;
}

void nvmeibs_mcs_destroy(void)
{
	unsigned long flags;
	NFIN;

	if (!s_mcs) {
		_NE(error_mcs_nvmeibs_mcs_destroy, "Fail to destroy, s_mcs does not exist");
		goto out;
	}

	_NT(trace_mcs_nvmeibs_mcs_destroy, "Destroy srv's mcs");
	/* prevent mcs-app from triggering stuff here (e.g.
	   re-starting reports by (re)opening file after
	   we'll stop the reports that already running, if any;
	   or running recv-handlers) after we strated dying */
	spin_lock_irqsave(&s_mcs->guard, flags);
	s_mcs->dying = true;
	spin_unlock_irqrestore(&s_mcs->guard, flags);

	/* stop reports, before freeing report's resources and
	   before removing mcs-proc, which report uses both */
	/* if proc file is/was already closing/closed by mcs-app,
	   this call shall do noting */
//	mcs_report_work_cancel();

	/* free reporting resources */
//	mcs_report_free();

	/* stop new mcs-recv by removing mcs proc file.
	   before removing marshller */
	mcs_proc_remove();

	/* free resources that recv-handlers uses:
	   marshaller, others in future  */
	nvmeib_mcs_remove(s_mcs->marshaller);

	/* free s_mcs */
	kfree(s_mcs);
	s_mcs = NULL;

out:
	NFOUT;
}

/*
 * TODO:
 * 1. Fill report fields marked with MCS_NOVAL (use YK's codecs?)
 * 2. integration - see 'YK' comments (in nvme.c too)
 *
 * Optional:
 * 1. Reuse code (except report-fill) from client module, (see all
 *    context/arg/container_of, etc.) - see 'TODO: if common code'
 * 2. allocate smart page once and use it for all disks (free it on destroy)
 * 3. Use nvmeib_ref instead of dying? useful if refcnt needed from
 *    recv-handler
 */

