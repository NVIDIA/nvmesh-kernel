#include "nvmeibt_debug.h"
#include "nvmeibt_common.h"
#include <ctype.h>
#include <dirent.h>
#include <libgen.h>
#include <malloc.h>
#include <pthread.h>
#include <linux/fs.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include "nvmeibt_local_disk_util.h"
#include "nvmeibt_local_disk.h"
#include "nvmeibt_ds.h"
#include "nvmeibt_toma.h"
#include "nvmeibt_topology.h"
#include "../common/nvmeib_heap.c"     // Compiling the shared heap code for TOMA without duplicating it

struct nvmeibt_alloc_free_summary_entry nvmeibt_alloc_free_summary_table[ALLOC_FREE_TABLE_SIZE];

#define MAX_TRANSFER_SIZE (uint64_t)65536 /* 64K aligned, otherwise we can have at most 124KB read, for disks that limit transfer to 128KB (including MD) */
#define MAX_READ_ATTEMPT_NUM	3

char *get_8_plus_3_char_str_of_now(void)
{
	static int64_t		prev_sec = INT_MAX;
	static unsigned int	prev_ms = INT_MAX;
	static char			return_8_plus_3_val[sizeof("hh:mm:ss") + 7 + 2];
	unsigned int		ms;
	struct timespec		now;
	struct tm			*_tm, tmp_tm;

	getnstimeofday(&now);
	if (now.tv_sec != prev_sec) {
		prev_sec = now.tv_sec;
		_tm = localtime_r(&(now.tv_sec), &tmp_tm);
		sprintf(return_8_plus_3_val, "%02d:%02d:%02d", _tm->tm_hour, _tm->tm_min, _tm->tm_sec);
		return_8_plus_3_val[sizeof("hh:mm:ss") - 1] = '.';	// overide the \0 that sprintf generated
	}
	ms = (unsigned int)NSEC_TO_MSEC(now.tv_nsec);
	if (ms != prev_ms) {
		prev_ms = ms;
		sprintf(return_8_plus_3_val + sizeof("hh:mm:ss"), "%03d", ms);
	}
	return return_8_plus_3_val;
}

static pthread_mutex_t alloc_free_guard;
static int lock_alloc_free_table(void)
{
	const int rv = pthread_mutex_lock(&alloc_free_guard);
	if (rv != 0) {
		N_Ef(error_common_lock_alloc_free_table, "Failed to lock rv=@RV, @AUTO_ERRNO", rv);
		nvmeibt_abort(ES_FATAL);
	}
	return rv;
}

static int unlock_alloc_free_table(void)
{
	const int rv = pthread_mutex_unlock(&alloc_free_guard);
	if (rv != 0) {
		N_Ef(error_common_unlock_alloc_free_table, "Failed to unlock rv=@RV, @AUTO_ERRNO", rv);
		nvmeibt_abort(ES_FATAL);
	}
	return rv;
}

static int static_alloc_free_idx;

int nvmeibt_get_alloc_free_table_idx(void)
{
	int			idx;

	lock_alloc_free_table();
	if (!static_alloc_free_idx) {
		static_alloc_free_idx = 2;
		nvmeibt_strlcpy(nvmeibt_alloc_free_summary_table[0].fname, "ALLOC_TOTAL", sizeof(nvmeibt_alloc_free_summary_table[0].fname));
		nvmeibt_alloc_free_summary_table[0].type = 'A';
		nvmeibt_strlcpy(nvmeibt_alloc_free_summary_table[1].fname, "FREE_TOTAL", sizeof(nvmeibt_alloc_free_summary_table[1].fname));
		nvmeibt_alloc_free_summary_table[1].type = 'F';
	}
	idx = static_alloc_free_idx++;
	if (idx > ALLOC_FREE_TABLE_SIZE) {
		N_Ef(error_common_nvmeibt_get_alloc_free_table_idx, "Too many ALLOC/FREE in the code");
		nvmeibt_abort(ES_FATAL);
	}
	unlock_alloc_free_table();
	// printf("nvmeibt_get_alloc_free_table_idx idx=%d\n", idx);
	return idx;
}

void *nvmeibt_alloc_free_and_account(
				enum nvmeibt_toma_alloc_mode mode,
				int idx, void *ptr, size_t nmemb, long long size)
{
	long long	prev_size = 0;
	long long	real_size;
	void		*p;
	int			summary_table_sum_line_idx = 0;	// 0 for alloc, 1 for free

	/*
	 * XXX we rely on glibc behavior that uses the size_t before the
	 * pointer to store the actual size allocated by the system.
	 */

	switch (mode) {
	case NVMEIBT_TOMA_MEM_FREE:
		summary_table_sum_line_idx = 1;
		p = ptr;
		real_size = -malloc_usable_size(p);
		free(p);
		break;
	case NVMEIBT_TOMA_MEM_MALLOC:
		p = nvmeibt_toma_malloc(size);
		real_size = malloc_usable_size(p);
		break;
	case NVMEIBT_TOMA_MEM_CALLOC:
		p = nvmeibt_toma_calloc(nmemb, size);
		real_size = malloc_usable_size(p);
		size = size * nmemb;
		break;
	case NVMEIBT_TOMA_MEM_REALLOC:
		p = ptr;
		prev_size = malloc_usable_size(p);
		p = nvmeibt_toma_realloc(ptr, size);
		real_size = malloc_usable_size(p);
		break;
	case NVMEIBT_TOMA_MEM_ALIGN:
		nvmeibt_toma_posix_memalign((void **) ptr, nmemb, (size_t) size);
		p = *((void **)ptr);
		real_size = malloc_usable_size(p);
		break;
	case NVMEIBT_TOMA_REG_RSC:
		p = ptr;
		real_size = size;
		break;
	case NVMEIBT_TOMA_UREG_RSC:
		summary_table_sum_line_idx = 1;
		p = ptr;
		real_size = -size;
		break;
	case NVMEIBT_TOMA_BM_ALLOC:
	case NVMEIBT_TOMA_BM_CALLOC:
	case NVMEIBT_TOMA_BM_ALIGNED_ALLOC:
	case NVMEIBT_TOMA_BM_ALIGNED_CALLOC:
		p = ptr;
		real_size = nvmeibt_bm_get_buf_alloc_size(p);
		break;
	case NVMEIBT_TOMA_BM_FREE:
		p = ptr;
		real_size = -nvmeibt_bm_get_buf_alloc_size(p);
		break;
	default:
		N_Ef(rwwoksu, "Unknown mode=@INT", mode);
			nvmeibt_abort(ES_FATAL);
			// Avoid compillation errors
			real_size = 0;
			p = NULL;
	}

	nvmeibt_alloc_free_summary_table[idx].n_calls++;
	nvmeibt_alloc_free_summary_table[idx].sum_sizes += size - prev_size;
	nvmeibt_alloc_free_summary_table[idx].sum_allocated_size += real_size - prev_size;

	if (mode & (NVMEIBT_TOMA_MEM_FREE | NVMEIBT_TOMA_MEM_MALLOC |
				NVMEIBT_TOMA_MEM_CALLOC | NVMEIBT_TOMA_MEM_ALIGN |
				NVMEIBT_TOMA_MEM_REALLOC |
				NVMEIBT_TOMA_REG_RSC |
				NVMEIBT_TOMA_UREG_RSC)) {
		// The totals are affected only for non-BM. I.e., real alloc/free
		nvmeibt_alloc_free_summary_table[summary_table_sum_line_idx].n_calls++;
		nvmeibt_alloc_free_summary_table[summary_table_sum_line_idx].sum_sizes += size - prev_size;
		nvmeibt_alloc_free_summary_table[summary_table_sum_line_idx].sum_allocated_size += real_size - prev_size;
	}

	return p;
}

