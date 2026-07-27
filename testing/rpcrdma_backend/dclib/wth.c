#include "xkr_incs.h"
#include "wth.h"
#include "requests.h"
#include "xtrace.h"

struct wth {
	const char *name;
	/* main thread request q */
	struct list_head requests;
	/* main thread request q guard */
	spinlock_t requests_guard;
	/* main thread wait for events on this wait_queue */
	wait_queue_head_t wqh;
	/* the main thread */
	struct task_struct *t;

	/* owner info */
	void *owner;
	void (*on_start)(void *);
	void (*on_stop)(void *);
	void (*on_exit)(void *);
};

/**
 * Start wth requests
 */

/**
 * End wth requests
 */

static void clear_requests(struct wth *t, struct list_head *l)
{
	struct request_base *r;

	FIN;
	while ((r = list_first_entry_or_null(l, struct request_base, link))) {
		list_del(&r->link);
		if (r->free)
			r->free(r);
	}
	FOUT;
}


static bool process_requests(struct wth *t, struct list_head *l)
{
	struct request_base *r;
	bool cont = true;
	bool call_free;

	FIN;
	while (cont &&
		   (r = list_first_entry_or_null(l, struct request_base, link))) {
		list_del(&r->link);
		xdtrace("wth %s recived request %s\n", t->name,
			rpcrdma_request_to_str(r->type));
		/* save if free needs to be called - do it here as the callback
		   may free the event...
		 */
		call_free = !!r->free;
		switch (r->type) {
		case rpcrdma_stop:
			if (t->on_stop)
				t->on_stop(t->owner);
			cont = false;
			break;
		default:
			r->call(r->owner ?: t->owner, r);
			break;
		}
		if (call_free)
			r->free(r);
	}
	clear_requests(t, l);
	FOUT;
	return cont;
}

static int run(void *arg)
{
	struct wth *t = arg;
	LIST_HEAD(next_request);
	unsigned long flags;
	bool cont = true;
	int rv;

	FIN;
	if (t->on_start)
		t->on_start(t->owner);
	while (!kthread_should_stop() && cont) {
		while (list_empty(&next_request)) {
			spin_lock_irqsave(&t->requests_guard, flags);
			rv = wait_event_interruptible_lock_irq(t->wqh,
				!list_empty(&t->requests), t->requests_guard);
			if (rv == 0) {
				xdtrace("New events for wth %s arrived\n", t->name);
				list_splice_tail_init(&t->requests, &next_request);
			}
			spin_unlock_irqrestore(&t->requests_guard, flags);
			if (rv != 0)
				xetrace("Waiting for new events in wth %s interrupted\n",
					t->name);
		}
		cont = process_requests(t, &next_request);
	}
	spin_lock_irqsave(&t->requests_guard, flags);
	clear_requests(t, &t->requests);
	spin_unlock_irqrestore(&t->requests_guard, flags);
	if (t->on_exit)
		t->on_exit(t->owner);
	FOUT;
	return 0;
}

struct wth * wth_create(struct wth_info *p)
{
	const char *name;
	struct wth *t;

	FIN;
	if (!(t = kzalloc(sizeof(*t), GFP_KERNEL))) {
		xetrace("Failed to allocate worker_thread %s memory\n", p->name);
		goto out;
	}
	if (!(name = kstrdup(p->name, GFP_KERNEL))) {
		xetrace("Failed to allocate worker_thread %s name memory\n", p->name);
		goto freep;
	}
	else
		t->name = name;

	t->owner = p->owner;
	t->on_start = p->on_start;
	t->on_exit = p->on_exit;
	INIT_LIST_HEAD(&t->requests);
	init_waitqueue_head(&t->wqh);
	spin_lock_init(&t->requests_guard);
	t->t = kthread_create(run, (void *)t, "%s", t->name);
	if (IS_ERR(t->t)) {
		xetrace("Fail to create worker_thread %s thread: %ld\n", t->name,
			PTR_ERR(t->t));
		goto freep;
	}
	else {
		/* increments usage counter so the thread can exit without
		   waiting for kthread_stop.  Otherwise if the thread leaves before
		   the call to kthread_stop we will crash when we eventually
		   call to kthread_stop.
		*/
		get_task_struct(t->t);
		/* beyond this point kthread_stop() MUST be called  */
	}
	wake_up_process(t->t);
	goto out;

freep:
	if (t->name)
		kfree(t->name);
	kfree(t);
	t = NULL;

out:
	FOUT;
	return t;
}

int wth_push_request(struct wth *t, struct request_base *r)
{
	unsigned long flags;

	FIN;
	spin_lock_irqsave(&t->requests_guard, flags);
	list_add_tail(&r->link, &t->requests);
	spin_unlock_irqrestore(&t->requests_guard, flags);
	wake_up(&t->wqh);
	FOUT;
	return 0;
}

void wth_free(struct wth *t)
{
	FIN;
	wth_send_stop(t);
	kthread_stop(t->t);
	put_task_struct(t->t);
	clear_requests(t, &t->requests);
	if (t->name)
		kfree(t->name);
	kfree(t);
	FOUT;
}

int wth_send_stop(struct wth *t)
{
	struct request_base *r;
	int rv;

	FIN;
	if ((r = kzalloc(sizeof(*r), GFP_KERNEL))) {
		r->type = rpcrdma_stop;
		wth_push_request(t, r);
		rv = 0;
	}
	else
		rv = -1;
	FOUT;
	return rv;
}

bool wth_is_current(struct wth *t)
{
	return t->t == current;
}

