#ifndef NVMEIBT_COMMON
#define NVMEIBT_COMMON

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/time.h>
#include <stdarg.h>
#include <stdint.h>
#include <limits.h>
#include <sys/syscall.h>
#include <time.h>
#include <printf.h>
#include <wchar.h>
#include <fcntl.h>
#include <malloc.h>

#include "utils/nvmeibt_utils.h"
#include "utils/nvmeibt_str.h"

typedef __CHAR16_TYPE__ char16_t;

#define BOOL signed char
#define TODO(x...)
#define PAGE_SHIFT		12
#define PAGE_SIZE		(1UL << PAGE_SHIFT)
#define NUM_4KBLKS_IN_BLKSET 	32
#define DISK_BLKSET_SIZE (NUM_4KBLKS_IN_BLKSET * SECTOR_SIZE)
#define NVMEIBT_MAX_CSV_LINE_LENGTH 512

#define N_PINGS_PER_RAFT_HEARTBEAT 4

#define	NVMEIBT_NOT_INITIALIZED_SER_VER		((unsigned long long)0xFFFFFFFFFFFFFFFF)

#define VERSION_OF_DELETED_VOL 999999000	// A large positive int32

#define RATIO_TO_ZERO_PER_ITERATION 		10000
#define RATIO_TO_TRIM_PER_ITERATION 		100
#define MIN_N_BYTES_TO_ZERO_PER_ITERATION 	(1<<30)		// 1G

enum NVMEIBT_ZEROING_STATE {
	NVMEIBT_ZEROING_STATE_UNINITIALIZED			= 0x0,
	NVMEIBT_ZEROING_STATE_REQUIRED				= (0x1 << 0),
	NVMEIBT_ZEROING_STATE_IN_WORK				= (0x1 << 1),
	NVMEIBT_ZEROING_STATE_DONE					= (0x1 << 2),
	NVMEIBT_ZEROING_STATE_NOT_NEEDED			= (0x1 << 3),
	NVMEIBT_ZEROING_STATE_IN_WORK_CANCELLING	= (0x1 << 4),
};

#define PRAID_VERSION_INVALID_VALUE (0x0)
#define PRAID_VERSION_INITIAL_VALUE (0x100)

#define ILLEGAL_CONFIG_VER			-1
#define DUMMY_OWNER					((int8_t)0xFF)

struct nvmeibt_big_msg {
	int		msg_type;
	int		big_msg_id;
	int 	data_len;
	char	data[0];	// A place holder for unknown size
};

typedef int (*nvmeibt_status_printf_fn_type)(void *ctx, const char *format, ...);

#define MAX_EXEC_WITH_ARGS_STR_LEN				2048

#include <linux/uuid.h>
#include "../common/nvmeib_shared.h"
#include "nvmeibt_debug.h"
#include "common/compat/kr_incs_asserts.h"
#include "common/compat/kr_incs_types.h"
#include "common/nvmeib_math.h"
#include "utils/nvmeibt_uuid.h"
#include "srv/nvmeibs_srv_toma_messages.h"

static inline int nvmeibt_do_ranges_overlap(unsigned long long s0, unsigned long long e0, unsigned long long s1, unsigned long long e1)
{
	return (e1 >= s0 && e0 >= s1);
}

char *get_8_plus_3_char_str_of_now(void);

enum nvmeibt_error_severity {
	ES_FATAL,
	ES_NON_FATAL,
};
void nvmeibt_abort(enum nvmeibt_error_severity es);

union last_LOG_index_union_praid_version{
	struct {
		unsigned int		minor;	// LSB
		unsigned int		major;	// MSB
	} praid_version;
	unsigned long long		ll;
};

#define	CONFIG_TRIM_MGMT		1
#define	CONFIG_TRIM_TOPO		2
#define CONFIG_TRIM_ALL					(CONFIG_TRIM_MGMT | CONFIG_TRIM_TOPO)

static inline bool is_trim_needed(uint8_t *all_flags, uint8_t specific_flag)
{
	*all_flags |= specific_flag;
	return (*all_flags == CONFIG_TRIM_ALL);
}


/**************************** Host Name utils *********************************/
#define ASCII_UUID_MAX_STR_LEN 64
struct nvmeibt_ascii_uuid {				// Used for disks textual uniqe identifier which does not have binary 128-bits equivalent
	char str[ASCII_UUID_MAX_STR_LEN];
};
static inline int is_ascii_uuid_eq(const struct nvmeibt_ascii_uuid *u1, const struct nvmeibt_ascii_uuid *u2)
	{return ((u1 && u2) && !strncmp(u1->str, u2->str, ASCII_UUID_MAX_STR_LEN - 1));}

struct nvmeibt_host_name {
	char host_name[NVMEIB_HOST_NAME_LEN];
};	// Printed via: @HOSTNAME
static inline bool hosts_name_is_eq(const struct nvmeibt_host_name *h1, const struct nvmeibt_host_name *h2)
{
	return ((h1) && (h2) && (strncmp((h1)->host_name, (h2)->host_name, sizeof((h1)->host_name)) == 0));
}