int nvmeibt_print_alloc_free_summary_table(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx)
{
	int			i, j;
	static int	n_printing_now;
	int			n_were_printing;
	size_t		total_alloc_minus_free_bytes;

	NFIN;
	lock_alloc_free_table();
		n_were_printing = n_printing_now++;
	unlock_alloc_free_table();
	if (n_were_printing) {
		goto skip;
	}
	total_alloc_minus_free_bytes = (nvmeibt_alloc_free_summary_table[0].sum_allocated_size + nvmeibt_alloc_free_summary_table[1].sum_allocated_size);
	if (printf_fn) {
		(*printf_fn)(printf_ctx, "total_alloc_minus_free=%jdM\n", total_alloc_minus_free_bytes >> 20);
		(*printf_fn)(printf_ctx, "\n- - - - -   MEM alloc and free   - - - - -\n");
		(*printf_fn)(printf_ctx, "File                          [Line] Type         n_calls          sum_sizes  sum_allocated_size\n");
	}
	else {
		N_Tf(trace_0_common_nvmeibt_print_alloc_free_summary_table, "total_alloc_minus_free=@ZU M\n", total_alloc_minus_free_bytes >> 20);
		N_Tf(trace_common_nvmeibt_print_alloc_free_summary_table, "\n- - - - -   MEM alloc and free   - - - - -");
		N_Tf(trace_1_common_nvmeibt_print_alloc_free_summary_table, "File                          [Line] Type         n_calls          sum_sizes  sum_allocated_size");
	}
	for (j = 0; j < 2; j++) {
		for (i = 0; i < static_alloc_free_idx; i++) {
			struct nvmeibt_alloc_free_summary_entry *e = &nvmeibt_alloc_free_summary_table[i];
			if (((j == 0) && (e->type > 'Z')) || ((j == 1) && (e->type <= 'Z'))) {
				continue;
			}
			if (printf_fn) {
				(*printf_fn)(printf_ctx, "%-30s[%4d] %c %18lld %18lld %18lldK\n", e->fname, e->line_no, e->type, e->n_calls, e->sum_sizes, e->sum_allocated_size >> 10);
			}
			else {
				N_Tf(trace_2_common_nvmeibt_print_alloc_free_summary_table, "@FNAME[@LINE_NO] @E_TYPE @N_CALLS @SUM_SIZES sum_allocated_size=@SUM_ALLOCATED_SIZE", e->fname, e->line_no, e->type, e->n_calls, e->sum_sizes, e->sum_allocated_size >> 10);
			}
		}
		if (j == 0) {
			if (printf_fn) {
				(*printf_fn)(printf_ctx, "- - - - - - - - - - - - - - - - - - - - - BM allocations - - - - - - - - - - - - - - - - - - - -\n");
			}
			else {
				N_Tf(trace_3_common_nvmeibt_print_alloc_free_summary_table, "- - - - - - - - - - - - - - - - - - - - - BM allocations - - - - - - - - - - - - - - - - - - - -");
			}
		}
	}
skip:
	lock_alloc_free_table();
		--n_printing_now;
	unlock_alloc_free_table();
	NFOUT;
	return 0;
}

void nvmeibt_validate_alloc_free_summary_table(void)
{
#ifdef TOMA_DEBUG
	size_t			total_alloc_minus_free_bytes;
	size_t			total_alloc_since_last_print;
	static size_t	last_print_total_alloc;
	const size_t	MAX_UNFREED_BYTES = (1000 * 1024 * 1024);

	NFIN;
	total_alloc_minus_free_bytes = (nvmeibt_alloc_free_summary_table[0].sum_allocated_size + nvmeibt_alloc_free_summary_table[1].sum_allocated_size);
	total_alloc_since_last_print = nvmeibt_alloc_free_summary_table[0].sum_allocated_size - last_print_total_alloc;
	if (total_alloc_minus_free_bytes < MAX_UNFREED_BYTES && total_alloc_since_last_print < 0x400000) {
		goto out;
	}
	nvmeibt_print_alloc_free_summary_table(NULL, NULL);
	if (total_alloc_minus_free_bytes >= MAX_UNFREED_BYTES) {
		N_Wf(warn_common_nvmeibt_validate_alloc_free_summary_table, "OOPS! total_alloc_minus_free=@ZU", total_alloc_minus_free_bytes >> 20);
		nvmeibt_abort(ES_FATAL);
	}
	last_print_total_alloc = nvmeibt_alloc_free_summary_table[0].sum_allocated_size;
out:
	NFOUT;
#endif	// #ifdef TOMA_DEBUG
}

