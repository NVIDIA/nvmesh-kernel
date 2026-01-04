#include "kr_incs.h"
#include "../common/compat/kr_incs_time.h"
#include "nvmeib_public_mmap.h"
#include "nvmeib_public_procfs.h"
#include "nvmeib_rsc_pool.h"
#include "nvmeib_trace_api.h"
#include "nvmeib_trace_stress_test.h"
#include <linux/timer.h>

/* clang-format off */
#include "nvmeib_trace.h"
#include "nvmeib_public.h"
/* clang-format on */

#define NVMEIB_TRACE_BUF_HEADER_SIZE 8 /* counter + flags + khz */
/* Max single trace size is PAGE_SIZE - header size - timestamp size */
#define NVMEIB_TRACE_MAX_SINGLE_TRACE_SIZE                                     \
	(PAGE_SIZE - NVMEIB_TRACE_BUF_HEADER_SIZE - sizeof(unsigned long long))

#define NVMEIB_TRACE_PROC_DIR "tracer"

#define NVMEIB_TRACE_SYSTEM_DEBUG 1

ulong tracer_dbg_level = 4;
module_param(tracer_dbg_level, ulong, 0644);
MODULE_PARM_DESC(tracer_dbg_level, "Tracer debug level, internal");

#if defined(NVMEIB_TRACE_SYSTEM_DEBUG) && NVMEIB_TRACE_SYSTEM_DEBUG > 0
/* Basic debug */
enum _tracer_dbg_mask_vals { add_trace_is_off };

#define __BIT(x) ((ulong)(1UL << (x)))

ulong tracer_dbg_mask = 0 /* | __BIT(add_trace_is_off) */;
module_param(tracer_dbg_mask, ulong, 0644);
MODULE_PARM_DESC(tracer_dbg_mask, "Tracer debug injections mask, internal");

#define IF_DBG_POINT(pt_, code_)                                               \
	if (tracer_dbg_mask & __BIT(pt_)) code_

