#include "trace_channel.h"
#include "unlink_list.h"
#include "capuch_worker.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define __MODULE_HDR "%s"
#define __MODULE_HDR_ARGS self->meta.name
#include "../../common/nvmeib_trace_api.h"
#include "trace_daemon_common.h"

#define MAX_CPUS 1024 /* 1024 should be eough, we will not use all of it anyway */

/**
 * Trace channel object
 */
struct trace_channel
{
	/** 1 if channel started, 0 therwise */
	int started;

	/* Config data channel is initialized with */
	trace_channel_meta_t meta;

	/* Private data read after after start only */
	struct
	{
		/** Total number of online cpus, can be non sequential */
		int ncpus;
		/** Per cpu workers descriptors */
		capuch_worker_t* per_cpu[MAX_CPUS];
		/** List of data to unlink as new data files are produced */
		unlink_list_t unlink_list;
		/** Pointer to an mmap manager */
		mmap_manager_t* mmap_mgr;
	} priv;
};

#define for_each_cpuid(trace_channel, cpuid)  \
	for(cpuid = 0; cpuid < MAX_CPUS; ++cpuid) \
		if(trace_channel->priv.per_cpu[cpuid])

/**
 * Initialize per cpu channel object for each online CPU.
 */
static int _init_online_per_cpu_objects(trace_channel_t* self)
{
	/**
	 * TODO: Better error handling
	 */
	FILE* f;
	int cpu, cpu0, cpu1;
	char c;

	f = fopen("/sys/devices/system/cpu/online", "r");
	assert(f);
	while(fscanf(f, "%d", &cpu0) == 1)
	{
		assert(cpu0 < MAX_CPUS);
		c = getc(f);
		if(c == '-')
		{
			assert(fscanf(f, "%d", &cpu1) == 1);
			assert(cpu1 < MAX_CPUS);
			for(cpu = cpu0; cpu <= cpu1; cpu++)
				assert((self->priv.per_cpu[cpu] = init_capuch_worker(self, cpu)));
			c = getc(f);
		}
		else if(c == ',')
		{
			assert((self->priv.per_cpu[cpu0] = init_capuch_worker(self, cpu0)));
		}
		if(c == '\n')
			break;
	}
	fclose(f);

	return 0;
}

/**
 * Initialize trace channel object as an opaque pointer.
 */
trace_channel_t* init_trace_channel(
	const char* dir, mmap_manager_t* mmap_mgr, const char* name, unsigned int id)
{
	trace_channel_t* self = calloc(sizeof(trace_channel_t), 1);
	assert(self);
	*self = (trace_channel_t){.started = 0,
		.meta.dir = strdup(dir),
		.meta.name = strdup(name),
		.meta.chid = id,
		.meta.max_logs = get_max_logs(name),
		.meta.bufs_per_log = get_buf_per_log(name),
		.priv.per_cpu = {0},
		.priv.mmap_mgr = mmap_mgr};
	assert(self->meta.dir && self->meta.name);
	unlink_list_init(&self->priv.unlink_list, self->meta.dir, self->meta.name);

	return self;
}

/**
 * Destroy trace channel object including all of its per cpu channels.
 */
void destroy_trace_channel(trace_channel_t* self)
{
	if(self)
	{
		join_trace_channel(self);
		unlink_list_destroy(&self->priv.unlink_list);
		free(self->meta.dir);
		free(self->meta.name);
		free(self);
	}
}



/**
 * Iterate unlink list and update workers with existing log IDs
 */
static int _update_workers_from_unlink_list(trace_channel_t* self)
{
	unlink_candidate_t *cand;
	pthread_mutex_t *lock = &self->priv.unlink_list.lock;

	pthread_mutex_lock(lock);
	cand = self->priv.unlink_list.head;
	while(cand)
	{
		if(self->priv.per_cpu[cand->cpu])
		{
			add_existing_log_id(self->priv.per_cpu[cand->cpu], cand->id);
		}
		cand = cand->next;
	}
	pthread_mutex_unlock(lock);

	return 0;
}

/**
 * Open trace channel
 * Create memory mappings and start worker threads
 */
