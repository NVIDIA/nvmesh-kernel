#ifndef KR_INCS_H
#define KR_INCS_H

int printk(const char *fmt, ...)  __attribute__ ((format (printf, 1, 2)));
#if defined(UM_APP) || defined(NVMESH_SIMULATOR)       // Compiling DP_LIB code into real UM (via inclusion of raw .c files )
	#include "common/compat/kr_incs_um_framework.h"
	#include "framework/mutex.h"
	#include "framework/completion.h"
	#include "framework/workqueue_emu.h"
	#include "framework/kth_emu.h"
	#include "rte_byteorder.h"		// SPDK layer basic
	#include "common/compat/kr_incs_types.h"
	#include "common/compat/kr_incs_time.h"
	#include "common_public/nvmeib_uuid_be.h"
	#include "common/nvmeib_str.h"
	#include "common/compat/kr_incs_sgl.h"
	#include <sys/uio.h>		// struct iovec, needed for UM to execute disk IO
	static inline struct iovec sg_as_iovec(struct scatterlist *sg) {
		return (struct iovec){.iov_base = (void*)sg_virt(sg), .iov_len=sg->length};
	}
	static inline void sg_table_as_iovec_array(struct sg_table const *t, struct iovec * iovec_iter, unsigned int n_iovecs) {
		struct scatterlist * curr_sg = NULL;
		unsigned int sg_idx = 0;
		BUG_ON(n_iovecs < t->nents);
		for_each_sg(t->sgl, curr_sg, t->nents, sg_idx) {
			*iovec_iter = sg_as_iovec(curr_sg);
			iovec_iter += 1;
		}
	}
