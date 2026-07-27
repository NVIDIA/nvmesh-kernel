#include "nvmeib_completion_noise.h"
#include "nvmeib.h"
#include "compat/kr_incs_time_rdtsc.h"
#include "nvmeib_jdr.h" 
#include "linux/time64.h"
#include "linux/hrtimer.h"

#define MAX_NOISE_THRESHOLD_LEVELS 5 /* Maximum number of threshold levels supported */

/* Counter names for JSON output - must match enum nvmeib_noise_ctrs order */
static const char *nvmeib_noise_ctr_names[NVMEIB_NOISE_CTRS_MAX] = {
	[NVMEIB_NOISE_CTRS_NVMEIBC_INTR] = "nvmeibc_intr",
	[NVMEIB_NOISE_CTRS_NVMEIBS_INTR] = "nvmeibs_intr",
	[NVMEIB_NOISE_CTRS_CQ_INTR] = "cq_intr",
	[NVMEIB_NOISE_CTRS_DISK_INTR] = "disk_intr",
	[NVMEIB_NOISE_CTRS_LOCAL_IO_PCPU_WQ] = "local_io_pcpu_wq",
	[NVMEIB_NOISE_CTRS_LOCAL_IO_CB] = "local_io_cb",
	[NVMEIB_NOISE_CTRS_IO_PCPU_CHANNEL_NOT_IN_MASK] = "io_pcpu_channel_not_in_mask",
	[NVMEIB_NOISE_CTRS_IO_COMPLETE_CB] = "io_complete_cb",
	[NVMEIB_NOISE_CTRS_IO_COMPLETE_CB_PCPU_WQ] = "io_complete_cb_pcpu_wq",
	[NVMEIB_NOISE_CTRS_DEFER_COMPLETE_IOCMD] = "defer_complete_iocmd",
	[NVMEIB_NOISE_CTRS_LOCK_DEFER_CB_PCPU_WQ] = "lock_defer_cb_pcpu_wq",
	[NVMEIB_NOISE_CTRS_LOCK_CB] = "lock_cb",
	[NVMEIB_NOISE_CTRS_NORDDA_PENDING_IO] = "nordda_pending_io",
	[NVMEIB_NOISE_CTRS_NORDDA_SEND_COMP] = "nordda_send_comp",
	[NVMEIB_NOISE_CTRS_LOCK_SUBMISSION] = "lock_submission",
	[NVMEIB_NOISE_CTRS_LOCK_DEFER_CB_PCPU_WQ_LOCAL] = "lock_defer_cb_pcpu_wq_local",
	[NVMEIB_NOISE_CTRS_LOCK_SUBMISSION_LOCAL] = "lock_submission_local",
};

struct noise_exceedance_stats {
	u64 count;              /* Number of times threshold was exceeded */
	u64 total_duration_us;  /* Total duration of all exceedances for average calculation */
	u64 min_duration_us;    /* Minimum exceedance duration */
	u64 max_duration_us;    /* Maximum exceedance duration */
};

struct threshold_level_stats {
	u64 threshold_start_cycles;        /* When threshold was first exceeded (in cycles) */
	bool threshold_exceeded;           /* Whether threshold is currently exceeded */
	u64 first_cycles_at_threshold_exceeded;  /* First cycles at threshold exceeded */
	u64 max_exceedance_duration_us;    /* Maximum duration of threshold exceedance in microseconds */
	struct noise_exceedance_stats exceedance_stats;  /* Statistics for this threshold level */
};

struct nvmeib_completion_noise_stats {
	u64 current_start_tick;         /* For completion measurements */
	u64 current_start_tick_submission;    /* For submission measurements */
	u64 current_start_tick_intr;    /* For interrupt measurements */

	/* Accumulated noisy cycles for current interval */
	u64 accumulated_noisy_cycles;
	u64 accumulated_local_noisy_cycles;  /* For LOCAL_IO_CB and NVMEIBS_INTR only */

	/* Unified threshold tracking for all levels */
	struct threshold_level_stats threshold_levels[MAX_NOISE_THRESHOLD_LEVELS];
	struct threshold_level_stats with_local_threshold_levels[MAX_NOISE_THRESHOLD_LEVELS];  /* For local noise tracking */

