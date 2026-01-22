#ifndef NVMESH_XTRACE_H
#define NVMESH_XTRACE_H

#define xtrace(f, fmt, ...) f("nvmesh:(%s/%d/%d)[%s:%s:%d]: " fmt, current->comm, current->pid, raw_smp_processor_id(), kbasename(__FILE__), __FUNCTION__, __LINE__, ## __VA_ARGS__)
#define xdtrace(fmt, ...) xtrace(pr_devel, fmt, ## __VA_ARGS__)
#define xttrace(fmt, ...) xtrace(pr_info, fmt, ## __VA_ARGS__)
#define xwtrace(fmt, ...) xtrace(pr_warn, fmt, ## __VA_ARGS__)
#define xetrace(fmt, ...) xtrace(pr_err, fmt, ## __VA_ARGS__)
#define FIN xdtrace("-->\n")
#define FOUT xdtrace("<--\n")
#define LINE xdtrace("-----\n")

#endif