#else								// Using dp lib make file (.a)
	#include "common/compat/kr_incs_types.h"
	#define pr_emerg(fmt, ...) 	printk("\001" "0" "\x1b[1;31m" fmt "\x1b[0;0m", ##__VA_ARGS__)
	#define pr_info(fmt, ...) 	printk("\001" "0" "\x1b[1;33m" fmt "\x1b[0;0m", ##__VA_ARGS__)
	#define WARN(        condition, fmt, ...)	({ const int hit___ = !!(condition); if (hit___) { pr_emerg(fmt, ##__VA_ARGS__); abort(); }; })
	#include "common/compat/kr_incs_asserts.h"
	#define	IFNAMSIZ	16
	#define cpu_to_be32(x) (x)
	#define be32_to_cpu(x) (x)
	#define cpu_to_be64(x)  (x)
	#define cpu_to_be16(x)  (x)
	#define be16_to_cpu(x)  (x)
	#define be64_to_cpu(x)  (x)
	#define rte_cpu_to_le_32(x) (x)
	#define rte_cpu_to_le_64(x) (x)
	#define rte_le_to_cpu_64(x) (x)
	#define rte_le_to_cpu_32(x) (x)

	// config
	#define NVMEIB_VOLUME_TYPE_H	// #include "nvmeib_volume_type.h"
	enum nvmeibc_config_volume_type { NORMAL_VOLUME = 0, };
	#define NVMEIBC_MCS_CLIENT_SCHEME 	// #include "nvmeibc_mcs_stub.h", auto generated file in kernel
	typedef enum { mcs_enum_dummy = 0}  enum_io_perm;	// Dummy config, Sync-lib does not need topology/config as caller takes care of it
	typedef enum { OWNER_SCHEME_FIRST_L_INC_A = 1, OWNER_SCHEME_SL_START_DEC_C = 4 } lock_server_type_e;
	struct nvmeibc_locks_scheme_conf { unsigned int type, maxNOwners, locksetShift; };	// Needed for locks server

	struct mutex {};	// Unused
	struct completion { volatile int ctr; };
	static inline void init_completion(    struct completion *x){ x->ctr = 0; }
	static inline void reinit_completion(  struct completion *x){ BUG_ON(x->ctr != 0); x->ctr = 1; }
	static inline void wait_for_completion(struct completion *x) { /* Sleep here*/ BUG_ON(x->ctr != 0); }
	static inline void complete(           struct completion *x) { x->ctr--; BUG_ON(x->ctr != 0); }

	// linux/workqueue.h
	struct work_struct { void (*func)(struct work_struct *work); };
	#define INIT_WORK(_work, _func) ({ (_work)->func = (_func); })
	#define schedule_work(work) ({ (work)->func(work); true; })
	#define schedule_work_on(cpu, work) schedule_work(work)
	#define workqe_struct 	work_struct
	#define init_timer(...) ({ BUG(); })

	// KTH emulation
	struct nvmeib_public_kth_id { uint32_t id; void *ptr; };
	struct nvmeib_public_kth_obj { struct nvmeib_public_kth_id kth; int events; };
	struct nvmeib_public_kth_params { const char *name, *short_name; int cpu; bool started, is_main; int (*run)(void *arg); int (*done)(void *arg); void *run_arg; void *done_arg;};
	struct nvmeib_public_kth_event { struct nvmeib_public_kth_id id; int type; int (*call)(void *, struct nvmeib_public_kth_event *); void (*free)(struct nvmeib_public_kth_event *); void *no_recipient; bool sync; };
	static inline const char * nvmeib_public_kth_get_name(void){ BUG(); return "?????";}
	static inline const char * nvmeib_kth_event_to_str(int i) { (void)i; BUG(); return "????";}
	static inline bool nvmeib_public_kth_is_err(struct nvmeib_public_kth_id id) { (void)id; BUG(); return false; }
	static inline void nvmeib_public_kth_obj_init(struct nvmeib_public_kth_obj *obj) { (void)obj; BUG(); }
	static inline struct nvmeib_public_kth_id nvmeib_public_kth_create(struct nvmeib_public_kth_params *p) { (void)p; BUG(); return (struct nvmeib_public_kth_id){.id = 0 }; }
	static inline int nvmeib_public_kth_free(struct nvmeib_public_kth_id id) { (void)id; BUG(); return 0; }
	static inline int nvmeib_public_kth_stop(struct nvmeib_public_kth_id id) { (void)id; BUG(); return 0; }
	static inline int nvmeib_public_kth_add_event(struct nvmeib_public_kth_event *e) { (void)e; BUG(); return 0; }
	static inline void nvmeib_public_kth_event_free(struct nvmeib_public_kth_event *e) { (void)e; BUG(); }
	static inline struct list_head * nvmeib_public_kth_switch_current_eq(int *c) { (void)c; BUG(); return NULL; }
	typedef bool (*filter_f)(struct nvmeib_public_kth_event *);
	static inline int nvmeib_public_kth_wait_events_timeout(filter_f f, bool d, struct nvmeib_public_kth_event **pe, long t) { (void)f; (void)d; (void)pe; (void)t; BUG(); return 0; }
#endif

#include "common/compat/kr_incs_malloc.h"
#include "common/compat/kr_incs_bit_ops.h"
#include "common/compat/kr_incs_data_structs.h"
#include "common/compat/kr_incs_sgl.h"
#include "common/compat/kr_incs_time_jiff.h"

#define NVMEIB_UTILS_BIN_TRACES_H	// include "common/nvmeib_utils_bin_traces.h"
#define NVMEIB_UTILS_H				// include "common/nvmeib_utils.h"
#define KR_UNDEF_H					// #include "kr_undef.h"
#define NVMEIB_PUBLIC_KTH_H			// #include "nvmeib_public_kth.h"
#define NVMEIB_PUBLIC_H				// include "nvmeib_public.h"

/******************************* Kernel macros *******************************/
#define mod_timer(...) ({  BUG(); })
struct delayed_work { struct work_struct work; };
#define INIT_DELAYED_WORK(_dwork, _func) INIT_WORK(&(_dwork)->work, _func)
#define schedule_delayed_work(dwork, ...) ({ schedule_work(&(dwork)->work); })
static inline unsigned int get_random_u32(void){ return (unsigned int)(rand()&0xFFFFFFFF); }

#define cpu_to_le32(x)  rte_cpu_to_le_32(x)
#define cpu_to_le64(x)  rte_cpu_to_le_64(x)
#define le64_to_cpu(x)  rte_le_to_cpu_64(x)
#define le32_to_cpu(x)  rte_le_to_cpu_32(x)