const char *nvmeibt_zeroing_state_str(enum NVMEIBT_ZEROING_STATE s)
{
	switch (s) {
	case NVMEIBT_ZEROING_STATE_UNINITIALIZED:	return "UNINITIALIZED";
	case NVMEIBT_ZEROING_STATE_REQUIRED:		return "REQUIRED";
	case NVMEIBT_ZEROING_STATE_IN_WORK:			return "IN_WORK";
	case NVMEIBT_ZEROING_STATE_DONE:			return "DONE";
	case NVMEIBT_ZEROING_STATE_NOT_NEEDED:		return "NOT_NEEDED";
	default :									return "???";
	}
}

uint64_t align_pba_e_down_to_blkset(const uint64_t pba_e, const int pblk_size)
{
	int		n_pblks_in_blkset = ALIGNED_1MB / pblk_size;

	return (rounddown(pba_e + 1, n_pblks_in_blkset) - 1);
}

uint64_t align_pba_s_up_to_blkset(const uint64_t pba_s, const int pblk_size)
{
	int		n_pblks_in_blkset = ALIGNED_1MB / pblk_size;

	return roundup(pba_s, n_pblks_in_blkset);
}

static inline unsigned int nvmeibt_n_blks_in_blkset(unsigned int blk_size)
{
	return (NUM_4KBLKS_IN_BLKSET * 4096 / blk_size);
}

uint64_t nvmeibt_align_n_blks_to_write_to_blkset(uint64_t pba_s, uint64_t n_pblk_to_write, unsigned int blk_size)
{
	uint64_t 	target_end_blk;

	// Round so that we will end BLKSET-aligned, and the next round will start aligned
	target_end_blk = rounddown(pba_s + n_pblk_to_write, nvmeibt_n_blks_in_blkset(blk_size));
	return (target_end_blk > pba_s ? target_end_blk - pba_s : n_pblk_to_write);
}

struct zeroing_ctx_data {
	pthread_mutex_t guard_mutex;
	pthread_cond_t 	zeroing_signal;
	int 			rv;
};

void zero_blks_on_done(void *ctx, int is_ok, struct nvmeib_nl_uk_comm_rep *msg)
{
	struct zeroing_ctx_data *zero_ctx = ctx;

	NFIN;
	NTOMA_ASSERT(error_common_zero_blks_on_done, (!msg && !is_ok) || msg->opcode == csc_zero_disk,
				"zeroing done, on non zeroing msg of type=@TYPE(@CSC_ZERO_DISK)", msg->opcode, csc_zero_disk);

	if (!zero_ctx) {
		 N_Ef(error_1_common_zero_blks_on_done, "Cannot wakeup caller thread as zeroing ctx is NULL");
		 nvmeibt_abort(ES_FATAL);
	}

	if (pthread_mutex_lock(&zero_ctx->guard_mutex) != 0) {
		N_Ef(error_2_common_zero_blks_on_done, "Cannot wakeup caller thread, cannot lock_mutex=@LOCK_MUTEX error: @AUTO_ERRNO", &zero_ctx->guard_mutex);
		nvmeibt_abort(ES_FATAL);
	}

	zero_ctx->rv = is_ok ? 0 : 1;

	// Wake the thread that called this zeroing operation.
	if (pthread_cond_signal(&zero_ctx->zeroing_signal) != 0) {
		N_Ef(error_3_common_zero_blks_on_done, "Cannot wakeup caller thread with cond_var=@COND_VAR pthread signal error: @AUTO_ERRNO", &zero_ctx->zeroing_signal);
		nvmeibt_abort(ES_FATAL);
	}

	if (pthread_mutex_unlock(&zero_ctx->guard_mutex) != 0) {
		N_Ef(error_4_common_zero_blks_on_done, "Cannot wakeup caller thread, cannot unlock_mutex=@UNLOCK_MUTEX error: @AUTO_ERRNO", &zero_ctx->guard_mutex);
		nvmeibt_abort(ES_FATAL);
	}

	NFOUT;
}

static void __zero_disk_fill_payload_params(struct nvmeib_zero_disk *m, const struct nvmeibt_ldisk_id_for_srvr_cmd *disk)
{
	const struct nvmeibt_disk_flow_params_t *p = nvmeibt_disk_flow_params_get(disk->Model, true);
	m->is_zeroing_using_test_and_write =   (bool)p->is_zeroing_using_test_and_write;
	m->is_using_nvme_trim_before_zero =    (bool)p->is_using_nvme_trim_before_zero;
	m->is_secure_erase_after_disk_format = (bool)p->is_secure_erase_after_disk_format;
	m->is_zeroing_mandatory =              (bool)p->is_zeroing_mandatory;
	N_Tf(trace_common_zero_disk_fill_payload_params,
		 "zero_payload={is_TandW=@RV, is_trim=@RV, is_secure=@RV, is_0_mand=@RV}",
		 m->is_zeroing_using_test_and_write, m->is_using_nvme_trim_before_zero,
		 m->is_secure_erase_after_disk_format, m->is_zeroing_mandatory);
}

