#include "trace_channel.h"
#include "capuch_worker.h"

#define _GNU_SOURCE
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
 * Unlink list - represents the list of data to clean as new data is produced
 */
typedef struct unlink_candidate
{
	int cpu;
	int id;
	time_t ts; /* Timestamp. Only relevant when initializing. */
	struct unlink_candidate* next;
} unlink_candidate_t;
typedef struct unlink_list
{
	pthread_mutex_t lock;
	int len;
	unlink_candidate_t* head;
	unlink_candidate_t* tail;
} unlink_list_t;

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
		/* current open logs */
		int open_files;
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
		.priv.unlink_list.len = 0,
		.priv.unlink_list.head = NULL,
		.priv.unlink_list.tail = NULL,
		.priv.mmap_mgr = mmap_mgr,
		.priv.open_files = 0};
	assert(self->meta.dir && self->meta.name);
	pthread_mutex_init(&self->priv.unlink_list.lock, NULL);

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
		pthread_mutex_destroy(&self->priv.unlink_list.lock);
		free(self->meta.dir);
		free(self->meta.name);
		free(self);
	}
}

/**
 * Utility function
 * Check whether string @str starts with string @pre
 */
int _starts_with(const char* pre, const char* str)
{
	size_t lenpre = strlen(pre), lenstr = strlen(str);
	return lenstr < lenpre ? 0 : strncmp(pre, str, lenpre) == 0;
}

/**
 * Utility function
 * Extract cpu and log id from filename
 */
int _parse_log_filename(const char* fname, const char* tname, int* cpu, int* idx)
{
	if(!_starts_with(tname, fname))
		return 0;
	else
	{
		size_t lentname = strlen(tname);
		*cpu = 0;
		*idx = 0;
		const char* sub = fname + lentname;
		while(*sub >= '0' && *sub <= '9')
		{
			*cpu = *cpu * 10 + *sub - '0';
			++sub;
		}
		if(*sub == '\0')
			return 0;
		if(*cpu >= MAX_CPUS)
			return 0;
		++sub;
		while(*sub >= '0' && *sub <= '9')
		{
			*idx = *idx * 10 + *sub - '0';
			++sub;
		}
		if(*sub != '\0')
			return 0;
		++idx;
		return 1;
	}
}

/**
 * Find the first log id already existing on disk for each cpu in channel, and update workers
 * accordingly
 */
