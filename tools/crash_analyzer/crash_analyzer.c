#include "crash/defs.h"

#include "../../common/nvmeib_trace_api.h"

#define DEFAULT_DUMP_DIR "."

/* Status prints */
#define DBG_LVL_DBG 50
#define DBG_LVL_INFO 30
#define DBG_LVL_ERR 10

int debug_level = 30;

#define __print(lvl_, ...)              \
	({                                  \
		if(debug_level >= lvl_)         \
			fprintf(fp, ##__VA_ARGS__); \
	})

/* debug */
#define vprint(fmt, ...) __print(DBG_LVL_DBG, "DEBUG: " fmt "\n", ##__VA_ARGS__)
/* info */
#define iprint(fmt, ...) __print(DBG_LVL_INFO, "INFO: " fmt "\n", ##__VA_ARGS__)
/* error */
#define eprint(fmt, ...) __print(DBG_LVL_ERR, "ERROR: " fmt "\n", ##__VA_ARGS__)

/* pointor to_ crash memory space, for strong type checking */
typedef struct
{
	uint64_t val;
} mem_ptr_t;

#define mem_ptr(val_) \
	(mem_ptr_t)       \
	{                 \
		.val = val_   \
	}

#define mem_ptr_off(val_, off_) \
	(mem_ptr_t)                 \
	{                           \
		.val = val_.val + off_  \
	}

#define mem_ptr_valid(ptr) ((ptr).val > 0)

/* A very basic, buggy and totally not reusable hash table implementation, that still does the job
 * to store struct offsets
 */
#define OFF_TABLE_SIZE 1024
typedef struct
{
	const char* strct;
	const char* mem;
	long off;
	int isok;
} mem_off_t;

mem_off_t mem_off_table[OFF_TABLE_SIZE];

uint32_t hash_string(const char* s)
{
	uint32_t hash = 0;

	for(; *s; ++s)
	{
		hash += *s;
		hash += (hash << 10);
		hash ^= (hash >> 6);
	}

	hash += (hash << 3);
	hash ^= (hash >> 11);
	hash += (hash << 15);

	return hash;
}

uint32_t hash_mem_off(const char* strct, const char* mem)
{
	return (hash_string(strct) + hash_string(mem)) % OFF_TABLE_SIZE;
}

long mem_off_resolve(const char* strct, const char* mem)
{
	uint32_t cell = hash_mem_off(strct, mem);
	vprint("Resolving `%s.%s` offset.", strct, mem);
	while(mem_off_table[cell].isok) /* assume at least one not OK, else infinite loop */
	{
		if(!strcmp(strct, mem_off_table[cell].strct) && !strcmp(mem, mem_off_table[cell].mem))
			return mem_off_table[cell].off;
	}
	mem_off_table[cell].strct = strct;
	mem_off_table[cell].mem = mem;
	mem_off_table[cell].off = ANON_MEMBER_OFFSET((char*)strct, (char*)mem);
	if(mem_off_table[cell].off < 0)
	{
		eprint("Resolving `%s.%s` offset - did you forget to load symbols?", strct, mem);
	}
	else
		mem_off_table[cell].isok = 1;

	vprint("Resolved to 0x%lx.", mem_off_table[cell].off);

	return mem_off_table[cell].off;
}

/* pointor to_ crash memory space, for strong type checking */
typedef struct
{
	uint64_t val;
} per_cpu_mem_ptr_t;

#define per_cpu_mem_ptr(val_) \
	(per_cpu_mem_ptr_t)       \
	{                         \
		.val = val_           \
	}

static mem_ptr_t get_cpu_var(per_cpu_mem_ptr_t pcpu, int cpu)
{
	return mem_ptr(pcpu.val + kt->__per_cpu_offset[cpu]);
}

#define __STRINGIFY(x) __STRINGIFY_(x)
#define __STRINGIFY_(x) #x

#define __CAT2(x, y) __CAT2_(x, y)
#define __CAT2_(x, y) x##y

/* Read a @member_ of struct type @struct from pointer @from_ and store result in @to_ */
#define read_member(to_, from_, strct_, member_)                                    \
	({                                                                              \
		long ___res___, ___off___;                                                  \
		if((___res___ = ((___off___ = mem_off_resolve(strct_, member_)) >= 0)))     \
		{                                                                           \
			vprint("Reading memory 0x%lx", (from_).val + ___off___);                \
			___res___ = readmem((from_).val + ___off___, KVADDR, &to_, sizeof(to_), \
				strct_ "." member_, RETURN_ON_ERROR);                               \
		}                                                                           \
		___res___;                                                                  \
	})

/* Read a value from buffer @from_ with @offset and store result in @to_ */
#define read_symbol(to_, sym_, offset_)                                                     \
	({                                                                                      \
		int ___res___ = 1;                                                                  \
		mem_ptr_t ___ptr___ = get_sym(sym_);                                                \
		if(!mem_ptr_valid(___ptr___))                                                       \
			___res___ = 0;                                                                  \
		if(___res___)                                                                       \
			___res___ = readmem(                                                            \
				___ptr___.val + offset_, KVADDR, &to_, sizeof(to_), sym_, RETURN_ON_ERROR); \
		___res___;                                                                          \
	})

/* Read a value from buffer @from_ with @offset and store result in @to_ */
#define read_memory(to_, from_, offset_) \
	readmem((from_).val + offset_, KVADDR, &to_, sizeof(to_), "mem", RETURN_ON_ERROR)

/* Dereference pointer @from_ and store it in @to_ */
#define deref_ptr(from_)                                                                         \
	({                                                                                           \
		mem_ptr_t ___ptr___;                                                                     \
		if(!readmem((from_).val, KVADDR, &___ptr___, sizeof(___ptr___), "ptr", RETURN_ON_ERROR)) \
			error(FATAL, "Pointer deref error 0x%lx", (from_).val);                              \
		___ptr___;                                                                               \
	})

/* Read @size_ bytes from buffer @from_ with @offset_ and store result in @to_ */
#define read_memory_buffer(to_, from_, offset_, size_) \
	readmem((from_).val + offset_, KVADDR, to_, size_, "buf", RETURN_ON_ERROR)

/* You know what container_of does */
static mem_ptr_t crash_container_of(mem_ptr_t ptr, const char* strct, const char* mem)
{
	return mem_ptr(ptr.val - mem_off_resolve(strct, mem));
}

/* Call @cb on each element of a kernel list */
static int crash_list_for_each_entry(
	mem_ptr_t head, const char* type, const char* link, void (*cb)(mem_ptr_t, void*), void* ctx)
{
	long off_next = mem_off_resolve("list_head", "next");
	long off_link = mem_off_resolve(type, link);

	mem_ptr_t iter;

	if(off_next < 0)
	{
		eprint("Unable to resolve kernel list structure, should not happen.");
		return -1;
	}

	if(off_link < 0)
	{
		eprint("Unable to resolve `%s.%s` offset.", type, link);
		return -1;
	}

	if(!read_memory(iter, head, off_next))
	{
		eprint("List 0x%lx of type `%s` is corrupted.", head.val, type);
		return -1;
	}

	while(iter.val != head.val)
	{
		cb(crash_container_of(iter, type, link), ctx);
		if(!read_memory(iter, iter, off_next))
		{
			eprint("List 0x%lx of type `%s` is corrupted.", head.val, type);
			return -1;
		}
	}

	return 0;
}

/* Resolve symbol to a pointer */
static mem_ptr_t get_sym(const char* sym)
{
	struct syment* s = symbol_search((char*)sym);
	if(!s)
	{
		eprint("Initializing symbol `%s` - did you forget to load symbols?", sym);
		return mem_ptr(0);
	}
	vprint("Address of `%s` is 0x%lx\n", sym, s->value);
	return mem_ptr(s->value);
}

static mem_ptr_t trace_system_ptr;
static size_t ncpus;
static size_t nchannels;
static mem_ptr_t names_ptr;
static char names[100][100];
static mem_ptr_t pcpu_ptr;
static per_cpu_mem_ptr_t pcpu[100];
static long free_list_off, ready_list_off, flushing_list_off, active_buf_off;

#define __dump_val(fmt_, val_, ...) vprint(#val_ " = " fmt_, val_, ##__VA_ARGS__)
void dump_globals()
{
	size_t chid;
	__dump_val("0x%lx", trace_system_ptr.val);
	__dump_val("%lu", ncpus);
	__dump_val("%lu", nchannels);
	__dump_val("0x%lx", names_ptr.val);
	__dump_val("0x%lx", pcpu_ptr.val);
	__dump_val("0x%lx", free_list_off);
	__dump_val("0x%lx", ready_list_off);
	__dump_val("0x%lx", flushing_list_off);
	__dump_val("0x%lx", active_buf_off);

	for(chid = 0; chid < nchannels; ++chid)
	{
		__dump_val("0x%lx", pcpu[chid].val);
		__dump_val("%s", names[chid]);
	}
}

int init_globals()
{
	if(!mem_ptr_valid(trace_system_ptr = get_sym("glob_trace_system")))
	{
		eprint("Reading `glob_trace_system` symbol");
		return -1;
	}
	if(!read_member(ncpus, trace_system_ptr, "nvmeib_trace_system", "md.ncpus"))
	{
		eprint("Reading member `md.ncpus`");
		return -1;
	}
	if(!read_member(nchannels, trace_system_ptr, "nvmeib_trace_system", "md.nchannels"))
	{
		eprint("Reading member `md.nchannels`");
		nchannels = 10;
		// return -1;
	}
	if(!read_member(names_ptr, trace_system_ptr, "nvmeib_trace_system", "names"))
	{
		eprint("Reading member `names`");
		return -1;
	}
	if(!read_member(pcpu_ptr, trace_system_ptr, "nvmeib_trace_system", "pcpu"))
	{
		eprint("Reading member `pcpu`");
		return -1;
	}
	if((free_list_off = mem_off_resolve("nvmeib_capuch", "actor.rsc_list")) < 0)
	{
		eprint("Reading member `free_list_off`");
		return -1;
	}
	if((ready_list_off = mem_off_resolve("nvmeib_capuch", "ready_list")) < 0)
	{
		eprint("Reading member `ready_list_off`");
		return -1;
	}
	if((flushing_list_off = mem_off_resolve("nvmeib_capuch", "flushing_list")) < 0)
	{
		eprint("Reading member `flushing_list_off`");
		return -1;
	}
	if((active_buf_off = mem_off_resolve("nvmeib_capuch", "active.buf")) < 0)
	{
		eprint("Reading member `active_buf_off`");
		return -1;
	}
	{
		uint64_t chid;
		for(chid = 0; chid < nchannels; ++chid)
		{
			mem_ptr_t name_ptr;
			if(!read_memory(name_ptr, names_ptr, chid * (sizeof(const char*))))
			{
				eprint("Reading channel %lu name ptr", chid);
				return -1;
			}
			if(!read_memory(names[chid], name_ptr, 0))
			{
				eprint("Reading channel %lu name", chid);
				return -1;
			}
			if(!read_memory(pcpu[chid], pcpu_ptr, chid * (sizeof(per_cpu_mem_ptr_t))))
			{
				eprint("Reading channel %lu pcpu", chid);
				return -1;
			}
		}
	}
	dump_globals();
	return 0;
}

#define CKSUM_OFFSET 4

/**
 * Read tracer buffer checksum (from where it is supposed to be - it can be actually junk there,
 * have to handle this case)
 */
int read_cksum(mem_ptr_t buf_struct_ptr)
{
	int cksum;
	mem_ptr_t data_ptr;
	vprint("Read cksum form buf 0x%lx", buf_struct_ptr.val);
	if(!read_member(data_ptr, buf_struct_ptr, "nvmeib_trace_buf", "act"))
	{
		eprint("Reding cksum - buffer struct from 0x%lx", buf_struct_ptr.val);
		return -1;
	}

	if(!data_ptr.val)
	{
		eprint("Data page is null");
		return -1;
	}

	if(!read_memory(cksum, data_ptr, CKSUM_OFFSET))
	{
		eprint("Reading cksum - buffer content from 0x%lx", data_ptr.val);
		return -1;
	}

	return cksum;
}

enum for_each_buffer_mode
{
	for_each_mode_unread,
	for_each_mode_all,
	for_each_mode_interesting
};

typedef struct buf_work
{
	int cksum;
	int cpu;
	const char* chname;
	int (*action)(void*, const char*, int, const void*, int, int);
	void* param;
	int term_offset;
} buf_work_t;

void call_buf_wrk(mem_ptr_t addr, void* ctx)
{
	char buf[PAGE_SIZE];
	mem_ptr_t buf_ptr;
	buf_work_t* wrk = ctx;
	if(!read_member(buf_ptr, addr, "nvmeib_trace_buf", "act"))
	{
		eprint("Parsing buf struct 0x%lx", addr.val);
		return;
	}
	if(!read_memory_buffer(buf, buf_ptr, 0, sizeof(buf)))
	{
		eprint("Reading buffer 0x%lx", buf_ptr.val);
		return;
	}
	wrk->action(wrk->param, wrk->chname, wrk->cpu, buf, sizeof(buf), wrk->term_offset);
}

/**
 * Do an @action for each buffer on each cpu
 * in @channel (pointer to_ per_cpu_info data structure)
 */
static void do_for_each_buffer(mem_ptr_t capuch, int cpu, const char* chname,
	int (*action)(void*, const char*, int, const void*, int, int), void* param,
	enum for_each_buffer_mode mode)
{
	int cksum;
	buf_work_t wrk;
	mem_ptr_t buf_ptr;
	size_t off;

	iprint("Capuch 0x%lx %s %d", capuch.val, chname, cpu);

	if(!read_member(buf_ptr, capuch, "nvmeib_capuch", "active.buf") || !buf_ptr.val)
	{
		vprint("Capuch is inactive, skipping");
		return;
	}

	assert(read_member(off, capuch, "nvmeib_capuch", "active.off"));

	if((cksum = read_cksum(buf_ptr)) == -1)
	{
		vprint("Capuch is inactive, skipping");
		return;
	}

	wrk = (buf_work_t){cksum, cpu, chname, action, param, 0};

	switch(mode)
	{
		case for_each_mode_unread:
			crash_list_for_each_entry(mem_ptr_off(capuch, flushing_list_off), "nvmeib_trace_buf",
				"rsc.link", call_buf_wrk, &wrk);
			crash_list_for_each_entry(mem_ptr_off(capuch, ready_list_off), "nvmeib_trace_buf",
				"rsc.link", call_buf_wrk, &wrk);
			wrk.term_offset = off;
			call_buf_wrk(buf_ptr, &wrk);
			break;
		case for_each_mode_all:
			crash_list_for_each_entry(mem_ptr_off(capuch, flushing_list_off), "nvmeib_trace_buf",
				"rsc.link", call_buf_wrk, &wrk);
			crash_list_for_each_entry(mem_ptr_off(capuch, ready_list_off), "nvmeib_trace_buf",
				"rsc.link", call_buf_wrk, &wrk);
			crash_list_for_each_entry(mem_ptr_off(capuch, free_list_off), "nvmeib_trace_buf",
				"rsc.link", call_buf_wrk, &wrk);
			wrk.term_offset = off;
			call_buf_wrk(buf_ptr, &wrk);
			break;
		case for_each_mode_interesting:
			/* In the new engine it is actually the same as 'all', but keep compatibility. */
			crash_list_for_each_entry(mem_ptr_off(capuch, flushing_list_off), "nvmeib_trace_buf",
				"rsc.link", call_buf_wrk, &wrk);
			crash_list_for_each_entry(mem_ptr_off(capuch, ready_list_off), "nvmeib_trace_buf",
				"rsc.link", call_buf_wrk, &wrk);
			crash_list_for_each_entry(mem_ptr_off(capuch, free_list_off), "nvmeib_trace_buf",
				"rsc.link", call_buf_wrk, &wrk);
			wrk.term_offset = off;
			call_buf_wrk(buf_ptr, &wrk);
			break;
	}
}

#undef TRY_READ_FROM_CHANNEL

/**
 * Given @cpu and @buffer, dump the buffer (append) to_ a file 'traces_dump_@cpu'
 */
static int dump_buffer(
	void* param, const char* channel_name, int cpu, const void* buffer, int length, int term_offset)
{
	const char* dirname = param;
	char logname[1024];
	FILE* outf;

	if (term_offset != 0) {
		if (term_offset <= 8) {
			/* Dont dump buffers that have no data */
			return 0;
		} else {
			if (term_offset < PAGE_SIZE - 6) {
				/* Terminate teh buffer with zeros */
				((char*)buffer)[term_offset] = ((char*)buffer)[term_offset + 1] =
					((char*)buffer)[term_offset + 2] = ((char*)buffer)[term_offset + 3] =
						((char*)buffer)[term_offset + 4] = ((char*)buffer)[term_offset + 5] = 0;
			}
		}
	}

	sprintf(logname, "%s/%s%d.0", dirname, channel_name, cpu);

	if((outf = fopen(logname, "ab")) == NULL)
	{
		error(INFO, "Error while opening %s.\n", logname);
		return -1;
	}
	if(fwrite(buffer, 1, length, outf) != length)
	{
		error(INFO, "Error while writing to_ %s.\n", logname);
		fclose(outf);
		return -1;
	}
	fclose(outf);
	return 0;
}

/**
 * Ensure directory @pathname exists
 */
static int try_mkdir(const char* pathname)
{
	int ret;

	ret = mkdir(pathname, 0777);
	if(ret < 0)
	{
		if(errno == EEXIST)
			return 0;

		eprint("mkdir `%s` failed\n", pathname);
		return -1;
	}

	return 0;
}

/**
 * Get pathname from arguments.
 */
static const char* get_outdir(int argc, char* argv[])
{
	if(argc <= 1)
	{
		return DEFAULT_DUMP_DIR;
	}

	return argv[1];
}

#define for_each_capuch(capuch_, chid_, cpu_)                                                 \
	for(cpu_ = 0; cpu_ < ncpus; ++cpu_)                                                       \
		for(chid_ = 0, capuch = deref_ptr(get_cpu_var(pcpu[chid_], cpu_)); chid_ < nchannels; \
			++chid_, capuch = deref_ptr(get_cpu_var(pcpu[chid_], cpu_)))

/**
 * Dump traces according to specified work @mode to the given @dirname
 */
static void traces_dump(const char* dirname, enum for_each_buffer_mode mode)
{
	mem_ptr_t capuch;
	uint64_t cpu, chid;

	if(init_globals())
	{
		eprint("Init globals failed");
		return;
	}
	if(try_mkdir(dirname))
	{
		eprint("Directory creation failed");
		return;
	}

	switch(mode)
	{
		case for_each_mode_unread:
			iprint("Going to dump unread buffers");
			break;
		case for_each_mode_all:
			iprint("Going to dump all buffers");
			break;
		case for_each_mode_interesting:
			iprint("Going to dump interesting buffers");
			break;
	}
	for_each_capuch(capuch, chid, cpu)
	{
		do_for_each_buffer(capuch, cpu, names[chid], dump_buffer, (void*)dirname, mode);
	}
	iprint("Done");
}

/**
 * Action handler for traces dump command. Will dump any unread buffers to disk
 */
static void traces_dump_unread(int argc, char* argv[])
{
	traces_dump(get_outdir(argc, argv), for_each_mode_unread);
}

/**
 * Action handler for traces dump command. Will dump all buffers to disk
 */
static void traces_dump_all(int argc, char* argv[])
{
	traces_dump(get_outdir(argc, argv), for_each_mode_all);
}

/**
 * Action handler for traces interesting command.
 * Will dump buffers that contain some valid data, from the old to new
 */
static void traces_dump_interesting(int argc, char* argv[])
{
	traces_dump(get_outdir(argc, argv), for_each_mode_interesting);
}

/***************************
All the below definitions:
Plugin registration stuff
***************************/
static void cmd_func(void)
{
	if(argcnt == 1)
		cmd_usage(pc->curcmd, SYNOPSIS);
	else if(!strcmp(args[1], "all"))
		traces_dump_all(argcnt - 1, args + 1);
	else if(!strcmp(args[1], "interesting"))
		traces_dump_interesting(argcnt - 1, args + 1);
	else if(!strcmp(args[1], "unread"))
		traces_dump_unread(argcnt - 1, args + 1);
	else
		cmd_usage(pc->curcmd, SYNOPSIS);
}

/* clang-format off */
static char* help_str[] = {
	"tracedump",
	"Dumps nvmesh binary tracing buffers",
	"(all|interesting|unread) DESTINATION_DIRECTORY",
	"This will dumps nvmesh binary tracing buffers.",
	"Available commands:",
	"all:",
	"    Dump all buffers to the disk, without any filtering.",
	"interesting:",
	"    Dumps buffers ever written to, regradless whether or not already dumped.",
	"unread:",
	"    Dump all (and only) events that were not flushed to the disk.",
	NULL};
/* clang-format on */

static struct command_table_entry command_table[] = {{"tracedump", cmd_func, help_str, 0}, {NULL}};

void __attribute__((constructor)) cmd_init(void)
{
	register_extension(command_table);
}

void __attribute__((destructor)) cmd_fini(void) {}