int nvmeibt_zero_disk_pblks(const struct nvmeibt_ldisk_id_for_srvr_cmd *ldisk,
		uint64_t pba_s, uint64_t n_pblk_to_zero, BOOL are_hw_blks)
{
	int						rv = -1;
	struct km_comm_msg_hdr *zero_msg;
	struct nvmeib_zero_disk *zero_msg_payload;
	struct zeroing_ctx_data *zero_ctx;
	pthread_condattr_t attr;

	NFIN;

	N_Tf(iemxjud, "disk=@STR zeroing pba_s=@PBA_S n_pblk=@N_PBLK", ldisk->ldisk_id.str, pba_s, n_pblk_to_zero);

	zero_msg = NNVMEIBT_BM_CALLOC(trace_common_nvmeibt_zero_disk_pblks, sizeof(*zero_msg) + sizeof(*zero_msg_payload));	// Disk zeroing does not work with malloc, must use calloc!
	zero_msg_payload = (struct nvmeib_zero_disk *)(zero_msg->data);
	zero_ctx = NNVMEIBT_BM_CALLOC(trace_1_common_nvmeibt_zero_disk_pblks, sizeof(*zero_ctx));

	/* Send a message to nvmeibs to zero the required blocks.
	   Wait for the callback of the completion to wake us up.*/

	snprintf(zero_msg_payload->disk_id, sizeof(zero_msg_payload->disk_id), "%.*s", (int)(sizeof(zero_msg_payload->disk_id) - 1), ldisk->ldisk_id.str);
	zero_msg_payload->vendor_id = ldisk->vendor_id;
	zero_msg_payload->start_hw_sector = pba_s;
	zero_msg_payload->n_hw_sectors = n_pblk_to_zero;
	zero_msg_payload->is_hw = are_hw_blks;
	__zero_disk_fill_payload_params(zero_msg_payload, ldisk);

	zero_msg->opcode = csc_zero_disk;
	zero_msg->on_done = zero_blks_on_done;
	zero_msg->ctx = (void *)zero_ctx;
	zero_msg->len = sizeof(*zero_msg_payload);

	if (pthread_mutex_init(&zero_ctx->guard_mutex, NULL) != 0) {
		N_Ef(error_common_nvmeibt_zero_disk_pblks, "Failed to create zero context guard @AUTO_ERRNO");
		goto out;
	}
	if (pthread_condattr_init(&attr) != 0) {
		N_Ef(error_1_common_nvmeibt_zero_disk_pblks, "Failed to create cond var attr @AUTO_ERRNO");
		goto free_mutex;
	}
	if (pthread_cond_init(&zero_ctx->zeroing_signal, &attr) != 0) {
		N_Ef(error_2_common_nvmeibt_zero_disk_pblks, "Failed to create zero context cond var @AUTO_ERRNO");
		goto free_mutex;
	}

	// Lock mutex
	if (pthread_mutex_lock(&zero_ctx->guard_mutex) != 0) {
		N_Ef(error_3_common_nvmeibt_zero_disk_pblks, "Cannot wakeup caller thread, cannot lock_mutex=@LOCK_MUTEX error: @AUTO_ERRNO", &zero_ctx->guard_mutex);
		nvmeibt_abort(ES_FATAL);
	}

	if (nvmeibt_send_msg_to_srv(zero_msg) != 0) {
		N_Ef(vnks023, "disk=@STR Unable to send local_disk_zero msg to srv!", ldisk->ldisk_id.str);
		goto free_cond;
	}

	// Wait for zeroing to finish
	if (pthread_cond_wait(&zero_ctx->zeroing_signal, &zero_ctx->guard_mutex) != 0) {
		N_Ef(error_5_common_nvmeibt_zero_disk_pblks, "Cannot wait for zeroing to finish, cond_var=@COND_VAR error: @AUTO_ERRNO", &zero_ctx->zeroing_signal);
		// nvmeibt_abort(ES_FATAL);
		goto free_cond;
	}

	if (zero_ctx->rv == 0) {
		N_Tf(qnccowl, "disk=@STR zeroed pba_s=@PBA_S n_pblk=@N_PBLK", ldisk->ldisk_id.str, pba_s, n_pblk_to_zero);
	}
	else {
		N_Ef(47j2lap, "disk=@STR failed to zero pba_s=@PBA_S n_pblk=@N_PBLK zero_ctx->rv=@RV", ldisk->ldisk_id.str, pba_s, n_pblk_to_zero, zero_ctx->rv);
		goto free_cond;
	}

	rv = 0;

free_cond:
	if (pthread_mutex_unlock(&zero_ctx->guard_mutex)) {
		N_Ef(xx_30, "Failed to unlock zero context guard @AUTO_ERRNO");
	}
	if (pthread_cond_destroy(&zero_ctx->zeroing_signal)) {
		N_Ef(xx_31, "Failed to destroy zero context cond var @AUTO_ERRNO");
	}

free_mutex:
	if (pthread_mutex_destroy(&zero_ctx->guard_mutex)) {
		N_Ef(xx_32, "Failed to destroy zero context guard @AUTO_ERRNO");
	}

out:
	NNVMEIBT_BM_FREE(trace_3_common_nvmeibt_zero_disk_pblks, zero_msg);
	NNVMEIBT_BM_FREE(trace_4_common_nvmeibt_zero_disk_pblks, zero_ctx);

	NFOUT;
	return rv;
}