	u64 submission_cycles_while_completion;  /* Submissions during completion */
	u64 intr_cycles_while_submission;        /* Interrupts during submission */
	u64 intr_cycles_while_completion;
	u64 ctrs[NVMEIB_NOISE_CTRS_MAX];
	u64 empty_mask_ctrs[NVMEIB_NOISE_CTRS_MAX];

	u64 cycles_start_interval;
};

struct nvmeib_completion_noise_pcpu {
	/* Per-CPU high-resolution timer for periodic measurement */
	struct hrtimer measurement_hrtimer;
	
	/* Stats - keep this last for easy container_of usage */
	struct nvmeib_completion_noise_stats stats;
};

static struct nvmeib_completion_noise_pcpu __percpu *completion_noise_stats;

static bool nvmeib_completion_noise_enabled = false;
module_param_named(completion_noise_enabled, nvmeib_completion_noise_enabled, bool, 0444);
MODULE_PARM_DESC(completion_noise_enabled, "Enable completion noise statistics collection");

static unsigned int nvmeib_completion_noise_measurement_interval_ms = 55; /* Calculated based on <average io time of 4k io> * 100 */

module_param_named(completion_noise_measurement_interval_ms, nvmeib_completion_noise_measurement_interval_ms, uint, 0444);
MODULE_PARM_DESC(completion_noise_measurement_interval_ms, "Measurement interval in milliseconds for noise statistics");

static int nvmeib_completion_noise_threshold_percentages[MAX_NOISE_THRESHOLD_LEVELS] = {1, 5, 10, 20, 50};
static int nvmeib_completion_noise_threshold_percentages_size = ARRAY_SIZE(nvmeib_completion_noise_threshold_percentages); /* Default number of thresholds */

module_param_array(nvmeib_completion_noise_threshold_percentages, int, &nvmeib_completion_noise_threshold_percentages_size, 0444);
MODULE_PARM_DESC(nvmeib_completion_noise_threshold_percentages, "Noise thresholds as CPU percentage (default: 1,5,10,20,50)");

static inline u64 cycles_to_us(u64 cycles)
{
	u64 tsc_freq_khz = (u64)nvmeib_public_tsc_khz();
	if (tsc_freq_khz == 0)
		return 0;
	
	return (cycles * 1000ULL) / tsc_freq_khz;
}

static void process_threshold_levels(u64 accumulated_cycles, u64 cycles_per_interval, u64 current_cycles,
				    struct threshold_level_stats *threshold_levels)
{
	int i;
	
	for (i = 0; i < min(nvmeib_completion_noise_threshold_percentages_size, MAX_NOISE_THRESHOLD_LEVELS); i++) {
		u64 threshold_cycles = (cycles_per_interval * nvmeib_completion_noise_threshold_percentages[i]) / 100ULL;
		bool threshold_exceeded_this_interval = (accumulated_cycles >= threshold_cycles);
		u64 exceedance_duration_cycles = current_cycles - threshold_levels[i].threshold_start_cycles;
		u64 exceedance_duration_us = cycles_to_us(exceedance_duration_cycles);
		u64 cycles_since_threshold_exceeded;
		
		if (threshold_exceeded_this_interval) {
			/* We pass threshold */
			if (!threshold_levels[i].threshold_exceeded) {
				threshold_levels[i].threshold_exceeded = true;
				threshold_levels[i].first_cycles_at_threshold_exceeded = threshold_levels[i].threshold_start_cycles;
			}
			cycles_since_threshold_exceeded = current_cycles - threshold_levels[i].first_cycles_at_threshold_exceeded;			
			threshold_levels[i].exceedance_stats.total_duration_us += exceedance_duration_us;
			threshold_levels[i].exceedance_stats.count++;
			threshold_levels[i].exceedance_stats.min_duration_us = min(threshold_levels[i].exceedance_stats.min_duration_us, exceedance_duration_us);
			threshold_levels[i].exceedance_stats.max_duration_us = max(threshold_levels[i].exceedance_stats.max_duration_us, cycles_to_us(cycles_since_threshold_exceeded));
		} else {
			/* We didn't pass threshold */
			threshold_levels[i].threshold_exceeded = false;
		}
		threshold_levels[i].threshold_start_cycles = current_cycles;
	}
}