// Atomic operations /inlcude/asm/atomic.h
typedef struct { long long c; } atomic64_t, atomic_long_t; 	// c - counter. Artificial structto support {0} initialization
typedef struct { int       c; } atomic_t;					// c - counter
#define ATOMIC_INIT(i)	{i}
static inline void 		atomic_set(             	atomic_t *v, int i) { v->c = i; }
static inline int  		atomic_read(      const 	atomic_t *v) { return v->c; }
static inline int  		atomic_dec_return(			atomic_t *v) { return --v->c; }
static inline int  		atomic_inc_return(			atomic_t *v) { return ++v->c; }
static inline int  		atomic_sub_return(  int x,	atomic_t *v) { (v->c)-=x; return v->c; }
static inline int  		atomic_add_return(  int x,	atomic_t *v) { (v->c)+=x; return v->c; }
static inline void 		atomic_add(         int x,	atomic_t *v) { (v->c)+=x; }
static inline void 		atomic_sub(         int x,	atomic_t *v) { (v->c)-=x; }
static inline int  		atomic_dec_and_test(     	atomic_t *v) { (v->c)--;   return (v->c==0); }
static inline int  		atomic_sub_and_test(int x,	atomic_t *v) { (v->c)-=x;  return (v->c==0); }
static inline void 		atomic_inc(					atomic_t *v) { ++v->c; }
static inline void 		atomic_dec(					atomic_t *v) { --v->c; }
static inline int  		atomic_xchg(				atomic_t *v, int n) {        const int rv = v->c; v->c =  n;                   return rv; }
static inline int  		atomic_cmpxchg(				atomic_t *v, int o, int n) { const int rv = v->c; v->c =  (v->c == o) ? n : o; return rv; }
static inline int  		atomic_add_unless(			atomic_t *v, int a, int u) { const int rv = v->c; v->c += (v->c != u) ? a : 0; return rv; }
static inline void 		atomic64_set(				atomic64_t *v, long long i) { v->c = i; }
static inline long long atomic64_dec_return(		atomic64_t *v) { return --v->c; }
static inline long long atomic64_inc_return(		atomic64_t *v) { return ++v->c; }
static inline void 		atomic64_inc(		 		atomic64_t *v) { ++v->c; }
static inline long long atomic64_read(		  const atomic64_t *v) { return v->c; }
static inline int  		atomic_dec_if_positive(		atomic_t *v) { v->c += (v->c > 0) ? -1 : 0; return v->c; }

// Built-in kernel spinlock_t (busy waiting lock)
typedef struct spinlock {} spinlock_t, raw_spinlock_t;
static inline int  	spin_lock_init(		spinlock_t *l) { (void)l; return 0; }
static inline int  	spin_lock_destroy(	spinlock_t *l) { (void)l; return 0; }
static inline void 	spin_unlock(   		spinlock_t *l) { (void)l; }
static inline int  	spin_is_locked(const spinlock_t *l) { (void)l; return false; }
static inline void 	spin_lock(     		spinlock_t *l) {(void)l; }
static inline int  	spin_lock_irqsave_(	spinlock_t *l, ulong *flags) { (void)l; (void)flags; return 0; }
static inline int  	spin_unlock_irqre_(	spinlock_t *l, ulong *flags) { (void)l; (void)flags; return 0; }
#define spin_lock_irqsave(l,f)      spin_lock_irqsave_(l,&f)	// Support for kernel macro definition
#define spin_unlock_irqrestore(l,f) spin_unlock_irqre_(l,&f)
#define DEFINE_SPINLOCK(l) spinlock_t (l)
#define in_interrupt()		false
struct rw_semaphore {}; // /linux/rwsem.h

/*********************** Emulation of Linux kthread.h ************************/
typedef struct {} call_single_data_t, wait_queue_head_t;
static inline void init_waitqueue_head(wait_queue_head_t* q) { (void)q; }
static inline void wake_up(			   wait_queue_head_t* q) { (void)q; }	// Not needed, not used