int nvmeibt_sscanf_csv_line(const char *line, ...)
{
	va_list 	ap;
	char 		val_type;
	void 		*val_ptr;
	const char 	*terminating_chr;
	const char	*cur_pos = line;
	int			is_eol = (*cur_pos == '\0');
	int			max_str_len;
	char		*endptr;
	int			is_ok = 1;
	int         strto_errno;

	va_start(ap, line);
	while (!is_eol){
		errno  		= 0;
		strto_errno = 0;
		endptr 		= NULL;
		terminating_chr = my_strchrnul(cur_pos, ',');
		is_eol = (*terminating_chr == '\0');
		val_type = (char)va_arg(ap, int);
		val_ptr = (void *)va_arg(ap, void *);
		if (val_ptr == 0) {
			break;
		}

		switch (val_type) {
		case '\0':
			is_eol = 1;
			N_Ef(error_common_nvmeibt_sscanf_csv_line, "The parsed string has extra arguments. memory overrun? line=\n@STR", line);
			is_ok = 0;
			break;
		case 's':              // string
			// Next arg is max_len
			max_str_len = (int)(long long)(val_ptr);
			val_ptr = (void *)va_arg(ap, void *);
			if (val_ptr == NULL) {
				is_eol = 1;
				break;
			}
			if ((terminating_chr - cur_pos) > (max_str_len - 1)) {
				N_Ef(error_1_common_nvmeibt_sscanf_csv_line, "String too long len=@LEN_LONG>@MAX_STR_LEN '@CUR_POS'", terminating_chr - cur_pos, max_str_len, cur_pos);
				is_ok = 0;
				is_eol = 1;
				break;
			}
			else {
				nvmeibt_strlcpy((char *)val_ptr, cur_pos, (terminating_chr - cur_pos + 1));
			}
			break;
		case 'g':              // 36 char GUID string with 4 dashes inside it.
			if (terminating_chr - cur_pos != URN_UUID_STR_LENGTH) {
				if (terminating_chr - cur_pos > 0) {
					N_Ef(error_2_common_nvmeibt_sscanf_csv_line, "Expecting " MACRO_DEF_TO_STR(URN_UUID_STR_LENGTH) " bytes UUID str @CUR_POS", cur_pos);
					is_ok = 0;
				}
			}
			nvmeibt_strlcpy((char *)val_ptr, cur_pos, (terminating_chr - cur_pos + 1));
			break;
		case '0':              // 2+32 char string that starts with "0x" (stripped)
			if (terminating_chr - cur_pos != 34) {
				N_Ef(error_3_common_nvmeibt_sscanf_csv_line, "Expecting 34 bytes UUID str @CUR_POS", cur_pos);
				is_ok = 0;
			}
			if (*cur_pos != '0' || *(cur_pos + 1) != 'x') {
				N_Ef(error_4_common_nvmeibt_sscanf_csv_line, "Expected the prefix '0x' before @CUR_POS", cur_pos);
				is_ok = 0;
			}
			else {
				cur_pos += 2;
			}
			nvmeibt_strlcpy((char *)val_ptr, cur_pos, (terminating_chr - cur_pos + 1));
			break;
		case 'd':              // int
			*(int *)val_ptr = strtol(cur_pos, &endptr, 10);
			strto_errno = errno;
			break;
		case 'D':              // int 64
			*(int64_t *)val_ptr = strtoll(cur_pos, &endptr, 10);
			strto_errno = errno;
			break;
		case 'u':              // U-int 32
			*(uint32_t *)val_ptr = strtoull(cur_pos, &endptr, 10);
			strto_errno = errno;
			break;
		case 'U':              // U-int 64
			*(unsigned long long int *)val_ptr = strtoull(cur_pos, &endptr, 10);
			strto_errno = errno;
			break;
		case 'x':              // HexaDecimal
			*(uint *)val_ptr = strtoul(cur_pos, &endptr, 16);
			strto_errno = errno;
			break;
		case 'X':              // HexaDecimal 64
			*(uint64_t *)val_ptr = strtoull(cur_pos, &endptr, 16);
			strto_errno = errno;
			break;
		case 'h':              // Hexadecimal starts with "0x" (stripped)
			if (*cur_pos != '0' || *(cur_pos + 1) != 'x') {
				N_Ef(error_5_common_nvmeibt_sscanf_csv_line, "Expected the prefix '0x' before @CUR_POS", cur_pos);
				is_ok = 0;
			}
			else {
				cur_pos += 2;
			}
			*(uint *)val_ptr = strtoul(cur_pos, &endptr, 16);
			strto_errno = errno;
			break;
		case 'c':              // char
			*(char *)val_ptr = *(char *)cur_pos;
			break;
		case 'B':              // BOOL (0/1)
			*(u8 *)val_ptr = strtoul(cur_pos, &endptr, 2);
			strto_errno = errno;
			break;
		case 'b':		// boolean
			if (strncmp(cur_pos, "true", sizeof("true") - 1) == 0)
				*(bool*)val_ptr = true;
			else if (strncmp(cur_pos, "false", sizeof("false") - 1) == 0)
				*(bool*)val_ptr = false;
			else {
				N_Ef(error_6_common_nvmeibt_sscanf_csv_line, "Invalid boolean string @CUR_POS", cur_pos);
				is_ok = 0;
			}
			break;
		default:
			N_Ef(error_7_common_nvmeibt_sscanf_csv_line, "Unknown val_type='@VAL_TYPE', cur_pos='@CUR_POS'", val_type, cur_pos);
			is_ok = 0;
		}

		if (strto_errno) {
			N_Ef(error_8_common_nvmeibt_sscanf_csv_line, "strto*(@STR) failed: val_type=@VAL_TYPE cur_pos=@PTR endptr=@PTR returned @STRERROR",\
				cur_pos, val_type, cur_pos, endptr, strerror(strto_errno));
			is_ok = 0;
		} else if (endptr == cur_pos) {
			N_Tf(vasgywu, "empty token type=@CHAR at pos=@SIZE_T", val_type, cur_pos - line);
		}

		cur_pos = terminating_chr + 1;
	}
    va_end(ap);
	return (is_ok ? cur_pos - line : -1);	// Return the total length scanned
}

static char my_hostname[NVMEIB_HOST_NAME_LEN] = "";
void nvmeibt_record_my_hostname(void)
{
	gethostname(my_hostname, sizeof(my_hostname) - 1);
	N_Tf(trace_common_nvmeibt_record_my_hostname, "my_node's hostname='@MY_HOSTNAME'", my_hostname);
}

const char *nvmeibt_get_my_hostname(void)
{
	return my_hostname;
}

void nvmeibt_common_init(void)
{
	nvmeibt_record_my_hostname();
}

int nvmeibt_fd_set_blocking(int fd, int blocking)
{
	int flags;
	int rv = -1;

	NFIN;
	flags = fcntl(fd, F_GETFL);
	if (flags < 0) {
		N_Ef(error_common_nvmeibt_fd_set_blocking, "fcntl on fd=@FD flags @FLAGS_INT (@AUTO_ERRNO)", fd, flags);
		goto out;
	}

	if (blocking) {
		flags &= ~O_NONBLOCK;
	}
	else {
		flags |= O_NONBLOCK;
	}

	if (fcntl(fd, F_SETFL, flags) < 0) {
		N_Ef(error_1_common_nvmeibt_fd_set_blocking, "fcntl on fd=@FD flags @FLAGS_INT (@AUTO_ERRNO)", fd, flags);
		goto out;
	}
	rv = 0;

out:
	NFOUT;
	return rv;
}

