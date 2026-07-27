#include "kr_incs.h"

#ifdef __KERNEL__
#include <linux/smp.h>
#include <linux/percpu.h>
#endif

#include "nvmeib.h"
#include "nvmeib_pcpu_wq.h"
#include "nvmeib_utils.h"
#include "nvmeibm_trace.h"
#include "kr_incs.h"
#include "nvmeib_q.h"

#define nvmeib_debug_level nvmeib_cmn_debug_level

struct nvmeib_pcpu_wq {
	struct workq_struct **wqs;
	unsigned int num_cpus;
	char name[32];
};

int nvmeib_pcpu_wq_trace_debug_level = NVMEIB_Q_LOG_LEVEL_VERBOSE; /* TRACE */
module_param_named(pcpu_wq_debug_level, nvmeib_pcpu_wq_trace_debug_level, int, 0644);
MODULE_PARM_DESC(pcpu_wq_debug_level, "Set trace level for nvmeib_pcpu_wq");

#define NVMEIB_PCPU_WQ_LOG(cpu, level, level_num, name, fmt, ...) \
	do { \
		if (level_num <= nvmeib_pcpu_wq_trace_debug_level) \
			_N##level##_SCOPE(name, nvmeib_pcpu_wq_trace, "[wq_cpu=@UINT_CPU] " fmt, cpu, ##__VA_ARGS__); \
	} while(0);

#define NVMEIB_PCPU_WQ_ERROR(cpu, name, fmt, ...) NVMEIB_PCPU_WQ_LOG(cpu, E, 1, name, fmt, ##__VA_ARGS__)
#define NVMEIB_PCPU_WQ_WARN(cpu, name, fmt, ...) NVMEIB_PCPU_WQ_LOG(cpu, W, 2, name, fmt, ##__VA_ARGS__)
#define NVMEIB_PCPU_WQ_INFO(cpu, name, fmt, ...) NVMEIB_PCPU_WQ_LOG(cpu, I, 3, name, fmt, ##__VA_ARGS__)
#define NVMEIB_PCPU_WQ_TRACE(cpu, name, fmt, ...) NVMEIB_PCPU_WQ_LOG(cpu, T, 4, name, fmt, ##__VA_ARGS__)
#define NVMEIB_PCPU_WQ_DEBUG(cpu, name, fmt, ...) NVMEIB_PCPU_WQ_LOG(cpu, D, 5, name, fmt, ##__VA_ARGS__)

static int create_nvmeib_q_on_cpu(struct nvmeib_pcpu_wq *pcpu_wq, unsigned int cpu)
{
	char wq_name[64];
	int rv = 0;

	NFIN;
	
	if (cpu >= pcpu_wq->num_cpus) {
		NVMEIB_PCPU_WQ_ERROR(cpu, error_nvmeib_pcpu_wq_create_cpu_wq, 
		      "Invalid CPU number: @UINT_CPU >= @UINT_CPU", cpu, pcpu_wq->num_cpus);
		rv = -EINVAL;
		goto out;
	}

	if (pcpu_wq->wqs[cpu]) {
		NVMEIB_PCPU_WQ_WARN(cpu, warn_nvmeib_pcpu_wq_create_cpu_wq,
		     "Work queue already exists");
		rv = 0;
		goto out;
	}

	snprintf(wq_name, sizeof(wq_name), "%s_%u", pcpu_wq->name, cpu);
	pcpu_wq->wqs[cpu] = wq_create_on(wq_name, cpu);
	
	if (!pcpu_wq->wqs[cpu]) {
		NVMEIB_PCPU_WQ_ERROR(cpu, error_1_nvmeib_pcpu_wq_create_cpu_wq,
		      "Failed to create work queue");
		rv = -ENOMEM;
		goto out;
	}

	NVMEIB_PCPU_WQ_INFO(cpu, info_nvmeib_pcpu_wq_create_cpu_wq,
	     "Created work queue");

out:
	NFOUT;
	return rv;
}

