#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "nvmeibt_debug.h"
#include "nvmeibt_common.h"

static inline size_t Str_get_allocated_size(const struct nvmeibt_Str *this)
{
	return this->allocated_size;
}

#define N_VERIFY_SIZE(name, this) NTOMA_ASSERT(name, Str_get_allocated_size(this) < (100 << 20), "nvmeibt_str->allocated_size=@SIZEOF", Str_get_allocated_size(this));

void nvmeibt_Str_clone(struct nvmeibt_Str *dst, const struct nvmeibt_Str *src)
{
	N_VERIFY_NVMEIBT_STR_INITIALIZED(error_str_nvmeibt_Str_clone, dst);
	if (dst != src) { nvmeibt_Str_strcpy(dst, nvmeibt_Str_str(src)); }
	N_VERIFY_SIZE(error_1_str_nvmeibt_Str_clone, dst);
}

void nvmeibt_Str_chop_last_char(struct nvmeibt_Str *this)
{
	N_VERIFY_NVMEIBT_STR_INITIALIZED(error_str_nvmeibt_Str_chop_last_char, this);
	if (this->str_len >= 1) {
		--(this->str_len);
		this->text_buf[this->str_len] = '\0';
	}
}

int nvmeibt_Str_strcmp(const struct nvmeibt_Str *str_ctx1, const struct nvmeibt_Str *str_ctx2)
{
	return strcmp(nvmeibt_Str_str(str_ctx1), nvmeibt_Str_str(str_ctx2));
}

void nvmeibt_Str_reuse(struct nvmeibt_Str *this)
{
	if (nvmeibt_Str_strlen(this)) {
		this->str_len = 0;
		*(this->text_buf) = '\0';
	}
}

size_t nvmeibt_Str_strlen(const struct nvmeibt_Str *this)
{
	N_VERIFY_NVMEIBT_STR_INITIALIZED(error_str_nvmeibt_Str_strlen, this);
	return this->str_len;
}

const char *nvmeibt_Str_str(const struct nvmeibt_Str *this)
{
	N_VERIFY_NVMEIBT_STR_INITIALIZED(error_str_nvmeibt_Str_str, this);
	return this->text_buf ? this->text_buf : "\0";
}

static inline char *_nvmeibt_Str_str(struct nvmeibt_Str *this)
{
	return this->text_buf;
}

/* NOTE: appends the output to the existing content in the buffer */
int nvmeibt_Str_sprintf(struct nvmeibt_Str *this, const char *format, ...)
{
	va_list arglist;
	size_t	size_needed;
	bool	is_enough_allocated;
	N_VERIFY_NVMEIBT_STR_INITIALIZED(error_str_nvmeibt_Str_sprintf, this);
	do {
		const size_t allocated_size = Str_get_allocated_size(this);
		const size_t len = nvmeibt_Str_strlen(this);
		va_start(arglist, format);
		size_needed = vsnprintf(_nvmeibt_Str_str(this) + len, allocated_size - len, format, arglist) + 1;
		va_end(arglist);

		is_enough_allocated = (len + size_needed <= allocated_size);
		if (!is_enough_allocated) {
			const size_t new_size = (len + size_needed + 1) * 110 / 100;	// 10% larger
			this->text_buf = NNVMEIBT_BM_REALLOC(trace_str_nvmeibt_Str_sprintf, this->text_buf, new_size);
			this->allocated_size = new_size;
		}
	} while (!is_enough_allocated);

	this->str_len += size_needed - 1;
	N_VERIFY_SIZE(error_1_str_nvmeibt_Str_sprintf, this);
	return size_needed - 1;
}

/* NOTE: appends the output to the existing content in the buffer */
int nvmeibt_Str_strncat(struct nvmeibt_Str *this, const char *str, size_t size)
{
	size_t size_needed, len, allocated_size;
	BOOL   is_enough_allocated;
	size_t new_size;
	if (!str) return 0;
	N_VERIFY_NVMEIBT_STR_INITIALIZED(error_str_nvmeibt_Str_strncat, this);
	allocated_size = Str_get_allocated_size(this);
	len = nvmeibt_Str_strlen(this);
	size_needed = strnlen(str, size) + 1;

	is_enough_allocated = (len + size_needed) <= allocated_size;
	if (!is_enough_allocated) {
		new_size = (len + size_needed + 1) * 110 / 100;	   // 10% larger
		this->text_buf = NNVMEIBT_BM_REALLOC(trace_str_nvmeibt_Str_strncat, this->text_buf, new_size);
		this->allocated_size = new_size;
	}

	nvmeibt_strlcpy(_nvmeibt_Str_str(this) + len, str, size_needed);
	this->str_len += size_needed - 1;
	N_VERIFY_SIZE(error_1_str_nvmeibt_Str_strncat, this);
	return 0;
}