/* We'll calculate thresholds dynamically in the timer callback based on actual CPU frequency */

/* Threshold calculation is now done dynamically in the timer callback */

static enum hrtimer_restart percpu_noise_measurement_hrtimer_callback(struct hrtimer *timer)
{
	struct nvmeib_completion_noise_pcpu *noise;
	struct nvmeib_completion_noise_stats *stats;
	u64 cycles_per_interval;
	ktime_t interval;
	u64 current_cycles;
	unsigned long flags;

	if (!nvmeib_completion_noise_enabled) {
		BUG();
	}

	/* Get the per-CPU data from the hrtimer */
	noise = container_of(timer, struct nvmeib_completion_noise_pcpu, measurement_hrtimer);
	stats = &noise->stats;

	local_irq_save(flags);

	current_cycles = nvmeib_public_get_cycles();
	cycles_per_interval = current_cycles - stats->cycles_start_interval;
	       
	process_threshold_levels(stats->accumulated_noisy_cycles, cycles_per_interval, current_cycles, stats->threshold_levels);
	process_threshold_levels(stats->accumulated_noisy_cycles + stats->accumulated_local_noisy_cycles, cycles_per_interval, current_cycles, stats->with_local_threshold_levels);
	
	/* Reset accumulated cycles for next interval */
	stats->accumulated_noisy_cycles = 0;
	stats->accumulated_local_noisy_cycles = 0;
	stats->cycles_start_interval = nvmeib_public_get_cycles();

	local_irq_restore(flags);

	/* Reschedule this CPU's hrtimer */
	interval = ms_to_ktime(nvmeib_completion_noise_measurement_interval_ms);
	nvmeib_public_hrtimer_forward(timer, hrtimer_cb_get_time(timer), interval);
	return HRTIMER_RESTART;
}

static void nvmeib_completion_noise_reset_cpu_stats(struct nvmeib_completion_noise_pcpu *noise)
{
	int i;
	struct nvmeib_completion_noise_stats *stats = &noise->stats;
	u64 current_cycles = nvmeib_public_get_cycles();
	
	/* Reset stats without touching the timer - just memset the stats struct */
	memset(stats, 0, sizeof(*stats));
	
	/* Initialize min_duration_us to max value for all threshold levels */
	for (i = 0; i < min(nvmeib_completion_noise_threshold_percentages_size, MAX_NOISE_THRESHOLD_LEVELS); i++) {
		/* Initialize regular threshold levels */
		stats->threshold_levels[i].exceedance_stats.min_duration_us = U64_MAX;
		stats->threshold_levels[i].threshold_start_cycles = current_cycles;
		
		/* Initialize local threshold levels */
		stats->with_local_threshold_levels[i].exceedance_stats.min_duration_us = U64_MAX;
		stats->with_local_threshold_levels[i].threshold_start_cycles = current_cycles;
	}
	stats->cycles_start_interval = current_cycles;
}

static void nvmeib_completion_noise_init_cpu_timer(struct nvmeib_completion_noise_pcpu *noise)
{
	/* Initialize per-CPU high-resolution timer pinned to this CPU */
	nvmeib_public_hrtimer_init(&noise->measurement_hrtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED);
	noise->measurement_hrtimer.function = percpu_noise_measurement_hrtimer_callback;
}

static void nvmeib_completion_noise_init_cpu(struct nvmeib_completion_noise_pcpu *noise)
{
	nvmeib_completion_noise_reset_cpu_stats(noise);
	nvmeib_completion_noise_init_cpu_timer(noise);
}

static void __start_timer(struct nvmeib_completion_noise_pcpu *noise)
{
	nvmeib_public_hrtimer_start(&noise->measurement_hrtimer, 
		      ms_to_ktime(nvmeib_completion_noise_measurement_interval_ms), 
		      HRTIMER_MODE_REL_PINNED);
}

