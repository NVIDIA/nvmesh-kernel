#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/statfs.h>
#include <linux/magic.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "mmap_manager.h"
#include "trace_channel.h"
#include "io_pet_channel.h"

#define __MODULE_HDR "trace_daemon"
#include "trace_daemon_common.h"

#define LIST_CHANNELS_PROC "/proc/nvmeib/tracer/chlist"
#define MMAP_PROC "/proc/nvmeib/tracer/mmap"

#define MAX_FILENAME 1024
#define MAX_FILENAME_STR "1024"
#define MAX_TRACE_CHANNELS 100
#define TRACE_CHANNELS_READ_BUF_SIZE 4096
#define MAX_TRACE_NAME 100
#define PING_INTERVAL 3000000
#define MAX_CGROUP_NAME_STR "120"

#define GLOBAL_CONFIG_FILE "/etc/nvmesh/nvmesh.conf"
#define LOCAL_CONFIG_FILE ".tracedaemon.conf"

typedef struct trace_channel_ctx
{
	trace_channel_t* ch;
} trace_channel_ctx_t;

/* Configuraion that can be controlled at runtime via SIGHUP */
trace_daemon_cfg_t trace_cfg = INIT_TRACE_CFG;

/* Global tracer context.
   Must be global as it is sharred between signal and regular context */
trace_channel_ctx_t ctx[MAX_TRACE_CHANNELS] = {{0}};

/* IO PET channel context (separate from regular trace channels) */
io_pet_channel_t* io_pet_ch = NULL;

static const char *nvmesh_is_prod_env_var_name = "NVMESH_IS_PRODUCTION";

void print_help(const char* name)
{
	printf("Usage:\n"
		   "\t%s [-h|bufs-per-log max-logs]\n",
		name);
}

/** Read configuration file.
 * It is located inside working directory and contains a series of KEY=value clauses
 */
void reread_config(const char* config_file)
{
	FILE* fp;
	char* line = NULL;
	size_t len = 0;

	fp = fopen(config_file, "r");
	if(fp == NULL)
		return; /*No config file found*/
	while(getline(&line, &len, fp) != -1)
	{
		int _data;
		char _channel[MAX_FILENAME];
		char _logs_path[MAX_FILENAME];
		char _loggers_cgroup[MAX_FILENAME];

		/*For simplicity to be able to use scanf, replace = with space*/
		if(strchr(line, '='))
			*strchr(line, '=') = ' ';
		if(sscanf(line, "TRACE_DAEMON_DBG \"%d\"", &_data) == 1)
		{
			_info("TRACE_DAEMON_DBG=\"%d\"", _data);
			trace_cfg.debug_lvl = _data;
			continue;
		}
		if(sscanf(line, "TRACE_BUFS_PER_LOG \"%d\"", &_data) == 1 ||
			sscanf(line, "TRACE_BUFS_PER_LOG=\"%d\"", &_data) == 1)
		{
			_info("TRACE_BUFS_PER_LOG=\"%d\"", _data);
			trace_cfg.buf_per_log = _data;
			continue;
		}
		if(sscanf(line, "TRACE_MAX_LOGS \"%d\"", &_data) == 1 ||
			sscanf(line, "TRACE_MAX_LOGS=\"%d\"", &_data) == 1)
		{
			_info("TRACE_MAX_LOGS=\"%d\"", _data);
			trace_cfg.max_logs = _data;
			continue;
		}
		if(sscanf(line, "TRACE_COMPRESS \"%d\"", &_data) ||
			sscanf(line, "TRACE_COMPRESS=\"%d\"", &_data))
		{
			_info("TRACE_COMPRESS=\"%d\"", _data ? 1 : 0);
			trace_cfg.compress = (_data ? 1 : 0);
			continue;
		}
		if(sscanf(line, "TRACE_BUFS_PER_LOG_%" MAX_FILENAME_STR "s \"%d\"", _channel, &_data) == 2 ||
			sscanf(line, "TRACE_BUFS_PER_LOG_%" MAX_FILENAME_STR "s=\"%d\"", _channel, &_data) == 2)
		{
			_info("TRACE_BUFS_PER_LOG_%s=\"%d\"", _channel, _data);
			get_trace_channel_cfg(&trace_cfg, _channel, 1)->buf_per_log = _data;
			continue;
		}
		if(sscanf(line, "TRACE_MAX_LOGS_%" MAX_FILENAME_STR "s \"%d\"", _channel, &_data) == 2 ||
			sscanf(line, "TRACE_MAX_LOGS_%" MAX_FILENAME_STR "s=\"%d\"", _channel, &_data) == 2)
		{
			_info("TRACE_MAX_LOGS_%s=\"%d\"", _channel, _data);
			get_trace_channel_cfg(&trace_cfg, _channel, 1)->max_logs = _data;
			continue;
		}
		if(sscanf(line, "TRACE_LOGS_PATH \"%" MAX_FILENAME_STR "[^\"]\"", _logs_path) == 1 ||
			sscanf(line, "TRACE_LOGS_PATH=\"%" MAX_FILENAME_STR "[^\"]\"", _logs_path) == 1)
		{
			_info("TRACE_LOGS_PATH=\"%s\"", _logs_path);
			strncpy(trace_cfg.logs_path, _logs_path, sizeof(trace_cfg.logs_path));
			continue;
		}
		if(sscanf(line, "LOGGERS_CGROUP \"%" MAX_CGROUP_NAME_STR "[^\"]\"", _loggers_cgroup) == 1)
		{
			_info("LOGGERS_CGROUP=\"%s\"", _loggers_cgroup);
			strncpy(trace_cfg.cgroup, _loggers_cgroup, sizeof(trace_cfg.cgroup));
			continue;
		}
	}
	fclose(fp);
	if(line)
		free(line);

	if (trace_cfg.logs_path[0] != 0) {
		struct statfs stat;
		if (statfs(trace_cfg.logs_path, &stat)) {
			_suicide("Could not stat %s", trace_cfg.logs_path);
		}
		_debug("fs_type = %ld", stat.f_type);
		if (stat.f_type == NFS_SUPER_MAGIC) {
			_info("Running with NFS support");
			trace_cfg.nfs = 1;
		}
		else if (stat.f_type == RAMFS_MAGIC || stat.f_type == TMPFS_MAGIC) {
			_info("Running with RAMDISK support");
			trace_cfg.ramfs = 1;
		}

	}
}