/* NOTE: appends the output to the existing content in the buffer */
int nvmeibt_Str_strcat(struct nvmeibt_Str *this, const char *str)
{
	return nvmeibt_Str_strncat(this, str, SIZE_MAX);
}

/* NOTE: appends the input to the existing content in the buffer */
ssize_t _Str_fread(struct nvmeibt_Str *this, int fd)
{
	char		*buf;
	int			 n_read_total = 0;
	const size_t buffer_size = 16 * PAGE_SIZE;

	N_VERIFY_NVMEIBT_STR_INITIALIZED(error_1_str_Str_fread, this);
	buf = NNVMEIBT_BM_ALIGNED_ALLOC(trace_1_str_Str_fread, PAGE_SIZE, buffer_size);
	while (true) {
		const ssize_t nread = NNVMEIBT_PREAD(warn_str_Str_fread, fd, buf, buffer_size, n_read_total, false);
		if (nread > 0) {
			nvmeibt_Str_strncat(this, buf, nread);
			n_read_total += nread;
		} else {
			if (nread < 0) {
				N_Ef(error_2_str_Str_fread, "Error reading from fd=@FD, @AUTO_ERRNO", fd);
				n_read_total = -1;
			}
			break;
		}
	};
	NNVMEIBT_BM_FREE(trace_2_str_Str_fread, buf);
	return n_read_total;
}

ssize_t nvmeibt_str_read_from_pipe_fd(struct nvmeibt_Str *this, int fd, const char *pipe_name)
{
	ssize_t read_max_size;
	ssize_t nread;
	ssize_t orig_len = nvmeibt_Str_strlen(this);
	do {
		read_max_size = nvmeibt_Str_strlen(this) + 1000;
		NNVMEIBT_STR_RESIZE_BUF(vsghdje, this, max(Str_get_allocated_size(this), nvmeibt_Str_strlen(this) + read_max_size + 1));
		errno = 0;
		nread = read(fd, this->text_buf + nvmeibt_Str_strlen(this), read_max_size);
		if (nread <= 0) {
			if (errno != EAGAIN) N_Ef(rvgka9b, "Error reading from fd=@FD, @AUTO_ERRNO", fd);
			break;
		}
		this->str_len += nread;
		this->text_buf[this->str_len] = '\0';
	} while (nread == read_max_size);
	N_Tf(vsgrfgj, "@STR n_read=@SIZE_T", pipe_name, nvmeibt_Str_strlen(this) - orig_len);
	return (nvmeibt_Str_strlen(this) - orig_len);
}

ssize_t _Str_fread_atomic(struct nvmeibt_Str *this, int fd)
{
	ssize_t		 nread;
	size_t		 text_buf_preloaded_len = nvmeibt_Str_strlen(this);
	const size_t buffer_size = 4 * PAGE_SIZE;
	NNVMEIBT_STR_RESIZE_BUF(t_zzz_23, this, max(Str_get_allocated_size(this), buffer_size));
	// iterate with increasing buf size until all data is read in one shot
	while (1) {
		const ssize_t free_space_len = Str_get_allocated_size(this) - text_buf_preloaded_len - 1;
		N_VERIFY_SIZE(error_2_str_Str_fread_atomic, this);
		nread = NNVMEIBT_PREAD_ATOMIC(t_zzz_24, fd, (char *)nvmeibt_Str_str(this) + text_buf_preloaded_len, free_space_len, (off_t)0, false);
		if (nread < 0) {
			N_Ef(error_3_str_Str_fread_atomic, "Error reading from fd=@FD, @AUTO_ERRNO", fd);
			break;
		}
		if (nread < free_space_len) {
			// Successful read
			this->str_len += nread;
			this->text_buf[this->str_len] = '\0';
			break;
		}
		// Prepare for the next iteration with a larger buffer
		NNVMEIBT_STR_RESIZE_BUF(t_zzz_25, this, Str_get_allocated_size(this) * 2);
	}
	return nread;
}