int start_trace_channel(trace_channel_t* self)
{
	int cpu;
	char filename[MAX_FILENAME];
	struct nvmeib_trace_header trace_hdr;

	/* Create percpu worker object for each active cpu, without starting them yet at this stage */
	if(_init_online_per_cpu_objects(self))
	{
		_error("Initializing per cpu objects");
		goto err;
	}

	/* Find out what files exist on the disk, update workers accordingly */
	if(unlink_list_populate(&self->priv.unlink_list, MAX_CPUS))
	{
		_error("Scanning previous logs");
		goto err;
	}
	if(_update_workers_from_unlink_list(self))
	{
		_error("Updating workers from unlink list");
		goto err;
	}

	/* Read header from the control proc */
	if(pread(get_control_proc(self->priv.mmap_mgr), &trace_hdr, sizeof(trace_hdr), 0) !=
		sizeof(trace_hdr))
	{
		_error("Read header %s", filename);
		goto err;
	}

	if(trace_hdr.ncpus <= 0 || trace_hdr.ncpus > MAX_CPUS)
	{
		_error("Bad ncpus %d", trace_hdr.ncpus);
		goto err;
	}

	/* Initialize structures with data from the header */
	self->priv.ncpus = trace_hdr.ncpus;
	
	if ((self->meta.max_logs / 2) < self->priv.ncpus){
		//a single file per CPU does not allow to have history. Let's stay wih the same disk usage, but smaller files.
		//see INIT_TRACE_CFG documentation
		uint32_t const MIN_LOG_FILES_PER_CPU = 4; //on log rotate, preserve 75% of information
		uint32_t const MIN_BUFS_PER_LOG = 64;
		uint32_t const MAX_BUFS_PER_LOG = 4096;

		uint32_t const total_bufs = self->meta.max_logs * self->meta.bufs_per_log;
		uint32_t const total_bufs_per_cpu = total_bufs / self->priv.ncpus;
		uint32_t const bufs_per_log = total_bufs_per_cpu / MIN_LOG_FILES_PER_CPU;
		
		if (bufs_per_log < MIN_BUFS_PER_LOG){
			self->meta.max_logs = MIN_LOG_FILES_PER_CPU * self->priv.ncpus;
			self->meta.bufs_per_log = MIN_BUFS_PER_LOG;
		} else if (bufs_per_log <= MAX_BUFS_PER_LOG){
			self->meta.max_logs = MIN_LOG_FILES_PER_CPU * self->priv.ncpus;
			self->meta.bufs_per_log = bufs_per_log;
		} else { //unlikely - in this case we have small amount of huge files
			self->meta.max_logs = (total_bufs_per_cpu / MAX_BUFS_PER_LOG) * self->priv.ncpus;
			self->meta.bufs_per_log = MAX_BUFS_PER_LOG;
		}
	}

	_info("Got channel cfg: name=%s, ncpus=%d, max_logs=%d bufs_per_log=%d", self->meta.name, self->priv.ncpus, self->meta.max_logs, self->meta.bufs_per_log);
	/* now back to steady state */ 
	do_unlink(self);

	/* Start the workers for each cpu */
	for_each_cpuid(self, cpu)
	{
		if(start_capuch_worker(self->priv.per_cpu[cpu]))
		{
			_error("Start worker CPU %d", cpu);
			goto err;
		}
	}

	self->started = 1;

	return 0;
err:
	for_each_cpuid(self, cpu) abort_capuch_worker(self->priv.per_cpu[cpu]);
	join_trace_channel(self);
	return -1;
}

/**
 * Wait for all trace channel workers to finish work
 */
void join_trace_channel(trace_channel_t* self)
{
	int cpu;
	self->started = 0;
	_info("joinging trace channel %s", self->meta.name);
	/* Stop all workers */
	for_each_cpuid(self, cpu)
	{
		destroy_capuch_worker(self->priv.per_cpu[cpu]);
	}
	/* Clean unlink list */
	unlink_list_clear(&self->priv.unlink_list);
}

/**
 * Get channel metadata
 */
const trace_channel_meta_t* get_ch_meta(trace_channel_t* self)
{
	return &self->meta;
}

/**
 * Get the file descriptor for channel control proc
 */
int get_control_proc_fd(trace_channel_t* self)
{
	return get_control_proc(self->priv.mmap_mgr);
}

/**
 * Get the mmap manager object pointer
 */
mmap_manager_t* get_mmap_manager(trace_channel_t* self)
{
	return self->priv.mmap_mgr;
}

/**
 * Read updated conf and set channel properties accordingly
 */
void reconf_trace_channel(trace_channel_t* self)
{
	_info("Reconf");
	self->meta.bufs_per_log = get_buf_per_log(self->meta.name);
	self->meta.max_logs = get_max_logs(self->meta.name);
	do_unlink(self);
}

/**
 * Add another candidate to the unlink queue
 */
void add_to_unlink(trace_channel_t* self, int cpu, int id, unsigned long ts)
{
	_info("Add to unlink cpu=%d file=%d", cpu, id);
	unlink_list_add(&self->priv.unlink_list, cpu, id, ts);
}

/**
 * Actually unlink next file in the queue
 */
void do_unlink(trace_channel_t* self)
{
	unlink_list_do_unlink(&self->priv.unlink_list, self->meta.max_logs);
}

void increment_open_files(trace_channel_t* self) {
	unlink_list_increment_open_files(&self->priv.unlink_list);
}

void decrement_open_files(trace_channel_t* self) {
	unlink_list_decrement_open_files(&self->priv.unlink_list);
}


char * get_channel_name(trace_channel_t* self) {
	return self->meta.name;
}
