#ifndef NVMEIB_COMPLETION_NOISE_H
#define NVMEIB_COMPLETION_NOISE_H

#include "kr_incs.h"


enum nvmeib_noise_type {
	NVMEIB_NOISE_COMPLETION = 0,
	NVMEIB_NOISE_SUBMISSION,
	NVMEIB_NOISE_INTERRUPT,
};

enum nvmeib_noise_ctrs {
	NVMEIB_NOISE_CTRS_CQ_INTR,
	NVMEIB_NOISE_CTRS_NVMEIBC_INTR,
	NVMEIB_NOISE_CTRS_IO_PCPU_CHANNEL_NOT_IN_MASK,
	NVMEIB_NOISE_CTRS_IO_COMPLETE_CB,
	NVMEIB_NOISE_CTRS_IO_COMPLETE_CB_PCPU_WQ,
	NVMEIB_NOISE_CTRS_DEFER_COMPLETE_IOCMD,
	NVMEIB_NOISE_CTRS_LOCK_DEFER_CB_PCPU_WQ,
	NVMEIB_NOISE_CTRS_LOCK_CB,
	NVMEIB_NOISE_CTRS_NORDDA_PENDING_IO,
	NVMEIB_NOISE_CTRS_NORDDA_SEND_COMP,
	NVMEIB_NOISE_CTRS_LOCK_SUBMISSION,

	/* marker for local only counters */
	NVMEIBS_NOISE_CTRS_LOCAL_ONLY_START,
	NVMEIB_NOISE_CTRS_NVMEIBS_INTR = NVMEIBS_NOISE_CTRS_LOCAL_ONLY_START,
	NVMEIB_NOISE_CTRS_LOCAL_IO_PCPU_WQ,
	NVMEIB_NOISE_CTRS_LOCAL_IO_CB,
	NVMEIB_NOISE_CTRS_DISK_INTR,
	NVMEIB_NOISE_CTRS_LOCK_SUBMISSION_LOCAL,
	NVMEIB_NOISE_CTRS_LOCK_DEFER_CB_PCPU_WQ_LOCAL,
	NVMEIBS_NOISE_CTRS_LOCAL_ONLY_MAX,

	NVMEIB_NOISE_CTRS_MAX,
};

int nvmeib_completion_noise_init(void);
void nvmeib_completion_noise_exit(void);
void nvmeib_completion_noise_start(enum nvmeib_noise_type type);
void nvmeib_completion_noise_end(enum nvmeib_noise_type type, const unsigned long *cpu_mask_bitmap, int bitmap_size,
	enum nvmeib_noise_ctrs ctr);
ssize_t nvmeib_completion_noise_fill_stats(void *priv, char *buf, size_t len);
ssize_t nvmeib_completion_noise_fill_stats_local(void *priv, char *buf, size_t len);
ssize_t nvmeib_completion_noise_reset_stats(void *priv, char *buf, size_t len);

#endif /* NVMEIB_COMPLETION_NOISE_H */ 