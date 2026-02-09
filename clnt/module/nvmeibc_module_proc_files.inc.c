#pragma push_macro("__FILE_LITERAL__")
#undef __FILE_LITERAL__
#define __FILE_LITERAL__  nvmeibc_module_proc_files_inc_c

#include "../core_unitest/corecomm_injections.h"
#include "utils/nvmeib_jdr/nvmeib_jdr.h"
#include "utils/nvmeib_jdr/nvmeib_txt.h"
#include "block/datapath_utils_generic/nvmeibc_block_dp_buffers.h"

#define VERSION_PROC_FRMT_VER 1
/********************* Shared proc files for all clnt-instances ***************/
static ssize_t fill_version_json(void *dummy, char *buffer, size_t len)
{
	int count = 0;
	(void)dummy;

	count += scnprintf(buffer + count, len - count,
					   "{\"module\" : \"clnt\", \"commit\" : \"%llx\", \"release\" : \"%s\", \"version\" : \"%s\", \"build_number\" : \"%s\", \"distro\" : \"%s\"",
		(u64)COMMIT_ID, __stringify(NVMESH_RELEASE), __stringify(NVMESH_VERSION), __stringify(BUILD_NUMBER), __stringify(BUILD_DISTRO));
	count += nvmeib_proc_add_json_proc_epilog(VERSION_PROC_FRMT_VER, buffer + count, len - count);
	count += scnprintf(buffer + count, len - count, "}\n");
	corecomm_inj_code(({
		count +=
		    scnprintf(buffer + count, len - count,
		              "WARNING: !!CORE_UNITEST enabled!!\n"
		              "Shall never run like that in production environment.\n");
	}));
	return count;
}

static ssize_t fill_dict_sign(void *dummy, char *buffer, size_t len) {
#define BUF_ADD(...) count += scnprintf(buffer+count, len-count, __VA_ARGS__)
	int count = 0;
	(void)dummy;
	BUF_ADD("%u\n", (unsigned int)NVMEIB_DICTIONARY_CKSUM);
	return count;
#undef BUF_ADD
}

#include "module/instance/nvmeibc_cinst.h"
static ssize_t fill_isntances_info(void *dummy, char *buffer, size_t len)
{
	struct jdr jdr = jdr_make((struct charvec){.base=buffer,.len=len});
	nvmeibc_cinst_array_debug_print(dummy, &jdr);
	return jdr_finalize(&jdr).len;
}

void __change_memory(char *buf);
void __change_memory(char *buf)			// can't make static as used by unit tests
{
#if NVMESH_IS_PRODUCTION_COMPILATION
	(void)buf;
#else
	u64 ptr = 0, val = 0;
	char type = 0;
	const int rv = sscanf(buf, "%c0x%llx=0x%llx", &type, &ptr, &val);
	if ((rv == 3)&&((ptr+0x1000) > 0x2000)) {			// Invalid ptr +-0x10
		#define __set_mem(tp, ptr, val) 	((tp *)ptr)[0] = (tp)val
		switch (type) {
		case 'c': case 'b':
		__set_mem(u8 , ptr, val); return;
		case 'i': case 'u':
		__set_mem(u32, ptr, val); return;
		case 'L': case 'l':
		__set_mem(u64, ptr, val); return;
		default:;
		}
	}
	NVMEIB_LOG_LONGTERM("Wrong format: @CHAR, @LLX[0]=@LLX", _E, /*Default*/, t_01_chngmem, type, ptr, val);
	return;
#endif
}

/* Proc callback, allows to echo text into binary tracing */
/*TODO: Reduce frame size to below 1024B*/
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wframe-larger-than="
static inline ssize_t __echo_msg_to_longterm_log(__attribute__ ((unused)) void * arg, char *buf, size_t len) {
	#define _MAX_ECHO_MSG_LEN 1024
	char safe_buf[_MAX_ECHO_MSG_LEN];
	if (len >= sizeof(safe_buf))
		return -EINVAL;

	strncpy(safe_buf, buf, len);
	safe_buf[len] = '\0'; /* Make sure to NULL terminate */
	NVMEIB_LOG_LONGTERM("@STR", _E, /*Default*/, t_01_echo_message, safe_buf);
	if (buf[0] == '@') {					// Special code inserting warnings
		unsigned long st_ents[8];
		struct nvmeib_stack_trace st = { .max_entries = ARRAY_SIZE(st_ents), .entries = st_ents, .skip = 0,};
		nvmeib_public_save_stack_trace(&st);
		NVMEIB_LOG_LONGTERM("Stack Trace Example:\n @STACK_TRACE\nEndExample", _E, /*Default*/, t_02_echo_message, &st);
		WARN((buf[1] == 'W'), "Debug Warning at %ld[sec]", (long unsigned)(jiffies/HZ));		// Test warning
	} else if (buf[0] == '#') {				// Special code for changing memory
		__change_memory(&buf[1]);
	}
	return len;
}

/******************************************************************************/

/* Clear pages allocation stats on all CPUs */
static ssize_t clear_pages_alloc_stats(void *dummy, char *buf, size_t len)
{
	(void)dummy;
	(void)buf;

	nvmeibc_pages_alloc_stats_clear();

	return len;
}

#define BLK_PAGES_ALLOC_STATS_PROC_FRMT_VER 1

/* Fill pages allocation stats summed over all CPUs.
   Latencies are converted from nanoseconds to microseconds with 100-nanosecond (1 decimal point) precision. */
static ssize_t fill_pages_alloc_stats(void *dummy, char *buffer, size_t len)
{
	struct nvmeib_txt txt = nvmeib_txt_make((struct charvec){.base = buffer, .len = len});

	(void)dummy;

	nvmeibc_pages_alloc_stats_to_txt(&txt);

	nvmeib_proc_add_txt_proc_epilog_txt(BLK_PAGES_ALLOC_STATS_PROC_FRMT_VER, &txt);
	return nvmeib_txt_finalize(&txt).len;
}

#pragma pop_macro("__FILE_LITERAL__")
