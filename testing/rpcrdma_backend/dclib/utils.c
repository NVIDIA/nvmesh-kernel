#include "kr_version.h"
#include "xkr_incs.h"
#include "xib_incs.h"
#include "utils.h"
#include "manager.h"
#include "xtrace.h"

#include <linux/inet.h>
#include <rdma/ib.h>

struct proc_file_info {
	proc_chng_cb *chng;
	void *arg;
	struct proc_dir_entry *dir;
	struct proc_dir_entry *ent;
	char *name;
};

#if KS_PDE_DATA
	#define get_priv_data(file) PDE_DATA(file_inode(file))
#else
	#define get_priv_data(file) PDE(file->f_path.dentry->d_inode)->data
#endif

static ssize_t proc_write(struct file *file, const char __user *userbuf,
	size_t len, loff_t *offset_p)
{
	char *buf;
	struct proc_file_info *p = get_priv_data(file);
	int rv;

	FIN;
	if (p->chng == NULL) {
		rv = -ENXIO;
		goto out;
	}

	if (len >= 128 * 1024) {
		rv = E2BIG;
		goto out;
	}

	if ((buf = kmalloc(len + 1, GFP_KERNEL)) == NULL) {
		rv = -ENOMEM;
		goto out;
	}

	if (copy_from_user(buf, userbuf, len)) {
		rv = -EFAULT;
	}
	else {
		buf[len] = '\0';
		rv = (*p->chng)(p->arg, buf, len);
	}

	if (rv > 0)
		*offset_p += rv;
	kfree(buf);

out:

	FOUT;
	return rv;
}

void * proc_create_entry(char *name, struct proc_dir_entry *dir,
	proc_chng_cb *chng, void *arg)
{
	static struct file_operations fops = { .write = proc_write };
	const int mode = 0200; 
	struct proc_file_info *p = kmalloc(sizeof(*p), GFP_KERNEL);

	FIN;
	if (p) {
		p->name = kstrdup(name, GFP_KERNEL);
		p->ent = proc_create_data(name, mode, dir, &fops, p);
		if (!p->ent) {
			kfree(p->name);
			kfree(p);
		}
		p->chng = chng;
		p->arg = arg;
		p->dir = dir;
	}
	FOUT;
	return p;
}

void proc_remove_entry(void *p)
{
	struct proc_file_info *q = p;

	FIN;
	remove_proc_entry(q->name, q->dir);
	kfree(q->name);
	kfree(q);
	FOUT;
}

void x_ref_init(struct x_ref *r)
{
	//FIN;;
	atomic_set(&r->cnt, 1);
	r->comp = NULL;
	atomic_set(&r->dying, 0);
	//FOUT;
}

int __must_check x_ref_get(struct x_ref *r)
{
	int _old;

	FIN;
	if (!atomic_read(&r->dying))
		_old = atomic_inc_not_zero(&r->cnt);
	else
		_old = 0;
	FOUT;
	return _old;
}

int x_ref_put(struct x_ref *r)
{
	int _new;

	FIN;
	if (!(_new = atomic_dec_return(&r->cnt))) {
		if (r->comp)
			complete(r->comp);
		else {
			xwtrace("r %p: cnt = 0 w/o comp\n", r);
			WARN_ON(1);
		}
	}
	FOUT;
	return _new;
}

int x_ref_release_start(struct x_ref *r)
{
	int dying;
	int rv;

	FIN;
	if ((dying = atomic_inc_return(&r->dying)) == 1) {
		xttrace("r %p: initiate release\n", r);
		rv = 0;
	}
	else {
		xttrace("r %p: release already initaited %d\n", r, dying);
		rv = -1;
	}

	FOUT;
	return rv;
}

void x_ref_release_wait(struct x_ref *r)
{
	DECLARE_COMPLETION_ONSTACK(comp);
	int n, a = 0, rvw;

	FIN;
	/* release prior init */
	if (atomic_read(&r->cnt) <= 0)
		goto out;

	r->comp = &comp;
	if ((n = atomic_dec_return(&r->cnt))) {
		xdtrace("r %p: wait for %d to finish\n", r, n);
		while ((rvw = wait_for_completion_interruptible_timeout(
			r->comp, NVMEIB_REF_WAIT_RELEASE)) <= 0) {
			xwtrace("r %p: wait for %d to finish, attempt %d\n",
				r, atomic_read(&r->cnt), a);
			a++;
		}
	}
	r->comp = NULL;

out:
	FOUT;
}

char * x_tss(struct sockaddr_storage *a, char buf[], int len)
{
	if (a->ss_family == AF_IB)
		snprintf(buf, len, "%pI6", ((struct sockaddr_ib *)a)->sib_addr.sib_raw);
	else
		snprintf(buf, len, "%pIS", (struct sockaddr *)a);
	return buf;
}

bool x_cmp_addr(struct sockaddr_storage *a, struct sockaddr_storage *b)
{
	struct sockaddr_ib *pib;
	struct sockaddr_ib *qib;
	struct sockaddr_in *pip;
	struct sockaddr_in *qip;

	if (a->ss_family == b->ss_family) {
		if (a->ss_family == AF_IB) {
			pib = (struct sockaddr_ib *)a;
			qib = (struct sockaddr_ib *)b;
			return memcmp(pib->sib_addr.sib_raw, qib->sib_addr.sib_raw,
				sizeof(pib->sib_addr.sib_raw)) == 0;
		}
		else {
			pip = (struct sockaddr_in *)a;
			qip = (struct sockaddr_in *)b;
			return pip->sin_addr.s_addr == qip->sin_addr.s_addr;
		}
	}
	else
		return false;
}

static atomic64_t global_uid = ATOMIC64_INIT(0);
u64 x_get_guid(void)
{
	return atomic64_inc_return(&global_uid);
}

char * x_tsid(union service_id *sid, char buf[], int len)
{
	snprintf(buf, len, "sid=%lld, index=%d, dct=%#x",
		be64_to_cpu(sid->global.sid),
		be32_to_cpu(sid->global.index),
		be32_to_cpu(sid->global.dct));
	return buf;
}