// linux/kref	- Todo: Remove, needed for block profiler
struct kref { atomic_t refcount; };
static inline void kref_init(struct kref *kref) { atomic_set(&kref->refcount, 1); }
static inline void kref_get( struct kref *kref) { WARN_ON_ONCE(atomic_inc_return(&kref->refcount) < 2); }
static inline int  kref_sub( struct kref *kref, unsigned int count, void (*release)(struct kref *kref)) {
	if (atomic_sub_and_test((int) count, &kref->refcount)) {
		release(kref);
		return 1;
	}
	return 0;
}
static inline int  kref_put( struct kref *kref, void (*release)(struct kref *kref)){ return kref_sub(kref, 1, release); }

#define cmpxchg(ptr, old, New)	({ if (*ptr == old) *ptr = New; })

/************************ Time / Date / Calendar  ****************************/
struct timer_list { void (*function)(ulong); ulong data; };

/************************ BIO  ****************************/
enum bio_req_io_types { READ = 0, WRITE = 1, REQ_DISCARD = (1 << 7), REQ_WRITE_SAME	= (1 << 9), };

struct bio_vec {					// Bio vecs ,ust be alligned to pages
	struct page	   *bv_page;		// Address to where read or write.
	unsigned int	bv_len;			// Length of the IO in bytes
	unsigned int	bv_offset;		// Typically 0 since IO in units of blocks, alligned to pages. Todo: remove this field
};

#define KS_BVEC_ITER (0)			// Use old BIO api
typedef void bio_end_io_t;			// Not defined the same way as in kernel as it is not used
struct bio {
	sector_t		bi_sector;		// Bio address (rlba) in 512 byte sectors
	enum bio_req_io_types bi_rw;
	unsigned short	bi_vcnt;		// how many bio_vec's
	unsigned short	bi_idx;			// Current index into bi_io_vec, initialize as 0
	unsigned int	bi_size;		// residual I/O count [bytes]
	struct bio_vec*	bi_io_vec;		// The actual vec list of bio pages
	// Execution assist fields. Caller should not touch them. They can be NULL.
	bio_end_io_t*	bi_end_io;
	void*			bi_private;
};

/************************** Transport ***************************************************/
#define NVMEIBC_IB_NET_IO_H		// include "nvmeibc_net_io.h"
enum { NVMEIBC_IB_OVEREAGER = 0xdeaddead, NVMEIBC_IB_OVEREAGER_MAX_REACHED = NVMEIBC_IB_OVEREAGER + 1, };
#define NVMEIBC_IB_ADMIN_CHANNEL_H
#define NVMEIBC_LOCKS_CHANNEL_H
struct nvmeibc_locks_channel {};
#define NVMEIBS_NVME_H
struct nvmeibs_disk_info {};
struct nvmeibs_disk_private_data {};
#define NVMEIBC_ADMIN_CHANNEL_H
struct admin_periodic {};
struct nvmeibc_toma_recv_msg {};
#define NVMEIBS_MAIN_H
#define NVMEIBS_TOMA_H
#define NVMEIBS_DISK_LOCKS_H
struct nvmeibc_volume_req_info {};
struct nvmeibs_nvme_req {};
#define NVMEIB_WORKQ								// Mark as if we are using Ofers implementation of workqueues

/**************** nvmeib *************************************************/
#define NVMEIBS_TYPES_H				// include "srv/nvmeibs_types.h"
#define NVMEIBS_SRV_TOMA_MESSAGES_H	// include "srv/nvmeibs_srv_toma_messages.h"
#define nvmeib_error_code_refine(code)  ((code)&0x4FFF)	// Take only relevant bits (remove the log bit and reserved bits)
#define nvmeib_error_code_has_dnr(code) ((code)&0x4000)	// Is do not retry bit turned on
#include "common/nvmeib.h"

#define NVMEIB_WD_H							// #include "nvmeib_wd.h"
#define CORECOMM_INJECTIONS_H			// #include "../core_unitest/corecomm_injections.h"
#define corecomm_inj_code(...)