/******************************************************************************/
enum nvmeibt_add_rv {
	NVMEIBT_ADD_UNINITIALIZED = 0x0,
	NVMEIBT_ADD_ALREADY_UP_TO_DATE = 0x1,
	NVMEIBT_ADD_MODIFIED = 0x2,
	NVMEIBT_ADD_NEW = 0x3,
	NVMEIBT_ADD_FAILED = 0x4,
	NVMEIBT_ADD_FAILED_OTHERS_FUNCTIONAL = 0x5,
	NVMEIBT_ADD_SKIPPED = 0x6,
};

#define LOG_FILE stdout
#define ERR_FILE stderr


#define sizeof_member(type, member) (sizeof((type *)0)->member)

/* named code blocks */
#define BLOCK(name)	goto name; name##_skip: if (0) name:
#define BREAK(name) goto name##_skip

int nvmeibt_sscanf_csv_line(const char *line, ...);
void nvmeibt_common_init(void);
const char *nvmeibt_zeroing_state_str(enum NVMEIBT_ZEROING_STATE s);
const char *nvmeibt_get_my_hostname(void);
uint64_t align_pba_e_down_to_blkset(const uint64_t pba_e, const int pblk_size);
uint64_t align_pba_s_up_to_blkset(const uint64_t pba_s, const int pblk_size);
uint64_t nvmeibt_align_n_blks_to_write_to_blkset(uint64_t pba_s, uint64_t n_pblk_to_write, unsigned int pblk_size);

struct nvmeibt_ldisk_id_for_srvr_cmd{	// Unique identifier of disk, for server cmds (format, zero). Todo: nvmeibt_local_disk_util_smart_info should use this struct
	struct nvmeibt_ascii_uuid	ldisk_id;
	char Model[NVMEIB_DISK_MAX_MODEL_STR_SIZE+1];	// +1 for printing with null terminated char
	u32	vendor_id;									// LKJ: should be u16
};

int nvmeibt_zero_disk_pblks(const struct nvmeibt_ldisk_id_for_srvr_cmd *disk,
	uint64_t pba_s, uint64_t n_pblk_to_zero, BOOL are_hw_blks);

struct nvmeibt_initiator_ctx {
	uint64_t	session_gid; /* for srm */
	uint64_t	th_info;
	uint64_t 	msg_ctx;
	uint64_t	blkset_num;
};

int nvmeibt_nonblock_fd(int fd);
int nvmeibt_fd_set_blocking(int fd, int blocking);

#include "utils/nvmeibt_bm.h"
static inline void *nvmeibt_toma_malloc(size_t size)
	{void *p; p = malloc((size)); if (!p) {nvmeibt_abort(ES_FATAL);} return p;}
static inline void *nvmeibt_toma_calloc(size_t nmemb, size_t size)
	{void *p; p = calloc((nmemb), (size)); if (!p) {nvmeibt_abort(ES_FATAL);} return p;}
static inline void *nvmeibt_toma_realloc(void *in_p, size_t size)
	{void *p; p = realloc((in_p), (size)); if (!p) {nvmeibt_abort(ES_FATAL);} return p;}
static inline int nvmeibt_toma_posix_memalign(void **memptr, size_t alignment, size_t size)
	{int rv; rv = posix_memalign((memptr), (alignment), (size)); if (rv) {nvmeibt_abort(ES_FATAL);} return rv;}

void nvmeibt_validate_alloc_free_summary_table(void);

int nvmeibt_get_alloc_free_table_idx(void);
#define ALLOC_FREE_FNAME_LEN 50
#define ALLOC_FREE_TABLE_SIZE 1000
struct nvmeibt_alloc_free_summary_entry {
	char		fname[ALLOC_FREE_FNAME_LEN];
	char		type;
	int			line_no;
	long long	n_calls;
	long long	sum_sizes;
	long long	sum_allocated_size;
};
extern struct nvmeibt_alloc_free_summary_entry nvmeibt_alloc_free_summary_table[ALLOC_FREE_TABLE_SIZE];

enum nvmeibt_toma_alloc_mode {
	NVMEIBT_TOMA_MEM_FREE		= 0x1 << 0,
	NVMEIBT_TOMA_MEM_MALLOC		= 0x1 << 1,
	NVMEIBT_TOMA_MEM_CALLOC		= 0x1 << 2,
	NVMEIBT_TOMA_MEM_REALLOC	= 0x1 << 3,
	NVMEIBT_TOMA_MEM_ALIGN		= 0x1 << 4,
	NVMEIBT_TOMA_REG_RSC		= 0x1 << 5,
	NVMEIBT_TOMA_UREG_RSC		= 0x1 << 6,
	//
	NVMEIBT_TOMA_BM_FREE			= 0x1 << 7,
	NVMEIBT_TOMA_BM_ALLOC			= 0x1 << 8,
	NVMEIBT_TOMA_BM_CALLOC			= 0x1 << 9,
	NVMEIBT_TOMA_BM_ALIGNED_ALLOC	= 0x1 << 10,
	NVMEIBT_TOMA_BM_ALIGNED_CALLOC	= 0x1 << 11,
};

void *nvmeibt_alloc_free_and_account(
				enum nvmeibt_toma_alloc_mode mode,
				int idx, void *ptr, size_t nmemb, long long size);