/* Function to initialize and start timer on specific CPU */
static void init_and_start_cpu_timer(void *unused)
{
	struct nvmeib_completion_noise_pcpu *noise;
	
	BUG_ON(!nvmeib_completion_noise_enabled);
	
	noise = this_cpu_ptr(completion_noise_stats);
	
	/* Initialize the timer on this CPU */
	nvmeib_completion_noise_init_cpu(noise);
	
	/* Start this CPU's high-resolution timer pinned to this CPU */
	__start_timer(noise);
}

/* Function to stop timer on specific CPU */
static void stop_cpu_timer(void *unused)
{
	struct nvmeib_completion_noise_pcpu *noise;

	BUG_ON(!completion_noise_stats);
	noise = this_cpu_ptr(completion_noise_stats);
	nvmeib_public_hrtimer_cancel(&noise->measurement_hrtimer);
}

int nvmeib_completion_noise_init(void)
{
	int i;

	if (!nvmeib_completion_noise_enabled)
		return 0;

	/* Validate threshold percentages */
	if (nvmeib_completion_noise_threshold_percentages_size <= 0) {
		pr_err("nvmeib: must specify at least one threshold\n");
		return -EINVAL;
	}
	
	if (nvmeib_completion_noise_threshold_percentages_size > MAX_NOISE_THRESHOLD_LEVELS) {
		pr_err("nvmeib: too many thresholds (max %d)\n", MAX_NOISE_THRESHOLD_LEVELS);
		return -EINVAL;
	}
	
	/* Validate each threshold value */
	for (i = 0; i < nvmeib_completion_noise_threshold_percentages_size; i++) {
		if (nvmeib_completion_noise_threshold_percentages[i] <= 0 || 
		    nvmeib_completion_noise_threshold_percentages[i] > 100) {
			pr_err("nvmeib: threshold percentage %d is invalid (must be 1-100)\n", 
			       nvmeib_completion_noise_threshold_percentages[i]);
			return -EINVAL;
		}
	}
	
	/* Check if values are in ascending order */
	for (i = 1; i < nvmeib_completion_noise_threshold_percentages_size; i++) {
		if (nvmeib_completion_noise_threshold_percentages[i] <= nvmeib_completion_noise_threshold_percentages[i-1]) {
			pr_err("nvmeib: threshold percentages must be in ascending order\n");
			return -EINVAL;
		}
	}

	/* Threshold calculation is now done dynamically in per-CPU timer callbacks */
	completion_noise_stats = nvmeib_public_alloc_percpu_cacheline(struct nvmeib_completion_noise_pcpu);
	if (!completion_noise_stats) {
		pr_err("Failed to allocate per-CPU completion noise statistics\n");
		return -ENOMEM;
	}	
	/* Initialize and start timers on each CPU separately to ensure CPU affinity */
	on_each_cpu(init_and_start_cpu_timer, NULL, 1 /* wait */);

	return 0;
}

void nvmeib_completion_noise_exit(void)
{
	if (nvmeib_completion_noise_enabled && completion_noise_stats) {
		/* Stop all per-CPU high-resolution timers on their respective CPUs */
		on_each_cpu(stop_cpu_timer, NULL, 1 /* wait */);
	}

	if (completion_noise_stats) {
		nvmeib_public_free_percpu(completion_noise_stats);
		completion_noise_stats = NULL;
	}
}

/* Function to start noise measurement */
void nvmeib_completion_noise_start(enum nvmeib_noise_type type)
{
	struct nvmeib_completion_noise_pcpu *noise;
	struct nvmeib_completion_noise_stats *stats;
	unsigned long flags;
	u64 current_cycles;

	if (!nvmeib_completion_noise_enabled)
		return;

	local_irq_save(flags);
	noise = this_cpu_ptr(completion_noise_stats);
	stats = &noise->stats;
	current_cycles = nvmeib_public_get_cycles();

	switch (type) {
	case NVMEIB_NOISE_COMPLETION:
		stats->current_start_tick = current_cycles;
		stats->submission_cycles_while_completion = 0;
		stats->intr_cycles_while_completion = 0;
		break;
	case NVMEIB_NOISE_SUBMISSION:
		stats->current_start_tick_submission = current_cycles;
		stats->intr_cycles_while_submission = 0;
		break;
	case NVMEIB_NOISE_INTERRUPT:
		stats->current_start_tick_intr = current_cycles;
		break;
	default:
		break;
	}

	local_irq_restore(flags);
}
EXPORT_SYMBOL(nvmeib_completion_noise_start);