/************************* Prevent inclusion of irrelevant client module stuff *******************************/
// Daniel: Do not remove defines below, they protects against future leaking wrong dependencies
#define NVMEIB_COMMON_ALL_H			// Prevent mistaken inclusion of NVME kernel simulator code
#define NVMEIBC_SIMU_DISK_H			// #include "nvmeibc_simu_disk.h"
#define KR_SIM_SVC_H				// #include "kr_sim_svc.h"
#define NVMEIBC_CINST_PARAMS_H		// #include "module/instance/nvmeibc_cinst_params.h"
struct nvmeibc_cinst_params_blk { void **_private; 	u32 binje; };
struct nvmeibc_cinst_params_main {};
struct nvmeibc_cinst_params_core {};
#define NVMEIBC_MAIN_H				// #include "nvmeibc_main.h"
uuid_be *nvmeibc_get_uuid(const struct nvmeibc_cinst_params_core *p);
#define NVMEIBA_API_H				// #include "nvmeiba_nvmesh_api.h"
#define NVMEIBC_BLOCK_API_OS_H		// #include "nvmeibc_block_api_os.h"
struct nvmeib_io_stats;
struct nvmeibc_os_api {struct nvmeib_io_stats *stats;};
static inline bool block_api_os_is_io_api_enabled(const struct nvmeibc_os_api *os) { (void)os; return false; } // No kernel bio, it comes from library caller

#define NVMEIBC_VOLUME_H			// #include "nvmeibc_volume.h"
static inline int nvmeibc_cinst_get_blok_inst_num(const struct nvmeibc_cinst_params_blk *p) { (void)p; return 0; }
#define nvmeibc_cinst_is_first_blok_instance(p) (true)		// There is only 1 instance so it is first
#define NVMEIBC_MAIN_BLOCK_GEN_WORK_SCHED_H
typedef void (*blk2blk_gen_work_t)(void* context);
static inline const struct nvmeibc_cinst_params_main *nvmeibc_isnt_params_blk2main( const struct nvmeibc_cinst_params_blk *p) { (void)p; return NULL; }
static inline const struct nvmeibc_cinst_params_core *nvmeibc_isnt_params_blk2core( const struct nvmeibc_cinst_params_blk* p) { (void)p; return NULL; }
static inline int nvmeibc_cinst_params_blk_get_cinst_params_core_tcp_mode(const struct nvmeibc_cinst_params_blk * p) { (void)p; return 0; }
struct nvmeib_cpu_mask_info;
struct nvmeibc_b_dp_cpu_masks;

// kr_incs.h WQ_ interface
#define nvmeib_schedule_work_on schedule_work_on
#define WORK_CPU_UNBOUND NR_CPUS
#define WQ_INIT_WORK INIT_WORK

#define NVMEIBC_MODULE_MAIN_H
#define NVMEIBC_CINST_H
#define NVMEIBC_CINST_PARAMS_H
#define NVMEIBC_MAIN_COMMON_H

// Operation/bdev throttling/elevator - both are disabled. Caller will implement elevetor & throttling by himself
#define NVMEIBC_DP_OPERATION_PER_CPU_INFRA_H	// #include "operation/nvmeibc_block_dp_operation_per_cpu.h"
#define NVMEIBC_DP_OPERATION_THROTTLING_H		// #include "operation/nvmeibc_block_dp_operation_throttling.h"
#define __mini_elevator_start_plug(...)
#define mini_elevator false
#define __mini_elevator_try_unify_op(...) ({ BUG(); 0; })
#define nvmeibc_operation_throttling_pull_next(...)

#define NVMEIBC_B_CP_ABND_SRV_RESOURCE_H		// Prevent inclusion of loser, it is a huge struct, should be opaque, and need to refactor the below as virtual function
struct nvmeibc_b_cp_loser { spinlock_t lock; };
struct nvmeibc_subscription_ctx;
void nvmeibc_b_cp_loser_aband_jour(struct nvmeibc_b_cp_loser *l, const struct nvmeibc_subscription_ctx *tr, int jent, u8 jid);
static inline u64 nvmeibc_jam_decode_lba(u64 enc_lba) { return enc_lba; }

#endif // #define KR_INCS_H