void nvmeibt_get_current_date_n_time(struct date_time *dt)
{
	struct timespec		now;
	struct tm tm;
	int64_t timep;

	if (getnstimeofday(&now) < 0) {
		return;
	}
	timep = (time_t)now.tv_sec;
	if (localtime_r(&timep, &tm) == 0) {
		return;
	}
	dt->year = tm.tm_year + 1900;
	dt->month = tm.tm_mon + 1;
	dt->day = tm.tm_mday;
	dt->hour = tm.tm_hour;
	dt->minute = tm.tm_min;
	dt->second = tm.tm_sec;
	dt->msecs = NSEC_TO_MSEC(now.tv_nsec);
}

int nvmeibt_vasprintf(char **str, const char *fmt, va_list args)
{
	int size = 0;
	va_list tmpa;

	NFIN;
	/* copy */
	va_copy(tmpa, args);

	/* apply variadic arguments to sprintf with format to get size */
	size = vsnprintf(NULL, 0, fmt, tmpa);

	/* toss args */
	va_end(tmpa);

	/* return -1 to be compliant if size is less than 0 */
	if (size < 0) {
		size = -1;
		goto out;
	}

	/* alloc with size plus 1 for `\0' */
	*str = (char *)NNVMEIBT_TOMA_MALLOC(trace_common_nvmeibt_vasprintf, size + 1);

	/* return -1 to be compliant if pointer is `NULL' */
	if (*str == NULL) {
		size = -1;
		goto out;
	}

	/* format string with original variadic arguments and set new size */
	size = vsprintf(*str, fmt, args);

out:
	NFOUT;
	return size;
}

int nvmeibt_asprintf(char **str, const char *fmt, ...)
{
	int size = 0;
	va_list args;

	NFIN;
	/* init variadic argumens */
	va_start(args, fmt);

	/* format and get size */
	size = nvmeibt_vasprintf(str, fmt, args);

	/* toss args */
	va_end(args);
	NFOUT;
	return size;
}

/*
 * Helpers to read/write from file descriptors exact payload size.
 * See toma/nvmeibt_common.h for details.
 */

/*
 * Generic write internal helper.
 * If @offset is negative use write(), otherwise used pwrite() with @offset.
 */
static ssize_t __do_write(int fd, const void *vptr, size_t size, off_t offset)
{
	const void	*ptr = vptr;
	size_t		count = 0;
	ssize_t		n;

	while (size > 0) {
		if (offset >= 0)
			n = pwrite(fd, ptr + count, size, offset + count);
		else
			n = write(fd, ptr + count, size);
		if (n < 0 && errno == EINTR) {
			N_Tf(__do_write_trace_1, "Interrupted write to fd=@FD (at @ZX/@ZX off @LLX), retrying",
				fd, count, size + count, (long long) offset);
			continue;
		} else if (n < 0 && errno == EAGAIN) {
			N_Tf(__do_write_trace_2, "Would-block write to fd=@FD (at @ZX/@ZX off @LLX), stopping",
				fd, count, size + count, (long long) offset);
			break;
		} else if (n < 0) {
			N_Tf(__do_write_trace_3, "Fail write to fd=@FD (at @ZX/@ZX off @LLX) (@AUTO_ERRNO)",
				fd, count, size + count, (long long) offset);
			return -1;
		} else if (n == 0) {
			/*
			 * POSIX states that if size > 0 then write(2) should never return 0.
			 * https://stackoverflow.com/questions/41904221/can-write2-return-0-bytes-written-and-what-to-do-if-it-does
			 */
			N_Ef(__do_write_error, "Zero write to fd=@FD (at @ZX/@ZX off @LLX)",
				fd, count, size + count, (long long) offset);
			NTOMA_ASSERT(__do_write_assert, 0, "Zero write to fd=@FD", fd);  /* only effective with TOMA_DEBUG set */
			errno = EIO;  /* EIO for general I/O error */
			return -1;
		}
		size -= n;
		count += n;
	}

	return count;
}

ssize_t nvmeibt_write(int fd, const void *vptr, size_t size)
{
	ssize_t rv;

	rv = __do_write(fd, vptr, size, -1);
	if (rv < 0) {
		N_Ef(error_common_nvmeibt_write, "Failed write to fd=@FD (@AUTO_ERRNO)", fd);
	} else if ((size_t ) rv < size) {
		N_Ef(error_1_common_nvmeibt_write, "Partial write @RV_SSIZE_T/@LLX to fd=@FD (@AUTO_ERRNO)", rv, (long long unsigned int)size, fd);
		rv = -1;
	}
	return rv;
}

ssize_t __nvmeibt_pwrite(int fd, const void *vptr, size_t size, off_t offset)
{
	ssize_t rv;

	NTOMA_ASSERT(error_common_nvmeibt_pwrite, offset >= 0, "invalid offset @OFFSET", (long long) offset);

	rv = __do_write(fd, vptr, size, offset);
	if (rv < 0) {
		N_Ef(error_1_common_nvmeibt_pwrite, "Failed write to fd=@FD (@AUTO_ERRNO)", fd);
	} else if ((size_t ) rv < size) {
		N_Ef(error_2_common_nvmeibt_pwrite, "Partial write @RV_SSIZE_T/@LLX to fd=@FD (@AUTO_ERRNO)", rv, (long long unsigned int)size, fd);
		rv = -1;
	}
	return rv;
}

/*
 * Generic read internal helper.
 * If @offset is negative use read(), otherwise used pread() with @offset.
 */