#define tracer_dbg_(lvl_, fmt_, ...)                                           \
	if (tracer_dbg_level >= lvl_) pr_info("DEBUG: " fmt_ "\n", ##__VA_ARGS__)			// dp_dbg_tools

#else /*NVMEIB_TRACE_SYSTEM_DEBUG > 1*/
/* No debug */
#define IF_DBG_POINT(...)
#define tracer_dbg_(...)
#define DBG_INLINE static inline
#endif /*defined(NVMEIB_TRACE_SYSTEM_DEBUG) && NVMEIB_TRACE_SYSTEM_DEBUG > 0*/

#if NVMEIB_TRACE_SYSTEM_DEBUG > 1
/* Advanced debug - no inline and pretty stack trace */
#define DBG_STATIC noinline __attribute__((visibility("default")))
#else
/*Simple debug - or no debug - make all static, inline per optimizer decision*/
#define DBG_STATIC static
#endif

#define tracer_dbg(fmt_, ...) tracer_dbg_(10, fmt_, ##__VA_ARGS__)
#define tracer_capuch_(lvl_, capuch_, fmt_, ...)                               \
	tracer_dbg_(lvl_, "capuch(%u,%u,%s,0x%lx) " fmt_, (capuch_)->key.chid,     \
	            (capuch_)->key.cpu, nvmeib_capuch_ch_conf(capuch_)->name,      \
	            (ulong)capuch_, ##__VA_ARGS__)

#define tracer_capuch(capuch_, fmt_, ...)                                      \
	tracer_capuch_(10, capuch_, fmt_, ##__VA_ARGS__)

#define tracer_info(fmt_, ...)                                                 \
	if (tracer_dbg_level >= 7)                                                 \
	pr_info("NVMesh trace system. Error code: 1061.: " fmt_ "\n", ##__VA_ARGS__)

#define tracer_err(fmt_, ...)                                                  \
	if (tracer_dbg_level >= 3)                                                 \
	pr_err("NVMesh trace system ERROR. Error code: 1062.: " fmt_ "\n", ##__VA_ARGS__)

#define NVMEIB_TRACE_BUG_ON(cond)                                              \
	({                                                                         \
		if (!!(cond)) {                                                        \
			tracer_err("BUG. Error code: 1063.: %s:%u - %s", __FILE__, __LINE__, #cond);          \
			BUG();                                                             \
		}                                                                      \
	})

#define NVMEIB_TRACE_BUG() NVMEIB_TRACE_BUG_ON(true);

#define trace_conf_for_each_ch(conf_, ch_)                                     \
	for (ch_ = &(conf_)->trace_chs[0];                                         \
	     ch_ < &(conf_)->trace_chs[nvmeib_trace_channel_max]; ++ch_)

#define mul_x_div_y(a, x, y) (x) * ((a) / (y)) + ((x) * ((a) % (y))) / (y)
#define MAX_CONFIG_STRING 4096

enum nvmeib_trace_channel_enum {
	render_ch_enum(NVMEIB_OPEN_BRACKETS(NVMEIB_TRACE_CH_LIST))
	    nvmeib_trace_channel_max
};

/**
 * The root object, defining the trace system
 */
struct nvmeib_trace_system {
	/* Trace system is a resource pool */
	struct nvmeib_rsc_pool pool;

	const char **names;
	struct nvmeib_capuch *__percpu **pcpu;

	u64 last_reconf_jif;

	/* User specified config. It is not applied directly, rather there is an
	 * apply process. */
	struct nvmeib_trace_system_conf {
		struct nvmeib_rsc_pool_conf pool;
		struct {
			u64 reconf_period;
			size_t write_timeout;
			size_t batch_size;
			size_t ephemeral_life_time;
			size_t flush_tresh_up;
			size_t flush_tresh_down;
			int flushing_first;
		} tracer;
		struct trace_channel_conf {
			enum nvmeib_trace_channel_enum chid;
			const char *name;
			int is_ephemeral;
			int is_non_flushable;
			struct nvmeib_rsc_pool_actor_conf actor;
		} trace_chs[nvmeib_trace_channel_max];
	} user_conf, default_conf;

	struct {
		size_t nchannels;
		size_t ncpus;
		cycles_t tsc_offset;
		uint tsc_khz;
		struct nvmeib_trace_header header;
	} md;

	struct {
		struct proc_dir_entry *parent;
		struct proc_dir_entry *proc_dir;
		struct nvmeib_public_procfs_ent *chlist;
		struct nvmeib_public_procfs_ent *sync;
		struct nvmeib_public_procfs_ent *conf;
		struct nvmeib_public_procfs_ent *stats;
		struct nvmeib_public_procfs_ent *stresstest;
	} proc;

	struct {
		bool active;
		struct completion last_user_completion;
		struct mmap_procfs_ent *proc;
		DECLARE_HASHTABLE(rscmap, 16); /* nvmeib_trace_buf */
	} mmap;

	bool closing; /* system is in shutdown process */

	struct list_head
	    dbg_capuch_list; /* No practical value, except crash analysis. Easy way
	                        to list all capuches in the system. */

} glob_trace_system;

/**
 * CPU/channel pair, tracing system main actor
 */
struct nvmeib_capuch {
	struct nvmeib_rsc_pool_actor actor; /* Capuch is an actor. Must be first. */

	/* Primary key*/
	union nvmeib_capuch_key key;

	size_t ephemeral_dump_progress; /* Ephemeral dump progress - nbufs left */
	bool is_ephemeral; /* Is ephemeral channel? */
	bool is_non_flushable; /* Is non flushable channel (memory only, short)? */

	/* Ready to flush */
	struct list_head ready_list; /* of type nvmeib_trace_buf */
	/* Flush issued on those */
	struct list_head flushing_list; /* of type nvmeib_trace_buf */

	size_t ready_size; /* size of the ready list */
	size_t flushing_size; /* size of the flushing list */
	size_t free_size; /* size of the free list */

	/* Indicates an event - flush, timeout, reconf etc. */
	wait_queue_head_t doorbell;
	
	/* Used to allow all traces to be flushed on shutdown */
	struct nvmeib_ref *flush_ref;

	struct {
		/* Buffer we are currently writing to */
		struct nvmeib_trace_buf {
			struct nvmeib_rsc_pool_rsc rsc; /* Tracebuf IS-A resource */
			struct hlist_node hlink; /* For mmap */
			size_t serial; /* Tracebuf unique ID, for mmap identification */
			u64 ready_jif; /* Time when it became ready (if in ready_list) */
			void *act; /* The actual data page pointer */
		} * buf;

		/* Current offset inside the active buffer */
		size_t off;
		/* Offset at the moment of the last read */
		size_t last_read_off;
		/* Last flush time */
		u64 last_flush_jif;

		/* These are used to write header */
		cycles_t last_tsc;
		uint buf_seq;
		uint flags;
	} active;

	struct {
		/* Is init done? Is teardown started? For debug. If init done - each
		 * list related operation per capuch must be on its own cpu. We can
		 * assert on that. */
		bool is_initialized;
		/* Link to debug list. No practical value, except for crash analysis. */
		struct list_head link;
	} dbg;

	struct {
		u64 bufs_lost;
		unsigned long long bufs_used;
	} stats;
};

/* Debug function validates - capuch critical op is on a correct CPU with
 * correct locks. */
DBG_STATIC void __dbg_capuch_valid(struct nvmeib_capuch *capuch) {
	if (capuch->dbg.is_initialized) {
		NVMEIB_TRACE_BUG_ON(!irqs_disabled());
		NVMEIB_TRACE_BUG_ON(capuch->key.cpu != smp_processor_id());
	}
	(void)capuch;
}

#define nvmeib_capuch_trace_system(capuch_)                                    \
	container_of((capuch_)->actor.pool, struct nvmeib_trace_system, pool)

#define nvmeib_capuch_ch_conf(capuch_)                                         \
	(&nvmeib_capuch_trace_system(capuch_)                                      \
	      ->user_conf.trace_chs[(capuch_)->key.chid])

/* Some reflection helpers */
static const char *nvmeib_trace_channel_names[] = {
    render_ch_names(NVMEIB_OPEN_BRACKETS(NVMEIB_TRACE_CH_LIST))};

render_per_cpu_export(NVMEIB_OPEN_BRACKETS(NVMEIB_TRACE_CH_LIST));

static struct nvmeib_capuch *__percpu *nvmeib_trace_channel_pcpu[] = {
    render_ch_pcpu_vars(NVMEIB_OPEN_BRACKETS(NVMEIB_TRACE_CH_LIST))};

#define __capuch_pcpu_ptr(trace_system_, chid_, cpu_)                          \
	(per_cpu_ptr((trace_system_)->pcpu[chid_], cpu_))

#define __capuch_by_key(trace_system_, chid_, cpu_)                            \
	({                                                                         \
		struct nvmeib_capuch **___capuchp___ =                                 \
		    __capuch_pcpu_ptr(trace_system_, chid_, cpu_);                     \
		struct nvmeib_capuch *___capuch___ =                                   \
		    ___capuchp___ ? *___capuchp___ : NULL;                             \
		___capuch___;                                                          \
	})

/**
 * Iterate over placeholders of all capuches in the system.
 * The iterator, capuch_ptr, is a double pointer - hence allowing allocating or
 * freeing actual capuch info.
 */
#define for_each_capuch_dptr(trace_system_, capuch_ptr_, chid_, cpu_)          \
	for_each_online_cpu(                                                       \
	    cpu_) for (chid_ = 0, capuch_ptr_ = __capuch_pcpu_ptr(trace_system_,   \
	                                                          chid_, cpu_);    \
	               chid_ < nvmeib_trace_channel_max;                           \
	               ++chid_,                                                    \
	               capuch_ptr_ =                                               \
	                   (chid_ < nvmeib_trace_channel_max)                      \
	                       ? __capuch_pcpu_ptr(trace_system_, chid_, cpu_)     \
	                       : NULL)

#define for_each_active_capuch_dptr(trace_system_, capuch_ptr_, chid_, cpu_)   \
	for_each_capuch_dptr(trace_system_, capuch_ptr_, chid_,                    \
	                     cpu_) if (*capuch_ptr_)

#define NVMEIB_TRACE_DEFAULT_CONFIG                                            \
	" global                 pool.size                  65536   \n"            \
	" global                 pool.reserve_percent       25      \n"            \
	" global                 tracer.write_timeout       2       \n"            \
	" global                 tracer.reconf_period       10      \n"            \
	" global                 tracer.batch_size          64      \n"            \
	" global                 tracer.ephemeral_life_time 10      \n"            \
	" global                 tracer.flush_tresh_up      2       \n"            \
	" global                 tracer.flush_tresh_down    3       \n"            \
	" global                 tracer.flushing_first      0       \n"            \
	" *                      actor.priority             128     \n"            \
	" *                      actor.min_greed            2       \n"            \
	" *                      actor.max_greed            14      \n"            \
	" *                      actor.min_rsc              4       \n"            \
	" nvmeibc_trace_eph      is_ephemeral               1       \n"            \
	" nvmeibs_trace_eph      is_ephemeral               1       \n"            \
	" nvmeibc_trace_eph      is_ephemeral               1       \n"            \
	" nvmeibc_trace_short    is_non_flushable           1       \n"            \
	" nvmeibc_trace_eter     actor.priority             256     \n"            \
	" nvmeibs_trace_eter     actor.priority             256     \n"            \
	" nvmeibp_trace_eter     actor.priority             256     \n"            \
	" nvmeibm_trace_eter     actor.priority             256     \n"            \
	" nvmeibc_trace_goodpath actor.priority             32      \n"            \
	" nvmeibc_trace_goodpath actor.max_greed            14      \n"            \
	" nvmeibc_trace_metrics  actor.priority             32      \n"            \
	" nvmeibc_trace_metrics  actor.max_greed            14      \n"            \
	" nvmeibs_trace_goodpath actor.priority             32      \n"            \
	" nvmeibs_trace_goodpath actor.max_greed            14      \n"

static char user_config[MAX_CONFIG_STRING] = "";
module_param_string(config, user_config, MAX_CONFIG_STRING, 0644);
MODULE_PARM_DESC(config, "Binary tracer engine configuration");

/**
 * Utility, pretty print the configuration
 */
#define __format_struct_key(dst_, len_, off_, obj_, key_, fmt_, pre_)          \
	(size_t) scnprintf((char *)dst_ + off_, len_ - off_,                       \
	                   pre_ #key_ ": " fmt_ "\n", (obj_)->key_)
DBG_STATIC size_t __nvmeib_trace_system_conf_to_string(
    char *buf, size_t len, struct nvmeib_trace_system_conf *conf) {
	size_t count = 0;
	struct trace_channel_conf *ch;

	/* clang-format off */
	count += __format_struct_key(buf, len, count, conf, pool.size, "%lu", "");
	count += __format_struct_key(buf, len, count, conf, pool.size_pcpu, "%lu", "");
	count += __format_struct_key(buf, len, count, conf, pool.reserve, "%lu", "");
	count += __format_struct_key(buf, len, count, conf, pool.reserve_percent, "%lu", "");

	count += __format_struct_key(buf, len, count, conf, tracer.write_timeout, "%lu", "");
	count += __format_struct_key(buf, len, count, conf, tracer.reconf_period, "%llu", "");
	count += __format_struct_key(buf, len, count, conf, tracer.batch_size, "%lu", "");
	count += __format_struct_key(buf, len, count, conf, tracer.ephemeral_life_time, "%lu", "");
	count += __format_struct_key(buf, len, count, conf, tracer.flush_tresh_up, "%lu", "");
	count += __format_struct_key(buf, len, count, conf, tracer.flush_tresh_down, "%lu", "");
	count += __format_struct_key(buf, len, count, conf, tracer.flushing_first, "%d", "");
	/* clang-format on */

	trace_conf_for_each_ch(conf, ch) {
		/* clang-format off */
		count += __format_struct_key(buf, len, count, ch, name, "%s", "");
		count += __format_struct_key(buf, len, count, ch, actor.priority, "%lu", "   ");
		count += __format_struct_key(buf, len, count, ch, actor.min_rsc, "%lu", "   ");
		count += __format_struct_key(buf, len, count, ch, actor.min_greed, "%lu", "   ");
		count += __format_struct_key(buf, len, count, ch, actor.max_greed, "%lu", "   ");

		count += __format_struct_key(buf, len, count, ch, is_ephemeral, "%d", "   ");
		count += __format_struct_key(buf, len, count, ch, is_non_flushable, "%d", "   ");
		/* clang-format on */
	}

	return count;
}

#define __capuch_prop_sum(trace_system_, prop_, type_)                         \
	({                                                                         \
		enum nvmeib_trace_channel_enum ___chid___;                             \
		uint ___cpu___;                                                        \
		struct nvmeib_capuch **___capuchp___;                                  \
		type_ ___total___ = 0;                                                 \
		for_each_active_capuch_dptr(trace_system_, ___capuchp___, ___chid___,  \
		                            ___cpu___) ___total___ +=                  \
		    (*___capuchp___)->prop_;                                           \
		___total___;                                                           \
	})
#define __format_capuch_prop_sum(dst_, len_, off_, trace_system_, key_, fmt_,  \
                                 pre_)                                         \
	(size_t)                                                                   \
	    scnprintf((char *)dst_ + off_, len_ - off_, pre_ #key_ ": " fmt_ "\n", \
	              __capuch_prop_sum(trace_system_, key_, size_t))

DBG_STATIC size_t __nvmeib_trace_system_stats_to_string(
    char *buf, size_t len, struct nvmeib_trace_system *trace_system) {
	size_t count = 0;
	enum nvmeib_trace_channel_enum chid;
	uint cpu;
	struct nvmeib_capuch **capuchp;

	/* clang-format off */
	count += __format_struct_key(buf, len, count, trace_system, pool.priv.stats.rebalances_issued, "%llu", "");
	count += __format_struct_key(buf, len, count, trace_system, pool.priv.stats.locks_taken, "%llu", "");
	count += __format_struct_key(buf, len, count, trace_system, pool.priv.stats.peak_pressure, "%llu", "");

	count += __format_struct_key(buf, len, count, trace_system, pool.priv.accepted.size, "%lu", "");
	count += __format_struct_key(buf, len, count, trace_system, pool.priv.accepted.size_pcpu, "%lu", "");
	count += __format_struct_key(buf, len, count, trace_system, pool.priv.accepted.reserve, "%lu", "");
	count += __format_struct_key(buf, len, count, trace_system, pool.priv.accepted.reserve_percent, "%lu", "");

	count += __format_struct_key(buf, len, count, trace_system, pool.priv.n_actors, "%lu", "");
	count += __format_struct_key(buf, len, count, trace_system, pool.priv.n_available_rsc, "%lu", "");
	count += __format_struct_key(buf, len, count, trace_system, pool.priv.total_pressure, "%lu", "");

	count += __format_capuch_prop_sum(buf, len, count, trace_system, stats.bufs_lost, "%lu", "total ");
	count += __format_capuch_prop_sum(buf, len, count, trace_system, free_size, "%lu", "total ");
	count += __format_capuch_prop_sum(buf, len, count, trace_system, ready_size, "%lu", "total ");
	count += __format_capuch_prop_sum(buf, len, count, trace_system, flushing_size, "%lu", "total ");
	/* clang-format on */

	for_each_active_capuch_dptr(trace_system, capuchp, chid, cpu) {
		count +=
		    scnprintf(buf + count, len - count, "> capuch %u,%u,%s,0x%lx:\n",
		              (*capuchp)->key.chid, (*capuchp)->key.cpu,
		              nvmeib_capuch_ch_conf(*capuchp)->name, (ulong)*capuchp);
		/* clang-format off */
		count += __format_struct_key(buf, len, count, *capuchp, actor.quota, "%lu", "\t");
		count += __format_struct_key(buf, len, count, *capuchp, actor.pressure, "%lu", "\t");
		count += __format_struct_key(buf, len, count, *capuchp, actor.greed, "%lu", "\t");
		count += __format_struct_key(buf, len, count, *capuchp, actor.nbufs, "%lu", "\t");

		count += __format_struct_key(buf, len, count, *capuchp, free_size, "%lu", "\t");
		count += __format_struct_key(buf, len, count, *capuchp, ready_size, "%lu", "\t");
		count += __format_struct_key(buf, len, count, *capuchp, flushing_size, "%lu", "\t");

		count += __format_struct_key(buf, len, count, *capuchp, stats.bufs_lost, "%llu", "\t");
		count += __format_struct_key(buf, len, count, *capuchp, stats.bufs_used, "%llu", "\t");
		/* clang-format on */
	}

	return count;
}

/* Parse configuration. Not thread safe. Quick and dirty.
 * Does the job. */
#define __try_set_config_key(dst_, prop_, key_, value_)                        \
	if (!strncmp(prop_, #key_, sizeof(#key_))) (dst_)->key_ = value_
DBG_STATIC void __parse_config(const char *config_param,
                               struct nvmeib_trace_system_conf *user_conf,
                               const char **names) {
	static char sketch[MAX_CONFIG_STRING] = {0}; /* Work area */
	char *ptr, *line;
	bool done = false;

	tracer_dbg_(50, "Parsing config <%s>\n", config_param);

	/* Split config by lines - for simplicity */
	strncpy(sketch, config_param, MAX_CONFIG_STRING);
	sketch[MAX_CONFIG_STRING - 1] = '\0';

	/* Scan line by line */
	ptr = sketch;
	while (!done) {
		static char scope[MAX_CONFIG_STRING];
		static char prop[MAX_CONFIG_STRING];
		static ulong value;
		int rv;

		line = ptr;
		while (*ptr != '\0' && *ptr != '\n' && *ptr != ';')
			ptr++;

		if (!*ptr)
			done = true; /* Guaranteed to reminate as we explicitly put 0
			                earlier */
		else
			*(ptr++) = '\0';
		if ((rv = sscanf(line, "%s %s %lu", scope, prop, &value)) != 3) {
			if (*line) tracer_dbg_(50, "Skipping bad config line <%s>\n", line);
			continue;
		}

		if (!strncmp(scope, "global", sizeof("global"))) {
			/* clang-format off */
			__try_set_config_key(user_conf, prop, pool.size, value);
			__try_set_config_key(user_conf, prop, pool.size_pcpu, value);
			__try_set_config_key(user_conf, prop, pool.reserve, value);
			__try_set_config_key(user_conf, prop, pool.reserve_percent, value);

			__try_set_config_key(user_conf, prop, tracer.write_timeout, value);
			__try_set_config_key(user_conf, prop, tracer.reconf_period, value);
			__try_set_config_key(user_conf, prop, tracer.batch_size, value);
			__try_set_config_key(user_conf, prop, tracer.ephemeral_life_time, value);
			__try_set_config_key(user_conf, prop, tracer.flushing_first, value);
			__try_set_config_key(user_conf, prop, tracer.flush_tresh_up, value);
			__try_set_config_key(user_conf, prop, tracer.flush_tresh_down, value);
			/* clang-format on */
		} else {
			/* Channel configuration */
			enum nvmeib_trace_channel_enum chid;
			for (chid = 0; chid < nvmeib_trace_channel_max; ++chid) {
				user_conf->trace_chs[chid].chid = chid;
				user_conf->trace_chs[chid].name = names[chid];
				if (!strncmp(scope, "*", sizeof("*")) ||
				    !strcmp(scope, user_conf->trace_chs[chid].name)) {
					/* clang-format off */
					__try_set_config_key(&user_conf->trace_chs[chid], prop, actor.priority, value);
					__try_set_config_key(&user_conf->trace_chs[chid], prop, actor.min_rsc, value);
					__try_set_config_key(&user_conf->trace_chs[chid], prop, actor.min_greed, value);
					__try_set_config_key(&user_conf->trace_chs[chid], prop, actor.max_greed, value);

					__try_set_config_key(&user_conf->trace_chs[chid], prop, is_ephemeral, value);
					__try_set_config_key(&user_conf->trace_chs[chid], prop, is_non_flushable, value);
					/* clang-format on */
				}
			}
		}
	}
}

/**
 * Basic sanity test on config
 */
DBG_STATIC bool __is_valid_config(struct nvmeib_trace_system_conf *conf) {
	struct trace_channel_conf *ch;

	if (conf->pool.reserve >= conf->pool.size) return false;
	if (conf->pool.reserve_percent >= 100) return false;
	if (conf->pool.reserve > conf->pool.size) return false;

	if (!conf->tracer.flush_tresh_up || !conf->tracer.flush_tresh_down)
		return false;

	trace_conf_for_each_ch(conf, ch) {
		if (ch->actor.max_greed < ch->actor.min_greed) return false;
	}

	return true;
}

/**
 * Apply valid trace system conf
 * @warning must be called under a lock
 */
DBG_STATIC void
__apply_trace_system_conf(struct nvmeib_trace_system *trace_system,
                          struct nvmeib_trace_system_conf *conf) {
	struct nvmeib_capuch **capuchp;
	uint cpu;
	enum nvmeib_trace_channel_enum chid;
	trace_system->user_conf = *conf;
	trace_system->pool.conf = trace_system->user_conf.pool;
	for_each_active_capuch_dptr(trace_system, capuchp, chid, cpu) {
		struct nvmeib_capuch *capuch = *capuchp;
		capuch->actor.conf =
		    trace_system->user_conf.trace_chs[capuch->key.chid].actor;
		capuch->is_ephemeral =
		    trace_system->user_conf.trace_chs[capuch->key.chid].is_ephemeral;
		capuch->is_non_flushable =
		    trace_system->user_conf.trace_chs[capuch->key.chid]
		        .is_non_flushable;
	}
}

/**
 * Read the user config and apply it
 * @warning Must be called either from init or under lock
 */
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wframe-larger-than"
#endif
DBG_STATIC bool __reconf_trace_system(struct nvmeib_trace_system *trace_system,
                                      const char *config_param) {
	struct nvmeib_trace_system_conf user_conf = trace_system->default_conf;
	__parse_config(config_param, &user_conf, trace_system->names);
	if (!__is_valid_config(&user_conf)) return false;
	__apply_trace_system_conf(trace_system, &user_conf);
	return true;
}
#ifdef __clang__
#pragma clang diagnostic pop
#endif

/***************************/
/* Buffer writing routines */
/***************************/

/* Determine whether active buffer is freshly opened */
#define __is_fresh(capuch_) ((capuch_)->active.off <= 8)

/* Write a  buffer termination watermark */
DBG_STATIC void __mark_terminate_buffer(struct nvmeib_capuch *capuch) {
	if (capuch->active.off <= PAGE_SIZE - 6) {
		void *p = capuch->active.buf->act + capuch->active.off;
		*(u32 *)p = 0;
		*(u16 *)(p + 4) = 0;
	}
}

/* Write buffer header first half (known on init) */
DBG_STATIC void __write_buffer_first_header(struct nvmeib_capuch *capuch) {
	capuch->active.off = NVMEIB_TRACE_BUF_HEADER_SIZE;
	capuch->active.last_read_off = NVMEIB_TRACE_BUF_HEADER_SIZE;
	*(uint *)capuch->active.buf->act =
	    (capuch->active.buf_seq++ & 0xffffff) | capuch->active.flags << 24;
	*(uint *)(capuch->active.buf->act + 4) =
	    nvmeib_capuch_trace_system(capuch)->md.tsc_khz;
}

/********************/
/*     Utils        */
/********************/

DBG_STATIC ulong __helper_irq_save(void) {
	ulong flags;
	local_irq_save(flags);
	return flags;
}

DBG_STATIC void __helper_irq_restore(ulong *flags) {
	local_irq_restore(*flags);
}

#define __irq_disable_guard()                                                  \
	__attribute__((cleanup(__helper_irq_restore))) ulong ___guard_flags___ =   \
	    __helper_irq_save()

#define _grab_one_from_list(l_, type_, member_)                                \
	({                                                                         \
		type_ *___tmp___;                                                      \
		NVMEIB_TRACE_BUG_ON(list_empty(l_));                                   \
		___tmp___ = list_first_entry(l_, type_, member_);                      \
		list_del_init(&(___tmp___)->member_);                                  \
		___tmp___;                                                             \
	})

/**
 * Helper function, will grab a buffer without too much thinking.
 * Never locks. May reuse ready buffers.
 * May fail, if called before init or after destroy.
 */

DBG_STATIC struct nvmeib_trace_buf *
__get_one_from_free(struct nvmeib_capuch *capuch) {
	__dbg_capuch_valid(capuch);
	if (capuch->free_size) {
		capuch->free_size--;
		return _grab_one_from_list(&capuch->actor.rsc_list,
		                           struct nvmeib_trace_buf, rsc.link);
	}
	return NULL;
}
static void __put_one_to_free(struct nvmeib_capuch *capuch,
                       struct nvmeib_trace_buf *buf) {
	__dbg_capuch_valid(capuch);
	capuch->free_size++;
	NVMEIB_TRACE_BUG_ON(!list_empty(&buf->rsc.link));
	list_add_tail(&buf->rsc.link, &capuch->actor.rsc_list);
}

DBG_STATIC struct nvmeib_trace_buf *
__get_one_from_ready(struct nvmeib_capuch *capuch) {
	__dbg_capuch_valid(capuch);
	if (capuch->ready_size) {
		capuch->ready_size--;
		return _grab_one_from_list(&capuch->ready_list, struct nvmeib_trace_buf,
		                           rsc.link);
	}
	return NULL;
}
static void __put_one_to_ready(struct nvmeib_capuch *capuch,
                        struct nvmeib_trace_buf *buf) {
	__dbg_capuch_valid(capuch);
	capuch->ready_size++;
	NVMEIB_TRACE_BUG_ON(!list_empty(&buf->rsc.link));
	list_add_tail(&buf->rsc.link, &capuch->ready_list);
}

DBG_STATIC struct nvmeib_trace_buf *
__get_one_from_flushing(struct nvmeib_capuch *capuch) {
	__dbg_capuch_valid(capuch);
	if (capuch->flushing_size) {
		capuch->flushing_size--;
		return _grab_one_from_list(&capuch->flushing_list,
		                           struct nvmeib_trace_buf, rsc.link);
	}
	return NULL;
}
static void __put_one_to_flushing(struct nvmeib_capuch *capuch,
                           struct nvmeib_trace_buf *buf) {
	__dbg_capuch_valid(capuch);
	capuch->flushing_size++;
	NVMEIB_TRACE_BUG_ON(!list_empty(&buf->rsc.link));
	list_add_tail(&buf->rsc.link, &capuch->flushing_list);
}

DBG_STATIC struct nvmeib_trace_buf *
__get_the_bloody_buffer(struct nvmeib_capuch *capuch) {
	/* Yuri - @TODO: some benchmarking is required here.
	 * There are 2 options. First - take buffers from flushing list, then
	 * from ready list. Second - the inverse, first ready, then flushing.
	 * In first case, there is a chance that if we grab a buffer, it is
	 * already flushed, so we lose nothing. On the other hand, there is a
	 * chance it is not flushed, in this case we corrupt the data on disk,
	 * and add one more write operations, increasing the flush time.
	 * In the second case, we definitely lose data, but data on disk will
	 * definitely be integeral, and overall disk pressure is lower, raising
	 * the chance that peak will be over soon.
	 * Both have advantages and disadvantages, I dunno what to chose really.
	 * Hence made it a configurable policy.
	 */
	struct nvmeib_trace_buf *buf = NULL;
	if (nvmeib_capuch_trace_system(capuch)->user_conf.tracer.flushing_first) {
		if (!(buf = __get_one_from_flushing(capuch)))
			buf = __get_one_from_ready(capuch);
	} else {
		if (!(buf = __get_one_from_ready(capuch)))
			buf = __get_one_from_flushing(capuch);
	}

	/* This will only happen during the destruction.
	 * Return the active buffer too.
	 */
	if (!buf) {
		buf = capuch->active.buf;
		capuch->active.buf = NULL;
	}

	return buf;
}

/**
 * Will get more buffers or return, depending on the quota
 */
static void __rebalance_capuch(struct nvmeib_capuch *capuch) {
#ifndef LLVM
	__irq_disable_guard();
#else
	ulong ___guard_flags___ = __helper_irq_save();
#endif
	ssize_t d = nvmeib_rsc_pool_quota(&capuch->actor) - capuch->actor.nbufs;
	if (d > 0) /* Can have more */ {
		__dbg_capuch_valid(capuch);
		capuch->free_size += nvmeib_rsc_pool_fetch_rsc(&capuch->actor, d);
	} else if (d < 0) /* Must return */ {
		d = -d; /* Represents how many buffers we have to return */
		while (capuch->free_size < d) {
			/* We don't have enough free buffers to return. Never hog on
			 * resources. Start moving your ass, free resources, NOW!!! */
			struct nvmeib_trace_buf *buf = __get_the_bloody_buffer(capuch);
			if (buf)
				__put_one_to_free(capuch, buf);
			else
				NVMEIB_TRACE_BUG(); /* It is impossible that we have to return
				          more bufs than we have. BUG. */
		}
		__dbg_capuch_valid(capuch);
		nvmeib_rsc_pool_return_rsc(&capuch->actor, d);
		capuch->free_size -= d;
	}
#ifdef LLVM
	__helper_irq_restore(&___guard_flags___);
#endif
	/* else we are good */
}

/********************/
/*     Events       */
/********************/

DBG_STATIC bool __has_anything_to_flush(struct nvmeib_capuch *capuch);

struct capuch_closing_param {
	struct nvmeib_capuch *capuch;
	struct nvmeib_ref *flush_ref;
};

DBG_STATIC bool __closing_flush_complete(struct nvmeib_capuch *capuch)
{
	if (capuch->is_non_flushable)
		return true;
	if (capuch->is_ephemeral)
		return capuch->ephemeral_dump_progress == 0;
	if (__has_anything_to_flush(capuch))
		return false;
	return true;
}

/**
 * Capuch shall be closed. Called by closing process, always on the right CPU.
 */
DBG_STATIC void on_capuch_closing(void *param_) {
	struct capuch_closing_param *param = param_;
	struct nvmeib_capuch *capuch = param->capuch;

	nvmeib_rsc_pool_actor_start_closing(&capuch->actor);

	if (!__closing_flush_complete(capuch)) {
		BUG_ON(!nvmeib_ref_get(param->flush_ref));
		capuch->flush_ref = param->flush_ref;
		tracer_capuch(capuch, "on_capuch_closing INC flush_ref (%p) count: %d\n",
			          capuch->flush_ref, nvmeib_ref_read(capuch->flush_ref));
		wake_up(&capuch->doorbell);
	}
}

/**
 * Capuch is stopping. Called by closing process, always on the right CPU.
 */
DBG_STATIC void on_capuch_stopping(void *capuch_) {
	struct nvmeib_capuch *capuch = capuch_;

	/* mmap must be terminated before we get here */
	NVMEIB_TRACE_BUG_ON(nvmeib_capuch_trace_system(capuch)->mmap.active);

	/* This is the only reason why this event is called via srq */
	__rebalance_capuch(capuch);
}

/**
 * New buffer is ready for flush. Event is triggerd by the tracing system.
 */
DBG_STATIC void on_buffer_flush_ready(struct nvmeib_capuch *capuch) {
#ifndef LLVM
	__irq_disable_guard(); /* Cant have IRQs here, else lists can be destroyed
	                        */
#else
	ulong ___guard_flags___ = __helper_irq_save();
#endif

	if (capuch->active.buf) {
		/* This is not the first time, we do have an active buffer */
		/* Buffer watermark - terminated */
		__mark_terminate_buffer(capuch);
		/* Move to ready */
		__put_one_to_ready(capuch, capuch->active.buf);
		capuch->active.buf = NULL;
		capuch->stats.bufs_used++;
	}

	if (capuch->actor.nbufs && !capuch->free_size) {
		/* We do not have enough resources. Need to ask for more. */
		/* First check - we are not during init. Second check - we don not have
		 * resources. */
		nvmeib_rsc_pool_inc_greed(&capuch->actor);
	}

	/* Rebalance resources */
	__rebalance_capuch(capuch);

	/* Get a new buffer. If possible - from free list. If not - whaever. */
	if (!(capuch->active.buf = __get_one_from_free(capuch))) {
		capuch->active.buf = __get_the_bloody_buffer(capuch);
		capuch->stats.bufs_lost++;
	}

	/* After allthese manipulations, do we have to flush? */
	if (!capuch->is_ephemeral && !capuch->is_non_flushable &&
	    capuch->ready_size) {
		/* We are here if after all manipulations we have a new element in the
		 * ready list. Wake up the flusher. */

		wake_up(&capuch->doorbell);
	}

	/* Prepare the next buffer for work */
	if (capuch->active.buf) {
		__write_buffer_first_header(capuch);
	} else {
		/* No buffers at all? I don't expect it to happen. Each capuch has
		 * at least min_bufs available. It must be an initialization
		 * problem. */
		NVMEIB_TRACE_BUG();
	}
#ifdef LLVM
	__helper_irq_restore(&___guard_flags___);
#endif
}

/**
 * How long do we have until we have to force a flush?
 * Can't be negativem 0 means timeout has already passed unspecified time ago.
 */
DBG_STATIC u64 __time_left_to_force_flush(struct nvmeib_capuch *capuch) {
	if (capuch->is_ephemeral || capuch->is_non_flushable)
		return nvmeib_capuch_trace_system(capuch)
		           ->user_conf.tracer.write_timeout *
		       HZ;
	else {
		u64 next_flush =
		    capuch->active.last_flush_jif +
		    nvmeib_capuch_trace_system(capuch)->user_conf.tracer.write_timeout *
		        HZ;
		u64 jif = jiffies;
		if (jif < next_flush) return next_flush - jif;
	}
	return 0;
}

/**
 * @return true if there is anything to flush
 */
DBG_STATIC bool __has_anything_to_flush(struct nvmeib_capuch *capuch) {
	return capuch->ready_size > 0 ||
		(capuch->active.buf && capuch->active.off != capuch->active.last_read_off);
}

/**
 * @return true if it decides to flush.
 */
DBG_STATIC bool __is_flush_required(struct nvmeib_capuch *capuch) {
	if (nvmeib_capuch_trace_system(capuch)->closing) return true;
	if (capuch->flush_ref) return true;
	if (!__has_anything_to_flush(capuch)) return false;
	if (capuch->ephemeral_dump_progress) return true;
	if (capuch->is_ephemeral || capuch->is_non_flushable) return false;
	if (capuch->ready_size >=
	    nvmeib_capuch_trace_system(capuch)->user_conf.tracer.batch_size)
		return true;
	return false;
}

/**
 * Batch flush finished. Event is trigerred by trace_deamon
 */
DBG_STATIC void on_flush_end(struct nvmeib_capuch *capuch) {
#ifndef LLVM
	__irq_disable_guard(); /* Critical section, no ctx switch allowed */
#else
	ulong ___guard_flags___ = __helper_irq_save();
#endif

	/* Flushing list is now free */
	if (capuch->flushing_size) {
		NVMEIB_TRACE_BUG_ON(list_empty(&capuch->flushing_list));
		__dbg_capuch_valid(capuch);
		list_splice_tail_init(&capuch->flushing_list, &capuch->actor.rsc_list);
		capuch->free_size += capuch->flushing_size;
		capuch->flushing_size = 0;
	}

	if (capuch->flush_ref && __closing_flush_complete(capuch)) {		
		nvmeib_ref_put(capuch->flush_ref);
		tracer_capuch(capuch, "on_flush_end DEC flush_ref (%p) count: %d\n",
			capuch->flush_ref, nvmeib_ref_read(capuch->flush_ref));
		capuch->flush_ref = NULL;
	}

#ifdef LLVM
	__helper_irq_restore(&___guard_flags___);
#endif
}

/**
 * Timed out waiting for flush. Triggered by timeout in control proc context.
 */
DBG_STATIC void on_flush_timeout(struct nvmeib_capuch *capuch) {
#ifndef LLVM
	__irq_disable_guard(); /* Critical section, no ctx switch allowed */
#else
	ulong ___guard_flags___ = __helper_irq_save();
#endif
	if (capuch->free_size * nvmeib_capuch_trace_system(capuch)
	                            ->user_conf.tracer.flush_tresh_up >
	    (capuch->flushing_size + capuch->ready_size) *
	        nvmeib_capuch_trace_system(capuch)
	            ->user_conf.tracer.flush_tresh_down) {
		/* We did not even use a big enough part of our quota.
		 */
		nvmeib_rsc_pool_dec_greed(&capuch->actor);
	}
#ifdef LLVM
	__helper_irq_restore(&___guard_flags___);
#endif
}

/* @TODO: I am pretty sure it can be don in a better way, but for now frome size
 * is > 4K and I am OK with it, maybe optimize later. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wframe-larger-than="
/**
 * Control proc entry point, triggered by trace_daemon read system call
 */
DBG_STATIC ssize_t control_proc_read(void *arg, char __user *buf, size_t len,
                                     loff_t *offset, struct file *file) {
	struct nvmeib_trace_dump_range range;
	struct nvmeib_trace_system *trace_system = arg;
	union nvmeib_capuch_key key = {.raw = *offset};
	struct nvmeib_capuch *capuch;
	size_t size_written;
	(void)file;

	if (len == sizeof(struct nvmeib_trace_header)) {
		/* Special case - metadata read */
		struct nvmeib_trace_header hdr = {trace_system->md.ncpus,
		                                  trace_system->md.tsc_khz};
		if (copy_to_user(buf, &hdr, sizeof(hdr))) return -EFAULT;
		return sizeof(hdr);
	}

	if (len != sizeof(range)) return -EINVAL;

	capuch = __capuch_by_key(trace_system, key.chid, key.cpu);
	if (!capuch) return -EINVAL;

	/* Invalid configuration */
	NVMEIB_TRACE_BUG_ON(trace_system->user_conf.tracer.batch_size >
	                    ARRAY_SIZE(range.cells));

	/* First event - finish the previous batch */
	on_flush_end(capuch);

	while (1) {
		int rv = 0;
		bool flush = false;
		u64 timeout = __time_left_to_force_flush(capuch);
		if (timeout > 0)
			rv = wait_event_interruptible_timeout(
			    capuch->doorbell, __is_flush_required(capuch), timeout);
		/* clang-format on */

		if (rv < 0) return rv; /* Error - returning it back to user */
		if (!rv) {
			/* timed out */
			on_flush_timeout(capuch);
			flush = __has_anything_to_flush(capuch);
		}

		if (nvmeib_rsc_pool_actor_need_return(&capuch->actor) && !capuch->flush_ref)
			__rebalance_capuch(capuch);

		if (__is_flush_required(capuch)) flush = true;

		if (flush)
			break;
		else
			capuch->active.last_flush_jif = jiffies;

		/* Go back to wait */
	}

	/* Now - time to flush */
	{
#ifndef LLVM
		__irq_disable_guard(); /* Critical section, no ctx switch allowed */
#else
		ulong ___guard_flags___ = __helper_irq_save();
#endif

		if (!capuch->ready_size) {
			/* No fully ready buffers - partial dump or lcosing */
			if (trace_system->closing) /* Closing means closing cut the crap */
				return 0;
			if (capuch->flush_ref && !__has_anything_to_flush(capuch)) {
				nvmeib_ref_put(capuch->flush_ref);
				tracer_capuch(capuch, "nothing to flush - DEC flush_ref (%p) count: %d\n",
					      capuch->flush_ref, nvmeib_ref_read(capuch->flush_ref));
				capuch->flush_ref = NULL;
				return 0;
			}
			/* We must have active buf here. If not - there are 2 options either
			 * closing (here we are not closing) or keep sleeping and not
			 * getting here. */
			NVMEIB_TRACE_BUG_ON(!__has_anything_to_flush(capuch));
			NVMEIB_TRACE_BUG_ON(
			    __is_fresh(capuch)); /* should not have gotten here */

			/* If it was an ephemeral dump - it ends here */
			capuch->ephemeral_dump_progress = 0;

			/* Partial dump */
			range.count = 0;
			range.cells[0] = capuch->active.buf->serial;
			__mark_terminate_buffer(capuch);
			capuch->active.last_read_off = capuch->active.off;
			size_written = sizeof(range.count) + sizeof(range.cells[0]);
		} else {
			/* We do have stuff to dump */
			range.count = 0;
			while (range.count < trace_system->user_conf.tracer.batch_size) {
				struct nvmeib_trace_buf *buf = __get_one_from_ready(capuch);
				if (!buf) break;
				range.cells[range.count++] = buf->serial;
				__put_one_to_flushing(capuch, buf);
				if (capuch->ephemeral_dump_progress)
					capuch->ephemeral_dump_progress--;
			}
			size_written =
			    sizeof(range.count) + range.count * sizeof(range.cells[0]);
		}
#ifdef LLVM
		__helper_irq_restore(&___guard_flags___);
#endif
	}

	capuch->active.last_flush_jif = jiffies;

	if (copy_to_user(buf, &range, size_written)) return -EFAULT;
	return size_written;
}
#pragma GCC diagnostic pop

/************************************/
/* Info procs, not mission critical */
/************************************/

/**
 * Fills in information about active configuration
 */
DBG_STATIC ssize_t fill_stats(void *trace_system_, char *buf, size_t len) {
	struct nvmeib_trace_system *trace_system = trace_system_;
	return __nvmeib_trace_system_stats_to_string(buf, len, trace_system);
}

/**
 * Fills in information about active configuration
 */
DBG_STATIC ssize_t fill_active_conf(void *trace_system_, char *buf,
                                    size_t len) {
	struct nvmeib_trace_system *trace_system = trace_system_;
	return __nvmeib_trace_system_conf_to_string(buf, len,
	                                            &trace_system->user_conf);
}

/**
 * Fills in information about trace channels available in the system
 */
DBG_STATIC ssize_t fill_list_trace_channels(void *trace_system_, char *buf,
                                            size_t len) {
	ssize_t count = 0;
	struct nvmeib_trace_system *trace_system = trace_system_;
	struct trace_channel_conf *ch;

	trace_conf_for_each_ch(&trace_system->user_conf, ch) {
		if (!ch->is_non_flushable)
			count += scnprintf(buf + count, len - count, "%s\n", ch->name);
		else
			count += scnprintf(buf + count, len - count, "#%s\n", ch->name);
	}
	return count;
}

/**
 * Exposes synchronization info - tsc, khz
 */
DBG_STATIC ssize_t fill_sync_info(void *trace_system_, char *buffer,
                                  size_t len) {
	struct nvmeib_trace_system *trace_system = trace_system_;
	return scnprintf(buffer, len, "%u %llu\n", trace_system->md.tsc_khz,
	                 (u64)trace_system->md.tsc_offset);
}

struct __stress_test_task {
	int cpu;
	struct task_struct *k;
	struct list_head progs;
	atomic_t *ntasks;
	struct completion *task_all_done;
};

DBG_STATIC int ___stress_test_worker(void *task_) {
	struct __stress_test_task *task = task_;
	struct nvmeib_trace_stress_prog *prog;
	list_for_each_entry(prog, &task->progs, link) prog->action(prog);
	if (!atomic_dec_return(task->ntasks)) complete(task->task_all_done);
	return 0;
}

DBG_STATIC ssize_t fill_stress_test(void *trace_system_, char *buffer,
                                    size_t len) {
	return scnprintf(buffer, len,
	                 "Short sample stress test program: \n"
	                 "CPU 1 to 10\n"
	                 "    RESET_COUNT\n"
	                 "CPU 1 to 5\n"
	                 "    REPEAT : 1000000\n"
	                 "        TRACE : This_trace_is_fast\n"
	                 "    END\n"
	                 "CPU 6 to 10\n"
	                 "    REPEAT : 100\n"
	                 "        TRACE : This_trace_is_slow\n"
	                 "        SLEEP : 10000\n"
	                 "    END\n");
}

/**
 * Accepts a command to start a stress test on a proc
 */
DBG_STATIC ssize_t set_stress_test(void *trace_system_, char *buf, size_t len) {
	struct nvmeib_trace_system *trace_system = trace_system_;
	struct __stress_test_task *pcpu_tasks;
	size_t cpu, start_cpu, end_cpu, off = 0;
	atomic_t ntasks = ATOMIC_INIT(trace_system->md.ncpus);
	DECLARE_COMPLETION_ONSTACK(comp);
	bool ok = true;
	struct nvmeib_trace_stress_prog *prog, *tmp;

	tracer_info("Starting stress test len=%lu <%s>", len, buf);

	if (!(pcpu_tasks = kzalloc(sizeof(struct __stress_test_task) *
	                               trace_system->md.ncpus,
	                           GFP_KERNEL)))
		return -ENOMEM;

	for (cpu = 0; cpu < trace_system->md.ncpus; ++cpu) {
		pcpu_tasks[cpu].cpu = cpu;
		INIT_LIST_HEAD(&pcpu_tasks[cpu].progs);
		pcpu_tasks[cpu].ntasks = &ntasks;
		pcpu_tasks[cpu].task_all_done = &comp;
	}

	while (len != off) {
		prog = nvmeib_trace_stress_cpu_and_prog_from_string(
		    buf, len, &off, &start_cpu, &end_cpu);
		if (!prog) {
			tracer_err("Parsing stress test prog at off=%lu <... %30s ...>",
			           off, buf + off);
			ok = false;
			break;
		}

		if (start_cpu > end_cpu || end_cpu >= trace_system->md.ncpus) {
			tracer_err("Invalid cpus %lu %lu", start_cpu, end_cpu);
			ok = false;
			prog->dtor(prog);
			break;
		}

		for (cpu = start_cpu; cpu <= end_cpu; ++cpu) {
			list_add_tail(&prog->copy(prog)->link, &pcpu_tasks[cpu].progs);
		}
		prog->dtor(prog);
	}

	if (ok) {
		tracer_info("All good, starting workers");
		for (cpu = 0; cpu < trace_system->md.ncpus; ++cpu) {
			struct nvmeib_trace_stress_prog *prog;
			if (!list_empty(&pcpu_tasks[cpu].progs))
				tracer_info("CPU %lu program:", cpu);
			list_for_each_entry(prog, &pcpu_tasks[cpu].progs, link)
			    prog->print(prog);
		}
		for (cpu = 0; cpu < trace_system->md.ncpus; ++cpu) {
			pcpu_tasks[cpu].k = kthread_create(___stress_test_worker,
			                                   &pcpu_tasks[cpu], "stworker");
			if (IS_ERR(pcpu_tasks[cpu].k)) {
				ok = false;
				if (cpu) wait_for_completion(&comp);
				break;
			}
			kthread_bind(pcpu_tasks[cpu].k, cpu);
			wake_up_process(pcpu_tasks[cpu].k);
		}
	}

	if (ok) wait_for_completion(&comp);

	for (cpu = 0; cpu < trace_system->md.ncpus; ++cpu) {
		list_for_each_entry_safe(prog, tmp, &pcpu_tasks[cpu].progs, link) {
			list_del_init(&prog->link);
			prog->dtor(prog);
		}
	}

	kfree(pcpu_tasks);

	tracer_info("Ending stress test");

	if (!ok)
		return -EINVAL;
	else
		return len;
}

/**
 * Mmap page fault handler - map offset to the actual page address
 */
DBG_STATIC int trace_mmap_page_fault(void *trace_system_, unsigned long serial,
                                     struct page **page) {
	struct nvmeib_trace_system *trace_system = trace_system_;
	struct nvmeib_trace_buf *buf;
#ifndef LLVM
	__irq_disable_guard();
#else
	ulong ___guard_flags___ = __helper_irq_save();
#endif
	tracer_dbg_(60, "mmap page fault attempt 0x%lx", serial);

	hash_for_each_possible(trace_system->mmap.rscmap, buf, hlink, serial) {
		if (buf->serial == serial) {
			tracer_dbg_(50, "mmap page fault success 0x%lx=>0x%lx", serial,
			            (unsigned long)buf->act);
			*page = virt_to_page(buf->act);
			trace_system->mmap.active = true;
			return 0;
		}
	}

	tracer_err("mmap failed to resolve 0x%lx", serial);
#ifdef LLVM
	__helper_irq_restore(&___guard_flags___);
#endif

	return -1;
}

/**
 * Mmap - last user terminated
 */
DBG_STATIC void trace_mmap_on_last(void *trace_system_) {
	struct nvmeib_trace_system *trace_system = trace_system_;
	trace_system->mmap.active = false;
	complete(&trace_system->mmap.last_user_completion);
}

/************************/
/*      Callbacks       */
/************************/
/**
 * Buffer constructor
 */
DBG_STATIC struct nvmeib_rsc_pool_rsc *
nvmeib_trace_buf_ctor(struct nvmeib_rsc_pool *pool) {
	(void)pool;
	if (!in_interrupt()) {
		/* Avoid reconf being done in interrupt context */
		static size_t serial =
		    0; /* This function is called under system-wide lock or from one
		          thread dutring init, it is completely OK to use statis
		          members here. */
		struct nvmeib_trace_buf *buf = kzalloc(sizeof(*buf), GFP_KERNEL);
		if (!buf) return NULL; /* Nomem */
		buf->act = (void *)__get_free_page(GFP_KERNEL);
		if (!buf->act) {
			kfree(buf);
			return NULL; /* Nomem */
		}
		buf->serial = serial++;

		/* Register it to mmap */
		{
			struct nvmeib_trace_system *trace_system =
			    container_of(pool, struct nvmeib_trace_system, pool);
			hash_add(trace_system->mmap.rscmap, &buf->hlink, buf->serial);
		}

		return &buf->rsc;
	}

	return NULL;
}

/**
 * Buffer destructor
 */
DBG_STATIC void nvmeib_trace_buf_dtor(struct nvmeib_rsc_pool *pool,
                                      struct nvmeib_rsc_pool_rsc *rsc) {
	struct nvmeib_trace_buf *buf =
	    container_of(rsc, struct nvmeib_trace_buf, rsc);
	NVMEIB_TRACE_BUG_ON(!buf);
	NVMEIB_TRACE_BUG_ON(!buf->act);
	NVMEIB_TRACE_BUG_ON(!list_empty(&rsc->link));

	/* Unregister it from mmap */
	hash_del(&buf->hlink);

	free_page((unsigned long)buf->act);
	kfree(buf);

	(void)pool;
}

DBG_STATIC void __on_before_rebalance(struct nvmeib_rsc_pool *pool, void *_) {
	struct nvmeib_trace_system *trace_system =
	    container_of(pool, struct nvmeib_trace_system, pool);

	if (trace_system->last_reconf_jif +
	        trace_system->user_conf.tracer.reconf_period * HZ >
	    jiffies) {
		__reconf_trace_system(trace_system, user_config);
		trace_system->last_reconf_jif = jiffies;
	}

	(void)_;
}

/***********************/
/*  Setup & teardown   */
/***********************/

DBG_STATIC void
__init_tracer_md_header(struct nvmeib_trace_system *trace_system) {
	unsigned int cpu, last_cpu = 0;
	struct timespec ts;
	cycles_t cyc;

	for_each_online_cpu(cpu) if (cpu > last_cpu) last_cpu = cpu;
	trace_system->md.ncpus = last_cpu + 1;
	trace_system->md.nchannels =
	    nvmeib_trace_channel_max; /* This is for crash nalyzer only, no other
	                                 use */

	trace_system->md.tsc_khz = nvmeib_public_tsc_khz();

	getnstimeofday_real(&ts);
	cyc = nvmeib_public_rdtsc();
	trace_system->md.tsc_offset =
	    mul_x_div_y(1000000000L * ts.tv_sec + ts.tv_nsec,
	                trace_system->md.tsc_khz, 1000000L) -
	    cyc;
}

static struct nvmeib_capuch *
nvmeib_create_capuch(struct nvmeib_trace_system *trace_system,
                     enum nvmeib_trace_channel_enum chid, uint cpu) {

	struct nvmeib_capuch *capuch = NULL;
	const struct trace_channel_conf *conf =
	    &trace_system->user_conf.trace_chs[chid];

	tracer_dbg("init capuch %s %u", conf->name, cpu);

	NVMEIB_TRACE_BUG_ON(chid != conf->chid); /* sanity */

	{
	#ifdef CONFIG_NUMA
		int node = per_cpu(numa_node, cpu);
		if (!(capuch =
		          kzalloc_node(sizeof(struct nvmeib_capuch), GFP_KERNEL, node)))
	#else
		if (!(capuch =
				kzalloc(sizeof(struct nvmeib_capuch), GFP_KERNEL)))
	#endif
			goto out;

		*capuch = (struct nvmeib_capuch){
		    .key.chid = conf->chid,
		    .key.cpu = cpu,
		    .is_ephemeral = conf->is_ephemeral,
		    .is_non_flushable = conf->is_non_flushable,
		    .actor.conf = conf->actor,
		};

		init_waitqueue_head(&capuch->doorbell);
		INIT_LIST_HEAD(&capuch->ready_list);
		INIT_LIST_HEAD(&capuch->flushing_list);

		/* For debug only, maintaining a full list of capuches. Never use for
		 * other purposes as it would introduce concurrency issues. */
		list_add_tail(&capuch->dbg.link, &trace_system->dbg_capuch_list);

		nvmeib_rsc_pool_register_actor(&trace_system->pool, &capuch->actor);
	}

	tracer_capuch(capuch, "init done");

out:
	return capuch;
}

/**
 * The entry point function
 */
int nvmeib_init_traces(struct proc_dir_entry *proc_dir) {
	int rv;
	struct nvmeib_trace_system *trace_system = &glob_trace_system;
	tracer_info("init start");

	/* Zero all to be on the safe side */
	memset(trace_system, 0, sizeof(*trace_system));

	/* Initialize all we can for now */
	__init_tracer_md_header(trace_system);
	trace_system->names = nvmeib_trace_channel_names;
	trace_system->pcpu = nvmeib_trace_channel_pcpu;
	INIT_LIST_HEAD(&trace_system->dbg_capuch_list);
	hash_init(trace_system->mmap.rscmap);

	/* Parse the default config */
	__parse_config(NVMEIB_TRACE_DEFAULT_CONFIG, &trace_system->default_conf,
	               trace_system->names);
	NVMEIB_TRACE_BUG_ON(!__is_valid_config(&trace_system->default_conf));

	/* Apply the user config if any */
	__reconf_trace_system(trace_system, user_config);

	/* Initialize the resource pool */
	trace_system->pool = (struct nvmeib_rsc_pool){
	    .construction.construct_rsc = nvmeib_trace_buf_ctor,
	    .construction.destruct_rsc = nvmeib_trace_buf_dtor,
	    .conf = trace_system->default_conf.pool};
	nvmeib_rsc_pool_init(&trace_system->pool);

	/* Start creating actors */
	tracer_info("init actors");
	{
		enum nvmeib_trace_channel_enum chid;
		uint cpu;
		struct nvmeib_capuch **capuchp;
		for_each_capuch_dptr(trace_system, capuchp, chid, cpu) {
			if (!(*capuchp = nvmeib_create_capuch(trace_system, chid, cpu))) {
				rv = -ENOMEM;
				goto out;
			}
		}

		/* After all capuches are initialized, have to grab a minimal nmber of
		 * buffers for each, to avoid a situation that someone has 0 buffers and
		 * reserve is 0. */
		for_each_capuch_dptr(trace_system, capuchp, chid, cpu) {
			__rebalance_capuch(*capuchp);
			(*capuchp)->dbg.is_initialized = true;
		}
	}

	/* Register callbacks (should be done after creating actors) */
	nvmeib_rsc_pool_register_callback(&trace_system->pool, on_before_rebalance,
	                                  __on_before_rebalance, NULL, GFP_KERNEL);

	/* Prepare the procfs */
	rv = -EEXIST;
	tracer_info("init trace procfs");
	trace_system->proc.parent = proc_dir;
	if (!(trace_system->proc.proc_dir =
	          proc_mkdir(NVMEIB_TRACE_PROC_DIR, trace_system->proc.parent)))
		goto out;
	trace_system->proc.chlist =
	    nvmeib_public_proc_create("chlist", trace_system->proc.proc_dir,
	                       fill_list_trace_channels, NULL, trace_system);
	if (trace_system->proc.chlist == NULL) goto out;

	trace_system->proc.conf =
	    nvmeib_public_proc_create("conf", trace_system->proc.proc_dir,
	                       fill_active_conf, NULL, trace_system);
	if (trace_system->proc.conf == NULL) goto out;

	trace_system->proc.stats = nvmeib_public_proc_create(
	    "stats", trace_system->proc.proc_dir, fill_stats, NULL, trace_system);
	if (trace_system->proc.stats == NULL) goto out;

	trace_system->proc.sync =
	    nvmeib_public_proc_create("sinfo", trace_system->proc.proc_dir, fill_sync_info,
	                       NULL, trace_system);
	if (trace_system->proc.sync == NULL) goto out;

	trace_system->proc.stresstest =
	    nvmeib_public_proc_create(".stresstest", trace_system->proc.proc_dir,
	                       fill_stress_test, set_stress_test, trace_system);
	if (trace_system->proc.stresstest == NULL) goto out;

	/* Prepare mmap */
	init_completion(&trace_system->mmap.last_user_completion);
	trace_system->mmap.proc = nvmeib_public_mmap_create(
	    "mmap", trace_system->proc.proc_dir, trace_mmap_page_fault,
	    trace_system, trace_mmap_on_last, control_proc_read, NULL, NULL, 0400);
	if (trace_system->mmap.proc == NULL) goto out;

	rv = 0;

	tracer_info("init done");

out:
	if (rv) { /* Error */
		tracer_err("during init traces - %d\n", rv);
		nvmeib_stop_traces();
	}
	return rv;
}
EXPORT_SYMBOL(nvmeib_init_traces);

void nvmeib_stop_traces(void) {
	struct nvmeib_trace_system *trace_system = &glob_trace_system;
	struct nvmeib_ref flush_ref;
	tracer_info("stop system");

	nvmeib_ref_init(&flush_ref);

	/* Tell all capuchs they are closing and flush all buffers to disk */
	{
		struct nvmeib_capuch **capuchp;
		uint cpu;
		enum nvmeib_trace_channel_enum chid;
		tracer_info("flush actors");
		for_each_active_capuch_dptr(trace_system, capuchp, chid, cpu) {
			struct capuch_closing_param param = {
				.capuch = *capuchp,
				.flush_ref = &flush_ref,
			};
			tracer_capuch(*capuchp, "close");
			if (smp_call_function_single(cpu, on_capuch_closing, &param,
				true)) 
			{
				tracer_err("critical, during closing procedure - unable to execute capuch=0x%lx closing procedure on cpu %u",
					(ulong)*capuchp, cpu);
				NVMEIB_TRACE_BUG();
			}
		}

		/* Wait for all capuchs to flush ready buffers to disk */
		tracer_info("told all capuchs to close and flush traces");
		nvmeib_ref_release_start(&flush_ref);
		nvmeib_ref_release_wait_n(&flush_ref, 1);
		tracer_info("waited for all capuchs to flush traces");

		/* set closing flag for any threads sleeping at control_proc_read and then wake them up */
		trace_system->closing = true;

		for_each_active_capuch_dptr(trace_system, capuchp, chid, cpu) {
			wake_up(&(*capuchp)->doorbell);
		}
	}

	/* Remove procs */
	tracer_info("stop procs\n");
	if (trace_system->proc.sync != NULL) {
		nvmeib_public_proc_remove(trace_system->proc.sync);
		trace_system->proc.sync = NULL;
	}
	if (trace_system->proc.chlist != NULL) {
		nvmeib_public_proc_remove(trace_system->proc.chlist);
		trace_system->proc.chlist = NULL;
	}
	if (trace_system->proc.conf != NULL) {
		nvmeib_public_proc_remove(trace_system->proc.conf);
		trace_system->proc.conf = NULL;
	}
	if (trace_system->proc.stats != NULL) {
		nvmeib_public_proc_remove(trace_system->proc.stats);
		trace_system->proc.stats = NULL;
	}
	if (trace_system->proc.stresstest != NULL) {
		nvmeib_public_proc_remove(trace_system->proc.stresstest);
		trace_system->proc.stresstest = NULL;
	}

	/* Stop mmap, thus ending all flushes */
	tracer_info("stop mmap");
	if (trace_system->mmap.proc) {
		nvmeib_public_mmap_release(trace_system->mmap.proc);
		if (trace_system->mmap.active)
			wait_for_completion(&trace_system->mmap.last_user_completion);
		nvmeib_public_mmap_remove(trace_system->mmap.proc);
	}

	if (trace_system->proc.proc_dir) {
		remove_proc_entry(NVMEIB_TRACE_PROC_DIR, trace_system->proc.parent);
		trace_system->proc.proc_dir = NULL;
	}
	tracer_info("mmap removed");

	/* At this point everything is flushed to disk, and no more traces ca
	 * enter. Destroy the resource pool. This will cause all capuches to
	 * return their resources.
	 */

	/* Before we are allowed to destroy the pool, all actors shall be
	 * stopped */
	{
		struct nvmeib_capuch **capuchp;
		uint cpu;
		enum nvmeib_trace_channel_enum chid;
		tracer_info("stop actors");
		for_each_active_capuch_dptr(trace_system, capuchp, chid, cpu) {
			tracer_capuch(*capuchp, "stop");
			if (smp_call_function_single(cpu, on_capuch_stopping, *capuchp,
			                             true)) {
				tracer_err("critical, during closing procedure - unable to execute capuch=0x%lx closing procedure on cpu %u",
				           (ulong)*capuchp, cpu);
				NVMEIB_TRACE_BUG();
			}

			/* Here, the all resources must be returned */
			NVMEIB_TRACE_BUG_ON((*capuchp)->actor.nbufs);
			NVMEIB_TRACE_BUG_ON((*capuchp)->free_size);
			NVMEIB_TRACE_BUG_ON((*capuchp)->ready_size);
			NVMEIB_TRACE_BUG_ON((*capuchp)->flushing_size);

			nvmeib_rsc_pool_unregister_actor(&(*capuchp)->actor);

			tracer_capuch((*capuchp), "stop done");
			kfree((*capuchp));

			*capuchp = NULL;
		}
	}

	/* Now the pool */
	tracer_info("stop resource pool\n");
	nvmeib_rsc_pool_free(&trace_system->pool);

	tracer_info("stopped");
}
EXPORT_SYMBOL(nvmeib_stop_traces);

void *nvmeib_add_trace(int len, struct nvmeib_capuch *capuch, u32 cksum) {
	void *p; /* write pointer */
	cycles_t tsc; /* timestamp */
	bool long_tsc = false; /* is timestamp (un)compressed */
	bool write_cksum = false; /* write checksum - fresh buffer */
	const struct nvmeib_trace_system *trace_system;

	IF_DBG_POINT(add_trace_is_off, ({ return NULL; }));

	if (len > NVMEIB_TRACE_MAX_SINGLE_TRACE_SIZE) {
		tracer_dbg("Attempt to trace illegal length=%d", len);
		return NULL;
	}

	if (!capuch) {
		tracer_dbg_(50, "nvmeib_add_trace - capuch = NULL");
		return NULL; /* Rarelly possible in case of call
		                during destructor */
	}

	trace_system = nvmeib_capuch_trace_system(capuch);

	{
#ifndef LLVM
		__irq_disable_guard(); /* Critical section under irqdisable */
#else
		ulong ___guard_flags___ = __helper_irq_save();
#endif

		if (capuch->actor.closing) {
			/* No more new traces allowed */
			return NULL;
		}

		/* What time is now? */
		tsc = nvmeib_public_rdtsc() + trace_system->md.tsc_offset;
		if (tsc - capuch->active.last_tsc > (1UL << 31)) {
			/* We need to use long tsc - too much bits */
			long_tsc = true;
			len += 4;
		}
		capuch->active.last_tsc = tsc;

		if (capuch->active.off + len > PAGE_SIZE || !capuch->active.buf) {
			/* not enough - need a new buffer */
			on_buffer_flush_ready(capuch);
		}

		if (!capuch->active.buf)
			return NULL; /* Could not get a new buffer, rare but possible
			                during destruction */

		if (__is_fresh(capuch)) {
			/* It is a fresh new buffer.
			 * First trace to a new buffer will always write a secondary
			 * header, including checksum and long timestamp.
			 */
			if (!long_tsc) {
				long_tsc = true;
				len += 4;
			}
			write_cksum = true; // Always make sure to write checksum
			len += sizeof(cksum);
		}

		p = capuch->active.buf->act + capuch->active.off;
		capuch->active.off += len;
#ifdef LLVM
		__helper_irq_restore(&___guard_flags___);
#endif
	}

	/* No longer in a critical section, here memory is already reserved, no
	 * internal structures modified */
	if (unlikely(write_cksum)) { /* Write checksum if needed */
		*(u32 *)p = cksum;
		p += sizeof(cksum);
	}
	if (unlikely(long_tsc)) { /* Long tsc */
		*(cycles_t *)p = (tsc << 1) | 1;
		return p + 8;
	} else { /* Short tsc */
		*(u32 *)p = (u32)tsc << 1;
		return p + 4;
	}
}
EXPORT_SYMBOL(nvmeib_add_trace);

/* Must be called with the main channel as parameter */
static void nvmeib_dump_ephemeral(enum nvmeib_trace_channel_enum chid) {
	int cpu;

	for_each_online_cpu(cpu) {
		struct nvmeib_capuch *capuch =
		    *__capuch_pcpu_ptr(&glob_trace_system, chid, cpu);
		if (capuch) {
			capuch->ephemeral_dump_progress = capuch->ready_size;
			wake_up(&capuch->doorbell);
		}
	}
}

void nvmeibc_dump_ephemeral(void) {
	nvmeib_dump_ephemeral(nvmeibc_trace_eph_chid);
}
EXPORT_SYMBOL(nvmeibc_dump_ephemeral);

void nvmeibs_dump_ephemeral(void) {
	nvmeib_dump_ephemeral(nvmeibs_trace_eph_chid);
}
EXPORT_SYMBOL(nvmeibs_dump_ephemeral);

/**
 * Kernel space implementation of symbol resolution by
 * address. See also the user space implementation in
 * nvmeib_trace_userspace.c
 */
#include "nvmeib_utils.h"
/* Some API from common_public module is required. Prolem: public loads
   after us. Solution: public passes an api structure to common.
   The structure is protected by nvmeib_ref. Its initial state is dying.
   Public initializes it and sets its state to active. After that, common
   start using it. At the unload, public ref back to dying and waits for
   all users to exit. */
struct nvmeib_ref tracer_public_api_ref = {.dying = ATOMIC_INIT(1)};
struct nvmeib_tracer_public_api tracer_public_api;

void nvmeib_reg_tracer_public_api(struct nvmeib_tracer_public_api *api) {
	BUG_ON(!nvmeib_ref_is_dying(&tracer_public_api_ref)); /* not 100% */
	/* First register, then init ref. In that order.*/
	tracer_public_api = *api;
	nvmeib_ref_init(&tracer_public_api_ref);
}
EXPORT_SYMBOL(nvmeib_reg_tracer_public_api);

void nvmeib_unreg_tracer_public_api(void) {
	/* Can sleep */
	if (!nvmeib_ref_release_start(&tracer_public_api_ref))
		nvmeib_ref_release_wait(&tracer_public_api_ref);
}
EXPORT_SYMBOL(nvmeib_unreg_tracer_public_api);

static inline bool __sym_resolve_kernel_bug_can_happen(void *addr)
{
	bool bug_can_happen = true;
	if (nvmeib_ref_get(&tracer_public_api_ref)) {
		bug_can_happen = tracer_public_api.sym_resolve_kernel_bug_can_happen(addr); /* already considers kernel version */
		nvmeib_ref_put(&tracer_public_api_ref);
	}
	return bug_can_happen;
}
/**
 * Get the length of the symbol string pointed by address
 */
int nvmeib_symbol_length(void *addr) {
	return __sym_resolve_kernel_bug_can_happen(addr) ?
		strlen("<kallsyms-bug>") : snprintf(NULL, 0, "%ps", addr);
}
EXPORT_SYMBOL(nvmeib_symbol_length);
/**
 * Print symbol string into dest
 */
int nvmeib_symbol_strcpy(char *dest, void *addr, int len) {
	return __sym_resolve_kernel_bug_can_happen(addr) ?
		scnprintf(dest, len, "%s", "<kallsyms-bug>") : scnprintf(dest, len, "%ps", addr);
}
EXPORT_SYMBOL(nvmeib_symbol_strcpy);

int nvmeib_stack_trace_length(void *addr) {
	struct nvmeib_stack_trace *st = addr;
	int i;
	int sum = 0;
	BUG_ON(!addr); /* Case is handled by code generator */
	for (i = 0; i < st->nr_entries; ++i)
		if (st->entries[i])
			sum += snprintf(NULL, 0, "[%016lx] %pS", st->entries[i], (void*)st->entries[i]) + 2;

	return sum + 1;
}
EXPORT_SYMBOL(nvmeib_stack_trace_length);

int nvmeib_stack_trace_strcpy(char *dest, void *addr, int len) {
	struct nvmeib_stack_trace *st = addr;
	int i;
	int sum = 0;
	BUG_ON(!addr); /* Case is handled by code generator */
	dest[sum++] = '\n';
	for (i = 0; i < st->nr_entries; ++i) {
		if (st->entries[i]) {
			dest[sum++] = '>';
			sum += scnprintf(dest + sum, len - sum, "[%016lx] %pS", st->entries[i], (void*)st->entries[i]);
			dest[sum++] = '\n';
		}
	}
	dest[sum] = '\0';

	return sum;
}
EXPORT_SYMBOL(nvmeib_stack_trace_strcpy);

unsigned long nvmeib_trace_tsc_to_ns(unsigned long timestamp) {
	/* Avoid overflow, alternative is to use __int128 */
	if (!glob_trace_system.md.tsc_khz) return -1;
	timestamp += glob_trace_system.md.tsc_offset;
	return 1000000L *
	           (timestamp / glob_trace_system.md.tsc_khz) + /* Lose precision */
	       (1000000L * (timestamp % glob_trace_system.md.tsc_khz)) /
	           glob_trace_system.md.tsc_khz; /* Compensate lost precision
	                                          */
}
EXPORT_SYMBOL(nvmeib_trace_tsc_to_ns);

nvmeib_public_save_stack_trace_t nvmeib_public_save_stack_trace_ptr;
EXPORT_SYMBOL(nvmeib_public_save_stack_trace_ptr);

bool nvmeib_hide_warnings_stack = NVMESH_IS_PRODUCTION_COMPILATION;							// In production, dont clutter dmesg by default
module_param_named(hide_warnings_stack, nvmeib_hide_warnings_stack, bool, 0644);
MODULE_PARM_DESC(hide_warnings_stack, "Hide warnings from dmesg, while keeping them still available in the binary traces.");

bool nvmeib_get_hide_warnings_stack(void);
bool nvmeib_get_hide_warnings_stack(void) {
	return nvmeib_hide_warnings_stack;
}
EXPORT_SYMBOL(nvmeib_get_hide_warnings_stack);