void nvmeib_completion_noise_end(enum nvmeib_noise_type type, const unsigned long *cpu_mask_bitmap, int bitmap_size,
				enum nvmeib_noise_ctrs ctr)
{
	struct nvmeib_completion_noise_pcpu *noise;
	struct nvmeib_completion_noise_stats *stats;
	u64 end_cycles, noise_cycles, start_tick;
	int current_cpu;
	bool is_noisy = false;
	bool has_cpu_mask;
	unsigned long flags;
	
	if (!nvmeib_completion_noise_enabled)
		goto out;

	has_cpu_mask = cpu_mask_bitmap && !bitmap_empty(cpu_mask_bitmap, bitmap_size);
	current_cpu = get_cpu();
	if (has_cpu_mask && test_bit(current_cpu, cpu_mask_bitmap)) {
		/* This is not noisy */
		goto put_cpu;
	}
	
	/* prevent irqs to get in as they also use the stats per cpu */
	local_irq_save(flags);

	noise = this_cpu_ptr(completion_noise_stats);
	stats = &noise->stats;
	end_cycles = nvmeib_public_get_cycles();

	switch (type) {
	case NVMEIB_NOISE_COMPLETION:
		start_tick = stats->current_start_tick;
		noise_cycles = end_cycles - start_tick - stats->submission_cycles_while_completion - stats->intr_cycles_while_completion;
		break;
	case NVMEIB_NOISE_SUBMISSION:
		start_tick = stats->current_start_tick_submission;
		noise_cycles = end_cycles - start_tick - stats->intr_cycles_while_submission;
		if (stats->current_start_tick != 0)
			stats->submission_cycles_while_completion += noise_cycles;
		break;
	case NVMEIB_NOISE_INTERRUPT:
		start_tick = stats->current_start_tick_intr;
		noise_cycles = end_cycles - start_tick;
		if (stats->current_start_tick != 0)
			stats->intr_cycles_while_completion += noise_cycles;
		if (stats->current_start_tick_submission != 0)
			stats->intr_cycles_while_submission += noise_cycles;
		break;
	default:
		WARN_ON_ONCE(1);
		goto irq_restore;
	}

	WARN_ON(start_tick == 0);
	/* Simply accumulate noisy cycles - all analysis happens in timer callback */
	if (ctr >= NVMEIBS_NOISE_CTRS_LOCAL_ONLY_START && ctr < NVMEIBS_NOISE_CTRS_LOCAL_ONLY_MAX) {
		stats->accumulated_local_noisy_cycles += noise_cycles;
	} else {
		stats->accumulated_noisy_cycles += noise_cycles;
	}
	
	if (has_cpu_mask) { 
		_NE(nvmeib_completion_noise_end_t1, "noisy completion, current_cpu=@INT, type=@INT, is_noisy=@BOOL has_cpu_mask=@BOOL", current_cpu, type, is_noisy, has_cpu_mask);
		stats->ctrs[ctr]++;
	} else {
		_ND(nvmeib_completion_noise_end_t2, "current_cpu=@INT, type=@INT, is_noisy=@BOOL has_cpu_mask=@BOOL", current_cpu, type, is_noisy, has_cpu_mask);
		stats->empty_mask_ctrs[ctr]++;
	}

irq_restore:
	local_irq_restore(flags);
put_cpu:
	put_cpu();
out:
	return;
}
EXPORT_SYMBOL(nvmeib_completion_noise_end);

static u64 get_average_exceedance_duration_us(const struct noise_exceedance_stats *stats)
{
	if (stats->count == 0)
		return 0;
	return stats->total_duration_us / stats->count;
}

