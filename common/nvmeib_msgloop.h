#ifndef NVMEIBS_MSGLOOP_H
#define NVMEIBS_MSGLOOP_H

#include "kr_incs.h"

struct msgloop_msg {
	struct list_head link;
	size_t len;
	struct kref ref_cnt;
	char data[];
};

struct msgloop_procfs_ent;

struct msg_vec {
	char *data;
	size_t len;
};

struct msgloop_procfs_ent *nvmeib_msgloop_create(char *name,
	struct proc_dir_entry *dir, int (*cb)(void *, char *, size_t, bool *posted),
	void (*on_open_cb)(void *), void (*on_close_cb)(void *), void *arg);

void nvmeib_msgloop_set_ready_cb(struct msgloop_procfs_ent *p,
				 void (*ready_cb)(void *));

struct msgloop_msg *nvmeib_msgloop_alloc_msg(size_t data_size);

void msgloop_get_msg(void *msg);
void msgloop_put_msg(void *msg);

void nvmeib_msgloop_set_max(struct msgloop_procfs_ent *p, int max_msg);
void nvmeib_msgloop_remove(struct msgloop_procfs_ent *p);
size_t nvmeib_msgloop_flush(struct msgloop_procfs_ent *p);
int  nvmeib_msgloop_get_count(const struct msgloop_procfs_ent *p);

int nvmeib_msgloop_send(struct msgloop_procfs_ent *p, char *data, size_t len);
int nvmeib_msgloop_sendv(struct msgloop_procfs_ent *p, struct msg_vec *vec, int cnt);
int nvmeib_msgloop_sendl(struct msgloop_procfs_ent *p, struct list_head *l);
#endif /* NVMEIBS_MSGLOOP_H */
