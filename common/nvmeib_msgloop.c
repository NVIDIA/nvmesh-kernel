#include "nvmeib_msgloop.h"
#include "nvmeib_utils.h"
#include <linux/poll.h>
#include "nvmeibm_trace.h"

#define IOCTL_GET_MSG_SIZE _IOR(0xbb, 1, size_t)

struct msgloop_procfs_ent {
	struct list_head kernel_to_user;
	wait_queue_head_t read_waitq;
	spinlock_t lock;
	int msg_count;
	int max_msg;
	bool flushing;
	void *arg;
	int (*cb)(void *arg, char *buf, size_t len, bool *posted);
	void (*on_close_cb)(void *);
	void (*on_open_cb)(void *);
	void (*on_ready_cb)(void *);
	struct proc_dir_entry *dir;
	char *name;
	struct proc_dir_entry *ent;
	bool shutdown;
	bool opened;
	bool retry;
};

static void msgloop_free_msg(struct kref *kref)
{
       struct msgloop_msg *msg = container_of(kref, struct msgloop_msg, ref_cnt);
       kfree(msg);
}

void msgloop_get_msg(void *_msg)
{
	struct msgloop_msg *msg = _msg;
	if (msg) {
	BUG_ON(kref_read(&msg->ref_cnt) < 1);
		kref_get(&msg->ref_cnt);
	}

}

void msgloop_put_msg(void *_msg)
{
	struct msgloop_msg *msg = _msg;
	if (msg)
		kref_put(&msg->ref_cnt, msgloop_free_msg);
}
EXPORT_SYMBOL(msgloop_put_msg);

struct msgloop_msg *nvmeib_msgloop_alloc_msg(size_t data_size)
{
	struct msgloop_msg *msg;

	msg = kzalloc(sizeof(*msg) + data_size, GFP_KERNEL);
	if (msg) {
		kref_init(&msg->ref_cnt);
	}

	return msg;
}
EXPORT_SYMBOL(nvmeib_msgloop_alloc_msg);

static ssize_t msgloop_proc_read(struct file *file, char __user *userbuf,
				 size_t len, loff_t *offset_p)
{
	struct msgloop_procfs_ent *p = file_get_priv_data(file);
	ssize_t copysize;
	struct msgloop_msg *msg;
	unsigned long flags;
	int rv;

	spin_lock_irqsave(&p->lock, flags);
	if (list_empty(&p->kernel_to_user) && p->flushing) {
		p->flushing = false;
		spin_unlock_irqrestore(&p->lock, flags);
		if (p->on_ready_cb) // signal the upper level to try and send pending messages
			p->on_ready_cb(p->arg);
		return -EOVERFLOW;
	}
	if ((file->f_flags & O_NONBLOCK) && list_empty(&p->kernel_to_user)) {
		/*  If O_NONBLOCK is set, read returns immediately */
		spin_unlock_irqrestore(&p->lock, flags);
		return -EWOULDBLOCK;
	}
	while (!p->shutdown && list_empty(&p->kernel_to_user)) {
		/* If there is no data in the input buffer,
		   by default read must block until at least one byte is there. */
		spin_unlock_irqrestore(&p->lock, flags);
		if ((rv = wait_event_interruptible(p->read_waitq,
				p->shutdown || !list_empty(&p->kernel_to_user))) < 0)
			return rv;
		spin_lock_irqsave(&p->lock, flags);
	}
	if (p->shutdown) {
		spin_unlock_irqrestore(&p->lock, flags);
		return 0;
	}
	msg = list_first_entry(&p->kernel_to_user, struct msgloop_msg, link);
	if (len) {
		_ND(trace_nvmeib_msgloop_msgloop_proc_read, "reading a message out of @MSG_COUNT pending messages", p->msg_count);
		list_del(&msg->link);
		--p->msg_count;
	}
	spin_unlock_irqrestore(&p->lock, flags);

	copysize = msg->len;
	if (len) {
		len = min(copysize, (ssize_t)len);
		if (len < copysize) {
			_NW(w_msgloop_proc_r, ": Userbuf is smaller than message size. msg will be partially copied and then dropped");
		}
		if (copy_to_user(userbuf, msg->data, len))
			copysize = -EFAULT;
		msgloop_put_msg(msg);
	}

	return copysize;
}