int nvmeibt_print_alloc_free_summary_table(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx);

#define NVMEIBT_TOMA_ALLOC_COMMON(mode)																							\
	static int nvmeibt_toma_alloc_common_idx;																					\
	if (!nvmeibt_toma_alloc_common_idx) {																						\
		nvmeibt_toma_alloc_common_idx = nvmeibt_get_alloc_free_table_idx();														\
		nvmeibt_strlcpy(nvmeibt_alloc_free_summary_table[nvmeibt_toma_alloc_common_idx].fname, __FILE__, ALLOC_FREE_FNAME_LEN);	\
		nvmeibt_alloc_free_summary_table[nvmeibt_toma_alloc_common_idx].line_no = __LINE__;										\
		nvmeibt_alloc_free_summary_table[nvmeibt_toma_alloc_common_idx].type = (mode);											\
	}

#define NNVMEIBT_TOMA_FREE(name, ppp)																						\
	({																														\
		if (ppp) {																											\
			NVMEIBT_TOMA_ALLOC_COMMON('F');																					\
			if (strcmp(__FILE__, "interfaces/log/nvmeibt_logger.c") != 0) {													\
				N_Tf(name, "FREE p=@PPP size=@SIZEOF", ppp, malloc_usable_size(ppp));										\
			}																												\
			nvmeibt_alloc_free_and_account(NVMEIBT_TOMA_MEM_FREE, nvmeibt_toma_alloc_common_idx, (ppp), 0, 0); 				\
			ppp = NULL;																										\
		}																													\
	})

#define NNVMEIBT_TOMA_MALLOC(name, size) 																						\
	({																															\
		void	*__p;																											\
		NVMEIBT_TOMA_ALLOC_COMMON('A');																							\
		__p = nvmeibt_alloc_free_and_account(NVMEIBT_TOMA_MEM_MALLOC, nvmeibt_toma_alloc_common_idx, NULL, 0, (size));			\
		if (strcmp(__FILE__, "interfaces/log/nvmeibt_logger.c")) {																\
			N_Tf(name, "MALLOC p=@PPP size=@SIZEOF", __p, malloc_usable_size(__p));												\
		}																														\
		__p;																													\
	})

#define NNVMEIBT_TOMA_CALLOC(name, nmemb, size) 																					\
	({																																\
		void	*__p;																												\
		NVMEIBT_TOMA_ALLOC_COMMON('C');																								\
		__p = nvmeibt_alloc_free_and_account(NVMEIBT_TOMA_MEM_CALLOC, nvmeibt_toma_alloc_common_idx, NULL, (nmemb), (size));		\
		if (strcmp(__FILE__, "interfaces/log/nvmeibt_logger.c")) {																	\
			N_Tf(name, "CALLOC p=@PPP size=@SIZEOF", __p, malloc_usable_size(__p));													\
		}																															\
		__p;																														\
	})

#define NNVMEIBT_TOMA_REALLOC(name, in_ptr, size) 																						\
	({																																	\
		void	*__p;																													\
		size_t __prev_size = malloc_usable_size(in_ptr);																				\
		NVMEIBT_TOMA_ALLOC_COMMON('R');																									\
		__p = nvmeibt_alloc_free_and_account(NVMEIBT_TOMA_MEM_REALLOC, nvmeibt_toma_alloc_common_idx, (in_ptr), 0, (size));				\
		if (strcmp(__FILE__, "interfaces/log/nvmeibt_logger.c")) {																		\
			N_Tf(name, "REALLOC prev=@PPP p=@PPP prev_size=@SIZEOF size=@SIZEOF", (in_ptr), __p, __prev_size, malloc_usable_size(__p));	\
		}																																\
		__p;																															\
	})

#define NNVMEIBT_TOMA_POSIX_MEMALIGN(name, pptr, alignment, size)																	\
	({																																\
		void *__p; 																													\
		int toma_posix_memalign_rv; 																								\
		NVMEIBT_TOMA_ALLOC_COMMON('P');																								\
		__p = nvmeibt_alloc_free_and_account(NVMEIBT_TOMA_MEM_ALIGN, nvmeibt_toma_alloc_common_idx, (pptr), (alignment), (size)); 	\
		if (strcmp(__FILE__, "interfaces/log/nvmeibt_logger.c")) {																	\
			N_Tf(name, "MEM_ALIGN ptr=@PPP p=@PPP size=@SIZEOF", *((void **)(pptr)), __p, malloc_usable_size(__p));					\
		}																															\
		toma_posix_memalign_rv = (__p != NULL ? 0 : -1);																			\
		toma_posix_memalign_rv;																										\
	})

#define NNVMEIBT_TOMA_STRDUP(name, _s_)				\
	({												\
		char *__s;									\
		size_t len = strlen(_s_) + 1;				\
		__s = NNVMEIBT_TOMA_MALLOC(name, len);		\
		nvmeibt_strlcpy(__s, _s_, len);				\
		__s;										\
	})

