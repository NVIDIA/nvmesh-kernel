#ifndef UNLINK_LIST_H
#define UNLINK_LIST_H

#include <time.h>
#include <pthread.h>

#define MAX_TS ((unsigned long)-1)

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
	int open_files;
	const char *dir;
	const char *name;
} unlink_list_t;

/* Initialize unlink list */
void unlink_list_init(unlink_list_t *list, const char *dir, const char *name);

/* Clear unlink list (free all entries, keep mutex initialized) */
void unlink_list_clear(unlink_list_t *list);

/* Destroy unlink list (clear entries and destroy mutex) */
void unlink_list_destroy(unlink_list_t *list);

/* Add candidate to unlink list */
void unlink_list_add(unlink_list_t *list, int cpu, int id, unsigned long ts);

/* Get next candidate to unlink (caller must free the candidate after use) */
unlink_candidate_t* unlink_list_get_next(unlink_list_t *list, int max_logs);

/* Increment open files counter */
void unlink_list_increment_open_files(unlink_list_t *list);

/* Decrement open files counter */
void unlink_list_decrement_open_files(unlink_list_t *list);

/* Try to remove log file (returns 0 on success, -1 on error, ignores ENOENT) */
int unlink_list_try_remove_log_file(unlink_list_t *list, 
	unlink_candidate_t *cand, const char *subdir, const char *ext);

/* Parse log filename to extract CPU and log ID (format: name + cpu + "." + id) */
int unlink_list_parse_log_filename(const char *fname, const char *tname, int *cpu, int *idx, int max_cpus);

/* Scan directory and populate unlink list with existing log files */
int unlink_list_populate(unlink_list_t *list, int max_cpus);

/* Process unlink list and remove old log files (returns number of files unlinked) */
int unlink_list_do_unlink(unlink_list_t *list, int max_logs);

#endif /* UNLINK_LIST_H */