static ssize_t msgloop_proc_write(struct file *file, const char __user *userbuf,
	size_t len, loff_t *offset_p)
{
	struct msgloop_procfs_ent *p = file_get_priv_data(file);
	char *buf;
	int wr_len;
	bool posted = false;

	if (len >= (2*1024*1024))
		return -E2BIG;

	if (p->cb == NULL)
		return -ENXIO;

	if (p->shutdown)
		return -EPIPE;

	if ((buf = kmalloc(len+1, GFP_KERNEL)) == NULL)
		return -ENOMEM;

	if (copy_from_user(buf,  userbuf, len))
		return -EFAULT;

	buf[len] = '\0';

	/* callbacks:
	 * nvmeibs_toma_client_proc_recv()
	 * nvmeibs_toma_server_proc_recv()
	 */
	wr_len = (*p->cb)(p->arg, buf, len, &posted);

	if (!posted)
		kfree(buf);

	return wr_len;
}

static unsigned int msgloop_proc_poll(
	struct file *f, struct poll_table_struct *tbl)
{
	struct msgloop_procfs_ent *p = file_get_priv_data(f);
	unsigned int mask;

	if (p->shutdown) {
		/* if device is shutdown let caller read EOF*/
		mask = POLLHUP | POLLRDHUP;
	}
	else {
		mask = POLLOUT | POLLWRNORM;  /* device can be written to without blocking */
		poll_wait(f, &p->read_waitq, tbl);
		/* if device has data available let caller read it */
		if (!list_empty(&p->kernel_to_user))
			mask |= POLLIN | POLLRDNORM;
	}
	_ND(trace_msgloop_proc_poll, "Polling on /proc @STR, mask=@INT32_HEX", p->name, mask);
	return mask;
}

static int msgloop_proc_open(struct inode *inode, struct file *file)
{
	unsigned long flags;
	struct msgloop_procfs_ent *p = file_get_priv_data(file);
	int rv = 0;

	spin_lock_irqsave(&p->lock, flags);
	if (!p->opened) {
		p->opened = true;
	} else
		rv = -EBUSY;
	spin_unlock_irqrestore(&p->lock, flags);
	_NT(trace_nvmeib_msgloop_msgloop_proc_open, "@MSG_COUNT pending messages", p->msg_count);

	if (rv == 0 && p->on_open_cb)
		p->on_open_cb(p->arg);

	return rv;
}

static size_t __msgloop_remove_all_msgs_locked(struct msgloop_procfs_ent *p)
{
	struct msgloop_msg *msg;
	size_t const removed = p->msg_count;

	while (!list_empty(&p->kernel_to_user)) {
		msg = list_first_entry(&p->kernel_to_user, struct msgloop_msg, link);
		list_del(&msg->link);
		msgloop_put_msg(msg);
	}
	p->msg_count = 0;
	return removed;
}

static int msgloop_proc_release(struct inode *inode, struct file *file)
{
	unsigned long flags;
	struct msgloop_procfs_ent *p = file_get_priv_data(file);

	if (p->on_close_cb)
		(*p->on_close_cb)(p->arg);
	BUG_ON(!p->opened);
	spin_lock_irqsave(&p->lock, flags);
	p->opened = false;
	if (p->msg_count > 0) {
		__msgloop_remove_all_msgs_locked(p);
	}
	p->flushing = false;
	spin_unlock_irqrestore(&p->lock, flags);

	return 0;
}

static long msgloop_proc_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	int rc = 0;
	size_t msg_size = 0;
	unsigned long flags;

	struct msgloop_procfs_ent *p = file_get_priv_data(f);
//	printk("msgloop_proc_ioctl cmd=%x   expected=%lx\n", cmd, IOCTL_GET_MSG_SIZE);

	spin_lock_irqsave(&p->lock, flags);
	if (list_empty(&p->kernel_to_user)) {
		msg_size = 0;
	} else {
		struct msgloop_msg *msg;
		msg = list_first_entry(&p->kernel_to_user, struct msgloop_msg, link);
		msg_size = msg->len;
	}
	spin_unlock_irqrestore(&p->lock, flags);

	switch (cmd) {
	case IOCTL_GET_MSG_SIZE:
		if ((_IOC_NR(cmd) != 1) || (_IOC_SIZE(cmd) != sizeof msg_size)) {
			printk("ioctl cmd/size is wrong  %d %d\n", _IOC_NR(cmd), _IOC_SIZE(cmd));
			rc = -EFAULT;
			break;
		}

		if (copy_to_user((void __user *)arg, &msg_size, sizeof msg_size)) {
			printk("copy_to_user() failed\n");
			rc = -EFAULT;
		}
		break;
	default:
		rc = -ENOTTY;
		break;
	}

	return rc;
}