#define NNVMEIBT_TOMA_REGISTER_RSC(name, ptr, size) \
	({ \
		void *__p; \
		NVMEIBT_TOMA_ALLOC_COMMON('N'); \
		__p = nvmeibt_alloc_free_and_account(NVMEIBT_TOMA_REG_RSC, nvmeibt_toma_alloc_common_idx, (ptr), (0), (size)); \
		if (strcmp(__FILE__, "interfaces/log/nvmeibt_logger.c")) { \
			N_Tf(name, "REG_RSC p=@PPP size=@SIZEOF", __p, size); \
		} \
		__p; \
	 })

#define NNVMEIBT_TOMA_UNREGISTER_RSC(name, ppp, size) \
	({ \
		if (ppp) { \
			NVMEIBT_TOMA_ALLOC_COMMON('N'); \
			if (strcmp(__FILE__, "interfaces/log/nvmeibt_logger.c")) { \
				N_Tf(name, "UREG_RSC p=@PPP size=@SIZEOF", ppp, size); \
			} \
			nvmeibt_alloc_free_and_account(NVMEIBT_TOMA_UREG_RSC, nvmeibt_toma_alloc_common_idx, (ppp), 0, size); \
		} \
	 })

#define NNVMEIBT_BM_ALLOC(name, size) 																				\
	({																												\
		void	*_myP_;																								\
		NVMEIBT_TOMA_ALLOC_COMMON('a');																				\
		_myP_ = nvmeibt_bm_allocate_buffer(size);																	\
		N_Tf(name, "BM_ALLOC p=@PPP size=@SIZE", _myP_, nvmeibt_bm_get_buf_alloc_size(_myP_));						\
		nvmeibt_alloc_free_and_account(NVMEIBT_TOMA_BM_ALLOC, nvmeibt_toma_alloc_common_idx, _myP_, 0, (size));		\
		_myP_;																										\
	})

#define NNVMEIBT_BM_CALLOC(name, size) 																				\
	({																												\
		void	*_myP_;																								\
		NVMEIBT_TOMA_ALLOC_COMMON('c');																				\
		_myP_ = nvmeibt_bm_calloc_buffer(size);																		\
		N_Tf(name, "BM_CALLOC p=@PPP size=@SIZE", _myP_, nvmeibt_bm_get_buf_alloc_size(_myP_));						\
		nvmeibt_alloc_free_and_account(NVMEIBT_TOMA_BM_CALLOC, nvmeibt_toma_alloc_common_idx, _myP_, 0, (size));	\
		_myP_;																										\
	})

