#ifndef WTH_H_INCLUDED
#define WTH_H_INCLUDED

struct wth_info {
	const char *name;
	void *owner;
	void (*on_start)(void *);
	void (*on_exit)(void *);
};

struct wth;
struct request_base;
struct wth * wth_create(struct wth_info *p);
void wth_free(struct wth *t);
int wth_push_request(struct wth *t, struct request_base *r);
int wth_send_stop(struct wth *t);
bool wth_is_current(struct wth *t);

#endif

