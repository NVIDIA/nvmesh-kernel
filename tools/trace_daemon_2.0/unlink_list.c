#include "unlink_list.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>

#define __MODULE_HDR "%s"
#define __MODULE_HDR_ARGS "unlink_list"

#include "trace_daemon_common.h"

/**
 * Initialize unlink list
 */
void unlink_list_init(unlink_list_t *list, const char *dir, const char *name)
{
	pthread_mutex_init(&list->lock, NULL);
	list->len = 0;
	list->head = NULL;
	list->tail = NULL;
	list->open_files = 0;
	list->dir = dir;
	list->name = name;
}

/**
 * Clear unlink list (free all entries, keep mutex initialized)
 */
void unlink_list_clear(unlink_list_t *list)
{
	pthread_mutex_lock(&list->lock);
	while (list->head) {
		unlink_candidate_t *tmp = list->head;
		list->head = tmp->next;
		free(tmp);
	}
	list->tail = NULL;
	list->len = 0;
	list->open_files = 0;
	/* Keep dir and name - they don't need to be cleared */
	pthread_mutex_unlock(&list->lock);
}

/**
 * Destroy unlink list (clear entries and destroy mutex)
 */
void unlink_list_destroy(unlink_list_t *list)
{
	unlink_list_clear(list);
	pthread_mutex_destroy(&list->lock);
	list->dir = NULL;
	list->name = NULL;
}

/**
 * Add candidate to unlink list
 */
void unlink_list_add(unlink_list_t *list, int cpu, int id, unsigned long ts)
{
	pthread_mutex_lock(&list->lock); /* Critical section */
	if (!list->tail) {
		assert((list->head = list->tail = calloc(1, sizeof(unlink_candidate_t))));
		list->tail->cpu = cpu;
		list->tail->id = id;
		list->tail->ts = ts;
	} else {
		unlink_candidate_t *cand;
		assert(list->head); /* Sanity check */
		assert((cand = calloc(1, sizeof(unlink_candidate_t))));
		cand->cpu = cpu;
		cand->id = id;
		cand->ts = ts;
		if (ts == MAX_TS) {
			/* If timestamp not specified - just add to tail */
			list->tail->next = cand;
			list->tail = cand;
		} else {
			/* Else - lets find where to add it (sorted by timestamp) */
			unlink_candidate_t *pos = list->head;
			unlink_candidate_t *prev = NULL;
			while (pos && ts > pos->ts) {
				prev = pos;
				pos = pos->next;
			}
			if (!pos) {
				/* Add to tail */
				list->tail->next = cand;
				list->tail = cand;
			} else {
				cand->next = pos;
				if (prev) {
					prev->next = cand;
				} else {
					list->head = cand;
				}
			}
		}
	}
	if (!list->head)
		abort();
	++list->len;
	pthread_mutex_unlock(&list->lock); /* Critical section end */
}

/**
 * Get next candidate to unlink (caller must free the candidate after use)
 * Returns NULL if no candidate should be unlinked
 */
unlink_candidate_t* unlink_list_get_next(unlink_list_t *list, int max_logs)
{
	unlink_candidate_t *cand = NULL;

	pthread_mutex_lock(&list->lock); /* Critical section */
	if (max_logs > 0 && ((list->len + list->open_files) > max_logs)) {
		cand = list->head;
		if (cand) {
			list->head = cand->next;
			--list->len;
			if (!list->len) {
				/* list is empty so reset tail as well */
				list->tail = list->head;
			}
		}
	}
	pthread_mutex_unlock(&list->lock); /* Critical section end */

	return cand;
}

/**
 * Increment open files counter (thread-safe)
 */
void unlink_list_increment_open_files(unlink_list_t *list)
{
	pthread_mutex_lock(&list->lock);
	list->open_files++;
	pthread_mutex_unlock(&list->lock);
}

/**
 * Decrement open files counter (thread-safe)
 */
void unlink_list_decrement_open_files(unlink_list_t *list)
{
	pthread_mutex_lock(&list->lock);
	list->open_files--;
	pthread_mutex_unlock(&list->lock);
}

/**
 * Try to remove log file (returns 0 on success, -1 on error, ignores ENOENT)
 */
int unlink_list_try_remove_log_file(unlink_list_t *list, 
	unlink_candidate_t *cand, const char *subdir, const char *ext)
{
	char filename[MAX_FILENAME];
	int rv;

	if (subdir && subdir[0] != '\0') {
		snprintf(filename, sizeof(filename), "%s/%s/%s%d.%d%s", 
			list->dir, subdir, list->name, cand->cpu, cand->id, ext ? ext : "");
	} else {
		snprintf(filename, sizeof(filename), "%s/%s%d.%d%s", 
			list->dir, list->name, cand->cpu, cand->id, ext ? ext : "");
	}

	/* Use syscall_or_nfs_syscall macro with NULL as self (self is not used by the macro) */
	rv = syscall_or_nfs_syscall(NULL, unlink, filename);
	if (rv < 0) {
		if (errno != ENOENT) { /* ignore missing files */
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
 * Check whether string @str starts with string @pre
 */
static int _starts_with(const char* pre, const char* str)
{
	size_t lenpre = strlen(pre), lenstr = strlen(str);
	return lenstr < lenpre ? 0 : strncmp(pre, str, lenpre) == 0;
}

/**
 * Parse log filename to extract CPU and log ID (format: name + cpu + "." + id)
 */
int unlink_list_parse_log_filename(const char *fname, const char *tname, int *cpu, int *idx, int max_cpus)
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
		if(*cpu >= max_cpus)
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
 * Scan directory and populate unlink list with existing log files
 */
int unlink_list_populate(unlink_list_t *list, int max_cpus)
{
	DIR* dp;
	struct dirent* ep;

	dp = opendir(list->dir);
	if(dp != NULL)
	{
		while((ep = readdir(dp)) != NULL)
		{
			int cpu, idx;
			if(unlink_list_parse_log_filename(ep->d_name, list->name, &cpu, &idx, max_cpus))
			{
				char path[MAX_FILENAME];
				time_t ts;
				struct stat st;
				snprintf(path, sizeof(path), "%s/%s", list->dir, ep->d_name);

				if(stat(path, &st))
					ts = 0;
				else
					ts = st.st_mtime;

				unlink_list_add(list, cpu, idx, ts);
			}
		}
		closedir(dp);
	}
	else
	{
		_error("Listing working dir %s", list->dir);
		/* We can continue in this situation, just report error and continue */
	}

	return 0;
}

/**
 * Process unlink list and remove old log files (returns number of files unlinked)
 */
int unlink_list_do_unlink(unlink_list_t *list, int max_logs)
{
	int unlinked_count = 0;

	while(1)
	{
		unlink_candidate_t* cand = unlink_list_get_next(list, max_logs);

		if(cand)
		{ /* We do have a valid candidate - actually unlink now */
			_info("Do unlink %s cpu=%d id=%d", list->name, cand->cpu, cand->id);
			unlink_list_try_remove_log_file(list, cand, "", "");
			unlink_list_try_remove_log_file(list, cand, "", ".lz4");
			unlink_list_try_remove_log_file(list, cand, ".cache", "");
			free(cand);
			unlinked_count++;
		}
		else
		{
			return unlinked_count; /*Finished*/
		}
	}
}

