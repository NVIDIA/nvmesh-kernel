#ifndef NVMEIBS_MCS_H
#define NVMEIBS_MCS_H

#include "kr_incs.h"

#define nvmeibs_mcs_snprintf_s(d, s) snprintf(d, sizeof(d), "%s", s)
#define nvmeibs_mcs_snprintf_u16(d, u) snprintf(d, sizeof(d), "%hu", u)

int nvmeibs_mcs_send(void *buf, int len);
typedef void nvmeibs_mcs_recv_cb(void *buf, int len);

int nvmeibs_mcs_create(struct proc_dir_entry *proc_dir,
	nvmeibs_mcs_recv_cb *recv_cb);
void nvmeibs_mcs_destroy(void);

#endif /* NVMEIBS_MCS_H */