void reconf(trace_channel_ctx_t ctx[])
{
	int i;
	for(i = 0; i < MAX_TRACE_CHANNELS; ++i)
	{
		if(ctx[i].ch)
			reconf_trace_channel(ctx[i].ch);
	}
	if(io_pet_ch)
		reconf_io_pet_channel(io_pet_ch);
}

void overide_default_compress_cfg(void)
{
	(void)nvmesh_is_prod_env_var_name;
#if 0
	const char *c_is_prod;
	int is_prod = 1; /* Default to production mode */

	c_is_prod = getenv(nvmesh_is_prod_env_var_name);
	if (c_is_prod) {
		is_prod = atoi(c_is_prod);
	}
	trace_cfg.compress = 0;
	trace_cfg.compress = is_prod ? 0 : 1;
#endif
}

/** We use SIGHUP as a trigger to reread configuration at runtime.
 * This is a common prcatice for daemons.
 */
void handle_sighup(int signal)
{
	if(signal != SIGHUP)
	{
		_error("Oops, caught wrong signal %d, should not happen", signal);
		return;
	}
	_info("SIGHUP, reread config") clean_channel_cfg(&trace_cfg);
	overide_default_compress_cfg();
	reread_config(GLOBAL_CONFIG_FILE);
	reread_config(LOCAL_CONFIG_FILE);
	reconf(ctx);
}