ssize_t _Str_fwrite(struct nvmeibt_Str *this, int fd)
{
	const size_t  this_strlen = nvmeibt_Str_strlen(this);
	const ssize_t n_written = nvmeibt_write(fd, nvmeibt_Str_str(this), this_strlen);
	if (n_written < 0) N_Ef(error_1_str_Str_fwrite, "csv buffer no full write (asked @STRLEN)", this_strlen);
	N_VERIFY_SIZE(error_2_str_Str_fwrite, this);
	return n_written;
}

ssize_t _Buf_fwrite(struct nvmeibt_Buf *this, int fd)
{
	const ssize_t n_written = nvmeibt_write(fd, this->data_buf, this->buf_len);
	if (n_written < 0) N_Ef(tqmut37, "csv buffer no full write (asked @STRLEN)", this->buf_len);
	return n_written;
}

TODO("is this needed?");
int nvmeibt_Str_fwrite(struct nvmeibt_Str *this, FILE *file)
{
	const size_t this_strlen = nvmeibt_Str_strlen(this);
	const size_t nwrite = fwrite(nvmeibt_Str_str(this), 1, this_strlen, file);
	if ((nwrite != this_strlen) || ferror(file)) {
		N_Ef(tmstrfwr1, "csv buffer no full write (asked @STRLEN, done @SIZEOF), (@AUTO_ERRNO)", this_strlen, nwrite);
		return -1;
	}
	N_VERIFY_SIZE(error_3_str_nvmeibt_Str_fwrite, this);
	return nwrite;
}

static inline int is_str_overlapping_nvmeibt_Str(const struct nvmeibt_Str *this, const char *str)
{
	return this->text_buf && nvmeibt_do_ranges_overlap((unsigned long long)nvmeibt_Str_str(this),
													   (unsigned long long)(nvmeibt_Str_str(this) + nvmeibt_Str_strlen(this)),
													   (unsigned long long)str,
													   (unsigned long long)(str + strlen(str)));
}

int nvmeibt_Str_strncpy(struct nvmeibt_Str *this, const char *str, size_t size)
{
	int rv = -1;
	N_VERIFY_NVMEIBT_STR_INITIALIZED(error_str_nvmeibt_Str_strncpy, this);
	if ((this->text_buf == str) && (this->text_buf)) {
		rv = 0;
		if (size != this->str_len) {
			N_Wf(warn_str_nvmeibt_Str_strncpy, "Copying string into itself but attempted to modify size, old size=@SIZEOF, new requested size=@SIZEOF, string=\n{@TEXT_BUF}.", this->str_len, size, this->text_buf);
			if (size < Str_get_allocated_size(this)) {
				this->text_buf[size - 1] = 0;
				this->str_len = size - 1;
			}
		}
	} else if (is_str_overlapping_nvmeibt_Str(this, str)) {
		TODO("use memmove() in these situations");
		N_Ef(error_1_str_nvmeibt_Str_strncpy, "Ignoring Copy: Overlapping src&dst. dst=@DST_PTR, src=@SRC_PTR, size=@SIZEOF, dst_string=\n{@TEXT_BUF}\n, src_string=\n{@STR}", this->text_buf, str, size, this->text_buf, str);
		TOMA_ABORT_IF_DEBUG(ES_FATAL);
	} else {
		this->str_len = 0;
		if (this->text_buf) *this->text_buf = 0;
		rv = nvmeibt_Str_strncat(this, str, size);
	}
	N_VERIFY_SIZE(error_2_str_nvmeibt_Str_strncpy, this);
	return rv;
}

int nvmeibt_Str_strcpy(struct nvmeibt_Str *this, const char *str)
{
	return nvmeibt_Str_strncpy(this, str, SIZE_MAX);
}

/************************  nvmesh.conf interface *****************************/
static uint8_t nvmeibt_KVP_char_type[256];
static uint8_t char_type_space = ' ';
static uint8_t char_type_word = 'w';
static uint8_t char_type_comment_till_EOL = '#';
static uint8_t char_type_EQ = '=';
static uint8_t char_type_EOL = '\n';
static uint8_t char_type_TERMINATING_NULL = '\0';

