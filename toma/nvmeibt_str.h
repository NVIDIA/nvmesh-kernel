/**
 * @file   nvmeibt_str.h
 * @Author Oren (orenl@excelero.com)
 * @date   Mar, 2017
 * @brief  The nvmeibt_str buffer helpers
 */

#ifndef NVMEIBT_STR
#define NVMEIBT_STR

#include <malloc.h>
#include "nvmeibt_params.h"
#include <string.h>
#include "nvmeibt_common.h"

/************************  nvmeibt_str ***************/
/*
 * Call NVMEIBT_STR_ALLOC() to allocate.
 * nvmeibt_str_sprintf() is the primary engine.
 * It can start with no previous allocation
 * It expands the allocation automatically, by all means
 * nvmeibt_Str_reuse is useful when writing from scratch, knowing
 * that roughly the same size is needed, so it will not free and later reallocate
 * the text buffer.
 * When done, use NVMEIBT_STR_FREE()
 */

/*
 * struct nvmeibt_str helpers:
 *
 * Rules:
 *
 * o  The buffer is always either empty or contains NULL-terminated string.
 * o  The functions str[n]?cpy() overwrite the buffer content from the start.
 * o  All other modifying functions append their output to current content.
 *
 * APIs:
 *
 * If an error
 * nvmeibt_Str_str():				return pointer to the string in the buffer
 * nvmeibt_Str_strlen():  			return actual length of string in th buffer
 * nvmeibt_Str_reuse():   			clear the buffer and allow reuse
 * nvmeibt_Str_strncat(): 			append a string into the buffer (size-bound)
 * nvmeibt_Str_strcat():  			append a string into the buffer
 * nvmeibt_Str_strncpy(): 			copy a string into the buffer (size-bound)
 * nvmeibt_Str_strcpy():  			copy a string into the buffer
 * nvmeibt_Str_sprintf):  			append a string formatted using sprintf()
 * NVMEIBT_STR_FREAD():    			append contents from file descriptor into buffer
 * NVMEIBT_STR_FWRITE():   			write contents from buffer to file descriptor
 * nvmeibt_Str_fwrite():  			write contents of buffer into file stream
 * nvmeibt_Str_strcmp():  			compare contents of two buffers
 * nvmeibt_Str_clone():   		 	clone one struct onto another
 * NVMEIBT_STR_ALLOC():				allocate an empty nvmeibt_str
 * NVMEIBT_STR_FREE():    			free a struct nvmeibt_str
 * NVMEIBT_STR_RESIZE_BUF():        Resizes the internal buffer.
 */

struct nvmeibt_Str {
	size_t						str_len;
	size_t						allocated_size;
	char						*text_buf;
	struct nvmeibt_Str*			__this_addr; // For internal use only (Private)
};

struct nvmeibt_Buf {
	size_t						buf_len;
	void						*data_buf;
};

struct nvmeibt_KVP {	// Key-Value-Paid. Points into the parsed string
	char	*key;
	size_t	key_len;
	char	*val;
	size_t	val_len;
};

int nvmeibt_Str_strncat(struct nvmeibt_Str *this, const char *str, size_t size);
int nvmeibt_Str_strcat(struct nvmeibt_Str *this, const char *str);
int nvmeibt_Str_sprintf(struct nvmeibt_Str *this, const char *format, ...);
int nvmeibt_Str_fwrite(struct nvmeibt_Str *this, FILE *file);
int nvmeibt_Str_strncpy(struct nvmeibt_Str *this, const char *str, size_t size);
int nvmeibt_Str_strcpy(struct nvmeibt_Str *this, const char *str);
const char *nvmeibt_Str_str(const struct nvmeibt_Str *this);
size_t nvmeibt_Str_strlen(const struct nvmeibt_Str *this);
void nvmeibt_Str_reuse(struct nvmeibt_Str *this);
int nvmeibt_Str_strcmp(const struct nvmeibt_Str *str_ctx1,
					   const struct nvmeibt_Str *str_ctx2);
void nvmeibt_Str_clone(struct nvmeibt_Str *dst, const struct nvmeibt_Str *src);
void nvmeibt_Str_chop_last_char(struct nvmeibt_Str *this);

int	nvmeibt_tokenize_KVP(char *in_str_null_terminated, size_t in_str_len_incl_null, struct nvmeibt_KVP *output_KVP_arr, int n_entries_output_KVP_arr);

typedef struct nvmeibt_str_with_escape_chars {
	char	s[8192];
} nvmeibt_str_with_escape_chars_t;
nvmeibt_str_with_escape_chars_t nvmeibt_escape_special_characters(const char *in);

#define N_VERIFY_NVMEIBT_STR_INITIALIZED(name, this) 						\
({																			\
		NTOMA_ASSERT(name, (this) && (this)->__this_addr == (this),			\
					 "Badly initialized nvmeibt_Str, was initialized at "	\
					 "ptr=@PPP, now at ptr=@PPP",							\
					 (this)->__this_addr, (this));							\
})