#if !KS_HAS_PROC_FS
	static struct file_operations proc_fops = {
		.open = msgloop_proc_open,
		.release = msgloop_proc_release,
		.read = msgloop_proc_read,
		.write = msgloop_proc_write,
		.poll = msgloop_proc_poll,
		.unlocked_ioctl = msgloop_proc_ioctl
	};
#else
	static struct proc_ops proc_fops = {
		.proc_open = msgloop_proc_open,
		.proc_release = msgloop_proc_release,
		.proc_read = msgloop_proc_read,
		.proc_write = msgloop_proc_write,
		.proc_poll = msgloop_proc_poll,
		.proc_ioctl = msgloop_proc_ioctl
	};
#endif

int nvmeib_msgloop_send(struct msgloop_procfs_ent *p, char *data, size_t len)
{
	struct msgloop_msg *msg;
	ulong flags;

	if (p->shutdown)
		return -EPIPE;

	if (len == 0) {
		return -EINVAL;
	}
	if ((p->max_msg > 0) && (p->msg_count >= p->max_msg))
		p->flushing = true;
	if (p->flushing)
		return -ENOSPC;
	/* allocate msg and data buffer - free it after read() */
	if ((msg = nvmeib_msgloop_alloc_msg(len + sizeof(*msg))) == NULL)
		return -ENOMEM;
	msg->len = len;
	if (data && len)
		memcpy(msg->data, data, len);
	spin_lock_irqsave(&p->lock, flags);
	list_add_tail(&msg->link, &p->kernel_to_user);
	++p->msg_count;
	spin_unlock_irqrestore(&p->lock, flags);

	wake_up_interruptible(&p->read_waitq);

	return 0;
}
EXPORT_SYMBOL(nvmeib_msgloop_send);

int nvmeib_msgloop_sendv(struct msgloop_procfs_ent *p, struct msg_vec *vec, int cnt)
{
	LIST_HEAD(msg_list);
	struct msgloop_msg *msg;
	ulong flags;
	int ii;
	int rv = 0;

	if (p->shutdown)
		return -EPIPE;

	if (!cnt)
		return 0;

	if ((p->max_msg > 0) && (p->msg_count+cnt > p->max_msg))
		p->flushing = true;
	if (p->flushing)
		return -ENOSPC;

	for (ii = 0; ii < cnt; ii++) {
		msg = nvmeib_msgloop_alloc_msg(vec[ii].len + sizeof(*msg));
		if (msg == NULL) {
			rv = -ENOMEM;
			goto free_msgs;
		}
		msg->len = vec[ii].len;
		if (vec[ii].data && vec[ii].len)
			memcpy(msg->data, vec[ii].data, vec[ii].len);
		list_add_tail(&msg->link, &msg_list);
	}

	spin_lock_irqsave(&p->lock, flags);
	list_splice_tail(&msg_list, &p->kernel_to_user);
	p->msg_count += cnt;
	spin_unlock_irqrestore(&p->lock, flags);

	wake_up_interruptible(&p->read_waitq);

	goto out;

free_msgs:
	while (!list_empty(&msg_list)) {
		msg = list_first_entry(&msg_list, struct msgloop_msg, link);
		list_del(&msg->link);
		msgloop_put_msg(msg);
	}

out:
	return rv;
}
EXPORT_SYMBOL(nvmeib_msgloop_sendv);

int nvmeib_msgloop_sendl(struct msgloop_procfs_ent *p, struct list_head *l)
{
	ulong flags;
	int cnt;
	struct list_head *lp;
	struct msgloop_msg *msg;
	int rv;

	if (p->shutdown) {
		rv = -EPIPE;
		goto out_nl;
	}
	cnt = 0;
	list_for_each(lp, l)
		++cnt;

	spin_lock_irqsave(&p->lock, flags);
	if ((p->max_msg > 0) && (p->msg_count+cnt > p->max_msg))
		p->flushing = true;

	if (p->flushing || !p->opened) {
		rv = -ENOSPC;
		goto out;
	}
	while ((msg = list_first_entry_or_null(l, struct msgloop_msg, link))) {
		BUG_ON(kref_read(&msg->ref_cnt) < 1);
		list_del_init(&msg->link);
		list_add_tail(&msg->link, &p->kernel_to_user);
	}
	p->msg_count += cnt;
	rv = 0;

out:
	spin_unlock_irqrestore(&p->lock, flags);
	wake_up_interruptible(&p->read_waitq);
out_nl:
	return rv;
}
EXPORT_SYMBOL(nvmeib_msgloop_sendl);