#define NNVMEIBT_BM_REALLOC(name, in_ptr, size) 																						\
	({																																	\
		void	*__p;																													\
		NVMEIBT_TOMA_ALLOC_COMMON('r');																									\
		__p = nvmeibt_bm_allocate_buffer(size);																							\
		N_Tf(name ## _A, "BM_ALLOC p=@PPP size=@SIZE", __p, nvmeibt_bm_get_buf_alloc_size(__p));										\
		nvmeibt_alloc_free_and_account(NVMEIBT_TOMA_BM_ALLOC, nvmeibt_toma_alloc_common_idx, (__p), 0, (size));							\
		if (in_ptr) { 																													\
			size_t prev_size = nvmeibt_bm_get_buf_alloc_size(in_ptr);																	\
			size_t copy_size = (prev_size < size) ? prev_size : size;																	\
			memcpy(__p, in_ptr, copy_size);																								\
			N_Tf(name ## _F, "BM_FREE p=@PPP size=@SIZE", in_ptr, (int)prev_size);														\
			nvmeibt_alloc_free_and_account(NVMEIBT_TOMA_BM_FREE, nvmeibt_toma_alloc_common_idx, (in_ptr), 0, (prev_size));				\
			nvmeibt_bm_free_buffer(in_ptr);																								\
		}																																\
		__p;																															\
	})

#define NNVMEIBT_BM_ALIGNED_ALLOC(name, _alignment_, _size_) 																	\
	({																															\
		void	*_myP_;																											\
		NVMEIBT_TOMA_ALLOC_COMMON('p');																							\
		_myP_ = nvmeibt_bm_allocate_dma_buffer((_alignment_), (_size_));														\
		N_Tf(name, "BM_ALIGNED_ALLOC p=@PPP size=@SIZE", _myP_, nvmeibt_bm_get_buf_alloc_size(_myP_));							\
		nvmeibt_alloc_free_and_account(NVMEIBT_TOMA_BM_ALIGNED_ALLOC, nvmeibt_toma_alloc_common_idx, _myP_, 0, (_size_));		\
		_myP_;																													\
	})
#define NNVMEIBT_BM_ALIGNED_CALLOC(name, _alignment_, _size_) 																\
	({																															\
		void	*_myP_;																											\
		NVMEIBT_TOMA_ALLOC_COMMON('m');																							\
		_myP_ = nvmeibt_bm_allocate_dma_buffer((_alignment_), (_size_));														\
		memset(_myP_, 0, (_size_));																								\
		N_Tf(name, "BM_ALIGNED_CALLOC p=@PPP size=@SIZE", _myP_, nvmeibt_bm_get_buf_alloc_size(_myP_));						\
		nvmeibt_alloc_free_and_account(NVMEIBT_TOMA_BM_ALIGNED_CALLOC, nvmeibt_toma_alloc_common_idx, _myP_, 0, (_size_));		\
		_myP_;																													\
	})

#define NNVMEIBT_BM_FREE(name, _myP_)																				\
	({																												\
		if (_myP_) {																								\
			NVMEIBT_TOMA_ALLOC_COMMON('f');																			\
			N_Tf(name, "BM_FREE p=@PPP size=@SIZE", _myP_, nvmeibt_bm_get_buf_alloc_size(_myP_));					\
			nvmeibt_alloc_free_and_account(NVMEIBT_TOMA_BM_FREE, nvmeibt_toma_alloc_common_idx, _myP_, 0, 0);		\
			nvmeibt_bm_free_buffer(_myP_);																			\
			_myP_ = NULL;																							\
		}																											\
	})

#define NNVMEIBT_ALLOC_ELEM(name, _x_, _y_)						\
	({															\
		XDLIST_TYPE(_x_) _v_;									\
		if (!(_v_ = XDLIST_FIRST(_x_))) {						\
			_v_ = NNVMEIBT_TOMA_CALLOC(name, 1, sizeof(*_v_));	\
		} else {												\
			XDLIST_DEL(&_v_->link);								\
			memset(_v_, 0, sizeof(*_v_));						\
		}														\
		_v_;													\
	})

#define NNVMEIBT_FREE_LIST(name, _x_)		\
	do {									\
		XDLIST_TYPE(_x_) _v_;				\
		XDLIST_FOREACH_SAFE(_v_, _x_) {		\
			XDLIST_DEL(&(_v_)->link);		\
			NNVMEIBT_TOMA_FREE(name, _v_);	\
		}									\
	} while (0)

#define NNVMEIBT_BM_FREE_LIST(name, _x_)	\
	do {									\
		XDLIST_TYPE(_x_) _v_;				\
		XDLIST_FOREACH_SAFE(_v_, _x_) {		\
			XDLIST_DEL(&(_v_)->link);		\
			NNVMEIBT_BM_FREE(name, _v_);	\
		}									\
	} while (0)

#define true 1
#define false 0
#ifndef bool
#define bool int
#endif

#define NVMEIBT_FCNTL_CLOEXEC(fd) do {						\
	int __nvmeibt_fd_flags = fcntl(fd, F_GETFD, 0);			\
	errno = 0;												\
	fcntl(fd, F_SETFD, __nvmeibt_fd_flags | FD_CLOEXEC);	\
} while (0)

#define NNVMEIBT_OPEN_READ(name, __path, __is_mandatory) ({ 						\
	int		__fd__;																	\
	__MEASURE_TOOK_INIT();															\
	errno = 0;																		\
	__fd__ = open((__path), O_RDONLY | O_CLOEXEC);									\
	if (__fd__ < 0) {																\
		if (__is_mandatory) {														\
			N_Wf(name ## 1, "OPEN_READ path=@PATH file_type='@STR' @AUTO_ERRNO", (__path), get_file_type_str(__path));		\
		} else {																	\
			N_Tf(name ## 2, "OPEN_READ path=@PATH file_type='@STR' @AUTO_ERRNO", (__path), get_file_type_str(__path));		\
		}																			\
	} else {																		\
		N_Tf(name ## 3, "open path=@PATH fd=@FD", (__path), __fd__);				\
	}																				\
	__MEASURE_TOOK(N_IMf(name ## _measure, "open(@PATH) Took @LLD ms", (__path), NSEC_TO_MSEC(__measure_took_time_took_nsec)));		\
	__fd__;																			\
})

#define NNVMEIBT_OPEN_READ_EXCL(name, __path) ({ 									\
	int		__fd__;																	\
	__MEASURE_TOOK_INIT();															\
	errno = 0;																		\
	__fd__ = open((__path), O_RDONLY | O_CLOEXEC | O_EXCL);							\
	if (__fd__ < 0) {																\
		N_Wf(name ## 2, "OPEN_READ_EXCL path=@PATH file_type='@STR' @AUTO_ERRNO", (__path), get_file_type_str(__path));		\
	} else {																		\
		N_Tf(name, "open path=@PATH fd=@FD", (__path), __fd__);						\
	}																				\
	__MEASURE_TOOK(N_IMf(name ## _measure, "open(@PATH) Took @LLD ms", (__path), NSEC_TO_MSEC(__measure_took_time_took_nsec)));		\
	__fd__;																			\
})

#define NNVMEIBT_OPEN_LOCAL_DISK_WRITE(name, __path) ({								\
	int		__fd__;																	\
	__MEASURE_TOOK_INIT();															\
	errno = 0;																		\
	__fd__ = open((__path), O_RDWR | O_DIRECT | O_SYNC | O_CLOEXEC);				\
	if (__fd__ < 0) {																\
		N_Wf(name ## 3, "OPEN_LOCAL_DISK_WRITE path=@PATH file_type='@STR' @AUTO_ERRNO", (__path), get_file_type_str(__path));		\
	} else {																		\
		N_Tf(name, "open path=@PATH fd=@FD", (__path), __fd__);						\
	}																				\
	__MEASURE_TOOK(N_IMf(name ## _measure, "open(@PATH) Took @LLD ms", (__path), NSEC_TO_MSEC(__measure_took_time_took_nsec)));		\
	__fd__;																			\
})

#define NNVMEIBT_FSYNC(name, __fd) ({												\
	int		__rv__;																	\
	__MEASURE_TOOK_INIT();															\
	errno = 0;																		\
	__rv__ = fsync(__fd);															\
	if (__rv__ < 0) {																\
		N_Wf(name ## 4, "FSYNC fd=@FD @AUTO_ERRNO", (__fd));						\
	}																				\
	__MEASURE_TOOK(N_IMf(name, "fsync(@FD) Took @LLD ms", (__fd), NSEC_TO_MSEC(__measure_took_time_took_nsec)));	\
	__rv__;																			\
})

#define NNVMEIBT_RENAME(name, __old, __new) ({										\
	int		__rv__;																	\
	__MEASURE_TOOK_INIT();															\
	errno = 0;																		\
	__rv__ = rename((__old), (__new));												\
	if (__rv__ < 0) {																\
		N_Wf(name ## 5, "RENAME @STR-->@STR @AUTO_ERRNO", (__old), (__new));		\
	}																				\
	__MEASURE_TOOK(N_IMf(name, "rename(@STR-->@STR) Took @LLD ms", (__old), (__new), NSEC_TO_MSEC(__measure_took_time_took_nsec)));		\
	__rv__;																			\
})

#define NNVMEIBT_LINK(name, __existing, __to_create) ({								\
	int		__rv__;																	\
	__MEASURE_TOOK_INIT();															\
	errno = 0;																		\
	__rv__ = link((__existing), (__to_create));										\
	if (__rv__ < 0) {																\
		N_Wf(name ## 6, "LINK @STR-->@STR @AUTO_ERRNO", (__to_create), (__existing));	\
	}																				\
	__MEASURE_TOOK(N_IMf(name ## _measure, "link(@PATH) Took @LLD ms", (__to_create), NSEC_TO_MSEC(__measure_took_time_took_nsec)));		\
	__rv__;																			\
})

#define NNVMEIBT_UNLINK(name, __path, is_OK_to_fail) ({								\
	int		__rv__;																	\
	__MEASURE_TOOK_INIT();															\
	errno = 0;																		\
	__rv__ = unlink(__path);														\
	if (__rv__ < 0 && !(is_OK_to_fail)) {											\
		N_Wf(name ## 7, "UNLINK path=@STR @AUTO_ERRNO", (__path));					\
	}																				\
	__MEASURE_TOOK(N_IMf(name, "unlink(@PATH) Took @LLD ms", (__path), NSEC_TO_MSEC(__measure_took_time_took_nsec)));	\
	__rv__;																			\
})

#define NNVMEIBT_OPEN(name, __path, flags_and_perm...) ({								\
	int __fd__;																			\
	__MEASURE_TOOK_INIT();																\
	errno = 0;																			\
	__fd__ = open((__path), O_CLOEXEC | flags_and_perm);								\
	if (__fd__ >= 0) {																	\
		N_Tf(name ## OK, "open path=@PATH fd=@FD", (__path), __fd__);					\
	} else {																			\
		N_Wf(name ## Err, "OPEN path=@PATH file_type='@STR' @AUTO_ERRNO", (__path), get_file_type_str(__path));		\
	}																					\
	__MEASURE_TOOK(N_IMf(name ## _measure, "open(@PATH) Took @LLD ms", (__path), NSEC_TO_MSEC(__measure_took_time_took_nsec)));		\
	__fd__;																				\
	})
#define NNVMEIBT_OPENAT(name, dirfd, __path, flags_and_perm...) ({				\
	int __fd__;																	\
	errno = 0;																	\
	__fd__ = openat(dirfd, (__path), O_CLOEXEC | flags_and_perm);				\
	if (__fd__ < 0) {															\
		N_Wf(name ## 8, "OPENAT path=@PATH file_type='@STR' @AUTO_ERRNO", (__path), get_file_type_str(__path));		\
	} else {																	\
		N_Tf(name, "open path=@PATH fd=@FD", (__path), __fd__);					\
	}																			\
	__fd__;																		\
	})
#define NNVMEIBT_EVENTFD(name, initval, flags) ({								\
	errno = 0;																	\
	int nvmeibt_eventfd_fd = eventfd((initval), (O_CLOEXEC | flags));			\
	N_Tf(name, "eventfd nvmeibt_fd=%d", nvmeibt_eventfd_fd);					\
	nvmeibt_eventfd_fd;															\
	})
#define NNVMEIBT_SOCKET(name, domain, type, protocol) ({						\
	int nvmeibt_socket_fd;														\
	errno = 0;																	\
	nvmeibt_socket_fd = socket((domain), (SOCK_CLOEXEC | type), (protocol));	\
	if (nvmeibt_socket_fd < 0) {												\
		N_Wf(name ## 9, "SOCKET domain=@INT @AUTO_ERRNO", (domain));			\
	} else {																	\
		N_Tf(name, "socket nvmeibt_fd=@FD", nvmeibt_socket_fd);					\
	}																			\
	nvmeibt_socket_fd;															\
	})
#define NNVMEIBT_EPOLL_CREATE1(name) ({ 																			\
	int		__fd__;																									\
	__MEASURE_TOOK_INIT();																							\
	errno = 0;																										\
	__fd__ = epoll_create1(EPOLL_CLOEXEC);																			\
	if (__fd__ < 0) {																								\
		N_Ef(name ## err, "Failed epoll_create1() @AUTO_ERRNO");													\
		nvmeibt_abort(ES_FATAL);																					\
	}																												\
	N_Tf(name, "epoll_create1() fd=@FD", __fd__);																	\
	__MEASURE_TOOK(N_IMf(name ## _measure, "epoll_create1() Took @LLD ms", NSEC_TO_MSEC(__measure_took_time_took_nsec)));			\
	__fd__;																											\
})
#define NNVMEIBT_EPOLL_CTL_READ_ADD(name, __epoll_fd_, __added_fd_, __fds_in_use_ptr__) ({ 	\
	struct epoll_event	__event;																				\
	int					__epoll_fd = (__epoll_fd_);																\
	int					__added_fd = (__added_fd_);																\
	errno = 0;																									\
	__event.events = (EPOLLIN | EPOLLPRI | EPOLLHUP | EPOLLERR);												\
	__event.data.ptr = __fds_in_use_ptr__;                             											\
	if (epoll_ctl(__epoll_fd, EPOLL_CTL_ADD, __added_fd, &__event) < 0) {										\
		N_Ef(name ## err, "Failed epoll_ctl(@INT, ADD, @INT) @AUTO_ERRNO", __epoll_fd, __added_fd);				\
		nvmeibt_abort(ES_FATAL);																				\
	}																											\
})

#define NNVMEIBT_CLOSE(name, _nvmeibt_close_fd) ({			\
	int	_fd_ = (_nvmeibt_close_fd);							\
	int _close_ret_ = 0;									\
	__MEASURE_TOOK_INIT();									\
	N_Tf(name, "close nvmeibt_fd=@FD", _fd_);				\
	if (_fd_ >= 0) {										\
		errno = 0;											\
		_close_ret_ = close(_fd_);							\
	}														\
	if (_close_ret_ < 0) {									\
		N_Ef(name ## _error, "Failed @INT=close(fd=@INT) @AUTO_ERRNO", _close_ret_, _fd_);     \
	}														\
	__MEASURE_TOOK(N_IMf(name ## _measure, "close(@INT) Took @LLD ms", _fd_, NSEC_TO_MSEC(__measure_took_time_took_nsec)));		\
	_nvmeibt_close_fd = -1;									\
	_close_ret_;											\
	})

#define NNVMEIBT_PIPE(name, __pipefd_array) ({											\
	int		__rv;																		\
	__MEASURE_TOOK_INIT();																\
	errno = 0;																			\
	__rv = pipe(__pipefd_array);														\
	if (__rv < 0) {																		\
		N_Wf(name ## 10, "PIPE @AUTO_ERRNO");											\
	} else {																			\
		N_Tf(name, "pipe(read_fd=@FD, write_fd=@FD)",									\
			 (__pipefd_array)[0], (__pipefd_array)[1]);									\
	}																					\
	__MEASURE_TOOK(N_IMf(name ## _measure, "pipe() Took @LLD ms", NSEC_TO_MSEC(__measure_took_time_took_nsec)));			\
	__rv;																				\
	})

/*
 * For use only on FILE* created from a file-decriptor using fdopen(),
 * to provide symmetry in logging.
 */
#define NNVMEIBT_FCLOSE(name, close_file) ({						\
	int _close_ret_ = 0;											\
	int _close_fd_ = fileno(close_file);							\
	__MEASURE_TOOK_INIT();											\
	N_Tf(name, "close nvmeibt_fd=@FD", _close_fd_);					\
	errno = 0;														\
	_close_ret_ = fclose(close_file);								\
	if (_close_ret_ < 0) {											\
		N_Ef(name ## _error, "Failed close(fd=@FD) err=@AUTO_ERRNO", _close_fd_);	\
	}																\
	__MEASURE_TOOK(N_IMf(name ## _measure, "close(@FD) Took @LLD ms", (_close_fd_), NSEC_TO_MSEC(__measure_took_time_took_nsec)));		\
	close_file = NULL;												\
	_close_ret_;													\
	})

#define MAX_REMOTE_GIDS 128

typedef enum {
	NCS_CONN_UP = 1,
	NCS_CONN_DOWN = 2,
} NVMEIBT_CONN_STATE_T;

uint64_t nvmeibt_get_guid(void);

struct date_time {
	uint16_t year;
	uint16_t month;
	uint16_t day;
	uint16_t hour;
	uint16_t minute;
	uint16_t second;
	uint16_t msecs;
};

void nvmeibt_get_current_date_n_time(struct date_time *dt);
int nvmeibt_date_time(
	FILE *stream, const struct printf_info *info, const void *const *args);
int nvmeibt_date_time_arginfo(
	const struct printf_info *info, size_t n, int *argtypes, int *size);

int nvmeibt_vasprintf(char **str, const char *fmt, va_list args);
int nvmeibt_asprintf(char **str, const char *fmt, ...);

/*
 * helpers to read/write from file descriptors exact payload size:
 * Return total number of bytes transferred (must equal the requested payload
 * size, except for the non-blocking API). Failures other than EINTR/EAGAIN
 * are not tolerated.
 *
 * nvmeibt_write(fd, buf, size)
 *   - write/read @size bytes at current file position (and update file position)
 *   - return total number of bytes written/read (@size) on success or -1 on error
 *   - partial write/read (bytes written/read < @size) considered error
 *
 * __nvmeibt_pwrite(fd, buf, size, offset)
 * nvmeibt_pread(fd, buf, size, offset)
 *   - write/read @size bytes at @offset (file position not updated)
 *   - return total number of bytes written/read (@size) on success or -1 on error
 *   - partial write/read (bytes written/read < @size) considered error
 */

ssize_t nvmeibt_write(int fd, const void *buf, size_t n);
ssize_t __nvmeibt_pwrite(int fd, const void *buf, size_t n, off_t offset);

ssize_t __nvmeibt_pread(int fd, void *vptr, size_t size, off_t offset, BOOL is_exact_size);
ssize_t __nvmeibt_pread_atomic(int fd, void *buf, size_t n, off_t offset,  BOOL is_exact_size);

#define NNVMEIBT_PWRITE(name, __fd, __buf, __n, __offset, __min_offset) ({				\
	ssize_t		__rv__;																	\
	__MEASURE_TOOK_INIT();																\
	if ((uint64_t)__offset < (uint64_t)__min_offset) {												\
		N_Ef(name ## _err, "offset=@OFFSET_INT min_offset=@OFFSET_INT", __offset, __min_offset);	\
		nvmeibt_abort(ES_FATAL);																	\
	}																								\
	__rv__ = __nvmeibt_pwrite((__fd), (__buf), (__n), (__offset));						\
	__MEASURE_TOOK(N_IMf(name ## _measure, "pwrite(@FD) Took @LLD ms", (__fd), NSEC_TO_MSEC(__measure_took_time_took_nsec)));		\
	__rv__;																				\
})

#define NNVMEIBT_PREAD NNVMEIBT_PREAD_ATOMIC

#define NNVMEIBT_PREAD_ATOMIC(name, __fd, __buf, __n, __offset, __is_exact_size) ({       \
	ssize_t     __rv__;                                                                   \
	__MEASURE_TOOK_INIT();                                                                \
	__rv__ = __nvmeibt_pread_atomic((__fd), (__buf), (__n), (__offset), __is_exact_size); \
	__MEASURE_TOOK(N_IMf(name ## _measure, "pread(@FD) Took @LLD ms", (__fd), NSEC_TO_MSEC(__measure_took_time_took_nsec)));     \
	__rv__;                                                                               \
})

int nvmeibt_close_all_nonstd_fds(BOOL is_verbose);

int nvmeibt_recursive_mkdir(const char *path, __mode_t mode);
int nvmeibt_recursive_mkdir_for_path(const char *path, __mode_t mode);
int nvmeibt_log2_int(unsigned int val);

void str_to_char16_str(const char *str, int len, char16_t *out);
void nvmeibt_urn_uuid_to_char16_str(const struct nvmeibt_urn_uuid *urn_uuid, char16_t *out);
void char16_str_to_str(const char16_t *wstr, int len, char *out);
void char16_str_to_nvmeibt_urn_uuid(const char16_t *wstr, struct nvmeibt_urn_uuid *urn_uuid);
void char16_str_to_union_nvmeib_uuid(const char16_t *wstr, union nvmeib_uuid *out);

uint64_t nvmeibt_host_writes_int128_to_uint64(unsigned char *data);
char* trim_whitespace(char *str);

#define ZEROINIT(x) do {memset(&x, 0, sizeof(x));} while (0)

#define ALIGNED_1MB (1024ll * 1024)

static inline BOOL is_128KB_aligned(uint64_t pba, int pblk_size)
{
        return ((pba * pblk_size) % (128ll * 1024)) == 0;
}

#include "../common/compat/kr_incs_time_rdtsc.h"

#define nvmeibt_topology_binary_topo_header			"BIN_TOPO"
#define nvmeibt_topology_binary_active_topo_header	"ACT_TOPO"
#define NVMEIBT_TOPOLOGY_BIN_NAME_LEN		(sizeof(nvmeibt_topology_binary_topo_header) - 1)
// Ronen - my compiler unfortunately does not support the following (yet)
//#if sizeof(nvmeibt_topology_binary_active_topo_header) != NVMEIBT_TOPOLOGY_BIN_NAME_LEN
//#	error NVMEIBT_TOPOLOGY_BIN_NAME_LEN mismatch
//#endif

typedef struct {
	volatile int counter;
} atomic_t;

#define atomic_read(v) ((v)->counter)

static inline int atomic_add(int i, atomic_t *v)
{
	return __sync_add_and_fetch(&v->counter, i);
}

static inline void atomic_set(atomic_t *v, int i)
{
	v->counter = i;
}

const char *get_file_type_str(const char *path);

#define IS_PERCENTAGE_EXCEEDED(x_part, x_100_percent, x_percent) \
	(((x_part) * 100) > ((x_100_percent) * (x_percent)))

#endif // #ifndef NVMEIBT_COMMON