static int __do_read(int fd, void *ptr, size_t size, off_t offset)
{
	size_t	count = 0;
	int		n;
	int		try_again = MAX_READ_ATTEMPT_NUM;

	while ((size > 0) && try_again) {
		n = pread(fd, (char *)ptr + count, size, offset + count);
    	if (n < 0) {
			if (errno == EINTR) {
				N_Tf(__do_read_trace_1, "Interrupted read from fd=@FD (at @ZX/@ZX off @LLX), retrying",
					 fd, count, size + count, (long long) offset);
			}
			else if (errno == EAGAIN) {
				N_Tf(__do_read_trace_2, "Would-block read from fd=@FD (at @ZX/@ZX off @LLX), stopping",
					fd, count, size + count, (long long) offset);
				break;
			}
			else {
				N_Tf(__do_read_trace_3, "Fail read from fd=@FD (at @ZX/@ZX off @LLX) (@AUTO_ERRNO)",
					 fd, count, size + count, (long long) offset);
				count = -1;
				break;
			}
		}
		else if (n == 0) {
			N_Tf(__do_read_trace_4, "Read EOF from fd=@FD (at @ZX/@ZX off @LLX)",
				fd, count, size + count, (long long) offset);
			break;
		}
    	else {
			size -= n;
			count += n;
		}
		try_again--;
	}

	return count;
}

ssize_t __nvmeibt_pread(int fd, void *vptr, size_t size, off_t offset, BOOL is_exact_size)
{
	ssize_t rv;

	NTOMA_ASSERT(error_common_nvmeibt_pread, offset >= 0, "invalid offset @OFFSET", (long long) offset);

	rv = __do_read(fd, vptr, size, offset);
	if (rv < 0) {
		N_Ef(error_1_common_nvmeibt_pread, "Failed read from fd=@FD (@AUTO_ERRNO)", fd);
	} else if (is_exact_size && ((size_t) rv < size)) {
		N_Ef(error_2_common_nvmeibt_pread, "Partial read @RV_SSIZE_T/@LLX from fd=@FD (@AUTO_ERRNO)", rv, (unsigned long long int)size, fd);
		rv = -1;
	}
	return rv;
}

/*
 * Generic atomic read/write helpers.
 * To be used e.g. with /proc/... files which assume/require whole action.
 */

ssize_t __nvmeibt_pread_atomic(int fd, void *vptr, size_t size, off_t offset,  BOOL is_exact_size)
{
	ssize_t rv;
	int		try_again = MAX_READ_ATTEMPT_NUM + 1;

	NTOMA_ASSERT(yu87tu5, offset >= 0, "invalid offset @OFFSET", (long long) offset);

    do {
		rv = pread(fd, vptr, size, offset);
		if (rv < 0) {
			N_Tf(bki98vt, "Failed atomic pread to fd=@FD (size @SIZEOF off @OFFSET) (@AUTO_ERRNO)",
				fd, size, (long long) offset);
			if (errno != EINTR)
    			break;
		} else if (is_exact_size && ((size_t) rv != size)) {
			N_Ef(floiy97, "Failed atomic pread to fd=@FD (size @SIZEOF off @OFFSET) (partial @RV_SSIZE_T))",
				fd, size, (long long) offset, rv);
			errno = 0;
			rv = -1;
		}
		try_again--;
	} while ((rv < 0) && try_again);
	return rv;
}

static uint64_t global_uid;

uint64_t nvmeibt_get_guid(void)
{
	//uint64_t rv = 1;
	//asm volatile ("	lock xaddq %q0, %1\n" : "+r" (rv), "+m" (global_uid) :: "memory", "cc");
	//return 1 + rv;
	//return ++global_uid;
	return __sync_add_and_fetch(&global_uid, 1);
}

/*
 * nvmeibt_close_all_nonstd_fds(): close all open file descriptors.
 * @is_terminate is set if called by TOMA at termination, and means true
 * shutdown by TOMA, and adds verbosity.
 * @is_terminate is not set if called from helper children before exec()
 * and must not alter the parent's address space state or generate any
 * logging; in particular, may not use FIN/FOUT/_{E|W|T}f().
 */
int nvmeibt_close_all_nonstd_fds(BOOL is_terminate)
{
	struct dirent *dirent;
	DIR *dir;
	char *endp;
	int dir_fd;
	int fd;

	/*
	 * CAUTION! WHEN @is_terminate == false MUST NOT:
	 *   use FIN/FOUT/_Ef/_Wf/_Tf and friends, or
	 *   change any data structure outside our stack,
	 *   or call any function that does either
	 */

	// EXPLICITLY NO FIN

	// Shutdown syslog explicitly, to avoid deleting it's fd and confusing it.
	if (is_terminate)
		closelog();

	dir = opendir("/proc/self/fd");
	if (dir == NULL)
		return -1;

	dir_fd = dirfd(dir);

	while ((dirent = readdir(dir)) != NULL) {
		fd = strtol(dirent->d_name, &endp, 10);
		/*
		 * if the conversion is valid (see man strtol), and the fd is above
		 * stderr, and it isn't our dir_fd - then close it.
		 */
		if ((*endp == '\0') && (dirent->d_name[0] != '\0') && (fd > 2) && (fd != dir_fd)) {
			if (is_terminate) {
				fprintf(stderr, "closing file %d\n", fd);
				//syslog(LOG_WARNING, "closing file %d\n", fd);
				if (1) {		// Print the soft link of fd, to understand what was leaked.
					char fp[PATH_MAX], tp[PATH_MAX];
					int len;
					snprintf(fp, sizeof(fp), "%s/%s", "/proc/self/fd", dirent->d_name);
					len = readlink(fp, tp, sizeof(tp)-1);
					if (len != -1)
						tp[len] = 0;
					syslog(LOG_WARNING, "closing file %d = %s\n", fd, tp);
					if (strstr(tp, ".vscode-server") || strstr(tp, "/dev/ptmx"))	// When running in vscode debugging server or terminal emulator, dont close those fd's.
						continue;
				}
			}
			close(fd);
		}
	}

	closedir(dir);

	// EXPLICITLY NO FOUT

	return 0;
}