size_t nvmeib_msgloop_flush(struct msgloop_procfs_ent *p)
{
	ulong flags;
	size_t removed = 0;

	if (p == NULL)
		return removed;

	spin_lock_irqsave(&p->lock, flags);
	removed = __msgloop_remove_all_msgs_locked(p);
	spin_unlock_irqrestore(&p->lock, flags);

	return removed;
}
EXPORT_SYMBOL(nvmeib_msgloop_flush);

int nvmeib_msgloop_get_count(const struct msgloop_procfs_ent *p)
{
	if (p != NULL)
		return p->msg_count;
	else
		return 0;
}
EXPORT_SYMBOL(nvmeib_msgloop_get_count);

struct msgloop_procfs_ent *nvmeib_msgloop_create(char *name,
	struct proc_dir_entry *dir, int (*cb)(void *arg, char *buf, size_t len, bool *posted),
	void (*on_open_cb)(void *), void (*on_close_cb)(void *), void *arg)
{
	struct msgloop_procfs_ent *p;
	int mode = 0400;

	if (cb != NULL)
		mode |= 0200;

	p = kzalloc(sizeof *p, GFP_KERNEL);
	if (!p)
		goto err_out;

	p->cb = cb;
	p->on_close_cb = on_close_cb;
	p->on_open_cb = on_open_cb;
	p->on_ready_cb = NULL;
	p->arg = arg;
	p->dir = dir;
	p->name = kstrdup(name, GFP_KERNEL);
	if (!p->name)
		goto err_free_procfs;

	spin_lock_init(&p->lock);
	INIT_LIST_HEAD(&p->kernel_to_user);
	p->max_msg = 0;
	p->flushing = false;
	p->msg_count = 0;

	init_waitqueue_head(&p->read_waitq);

	p->shutdown = false;
	p->opened = false;
	p->ent = proc_create_data(name, mode, dir, &proc_fops, p);
	if (!p->ent)
	  goto err_free_name;

	return p;

err_free_name:
	kfree(p->name);
err_free_procfs:
	kfree(p);
err_out:
	return NULL;
}
EXPORT_SYMBOL(nvmeib_msgloop_create);

void nvmeib_msgloop_set_ready_cb(struct msgloop_procfs_ent *p,
				 void (*ready_cb)(void *))
{
	p->on_ready_cb = ready_cb;
}
EXPORT_SYMBOL(nvmeib_msgloop_set_ready_cb);

void nvmeib_msgloop_remove(struct msgloop_procfs_ent *p)
{
#define MSGLOOP_TIMEOUT_MS 1000
	unsigned long loop_timeout = jiffies + msecs_to_jiffies(MSGLOOP_TIMEOUT_MS);

	if (p == NULL)
		return;
	p->shutdown = true;
	wake_up_interruptible_all(&p->read_waitq);
	remove_proc_entry(p->name, p->dir);
	spin_lock(&p->lock);
	__msgloop_remove_all_msgs_locked(p);
	spin_unlock(&p->lock);

	/* Calling wake_up_interruptible_all() is not enough, we need to wait
	 * until it actually happen before freeing the resources. We don't
	 * want use after free....
	 */
	while (waitqueue_active(&p->read_waitq)) {
		if (jiffies > loop_timeout) {
			_NW(warn_nvmeib_msgloop_nvmeib_msgloop_remove, "Timedout waiting for proc wait queue");
			break;
		}
		schedule();
	}

	kfree(p->name);
	kfree(p);
}
EXPORT_SYMBOL(nvmeib_msgloop_remove);

void nvmeib_msgloop_set_max(struct msgloop_procfs_ent *p, int max_msg)
{
	p->max_msg = max_msg;
}
EXPORT_SYMBOL(nvmeib_msgloop_set_max);