static void nvmeib_pcpu_wq_destroy_cpu_wq(struct nvmeib_pcpu_wq *pcpu_wq, unsigned int cpu)
{
	NFIN;
	
	if (cpu >= pcpu_wq->num_cpus) {
		NVMEIB_PCPU_WQ_ERROR(cpu, error_nvmeib_pcpu_wq_destroy_cpu_wq,
		      "Invalid CPU number: @UINT_CPU >= @UINT_CPU", cpu, pcpu_wq->num_cpus);
		goto out;
	}

	if (!pcpu_wq->wqs[cpu]) {
		NVMEIB_PCPU_WQ_WARN(cpu, warn_nvmeib_pcpu_wq_destroy_cpu_wq,
		     "No work queue exists");
		goto out;
	}

	NVMEIB_PCPU_WQ_INFO(cpu, info_nvmeib_pcpu_wq_destroy_cpu_wq,
	     "Destroying work queue");
	
	/* Drain any remaining work before stopping the queue */
	wq_drain(pcpu_wq->wqs[cpu]);
	wq_destroy(pcpu_wq->wqs[cpu]);
	pcpu_wq->wqs[cpu] = NULL;

out:
	NFOUT;
}

struct nvmeib_pcpu_wq *nvmeib_pcpu_wq_create(const char *name)
{
	struct nvmeib_pcpu_wq *pcpu_wq = NULL;
	int cpu;
	int ret;

	NFIN;
	
	pcpu_wq = kzalloc(sizeof(*pcpu_wq), GFP_KERNEL);
	if (!pcpu_wq) {
		NVMEIB_PCPU_WQ_ERROR(0, error_nvmeib_pcpu_wq_create,
		      "Failed to allocate pcpu_wq structure");
		goto err_out;
	}

	pcpu_wq->num_cpus = num_possible_cpus();
	strlcpy(pcpu_wq->name, name, sizeof(pcpu_wq->name));
	pcpu_wq->name[sizeof(pcpu_wq->name) - 1] = '\0';

	/* Allocate array to hold per-CPU work queue pointers */
	pcpu_wq->wqs = kzalloc(sizeof(struct nvmeib_q *) * pcpu_wq->num_cpus, GFP_KERNEL);
	if (!pcpu_wq->wqs) {
		NVMEIB_PCPU_WQ_ERROR(0, error_1_nvmeib_pcpu_wq_create,
		      "Failed to allocate work queue array");
		goto err_destroy;
	}

	/* Create work queues for all currently online CPUs */
	for_each_online_cpu(cpu) {
		ret = create_nvmeib_q_on_cpu(pcpu_wq, cpu);
		if (ret < 0) {
			NVMEIB_PCPU_WQ_ERROR(cpu, error_2_nvmeib_pcpu_wq_create,
			      "Failed to create work queue for CPU @UINT_CPU: @RV", cpu, ret);
			goto err_destroy;
		}
	}

	NVMEIB_PCPU_WQ_INFO(0, info_nvmeib_pcpu_wq_create,
	     "Created pcpu_wq instance: @PCPU_WQ", name);
	goto out;

err_destroy:
	nvmeib_pcpu_wq_destroy(pcpu_wq);
	pcpu_wq = NULL;
	goto out;

err_out:
	pcpu_wq = NULL;

out:
	NFOUT;
	return pcpu_wq;
}
EXPORT_SYMBOL(nvmeib_pcpu_wq_create);

void nvmeib_pcpu_wq_destroy(struct nvmeib_pcpu_wq *pcpu_wq)
{
	unsigned int cpu;

	NFIN;
	
	if (!pcpu_wq) {
		NVMEIB_PCPU_WQ_WARN(0, warn_nvmeib_pcpu_wq_destroy,
		     "Attempting to destroy NULL pcpu_wq");
		goto out;
	}

	if (pcpu_wq->wqs) {
		/* Drain and destroy all work queues */
		for (cpu = 0; cpu < pcpu_wq->num_cpus; cpu++) {
			if (pcpu_wq->wqs[cpu]) {
				nvmeib_pcpu_wq_destroy_cpu_wq(pcpu_wq, cpu);
			}
		}
		kfree(pcpu_wq->wqs);
	}

	NVMEIB_PCPU_WQ_INFO(0, info_nvmeib_pcpu_wq_destroy,
	     "Destroyed pcpu_wq instance: @PCPU_WQ", pcpu_wq->name);
	
	kfree(pcpu_wq);

out:
	NFOUT;
}
EXPORT_SYMBOL(nvmeib_pcpu_wq_destroy);