int main(int argc, char* argv[])
{
	struct sigaction sa;
	const char *dir;

	/* start with cwd */
	trace_cfg.logs_path[0] = '.';

	overide_default_compress_cfg();
	/*First config read*/
	reread_config(GLOBAL_CONFIG_FILE);
	reread_config(LOCAL_CONFIG_FILE);

	/* Parse args - args may override some config */
	if(argc > 3)
	{
		print_help(argv[0]);
		return 0;
	}
	else if(argc > 1)
	{
		if(!strcmp(argv[1], "-h"))
		{
			print_help(argv[0]);
			return 0;
		}
		trace_cfg.buf_per_log = atoi(argv[1]);
		if(argc > 2)
			trace_cfg.max_logs = atoi(argv[2]);
		if(trace_cfg.buf_per_log <= 0 || trace_cfg.max_logs <= 0)
		{
			_error("Invalid input");
			print_help(argv[0]);
			return EINVAL;
		}
	}

	{
		/*Setup SIGHUP handler*/
		sa.sa_handler = &handle_sighup;
		sa.sa_flags = SA_RESTART;
		/*Block every signal during the handler*/
		sigfillset(&sa.sa_mask);
		/*Intercept SIGHUP*/
		if(sigaction(SIGHUP, &sa, NULL) == -1)
		{
			_suicide("Could not setup the signal handler");
		}
	}

	if (trace_cfg.logs_path[0] == 0)
		dir = ".";
	else
		dir = trace_cfg.logs_path;

	if (trace_cfg.cgroup[0] != 0) {
		char pid[12] = {0};
		char cgroup_procs[MAX_FILENAME];
		int fd = -1;

		snprintf(cgroup_procs, MAX_FILENAME, "/sys/fs/cgroup/blkio/%s/cgroup.procs", trace_cfg.cgroup);
		fd = open(cgroup_procs, O_WRONLY, 0755);
		if (fd < 0) {
			_suicide("Failed to assign %s", cgroup_procs);
		}

		sprintf(pid, "%ld", (long int)(getpid()));
		if (write(fd, pid, strlen(pid)) != strlen(pid)) {
			_suicide("Failed to assign pid(%s) to cgroup", pid);
		}
	}

	/* Actual work */
	while(1)
	{
		_info("Waiting for control proc");
		while(access(LIST_CHANNELS_PROC, F_OK) == -1)
			usleep(PING_INTERVAL); /* Wait until control proc is available */
		while(access(MMAP_PROC, F_OK) == -1)
			usleep(PING_INTERVAL); /* Wait until control proc is available */
		while(access(IO_PET_PROC_PATH, F_OK) == -1)
			usleep(PING_INTERVAL); /* Wait until control proc is available */

		_info("Control proc available");
		{
			/* Start with mmap manager */
			mmap_manager_t* mmap = init_mmap_manager(MMAP_PROC);
			int i = 0;
			if(!mmap)
				_suicide("Create mmap manager %s", MMAP_PROC);

			/* Now create channel workers */
			{
				FILE* flist = fopen(LIST_CHANNELS_PROC, "r");
				char tname[MAX_TRACE_NAME];

				if(!flist)
					_suicide("Open %s", LIST_CHANNELS_PROC);
				for(i = 0; i < MAX_TRACE_CHANNELS && fscanf(flist, "%99s\n", tname) != EOF; ++i)
				{
					if(tname[0] == '#')
						continue; /* # are non flushable channels, appear there for debug only */
					ctx[i].ch = init_trace_channel(dir, mmap, tname, i);
					if(!ctx[i].ch)
						_suicide("Init trace channel %s", tname);
					if(start_trace_channel(ctx[i].ch))
						_suicide("Start trace channel %s", tname);
				}
				fclose(flist);
			}

			/* Initialize IO PET channel */
			_info("Initializing IO PET channel");
			io_pet_ch = init_io_pet_channel(dir, IO_PET_CHANNEL_NAME);
			if(!io_pet_ch){
				_suicide("Failed to init IO PET channel");
			}
			if(start_io_pet_channel(io_pet_ch)){
				_suicide("Failed to start IO PET channel");
			}
			
			_info("Closed control proc, working");
			//There is a bug here, in some cases, the thread may not start running or trying to read from the proc 
			//but we will call abort functionality. We need some barrier here


			for(i = 0; i < MAX_TRACE_CHANNELS; ++i)
			{
				destroy_trace_channel(ctx[i].ch);
				ctx[i].ch = NULL;
			}
			
			destroy_io_pet_channel(io_pet_ch);
			io_pet_ch = NULL;

			destroy_mmap_manager(mmap);

			_info("Cycle terminated");
		}
	}
	return 0;
}