static void nvmeibt_tokenize_KVP_init(void)
{
	int i;

	for (i = 1; i <= ' '; i++) {
		nvmeibt_KVP_char_type[i] = char_type_space;	   // The strange chars (incl space)
	}
	for (i = ' ' + 1; i < 256; i++) {
		nvmeibt_KVP_char_type[i] = char_type_word;	  // Every char is accepted (say in a URL) starts a new token
	}
	// Space
	// nvmeibt_KVP_char_type[' '] = char_type_space;	// space, already assigned earlier
	nvmeibt_KVP_char_type['\t'] = char_type_space;	  // tab
	nvmeibt_KVP_char_type['"'] = char_type_space;	 // Sort of strip them
	//
	nvmeibt_KVP_char_type['\n'] = char_type_EOL;	// new_line
	nvmeibt_KVP_char_type['\r'] = char_type_EOL;	// carriage-return
	nvmeibt_KVP_char_type['\0'] = char_type_TERMINATING_NULL;	 // null
	nvmeibt_KVP_char_type['='] = char_type_EQ;
	nvmeibt_KVP_char_type['#'] = char_type_comment_till_EOL;
}

enum PARSE_STATE {
	PARSE_STATE_IGNORE_TILL_EOL = 1,
	PARSE_STATE_BEFORE_KEY = 2,
	PARSE_STATE_IN_KEY = 3,
	PARSE_STATE_BEFORE_EQ = 4,
	PARSE_STATE_BEFORE_VAL = 5,
	PARSE_STATE_IN_VAL = 6,
};