int _get_start_log_id(trace_channel_t* self)
{
	DIR* dp;
	struct dirent* ep;

	dp = opendir(self->meta.dir);
	if(dp != NULL)
	{
		while((ep = readdir(dp)) != NULL)
		{
			int cpu, idx;
			if(_parse_log_filename(ep->d_name, self->meta.name, &cpu, &idx))
			{
				if(self->priv.per_cpu[cpu])
				{
					char path[MAX_FILENAME];
					time_t ts;
					struct stat st;
					snprintf(path, MAX_FILENAME, "%s/%s", self->meta.dir, ep->d_name);

					if(stat(path, &st))
						ts = 0;
					else
						ts = st.st_mtime;

					add_existing_log_id(self->priv.per_cpu[cpu], idx);
					add_to_unlink(self, cpu, idx, ts);
				}
			}
		}
		closedir(dp);
	}
	else
	{
		_error("Listing working dir");
		/* We can continue in this situation, just report error and continue */
	}

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
	if(_get_start_log_id(self))
	{
		_error("Scanning previous logs");
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
	/* max logs should not be less than number of cpus as logs will be missing,
	   if one is intersting in recude size of logs it should set TRACE_BUFS_PER_LOG */
	self->meta.max_logs = max(self->meta.max_logs, self->priv.ncpus);
	_info("Got channel cfg: ncpus=%d", self->priv.ncpus);
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
	while(self->priv.unlink_list.head)
	{
		unlink_candidate_t* tmp = self->priv.unlink_list.head;
		self->priv.unlink_list.head = self->priv.unlink_list.head->next;
		free(tmp);
	}
	self->priv.unlink_list.len = 0;
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
	pthread_mutex_lock(&self->priv.unlink_list.lock); /* Critical section */
	if(!self->priv.unlink_list.tail)
	{
		assert((self->priv.unlink_list.head = self->priv.unlink_list.tail =
					calloc(1, sizeof(unlink_candidate_t))));
		self->priv.unlink_list.tail->cpu = cpu;
		self->priv.unlink_list.tail->id = id;
		self->priv.unlink_list.tail->ts = ts;
	}
	else
	{
		unlink_candidate_t* cand;
		assert(self->priv.unlink_list.head); /* Sanity check */
		assert((cand = calloc(1, sizeof(unlink_candidate_t))));
		cand->cpu = cpu;
		cand->id = id;
		cand->ts = ts;
		if(ts == MAX_TS)
		{ /* If timestamp not specified - just add to tail */
			self->priv.unlink_list.tail->next = cand;
			self->priv.unlink_list.tail = cand;
		}
		else
		{ /* Else - lets find where to add it */
			unlink_candidate_t* pos = self->priv.unlink_list.head;
			unlink_candidate_t* prev = NULL;
			while(pos && ts > pos->ts)
			{
				prev = pos;
				pos = pos->next;
			}
			if(!pos)
			{ /* Add to tail */
				self->priv.unlink_list.tail->next = cand;
				self->priv.unlink_list.tail = cand;
			}
			else
			{
				cand->next = pos;
				if(prev)
				{
					prev->next = cand;
				}
				else
				{
					self->priv.unlink_list.head = cand;
				}
			}
		}
	}
	if (!self->priv.unlink_list.head)
		abort();
	++self->priv.unlink_list.len;
	pthread_mutex_unlock(&self->priv.unlink_list.lock); /* Critical section end */
}

static int __try_remove_log_files(trace_channel_t *self, unlink_candidate_t *cand, const char *subdir, const char *ext)
{
	trace_channel_meta_t *meta = &self->meta;
	char filename[MAX_FILENAME];
	int rv;

	snprintf(filename, sizeof(filename), "%s/%s/%s%d.%d%s", meta->dir, subdir, meta->name, cand->cpu, cand->id, ext);
	rv = syscall_or_nfs_syscall(self, unlink, filename);
	if (rv < 0) {
		if (errno != ENOENT) { // ignore missing files
			_info("Unlink failed for file %s reason: %s", filename, strerror(errno));
		 } else {
			rv = 0;
		 }
		return rv;
	} else {
		_info("Unlink %s", filename);
	}
	return 0;
}

/**
 * Actually unlink next file in the queue
 */
void do_unlink(trace_channel_t* self)
{
	while(1)
	{
		unlink_candidate_t* cand;
		cand = NULL;

		pthread_mutex_lock(&self->priv.unlink_list.lock); /* Critical section */
		_info("Do unlink %s (len=%d,max=%d)", self->meta.name, self->priv.unlink_list.len + self->priv.open_files,
		self->meta.max_logs);
		if(self->meta.max_logs > 0 && ((self->priv.unlink_list.len + self->priv.open_files) > self->meta.max_logs))
			cand = self->priv.unlink_list.head;
		if(cand)
		{
			self->priv.unlink_list.head = self->priv.unlink_list.head->next;
			--self->priv.unlink_list.len;
			if (!self->priv.unlink_list.len) {
				_info("Unlinking list is empty - reset tail"); 
				/* list is empty so reset tail as well */
				self->priv.unlink_list.tail = self->priv.unlink_list.head;
			}
		}
		pthread_mutex_unlock(&self->priv.unlink_list.lock); /* Critical section end */

		if(cand)
		{ /* We do have a valid candidate - actually unlink now */
			__try_remove_log_files(self, cand, "", "");
			__try_remove_log_files(self, cand, "", ".lz4");
			__try_remove_log_files(self, cand, ".cache", "");
			free(cand);
		}
		else
		{
			return; /*Finished*/
		}
	}
}

void increment_open_files(trace_channel_t* self) {
	pthread_mutex_lock(&self->priv.unlink_list.lock);
	self->priv.open_files++;
	pthread_mutex_unlock(&self->priv.unlink_list.lock);
}

void decrement_open_files(trace_channel_t* self) {
	pthread_mutex_lock(&self->priv.unlink_list.lock);
	self->priv.open_files--;
	pthread_mutex_unlock(&self->priv.unlink_list.lock);
}


char * get_channel_name(trace_channel_t* self) {
	return self->meta.name;
}