static void write_threshold_exceedances_json(struct jdr *jdr_inst, const char *array_name,
					     const struct threshold_level_stats *threshold_levels,
					     int max_thresh_idx)
{
	jdr_array_scope(jdr_inst, array_name);
	int thresh_idx;
	for (thresh_idx = 0; thresh_idx < max_thresh_idx; thresh_idx++) {
		u64 avg_duration_us = get_average_exceedance_duration_us(&threshold_levels[thresh_idx].exceedance_stats);
		
		jdr_object_scope(jdr_inst, NULL);
		jdr_write_var(jdr_inst, threshold_percentage, nvmeib_completion_noise_threshold_percentages[thresh_idx]);
		jdr_write_var(jdr_inst, count, threshold_levels[thresh_idx].exceedance_stats.count);
		jdr_write_var(jdr_inst, min_duration_us, 
						threshold_levels[thresh_idx].exceedance_stats.min_duration_us == U64_MAX ? 0 : threshold_levels[thresh_idx].exceedance_stats.min_duration_us);
		jdr_write_var(jdr_inst, max_duration_us, threshold_levels[thresh_idx].exceedance_stats.max_duration_us);
		jdr_write_var(jdr_inst, avg_duration_us, avg_duration_us);
	}
}

struct cpu_noise_data_per_cpu {
	struct threshold_level_stats threshold_levels[MAX_NOISE_THRESHOLD_LEVELS];
	struct threshold_level_stats with_local_threshold_levels[MAX_NOISE_THRESHOLD_LEVELS];
	u64 ctrs[NVMEIB_NOISE_CTRS_MAX];
	u64 empty_mask_ctrs[NVMEIB_NOISE_CTRS_MAX];
	u64 accumulated_local_noisy_cycles;
};

struct cpu_noise_data {
	struct cpu_noise_data_per_cpu __percpu *percpu_data;
};

static void collect_cpu_noise_data(void *data)
{
	struct cpu_noise_data *noise_data = data;
	struct nvmeib_completion_noise_pcpu *noise;
	struct nvmeib_completion_noise_stats *stats;
	struct cpu_noise_data_per_cpu *cpu_data;
	int i;
	unsigned long flags;

	/* this runs on ipi */
	
	noise = this_cpu_ptr(completion_noise_stats);
	stats = &noise->stats;
	cpu_data = this_cpu_ptr(noise_data->percpu_data);
	
	local_irq_save(flags);
	
	/* Copy threshold level stats for all threshold levels */
	for (i = 0; i < min(nvmeib_completion_noise_threshold_percentages_size, MAX_NOISE_THRESHOLD_LEVELS); i++) {
		cpu_data->threshold_levels[i] = stats->threshold_levels[i];
		cpu_data->with_local_threshold_levels[i] = stats->with_local_threshold_levels[i];
	}
	
	/* Copy counter arrays */
	for (i = 0; i < NVMEIB_NOISE_CTRS_MAX; i++) {
		cpu_data->ctrs[i] = stats->ctrs[i];
		cpu_data->empty_mask_ctrs[i] = stats->empty_mask_ctrs[i];
	}
	
	/* Copy local noise cycles */
	cpu_data->accumulated_local_noisy_cycles = stats->accumulated_local_noisy_cycles;
	
	local_irq_restore(flags);
}