int	nvmeibt_tokenize_KVP(char *in_str_null_terminated, size_t in_str_len_incl_null, struct nvmeibt_KVP *output_KVP_arr, int n_entries_output_KVP_arr)
{
	int						rv = 0;
	size_t					i;
	int						output_KVP_arr_entry_no = 0;
	uint8_t					char_type;
	uint8_t					prev_char_type = char_type_EOL;
	int						cur_line_offset_for_logging = 0;
	//
	enum PARSE_STATE		parse_state;

	NFIN;
	nvmeibt_tokenize_KVP_init();
	parse_state = PARSE_STATE_BEFORE_KEY;
	for (i = 0; i < in_str_len_incl_null; i++) {
		char_type = nvmeibt_KVP_char_type[(uint8_t)(in_str_null_terminated[i])];

		/*
		N_Tf(ccrqwb2, "@CHAR: type=@CHAR state=@CHAR",
			 in_str_null_terminated[i],
			 (char_type == char_type_space ? 'S' : char_type == char_type_word ? 'W' : char_type == char_type_EOL ? 'L' : char_type == char_type_EQ ? '=' :
			  char_type == char_type_TERMINATING_NULL ? 'U' : char_type == char_type_comment_till_EOL ? '#' : 'X'),
			 (parse_state == PARSE_STATE_IGNORE_TILL_EOL ? '#' : parse_state == PARSE_STATE_BEFORE_KEY ? 'k' : parse_state == PARSE_STATE_IN_KEY ? 'K' :
			  parse_state == PARSE_STATE_BEFORE_EQ ? '=' : parse_state == PARSE_STATE_BEFORE_VAL ? 'v' : parse_state == PARSE_STATE_IN_VAL ? 'V' : 'X'));
		*/

		if (parse_state == PARSE_STATE_IGNORE_TILL_EOL) {
			if (char_type == char_type_EOL) {
				prev_char_type = char_type;
				cur_line_offset_for_logging = i + 1;
				parse_state = PARSE_STATE_BEFORE_KEY;
			} else {
				continue;
			}
		}
		if (char_type == prev_char_type) {
			if ((parse_state == PARSE_STATE_BEFORE_VAL) && (in_str_null_terminated[i] == '"')) {	// the val == ""
				output_KVP_arr[output_KVP_arr_entry_no].val = in_str_null_terminated + i;
				parse_state = PARSE_STATE_IN_VAL;	 // It happened suddenly due to an empty val
			} else {
				continue;
			}
		}
		// This char signifies an end of the prev state
		//
		// Close the current key/val (if any) upon change
		if (parse_state == PARSE_STATE_IN_KEY) {
			output_KVP_arr[output_KVP_arr_entry_no].key_len = (in_str_null_terminated + i - output_KVP_arr[output_KVP_arr_entry_no].key);
			in_str_null_terminated[i] = '\0';	 // destructive. Push a \0 over a non-important char
			parse_state = PARSE_STATE_BEFORE_EQ;
		} else if (parse_state == PARSE_STATE_IN_VAL) {
			// End of KVP
			output_KVP_arr[output_KVP_arr_entry_no].val_len = (in_str_null_terminated + i - output_KVP_arr[output_KVP_arr_entry_no].val);
			in_str_null_terminated[i] = '\0';	 // destructive. Push a \0 over a non-important char
			N_Tf(tyd786k, "key=@STR val=@STR", output_KVP_arr[output_KVP_arr_entry_no].key, output_KVP_arr[output_KVP_arr_entry_no].val);
			parse_state = PARSE_STATE_IGNORE_TILL_EOL;
			output_KVP_arr_entry_no++;
		}
		// start a new state
		if (char_type == char_type_space) {
			// Nothing
		} else if (char_type == char_type_comment_till_EOL) {
			if (parse_state == PARSE_STATE_IN_VAL) {
				// '#' inside VAL is valid
			} else {
				parse_state = PARSE_STATE_IGNORE_TILL_EOL;
			}
		} else if (char_type == char_type_EOL) {
			if (parse_state != PARSE_STATE_IGNORE_TILL_EOL) {
				N_Wf(0h3k2ha, "Ignoring Illegal line '@STR'", in_str_null_terminated + cur_line_offset_for_logging);
			}
			cur_line_offset_for_logging = i + 1;
			parse_state = PARSE_STATE_BEFORE_KEY;
		} else if (char_type == char_type_EQ) {
			if (parse_state == PARSE_STATE_BEFORE_EQ) {
				parse_state = PARSE_STATE_BEFORE_VAL;
			} else if (parse_state == PARSE_STATE_IN_VAL) {
				// '=' inside VAL is valid
			} else {
				N_Wf(ctvsgh2, "OOPS at '@STR'", in_str_null_terminated + i);
			}
		} else if (char_type == char_type_word) {
			if (parse_state == PARSE_STATE_BEFORE_KEY) {
				// Validate and progress the output_KVP_arr_entry_no
				if (output_KVP_arr_entry_no >= n_entries_output_KVP_arr) {
					N_Ef(5y38skn, "Parsing KVP_no=@INT into an array of @INT tokens", output_KVP_arr_entry_no, n_entries_output_KVP_arr);
					rv = -1;
					goto out;
				}
				//
				output_KVP_arr[output_KVP_arr_entry_no].key = in_str_null_terminated + i;
				parse_state = PARSE_STATE_IN_KEY;
			} else if (parse_state == PARSE_STATE_BEFORE_VAL) {
				output_KVP_arr[output_KVP_arr_entry_no].val = in_str_null_terminated + i;
				parse_state = PARSE_STATE_IN_VAL;
			} else {
				N_Wf(sghsuy3, "OOPS at '@STR'", in_str_null_terminated + i);
			}
		}
		prev_char_type = char_type;
	}
	rv = output_KVP_arr_entry_no;
out:
	NFOUT;
	return rv;
}

nvmeibt_str_with_escape_chars_t nvmeibt_escape_special_characters(const char *in)
{
	nvmeibt_str_with_escape_chars_t		out_buf;
	char								*out = &(out_buf.s[0]);
	const char							*p = in - 1;

	while (*(++p)) {
		if ((out - out_buf.s) > ((long int)sizeof(out_buf.s) - 10)) {
			NVMEIBT_LONG_TRACE_WRAPPER(crshjwe, "String is too long", in, sizeof(out_buf.s));
			N_Ef(v0wj4iw, "String is too long");
			nvmeibt_abort(ES_FATAL);
		}
		switch (*p) {
		case '"':	*(out++) = '\\';	*(out++) = '"';		continue;
		case '\b':	*(out++) = '\\';	*(out++) = 'b';		continue;
		case '\f':	*(out++) = '\\';	*(out++) = 'f';		continue;
		case '\n':	*(out++) = '\\';	*(out++) = 'n';		continue;
		case '\r':	*(out++) = '\\';	*(out++) = 'r';		continue;
		case '\t':	*(out++) = '\\';	*(out++) = 't';		continue;
		case '\\':	*(out++) = '\\';	*(out++) = '\\';	continue;
		default:	*(out++) = *p;							continue;
		}
	}
	*out = 0;
	return out_buf;
}