#define NVMEIBT_BUF_INIT(this)													\
({																				\
	(this)->data_buf = NULL;													\
	(this)->buf_len = 0;														\
})

#define NNVMEIBT_BUF_FREE(name, this)											\
({																				\
	if ((this)->data_buf) {														\
		NNVMEIBT_BM_FREE(name ## _free,  (this)->data_buf);						\
		(this)->data_buf = NULL;												\
		(this)->buf_len = 0;													\
	}																			\
})

#define NNVMEIBT_BUF_RESIZE(name, this, required_len)							\
({																				\
	if ((this)->buf_len < (required_len)) {									    \
		(this)->data_buf = NNVMEIBT_BM_REALLOC(name ## _realloc, 				\
								(this)->data_buf, required_len);				\
	}																			\
	(this)->buf_len = required_len;												\
})

/* sets the size of the buffer containing the string
   nvmeibt_Str_strlen(this) should be smaller than the new size */
#define NNVMEIBT_STR_RESIZE_BUF(name, this, required_txt_len)					\
({																				\
	size_t __required_txt_len = required_txt_len;								\
	N_VERIFY_NVMEIBT_STR_INITIALIZED(name ## _verify, this);					\
	if ((this)->str_len > (__required_txt_len)) {								\
		N_Wf(name ## _warn, "str_len=@BUFF_SIZE > required_txt_size=@BUFF_SIZE",\
			 (this)->str_len, (__required_txt_len));							\
		__required_txt_len = (this)->str_len;									\
	}																			\
	if ((this)->allocated_size != (__required_txt_len) + 1) {					\
		(this)->text_buf = NNVMEIBT_BM_REALLOC(name ## _realloc, 				\
								(this)->text_buf, (__required_txt_len) + 1);	\
		(this)->allocated_size = (__required_txt_len) + 1;						\
	}																			\
	if (this->allocated_size > 0) {												\
		(this)->text_buf[(this)->str_len] = '\0';								\
	}																			\
})

/* Allocates and initializes struct nvmeibt_Str
   Return Value: new allocated and initialized struct nvmeibt_Str*
   should be matched with a NVMEIBT_STR_FREE()		               */
#define NNVMEIBT_STR_ALLOC(name)											\
({																			\
	struct nvmeibt_Str *NSA__ret =											\
						NNVMEIBT_BM_CALLOC(name,	 						\
						sizeof(struct nvmeibt_Str));						\
	(NSA__ret)->__this_addr = (NSA__ret);									\
	(NSA__ret);																\
})

/* Frees anything allocated with NVMEIBT_STR_ALLOC(_WITH_BUF)?() */
#define NNVMEIBT_STR_FREE(name, to_free)									\
({																			\
	if (to_free) {															\
		(to_free)->str_len = 0xDEADBEEF;									\
		NNVMEIBT_BM_FREE(name ## _1, (to_free)->text_buf);					\
		NNVMEIBT_BM_FREE(name ## _2, to_free);								\
		(to_free) = NULL;													\
	}																		\
})

ssize_t _Str_fread(struct nvmeibt_Str *this, int fd);
ssize_t nvmeibt_str_read_from_pipe_fd(struct nvmeibt_Str *this, int fd, const char* pipe_name);
ssize_t _Str_fread_atomic(struct nvmeibt_Str *this, int fd);
ssize_t _Str_fwrite(struct nvmeibt_Str *this, int fd);
ssize_t _Buf_fwrite(struct nvmeibt_Buf *this, int fd);

#define NNVMEIBT_STR_FREAD NNVMEIBT_STR_FREAD_ATOMIC

#define NNVMEIBT_STR_FREAD_ATOMIC(name, this, fd) ({						\
	ssize_t	__nread__;														\
	__MEASURE_TOOK_INIT();													\
	__nread__ = _Str_fread_atomic((this), (fd));							\
	__MEASURE_TOOK(N_IMf(name ## _measure, "_Str_read_atomic(@FD) Took @LLD ms", (fd), NSEC_TO_MSEC(__measure_took_time_took_nsec))); \
	__nread__;																\
})

#define NNVMEIBT_STR_FWRITE(name, this, fd) ({ 																\
	ssize_t __nwritten__;																				\
	__MEASURE_TOOK_INIT();																				\
	__nwritten__ = _Str_fwrite((this), (fd));															\
	__MEASURE_TOOK(N_IMf(name ## _measure, "nvmeibt_str_write(@FD) Took @LLD ms", (fd), NSEC_TO_MSEC(__measure_took_time_took_nsec))); \
	__nwritten__;																						\
})

#define NNVMEIBT_BUF_FWRITE(name, this, fd) ({															\
	ssize_t __nwritten__;																				\
	__MEASURE_TOOK_INIT();																				\
	__nwritten__ = _Buf_fwrite((this), (fd));															\
	__MEASURE_TOOK(N_IMf(name ## _measure, "nvmeibt_buf_write(@FD) Took @LLD ms", (fd), NSEC_TO_MSEC(__measure_took_time_took_nsec))); \
	__nwritten__;																						\
})

/*************************************************************/
#endif /* NVMEIBT_STR */