static ssize_t nvmeib_completion_noise_fill_stats_impl(void *priv, char *buf, size_t len, bool use_local)
{
	struct jdr jdr_inst = jdr_make((struct charvec){.base = buf, .len = len});
	int cpu;
	struct cpu_noise_data noise_data;
	int online_cpu_count = 0;
	int current_cpu_index = 0;
	int max_thresh_idx = min(nvmeib_completion_noise_threshold_percentages_size, MAX_NOISE_THRESHOLD_LEVELS);
	int max_ctr_idx = use_local ? NVMEIBS_NOISE_CTRS_LOCAL_ONLY_MAX : NVMEIBS_NOISE_CTRS_LOCAL_ONLY_START;

	if (!nvmeib_completion_noise_enabled) {
		jdr_write_var(&jdr_inst, enabled, false);
		jdr_write_var(&jdr_inst, message, (char const*)"Completion noise statistics disabled");
		goto finalize;
	}
	
	noise_data.percpu_data = nvmeib_public_alloc_percpu_cacheline(struct cpu_noise_data_per_cpu);
	if (!noise_data.percpu_data) {
		jdr_write_var(&jdr_inst, enabled, true);
		jdr_write_var(&jdr_inst, error, (char const*)"Failed to allocate memory for statistics");
		goto finalize;
	}
	
	on_each_cpu(collect_cpu_noise_data, &noise_data, 1 /* wait */);
	
	for_each_online_cpu(cpu) {
		online_cpu_count++;
	}
	
	jdr_write_var(&jdr_inst, enabled, true);
	jdr_write_var(&jdr_inst, tsc_frequency_khz, nvmeib_public_tsc_khz());
	jdr_write_var(&jdr_inst, measurement_interval_ms, nvmeib_completion_noise_measurement_interval_ms);
	
	/* Add threshold percentages array */
	{
		jdr_array_scope(&jdr_inst, "threshold_percentages");
		int thresh_idx;
		for (thresh_idx = 0; thresh_idx < max_thresh_idx; thresh_idx++) {
			jdr_object_scope(&jdr_inst, NULL);
			jdr_write_var(&jdr_inst, index, thresh_idx);
			jdr_write_var(&jdr_inst, percentage, nvmeib_completion_noise_threshold_percentages[thresh_idx]);
		}
	}
	{
		jdr_array_scope(&jdr_inst, "cpu_stats");
		
		current_cpu_index = 0;
		for_each_online_cpu(cpu) {
			int i;
			struct cpu_noise_data_per_cpu *cpu_data = per_cpu_ptr(noise_data.percpu_data, cpu);
			
			jdr_object_scope(&jdr_inst, NULL);
			jdr_write_var(&jdr_inst, cpu, cpu);
		
			/* Add threshold exceedances array */
			write_threshold_exceedances_json(&jdr_inst, "threshold_exceedances", 
							use_local ? cpu_data->with_local_threshold_levels : cpu_data->threshold_levels, 
							max_thresh_idx);
		
			{
				jdr_object_scope(&jdr_inst, "ctrs");
				for (i = 0; i < max_ctr_idx; i++) {
					jdr_inst.ops.u64(&jdr_inst, nvmeib_noise_ctr_names[i], cpu_data->ctrs[i]);
				}
			}
		
			{
				jdr_object_scope(&jdr_inst, "empty_mask_ctrs");
				for (i = 0; i < max_ctr_idx; i++) {
					jdr_inst.ops.u64(&jdr_inst, nvmeib_noise_ctr_names[i], cpu_data->empty_mask_ctrs[i]);
				}
			}
		}
	}
	
	nvmeib_public_free_percpu(noise_data.percpu_data);
	
finalize:
	return jdr_finalize(&jdr_inst).len;
}

ssize_t nvmeib_completion_noise_fill_stats(void *priv, char *buf, size_t len)
{
	return nvmeib_completion_noise_fill_stats_impl(priv, buf, len, false);
}
EXPORT_SYMBOL(nvmeib_completion_noise_fill_stats);

ssize_t nvmeib_completion_noise_fill_stats_local(void *priv, char *buf, size_t len)
{
	return nvmeib_completion_noise_fill_stats_impl(priv, buf, len, true);
}
EXPORT_SYMBOL(nvmeib_completion_noise_fill_stats_local);

static void reset_cpu_noise_data(void *unused)
{
	struct nvmeib_completion_noise_pcpu *noise;
	
	/* this runs on ipi */
	if (!nvmeib_completion_noise_enabled)
		return;
	
	local_irq_disable();
	noise = this_cpu_ptr(completion_noise_stats);
	nvmeib_public_hrtimer_cancel(&noise->measurement_hrtimer);
	nvmeib_completion_noise_reset_cpu_stats(noise);
	__start_timer(noise);
	local_irq_enable();
}

ssize_t nvmeib_completion_noise_reset_stats(void *priv, char *buf, size_t len)
{
	int reset;
	
	if (!nvmeib_completion_noise_enabled) {
		return -ENODEV;
	}
	
	if (sscanf(buf, "%d", &reset) != 1 || reset != 0) {
		return -EINVAL;
	}
	
	on_each_cpu(reset_cpu_noise_data, NULL, 1 /* wait */);
	
	return len;
}
EXPORT_SYMBOL(nvmeib_completion_noise_reset_stats); 