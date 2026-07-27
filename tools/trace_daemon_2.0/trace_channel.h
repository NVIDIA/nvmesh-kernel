#ifndef TRACE_CHANNEL_H
#define TRACE_CHANNEL_H

#include "mmap_manager.h"

#define MAX_TS ((unsigned long)-1)

typedef struct trace_channel_meta
{
	unsigned int chid;
	char* dir;
	char* name;
	char* eph;
	int max_logs;
	int bufs_per_log;
} trace_channel_meta_t;

struct trace_channel;
typedef struct trace_channel trace_channel_t;

const trace_channel_meta_t* get_ch_meta(trace_channel_t* self);
int get_ch_max_bufs(trace_channel_t* self);
int get_control_proc_fd(trace_channel_t* self);
mmap_manager_t* get_mmap_manager(trace_channel_t* self);
trace_channel_t* init_trace_channel(const char* dir, mmap_manager_t* mmap, const char* name, unsigned int id);
void destroy_trace_channel(trace_channel_t* self);
int start_trace_channel(trace_channel_t* self);
void join_trace_channel(trace_channel_t* self);
void reconf_trace_channel(trace_channel_t* self);
void add_to_unlink(trace_channel_t* self, int cpu, int id, unsigned long ts);
void do_unlink(trace_channel_t* self);
void increment_open_files(trace_channel_t* self);
void decrement_open_files(trace_channel_t* self);
char * get_channel_name(trace_channel_t* self);
#endif /* TRACE_CHANNEL_H */