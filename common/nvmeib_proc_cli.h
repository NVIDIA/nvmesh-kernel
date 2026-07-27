#ifndef NVMEIBC_PROC_CLI
#define NVMEIBC_PROC_CLI

struct msgloop_procfs_ent;

void *nvmeib_cli_init(void);
void  nvmeib_cli_remove(void *cli);
int   nvmeib_cli_send_proc(void *cli, void *msg, size_t len, struct msgloop_procfs_ent *ent);
#endif