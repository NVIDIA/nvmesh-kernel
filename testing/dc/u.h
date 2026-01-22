#ifndef U_H_INCLUDED
#define U_H_INCLUDED

#include "kr_incs.h"

#define trace(fmt, ...) pr_err("nvmesh:(%d)[%s:%s:%d]: " fmt, current->pid, __FILE__, __FUNCTION__, __LINE__, ## __VA_ARGS__)
#define FIN trace("-->\n")
#define FOUT trace("<--\n")
#define LINE trace("---\n")

#define DC_PROC_STR "dc_test"
#define DC_PROC_S_STR "dc_test_s"
#define DC_PROC_C_STR "dc_test_c"
#define DC_PROC_IP_CHAR 'E'
#define DC_PROC_IB_CHAR 'I'
#define DC_PROC_IB_ADDR_BUFFER 64
typedef int dc_proc_chng_cb(void *arg, char *buf, int len);
struct proc_dir_entry;
void * dc_proc_create(char *name, struct proc_dir_entry *dir,
	dc_proc_chng_cb *chng, void *arg);
void dc_proc_remove(void *p);
struct workqueue_struct;
int start_server_thread(struct workqueue_struct **wq, const char *name);

#endif