int nvmeibt_recursive_mkdir(const char *path, __mode_t mode)
{
	char	copy[PATH_MAX];
	char	*parent;

	if (access(path, X_OK) == 0)
		return 0;

	if (strlen(path) >= PATH_MAX) {
		N_ETf(error_common_nvmeibt_recursive_mkdir, "path too long ('@PATH')", path);
		return -1;
	}

	/* dirname(3) manpage recommends copying the path */
	nvmeibt_strlcpy(copy, path, sizeof(copy));
	parent = dirname(copy);

	if (strcmp(parent, "/") && strcmp(parent, "."))
		nvmeibt_recursive_mkdir(parent, mode);

	if (mkdir(path, mode) != 0 && errno != EEXIST) {
		N_ETf(error_1_common_nvmeibt_recursive_mkdir, "mkdir ('@PATH') failed, @AUTO_ERRNO", path);
		return -1;
	}

	return 0;
}

int nvmeibt_recursive_mkdir_for_path(const char *path, __mode_t mode)
{
	char	copy[PATH_MAX];
	char	*parent;

	if (strlen(path) >= PATH_MAX) {
		N_Ef(error_common_nvmeibt_recursive_mkdir_for_path, "path too long ('@PATH')", path);
		return -1;
	}

	nvmeibt_strlcpy(copy, path, sizeof(copy));
	parent = dirname(copy);

	return nvmeibt_recursive_mkdir(parent, mode);
}

int nvmeibt_log2_int(unsigned int val)
{
	int log2 = 0;

	if (val > 0)
		--val;
	if (val > (1<<16)) {
		val >>= 16;
		log2 += 16;
	}
	if (val > (1<<8)) {
		val >>= 8;
		log2 += 8;
	}
	if (val > (1<<4)) {
		val >>= 4;
		log2 += 4;
	}
	if (val > (1<<2)) {
		val >>= 2;
		log2 += 2;
	}
	if (val > (1<<1)) {
		val >>= 1;
		log2 += 1;
	}
	return log2 + val;
}

/**
 * Converts a char * to char16_t *
 *
 * @author max (7/18/17)
 *
 * @param str
 * @param len
 * @param out
 */
void str_to_char16_str(const char *str, int len, char16_t *out)
{
	int i;
	char* pout = (char*)out;

	memset(pout, 0, len * sizeof(char16_t));

	for (i = 0; i < len; i++) {
		pout[i*2] = (str[i]);
	}
}

void nvmeibt_urn_uuid_to_char16_str(const struct nvmeibt_urn_uuid *urn_uuid, char16_t *out)
{
	unsigned int	i;
	char			*pout = (char*)out;

	memset(pout, 0, sizeof(*urn_uuid) * sizeof(char16_t));

	for (i = 0; i < sizeof(*urn_uuid); i++) {
		pout[i*2] = (urn_uuid->str[i]);
	}
}

void char16_str_to_str(const char16_t *wstr, int len, char *out)
{
	int i;

	memset(out, '\0', len);
	for (i = 0; i < len - 1; i++) {
		out[i] = ((const char *)wstr)[2 * i]; TODO(add support for non english strings as well,  by looking at the other part of the multibyte char, and acting accordingly)
	}
}

void char16_str_to_nvmeibt_urn_uuid(const char16_t *wstr, struct nvmeibt_urn_uuid *urn_uuid)
{
	unsigned int	i;

	memset(urn_uuid, '\0', sizeof(*urn_uuid));
	for (i = 0; i < sizeof(*urn_uuid); i++) {
		urn_uuid->str[i] = ((const char *)wstr)[2 * i]; TODO(add support for non english strings as well,  by looking at the other part of the multibyte char, and acting accordingly)
	}
}

void char16_str_to_union_nvmeib_uuid(const char16_t *wstr, union nvmeib_uuid *out)
{
	struct nvmeibt_urn_uuid		tmp_urn_uuid;
	char16_str_to_nvmeibt_urn_uuid(wstr, &tmp_urn_uuid);
	nvmeibt_urn_uuid_to_union_uuid(out, &tmp_urn_uuid);
}

uint64_t nvmeibt_host_writes_int128_to_uint64(unsigned char *data)
{
	return *(uint64_t *)(data);
}

void uuid_mgmt_format_to_urn_str(const char uuid[16], char *out)
{
	int i;
	char *itr;

	itr = out;
	for (i = 0; i < 4; ++i) {
		itr += sprintf(itr, "%02x", uuid[i] & 0xff);
	}

	*itr++ = '-';
	for (i = 4; i < 10; ++i) {
		itr += sprintf(itr, "%02x", uuid[i] & 0xff);
		if (i & 0x1)
			*itr++ = '-';
	}
	for (i = 10; i < 16; ++i)
		itr += sprintf(itr, "%02x", uuid[i] & 0xff);
}

/*
	Trim the whitespaces from the beginning and the end of the string.
    Input: A proper null-terminated string, in accessible memory. No validation
    Output: A null terminated string, with the leading and trailing spaces removed.
            The output string tarts inside the input string (same memory)
*/
char *trim_whitespace(char *str)
{
	char *end;

	if (!str || *str == 0) {
		return "";
	}
	end = str + strlen(str) - 1;
	/* trim leading space */
	while (isspace(*str))
		str++;
	/* all spaces? */
	if (*str == 0) {
		return str;
	}
	/* trim trailing space */
	while (end > str && isspace(*end)) {
		*end = 0;	// write new null terminator
		end--;
	}
	return str;
}

const char *get_file_type_str(const char *path) {
	struct stat sb;
	if (lstat(path, &sb) == -1)
		return "ERROR";

	switch (sb.st_mode & S_IFMT) {
	case S_IFBLK:  return "block device";
	case S_IFCHR:  return "character device";
	case S_IFDIR:  return "directory";
	case S_IFIFO:  return "FIFO/pipe";
	case S_IFLNK:  return "symlink";
	case S_IFREG:  return "regular file";
	case S_IFSOCK: return "socket";
	default:       return "unknown?";
	}
}

