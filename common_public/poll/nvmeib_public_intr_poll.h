#ifndef NVMEIB_PUBLIC_INTR_POLL_H
#define NVMEIB_PUBLIC_INTR_POLL_H

#include "kr_incs.h"

struct nvmeib_irq_poll {
	struct irq_poll iop;

	/* Barriers for when scheduling between CPUs 
	 * (cannot use ipoller lock of new CPU as it may also be in the list of the ipoller of another CPU) */
	atomic_t sched;
	atomic_t disabled;

	bool 	poll_linger;
	unsigned long last_nonempty_comp_jif;
};

int nvmeib_public_intr_pollers_start(void);
void nvmeib_public_intr_pollers_stop(void);
void nvmeib_public_intr_poll_init(
	struct nvmeib_irq_poll *p, int weight, irq_poll_fn *poll_fn);
void nvmeib_public_intr_poll_sched(struct nvmeib_irq_poll *p, int cpu);
void nvmeib_public_intr_poll_complete(struct nvmeib_irq_poll *p);
bool nvmeib_public_intr_poll_is_sched(struct nvmeib_irq_poll *p);
void nvmeib_public_intr_poll_disable(struct nvmeib_irq_poll *iop);
void nvmeib_public_intr_poll_enable(struct nvmeib_irq_poll *iop);

struct nvmeib_intr_pollers_ft {
	void (*init)(struct nvmeib_irq_poll *p, int weight, irq_poll_fn *poll_fn);
	void (*sched)(struct nvmeib_irq_poll *p, int cpu);
	void (*complete)(struct nvmeib_irq_poll *p);
	bool (*is_sched)(struct nvmeib_irq_poll *p);
	void (*disable)(struct nvmeib_irq_poll *p);
	void (*enable)(struct nvmeib_irq_poll *p);
};

#endif