bool nvmeib_pcpu_wq_add_work_on_core(struct nvmeib_pcpu_wq *pcpu_wq, 
					    unsigned int cpu, struct workqe_struct *entry)
{
	struct workq_struct *q;
	bool rv = false;

	NFIN;
	
	if (cpu == WORK_CPU_UNBOUND)
		cpu = smp_processor_id();

	if (!pcpu_wq || cpu >= pcpu_wq->num_cpus || !pcpu_wq->wqs || !pcpu_wq->wqs[cpu]) {
		NVMEIB_PCPU_WQ_ERROR(cpu, error_nvmeib_pcpu_wq_add_work,
		      "Invalid pcpu_wq or no work queue");
		rv = false;
		goto out;
	}

	q = pcpu_wq->wqs[cpu];
	rv = wq_add_work(q, entry);

out:
	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_pcpu_wq_add_work_on_core);


void nvmeib_pcpu_wq_flush_on_core(struct nvmeib_pcpu_wq *pcpu_wq, unsigned int cpu)
{
	struct workq_struct *q;

	NFIN;
	
	if (!pcpu_wq || cpu >= pcpu_wq->num_cpus || !pcpu_wq->wqs || !pcpu_wq->wqs[cpu]) {
		NVMEIB_PCPU_WQ_ERROR(cpu, error_nvmeib_pcpu_wq_flush,
		      "Invalid pcpu_wq or no work queue");
		goto out;
	}

	q = pcpu_wq->wqs[cpu];
	wq_flush(q);

out:
	NFOUT;
}
EXPORT_SYMBOL(nvmeib_pcpu_wq_flush_on_core);

void nvmeib_pcpu_wq_drain_on_core(struct nvmeib_pcpu_wq *pcpu_wq, unsigned int cpu)
{
	struct workq_struct *q;

	NFIN;
	
	if (!pcpu_wq || cpu >= pcpu_wq->num_cpus || !pcpu_wq->wqs || !pcpu_wq->wqs[cpu]) {
		NVMEIB_PCPU_WQ_ERROR(cpu, error_nvmeib_pcpu_wq_drain,
		      "Invalid pcpu_wq or no work queue");
		goto out;
	}

	q = pcpu_wq->wqs[cpu];
	wq_drain(q);

out:
	NFOUT;
}
EXPORT_SYMBOL(nvmeib_pcpu_wq_drain_on_core);

bool nvmeib_pcpu_wq_flush_work_on_core(struct nvmeib_pcpu_wq *pcpu_wq,
				       unsigned int cpu, struct workqe_struct *entry)
{
	struct workq_struct *q;
	bool ret = false;

	NFIN;

	if (!pcpu_wq || cpu >= pcpu_wq->num_cpus || !pcpu_wq->wqs || !pcpu_wq->wqs[cpu]) {
		NVMEIB_PCPU_WQ_ERROR(cpu, error_nvmeib_pcpu_wq_flush_work,
				     "Invalid pcpu_wq or no work queue");
		goto out;
	}

	q = pcpu_wq->wqs[cpu];
	ret = wq_flush_work(q, entry);

out:
	NFOUT;
	return ret;
}
EXPORT_SYMBOL(nvmeib_pcpu_wq_flush_work_on_core);

bool nvmeib_pcpu_wq_cancel_work_on_core(struct nvmeib_pcpu_wq *pcpu_wq,
					unsigned int cpu, struct workqe_struct *entry)
{
	struct workq_struct *q;
	bool ret = false;

	NFIN;

	if (!pcpu_wq || cpu >= pcpu_wq->num_cpus || !pcpu_wq->wqs || !pcpu_wq->wqs[cpu]) {
		NVMEIB_PCPU_WQ_ERROR(cpu, error_nvmeib_pcpu_wq_cancel_work,
				     "Invalid pcpu_wq or no work queue");
		goto out;
	}

	q = pcpu_wq->wqs[cpu];
	ret = wq_cancel_work(q, entry);

out:
	NFOUT;
	return ret;
}
EXPORT_SYMBOL(nvmeib_pcpu_wq_cancel_work_on_core);

struct workq_struct *nvmeib_pcpu_wq_get_queue(struct nvmeib_pcpu_wq *pcpu_wq, unsigned int cpu)
{
	struct workq_struct *rv = NULL;

	NFIN;

	if (!pcpu_wq || cpu >= pcpu_wq->num_cpus || !pcpu_wq->wqs) {
		rv = NULL;
		goto out;
	}
	
	rv = pcpu_wq->wqs[cpu];

out:
	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_pcpu_wq_get_queue);

/* No CPU hotplug support - simplified implementation */
