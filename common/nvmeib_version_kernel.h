#ifndef NVMEIB_VERSION_KERNEL_H
#define NVMEIB_VERSION_KERNEL_H

/* Create/Remove API - Not Thread Safe */
struct proc_dir_entry;
int nvmeib_version_proc_create(struct proc_dir_entry *dir);
void nvmeib_version_proc_remove(void);

int nvmeib_metrics_meta_proc_create(struct proc_dir_entry *proc_dir);
void nvmeib_metrics_meta_proc_remove(void);

#endif /* NVMEIB_VERSION_KERNEL_H */
