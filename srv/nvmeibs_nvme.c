#include "nvmeib.h"

#include "nvmeibs_types.h"
#include "nvmeibs_nvme.h"
#include "nvmeibs_disk.h"
#include "nvmeib_utils.h"
#include "nvmeibs_test.h"
#include "nvmeib_public_procfs.h"
#include "nvmeib_public.h"
#include "nvmeibs_distribute_intrs.h"
#include "nvmeibs_mcs_stub.h"
#include "nvmeibs_mcs.h"
#include "nvmeibs_toma.h"
#include "nvmeibs_serjio.h"
#include <linux/miscdevice.h>
#include "nvmeibs_trace.h"
#include "nvmeibs_srv_toma_messages.h"
#include "nvmeibs_um_comm.h"
#include "../core_unitest/corecomm_injections.h"
#include "nvmeib_public.h"
#include "nvmeib_io_stats.h"
#include "nvmeib_common_os_block_api.h"
#include "common/proc_epilog.h"
#include "nvmeib_completion_noise.h"
/* Must be last to override module_{init/exit} */
#include "kr_undef.h"

struct workqueue_struct *nvmeibs_nvme_wq = NULL;
EXPORT_SYMBOL(nvmeibs_nvme_wq);

#ifndef PCI_MSIX_ENTRY_CTRL_MASKBIT
#define   PCI_MSIX_ENTRY_CTRL_MASKBIT 1
#endif

#define START_FROZEN 0
#define DUMMY_SIZE 1100000

/*
static bool no_rdda = false;
module_param(no_rdda, bool, 0444);
MODULE_PARM_DESC(no_rdda, "Used to disable Remote Direct Disk Access.");
*/
unsigned max_client_rsrc = NVMEIBS_MAX_DISK_RESOURCES_PER_CLIENT;
module_param(max_client_rsrc, uint, 0644);
MODULE_PARM_DESC(max_client_rsrc, "RDDA is deprecated. Maximum number of RDDA connections per client.");

static int max_completions = 64;
module_param(max_completions, int, 0644);
MODULE_PARM_DESC(max_completions, "Maximum number of networking completions to handle per interrupt.");

static bool nvmeibs_defer_process_io_cq = false;
module_param_named(defer_process_io_cq, nvmeibs_defer_process_io_cq, bool, 0644);
MODULE_PARM_DESC(defer_process_io_cq, "Defer all IO completions to a per completion queue thread, so it is not done in the interrupt context.");

static bool nvmeibs_use_nvme_kwq = true;
module_param_named(use_nvme_kwq, nvmeibs_use_nvme_kwq, bool, 0444);
MODULE_PARM_DESC(use_nvme_kwq, "Determines whether to use a kernel workqueue instead of a wakeup thread for processing completion queues.");

static bool nvmeibs_nvme_wq_unbound = false;
module_param_named(nvme_wq_unbound, nvmeibs_nvme_wq_unbound, bool, 0444);
MODULE_PARM_DESC(nvme_wq_unbound, "Determines whether to use an unbound kernel workqueue for nvmeibs_nvme (true) or a bound one (false).");

/* we dont really need to expose this param as we dont fail anything if we alloc less qs.
   it is just used as an initial/default value for when drive has not too many (< 30) qs */
static int nvmeibs_min_local_nvmeqs = 1;
module_param_named(min_local_nvmeqs, nvmeibs_min_local_nvmeqs, int, 0644);
MODULE_PARM_DESC(min_local_nvmeqs, "Minimum number of NVMe queues per drive to reserve for non-RDDA usage. As RDDA is deprecated, this is obsolete.");

static int nvmeibs_max_local_nvmeqs = 0;
module_param_named(max_local_nvmeqs, nvmeibs_max_local_nvmeqs, int, 0444);
MODULE_PARM_DESC(max_local_nvmeqs, "Maximum NVMe queues for local operation. A value of 0 sets the actual maximum to the lower of the number of CPUs, drive queues, doorbells and MSI-X interrupts available. The value is replaced by the actual number calculated.");

static bool nvmeibs_iommu_enabled = false;
module_param_named(iommu_enabled, nvmeibs_iommu_enabled, bool, 0444);
MODULE_PARM_DESC(iommu_enabled, "Informs the internal NVMesh NVMe driver that the IOMMU is enabled on the node.");

static char ignore_disks[1024];
static char ignore_disks[1024];
module_param_string(ignore_disks, ignore_disks, sizeof(ignore_disks), 0600);
MODULE_PARM_DESC(ignore_disks, "Comma separated list of PCI IDs of NVMe drives to ignore. For example, \"0000:03:00.0,0000:12:01.0\".");

static char ignore_disks_serials[1024];
module_param_string(ignore_disks_serials, ignore_disks_serials, sizeof(ignore_disks_serials), 0600);
MODULE_PARM_DESC(ignore_disks_serials, "Comma separated list of serial IDs of NVMe drives to ignore. For example, \"S23YNAAH201234,S23YNAAH202345\".");

char *dummy_id = NULL;
module_param(dummy_id, charp, 0444);
MODULE_PARM_DESC(dummy_id, "Serial ID to be used for drives on drive-less targets. Dummy drives are rarely needed, only for an arbiter on a 2-node system.");
#define DUMMY_DISK_PATH "/var/opt/nvmesh/metadata_disk_image/"
static bool dummy_disk_added;

static int nvme_number_offset = 1000;
module_param(nvme_number_offset, int, 0444);
MODULE_PARM_DESC(nvme_number_offset, "Offset for /dev/nvme%d device names.");

#define NVMEIBS_CAP_MAX_TRANSFER_SIZE (32 * 4096) /* cap max-transfer to 128KB */
static bool nvmeibs_cap_transfer_size = 1;
module_param_named(cap_transfer_size, nvmeibs_cap_transfer_size, bool, 0444);
MODULE_PARM_DESC(cap_transfer_size, "Cap all disks' max-transfer-size to 128 KB, even if the drive supports larger transfers.");

static char *fake_serial = NULL;
module_param(fake_serial, charp, 0444);
MODULE_PARM_DESC(fake_serial, "Do not use for production systems. Fake serial number for a fake NVMe drive, which should be machine specific.");

#define NVMEIBS_FORMAT_TIMEOUT_SECONDS_FIRST_TRY 300
static int nvmeibs_format_timeout_seconds_second_try = 3600;
module_param_named(format_timeout_seconds_seconds_try,
					nvmeibs_format_timeout_seconds_second_try, int, 0644);
MODULE_PARM_DESC(format_timeout_seconds_second_try, "Seconds to wait for an NVMe format to complete on a second attempt after a failed first attempt.");

static bool nvmeibs_gcp_mode = false;
module_param_named(gcp_mode, nvmeibs_gcp_mode, bool, 0644);
MODULE_PARM_DESC(gcp_mode, "Use only drives that are specified in gcp_drives_to_uuid_list.");

#define GCP_MAX_DRIVES 32
char *gcp_drives_to_uuid_list[GCP_MAX_DRIVES];
int gcp_num_drives;
module_param_array(gcp_drives_to_uuid_list, charp, &gcp_num_drives, 0444);
MODULE_PARM_DESC(gcp_drives_to_uuid_list, "Related to GCP mode, i.e., specifically for GCP virtual NVMe drives. This provides a list of UUIDs of drives to be used. This should be provided on module invocation, i.e., during Target service startup.");

static bool nvmeibs_fake_large_disks = false;
module_param_named(fake_large_disks, nvmeibs_fake_large_disks, bool, 0444);
MODULE_PARM_DESC(fake_large_disks, "Do not use for production systems. Fake the system having larger disks by overriding their size. This is used for developing support for larger drives.");

static uint64_t nvmeibs_fake_large_disk_size_lba = ((uint64_t)1 << (NVMEIB_EC_JMDC_BITS_J2D + NVMEIB_EC_JMDC_BITS_LINK));
module_param_named(fake_large_disk_size_lba, nvmeibs_fake_large_disk_size_lba, ullong, 0444);
MODULE_PARM_DESC(fake_large_disk_size_lba, "Do not use for production systems. The size of fake large disks, in 4k units.");

static bool nvmeibs_use_intr_shaper = true;
module_param_named(use_intr_shaper, nvmeibs_use_intr_shaper, bool, 0644);
MODULE_PARM_DESC(use_intr_shaper, "Determines whether to use an interrupt shaper for NVMe completions.");

static bool nvmeibs_nvme_doorbell_batch = true;
module_param_named(nvme_doorbell_batch, nvmeibs_nvme_doorbell_batch, bool, 0644);
MODULE_PARM_DESC(nvme_doorbell_batch, "Determines whether to batch NVMe doorbell requests.");

static void nvmeibs_free_drives(struct kref *kref);

//OM: increase value as we may be submitting many reset drivven cmds in parallel
#define ADMIN_Q_DEPTH	16

#define LOCAL_IOQ_DEPTH	1024
#define N_ASYNC_EVENTS	1
#define MAX_WAIT_SECONDS 30

//#define MAX_LOCAL_IOQS 4

#ifdef LOCALIZE_SPINLOCK_FOR_PERF
unsigned long noinline __s_nvme_raw_spin_lock_irqsave(raw_spinlock_t *lock)
{
	unsigned long flags;

	local_irq_save(flags);
	preempt_disable();
	spin_acquire(&lock->dep_map, 0, 0, _RET_IP_);
	/*
	 * On lockdep we dont want the hand-coded irq-enable of
	 * do_raw_spin_lock_flags() code, because lockdep assumes
	 * that interrupts are not re-enabled during lock-acquire:
	 */
#ifdef CONFIG_LOCKDEP
	LOCK_CONTENDED(lock, do_raw_spin_trylock, do_raw_spin_lock);
#else
	do_raw_spin_lock_flags(lock, &flags);
#endif
	return flags;
}

#undef spin_lock_irqsave
#define spin_lock_irqsave(lock, flags)                          \
do {                                                            \
		flags = __s_nvme_raw_spin_lock_irqsave(spinlock_check(lock));     \
} while (0)
#endif // LOCALIZE_SPINLOCK_FOR_PERF

struct nvme_bar {
	u64		cap;	/* Controller Capabilities */
	u32		vs;		/* Version */
	u32		intms;	/* Interrupt Mask Set */
	u32		intmc;	/* Interrupt Mask Clear */
	u32		cc;		/* Controller Configuration */
	u32		rsvd1;	/* Reserved */
	u32		csts;	/* Controller Status */
	u32		rsvd2;	/* Reserved */
	u32		aqa;	/* Admin Queue Attributes */
	u64		asq;	/* Admin SQ Base Address */
	u64		acq;	/* Admin CQ Base Address */
	u32		cmbloc;	/* Controller Memory Buffer Location */
	u32		cmbsz;	/* Controller Memory Buffer Size */
};

struct nvme_admin_cmd {
	__u8	opcode;
	__u8	flags;
	__u16	rsvd1;
	__u32	nsid;
	__u32	cdw2;
	__u32	cdw3;
	__u64	metadata;
	__u64	addr;
	__u32	metadata_len;
	__u32	data_len;
	__u32	cdw10;
	__u32	cdw11;
	__u32	cdw12;
	__u32	cdw13;
	__u32	cdw14;
	__u32	cdw15;
	__u32	timeout_ms;
	__u32	result;
};

struct nvme_user_io {
	__u8	opcode;
	__u8	flags;
	__u16	control;
	__u16	nblocks;
	__u16	rsvd;
	__u64	metadata;
	__u64	addr;
	__u64	slba;
	__u32	dsmgmt;
	__u32	reftag;
	__u16	apptag;
	__u16	appmask;
};

#ifndef NVME_IOCTL_ID
#	define NVME_IOCTL_ID		_IO('N', 0x40)
#endif
#ifndef NVME_IOCTL_ADMIN_CMD
#	define NVME_IOCTL_ADMIN_CMD	_IOWR('N', 0x41, struct nvme_admin_cmd)
#endif
#ifndef NVME_IOCTL_SUBMIT_IO
#	define NVME_IOCTL_SUBMIT_IO	_IOW('N', 0x42, struct nvme_user_io)
#endif
#ifndef NVME_IOCTL_IO_CMD
#	define NVME_IOCTL_IO_CMD	_IOWR('N', 0x43, struct nvme_admin_cmd)
#endif

static inline u64 NVMEIB_CAP_TIMEOUT(u64 x)
{
	return MAX(NVME_CAP_TIMEOUT(x), MAX_WAIT_SECONDS * 2);
}

static ulong nvmeibs_timeout = (15*HZ);
static ulong submit_wait_timeout = (15*HZ);
module_param(submit_wait_timeout, ulong, 0644);
MODULE_PARM_DESC(submit_wait_timeout, "Timeout for NVMe admin operations such as drive formatting. Does not affect a second format attempt after a failure, as some drives take a long time to format, especially larger ones. Value in milliseconds.");

static atomic64_t global_uid = ATOMIC64_INIT(0);
static inline u64 get_guid(void)
{
	return atomic64_inc_return(&global_uid);
}

struct drive_params {
	struct device_data *dev;
	u64 gid;
	u64 size;	/* in blocks */
	u32 nsid;
	u8 flbas;
	u64 ncap;
	u8 dps;
	u8 nmic;
	int block_len;
	int metadata;
	bool mtdt_extd;
	char id_str[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];
	struct gendisk *gendisk;
	//struct bio_list bio_list[MAX_LOCAL_IOQS];
	struct bio_list *bio_list;
	bool frozen;
	struct list_head freeze_link;
	struct nvmeibs_disk_info info;
	struct drive_params *next;
};

struct req_id {
	nvme_callback_t *callback;
	void *arg;
	ulong issue_time;
	bool aborted;
};

enum local_q_state {
	LOCAL_Q_ON			= 0,
	LOCAL_Q_STOP_NEW_IO	= 1,
	LOCAL_Q_LAST_COMP 	= 2,
	LOCAL_Q_OFF 		= 3,
	LOCAL_Q_DYING 		= 4,
	LOCAL_Q_DESTROYED 	= 5,
};

static inline const char *ioq_state_str(enum local_q_state sts)
{
	switch (sts) {
	case LOCAL_Q_ON			: return "ON";
	case LOCAL_Q_STOP_NEW_IO: return "STOP_NEW_IO";
	case LOCAL_Q_LAST_COMP 	: return "LAST_COMP";
	case LOCAL_Q_OFF		: return "OFF";
	case LOCAL_Q_DYING		: return "DYING";
	case LOCAL_Q_DESTROYED	: return "DESTROYED";

	default: return "???";
	}
}

enum local_q_irq_state {
	LOCAL_Q_IRQ_NONE			= 0,
	LOCAL_Q_IRQ_DISABLE_NOSYNC	= 1,
	LOCAL_Q_IRQ_ENABLE  		= 2,
};

struct nvme_qp {
	struct device_data *dev;
	void (*complete_fn)(struct nvme_qp *q);
	struct nvme_completion *cq;
	dma_addr_t cq_phys;
	int cq_len;
	int cq_head;
	int old_cq_head;
	int cq_phase;
	u32 __iomem *cq_doorbell;
	struct nvme_command *sq;
	dma_addr_t sq_phys;
	int sq_len;
	int sq_head;
	int sq_tail;
	u32 __iomem *sq_doorbell;
	u64 msix_addr;
	u32 msix_payload;
	spinlock_t q_lock;
	int locking_cpu;
	struct req_id *ids;
	int total_ids;
	int used_ids;
	ulong id_bitmap[round_up(MAX(ADMIN_Q_DEPTH, LOCAL_IOQ_DEPTH), sizeof(ulong))];
	wait_queue_head_t waiting;
	int id;
	char interrupt_name[24];
	struct task_struct *thread;
	struct completion th_ready;
	bool polling;
	int irq_debug;
	enum local_q_state state;
	struct ioqm_q_alloc_workq *ioqm_alloc_w;
	enum local_q_irq_state irq_state;
	bool dying;
	struct nvmeib_qp_stats_pcpu __percpu * qp_stats;
	struct nvme_qp_cmds_stats nvme_qp_stats;
	struct work_struct process_cq_work;
};

/* -------------------------------------------------------------------------- *
 *                      IOQM - IO Queues Manager                              *
 *                                                                            *
 *  IO queues can be allocated to the srv to be used by remote clients in     *
 *  which case they are no longer used for local cmds.                        *
 * -------------------------------------------------------------------------- */

/*
 * IOQM Structures
 */

/* io queues manager per device */
struct device_ioqm {
	/* map the available ioqs in
	   d->local_ioq[] (0 based) */
	u32 n_virt_lioqs;
	u32 *virt_lioqs;
	/* free client-qs pool */
	u32 n_free_client_qs;
	u32 *free_client_qs;
	/* ioqm works in progress (+1).
	   practically up to 1 work,
	   instead of using lock */
	atomic_t refcnt;
	/* pending ioq reqs - allows oversubscription.
	   (not atomic, referenced from wq only) */
	u32 n_pending_reqs;
	struct completion *release_comp;
};

/* io queue allocation work */
//OL: use q's state instead, no?
enum ioqm_q_alloc_state {
	IOQM_Q_ALLOC_START	= 0,
	IOQM_Q_ALLOC_NO_IO = 1,
	IOQM_Q_ALLOC_DONE  = 2,
};
struct ioqm_q_alloc_workq {
	struct workqe_struct work;
	u64 disk_handle;
	enum ioqm_q_alloc_state state;
	int qid;
};

/* io queue free work */
struct ioqm_q_free_workq {
	struct workqe_struct work;
	u64 disk_handle;
	int qid;
};

/* io queues manager main workqueue */
struct workq_struct *ioqm_wq = NULL;

/*
 * Globals & Defines
 */

/* num possible cpus */
static int np_cpus;
/* minimin number of local io queues per device.
   must be > 0, o/w ioqm_get_q_.*()
   functions may loop endlessly */

struct device_data {
	struct device_data *next;
	struct drive_params *drives;
	struct pci_dev *pci_dev;
	bool need_reset;
	bool reset_pending; /* waiting for sts.rdy */
	bool removed;
	bool formatting;
	//struct msix_entry msix_entries[MAX_LOCAL_IOQS + 1];
	struct msix_entry *msix_entries;
	phys_addr_t mmio_phys;
	struct nvme_bar __iomem *mmio;
	void __iomem *doorbells;
	int doorbell_stride;
	int seq;
	u64 cap;
	u32 cc;
	char serial[24];
	char model[NVMEIB_DISK_MAX_MODEL_STR_SIZE];
	u16 vendor;
	u16 cntlid;
	int ser_len;
	bool trim_supported;
	int max_abort;
	size_t max_transfer;
	size_t err_log_size;
	int alignment_size;	// Disk access alignment requiremnt in 4K blocks
	int max_msix;
	phys_addr_t msix_table_phys;
	void __iomem *msix_table;
	/* maximum number of nvme ioqs = N */
	int max_ioqs;
	int min_local_ioqs;
	/* maximum number of local ioqs = min(#CPUs, #Doorbells in 4K)
	   qid: {1 ... max_ioqs} */
	int max_local_ioqs;
	/* maximum number of client qs = max_ioqs - d->min_local_ioqs
	   qid: {1 + d->min_local_ioqs ... max_ioqs} */
	int max_client_qs;
	int n_async;
	struct nvmeibs_q_info *qs_info;
	struct nvme_qp *adminq;
	int admin_timeout;
	//struct nvme_qp *local_ioq[MAX_LOCAL_IOQS];
	struct device_ioqm ioqm;
	struct nvme_qp **local_ioq;
	//struct list_head remote_iops_q[MAX_LOCAL_IOQS];
	struct list_head *remote_iops_q;
	struct mutex dev_lock;
	ulong wait_started;
	ulong wait_until;
	u32 async_result;
	struct work_struct async_work;
	struct delayed_work dwork;
	struct work_struct remove_work;
	bool remove_wip;
	struct list_head rm_all_link;

	u32 n_smart_reads;				// Counter of smart reads
	bool read_test_done;
	struct page *test_page;
	dma_addr_t test_dma;
	struct page *test_meta_page;
	dma_addr_t test_meta_dma;
	struct nvmeib_public_procfs_ent *proc_smart;
	struct nvmeib_public_procfs_ent *proc_log;
	struct completion reset_done;
	struct kref kref;

	char cname[16];
	struct miscdevice cdev;
	struct dma_pool *prp_page_pool;
	struct dma_pool *prp_small_pool;

	bool format_submitted;
	uint timeouted_formats;

	int max_completions;
	bool defer_process_io_cq;

	bool use_intr_shaper;
	//struct task_struct *thread[MAX_LOCAL_IOQS];
	//struct task_struct **thread;

	struct completion *free_drv_comp;
	spinlock_t free_drv_comp_lock;
};

char *nvmeibs_nvme_get_model(struct device_data *d)
{
	return d->model;
}

char *nvmeibs_nvme_get_native_serial(struct device_data *d)
{
	return d->serial;
}

ssize_t nvmeibs_nvme_print_qs(struct device_data *d, char *buffer, int len)
{
	int count = 0;

	NFIN;
	if (d)
		count += scnprintf(buffer + count, len - count,
			"\"max_ioqs\":%d,\n \"num_local_ioqs\":%d",
			d->max_ioqs, d->ioqm.n_virt_lioqs);

	NFOUT;
	return count;

}

#define CORE_SERVER_NVME_QPS_PROC_FRMT_VER 1
ssize_t nvmeibs_nvme_disk_qp_stats_fill(struct nvmeibs_disk_info *di,
										char *buf, int len)
{
#define BUF_ADD(...)	count += scnprintf(buf+count, len-count, __VA_ARGS__)
#define BUF_ADD_QP_NAME(__q, __idx)																			\
	do {																									\
		snprintf(qp_name, QPS_STATS_PAD_BLANKS_LEN_LINE_NUM + QPS_STATS_PAD_BLANKS_LEN_NAME,				\
				 "[%03d] %s (vec=%03d)", __idx, __q->interrupt_name, d->msix_entries[__idx].vector);	 	\
		/* no newline */																					\
		BUF_ADD("%-*s: ", QPS_STATS_PAD_BLANKS_LEN_LINE_NUM + QPS_STATS_PAD_BLANKS_LEN_NAME, qp_name);		\
	} while (0)

	struct device_data *d = di ? di->dev : NULL;
	struct nvme_qp *q;
	int qid, idx = 0;
	char qp_name[QPS_STATS_PAD_BLANKS_LEN_NAME + QPS_STATS_PAD_BLANKS_LEN_NAME + 1];
	ssize_t count = 0;
	NFIN;

	if (!d) return 0;

	BUF_ADD("*\n");

	/* ADMIN */
	BUF_ADD_QP_NAME(d->adminq, idx);
	count += nvmeib_qp_stats_fill(d->adminq->qp_stats, buf + count, len - count);
	idx++;

	/* IOs */
	for (qid = 0; qid < d->max_ioqs; ++qid) {
		if ((q = d->local_ioq[qid])) {
			BUF_ADD_QP_NAME(q, idx);
			count += nvmeib_qp_stats_fill(q->qp_stats, buf + count, len - count);
			idx++;
		}
	}

	/* Footer */
	count += nvmeib_add_proc_status_footer(buf+count, len-count, "qps");
	count += nvmeib_proc_add_txt_proc_epilog(CORE_SERVER_NVME_QPS_PROC_FRMT_VER, buf + count, len - count);

	NFOUT;
	return count;

#undef BUF_ADD_QP_NAME
#undef BUF_ADD
}

void nvmeibs_nvme_disk_qp_stats_reset(struct nvmeibs_disk_info *di) {
	struct nvme_qp *q;
	struct device_data *d = di ? di->dev : NULL;
	int qid;

	if (d) {
		nvmeib_qp_stats_reset(d->adminq->qp_stats);

		for (qid = 0; qid < d->max_ioqs; ++qid) {
			if ((q = d->local_ioq[qid]))
				nvmeib_qp_stats_reset(q->qp_stats);
		}
	}
}

#define dump_dev_state(_name_, __d__)                                     \
do {                                                              \
	_ND(_name_, "dev @SERIAL (@DEVICE_PTR) state: nr @NEED_RESET, rm @REMOVED, rp @RESET_PENDING)",                \
		__d__->serial, __d__,                                     \
		__d__->need_reset, __d__->removed, __d__->reset_pending); \
} while (0)

struct external_drive {
	struct nvmeibs_disk_info info;
#if KS_HAS_BDEV_FILE_OPEN_BY_PATH
	struct file *block_dev;
#elif KS_HAS_BDEV_OPEN_BY_PATH
	struct bdev_handle *block_dev;
#else
	struct block_device *block_dev;
#endif
	struct nvmeib_public_procfs_ent *proc_smart;
	bool frozen;
	u16 vendor;
	char model[NVMEIB_DISK_MAX_MODEL_STR_SIZE];
	struct external_drive *next;
	/* To control number of outstanding commands to external drive */
	struct completion done;
	struct kref done_kref;
};

static void done_kref_release(struct kref *done_kref)
{
    struct external_drive *p = container_of(done_kref, struct external_drive, done_kref);
	complete(&p->done);
}

static struct external_drive *external_drives;

static struct device_data *device_list = NULL;
static atomic_t total_pending;
static DECLARE_COMPLETION(scan_complete);
static struct task_struct *nvmeibs_kthread = NULL;
struct proc_dir_entry *nvmeibs_proc_dir = NULL;
struct proc_dir_entry *nvmeibs_proc_disks_dir = NULL;
static struct nvmeib_public_procfs_ent *nvmeof_proc;
static int next_seq = 0;

static DECLARE_RWSEM(global_lock);
static int local_major = 0;

u16 nvmeibs_nvme_get_vendor(const struct nvmeibs_disk_info *info)
{
	if (!info)
		return 0;
	if (info->dev)
		return info->dev->vendor;
	if (info->external)
		return info->external->vendor;
	return 0;
}


static int nvmeibs_open(struct BLK_MODE_OPEN_OBJ_T *bdev, BLK_MODE_T mode);
#if KS_HAS_BLKMODE
static void nvmeibs_release(struct gendisk *disk);
#elif KS_BLOCK_DEV_DEVICE_CLOSE_VOID
static void nvmeibs_release(struct gendisk *disk, BLK_MODE_T mode);
#else
static int nvmeibs_release(struct gendisk *disk, BLK_MODE_T mode);
#endif
static int nvmeibs_ioctl(struct block_device *bdev, fmode_t mode,
	unsigned int cmd, unsigned long arg);

static void free_qp(struct nvme_qp *q)
{
	struct device *dev = &q->dev->pci_dev->dev;
	unsigned long flags;
	_ND(trace_nvme_free_qp, "Try free_qp(id=@ID_INT) used_ids=@USED_IDS, state @STATE", q->id, q->used_ids,
	   q->state);
	spin_lock_irqsave(&q->q_lock, flags);

	//OM: add cond that remote_ioqs and bio_list are empty
	wait_event_interruptible_lock_irq_timeout(q->waiting,
			q->used_ids <= (q->id == 0 ? N_ASYNC_EVENTS : 0),
			q->q_lock, 2*nvmeibs_timeout);
	spin_unlock_irqrestore(&q->q_lock, flags);

	_ND(trace_1_nvme_free_qp, "Cont free_qp(id=@ID_INT) used_ids=@USED_IDS", q->id, q->used_ids);

	if (q->qp_stats)
		nvmeib_qp_stats_free(q->qp_stats);

	kfree(q->ids);
	if (q->sq)
		dma_free_coherent(dev, round_up(q->sq_len*sizeof(*q->sq), PAGE_SIZE),
					q->sq, q->sq_phys);
	if (q->cq)
		dma_free_coherent(dev, round_up(q->cq_len*sizeof(*q->cq), PAGE_SIZE),
					q->cq, q->cq_phys);
	if (q->ioqm_alloc_w)
		_NT(trace_2_nvme_free_qp, "qid @QID: pending work - shall be freed by work itself", q->id -1);
	kfree(q);
	_ND(trace_3_nvme_free_qp, "Done free_qp()");
}

static struct nvme_qp *alloc_qp(struct device_data *d, int len)
{
	struct nvme_qp *q;
	struct device *dev = &d->pci_dev->dev;

	q = kzalloc(sizeof(*q), GFP_KERNEL);
	if (q == NULL)
		return NULL;
 	q->dev = d;
	spin_lock_init(&q->q_lock);
	q->locking_cpu = -1;
	init_waitqueue_head(&q->waiting);

	q->cq = dma_alloc_coherent(dev, round_up(len*sizeof(*q->cq), PAGE_SIZE),
								&q->cq_phys, GFP_KERNEL);
	if (q->cq == NULL)
		goto out;
	q->cq_len = len;
	memset(q->cq, 0xcc, len*sizeof(*q->cq));

	q->sq = dma_alloc_coherent(dev, round_up(len*sizeof(*q->sq), PAGE_SIZE),
								&q->sq_phys, GFP_KERNEL);
	if (q->sq == NULL)
		goto out;
	q->sq_len = len;
	memset(q->sq, 0, len*sizeof(*q->sq));

	q->ids = kcalloc(len, sizeof(*q->ids), GFP_KERNEL);
	if (q->ids == NULL)
		goto out;
	q->total_ids = len-1;

	q->qp_stats = nvmeib_qp_stats_alloc();
	if (q->qp_stats == NULL)
		goto out;

	_ND(trace_nvme_alloc_qp, "allocated qp sq=@SQ(phys=@PHYS) cq=@CQ(phys=@PHYS)",
		q->sq, q->sq_phys, q->cq, q->cq_phys);
	return q;
out:
	free_qp(q);
	return NULL;
}

/*
 * IOQM Functions
 */

static int ioqm_alloc(struct device_data *d, int n)
{
	int rv = 0;
	NFIN;

	d->ioqm.virt_lioqs = kcalloc(n, sizeof(*d->ioqm.virt_lioqs) ,GFP_KERNEL);
	if (!d->ioqm.virt_lioqs)
		goto err_out;

	d->ioqm.free_client_qs = kcalloc(n, sizeof(*d->ioqm.free_client_qs) ,GFP_KERNEL);
	if (!d->ioqm.free_client_qs)
		goto err_free;

	goto out;

err_free:
	kfree(d->ioqm.virt_lioqs);
	d->ioqm.virt_lioqs = NULL;
err_out:
	rv = -1;
out:
	NFOUT;
	return rv;
}

static void ioqm_init(struct device_data *d)
{
	int ii;
	NFIN;

	d->ioqm.n_virt_lioqs = d->min_local_ioqs;
	for (ii = 0; ii < d->ioqm.n_virt_lioqs; ii++)
		d->ioqm.virt_lioqs[ii] = ii; /* ptr to physical qid */
	d->ioqm.n_free_client_qs = 0;
	d->ioqm.release_comp = NULL;
	d->ioqm.n_pending_reqs = 0;
	atomic_set(&d->ioqm.refcnt, 1);

	NFOUT;
}

static void ioqm_release(struct device_data *d)
{
	DECLARE_COMPLETION_ONSTACK(comp);
	int n;
	NFIN;

	if (atomic_read(&d->ioqm.refcnt) > 0) {
		d->ioqm.release_comp = &comp;
		if ((n = atomic_dec_return(&d->ioqm.refcnt))) {
			_NT(trace_nvme_ioqm_release, "dev @SERIAL (@DEVICE_PTR): wait for @REFCNT works to finish",
			   d->serial, d, n);
			wait_for_completion(d->ioqm.release_comp);
		}
		d->ioqm.release_comp = NULL;
	}
	else
		_ND(trace_1_nvme_ioqm_release, "ioqm release w/o prior init");
	NFOUT;
}

static void ioqm_free(struct device_data *d)
{
	NFIN;

	BUG_ON(atomic_read(&d->ioqm.refcnt));
	if (d->ioqm.virt_lioqs) {
		kfree(d->ioqm.virt_lioqs);
		d->ioqm.virt_lioqs = NULL;
	}
	if (d->ioqm.free_client_qs) {
		kfree(d->ioqm.free_client_qs);
		d->ioqm.free_client_qs = NULL;
	}

	NFOUT;
}

static bool nvmeibs_qid_hint = false;
module_param_named(qid_hint, nvmeibs_qid_hint, bool, 0644);
MODULE_PARM_DESC(qid_hint, "Send data on a channel per the CPU id, mainly relevant for SIW.");

static inline int ioqm_get_submit_qid(struct device_data *d, unsigned qid_hint_plus1)
{
	int vid = (unlikely(nvmeibs_qid_hint && qid_hint_plus1) ? qid_hint_plus1 - 1 : smp_processor_id()) % d->ioqm.n_virt_lioqs;
	int qid = d->ioqm.virt_lioqs[vid];
	_ND(trace_nvme_ioqm_get_submit_qid, "qid @QID (n_virt_lioqs @N_VIRT_LIOQS)", qid, d->ioqm.n_virt_lioqs);
	return qid;
}

static inline int ioqm_get_q_spin_lock(struct device_data *d, unsigned long *pflags)
{
	int qid = -1;
	struct nvme_qp *q;
	enum local_q_state q_state;

	NFIN;
	do {
		qid = ioqm_get_submit_qid(d, 0); /* 0 based */
		q = d->local_ioq[qid];
		spin_lock_irqsave(&q->q_lock, *pflags);
		if ((q_state = q->state) != LOCAL_Q_ON) {
			spin_unlock_irqrestore(&q->q_lock, *pflags);
			if (q_state >= LOCAL_Q_DYING) {
				return -1;
			}
			_NE(error_nvme_ioqm_get_q_spin_lock, "qid @QID (@QUEUE) not ON (state @STATE)", qid, q, q_state);
			qid = -1;
		}
	} while (qid < 0);

	NFOUT;
	return qid;
}

/* if submit-local-cmd is called from process-cq where q is already locked,
   dont attempt to lock. The code no-longer holds the lock for that case. */
static inline int ioqm_get_q_spin_lock_irqsave_recursive(struct device_data *d,
							ulong *pflags,
							bool *should_unlock, unsigned qid_hint_plus1)
{
	int qid;
	struct nvme_qp *q;
	bool should_lock;
	NFIN;

	do {

		qid = ioqm_get_submit_qid(d, qid_hint_plus1); /* 0 based */
		q = d->local_ioq[qid];
		should_lock = !irqs_disabled() || q->locking_cpu != smp_processor_id();
		if (should_lock) {
			spin_lock_irqsave(&q->q_lock, *pflags);
			q->locking_cpu = smp_processor_id();
		}
		if (q->state != LOCAL_Q_ON) {
			if (should_lock) {
				q->locking_cpu = -1;
				spin_unlock_irqrestore(&q->q_lock, *pflags);
			}
			if (q->state >= LOCAL_Q_DYING) {
				if (should_unlock)
					*should_unlock = false;
				return -1;
			}
			_NE(error_nvme_ioqm_get_q_spin_lock_irqsave_recursive, "qid @QID (@QUEUE) not ON (state @STATE)", qid, q, q->state);
			qid = -1;
		}
	} while (qid < 0);

	*should_unlock = should_lock;
	NFOUT;
	return qid;
}

static bool local_q_no_io_(struct nvme_qp *q)
{
	struct device_data *d = q->dev;
	int qid = q->id - 1;
	struct drive_params *drv;
	bool no_io = false;
	NFIN;

	BUG_ON(!spin_is_locked(&q->q_lock));
	if (q->used_ids > 0)
		goto out;
	if (!list_empty(&d->remote_iops_q[qid]))
		goto out;
	for (drv = d->drives; drv != NULL; drv = drv->next)
		if (bio_list_peek(&drv->bio_list[qid]))
			goto out;
	no_io = true;

out:
	NFOUT;
	return no_io;
}

/**
 * Find dev of @info, if dev is not being reset/removed,
 * try to inc dev's ioqm ref-cnt.
 * If succeeded, abandon-outstanding cant start until ref-cnt
 * goes to 0. This blocks device reset, remove and shutdown.
 */
static inline bool ioqm_ref_inc(u64 disk_handle,
								 struct nvmeibs_disk_info **info)
{
	struct device_data *d;
	struct drive_params *drv;
	NFIN;

	*info = NULL;
	down_read(&global_lock);
	for (d = device_list; d != NULL; d = d->next) {
		mutex_lock(&d->dev_lock);
		if (!(d->need_reset || d->reset_pending || d->removed)) {
			for (drv = d->drives; drv != NULL; drv = drv->next) {
				if (drv->gid == disk_handle) {
					/* paranoia check */
					if (drv->gid != drv->info.handle) {
						_NE(error_nvme_ioqm_ref_inc, "Oops: gid (#@GID_LLONG) != handle (#@HANDLE)",
						   drv->gid, drv->info.handle);
						BUG();
					}
					/* we use atomic-cnt instead of lock */
					if (atomic_inc_not_zero_hint(&d->ioqm.refcnt, 1)) {
						*info = &drv->info;
						_ND(trace_nvme_ioqm_ref_inc, "dev @SERIAL (@DEVICE_PTR): inc ref-cnt", d->serial, d);
					}
					else {
						_NE(error_1_nvme_ioqm_ref_inc, "dev @SERIAL (@DEVICE_PTR): could not inc ref-cnt although dev "
						   "rst/rm are not in progress", d->serial, d);
						BUG();
					}
					break;
				}
			}
		}
		mutex_unlock(&d->dev_lock);
		if (*info)
			break;
	}
	up_read(&global_lock);

	NFOUT;
	return !!(*info);
}

static inline void ioqm_ref_dec(struct device_data *d)
{
	int n;
	NFIN;

	if (!(n = atomic_dec_return(&d->ioqm.refcnt))) {
		BUG_ON(!d->ioqm.release_comp);
		complete(d->ioqm.release_comp);
	}
	else
		_NT(trace_nvme_ioqm_ref_dec, "dev @SERIAL (@DEVICE_PTR): @REFCNT works remained", d->serial, d, n);

	NFOUT;
}

static int ioqm_add_work(struct workqe_struct *work)
{
	return wq_add_work(ioqm_wq, work) ? 0 : -1;
}

#define initaite_dev_rst(_name_, __d__) \
do {                                  \
	_NT(_name_, "initiate dev rst ...");     \
	dump_dev_state(__debug__ ## _name_, __d__);  		  \
	__d__->need_reset = true;		  \
} while (0)

#define ioqm_initaite_dev_rst(_name_, __d__) initaite_dev_rst(_name_, __d__)

static int kthread_process_drive_cq(void *arg);
static inline int local_q_kthread_start_(struct nvme_qp *q)
{
	struct device_data *d = q->dev;
	int qid = q->id - 1;
	char thread_name[16];
	struct task_struct *t;
	int rv = -1;
	NFIN;

	/* Kthread name is only 16 chars in 3.10, so we try and take the head
	 * and tail... no guarantee that it is unique */

	BUG_ON(q->thread);
	snprintf(thread_name, sizeof thread_name, "np%02d%.3s-%5s%1d",
			 d->seq, d->serial, d->serial + 9, qid);
	init_completion(&q->th_ready);
	_NT(trace_nvme_local_q_kthread_start, "qid @QID: starting kthread...", qid);
	t = kthread_run(kthread_process_drive_cq, q, "%s", thread_name);
	if (!IS_ERR(t)) {
		wait_for_completion(&q->th_ready);
		q->thread = t;
		rv = 0;
	}
	else {
		_NE(error_nvme_local_q_kthread_start, "Failed to create thread for @THREAD_NAME, error=@ERROR_LONG",
		   thread_name, PTR_ERR(t));
	}

	NFOUT;
	return rv;
}

/**
 * @qid - the q id relative to local ioqs range (i.e. q->id - 1)
 * no done under q lock are
 */
static inline void local_q_kthread_stop(struct nvme_qp *q)
{
	int qid = q->id - 1;
	struct task_struct *t;
	ulong flags;
	NFIN;

	spin_lock_irqsave(&q->q_lock, flags);
	t = q->thread;
	q->thread = NULL;
	spin_unlock_irqrestore(&q->q_lock, flags);

	if (t) {
		_NT(trace_nvme_local_q_kthread_stop, "qid @QID: stopping kthread...", qid);
		kthread_stop(t);
	}

	NFOUT;
}

static irqreturn_t nvmeibs_intr(int irq, void *arg);
static inline int local_q_request_irq(struct nvme_qp *q)
{
	struct device_data *d = q->dev;
	int qid = q->id;
	int rv = -1;
	NFIN;

	BUG_ON(qid > nvmeibs_max_local_nvmeqs);
	if (q->irq_state == LOCAL_Q_IRQ_NONE) {
		_NT(trace_nvme_local_q_request_irq, "Request IRQ: Disk @SERIAL qid=@QID irq=@IRQ",
			d->serial, qid, d->msix_entries[qid].vector);
		snprintf(q->interrupt_name,
			sizeof q->interrupt_name, "nvme %d io%d", d->seq, qid);
		if ((rv = request_irq(d->msix_entries[qid].vector, nvmeibs_intr, 0,
				q->interrupt_name, q)) < 0) {
			_NE(error_nvme_local_q_request_irq, "Failed request_irq qid @QID, rv=@RV ", qid, rv);
		}
		else
			q->irq_state = LOCAL_Q_IRQ_ENABLE;
	}

	NFOUT;
	return rv;
}

static inline void local_q_free_irq(struct nvme_qp *q)
{
	struct device_data *d = q->dev;
	NFIN;

	if (q->irq_state != LOCAL_Q_IRQ_NONE) {
		_NT(trace_nvme_local_q_free_irq, "Free IRQ: Disk @SERIAL qid=@QID irq=@IRQ",
			d->serial, q->id, d->msix_entries[q->id].vector);
		free_irq(d->msix_entries[q->id].vector, q);
		q->irq_state = LOCAL_Q_IRQ_NONE;
	}

	NFOUT;
}

static inline void local_q_modify_irq(struct nvme_qp *q,
									  enum local_q_irq_state new_state)
{
	struct device_data *d = q->dev;
	NFIN;

	if (q->irq_state == LOCAL_Q_IRQ_NONE) {
		_NT(trace_nvme_local_q_modify_irq, "Cannot modify irq state (to @NEW_STATE), "
		   "request-irq() was not called", new_state);
		goto out;
	}
	if (new_state != q->irq_state) {
		//_NT(trace_1_nvme_local_q_modify_irq, "@OP_STR IRQ: Disk @SERIAL qid=@QID irq=@IRQ",
		//	new_state == LOCAL_Q_IRQ_ENABLE ? "Enable" : "Disable",
		//	d->serial, q->id, d->msix_entries[q->id].vector);
		if (new_state == LOCAL_Q_IRQ_ENABLE)
			enable_irq(d->msix_entries[q->id].vector);
		else if (new_state == LOCAL_Q_IRQ_DISABLE_NOSYNC)
			disable_irq_nosync(d->msix_entries[q->id].vector);
		else {
			_NE(error_nvme_local_q_modify_irq, "unknown irq state @NEW_STATE", new_state);
			BUG();
		}
		q->irq_state = new_state;
	}

out:
	NFOUT;
}

static inline int end_use_q(struct device_data *d, int qid);
static inline int end_use_local_q(struct nvme_qp *q, bool shutdown)
{
	struct device_data *d = q->dev;
	unsigned long flags;
	int rv = -1;
	NFIN;

	_NT(trace_nvme_end_use_local_q, "Disk @SERIAL, qid=@QID (state @IOQ_STATE_STR)",
		d->serial, q->id, ioq_state_str(q->state));

	spin_lock_irqsave(&q->q_lock, flags);
	if (!shutdown) {
		/* in this state intr-handler wont wakeup q's kthread
		and the q's kthread wont re-enabled q's irq. */
		BUG_ON(q->state != LOCAL_Q_LAST_COMP);
	} else {
		q->state = LOCAL_Q_DYING;
	}
	spin_unlock_irqrestore(&q->q_lock, flags);

	if (nvmeibs_use_nvme_kwq) {
		nvmeib_public_cancel_work_sync(&q->process_cq_work);
	} else {
		local_q_kthread_stop(q);
	}

	local_q_free_irq(q);

	/* destroy sq and cq */
	if (end_use_q(d, q->id) == -ETIMEDOUT) {
		_NT(trace_1_nvme_end_use_local_q, "Fail to destroy nvme qpair @ID_INT", q->id);
		goto out;
	}
	spin_lock_irqsave(&q->q_lock, flags);
	q->state = shutdown ? LOCAL_Q_DESTROYED : LOCAL_Q_OFF;
	spin_unlock_irqrestore(&q->q_lock, flags);
	rv = 0;

out:
	NFOUT;
	return rv;
}

/* IOQM: place holder
   if all ioqm's members are referenced from work, no need for locks */
static inline void ioqm_lock(struct device_data *d) {}
static inline void ioqm_unlock(struct device_data *d) {}
int use_client_q(struct nvmeibs_q_info *q);
int end_use_client_q(struct nvmeibs_q_info *q);

static void ioqm_q_alloc_work(struct workqe_struct *work)
{
	struct ioqm_q_alloc_workq *w =
		container_of(work, struct ioqm_q_alloc_workq, work);
	enum ioqm_q_alloc_state work_state = w->state;
	u64 disk_handle = w->disk_handle;
	int qid = w->qid;
	struct nvmeibs_disk_info *info = NULL;
	struct device_data *d;
	struct drive_params *drv __attribute__((unused));
	struct nvme_qp *q;
	bool free_w = true;
	NFIN;

	_NT(trace_nvme_ioqm_q_alloc_work, "w @OPR_PTR, w->qid @QID", w, w->qid);
	BUG_ON(work_state != IOQM_Q_ALLOC_START &&
		   work_state != IOQM_Q_ALLOC_NO_IO);

	/* get drv->info of disk-handle and (if its dev not rst/rm) inc ref-cnt */
	if (!ioqm_ref_inc(disk_handle, &info)) {
		_NT(trace_1_nvme_ioqm_q_alloc_work, "Fail to start ioq alloc handle #@DISK_HANDLE, not sending rsp to srv",\
		   disk_handle);
		goto out;
	}
	d = info->dev;
	drv = info->drv;
	/* !!! Dont take dev's lock after inc refcnt, abandon-outstanding !!!
	 * !!! might be waiting for us to finish while locking the dev.   !!! */

	ioqm_lock(d);
	if (work_state == IOQM_Q_ALLOC_START) {
		_NT(trace_2_nvme_ioqm_q_alloc_work, "work state START");
		if (d->ioqm.n_free_client_qs > 0) {
			d->ioqm.n_free_client_qs--;
			qid = d->ioqm.free_client_qs[d->ioqm.n_free_client_qs];
			work_state = IOQM_Q_ALLOC_DONE;
		}
		else if (d->ioqm.n_virt_lioqs > d->min_local_ioqs) {
			unsigned long flags;

			/*
			 * select a local-q to alloc for client
			 */
			d->ioqm.n_virt_lioqs--;
			qid = d->ioqm.virt_lioqs[d->ioqm.n_virt_lioqs];
			q = d->local_ioq[qid];
			_NT(trace_3_nvme_ioqm_q_alloc_work, "qid @QID (@QUEUE): initaite q-alloc", qid, q);

			/*
			 * start local-q shutdown sequence
			 */
			spin_lock_irqsave(&q->q_lock, flags);
			BUG_ON(q->state != LOCAL_Q_ON);
			q->state = LOCAL_Q_STOP_NEW_IO;
			if (local_q_no_io_(q)) {
				work_state = IOQM_Q_ALLOC_NO_IO;
				q->state = LOCAL_Q_LAST_COMP; /* if intr-handler just
				handled last-comp and/but woke up the q's polling thread
				now (althought there's no io), this prevent process-cq */
			}
			else {
				_NT(trace_4_nvme_ioqm_q_alloc_work, "qid @QID: wait for io-done (dec ref-cnt)", qid);
				BUG_ON(q->ioqm_alloc_w);
				w->qid = qid;
				q->ioqm_alloc_w = w;
				free_w = false;
				/* from here on, do not change @w */
			}
			spin_unlock_irqrestore(&q->q_lock, flags);
		}
		else {
			d->ioqm.n_pending_reqs++;
			_NT(trace_5_nvme_ioqm_q_alloc_work, "n_pending_reqs @N_PENDING_REQS", d->ioqm.n_pending_reqs);
			WARN_ON_ONCE(1);
			goto unlock;
		}
	}

	/* If @qid was local finish local-q shutdown sequence.
	   Then in client-q @qid and hand it to srv */
	BUG_ON(qid == -1);
	if (work_state == IOQM_Q_ALLOC_NO_IO) {
		_NT(trace_6_nvme_ioqm_q_alloc_work, "work state NO_IO");
		if (end_use_local_q(d->local_ioq[qid], false) < 0) {
			_NT(trace_7_nvme_ioqm_q_alloc_work, "Fail to off local-q (qid @QID)", qid);
			goto err;
		}
		work_state = IOQM_Q_ALLOC_DONE;
	}

	if (work_state == IOQM_Q_ALLOC_DONE) {
		_NT(trace_8_nvme_ioqm_q_alloc_work, "work state ALLOC_DONE");
		if (use_client_q(&d->qs_info[qid]) < 0) {
			_NT(trace_9_nvme_ioqm_q_alloc_work, "Fail to init client-q (qid @QID)", qid);
			goto err;
		}
		nvmeibs_disk_nvme_ioq_alloc_done(&d->qs_info[qid]);
	}
	goto unlock;

err:
	ioqm_initaite_dev_rst(trace_10_nvme_ioqm_q_alloc_work, d);
//	ioqm_initaite_dev_rst(d);

unlock:
	ioqm_unlock(d);
	ioqm_ref_dec(d);

out:
	if (free_w)
		kfree(w);
	NFOUT;
}

static int reinit_q(struct nvme_qp *q, bool full_init);
static void ioqm_q_free_work(struct workqe_struct *work)
{
	struct ioqm_q_free_workq *w =
		container_of(work, struct ioqm_q_free_workq, work);
	u64 disk_handle = w->disk_handle;
	int qid = w->qid - 1;
	struct nvmeibs_disk_info *info = NULL;
	struct device_data *d;
	struct drive_params *drv __attribute__((unused));
	NFIN;

	_NT(trace_nvme_ioqm_q_free_work, "w @OPR_PTR, w->qid @QID", w, w->qid);
	kfree(w);

	/* get drv->info of disk-handle and (if its dev not rst/rm) inc ref-cnt */
	if (!ioqm_ref_inc(disk_handle, &info)) {
		_NT(trace_1_nvme_ioqm_q_free_work, "Fail to start ioq free handle #@DISK_HANDLE", disk_handle);
		goto out;
	}
	d = info->dev;
	drv = info->drv;
	/* !!! Dont take dev's lock after inc refcnt, abandon-outstanding !!!
	 * !!! might be waiting for us to finish while locking the dev.   !!! */

	if (qid < d->min_local_ioqs || qid >= d->max_ioqs) {
		_NT(trace_2_nvme_ioqm_q_free_work, "Invalid qid (@QID) to free", qid);
		WARN_ON_ONCE(1);
		goto ref_dec;
	}

	ioqm_lock(d);
	BUG_ON(d->ioqm.n_virt_lioqs > d->max_local_ioqs);
	if (!d->qs_info[qid].inuse && d->local_ioq[qid]->state != LOCAL_Q_OFF) {
		_NE(error_nvme_ioqm_q_free_work, "qid @QID not owned by srv-layer (inuse @INUSE, state @IOQ_STATE_STR), bail", qid,
		   d->qs_info[qid].inuse, ioq_state_str(d->local_ioq[qid]->state));
		goto unlock;
	}
	if (end_use_client_q(&d->qs_info[qid]) == -ETIMEDOUT) {
		_NT(trace_3_nvme_ioqm_q_free_work, "Fail to destroy nvme qpair, qid @QID", qid);
		goto err;
	}

	if (!d->ioqm.n_pending_reqs) {
		struct nvme_qp *q = d->local_ioq[qid];
		BUG_ON(q->state != LOCAL_Q_OFF);
		if (d->ioqm.n_virt_lioqs < d->max_local_ioqs) {
			if (reinit_q(q, true) < 0) {
				_NT(trace_4_nvme_ioqm_q_free_work, "Fail to reinit local-q, qid @QID", qid);
				goto err;
			}
			d->ioqm.virt_lioqs[d->ioqm.n_virt_lioqs] = qid;
			d->ioqm.n_virt_lioqs++;
		}
		else {
			d->ioqm.free_client_qs[d->ioqm.n_free_client_qs] = qid;
			d->ioqm.n_free_client_qs++;
		}
	}
	else {
		struct nvmeibs_q_info *q = &d->qs_info[qid];
		if (use_client_q(q) < 0) {
			_NT(trace_5_nvme_ioqm_q_free_work, "Fail to init client-q, qid @QID", qid);
			goto err;
		}
		d->ioqm.n_pending_reqs--;
		nvmeibs_disk_nvme_ioq_alloc_done(q);
	}
	goto unlock;

err:
	ioqm_initaite_dev_rst(trace_6_nvme_ioqm_q_free_work, d);
//	ioqm_initaite_dev_rst(d);

unlock:
	ioqm_unlock(d);

ref_dec:
	ioqm_ref_dec(d);

out:
	NFOUT;
}

/**
 * The nvmeibs_nvme_ioq_.*() API functions are called by the
 * srv-layer. We dont rely on the srv-layer not to call this
 * API during remove-disk. On the other hand, we cant use the
 * dev lock in this context, Why? The nvmeibs_disk_nvme_.*()
 * API (add/remove-disk) are called by us under dev-lock.
 * The srv takes a lock within these APIs which it also takes
 * before calling our APIs. Thus, if we take the dev-lock in
 * this context (reversed order), we'll deadlock.
 *
 * Thus, ioq alloc and free are schduled on the the ioqm's
 * main wq where dev-lock can be safely acquired and allow
 * validating @info.
 */

/**
 * This function is called by the srv-layer to request
 * allocation of @n_qs io queues for the disk/drv @disk_handle.
 *
 * The srv-layer can call this function only if remove-disk
 * API was not called for drv->info of @disk_handle.
 *
 * Return: on success the number of submitted ioq-alloc requests
 *         or <0 on error
 */
int nvmeibs_nvme_ioq_alloc_request(u64 disk_handle, u32 n_qs)
{
	struct ioqm_q_alloc_workq *w;
	int ii;
	int rv;

	NFIN;

	if (!disk_handle) {
		_NT(trace_nvme_nvmeibs_nvme_ioq_alloc_request, "invalid disk_handle");
		rv = -EINVAL;
		goto out;
	}
	if (n_qs > 256) { /* sanity?! */
		_NT(trace_1_nvme_nvmeibs_nvme_ioq_alloc_request, "invalid n_qs @N_QS", n_qs);
		rv = -EINVAL;
		goto out;
	}

	rv = 0;
	for (ii = 0; ii < n_qs; ii++) {
		if (!(w = kzalloc(sizeof(*w), GFP_ATOMIC))) {
			_NT(trace_2_nvme_nvmeibs_nvme_ioq_alloc_request, "OOM: ioqm work (@II of @N_QS), handle #@DISK_HANDLE",
			   ii, n_qs, disk_handle);
			break;
		}
		WQ_INIT_WORK(&w->work, ioqm_q_alloc_work);
		w->disk_handle = disk_handle;
		w->state = IOQM_Q_ALLOC_START;
		w->qid = -1;
		if (ioqm_add_work(&w->work) < 0) {
			_NT(trace_3_nvme_nvmeibs_nvme_ioq_alloc_request, "Fail to add ioq-alloc work (@II of @N_QS), handle #@DISK_HANDLE",
			   ii, n_qs, disk_handle);
			kfree(w);
			break;
		}
		rv++;
	}

out:
	NFOUT;
	return rv;
}

/**
 * This function is called by the srv-layer to free client-q.
 * @qid is nvme ioq-id as given in nvmeibs_q_info->qid.
 *
 * The srv-layer can call this function only if remove-disk
 * API was not called for drv->info of @disk_handle.
 */
int nvmeibs_nvme_ioq_free(u64 disk_handle, u32 qid)
{
	struct ioqm_q_free_workq *w;
	int rv = -1;
	NFIN;

	if (!disk_handle) {
		_NT(trace_nvme_nvmeibs_nvme_ioq_free, "invalid disk_handle");
		rv = -EINVAL;
		goto out;
	}

	if (!(w = kzalloc(sizeof(*w), GFP_ATOMIC))) {
		_NT(trace_1_nvme_nvmeibs_nvme_ioq_free, "OOM: ioqm work (qid @QID)", qid);
		rv = -ENOMEM;
		goto out;
	}
	WQ_INIT_WORK(&w->work, ioqm_q_free_work);
	w->disk_handle = disk_handle;
	w->qid = qid;
	if ((rv = ioqm_add_work(&w->work)) < 0) {
		_NT(trace_2_nvme_nvmeibs_nvme_ioq_free, "Fail to add ioq-free work, handle #@DISK_HANDLE", disk_handle);
		kfree(w);
	}

out:
	NFOUT;
	return rv;
}

/**
 * This function resets @q, owned by a remote client, synchronously.
 * It does not lock the dev thus can run in caller's ctx.
 *
 * The srv-layer can call this function only before/during
 * nvme-layer called/calling its remove-disk API but not after
 * it had returned.
 */
static int nvme_client_q_reset(struct nvmeibs_q_info *q, bool reuse)
{
	struct device_data *d = q->disk->dev;
	int rv = -1;
	NFIN;

	if (d->need_reset || d->removed) {
		dump_dev_state(trace_nvme_nvme_client_q_reset, d);
//		dump_dev_state(d);
		goto out;
	}

	if (end_use_client_q(q) == -ETIMEDOUT)
		rv = -ETIMEDOUT;
	else if (reuse) {
		rv = use_client_q(q);
	}

out:
	NFOUT;
	return rv;
}

int nvmeibs_nvme_client_q_reset(struct nvmeibs_q_info *q)
{
	return nvme_client_q_reset(q, true);
}

int nvmeibs_nvme_client_q_destroy(struct nvmeibs_q_info *q)
{
	return nvme_client_q_reset(q, false);
}

/* -------------------------------------------------------------------------- *
 *                                                                            *
 * -------------------------------------------------------------------------- */

static inline void nvme_add_disk(struct drive_params *drv)
{
	NFIN;
	drv->info.add_jif = jiffies;
	drv->gid = drv->info.handle = get_guid();
	_NT(trace_nvme_nvme_add_disk, "Add disk to srv-layer, handle @HANDLE", drv->info.handle);
	if(nvmeibs_disk_nvme_add_disk(&drv->info) != 0)
		drv->gid = 0;
	NFOUT;
}

static inline void nvme_remove_disk(struct drive_params *drv)
{
	NFIN;
	if (drv->gid != 0) {
		/* 1) invalidate QUEUED (ioqm) requests from srv in case
		 *    they are scheduled after dev is back from reset/remove
		 * 2) remove-disk only if added && not removed yet.
		 */
		drv->gid = 0;
		_NT(trace_nvme_nvme_remove_disk, "Remove disk to srv-layer, handle @HANDLE", drv->info.handle);
		nvmeibs_disk_nvme_remove_disk(&drv->info);
	}
	NFOUT;
}

static inline bool is_cq_empty(struct nvme_qp *q)
{
	struct nvme_completion *cqp = &q->cq[q->cq_head];

	return (le16_to_cpu(cqp->status) & 1) == q->cq_phase;
}

static int nvmeibs_process_cq(struct nvme_qp *q)
{
	struct nvme_completion *cqp;
	unsigned cmdid;
	struct req_id volatile *rqp;
	int num_handled;
	nvme_callback_t *callback = 0;
	int status = 0;  /* GCC */
	u32 result = 0;  /* GCC */
	void *arg = NULL;  /* GCC */
	int n = 0;
	int d_max_completions = q->dev->max_completions;
	bool in_interrupt = in_interrupt();

	if (q->state != LOCAL_Q_ON && q->state != LOCAL_Q_STOP_NEW_IO) {
		_NT(trace_nvme_nvmeibs_process_cq, "qid @QID (@QUEUE), local q state is @STATE", q->id-1 , q, q->state);
		num_handled = 0;
		goto out;
	}

	nvmeib_qp_stats_on_poll_cq(q->qp_stats, 0, recv);

	q->locking_cpu = smp_processor_id();
	for (num_handled = 0; num_handled < d_max_completions || d_max_completions == 0; num_handled++) {
		if (!in_interrupt) {
			nvmeib_completion_noise_start(NVMEIB_NOISE_COMPLETION);
		}
		cqp = &q->cq[q->cq_head];
		if (is_cq_empty(q)) {
			nvmeib_qp_stats_on_poll_cq_empty(q->qp_stats);
			break;
		}
		if (le16_to_cpu(cqp->sq_id) != q->id) {
			_NE(error_nvme_nvmeibs_process_cq, "completion sq (@LE16_TO_CPU) != @ID_INT", le16_to_cpu(cqp->sq_id), q->id);
			goto cont;
		}
		q->sq_head = le16_to_cpu(cqp->sq_head); /* reclaim sq entries */
		cmdid = le16_to_cpu(cqp->command_id);
		if (cmdid >= q->total_ids) {
			_NE(error_1_nvme_nvmeibs_process_cq, "Illegal id @CMDID", cmdid);
			goto cont;
		}
		/* clear id, marke as not in use.
		   (test becasue we may have marked it due to timeout) */
		if (test_and_clear_bit(cmdid, q->id_bitmap)) {
			rqp = &q->ids[cmdid];
/*			(*rqp->callback)(rqp->arg, le16_to_cpu(cqp->status) >> 1,
							le32_to_cpu(cqp->result)); */
			callback = rqp->callback;
			arg = rqp->arg;
			status = le16_to_cpu(cqp->status) >> 1;
			result = le32_to_cpu(cqp->result.u32);
			rqp->callback = NULL;
			--q->used_ids;
			n++;

			/* check if this q should be given to the client */
			if (q->ioqm_alloc_w && local_q_no_io_(q)) {
				struct ioqm_q_alloc_workq *w = q->ioqm_alloc_w;
				BUG_ON(q->state != LOCAL_Q_STOP_NEW_IO);
				q->state = LOCAL_Q_LAST_COMP;
				q->ioqm_alloc_w = NULL;
				w->state = IOQM_Q_ALLOC_NO_IO;
				if (ioqm_add_work(&w->work) < 0) {
					_NT(trace_1_nvme_nvmeibs_process_cq, "Fail to add ioq-alloc work, q @QUEUE, dev @DEV", q, q->dev);
					kfree(w);
					ioqm_initaite_dev_rst(trace_2_nvme_nvmeibs_process_cq, q->dev);
//					ioqm_initaite_dev_rst(q->dev);
				}
			}
		}
		cont:
		if (++q->cq_head == q->cq_len) {
			q->cq_head = 0;
			q->cq_phase ^= 1;
		}
		if (!nvmeibs_nvme_doorbell_batch) {
			writel(q->cq_head, q->cq_doorbell);
		}
		if (callback) {
			q->locking_cpu = -1;
			spin_unlock(&q->q_lock); /* irqs stay disabled */
			(*callback)(arg, status, result);
			spin_lock(&q->q_lock);
			q->locking_cpu = smp_processor_id();
			callback = 0;
		}
	}

	if (num_handled != 0) {
		wake_up(&q->waiting);
		if (nvmeibs_nvme_doorbell_batch && q->old_cq_head != q->cq_head) {
			/* as we unlock the q_lock when running the callback other thread/intterupts can update the cq_head before us */
			writel(q->cq_head, q->cq_doorbell);
			q->old_cq_head = q->cq_head;
		}
		if (q->complete_fn != NULL)
			(*q->complete_fn)(q);
	}

	q->locking_cpu = -1;

	nvmeib_qp_stats_on_update_cqes(q->qp_stats, 0, n);

out:
	return num_handled;
}

extern struct nvmeib_intr_shaper *s_intr_shaper;

static irqreturn_t nvmeibs_intr(int irq, void *arg)
{
	struct nvme_qp *q = (struct nvme_qp *)arg;
	struct device_data *d = q->dev;
	int d_max_completions = d->max_completions;
	bool d_use_intr_shaper = d->use_intr_shaper;
	int d_defer_process_io_cq = d->adminq != q ? (d->defer_process_io_cq) : 0;
	int num_handled = 0;
	static long last_time = 0;
	bool offload_enabled = (q->thread || nvmeibs_use_nvme_kwq) && d->adminq != q;

	nvmeib_intr_shaper_intr_enter(s_intr_shaper, INTR_SHAPER_INTR_TYPE_SERVER_NVME);
	nvmeib_completion_noise_start(NVMEIB_NOISE_INTERRUPT);
	/* interrupt shaper is on and not admin cq and offload is enabled */
	if (!d_defer_process_io_cq && d_use_intr_shaper && offload_enabled) {
		d_defer_process_io_cq = nvmeib_intr_shaper_intr_should_wake_up(s_intr_shaper);
	}

	if (q->irq_debug == 2) {
		q->irq_debug = 0;
		_ND(trace_nvme_nvmeibs_intr, "Got local IRQ again");
	}
	spin_lock(&q->q_lock);
	nvmeib_qp_stats_on_interrupt(q->qp_stats);
	if (!d_defer_process_io_cq) {
		num_handled = nvmeibs_process_cq(q);
		if (d_use_intr_shaper && offload_enabled && !is_cq_empty(q)) {
			nvmeib_intr_shaper_intr_polled(s_intr_shaper, num_handled);
			d_defer_process_io_cq = nvmeib_intr_shaper_intr_should_wake_up(s_intr_shaper);
		}
	}

	/* cq is still not empty or defer process io cq is enabled */
	if (((num_handled == d_max_completions && d_max_completions) || d_defer_process_io_cq)) {
		//_ND(trace_nvme_nvmeibs_intr_offload_sched, "Offload sched serial=@SERIAL qid=@QID is_admin=@BOOL", d->serial, q->id, d->adminq == q);
		nvmeib_qp_stats_on_offload_sched(q->qp_stats);
		if (nvmeibs_use_nvme_kwq) {
			queue_work(nvmeibs_nvme_wq, &q->process_cq_work);
		} else {
			local_q_modify_irq(q, LOCAL_Q_IRQ_DISABLE_NOSYNC);
			/* Set polling=true before wake_up_process to ensure the woken thread sees it.
			 * The smp_mb__before_atomic in wake_up_process provides the necessary barrier. */
			WRITE_ONCE(q->polling, true);
			/* Ensure the polling mode is visible before waking up the thread */
			smp_mb();
			wake_up_process(q->thread);
		}
		if (q->irq_debug < 1 || jiffies > last_time + 5 * HZ) {
			last_time = jiffies;
			q->irq_debug = 1;
			_ND(trace_1_nvme_nvmeibs_intr, "nvme@SEQ (@SERIAL): Switch local IRQ to thread", d->seq, d->serial);
		}
	}
	spin_unlock(&q->q_lock);
	nvmeib_completion_noise_end(NVMEIB_NOISE_INTERRUPT, NULL, 0, NVMEIB_NOISE_CTRS_NVMEIBS_INTR);

	nvmeib_intr_shaper_intr_exit(s_intr_shaper);
	/* Always return IRQ_HANDLED to avoid "nobody cared" errors.
	 * We may get spurious interrupts due to race conditions when switching
	 * between interrupt and polling modes (disable_irq_nosync doesn't wait
	 * for in-flight interrupts to be masked by hardware). */
	return IRQ_HANDLED;
}

struct cmd_result {
	struct completion completion;
	int status;
	u32 result;
};

static void submit_cmd(struct nvme_qp *q)
{
	writel(q->sq_tail, q->sq_doorbell);
}

static ulong wait_with_pauses(struct device_data *dev, struct completion *comp,
							 ulong timeout, ulong pause)
{
	bool wait;
	int i = 0;
	ulong loops, w_rv =0, total_timeout = timeout, total_wait = 0;
	loops = DIV_ROUND_UP(timeout, pause);
	timeout = min(timeout, pause);

	do {
		wait = false;
		w_rv = wait_for_completion_timeout(comp, timeout);
		if (!w_rv && ++i < loops) {
			wait = true;
			_NT(wait_with_pauses_done_loop_f, "@SERIAL Done loop @INT/@LU with w_rv=@LU, retry=@BOOL, timeout=@LU,"
												" need_reset: @BOOL, reset_pending: @BOOL, removed: @BOOL",
												dev->serial, i, loops, w_rv, wait, timeout,
												dev->need_reset, dev->reset_pending, dev->removed);
			total_wait += timeout;
		}
	} while (wait);

	if (!w_rv) {
		_NE(wait_with_pauses_done_loop_d, "Timeout, total wait time @LU secs (i=@INT)", total_wait/HZ, i);
		return 0;
	}

	return total_timeout - total_wait;
}

#define PCI_CONFIG_STS	6
/* This function starts with q_lock being held - by inside get_cmd()
 * and return with the lock released.
 * Ugly, but this is how it was written
 */
static void nvmeibs_submit_wait(struct nvme_qp *qp, struct nvme_command *cmd,
				struct cmd_result *result, unsigned long *pflags)
{
	u16 command_id = cmd->common.command_id;
	u16 sts;
	ulong timeout = submit_wait_timeout;
	ulong wait_rv, wait_dt, pause = timeout;
	struct device_data *dev = qp->dev;
	result->status = -ETIMEDOUT;

	dev->n_smart_reads++;
	if (dev->need_reset || dev->reset_pending || dev->removed ||
			dev->format_submitted) {
		/* disk either needs a reset or is in the middle of one so don't submit cmd */
		dump_dev_state(trace_nvme_nvmeibs_submit_wait, qp->dev);
//		dump_dev_state(qp->dev);
		if (!dev->format_submitted)
			/* prevent flodding */
			_NT(trace_1_nvme_nvmeibs_submit_wait,
			 "Disk @SERIAL need_reset=@NEED_RESET reset_pending=@RESET_PENDING format_submitted=@BOOL",
			  dev->serial, dev->need_reset, dev->reset_pending, dev->format_submitted);
		if(test_and_clear_bit(command_id, qp->id_bitmap)) {
			struct req_id *rqp = &qp->ids[command_id];
			--qp->used_ids;
			(*rqp->callback)(rqp->arg, NVME_SC_NS_NOT_READY,
							le32_to_cpu(0));
			rqp->callback = NULL;
			qp->sq_tail = (qp->sq_tail ? : qp->sq_len) - 1;
		}
		spin_unlock_irqrestore(&qp->q_lock, *pflags);
		return;
	}

	if (qp == qp->dev->adminq) {
		if (cmd->common.opcode == nvme_admin_format_nvm) {
			timeout = !dev->timeouted_formats? NVMEIBS_FORMAT_TIMEOUT_SECONDS_FIRST_TRY * HZ:
											nvmeibs_format_timeout_seconds_second_try * HZ;
			pause = 20 * HZ;
			dev->format_submitted = true;
		}
		else
			pause = timeout = qp->dev->admin_timeout;
	}
	_NT(trace_0_nvme_nvmeibs_submit_wait,
		"Submitting to Disk @SERIAL, admin cmd=@HEX08, wait timeout will be @DURATION(HZ=@INT)",
		qp->dev->serial, cmd->common.opcode, timeout, HZ);

	submit_cmd(qp);
	spin_unlock_irqrestore(&qp->q_lock, *pflags);

	wait_rv = wait_with_pauses(dev, &result->completion, timeout, pause);
	if (cmd->common.opcode == nvme_admin_format_nvm) {
		dev->format_submitted = false;
		if (wait_rv == 0)
			dev->timeouted_formats++;
	}
	wait_dt = timeout - wait_rv;
	if (wait_dt > (5 * HZ)) {
		_NW(warn_0_nvme_nvmeibs_submit_wait,
			"dev @SERIAL (@DEVICE_PTR), cmd (@HEX08) took @DURATION, "
			"timeout=@DURATION",
			qp->dev->serial, qp->dev,
			(qp == qp->dev->adminq) ? cmd->common.opcode : -1, wait_dt, timeout);
	}
	if (wait_rv == 0) {
	/* Simulate an interrupt if timed-out */
		spin_lock_irqsave(&qp->q_lock, *pflags);
		nvmeib_qp_stats_on_offth_iter(qp->qp_stats);
		(void) nvmeibs_process_cq(qp);
		if (result->status == -ETIMEDOUT) {
			qp->ids[command_id].callback = NULL;
			if(test_and_clear_bit(command_id, qp->id_bitmap)) {
				--qp->used_ids;
			}
			initaite_dev_rst(trace_dev_rst_nvme_nvmeibs_submit_wait, qp->dev);
			qp->dev->admin_timeout += submit_wait_timeout;
		}
		spin_unlock_irqrestore(&qp->q_lock, *pflags);
		pci_read_config_word(qp->dev->pci_dev, PCI_CONFIG_STS, &sts);
		if (qp->dev->need_reset) {
			pr_warn("nvmeibs: timedout csts=0x%x pci_sts=0x%x nvme%d:"
					" cmd=%d cmdid=%d\n",
				readl(&qp->dev->mmio->csts), sts,
				qp->dev->seq, cmd->common.opcode, command_id);
		}
	}
}

/* Will hold q_lock after returned (except for error flow) */
static struct nvme_command *get_cmd(struct nvme_qp *q,
									nvme_callback_t *callback,
									void *arg,
									unsigned long *pflags)
{
	int id;
	struct nvme_command *cmd;
	int err;

	spin_lock_irqsave(&q->q_lock, *pflags);
	err = wait_event_interruptible_lock_irq_timeout(q->waiting,
			(q->sq_tail + 1 != (q->sq_head ? : q->sq_len)) &&
			(q->used_ids < q->total_ids),
		q->q_lock, nvmeibs_timeout);

	if (err <= 0) {
		_NT(trace_nvme_get_cmd, "Disk @SERIAL: sq_tail=@SQ_TAIL, sq_head=@SQ_HEAD, sq_len=@SQ_LEN, used_ids=@USED_IDS, total_ids=@TOTAL_IDS, id=@ID_INT",
		   q->dev->serial, q->sq_tail, q->sq_head, q->sq_len, q->used_ids, q->total_ids, q->id);
		_NE(error_nvme_get_cmd, "wait_event...() returned @ERR", err);
		goto out;
	}

	id = find_first_zero_bit(q->id_bitmap, q->total_ids);
	BUG_ON(id >= q->total_ids);
	++q->used_ids;
	set_bit(id, q->id_bitmap);
	q->ids[id].callback = callback;
	q->ids[id].arg = arg;
	q->ids[id].issue_time = jiffies + q->used_ids * HZ;
// Allow 1 extra second before timeout for every command in front.
	q->ids[id].aborted = false;
	cmd = &q->sq[q->sq_tail];
	if (++q->sq_tail == q->sq_len)
		q->sq_tail = 0;
	memset(cmd, 0, sizeof(*cmd));
	cmd->common.command_id = id;
	return cmd;
out:
	spin_unlock_irqrestore(&q->q_lock, *pflags);
	return NULL;
}

static void admin_cmd_done(void *arg, int status, u32 result)
{
	struct cmd_result *r = (struct cmd_result *)arg;

	if (r == NULL) {
		_NE(t07_nsnvme, "nvmeibs: completion with null result");
		return;
	}
	r->status = status;
	r->result = result;
	complete(&r->completion);
}

/* Identify drive or namespace */
static int get_ident(struct device_data *d, u32 nsid, dma_addr_t buf)
{
	struct cmd_result result;
	struct nvme_command *cmd;
	unsigned long flags;

	init_completion(&result.completion);
	cmd = get_cmd(d->adminq, admin_cmd_done, &result, &flags);
	if (cmd == NULL)
		return -ENOMEM;

	cmd->common.opcode = nvme_admin_identify;
	cmd->identify.nsid = cpu_to_le32(nsid);
	cmd->identify.cns = cpu_to_le32(nsid == 0 ? 1 : 0);
	cmd->identify.dptr_prp1 = cpu_to_le64(buf);
	if ((buf & ~PAGE_MASK) != ((buf+4095) & ~PAGE_MASK))
		cmd->identify.dptr_prp2 = cpu_to_le64((buf+4096) & PAGE_MASK);

	nvmeibs_submit_wait(d->adminq, cmd, &result, &flags);

	if (result.status != 0)
		_NW(warn_nvme_get_ident, "Identify(ns=@NSID) status = @STATUS", nsid, result.status);
	return result.status;
}

/* Get a simple feature, buffer output is not supported here */
static int __attribute__ ((unused))
get_feature(struct device_data *d, int nsid, int feature)
{
	struct cmd_result result;
	struct nvme_command *cmd;
	unsigned long flags;

	init_completion(&result.completion);
	cmd = get_cmd(d->adminq, admin_cmd_done, &result, &flags);
	if (cmd == NULL)
		return -ENOMEM;

	cmd->common.opcode = nvme_admin_get_features;
	cmd->features.nsid = cpu_to_le32(nsid);
	cmd->features.fid = cpu_to_le32(feature);

	nvmeibs_submit_wait(d->adminq, cmd, &result, &flags);

	if (result.status != 0)
		_NW(warn_nvme_get_feature, "get feature(@FEATURE) status = @STATUS", feature, result.status);
	return result.result;
}

/* Set a simple feature, buffer input is not supported here */
static int set_feature(struct device_data *d, u32 nsid, int feature, u32 value)
{
	struct cmd_result result;
	struct nvme_command *cmd;
	unsigned long flags;

	init_completion(&result.completion);
	cmd = get_cmd(d->adminq, admin_cmd_done, &result, &flags);
	if (cmd == NULL)
		return -ENOMEM;

	cmd->common.opcode = nvme_admin_set_features;
	cmd->features.nsid = cpu_to_le32(nsid);
	cmd->features.fid = cpu_to_le32(feature);
	cmd->features.dword11 = cpu_to_le32(value);

	nvmeibs_submit_wait(d->adminq, cmd, &result, &flags);

	if (result.status != 0) {
		if (feature == NVME_FEAT_NUM_QUEUES)
			result.result = 0; /* num IO CQ/SQ allocated */
		_NW(warn_nvme_set_feature, "set feature(@FEATURE) status = @STATUS, result = @RESULT_INT",
			feature, result.status, result.result);
	}
	return result.result;
}

static int create_q(struct device_data *d,
		int op, int qid, u64 addr, int size, int flags, int ref)
{
	struct cmd_result result;
	struct nvme_command *cmd;
	unsigned long irqflags;

	init_completion(&result.completion);
	cmd = get_cmd(d->adminq, admin_cmd_done, &result, &irqflags);
	if (cmd == NULL)
		return -ETIMEDOUT;

	cmd->common.opcode = op;
	cmd->create_cq.cqid = cpu_to_le16(qid);
	cmd->create_cq.qsize = cpu_to_le16(size-1);
	cmd->create_cq.cq_flags = cpu_to_le16(flags);
	cmd->create_cq.irq_vector = cpu_to_le16(ref);
	cmd->create_cq.prp1 = cpu_to_le64(addr);

	nvmeibs_submit_wait(d->adminq, cmd, &result, &irqflags);
	if (unlikely(result.status != 0)) {
		_NE(error_nvme_create_q, "Q alloc error @STATUS", result.status);
		return -EIO;
	}
	return 0;
}

static inline int create_cq(
	struct device_data *d, int cqid, u64 addr, int size, int irq)
{
	return create_q(d, nvme_admin_create_cq, cqid, addr, size,
			NVME_QUEUE_PHYS_CONTIG | NVME_CQ_IRQ_ENABLED, irq);
}

static inline int create_sq(
	struct device_data *d, int sqid, u64 addr, int size, int cqid)
{
	return create_q(d, nvme_admin_create_sq, sqid, addr, size,
			NVME_QUEUE_PHYS_CONTIG | NVME_SQ_PRIO_HIGH, cqid);
}

static int destroy_q(struct device_data *d, int op, int qid)
{
	struct cmd_result result;
	struct nvme_command *cmd;
	unsigned long flags;

	init_completion(&result.completion);
	cmd = get_cmd(d->adminq, admin_cmd_done, &result, &flags);
	if (cmd == NULL)
		return -ETIMEDOUT;

	cmd->common.opcode = op;
	cmd->delete_queue.qid = cpu_to_le16(qid);

	nvmeibs_submit_wait(d->adminq, cmd, &result, &flags);
	return result.status;
}

static inline int destroy_cq(struct device_data *d, int qid)
{
	return destroy_q(d, nvme_admin_delete_cq, qid);
}

static inline int destroy_sq(struct device_data *d, int qid)
{
	return destroy_q(d, nvme_admin_delete_sq, qid);
}

static int abort_cmd(struct device_data *d, int qid, int cmdid)
{
	struct cmd_result result;
	struct nvme_command *cmd;
	unsigned long flags;

	init_completion(&result.completion);
	cmd = get_cmd(d->adminq, admin_cmd_done, &result, &flags);
	if (cmd == NULL)
		return -ENOMEM;

	cmd->common.opcode = nvme_admin_abort_cmd;
	cmd->abort.sqid = cpu_to_le16(qid);
	cmd->abort.cid = cpu_to_le16(cmdid);

	nvmeibs_submit_wait(d->adminq, cmd, &result, &flags);
	return result.status;
}

static int get_log(struct device_data *d, u8 logpage, u32 nsid, dma_addr_t buf,
					ssize_t len)
{
	struct cmd_result result;
	struct nvme_command *cmd;
	unsigned long flags;

	init_completion(&result.completion);
	cmd = get_cmd(d->adminq, admin_cmd_done, &result, &flags);
	if (cmd == NULL)
		return -ENOMEM;

	cmd->common.opcode = nvme_admin_get_log_page;
	cmd->common.nsid = nsid;
	cmd->common.cdw10[0] = cpu_to_le32(((len << 14) | logpage) - (1 << 16));
	cmd->common.dptr_prp1 = cpu_to_le64(buf);
	if ((buf & ~PAGE_MASK) != ((buf+4095) & ~PAGE_MASK))
		cmd->common.dptr_prp2 = cpu_to_le64((buf+4096) & PAGE_MASK);

	nvmeibs_submit_wait(d->adminq, cmd, &result, &flags);

	if (result.status != 0)
		_NT(trace_nvme_get_log, "getlog() status = @STATUS", result.status);
	return result.status;
}

static int atoi(const char *s)
{
	int res = 0;
	while ('0' <= *s && *s <= '9')
		res = 10*res + *s++ - '0';
	return res;
}

/* This function is called only if gcp_mode is true */
static int set_str_by_gcp_uuid(struct drive_params *drv)
{
	int i = 0, rv = -ENOENT;
	const char *dev_name_str = dev_name(&drv->dev->pci_dev->dev);
	int name_len;
	char *p;

	name_len = strlen(dev_name_str);

	if (gcp_num_drives == 0) {
		_NE_dmesg(set_str_by_gcp_uuid_err1, "GCP: No drives were specified in gcp_drives_to_uuid_list!");
		goto out;
	}

	//TODO: Change this to use sscanf with the use of the delimiter ";"
	for (i = 0; i < gcp_num_drives; i++) {
		if (!strncmp(gcp_drives_to_uuid_list[i], dev_name_str, name_len) && atoi(gcp_drives_to_uuid_list[i] + name_len + 1) == drv->nsid) {
			p = gcp_drives_to_uuid_list[i] + name_len + 1;
			while ('0' <= *p && *p <= '9') {
				++p;
			}
			_NT(gcp_uuid_match_found, "GCP: Found match, BDF: @STR NSID: @INT UUID: @STR", dev_name_str, drv->nsid, p + 1);
			if (strnlen(p + 1, sizeof(drv->id_str)) == sizeof(drv->id_str)) {
				/* Too long! Need it to be < sizeof(drv->id_str) */
				/* strnlen will go over the string up to sizeof(drv->id_str), or it hits null. As we should have the '\0' at the end if we fit correctly.
				   if the len equals sizeof(drv->id_str) this means strlen didn't find the null, and its too long*/
				_NE_dmesg(set_str_by_gcp_uuid_err2, "GCP: UUID provided was too long @STR!", p + 1);
				rv = -EINVAL;
				goto prnt_content;
			}
			strncpy(drv->id_str, p + 1, sizeof(drv->id_str));
			rv = 0;
			goto out;
		}
	}
prnt_content:
	if (rv) {
		_NE_dmesg(set_str_by_gcp_uuid_err5, "GCP: No match to BDF: @STR NSID: @INT! rv = @INT", dev_name_str, drv->nsid, rv);
		_NE_dmesg(set_str_by_gcp_uuid_err3, "gcp_drives_to_uuid_list content:");
		for (i = 0; i < gcp_num_drives; i++) {
			_NE_dmesg(set_str_by_gcp_uuid_err4, "@STR", gcp_drives_to_uuid_list[i]);
		}
		goto out;
	}
out:
	return rv;
}

static int set_drv_id_str(struct drive_params *drv)
{
	struct device_data *d = drv->dev;
	int rv = 0;
	if (nvmeibs_gcp_mode) {
		rv = set_str_by_gcp_uuid(drv);
		goto out;
	}
	snprintf(drv->id_str, 32, "%.*s.%d", d->ser_len, d->serial, drv->nsid);
out:
	return rv;
}

static int get_device_params(struct device_data *d)
{
	struct device *dev = &d->pci_dev->dev;
	u32 nn, nsid;
	struct drive_params *drv, **pdrv;
	struct nvme_lbaf format;
	int ser_len;
	void *ident_buf;
	dma_addr_t ident_phys;
	int err;
	struct nvme_id_ns *id_ns;
	struct nvme_id_ctrl *id_ctrl;

	ident_buf = dma_alloc_coherent(dev, 4096, &ident_phys, GFP_KERNEL);
	if (ident_buf == NULL)
		return -ENOMEM;
	if ((err = get_ident(d, 0, ident_phys)) != 0)
		goto out;
	id_ctrl = ident_buf;
	if (fake_serial != NULL)
		ser_len = nvmeib_copy_str_trim_spaces(d->serial, fake_serial, sizeof(d->serial), NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE_SPEC);
	else
		ser_len = nvmeib_copy_str_trim_spaces(d->serial, id_ctrl->sn, sizeof(d->serial), NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE_SPEC);
	nvmeib_copy_str_trim_spaces(d->model,  id_ctrl->mn, sizeof(d->model) , NVMEIB_DISK_MAX_MODEL_STR_SIZE);
	d->vendor = le16_to_cpu(id_ctrl->vid);
	d->cntlid = le16_to_cpu(id_ctrl->cntlid);
	d->ser_len = ser_len;
	if (id_ctrl->mdts != 0)
		d->max_transfer = 1l << (id_ctrl->mdts + NVME_CAP_MPSMIN(d->cap) + 12);

	if (nvmeibs_cap_transfer_size) {
		_NI(trace_nvme_get_device_params, "Capping max_transfer to @TRANSFER", NVMEIBS_CAP_MAX_TRANSFER_SIZE);
		if (!d->max_transfer || d->max_transfer > NVMEIBS_CAP_MAX_TRANSFER_SIZE)
			d->max_transfer = NVMEIBS_CAP_MAX_TRANSFER_SIZE;
	}

	if ((d->pci_dev->vendor == PCI_VENDOR_ID_INTEL) &&
						(d->pci_dev->device == 0x0953) && id_ctrl->vs[3] != 0)
		d->alignment_size = 1 << (NVME_CAP_MPSMIN(d->cap) + id_ctrl->vs[3]);

	_ND(trace_1_nvme_get_device_params, "Private[3] = 0x@DB", id_ctrl->vs[3]);
	_ND(trace_2_nvme_get_device_params, "async event limit AERL=@AERL", id_ctrl->aerl);
	d->n_async = min(id_ctrl->aerl+1, N_ASYNC_EVENTS);
	d->err_log_size = (id_ctrl->elpe+1) * 64;
	d->max_abort = id_ctrl->acl + 1;
	d->trim_supported = (id_ctrl->oncs & NVME_CTRL_ONCS_DSM) ? true : false;
	if (d->err_log_size == 0)
		d->err_log_size = 512;
	if (d->err_log_size > PAGE_SIZE)
		d->err_log_size = PAGE_SIZE;
	nn = le32_to_cpu(id_ctrl->nn);

	_NI(trace_3_nvme_get_device_params, "get_device_params(@SERIAL) model='@MODEL' @NN namespaces @MAX_MSIX interrupts "
			"firmware version='@FIRMWARE'",
			d->serial, d->model, nn, d->max_msix, id_ctrl->fr);

	pdrv = &d->drives;
	id_ns = ident_buf;
	for (nsid = 1; nsid <= nn; nsid++) {
		if((err = get_ident(d, nsid, ident_phys)) != 0)
			goto out;
		if (id_ns->nsze == 0)	/* ignore zero size namespaces */
			continue;
		if (!(drv = kzalloc(sizeof(*drv), GFP_KERNEL)) ||
			!(drv->bio_list = kcalloc(d->max_msix, sizeof(*drv), GFP_KERNEL))) {
			if (drv)
				kfree(drv);
			err = -ENOMEM;
			goto out;
		}
		INIT_LIST_HEAD(&drv->info.link);
		INIT_LIST_HEAD(&drv->info.mem_priv_list);
		INIT_LIST_HEAD(&drv->freeze_link);
		init_completion(&drv->info.remove_done);
		drv->dev = d;
		drv->nsid = nsid;
		drv->size = le64_to_cpu(id_ns->nsze);
		drv->ncap = le64_to_cpu(id_ns->ncap);
		drv->flbas = id_ns->flbas;
		drv->dps = id_ns->dps;
		drv->nmic = id_ns->nmic;
		format = id_ns->lbaf[id_ns->flbas & 0xf];

		drv->block_len = 1 << format.ds;
		drv->metadata = le16_to_cpu(format.ms);
		if (drv->metadata != 0)
			drv->mtdt_extd = ((id_ns->mc & 1) && (id_ns->flbas & 0x10));

/* For the time being LIE about max transfer as if we can support large transfers
		if (drv->mtdt_extd)
			d->max_transfer = PAGE_SIZE;
*/
		err = set_drv_id_str(drv);
		if (err) {
			goto out;
		}
		_NT(trace_4_nvme_get_device_params, "nvme@NSID: Namespace (@ID_STR) blklen=@BLKLEN nblocks=@NBLOCKS_LLONG mtds=@MTDS, "
		   "separated=@SEPARATED", nsid, drv->id_str, drv->block_len, drv->size,
			drv->metadata, drv->mtdt_extd ? 'N' : 'Y');
		*pdrv = drv;
		pdrv = &drv->next;
	}

out:
	dma_free_coherent(dev, 4096, ident_buf, ident_phys);
	return err;
}

static void __attribute__ ((unused)) dump_cqs(struct device_data *d)
{
	struct nvmeibs_q_info *q;
	struct nvme_completion *cq;
	int i, j;

	for (d = device_list; d != NULL; d = d->next) {
		// _NI(t01_nsnvme, "drive @STR", d->serial);
		for (i = 0; i < d->max_client_qs; i++) {
			q = &d->qs_info[d->min_local_ioqs + i];
			if (!q->inuse) {
				_NI(t02_nsnvme, "q @PTR, qid @INT inuse by client", q, q->qid);
				continue;
			}
			cq = q->cq;
			_NE(t08_nsnvme, "cq(@INT) -> @PTR\n", q->qid, cq);
			for (j = 0; j <= 1; j++, cq++)
				_NI(t03_nsnvme, "entry @INT: result=@X sq_head=@X sq_id=@X cmd_id=@X status=@X",
					j, cq->result.u32, cq->sq_head, cq->sq_id, cq->command_id, cq->status);
		}
	}
}

static void q_async_event(struct device_data *d);

static void async_event_work(struct work_struct *work)
{
	struct device_data *d = container_of(work, struct device_data, async_work);
	u8 which_log;
	u32 nsid;
	size_t log_size;
	void *log_buf;
	dma_addr_t log_buf_phys;
	struct device *dev = &d->pci_dev->dev;
	int code;
	int err;

	_NE(error_nvme_async_event_work, "Got async event @ASYNC_RESULT", d->async_result);

	which_log = (d->async_result >> 16) & 0xff;
	if (which_log != 0) {
		nsid = 0xffffffff;
		log_size = ((which_log == NVMEIB_LOG_ERROR) ? d->err_log_size : 512);
		log_buf = dma_alloc_coherent(dev, log_size, &log_buf_phys, GFP_KERNEL);
		if (log_buf != NULL) {
			err = get_log(d, which_log, nsid, log_buf_phys, log_size);
			if (err)
				_NE(error_1_nvme_async_event_work, "get_log failed err=@ERR.", err);

			// TODO: handle log here (pass to mgmt)
			dma_free_coherent(dev, log_size, log_buf, log_buf_phys);
		}
	} else {
		_NW(t05_nsnvme, "NVMe Async Event without corresponding LOG");
	}
	code = ((d->async_result & 7) << 8) | ((d->async_result >> 8) & 0xff);
	q_async_event(d);
	if (d->drives)
		nvmeibs_async_to_mgmt(&d->drives->info, code);

	kref_put(&d->kref, nvmeibs_free_drives);
}

static void async_event_happened(void *arg, int status, u32 result)
{
	struct device_data *d = arg;

	if (status != 0) {
		if (status != 0x8)
			_NE(error_nvme_async_event_happened, "Async event status=@STATUS result=@RESULT_INT", status, result);
		return;
	}
	if (!kref_get_unless_zero(&d->kref))
		return;
	d->async_result = result;
	schedule_work(&d->async_work);
}

static void q_async_event(struct device_data *d)
{
	struct nvme_command *cmd;
	unsigned long flags;

	cmd = get_cmd(d->adminq, async_event_happened, d, &flags);
	cmd->common.opcode = nvme_admin_async_event;
	submit_cmd(d->adminq);
	spin_unlock_irqrestore(&d->adminq->q_lock, flags);
}

static int setup_adminq(struct device_data *d)
{
	int err;
	struct nvme_qp *q = d->adminq;
	int i;
	int num_local_msix = min(d->max_msix, nvmeibs_max_local_nvmeqs+1); /* nvmeibs-max_local_nvmeqs is nr-cpus unless limitted by user */

	_NT(t0_setup_adminq, "num_local_msix is min of: "
						 "d->max_msix=@INT, max_local_nvmeqs=@INT (considers num-cpus)",
		d->max_msix, nvmeibs_max_local_nvmeqs);

	if((q = alloc_qp(d, ADMIN_Q_DEPTH)) == NULL)
		return -ENOMEM;
	d->adminq = q;
	d->admin_timeout = 4*submit_wait_timeout;

	q->sq_doorbell = d->doorbells;
	q->cq_doorbell = d->doorbells + d->doorbell_stride;

	// Set up msix entries for the admin q and for potential local IO qs.
	for (i = 0; i < num_local_msix; i++)
		d->msix_entries[i].entry = i;
	_ND(trace_nvme_setup_adminq, "try set up adminq");

	//OM:
	//set affinity hint to ensure completions
	//arrive on the same cpu they were received on
#if !KS_PCI_ENABLE_MSIX_DEPRECATED
	if ((err = pci_enable_msix(d->pci_dev, d->msix_entries, num_local_msix)) > 0) {
		_NW(trace_1_nvme_setup_adminq, "Max MSI-X limited to @ERR", err);
		num_local_msix = err;
		err = pci_enable_msix(d->pci_dev, d->msix_entries, num_local_msix);
	}
#else
	if ((err = pci_enable_msix_range(d->pci_dev, d->msix_entries, 1, num_local_msix)) > 0) {
		if (err < num_local_msix) {
			_NW(trace_1_nvme_setup_adminq, "Max MSI-X limited to @ERR", err);
			num_local_msix = err;
		}
		err = 0;
	}
#endif
	d->max_local_ioqs = num_local_msix - 1;
	_NT(t1_setup_adminq, "d->max_local_ioqs=@INT", d->max_local_ioqs);

	if (err) {
		_NE(error_nvme_setup_adminq, "Fail to configure device's MSI-X capability (err @ERR)", err);
		return -EINVAL;
	}
	_ND(trace_2_nvme_setup_adminq, "msix enabled");
	snprintf(q->interrupt_name,
		sizeof q->interrupt_name, "nvme %d admin", d->seq);

	if((err = request_irq(d->msix_entries[0].vector, nvmeibs_intr, IRQF_SHARED,
			q->interrupt_name, q)) < 0)
		return err;
	_ND(trace_3_nvme_setup_adminq, "irq requested");

	writel(((q->cq_len-1)<<16) | (q->sq_len-1), &d->mmio->aqa);
	writeq(q->sq_phys, &d->mmio->asq);
	writeq(q->cq_phys, &d->mmio->acq);

	return 0;
}

static int wait_for_ready(struct device_data *d, int wanted)
{
	long wait_time = NVMEIB_CAP_TIMEOUT(d->cap);
	u32 csts;
	int i;

	if (wanted)
		wanted = NVME_CSTS_RDY;
	_NT(trace_nvme_wait_for_ready, "wait_time = @WAIT_TIME*0.5sec", wait_time);
	for (i = 0; ((csts = readl(&d->mmio->csts)) & NVME_CSTS_RDY) != wanted &&
									i < wait_time && csts != 0xffffffff; i++) {
		if (msleep_interruptible(500) != 0)
			return -EINTR;
	}
	_NT(trace_1_nvme_wait_for_ready, "waited @WAIT_TIME csts=@CSTS", (long)i, readl(&d->mmio->csts));
	if (i >= wait_time || csts == 0xffffffff)
		return -ENODEV;
	return 0;
}

static void set_info(struct drive_params *drv)
{
	struct device_data *d = drv->dev;
	struct nvmeibs_disk_info *info = &drv->info;
	int max_pages;
	int i;

	BUG_ON(!list_empty(&info->link));
	info->block_size = drv->block_len;
	info->block_shift = ffs(drv->block_len) - 1;
	info->blocks = nvmeibs_fake_large_disks ? nvmeibs_fake_large_disk_size_lba : drv->size;
	info->hw_blocks = drv->size;
	info->metadata = drv->metadata;
	info->mtdt_extd = drv->mtdt_extd;
	strlcpy(info->disk_id, drv->id_str, sizeof info->disk_id);
	info->seq = d->seq;
	info->nsid = drv->nsid;
	if (d->max_transfer != 0)
		info->max_request_size =
			min((int)(d->max_transfer / info->block_size), 65536);
	else
		info->max_request_size = 65536;
	max_pages = NVMEIBS_MAX_BOUNCE_BUFFER_PAGES - (info->metadata ? 1 : 0);
	info->max_request_size = min(info->max_request_size,
		min((int)(PAGE_SIZE / sizeof(u64)), max_pages)
		 << (PAGE_SHIFT - info->block_shift));
	info->alignment_size = d->alignment_size;
	info->n_qs = d->max_client_qs;
	info->qs = &d->qs_info[d->min_local_ioqs];
	info->msix_addr_phys = d->msix_table_phys;
	info->max_ioqs = d->max_ioqs;
	info->min_local_ioqs = d->min_local_ioqs;
	info->max_local_ioqs = d->max_local_ioqs;

	_ND(trace_nvme_set_info, "dev @DEVICE_PTR, link @MAX_CLIENT_QS client qs to disk info @INFO_PTR", d, d->max_client_qs, info);
	for (i = 0; i < info->n_qs; i++) {
		//OM: potential bug
		//if srv-layer doesn't copy d->qs_info[].disk of drive
		//(namespace) N,drive (namespace) N+1 overrides it.
		//in any case, the nvme-layer shall not reference q->disk->drv
		info->qs[i].disk = info;
		_ND(trace_1_nvme_set_info, "[@QID] q @QUEUE", i, &info->qs[i]);
	}
	info->drv = drv;
	info->dev = d;
	info->gendisk = drv->gendisk;
}

static int re_ident_ns(struct drive_params *drv)
{
	struct device_data *d = drv->dev;
	struct device *dev = &d->pci_dev->dev;
	dma_addr_t ident_dma;
	int err = 0;
	struct nvme_id_ns *id_ns;
	struct nvme_lbaf format;

	id_ns = dma_alloc_coherent(dev, 4096, &ident_dma, GFP_KERNEL);
	if (id_ns == NULL)
		return -ENOMEM;
	if ((err = get_ident(d, drv->nsid, ident_dma)) != 0) {
		err = -EIO;
		goto free_dma;
	}
	drv->size = le64_to_cpu(id_ns->nsze);
	drv->ncap = le64_to_cpu(id_ns->ncap);
	drv->flbas = id_ns->flbas;
	drv->dps = id_ns->dps;
	drv->nmic = id_ns->nmic;
	format = id_ns->lbaf[id_ns->flbas & 0xf];
	drv->block_len = 1 << format.ds;
	drv->mtdt_extd = ((id_ns->mc & 1) && (id_ns->flbas & 0x10));
	drv->metadata = le16_to_cpu(format.ms);
	if (drv->gendisk) {
		struct request_queue *rq = drv->gendisk->queue;
#if KS_HAS_QUEUE_LIMITS_START_UPDATE
		struct queue_limits lim __attribute__((unused)) = queue_limits_start_update(rq);
		rq->limits.logical_block_size = drv->block_len;
		rq->limits.physical_block_size = drv->block_len;
		queue_limits_cancel_update(rq);
#else
		blk_queue_logical_block_size(rq, drv->block_len);
		blk_queue_physical_block_size(rq, drv->block_len);
#endif
		set_capacity(drv->gendisk, drv->size * (drv->block_len/512));
	}
	set_info(drv);
	_NT(trace_nvme_re_ident_ns, "Re_ID nvme@NSID: Namespace (@ID_STR) blklen=@BLKLEN nblocks=@NBLOCKS_LLONG mtds=@MTDS "
	   "separated=@SEPARATED",
		drv->nsid, drv->id_str, drv->block_len, drv->size, drv->metadata,
		drv->mtdt_extd ? 'N' : 'Y');
free_dma:
	dma_free_coherent(dev, 4096, id_ns, ident_dma);
	return err;
}

/* Depending on Kernel Version these may not be defined in pci_regs.h */
#ifndef PCI_MSIX_TABLE
#define PCI_MSIX_TABLE 4
#endif

#ifndef PCI_MSIX_ENTRY_SIZE
#define PCI_MSIX_ENTRY_SIZE 16
#endif

#ifndef PCI_MSIX_ENTRY_DATA
#define PCI_MSIX_ENTRY_DATA 8
#endif

static int map_msix_table(struct device_data *d)
{
	struct pci_dev *dev = d->pci_dev;
	unsigned n_entries = d->max_msix;

	u32 table_offset;
	u8 bir;
	u8 msix_cap;

#if KS_DEV_MSIX_CAP
	msix_cap = dev->msix_cap;
#else
	msix_cap = pci_find_capability(dev, PCI_CAP_ID_MSIX);
#define PCI_MSIX_TABLE_BIR     0x00000007
#define PCI_MSIX_TABLE_OFFSET  0xfffffff8
#endif


	pci_read_config_dword(dev, msix_cap + PCI_MSIX_TABLE, &table_offset);
	bir = (u8)(table_offset & PCI_MSIX_TABLE_BIR);
	table_offset &= PCI_MSIX_TABLE_OFFSET;
	d->msix_table_phys = pci_resource_start(dev, bir) + table_offset;
	d->msix_table = ioremap(d->msix_table_phys,
		n_entries * PCI_MSIX_ENTRY_SIZE);
	if (!d->msix_table)
		return -ENOMEM;

	_ND(trace_nvme_map_msix_table, "msix phys=@PHYS, mapped=@MAPPED",
		d->msix_table_phys, (u64)d->msix_table);

	return 0;
}

void set_msix_vector(
	struct nvmeibs_q_info *q, u64 msix_table_addr, u64 addr, u32 payload)
{
//	BUG_ON((u64)q->msix_phys != msix_table_addr);

	/* write the address (@addr) to `q->msix_vec + 0` */
	writeq(addr, q->msix_vec);

	/* write the payload (@payload) to `q->msix_vec + 8` */
	writeq((u64)payload, q->msix_vec + PCI_MSIX_ENTRY_DATA);
	wmb();
}

void mask_msix_vector(struct nvmeibs_q_info *q)
{
	u32 ctrl = PCI_MSIX_ENTRY_CTRL_MASKBIT;
	writel(ctrl, q->msix_vec + PCI_MSIX_ENTRY_VECTOR_CTRL);
	wmb();
}

void nvmeibs_nvme_dump_msix_table(struct nvmeibs_disk_info *di)
{
	struct device_data *dev = di->dev;
	int i;
	struct msix_entry {
		u32 addr_lo;
		u32 addr_hi;
		u32 data;
		u32 ctrl;
	};

	struct msix_entry __iomem *entry;

	_NI(trace_nvme_nvmeibs_nvme_dump_msix_table, "Disk: @DISK_ID_STR", di->disk_id);

	for (i = 0, entry = dev->msix_table; i < dev->max_msix; ++i, ++entry) {
		_NI(trace_1_nvme_nvmeibs_nvme_dump_msix_table, "MSIX [@IDX] - Address: @ADDR_HI @ADDR_LO, Data: @DATA_INT, Vector: @CTRL",
		   i, entry->addr_hi, entry->addr_lo, entry->data, entry->ctrl);
	}
}
EXPORT_SYMBOL(nvmeibs_nvme_dump_msix_table);

static ssize_t add_hex128(char *buf, ssize_t len, char *name, u8 data[16])
{
	ssize_t filled = 0;
	int i;
	bool found_non0 = false;

	filled += scnprintf(buf+filled, len - filled, "%s=0x", name);
	for (i = 15; i >= 0; i--) {
		if (found_non0) {
			filled += scnprintf(buf+filled, len - filled, "%02x", data[i]);
		}
		else if (i == 0 || data[i] != 0) {
			filled += scnprintf(buf+filled, len - filled, "%x", data[i]);
			found_non0 = true;
		}
	}
	filled += scnprintf(buf+filled, len - filled, "\n");
	return filled;
}

static void __attribute__ ((unused))
check_ioq(struct nvme_qp *q)
{
	int i;
	int cnt = 0;
	struct device_data *d = q->dev;
	struct drive_params *drv;
	struct bio *bio;

	// _NI(t0a_nsnvme, "bios_sent=%d bios_done=%d", bios_sent, bios_done);
	// _NI(t0b_nsnvme, "bios_sumbited=%d bios_completed=%d", bios_sumbited, bios_completed);
	// _NI(t0c_nsnvme, "spurious_cnt=%d, multihandle_cnt=%d, total_handle=%d", // spurious_cnt, multihandle_cnt, total_handle);
	for (drv = d->drives; drv != NULL &&
			(bio = bio_list_peek(&drv->bio_list[q->id - 1])) == NULL;
			drv = drv->next);
	if (bio != NULL)
		_NI(t0d_nsnvme, "bio @PTR is stuck.", bio);
	_NI(t0e_nsnvme, "qid=@INT sq_head=@INT sq_tail=@INT cq_head=@INT",
		q->id, q->sq_head, q->sq_tail, q->cq_head);
	for (i = 0; i < q->total_ids; i++) {
		if (q->ids[i].callback != NULL) {
			_NI(t0f_nsnvme, "cmd_id=@INT arg=@PTR", i, q->ids[i].arg);
			++cnt;
		}
	}
	_NI(t0g_nsnvme, "used_ids=@INT counted=@INT", q->used_ids, cnt);
}

#define smart_line(...)	\
	filled += scnprintf(buf+filled, len-filled, __VA_ARGS__)
#define smart_hex128(name, data)	\
	filled += add_hex128(buf+filled, len-filled, name, data)

int nvmeibs_disk_info_numa_node(struct nvmeibs_disk_info *d)
{
	int rv = 0;
#ifdef CONFIG_NUMA
	if (d && d->dev && d->dev->pci_dev)
		rv = d->dev->pci_dev->dev.numa_node;
#if KS_HAS_BDEV_FILE_OPEN_BY_PATH
	else if (d && d->external && d->external->block_dev)
		rv = file_bdev(d->external->block_dev)->bd_disk->queue->node;
#elif KS_HAS_BDEV_OPEN_BY_PATH
	else if (d && d->external && d->external->block_dev && d->external->block_dev->bdev &&
			d->external->block_dev->bdev->bd_disk && d->external->block_dev->bdev->bd_disk->queue)
		rv = d->external->block_dev->bdev->bd_disk->queue->node;
#else
	else if (d && d->external && d->external->block_dev &&
			d->external->block_dev->bd_disk && d->external->block_dev->bd_disk->queue)
		rv = d->external->block_dev->bd_disk->queue->node;
#endif
#endif

	return rv;
}

#define CORE_SERVER_NVME_SMART_PROC_FRMT_VER 1
static ssize_t smart_fill_buf(void *arg, char *buf, size_t len)
{
	struct device_data *d = arg;
	struct device *dev = &d->pci_dev->dev;
	ssize_t filled = 0;
	int err;
	struct nvme_smart_log *smart;
	dma_addr_t log_buf_phys;
	struct drive_params *drv;

/*
	if (d->local_ioq != NULL)
		check_ioq(d->local_ioq);
	dump_cqs(d);
*/

	smart_line("Pci Address=%s\n", dev_name(&d->pci_dev->dev));
	smart_line("Serial Number=%.*s\n", NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE_SPEC, d->serial);
	smart_line("Vendor=%#x\n", d->vendor);
	smart_line("Model=%.*s\n", NVMEIB_DISK_MAX_MODEL_STR_SIZE, d->model);
	smart_line("Submission Queues=%d\n", d->max_ioqs);
	smart_line("Completion Queues=%d\n", d->max_ioqs);
	smart_line("MSIX Interrupts=%d\n", d->max_msix);
	smart_line("Num admin cmds=%u\n", d->n_smart_reads);
	for (drv = d->drives; drv != NULL; drv = drv->next)
		smart_line("Namespace Id=%d\n", drv->nsid);
#ifdef CONFIG_NUMA
	smart_line("Numa Node=%d\n", dev->numa_node);
#endif

	smart = dma_alloc_coherent(dev, sizeof *smart, &log_buf_phys, GFP_KERNEL);
	if (smart == NULL) {
		_NE(error_nvme_smart_fill_buf, "No memory for reading smart information.");
		return filled;
	}
	err = get_log(d, NVMEIB_LOG_SMART, 0xffffffff, log_buf_phys, sizeof *smart);
	if (err != 0) {
		_NE(error_1_nvme_smart_fill_buf, "Failed to read smart information, err=@ERR.", err);
		dma_free_coherent(dev, sizeof *smart, smart, log_buf_phys);
		return filled;
	}
	smart_line("Critical Warning=%#x\n", smart->critical_warning);
	smart_line("Temperature=%d K\n",
		smart->temperature[0]+(smart->temperature[1]<<8));
	smart_line("Available Spare=%d %%\n", smart->avail_spare);
	smart_line("Available Spare Threshold=%d %%\n", smart->spare_thresh);
	smart_line("Percentage Used=%d %%\n", smart->percent_used);
	smart_hex128("Data Units Read", smart->data_units_read);
	smart_hex128("Data Units Written", smart->data_units_written);
	smart_hex128("Host Read Commands", smart->host_reads);
	smart_hex128("Host Write Commands", smart->host_writes);
	smart_hex128("Controller Busy Time", smart->ctrl_busy_time);
	smart_hex128("Power Cycles", smart->power_cycles);
	smart_hex128("Power On Hours", smart->power_on_hours);
	smart_hex128("Unsafe Shutdowns", smart->unsafe_shutdowns);
	smart_hex128("Media Errors", smart->media_errors);
	smart_hex128("Number of Error Information Log Entries",
					smart->num_err_log_entries);
	filled += nvmeib_proc_add_smart_proc_epilog(CORE_SERVER_NVME_SMART_PROC_FRMT_VER, buf + filled, len - filled);
	dma_free_coherent(dev, sizeof *smart, smart, log_buf_phys);

	return filled;
}


//static int is_qemu_device(struct device_data *d)
//{
//	if (!d)
//		return 0;
//	if (strncmp(d->model, "QEMU", 4))
//		return 0;
//	return 1;
//}

struct errlog_entry {
	u64	err_count;
	u16 sq_id;
	u16 cmd_id;
	u16 status;
	u16 location;
	u64 lba;
	u32 nsid;
	u8 vs;
	u8 reserved[35];
};

#define CORE_SERVER_NVME_LOG_FILL_BUF_PROC_FRMT_VER 1
static ssize_t log_fill_buf(void *arg, char *buf, size_t len)
{
	// struct pci_dev *pdev = to_pci_dev(dev);
	// struct device_data *d = pci_get_drvdata(pdev);
	struct device_data *d = arg;
	struct device *dev = &d->pci_dev->dev;
	ssize_t filled = 0;
	int err;
	struct errlog_entry *errlog;
	dma_addr_t log_buf_phys;
	int i;

	_ND(trace_nvme_log_fill_buf, "err_log_size = @ERR_LOG_SIZE", d->err_log_size);
	errlog = dma_alloc_coherent(dev, d->err_log_size, &log_buf_phys, GFP_KERNEL);
	if (errlog == NULL)
		return -ENOMEM;
	err = get_log(d, NVMEIB_LOG_ERROR, 0xffffffff, log_buf_phys, d->err_log_size);
	if (err != 0) {
		_NE(error_nvme_log_fill_buf, "get-log failed err=@ERR.", err);
		dma_free_coherent(dev, d->err_log_size, errlog, log_buf_phys);
		return -EIO;
	}
	for (i = 0; i < d->err_log_size/64; i++) {
		filled += scnprintf(buf+filled, len-filled,
			"%lld: sqid=%d cmd=%d stat=%#x loc=%#x LBA=%#llx nsid=%d vs=%d\n",
			errlog[i].err_count, errlog[i].sq_id, errlog[i].cmd_id,
			errlog[i].status, errlog[i].location, errlog[i].lba,
			errlog[i].nsid, errlog[i].vs);
	}
	filled += nvmeib_proc_add_txt_proc_epilog(CORE_SERVER_NVME_LOG_FILL_BUF_PROC_FRMT_VER, buf + filled, len - filled);
	dma_free_coherent(dev, d->err_log_size, errlog, log_buf_phys);
	return filled;
}

static ssize_t ext_smart_fill(void *arg, char *buf, size_t len)
{
	struct external_drive *d = arg;
	ssize_t filled = 0;
	// struct nvme_smart_log *smart;
	static u8 dummy128[16] = {0};

	smart_line("Serial Number=%.*s\n", NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE_SPEC, d->info.disk_id);
	smart_line("Vendor=%#x\n", d->vendor);
	smart_line("Model=%.*s\n", NVMEIB_DISK_MAX_MODEL_STR_SIZE, d->model);
	smart_line("Submission Queues=%d\n", 1);
	smart_line("Completion Queues=%d\n", 1);
	smart_line("MSIX Interrupts=%d\n", 2);
	smart_line("Namespace Id=0\n");
#ifdef CONFIG_NUMA
#if KS_HAS_BDEV_FILE_OPEN_BY_PATH
	if (d && d->block_dev)
		smart_line("Numa Node=%d\n", file_bdev(d->block_dev)->bd_disk->queue->node);
#elif KS_HAS_BDEV_OPEN_BY_PATH
	if (d->block_dev->bdev->bd_disk && d->block_dev->bdev->bd_disk->queue)
		smart_line("Numa Node=%d\n", d->block_dev->bdev->bd_disk->queue->node);
#else
	if (d->block_dev->bd_disk && d->block_dev->bd_disk->queue)
		smart_line("Numa Node=%d\n", d->block_dev->bd_disk->queue->node);
#endif
#endif

	smart_line("Critical Warning=%#x\n", 0);
	smart_line("Temperature=%d K\n", 300);
	smart_line("Available Spare=%d %%\n", 100);
	smart_line("Available Spare Threshold=%d %%\n", 10);
	smart_line("Percentage Used=%d %%\n", 0);
	smart_hex128("Data Units Read", dummy128);
	smart_hex128("Data Units Written", dummy128);
	smart_hex128("Host Read Commands", dummy128);
	smart_hex128("Host Write Commands", dummy128);
	smart_hex128("Controller Busy Time", dummy128);
	smart_hex128("Power Cycles", dummy128);
	smart_hex128("Power On Hours", dummy128);
	smart_hex128("Unsafe Shutdowns", dummy128);
	smart_hex128("Media Errors", dummy128);
	smart_hex128("Number of Error Information Log Entries", dummy128);

	return filled;
}

static LIST_HEAD(freeze_wait_list);
static DEFINE_MUTEX(freeze_wait_lock);

static void freeze_wait_add(struct drive_params *drv)
{
	if (unlikely(!list_empty(&drv->freeze_link))) {
		_NE(error_nvme_freeze_wait_add, "dev @SERIAL (@DEVICE_PTR) already linked", drv->id_str, drv);
		return;
	}
	if (unlikely(!drv->frozen)) {
		_NE(error_1_nvme_freeze_wait_add, "dev @SERIAL (@DEVICE_PTR) not frozen", drv->id_str, drv);
		return;
	}

	mutex_lock(&freeze_wait_lock);
	_NT(trace_nvme_freeze_wait_add, "dev @SERIAL (@DEVICE_PTR) - add", drv->id_str, drv);
	list_add_tail(&drv->freeze_link, &freeze_wait_list);
	mutex_unlock(&freeze_wait_lock);
}

static void freeze_wait_del(struct drive_params *drv)
{
	if (unlikely(list_empty(&drv->freeze_link))) {
		_NE(error_nvme_freeze_wait_del, "dev @SERIAL (@DEVICE_PTR) not linked", drv->id_str, drv);
		return;
	}
	if (unlikely(!drv->frozen)) {
		_NE(error_1_nvme_freeze_wait_del, "dev @SERIAL (@DEVICE_PTR) not frozen", drv->id_str, drv);
		return;
	}

	mutex_lock(&freeze_wait_lock);
	_NT(trace_nvme_freeze_wait_del, "dev @SERIAL (@DEVICE_PTR) - del", drv->id_str, drv);
	list_del_init(&drv->freeze_link);
	mutex_unlock(&freeze_wait_lock);

}

static void freeze_wait_done(const char *id)
{
	struct drive_params *drv;

	mutex_lock(&freeze_wait_lock);
	list_for_each_entry(drv, &freeze_wait_list, freeze_link) {
		if (strcmp(drv->id_str, id) == 0) {
			_NT(trace_nvme_freeze_wait_done, "dev @SERIAL - complete (drv=@DRV)",
			   drv->id_str, id);
			complete(&drv->info.remove_done);
			break;
		}
	}
	mutex_unlock(&freeze_wait_lock);
}

void nvmeibs_remove_done(const char *id)
{
	freeze_wait_done(id);
}

extern void print_separated_lines(const char *prefix, char *p, int count);

const char *nvmeibs_get_status(const struct nvmeibs_disk_info *di)
{
   if ((di->dev && di->drv->frozen) || (di->external && di->external->frozen))
	   return "Frozen";
   if (di->metadata > 0 && di->mtdt_extd && !DISK_ALLOW_INLINE_MD)
	   return "Invalid Format";
   return "Ok";
}

#if KS_HAS_SET_FS && defined(KERNEL_DS)
#include <linux/uaccess.h>
#else
#include <linux/namei.h>
#endif

static bool dummy_disk_fs_stat(struct kstat *stat)
{
	char path[256] = DUMMY_DISK_PATH;
	int rv;
#if KS_HAS_SET_FS && defined(KERNEL_DS)
	mm_segment_t oldfs;
#else
	struct path kpath;
#endif

	if (!stat) {
		_NT(trace_0_find_dummy_disk_path, "no stat");
		rv = -EINVAL;
		goto out;
	}
	if (!dummy_id) {
		_NT(trace_1_find_dummy_disk_path, "dummy-disk name not initialized");
		rv = -ENXIO;
		goto out;
	}
	if (strlen(dummy_id) > (sizeof(path) - 1 - strlen(path))) {
		_NT(trace_2_find_dummy_disk_path, "dummy-disk name too big @STR",
			dummy_id);
		rv = -E2BIG;
		goto out;
	}

	strncat(path, dummy_id, sizeof(path) - 1 - strlen(path));

#if KS_HAS_SET_FS && defined(KERNEL_DS)
	oldfs = get_fs();
	set_fs(KERNEL_DS); // YR: this used to be get_ds, switch due to 5.x dropping get_ds
	rv  = vfs_stat(path, stat); //https://www.spinics.net/lists/linux-api/msg42587.html
	set_fs(oldfs);
#else
	if ((rv = kern_path(path, LOOKUP_FOLLOW, &kpath))) {
		_NE(trace_vmeibs_get_status_kern_path, "kern_path returned @RV", rv);
	}
	else {
		rv = vfs_getattr(&kpath, stat, STATX_BASIC_STATS,
						AT_NO_AUTOMOUNT);
		path_put(&kpath);
	}
#endif

	if (rv) {
		_NT(trace_3_find_dummy_disk_path, "dummy-disk @STR not found at @PATH",
			dummy_id, path);
		rv = -ENOENT;
		goto out;
	}
	rv = 0;

out:
	return rv;
}

static void dummy_disk_add(void)
{
	struct kstat stat;

	if (dummy_id && !dummy_disk_fs_stat(&stat)) {
		_NT(trace_1_dummy_disk_add, "Dummy-disk @STR add", dummy_id);
		dummy_disk_added = true;
		nvmeibs_um_comm_add_dummy_disk(nvmeibs_get_um_comm(),
									   dummy_id, strlen(dummy_id));
	}
}

static void dummy_disk_remove(void)
{
	if (dummy_disk_added) {
		_NT(trace_0_dummy_disk_remove, "Dummy-disk @STR remove", dummy_id);
		nvmeibs_um_comm_remove_dummy_disk(nvmeibs_get_um_comm(),
										  dummy_id, strlen(dummy_id));
	}
}

ssize_t fill_disks(void *dummy, char *buffer, size_t len)
{
	struct device_data *d;
	struct drive_params *drv;
	struct external_drive *p;
	struct nvmeibs_disk_info *di;
	int count = 0;
	extern char *dummy_id;

	count += scnprintf(buffer + count, len - count,
			   NVMEIBS_DISKS_CSV_HEADER "\n");
	down_read(&global_lock);
	for (d = device_list; d != NULL; d = d->next) {
		for (drv = d->drives; drv != NULL; drv = drv->next) {
			di = &drv->info;
			if (di->gendisk == NULL)
				continue;
			count += scnprintf(buffer + count, len - count,
				"%s,%lld,%lld,%d,%d,%d,%d,/dev/%.32s,%d,%s,%d,%.40s\n",
				di->disk_id,
				di->blocks,
				di->hw_blocks,
				di->block_size,
				di->max_request_size,
				di->seq,
				di->nsid,
				di->gendisk->disk_name,
				di->metadata,
				nvmeibs_get_status(di),
				d->vendor,
				d->model
				);
		}
	}
	for (p = external_drives; p != NULL; p = p->next) {
		di = &p->info;
		//if (di->gendisk == NULL)
		//	continue;
		count += scnprintf(buffer + count, len - count,
			"%s,%lld,%lld,%d,%d,%d,%d,/dev/%.32s,%d,%s,%d\n",
			di->disk_id,
			di->blocks,
			di->hw_blocks,
			di->block_size,
			di->max_request_size,
			di->seq,
			di->nsid,
			di->gendisk ? di->gendisk->disk_name : "(none)",
			di->metadata,
			nvmeibs_get_status(di),
			p->vendor
			);
	}
	up_read(&global_lock);
	if (dummy_id != NULL) {
		struct kstat stat;
		if (!dummy_disk_fs_stat(&stat)) {
            if (stat.size == DUMMY_SIZE) {
			    count += scnprintf(buffer + count, len - count,
                    "%s,%lld,%ld,1,0,0,%s%s,0,Ok,0\n", dummy_id,
				    stat.size / stat.blksize, (long)stat.blksize,
					DUMMY_DISK_PATH, dummy_id);

		    }
            else {
                _NE(trace_101_fill_disks, "OOPS - dummy disk size mismatch: "
                    "expected @INT, from_stat @INT",
                    (int)DUMMY_SIZE, (int)stat.size);
            }
        }
		else if (dummy_disk_added)
			_NE(trace_0_fill_disks, "OOPS, dummy-disk file was removed");

	}
	print_separated_lines("disks.csv", buffer, count);
	return count;
}

#if KS_HAS_BDEV_FILE_OPEN_BY_PATH
static inline struct block_device *ed2bd(struct external_drive *ed) { return file_bdev(ed->block_dev); }
#elif KS_HAS_BDEV_OPEN_BY_PATH
static inline struct block_device *ed2bd(struct external_drive *ed) { return ed->block_dev->bdev; }
#else
static inline struct block_device *ed2bd(struct external_drive *ed) { return ed->block_dev; }
#endif

static void set_external_info(struct external_drive *p)
{
	struct nvmeibs_disk_info *info = &p->info;
	struct block_device *bd = ed2bd(p);
	char name_buf[16];
	struct request_queue *q = bdev_get_queue(bd);
	int max_sectors = queue_max_sectors(q);

	INIT_LIST_HEAD(&info->link);
	INIT_LIST_HEAD(&info->mem_priv_list);
	init_completion(&info->remove_done);
	info->seq = next_seq++;		/* protected with global_lock */
	info->block_size = bdev_logical_block_size(bd);
	info->blocks = get_capacity(bd->bd_disk) /
					(info->block_size >> 9);
	info->hw_blocks = info->blocks;
	info->block_shift = ffs(info->block_size) - 1;
	info->metadata = 0;
	info->mtdt_extd = false;
	info->max_request_size = min((128*1024) / info->block_size, max_sectors);
	info->nsid = 0;
	info->alignment_size = 0;
	info->n_qs = 0;
	info->qs = NULL;
	info->drv = NULL;
	info->dev = NULL;
	info->external = p;
	info->io_stats = nvmeib_io_stats_create(info->disk_id, VERB_RW_T_RECOV_BITMASK, info->block_size);
	snprintf(name_buf, sizeof name_buf, "smart%d", info->seq);
	p->proc_smart =
		nvmeib_public_proc_create(name_buf, nvmeibs_proc_dir, ext_smart_fill, NULL, p);
}

static void _dev_shutdown(struct device_data *d)
{
	u32 csts;

	d->wait_started = jiffies;
	d->wait_until = d->wait_started + NVMEIB_CAP_TIMEOUT(d->cap)*HZ/2;
	csts = readl(&d->mmio->csts);
	if (((csts & NVME_CSTS_RDY) != NVME_CSTS_RDY) && ((d->cc & NVME_CC_ENABLE) == NVME_CC_ENABLE)) {
		_NT(trace_nvme_dev_shutdown, "Shutting down not-ready device, waiting until device ready"
		   " csts=@CSTS pci_dev=@PCI_DEV   @DEV_NAME", csts, d->pci_dev, dev_name(&d->pci_dev->dev));
		wait_for_ready(d, 1);
	}
	writel(d->cc | NVME_CC_SHN_NORMAL, &d->mmio->cc);
	while (((csts = readl(&d->mmio->csts)) & NVMEIB_CSTS_SHST_MASK) !=
				NVME_CSTS_SHST_CMPLT && csts != 0xffffffff) {
		msleep(100);
		if (time_after(jiffies, d->wait_until)) {
			_NE(t0h_nsnvme, "nvmeibs:dev_shutdown(@STR) not going down. csts=@X", d->serial, readl(&d->mmio->csts));
			return;
		}
	}
}

static int get_vec_info(struct bio *bio,
			struct bio_vec *vec,
			unsigned *len,
			sector_t *bi_sector)
{
#if KS_BIO_VEC_RET_STRUCT
	*vec = bio_iovec(bio);
	*bi_sector = bio->bi_iter.bi_sector;
	*len = min((unsigned)PAGE_SIZE - vec->bv_offset, vec->bv_len);

	return 0;
#else
	*vec = *bio_iovec(bio);
	*bi_sector = bio->bi_sector;
	*len = vec->bv_len;

	return vec->bv_offset + vec->bv_len > PAGE_SIZE;
#endif
}

struct nvmeibs_bio_ctxt {
	struct bio *bio;
	struct device *dev;
	dma_addr_t prp1, prp2;
	size_t prp1_len, prp2_len;
	struct page *mtdt_page;
	dma_addr_t mtdt_dma;
	dma_addr_t mtdt_dma_ptr;
	size_t mtdt_len;
};

static struct nvmeibs_bio_ctxt *
nvmeibs_bio_ctxt_alloc(struct device *dev, struct bio *bio)
{
	struct nvmeibs_bio_ctxt *ctxt;

	ctxt = kzalloc(sizeof(*ctxt), GFP_ATOMIC);
	if (!ctxt)
		return NULL;

	ctxt->bio = bio;
	ctxt->dev = dev;

	return ctxt;
}

static void nvmeibs_bio_ctxt_free(struct nvmeibs_bio_ctxt *ctxt)
{
	if (ctxt->prp1_len)
		dma_unmap_page(ctxt->dev, ctxt->prp1, ctxt->prp1_len, DMA_BIDIRECTIONAL);
	if (ctxt->prp2_len)
		dma_unmap_page(ctxt->dev, ctxt->prp2, ctxt->prp2_len, DMA_BIDIRECTIONAL);
	if (ctxt->mtdt_len) {
		int i;
		u64 *p = page_address(ctxt->mtdt_page);
		dma_unmap_page(ctxt->dev, ctxt->mtdt_dma, ctxt->mtdt_len, DMA_BIDIRECTIONAL);
		_ND(trace_nvme_nvmeibs_bio_ctxt_free, "dump metadata @ @METADATA len=@LEN_LONG", p, ctxt->mtdt_len);
		for (i = 0; i < (ctxt->mtdt_len >> 3); i++)
			_ND(trace_1_nvme_nvmeibs_bio_ctxt_free, "metadata[@IDX] = @RAW", i, p[i]);
		__free_page(ctxt->mtdt_page);
	}
	kfree(ctxt);
}

static void nvmeibs_bio_part_done(void *arg, int status, u32 result)
{
	struct nvmeibs_bio_ctxt *ctxt = arg;
	struct bio *bio = ctxt->bio;

#if KS_BI_REMAINING
#if KS_BI_REMAINING_UNDERSCORE
	_ND(trace_nvmeibs_bio_part_done, "bi_remaining=@COUNT bio=@BIO", atomic_read(&bio->__bi_remaining), bio);
#else
	_ND(trace_nvmeibs_bio_part_done, "bi_remaining=@COUNT bio=@BIO", atomic_read(&bio->bi_remaining), bio);
#endif
#endif
	_ND(trace_nvme_nvmeibs_bio_part_done, "bio_part_done(bio=@BIO status=@STATUS)", bio, status);
	if (status)
		_NT(trace_1_nvme_nvmeibs_bio_part_done, "bio_part_done(status=@STATUS)", status);
	if ((status & 0x3FF) == EPERM_READ_FAIL)
		_NT(error_nvme_nvmeibs_bio_part_done, "Unrecovered Read Error");

	bio_endio(bio, status ? -EIO : 0);

	nvmeibs_bio_ctxt_free(ctxt);
}

#if 0
struct prpl_list {
	struct prpl_list *next;
	dma_addr_t dma;
	struct device *dev;
} *prpl_freelist = NULL;
#endif

static volatile struct nvmeibs_dsm_range *dsm_freelist = NULL;
struct nvmeibs_dsm_range {
	__le32			cattr;
	__le32			nlb;
	__le64			slba;
	struct device	*dev;
	dma_addr_t		dma;
	struct bio		*bio;
	struct nvmeibs_dsm_range *next;
};

static void nvmeibs_trim_done(void *arg, int status, u32 result)
{
	struct nvmeibs_dsm_range *dsm_buf = arg;
	struct nvmeibs_dsm_range *old;

	_ND(trace_nvme_nvmeibs_trim_done, "discard bio = @BIO status=@STATUS", dsm_buf->bio, status);
	bio_endio(dsm_buf->bio, status ?-EIO : 0);
	do {
		dsm_buf->next = old = (struct nvmeibs_dsm_range *)dsm_freelist;
	} while (cmpxchg(&dsm_freelist, old, dsm_buf) != old);
	// dma_free_coherent(dsm_buf->dev, sizeof *dsm_buf, dsm_buf, dsm_buf->dma);
}

static int local_submit_discard(struct nvme_qp *q,
				 struct device *dev,
				 struct drive_params *drv,
				 struct nvme_command *cmd,
				 int cmd_id,
				 struct bio *bio)
{
	struct nvmeibs_dsm_range *dsm_buf;
	dma_addr_t dsm_buf_dma;
	struct nvmeib_dsm_cmd *dsm_cmd = (struct nvmeib_dsm_cmd *)cmd;

	dsm_buf = dma_alloc_coherent(dev, sizeof *dsm_buf, &dsm_buf_dma, 0);
	if (dsm_buf == NULL) {
		bio_endio(bio, -ENOMEM);
		return -ENOMEM;
	}

	dsm_buf->cattr = 0;
#if KS_BIO_VEC_RET_STRUCT
	if (drv->metadata) {
		dsm_buf->nlb = cpu_to_le32(bio->bi_iter.bi_size >> NVMEIBC_SECTOR_SHIFT);
		dsm_buf->slba = cpu_to_le64(bio->bi_iter.bi_sector >> (NVMEIBC_SECTOR_SHIFT - 9));
	} else {
		dsm_buf->nlb = cpu_to_le32(bio->bi_iter.bi_size >> drv->info.block_shift);
		dsm_buf->slba = cpu_to_le64(bio->bi_iter.bi_sector >> (drv->info.block_shift - 9));
	}
#else
	if (drv->metadata) {
		dsm_buf->nlb = cpu_to_le32(bio->bi_size >> NVMEIBC_SECTOR_SHIFT);
		dsm_buf->slba = cpu_to_le64(bio->bi_sector >> (NVMEIBC_SECTOR_SHIFT - 9));
	} else {
		dsm_buf->nlb = cpu_to_le32(bio->bi_size >> drv->info.block_shift);
		dsm_buf->slba = cpu_to_le64(bio->bi_sector >> (drv->info.block_shift - 9));
	}
#endif

	dsm_buf->dev = dev;
	dsm_buf->dma = dsm_buf_dma;
	dsm_buf->bio = bio;
	q->ids[cmd_id].callback = nvmeibs_trim_done;
	q->ids[cmd_id].arg = dsm_buf;

	dsm_cmd->opcode = nvme_cmd_dsm;
	dsm_cmd->nsid = cpu_to_le32(drv->nsid);
	dsm_cmd->prp1 = cpu_to_le64(dsm_buf_dma);
	dsm_cmd->nr = 0;
	dsm_cmd->attributes = cpu_to_le32(NVME_DSMGMT_AD);
	_ND(trace_nvme_local_submit_discard, "discard bio = @BIO nsid=@NSID cmd=@DL,@DL,@DL,@DL,@DL"
	   ",@DL,@DL,@DL  dsm_buf=@DL,@DL",
	   bio, dsm_cmd->nsid, ((u64*)cmd)[0], ((u64*)cmd)[1],
	   ((u64*)cmd)[2], ((u64*)cmd)[3], ((u64*)cmd)[4], ((u64*)cmd)[5],
	   ((u64*)cmd)[6], ((u64*)cmd)[7],
	   ((u64*)dsm_buf)[0], ((u64*)dsm_buf)[1]);
	return 0;
}

static void local_submit_bios(struct nvme_qp *q)
{
	struct device_data *d = q->dev;
	struct device *dev = &d->pci_dev->dev;
	struct drive_params *drv;
	struct bio *bio = NULL;
	struct nvme_command *cmd;
	struct bio_vec vec;
	unsigned len;
	int cmd_id = 0;
	int sq_tail;
	sector_t bi_sector;
	struct nvmeib_dsm_cmd *dsm_cmd __attribute__((unused));
	int rc;

	if (d->need_reset || d->reset_pending || d->removed || d->formatting ||
		q->dying) {
		for (drv = d->drives; drv != NULL; drv = drv->next) {
			while ((bio = bio_list_pop(&drv->bio_list[q->id - 1])) != NULL)
				bio_endio(bio, -ENXIO);
		}
		return;
	}

	for (drv = d->drives; drv != NULL &&
			(bio = bio_list_peek(&drv->bio_list[q->id - 1])) == NULL;
			drv = drv->next);
	sq_tail = q->sq_tail;

	while (bio != NULL && sq_tail + 1 != (q->sq_head ? : q->sq_len) &&
			(q->used_ids < q->total_ids)) {
		struct nvmeibs_bio_ctxt *ctxt;

		cmd_id = find_next_zero_bit(q->id_bitmap, q->total_ids, cmd_id);
		BUG_ON(cmd_id >= q->total_ids);
		q->ids[cmd_id].issue_time = jiffies + q->used_ids * HZ;
		q->ids[cmd_id].aborted = false;

		cmd = &q->sq[sq_tail];
		memset(cmd, 0, sizeof(*cmd));
		cmd->rw.command_id = cmd_id;

		/* Not all kernels have the nvme_dsm_cmd defined so we use our own */
		dsm_cmd = (struct nvmeib_dsm_cmd *)cmd;

#ifdef REQ_OP_BITS
		if (bio_op(bio) == REQ_OP_DISCARD) {
#elif defined(BIO_DISCARD)
		if (bio->bi_rw & BIO_DISCARD) {
#else
		if (bio->bi_rw & REQ_DISCARD) {
#endif
			if(local_submit_discard(q, dev, drv, cmd, cmd_id, bio) == 0) {
				if (++sq_tail == q->sq_len)
					sq_tail = 0;
				set_bit(cmd_id, q->id_bitmap);
				++q->used_ids;
			}
			goto cont;
		}

		ctxt = nvmeibs_bio_ctxt_alloc(dev, bio);
		if (!ctxt) {
			_NE(t0j_nsnvme, "nvmeibs local: failed allocating context");
			bio_endio(bio, -ENOMEM);
			goto cont;
		}

		q->ids[cmd_id].callback = nvmeibs_bio_part_done;
		q->ids[cmd_id].arg = ctxt;

		rc = get_vec_info(bio, &vec, &len, &bi_sector);
		if (rc) {
			_NE(t0k_nsnvme, "nvmeibs local: I/O too big");
			bio_endio(bio, -EINVAL);
			goto cont;
		}
		if (bi_sector & ((drv->block_len >> 9)-1) ||
								len & (drv->block_len - 1)) {
			_NE(error_nvme_local_submit_bios, "Unaligned access bi_sector=@BI_SECTOR len=@LEN",
			    (u64)bi_sector, len);
			bio_endio(bio, -EINVAL);
			goto cont;
		}
#ifdef REQ_OP_BITS
		cmd->rw.opcode = (bio_op(bio) == REQ_OP_WRITE) ? nvme_cmd_write : nvme_cmd_read;
#else
		cmd->rw.opcode = (bio->bi_rw & REQ_WRITE) ? nvme_cmd_write : nvme_cmd_read;
#endif
		cmd->rw.nsid = cpu_to_le32(drv->nsid);

		ctxt->prp1 = dma_map_page(dev, vec.bv_page, vec.bv_offset, len,
					  DMA_BIDIRECTIONAL);
		if (dma_mapping_error(dev, ctxt->prp1)) {
			_NE(error_1_nvme_local_submit_bios, "Error mapping bv page to nvme device");
			bio_endio(bio, -ENOMEM);
			goto cont;
		}
		ctxt->prp1_len = len;
		if (drv->metadata) {
			u64 *mp;
			ctxt->mtdt_page = alloc_page(GFP_ATOMIC);
			if (ctxt->mtdt_page) {
				ctxt->mtdt_len = drv->metadata * (len >> drv->info.block_shift);
				mp = page_address(ctxt->mtdt_page);
				_ND(trace_nvme_local_submit_bios, "metadata @@MP len=@LEN_LONG", mp, ctxt->mtdt_len);
				memset(mp, 0, ctxt->mtdt_len);
				ctxt->mtdt_dma = dma_map_page(dev, ctxt->mtdt_page, 0,
										ctxt->mtdt_len, DMA_BIDIRECTIONAL);
				if (dma_mapping_error(dev, ctxt->mtdt_dma)) {
					_NE(error_2_nvme_local_submit_bios, "Error mapping metadata page to nvme device");
					__free_page(ctxt->mtdt_page);
					ctxt->mtdt_page = NULL;
				}
			}
			if (ctxt->mtdt_page == NULL) {
				ctxt->mtdt_len = 0;
				bio_endio(bio, -ENOMEM);
				goto cont;
			}
			if (drv->mtdt_extd)
				cmd->rw.dptr_prp2 = cpu_to_le64(ctxt->mtdt_dma);
			else
				cmd->rw.metadata = cpu_to_le64(ctxt->mtdt_dma);
		}
		cmd->rw.slba = cpu_to_le64(bi_sector >> (drv->info.block_shift - 9));
		cmd->rw.length = cpu_to_le16((len >> drv->info.block_shift) - 1);

		cmd->rw.dptr_prp1 = cpu_to_le64(ctxt->prp1);
		set_bit(cmd_id, q->id_bitmap);
		++q->used_ids;
		if (++sq_tail == q->sq_len)
			sq_tail = 0;
		_ND(trace_1_nvme_local_submit_bios, "cmd->rw.length=@LENGTH_INT (@LE16_TO_CPU) bio=@BIO", (u16)cmd->rw.length, le16_to_cpu(cmd->rw.length), bio);
#if KS_BI_REMAINING
		bio_advance(bio, len);
		if (bio_has_data(bio)) {
#if KS_BI_INC_REMAINING
			bio_inc_remaining(bio);
#elif KS_BI_REMAINING_UNDERSCORE
			bio_set_flag(bio, BIO_CHAIN);
			atomic_inc(&bio->__bi_remaining);
#else
			atomic_inc(&bio->bi_remaining);
#endif
#if KS_BI_REMAINING_UNDERSCORE
			_ND(trace_local_submit_bios, "bi_remaining=@COUNT bio=@BIO", atomic_read(&bio->__bi_remaining), bio);
#else
			_ND(trace_local_submit_bios, "bi_remaining=@COUNT bio=@BIO", atomic_read(&bio->bi_remaining), bio);
#endif
			continue;
		}
#endif
cont:
		bio_list_pop(&drv->bio_list[q->id - 1]);
		while (drv != NULL && (bio = bio_list_peek(&drv->bio_list[q->id - 1]))
			== NULL)
			drv = drv->next;
	}
	if (q->sq_tail != sq_tail) {
		writel(sq_tail, q->sq_doorbell);
		q->sq_tail = sq_tail;
	}
}

static REQ_RET
#if !KS_REQUEST_QUEUE_HAS_REQUEST_FN
	nvmeibs_make_request(struct bio *bio)
#else
	nvmeibs_make_request(struct request_queue *q, struct bio *bio)
#endif
{
#if !KS_REQUEST_QUEUE_HAS_REQUEST_FN
	struct request_queue *q = bio_gendisk(bio)->queue;
#endif
	struct drive_params *drv = q->queuedata;
	struct device_data *d = drv->dev;
	unsigned long flags;
	int qid;
	struct nvme_qp *qp;

#if KS_NO_BIO_IS_RW
	if (bio_op(bio) != REQ_OP_DISCARD && !((bio_op(bio) == REQ_OP_READ) || (bio_op(bio) == REQ_OP_WRITE))) {
		_NE(error_nvmeibs_make_request, "bio @BIO not supported bio_op=@BIO_OP",
		    bio, (unsigned long)bio_op(bio));
#elif defined(REQ_OP_BITS)
	if (bio_op(bio) != REQ_OP_DISCARD && !bio_is_rw(bio)) {
		_NE(error_nvmeibs_make_request, "bio @BIO not supported bio_op=@BIO_OP", bio, bio_op(bio));
#elif defined(BIO_DISCARD)
	if (!(bio->bi_rw & BIO_DISCARD) && !bio_is_rw(bio)) {
		_NE(error_nvmeibs_make_request, "bio @BIO not supported bio_op=@BIO_OP", bio, bio_op(bio));
#else
	if (!(bio->bi_rw & REQ_DISCARD) && !bio_is_rw(bio)) {
		_NE(error_nvme_nvmeibs_make_request, "bio @BIO not supported bio_op=@BIO_OP", bio, bio->bi_rw);
#endif
		bio_io_error(bio);
		goto out;
	}

	if ((qid = ioqm_get_q_spin_lock(d, &flags)) < 0) {
		bio_endio(bio, -ENXIO);
		goto out;
	}
	qp = d->local_ioq[qid];

	if (d->need_reset || d->reset_pending || d->removed) {
		bio_endio(bio, -ENXIO);
	} else if (d->formatting) {
		bio_endio(bio, -EBUSY);
	} else {
		bio_list_add(&drv->bio_list[qp->id - 1], bio);
		local_submit_bios(qp);
	}
	spin_unlock_irqrestore(&qp->q_lock, flags);
out:;
	return REQ_RET_ZERO;
}

static const struct block_device_operations nvmeibs_fops = {
	.owner        = THIS_MODULE,
	.open         = nvmeibs_open,
	.release      = nvmeibs_release,
	.ioctl        = nvmeibs_ioctl,
	.compat_ioctl = nvmeibs_ioctl,
#if !KS_REQUEST_QUEUE_HAS_REQUEST_FN
	.submit_bio = nvmeibs_make_request,
#endif
};

static void local_req_cb(void *arg, int status, u32 result)
{
	struct nvmeibs_nvme_req *req = arg;
	nvmeibs_disk_record_stats(req->disk_info, req, status); //consider @result
	BUG_ON(!req->use_sg);
	if (status != 0)
		req->status = status;
	if (atomic_dec_and_test(&req->parts_out)) {
		struct device *dev = &req->disk_info->dev->pci_dev->dev;
		enum dma_data_direction dir =
			req->nvme_op == nvme_cmd_read ? DMA_FROM_DEVICE : DMA_TO_DEVICE;
		if (!req->sg_already_mapped) {
			dma_unmap_sg(dev, req->table.sgl, req->table.orig_nents, dir);
		}
		if (req->metadata && !req->sg_md_already_mapped)
			dma_unmap_single(dev, req->mtdt_dma, req->mtdt_size, dir);
		(req->cb)(req->arg, req->status, result);
	}
}

static void free_prpl_cb(void *arg, int status, u32 result)
{
	dma_addr_t *prpl = arg;
	struct nvmeibs_nvme_req *req = (void *)prpl[0];
#if 0
	struct prpl_list *prpl = arg;
	struct prpl_list *old;

	prpl->dev = &req->disk_info->dev->pci_dev->dev;
	do {
		prpl->next = old = prpl_freelist;
	} while (cmpxchg(&prpl_freelist, old, prpl) != old);
#endif
	dma_unmap_single(&req->disk_info->dev->pci_dev->dev, prpl[1], PAGE_SIZE, DMA_TO_DEVICE);
	kfree(prpl);
	// dma_free_coherent(&req->disk_info->dev->pci_dev->dev, PAGE_SIZE, prpl, prpl[1]);
	local_req_cb(req, status, result);
}

extern bool nvmeibs_disk_collect_stats;

/* Trampoline to update stats */
static void remote_iops_stats_cb(void *arg, int status, u32 result)
{
	struct nvmeibs_nvme_req *req = arg;

	/* Collect stats */
	nvmeibs_disk_record_stats(req->disk_info, req, status);

	/* Call req callback */
	(*req->cb)(req->arg, status, result);
}

static void process_remote_iops(struct device_data *d, int qid)
{
	struct nvme_qp *q = d->local_ioq[qid];
	int id = 0;
	struct nvmeibs_nvme_req *req;
	struct nvme_command *cmd;
	int sq_tail = q->sq_tail;
	size_t m_size;
	bool unaligned;
	int n = 0;

	if (d->need_reset || d->reset_pending || d->removed)
		return;
	while (!list_empty(&d->remote_iops_q[qid]) &&
			sq_tail + 1 != (q->sq_head ? : q->sq_len) &&
			(q->used_ids < q->total_ids)) {
		req = list_first_entry(&d->remote_iops_q[qid], struct nvmeibs_nvme_req,
			link);
		req->nvme_qp_stats = &q->nvme_qp_stats;

		id = find_next_zero_bit(q->id_bitmap, q->total_ids, id);
		BUG_ON(id >= q->total_ids);
		set_bit(id, q->id_bitmap);
		if (nvmeibs_disk_collect_stats) {
			/* Use trampoline to collect stats */
			q->ids[id].callback = remote_iops_stats_cb;
			q->ids[id].arg = req;
		} else {
			q->ids[id].callback = req->cb;
			q->ids[id].arg = req->arg;
		}
		q->ids[id].issue_time = jiffies + q->used_ids * HZ;
		q->ids[id].aborted = false;
		++q->used_ids;

		NVMEIB_LOG_GOODPATH("@CORE_SERVER_OP_DUMP",
							_T, goodpath_nvmeibs, trace_process_remote_iops,
							req->nvme_op, req->disk_block, req->data_len);

		cmd = &q->sq[sq_tail];
		if (++sq_tail == q->sq_len)
			sq_tail = 0;
		memset(cmd, 0, sizeof(*cmd));
		cmd->common.opcode = req->nvme_op;
		cmd->common.flags = 0;
		cmd->common.command_id = id;
		// cmd->common.nsid = d->drives->nsid;
		cmd->common.nsid = req->disk_info->drv->nsid;

		if (req->nvme_op == nvme_cmd_dsm) {
//			struct nvmeibs_dsm_range *dp =
//				req->use_sg ? sg_virt(req->table.sgl) :
//								phys_to_virt(req->buf_addrs[0]);
//			_NT(process_remote_iops_t1, "TRIM req=@PTR slba=@_X nlb=@INT32_HEX data_len=@LD dp=@PTR", req, dp->slba, dp->nlb, req->data_len, dp);
			((struct nvmeib_dsm_cmd *)cmd)->nr = cpu_to_le32((req->data_len >> 4) - 1);
			((struct nvmeib_dsm_cmd *)cmd)->attributes = cpu_to_le32(NVME_DSMGMT_AD);
		} else {
			cmd->rw.slba = req->use_hw_blocks ? cpu_to_le64(req->disk_block) :
					cpu_to_le64(req->disk_block << (NVMEIBC_SECTOR_SHIFT - d->drives->info.block_shift));
			cmd->rw.length = cpu_to_le16((req->data_len >> d->drives->info.block_shift) - 1);
		}

		if (req->nvme_op == nvme_cmd_write_uncor) {
			list_del_init(&req->link);
		} else if (req->nvme_op == nvme_cmd_write_zeroes) {
			list_del_init(&req->link);
		} else if (req->use_sg) {
			/* -------------------------------------------------------------- *
			 * Local disk access
			 * -------------------------------------------------------------- */
			int prp_i;
			dma_addr_t prpl_dma;
			void **prpl = 0;
			u64 prp2 = 0;
			size_t length = 0;

			/* First part/nvme-cmd of this ulp-req */
			if (atomic_read(&req->parts_out) == 0) {
				enum dma_data_direction dir = req->nvme_op == nvme_cmd_read ?
											DMA_FROM_DEVICE : DMA_TO_DEVICE;
				req->table.orig_nents = req->table.nents;
				if (!req->sg_already_mapped) {
					req->table.nents = dma_map_sg(&d->pci_dev->dev,
										req->table.sgl, req->table.nents, dir);
				}
				if (req->table.nents == 0) {
					_NE(error_2_nvme_process_remote_iops, "Error DMA mapping to nvme device");
					req->table.nents = req->table.orig_nents;
					--q->used_ids;
					clear_bit(id, q->id_bitmap);
					if (sq_tail == 0)
						sq_tail = q->sq_len;
					--sq_tail;
					goto out;
				}
				if (req->metadata && req->mtdt_size) {
					m_size = (req->data_len >> d->drives->info.block_shift) *
						d->drives->metadata;
					if (req->mtdt_size != m_size) {
						_NE(error_process_remote_iops, "metatdata WRONG size @M_SIZE, should be @M_SIZE", req->mtdt_size, m_size);
						BUG();
					}
					if (!req->sg_md_already_mapped) {
						req->mtdt_dma = dma_map_single(&d->pci_dev->dev,
										req->metadata, req->mtdt_size, dir);
						if (dma_mapping_error(&d->pci_dev->dev, req->mtdt_dma)) {
							_NE(error_1_process_remote_iops, "Failed dma_map for metdata size=@M_SIZE", req->mtdt_size);
							(*req->cb)(req->arg, -ENOMEM, 0);
							dma_unmap_sg(&d->pci_dev->dev, req->table.sgl, req->table.orig_nents, dir);
							list_del_init(&req->link);
							goto out;
						}
					}
					req->mtdt_dma_ptr = req->mtdt_dma;
				}

				req->sgp = req->table.sgl;
				req->resid_len = req->data_len;
				/* Init to 1; each part/nvme-cmd of this req BUT THE LAST will inc before posting to drive.
				   This way we dont complete to ulp before all parts were posted && completed
				   This way we also handle the case we fail posting a middle part after all prev parts had completed */
				atomic_set(&req->parts_out, 1);
			}

			q->ids[id].callback = local_req_cb;
			q->ids[id].arg = req;
			if (req->metadata && req->mtdt_size) {
				if(d->drives->mtdt_extd)
					prp2 = req->mtdt_dma_ptr;
				else
					cmd->rw.metadata = cpu_to_le64(req->mtdt_dma_ptr);
			}
			unaligned = false;
			cmd->common.dptr_prp1 = sg_dma_address(req->sgp);
			/* prp1 = from first sg-chunk till 1st page-boundary
			   and initialize loop's vars to point passed prp1 */
			if (sg_dma_len(req->sgp) + (sg_dma_address(req->sgp) & ~PAGE_MASK) <= PAGE_SIZE) {
				if (sg_dma_len(req->sgp) + (sg_dma_address(req->sgp) & ~PAGE_MASK) < PAGE_SIZE)
					unaligned = true;
				length += sg_dma_len(req->sgp);
				req->sgp = sg_next(req->sgp);
				--req->table.nents;
			}
			else {
				int len = PAGE_SIZE - (sg_dma_address(req->sgp) & ~PAGE_MASK);
				length += len;
				sg_dma_address(req->sgp) += len;
				sg_dma_len(req->sgp) -= len;
			}
			BUG_ON(req->metadata && req->sgp != NULL && d->drives->mtdt_extd);

			/* we use only last 510 prp-entries in prp-list as we stash info in
			   the first 2 entries needed for completing each part:
			   - prp-list[0] points to @req
			   - prp-list[1] points to @prpl_dma
			   - Note, callback's arg is @prpl.

			   why not saving this info in the @req itself?
			   because we may have multiple (unknown?) parts/nvme-cmds for this
			   single ulp-req namely if it has many unaligned middle pages with
			   gap>2 btw them such that we are forced to use multiple prp-lists
			 */

			for (prp_i = 1; req->sgp != NULL && prp_i < 510 && req->table.nents > 0 &&
					!unaligned && (sg_dma_address(req->sgp) & ~PAGE_MASK) == 0; prp_i++) {
				if (prp_i == 2) {
					/* we need more than 2 prp entries -> alloc 1 page for prp-list */
				#ifdef CONFIG_NUMA
					prpl = kmalloc_node(PAGE_SIZE, GFP_NOWAIT, d->pci_dev->dev.numa_node);
				#else
					prpl = kmalloc(PAGE_SIZE, GFP_NOWAIT);
				#endif
					if (prpl == NULL)
						break;
					prpl[0] = (void *)req;
					q->ids[id].callback = free_prpl_cb;
					q->ids[id].arg = prpl;
					prpl[2] = (void *)cpu_to_le64(prp2); /* prp2 was set by prev-iter */
				}
				if (prp_i == 1)
					prp2 = sg_dma_address(req->sgp);
				else
					prpl[prp_i+1] = (void *)cpu_to_le64(sg_dma_address(req->sgp));
				if (sg_dma_len(req->sgp) <= PAGE_SIZE) {
					if (sg_dma_len(req->sgp) < PAGE_SIZE)
						unaligned = true;
					length += sg_dma_len(req->sgp);
					req->sgp = sg_next(req->sgp);
					--req->table.nents;
				}
				else {
					length += PAGE_SIZE;
					sg_dma_address(req->sgp) += PAGE_SIZE;
					sg_dma_len(req->sgp) -= PAGE_SIZE;
				}
			}
			req->disk_block += (length >> NVMEIBC_SECTOR_SHIFT);
			req->mtdt_dma_ptr +=
				(d->drives->metadata * length) >> d->drives->info.block_shift;
			req->resid_len -= length;
			if (req->resid_len == 0) {
				list_del_init(&req->link);
			} else {
				atomic_inc(&req->parts_out);
			}

			/* if we've used prp-list, map it to dev and point it from prpl[1] */
			if (prp_i > 2) {
				prpl_dma = dma_map_single(&d->pci_dev->dev, prpl, PAGE_SIZE, DMA_TO_DEVICE);
				if (dma_mapping_error(&d->pci_dev->dev, prpl_dma)) {
					kfree(prpl);
					if (req->resid_len != 0)
						list_del_init(&req->link);
					(*req->cb)(req->arg, -ENOMEM, 0);
					break;
				}
				prpl[1] = (void *)prpl_dma;
				prp2 = prpl_dma + 2*sizeof(prpl[0]);
			}
			/* else, @prp2 holds 2nd and last prp-entry */

			/* @prp2 is now either prp-entry2 or prp-list */
			cmd->common.dptr_prp2 = cpu_to_le64(prp2);
			BUG_ON(length == 0);
			if (req->nvme_op != nvme_cmd_dsm) {
				if (length & ((1 << d->drives->info.block_shift) - 1)) {
					_NE(error_3_process_remote_iops, "BAD LENGTH length=@LEN_SIZET prp_i=@IDX data_len=@LEN_SIZET resid_len=@LEN_SIZET", length, prp_i,
						req->data_len, req->resid_len);
					BUG();
				}
				cmd->rw.length = cpu_to_le16((length >> d->drives->info.block_shift) - 1);
			}
		} else {
			/* -------------------------------------------------------------- *
			 * Non local disk access (No-RDDA)
			 * -------------------------------------------------------------- */
			size_t data_len = req->data_len;
			m_size = (data_len >> d->drives->info.block_shift) *
				d->drives->metadata;
			if (req->metadata) {
				if (req->mtdt_size != m_size) {
					_NE(error_3_nvme_process_remote_iops, "metatdata WRONG size @MTDT_SIZE_LONG, should be @M_SIZE", req->mtdt_size, m_size);
					BUG();
				}
				if (!req->mtdt_dma_ptr) {
					_NE(error_4_nvme_process_remote_iops, "req @REQ, metatdata dma addr not initialized", req);
					BUG();
				}
			}
			if(d->drives->mtdt_extd)
				data_len += m_size;
			else
				cmd->rw.metadata = cpu_to_le64(req->mtdt_dma_ptr);
			cmd->common.dptr_prp1 = req->buf_addrs[req->buf_offset >> PAGE_SHIFT] |
				(req->buf_offset & ~PAGE_MASK);
			if (data_len + (cmd->common.dptr_prp1 & ~PAGE_MASK) > PAGE_SIZE) {
				if (data_len + (cmd->common.dptr_prp1 & ~PAGE_MASK) <= 2*PAGE_SIZE) {
					cmd->common.dptr_prp2 =
						req->buf_addrs[(req->buf_offset >> PAGE_SHIFT)+1];
				}
				else {
					if ((req->prpl_phys & PAGE_MASK) !=
						((req->prpl_phys + sizeof(u64)*((req->buf_offset+data_len-1)
											>> PAGE_SHIFT)) & PAGE_MASK)) {
						_NE(error_5_nvme_process_remote_iops, "Misaligned prplist for remoted io - "
						   "prpl_phys=@PRPL_PHYS, req->buf_offset=@BUF_OFFSET_LLONG, data_len=@DATA_LEN_LLONG",
							req->prpl_phys, (u64)req->buf_offset, (u64)data_len);
						BUG();
					}
					cmd->common.dptr_prp2 = cpu_to_le64(req->prpl_phys +
						sizeof(u64)*((req->buf_offset >> PAGE_SHIFT) + 1));
				}
			}
			list_del_init(&req->link);
		}
		n++;
	}
out:
	if (q->sq_tail != sq_tail) {
		writel(sq_tail, q->sq_doorbell);
		q->sq_tail = sq_tail;
	}
	nvmeib_qp_stats_on_post_send_add(q->qp_stats, n);
}

#if KS_ENDIO_1ARG
static void ext_bio_done(struct bio *bio)
{
	struct nvmeibs_nvme_req *req = bio->bi_private;

	req->cb(req->arg, bio_error(bio), 0);
	bio_put(bio);
}
#else
static void ext_bio_done(struct bio *bio, int error)
{
	struct nvmeibs_nvme_req *req = bio->bi_private;

	req->cb(req->arg, error, 0);
	bio_put(bio);
	kref_put(&req->disk_info->external->done_kref, done_kref_release);
}
#endif

static void submit_external_iop(struct work_struct *work)
{
	struct nvmeibs_nvme_req *req =
			container_of(work, struct nvmeibs_nvme_req, work);
	struct external_drive *d = req->disk_info->external;
	struct bio *bio = NULL;
	int npages = (req->data_len + PAGE_SIZE - 1) >> PAGE_SHIFT;
	int rw;
	int i;
	sector_t disk_block = req->disk_block;
	int err = 0;

	if (!req->use_hw_blocks)
		disk_block <<= (NVMEIBC_SECTOR_SHIFT - d->info.block_shift);
	if (req->nvme_op == nvme_cmd_dsm) {
		struct nvmeibs_dsm_range *dsm_ptr;

		if (req->use_sg)
			dsm_ptr = sg_virt(req->table.sgl);
		else
			dsm_ptr = (void *)req->buf_addrs[0];
		_NT(trace_nvme_submit_external_iop, "discard(slba=@SLBA_LONG nlb=@NLB) <<@SHIFT",
				(long)dsm_ptr->slba, (long)dsm_ptr->nlb,
				d->info.block_shift - 9);
#if KS_BLKDEV_ISSUE_DISCARD_HAS_FLAGS
		err = blkdev_issue_discard(d->block_dev,
			le64_to_cpu(dsm_ptr->slba)<< (d->info.block_shift - 9),
			le64_to_cpu(dsm_ptr->nlb) << (d->info.block_shift - 9), GFP_KERNEL, 0);
#else
		err = blkdev_issue_discard(ed2bd(d),
					   le64_to_cpu(dsm_ptr->slba)<< (d->info.block_shift - 9),
					   le64_to_cpu(dsm_ptr->nlb) << (d->info.block_shift - 9), GFP_KERNEL);
#endif
		(*req->cb)(req->arg, err, 0);
		kref_put(&req->disk_info->external->done_kref, done_kref_release);
		if (err) {
			_NE(error_nvme_submit_external_iop, "discard Error @ERR", err);
		}
	} else if (req->nvme_op == nvme_cmd_write_zeroes) {
		blkdev_issue_zeroout(ed2bd(d), disk_block,
            req->data_len >> 9, GFP_KERNEL
#if KS_ZEROOUT_DISCARD
			, false
#endif
			);
	} else {	/* It's an I/O command */
		if (req->nvme_op == nvme_cmd_read)
			rw = READ;
		else if (req->nvme_op == nvme_cmd_write)
			rw = WRITE;
		else {
			err = -EINVAL;
			goto err;
		}
		err = -ENOMEM;
#if KS_BIO_ALLOC_HAS_BLOCK_DEVICE
		bio = bio_alloc(ed2bd(d), npages, 0, GFP_KERNEL);
#else
		bio = bio_alloc(GFP_KERNEL, npages);
#endif
		if (bio == NULL)
			goto err;
		nvmeib_bio_set_dev(bio, ed2bd(d));
		if (req->use_sg) {
			struct scatterlist *s;
			int n = 0;
			for (i = 0, s = req->table.sgl; s != NULL && i < req->table.nents;
					s = sg_next(s), i++) {
				if (!bio_add_page(bio, sg_page(s), s->length, s->offset))
					goto err;
				++n;
				BUG_ON(n > npages);
			}
		}
		else {
			/* buf_addrs list contains struct page ptrs */
			for (i = 0; i < req->data_len >> PAGE_SHIFT; i++) {
				WARN_ON(req->buf_addrs[i] & (~PAGE_MASK));
				if(!bio_add_page(bio, virt_to_page(req->buf_addrs[i]),
					PAGE_SIZE, 0)) {
						/* Usually means that we added more sectors than allowed */
						WARN_ON(1);
						goto err;
				}
			}
			if ((req->data_len & ~PAGE_MASK) != 0) {
				WARN_ON(req->buf_addrs[i] & (~PAGE_MASK));
				if(!bio_add_page(bio, virt_to_page(req->buf_addrs[i]),
					req->data_len & ~PAGE_MASK, 0)) {
						WARN_ON(1);
						goto err;
					}
			}
		}

#if KS_BVEC_ITER
		/* sectors are always in a units of 512 bytes so we need to convert again */
		bio->bi_iter.bi_sector = disk_block << (d->info.block_shift - 9);
#else
		bio->bi_sector = disk_block << (d->info.block_shift - 9);
#endif
		bio->bi_private = req;
		bio->bi_end_io = ext_bio_done;

#ifdef REQ_OP_BITS
#if KS_HAS_BIO_SET_OP_ATTRS
		bio_set_op_attrs(bio, rw, 0);
#else
		bio->bi_opf = (bio->bi_opf & ~REQ_OP_MASK) | rw;
#endif
		submit_bio(bio);
#else
		submit_bio(rw, bio);
#endif
		return;
err:
		(*req->cb)(req->arg, err, 0);
		if (bio != NULL)
			bio_put(bio);
		kref_put(&req->disk_info->external->done_kref, done_kref_release);
	}
}

int submit_local_cmd(struct nvmeibs_disk_info *d_info,
					 struct nvmeibs_nvme_req *req)
{
	struct device_data *d = d_info->dev;

	int qid;
	struct nvme_qp *q;
	ulong flags = 0;
	bool should_unlock;
	size_t mtdt_size;
	int rv = 0;
	bool op_uses_md = req->nvme_op == nvme_cmd_read ||
		req->nvme_op == nvme_cmd_write || req->nvme_op == nvme_cmd_compare;

	req->stats.start_time = nvmeib_public_ktime_get();

	NFIN;
	_ND(trace_nvme_submit_local_cmd, "submit_local_cmd d=@DEVICE_PTR req=@REQ", d, req);
	req->disk_info = d_info;
	if (d == NULL) {		// check for NVMeOF/SATA cases
		if (req->nvme_op == nvme_cmd_write_uncor) {
			rv = -EOPNOTSUPP;
			goto out;
		}
		if (d_info->external) {
			INIT_WORK(&req->work, submit_external_iop);
			kref_get(&d_info->external->done_kref);
			rv = schedule_work(&req->work) ? 0 : -EBUSY;
			if (rv) {
				kref_put(&d_info->external->done_kref, done_kref_release);
			}
			goto out;
		}
		else {
			rv = -ENODEV;
			goto out;
		}
	}
	if (req->nvme_op == nvme_cmd_dsm && !d->trim_supported) {
		rv = -EOPNOTSUPP;
		_NI(error_no_trim, "nvme@SEQ Trim not supported", d->seq);
		goto out;
	}
	if (req->metadata && d->drives->metadata == 0) {
		_NE(error_nvme_submit_local_cmd, "Drive does not support Metadata");
		rv = -EINVAL;
		goto out;
	}
	if (req->metadata == NULL &&
			d->drives->metadata && !d->drives->mtdt_extd &&
			op_uses_md) {
		_NE(error_1_nvme_submit_local_cmd, "Drive requires Metadata");
		rv = -EINVAL;
		goto out;
	}
	if (op_uses_md) {
		mtdt_size = (req->data_len >> d_info->block_shift) * d->drives->metadata;
		if (req->metadata && req->mtdt_size && req->mtdt_size != mtdt_size) {
			_NE(error_2_nvme_submit_local_cmd, "Metadata size wrong: recv @MTDT_SIZE_LONG exp @MTDT_SIZE_LONG",
				mtdt_size, req->mtdt_size);
			rv = -EMSGSIZE;
			goto out;
		}
		if (req->metadata && mtdt_size &&
			((u64)req->metadata & PAGE_MASK) != (((u64)req->metadata + mtdt_size - 1) & PAGE_MASK)) {
			_NE(error_misaligned, "Metadata misaligned. metadata=@METADATA size=@MTDT_SIZE\n", req->metadata, mtdt_size);
			rv = -EINVAL;
			goto out;
		}
	}

	/* This function can be called from comp-intr
	   path where q is already locked by this cpu */
	atomic_set(&req->parts_out, 0);
	req->status = 0;
	qid = ioqm_get_q_spin_lock_irqsave_recursive(d, &flags, &should_unlock, req->qid_hint_plus1);
	if (qid < 0) {
		rv = -ENXIO;
		goto out;
	}
	q = d->local_ioq[qid];

	if (d->need_reset || d->reset_pending || d->removed)
		rv = -ENXIO;
	else
		list_add_tail(&req->link, &d->remote_iops_q[qid]);
	process_remote_iops(d, qid);
	if (should_unlock) {
		q->locking_cpu = -1;
		spin_unlock_irqrestore(&q->q_lock, flags);
	}
out:
	NFOUT;
	return rv;
}

struct device *get_nvme_dma_device(struct device_data *dev)
{
	if (dev == NULL || dev->pci_dev == NULL)
		return NULL;
	return &dev->pci_dev->dev;
}

static void local_complete_fn(struct nvme_qp *q)
{
	local_submit_bios(q);
	process_remote_iops(q->dev, q->id - 1);
}

/* This function was the body of end_use_client_q().
   It was (and is) only treating ETIMEDOUT as error */
static inline int end_use_q(struct device_data *d, int qid)
{
	int rv;
	NFIN;

	_NT(trace_nvme_end_use_q, "Disk @SERIAL - Destroy NVME SQ/CQ @QID",
	   d->serial, qid);

	if ((rv = destroy_sq(d, qid)) != NVME_SC_SUCCESS) {
		if (rv == -ETIMEDOUT) {
			_NE(error_nvme_end_use_q, "Timeout destroying NVME Disk @SERIAL SQ @QID",
			   d->serial, qid);
			return rv;
		}

		_NE(error_1_nvme_end_use_q, "Error @RV destroying NVME Disk @SERIAL SQ @QID",
		   rv, d->serial, qid);
	}

	if ((rv = destroy_cq(d, qid)) != NVME_SC_SUCCESS) {
		if (rv == -ETIMEDOUT) {
			_NE(error_2_nvme_end_use_q, "Timeout destroying NVME Disk @SERIAL CQ @QID",
			   d->serial, qid);
			return rv;
		}

		_NE(error_3_nvme_end_use_q, "Error @RV destroying NVME Disk @SERIAL CQ @QID",
		   rv, d->serial, qid);
	}

	NFOUT;
	return rv;
}

//OL: return err if inuse (or if inner func returns rv < 0)
int end_use_client_q(struct nvmeibs_q_info *q)
{
	struct device_data *d = q->disk->dev;

	if (q->inuse) {
#ifdef PRINT_QS_ON_RESET
		struct nvme_completion *cq;
		int j;

		cq = q->cq;
		_NI(t0l_nsnvme, "cq(@INT) -> @PTR\n", q->qid, cq);
		for (j = 0; j < q->cq_len; j++, cq++)
			_NI(t0m_nsnvme, "   entry @INT: result=@X sq_head=@X sq_id=@X cmd_id=@X status=@X",
		j, cq->result, cq->sq_head, cq->sq_id, cq->command_id, cq->status);
#endif // PRINT_QS_ON_RESET
		if (end_use_q(d, q->qid) == -ETIMEDOUT)
			return -ETIMEDOUT;
		q->inuse = false;
		_ND(trace_nvme_end_use_client_q, "qid @QID, marked as not inuse", q->qid);
	}

	return 0;
}

static void end_use_all_local_qs(struct device_data *d)
{
	struct nvme_qp *q;
	int qid;

	_NT(trace_nvme_end_use_all_local_qs,
		"dev @DEVICE_PTR, end use @MIN_LOCAL_IOQS (@INT) local qs",
		d, d->min_local_ioqs, d->ioqm.n_virt_lioqs);

	/* EC-6183: changed loop count from max-local_ioqs min-local_ioqs
	   w/o dealing with ioqm (unused feature), hence the WARN-ON */
	WARN_ON(d->min_local_ioqs != d->ioqm.n_virt_lioqs);

	for (qid = 0; qid < d->min_local_ioqs; ++qid) {
		if ((q = d->local_ioq[qid])) {
			_NT(trace_1_nvme_end_use_all_local_qs, "end use local q @QUEUE (qid @QID)", q, qid);
			end_use_local_q(q, true);
		}
	}
}

static void end_use_all_qs(struct device_data *d)
{
	struct nvmeibs_q_info *q;
	int ii;

	if (d->qs_info != NULL) {
		_NT(trace_nvme_end_use_all_qs, "dev @DEVICE_PTR, end use @MAX_CLIENT_QS client qs", d, d->max_client_qs);
		for (ii = 0; ii < d->max_client_qs; ii++) {
			q = &d->qs_info[d->min_local_ioqs + ii];
			_NT(trace_1_nvme_end_use_all_qs, "[@II], q @QUEUE (qid @QID)", ii, q, q->qid);
			if (end_use_client_q(q) == -ETIMEDOUT) {
				/* Drive is not responding to Admin commands - give up */
				break;
			}
		}
	}
}

static void free_client_qs(struct device_data *d)
{
	struct nvmeibs_q_info *q;
	struct device *dev;
	int i, ii;

	dev = &d->pci_dev->dev;
	if (d->qs_info != NULL) {
		for (ii = 0; ii < d->max_client_qs; ii++) {
			q = &d->qs_info[d->min_local_ioqs + ii];
			_ND(trace_nvme_free_client_qs, "Free client q @QUEUE, qid @QID", q, q->qid);
			if (q->prpl != NULL)
				dma_free_coherent(dev, PAGE_SIZE, q->prpl, q->prpl_phys);
			if (q->sq != NULL)
				dma_free_coherent(dev, PAGE_SIZE, q->sq, q->sq_phys);
			if (q->cq != NULL)
				dma_free_coherent(dev, PAGE_SIZE, q->cq, q->cq_phys);
			if (q->bb_addr_virt != NULL) {
				for (i = 0; i < q->bb_npages; i++)
					if (q->bb_addr_virt[i] != NULL)
						dma_free_coherent(dev, PAGE_SIZE, q->bb_addr_virt[i],
							q->bb_addr_nvme[i]);
				kfree(q->bb_addr_virt);
			}
			kfree(q->bb_addr_nvme);
		}
		kfree(d->qs_info);
		d->qs_info = NULL;
	}
	if (d->drives) {
		d->drives->info.qs = NULL;
		d->drives->info.n_qs = 0;
	}
}
#if 0 /* DEBUG ONLY */
static irqreturn_t nvmeibs_intr_debug(int irq, void *arg)
{
	struct nvmeibs_q_info *q = arg;
	nvmeibs_trigger_ib(q);
	return IRQ_HANDLED;
}
#endif

int nvmeibs_nvme_max_io_bb(struct device_data *d)
{
	int npages = NVMEIBS_MAX_BOUNCE_BUFFER_PAGES;

	if (d) {
		if (d->max_transfer > 0 && (d->max_transfer >> PAGE_SHIFT) < npages) {
			npages = (d->max_transfer >> PAGE_SHIFT);
			/* Add a page for MD even if there is none because the format might change */
			npages++;
		}
	}
	if (npages > PAGE_SIZE / sizeof(u64))	/* 1 page prp list */
		npages = PAGE_SIZE / sizeof(u64);
	return npages;
}

static int alloc_client_qs(struct device_data *d)
{
	struct nvmeibs_q_info *q;
	int qid;
	struct device *dev = &d->pci_dev->dev;
	size_t qmax;
	int err = -ENOMEM;
	int i, ii;
	int npages = nvmeibs_nvme_max_io_bb(d);

	_NT(trace_nvme_alloc_client_qs, "disk: @SERIAL - max_transfer: @MAX_TRANSFER, metadata: @METADATA_INT => npages for allocation = @NPAGES",
	   d->serial, d->max_transfer, d->drives->metadata, npages);

	if (d->max_client_qs > 0 &&
		(d->qs_info = kcalloc(d->max_ioqs, sizeof(*q), GFP_KERNEL)) == NULL)
		return -ENOMEM;
	for (ii = 0; ii < d->max_client_qs; ii++) {
		qid = 1 + d->min_local_ioqs + ii;
		q = &d->qs_info[qid -1];
		q->qid = qid;
		q->bb_addr_nvme = kcalloc(npages, sizeof(dma_addr_t), GFP_KERNEL);
		q->bb_addr_virt = kcalloc(npages, sizeof(void *), GFP_KERNEL);

		if (!q->bb_addr_nvme || !q->bb_addr_virt)
			goto out;

		q->bb_npages = npages;
		for (i = 0; i < npages; i++) {
			q->bb_addr_virt[i] = dma_alloc_coherent(dev,
								PAGE_SIZE,
								&q->bb_addr_nvme[i],
								GFP_KERNEL);
			if (!q->bb_addr_virt[i])
				goto out;
		}

		/* We always allocate page for MD because format might change */
		q->bb_mtdt_nvme = q->bb_addr_nvme[npages - 1];
		q->bb_mtdt_virt = q->bb_addr_virt[npages - 1];
		if (q->bb_mtdt_virt != NULL)
			q->bb_mtdt_nvme_len = PAGE_SIZE;

		qmax = NVME_CAP_MQES(d->cap) + 1;

		dev = &d->pci_dev->dev;
		/* please pay attention that the queue size is used in other places */
		q->cq = dma_alloc_coherent(dev, PAGE_SIZE, &q->cq_phys, GFP_KERNEL);
		q->sq = dma_alloc_coherent(dev, PAGE_SIZE, &q->sq_phys, GFP_KERNEL);
		q->prpl = dma_alloc_coherent(dev, PAGE_SIZE, &q->prpl_phys, GFP_KERNEL);

		if (q->sq == NULL || q->cq == NULL || q->prpl == NULL)
			goto out;

		for (i = 1; i < npages; i++)
			q->prpl[i-1] = q->bb_addr_nvme[i];

		q->cq_len = min(PAGE_SIZE/sizeof(*q->cq), qmax);
		q->cq_db_phys = d->mmio_phys + 4096 + (2*qid+1)*d->doorbell_stride;
		q->cq_doorbell = (void *)d->mmio + 4096 + (2*qid+1)*d->doorbell_stride;

		q->sq_len = min(PAGE_SIZE/sizeof(*q->sq), qmax);
		q->sq_db_phys = d->mmio_phys + 4096 + 2*qid*d->doorbell_stride;
		q->sq_doorbell = (void *)d->mmio + 4096 + 2*qid*d->doorbell_stride;
		q->msix_vec = d->msix_table + qid*PCI_MSIX_ENTRY_SIZE;

		_ND(trace_1_nvme_alloc_client_qs, "[@II] allocated client q @QUEUE qid @QID", ii, q, qid);
	}
	return 0;
out:
	free_client_qs(d);
	return err;
}

static struct device_data *alloc_dev_data(int max_msix)
{
	struct device_data *d;
	int qid;
	int n = max_msix;

	if(!(d = kzalloc(sizeof(*d), GFP_KERNEL)) ||
	   !(d->msix_entries = kzalloc(n * sizeof(*d->msix_entries) ,GFP_KERNEL)) ||
	   !(d->local_ioq = kzalloc(n * sizeof(*d->local_ioq) ,GFP_KERNEL)) ||
	   !(d->remote_iops_q = kzalloc(n * sizeof(*d->remote_iops_q) ,GFP_KERNEL)) ||
	   ioqm_alloc(d, n) < 0) {

		if (d) {
			ioqm_free(d);
			kfree(d->remote_iops_q);
			kfree(d->local_ioq);
			kfree(d->msix_entries);
			kfree(d);
		}
		return NULL;
	}

	mutex_init(&d->dev_lock);
	kref_init(&d->kref);
	kref_get(&d->kref);
	for (qid = 0; qid < n /*MAX_LOCAL_IOQS*/; ++qid) {
		INIT_LIST_HEAD(&d->remote_iops_q[qid]);
	}
	INIT_LIST_HEAD(&d->cdev.list);
	init_completion(&d->reset_done);
	return d;
}

static void nvmeibs_free_drives(struct kref *kref)
{
    struct device_data *d = container_of(kref, struct device_data, kref);
	struct pci_dev *pdev = d->pci_dev;
	struct drive_params *drv, *next;
	struct nvme_qp *q;
	int qid;
	struct completion *free_drv_comp;
	unsigned long flags;

	spin_lock_irqsave(&d->free_drv_comp_lock, flags);
	free_drv_comp = d->free_drv_comp;
	spin_unlock_irqrestore(&d->free_drv_comp_lock, flags);

	_NI(trace_nvme_nvmeibs_free_drives, "Free nvme@SEQ (pci_dev=@PCI_DEV, d=@DEVICE_PTR)", d->seq, pdev, d);
	for (qid = 0; qid < d->max_ioqs; ++qid) {
		if ((q = d->local_ioq[qid]))
			free_qp(q);
		d->local_ioq[qid] = 0;
	}
	if (d->adminq)
		free_qp(d->adminq);

	/* Only now, after free-qp() did dma-free using
	   d->pci_dev->dev, do kref-put of the pci-dev */
	pci_dev_put(pdev);

	for (drv = d->drives; drv != NULL; drv = next) {
		next = drv->next;
		kfree(drv->bio_list);
		nvmeib_io_stats_free(drv->info.io_stats);
		kfree(drv);
	}
	ioqm_free(d);
	kfree(d->remote_iops_q);
	kfree(d->local_ioq);
	kfree(d->msix_entries);
	kfree(d);

	if (free_drv_comp)
		complete(free_drv_comp);
}

static void abandon_outstanding(struct device_data *d);

static void free_drives(struct device_data *d)
{
	struct drive_params *drv, *next;

	for (drv = d->drives; drv != NULL; drv = next) {
		// struct nvmeibs_disk_info *info = &drv->info;

		next = drv->next;

		// nvmeibs_deregister_disk_resources(NULL, info);

		if (drv->gendisk)
			del_gendisk(drv->gendisk);
	}
}

static bool free_dev_data(struct device_data *d)
{
	free_client_qs(d);
	return kref_put(&d->kref, nvmeibs_free_drives) != 0;
}

static void nvmeibs_shutdown(struct pci_dev *pdev)
{
	struct device_data *d = pci_get_drvdata(pdev);

	if (!d) {
		_NT(trace_nvme_nvmeibs_shutdown, "Skipping shutdown, device was already removed ...");
		return;
	}

	if (irqs_disabled()) {
		_NE(error_nvme_nvmeibs_shutdown, "irqs disabled, cant use mutex");
		BUG();
	}
	/* block main kthread from clearing d->removed
	   during abandon-outstanding */
	mutex_lock(&d->dev_lock);
	free_drives(d);
	_dev_shutdown(d);
	d->removed = true;
	abandon_outstanding(d);
	mutex_unlock(&d->dev_lock);
}

static void kthread_process_drive_cq_work_func(struct work_struct *work)
{
	struct nvme_qp *q = container_of(work, struct nvme_qp, process_cq_work);
	unsigned long flags;
	int i = 0;

	spin_lock_irqsave(&q->q_lock, flags);
	if (q->state == LOCAL_Q_ON || q->state == LOCAL_Q_STOP_NEW_IO) {
		i = nvmeibs_process_cq(q);
		//_ND(trace_nvme_kthread_process_drive_cq_work_func_processed, "Work function processed @INT completions for drive @SERIAL qid=@QID", i, q->dev->serial, q->id);
		if (!is_cq_empty(q)) {
			/* resched itself */
			queue_work(nvmeibs_nvme_wq, &q->process_cq_work);
		}
	}
	spin_unlock_irqrestore(&q->q_lock, flags);
}

/*
 * This function can be called either
 * 1) after (back from) nvme-reset   --> q state can be any but
 * 2) when client returns the queues --> q state is OFF.
 *
 * Thus in both cases we need to:
 * a) create cq/sq and polling-kthread and;
 * b) setup msix and irq if not already setup.
 */
static inline int reuse_local_q_(struct nvme_qp *q)
{
	struct device_data *d = q->dev;
	int rv = -1;
	NFIN;

	_NT(trace_nvme_reuse_local_q, "Disk @SERIAL, qid=@QID (state @IOQ_STATE_STR)",
		d->serial, q->id, ioq_state_str(q->state));

	if ((rv = create_cq(d, q->id, q->cq_phys, q->cq_len, q->id)) < 0)
		goto out;
	if ((rv = create_sq(d, q->id, q->sq_phys, q->sq_len, q->id)) < 0)
		goto destroy_cq;

	/* re/enable irq */
	if (q->irq_state == LOCAL_Q_IRQ_NONE) {
		if ((rv = local_q_request_irq(q)) < 0)
			goto destroy_sq;
	}
	WARN_ON_ONCE(q->irq_state != LOCAL_Q_IRQ_ENABLE);

	/* start polling kthread */
	if (!nvmeibs_use_nvme_kwq) {
		WRITE_ONCE(q->polling, false);
		if (local_q_kthread_start_(q) < 0)
			goto free_irq;
	}

	q->state = LOCAL_Q_ON;
	rv = 0;
	goto out;

free_irq:
	local_q_free_irq(q);

destroy_sq:
	destroy_sq(d, q->id);

destroy_cq:
	destroy_cq(d, q->id);

out:
	NFOUT;
	return rv;
}
int use_client_q(struct nvmeibs_q_info *q)
{
	struct device_data *d = q->disk->dev;
	int qid = q->qid;
	int err;

	if (!q->inuse) {

		memset(q->cq, 0, PAGE_SIZE);
		if((err = create_cq(d, qid, q->cq_phys, q->cq_len, qid)) < 0)
			goto out;

		memset(q->sq, 0, PAGE_SIZE);
		if((err = create_sq(d, qid, q->sq_phys, q->sq_len, qid)) < 0) {
			destroy_cq(d, q->qid);
			goto out;
		}
		q->inuse = true;
		_ND(trace_nvme_use_client_q, "qid @QID, marked as inuse", q->qid);
	}
	else
		BUG();
	return 0;
out:
	return err;
}

struct abort_request {
	struct list_head link;
	struct device_data *d;
	u16 qid;
	u16 cmdid;
};

static int reinit_q(struct nvme_qp *q, bool full_init)
{
	ulong flags;
	int rv;

	if (q == NULL) {
		rv = -1;
		goto out;
	}

	//OM: why do we need the lock?
	spin_lock_irqsave(&q->q_lock, flags);
	q->cq_head = 0;
	q->old_cq_head = 0;
	q->cq_phase = 0;
	q->sq_head = 0;
	q->sq_tail = 0;
	memset(q->cq, 0, q->cq_len * sizeof(struct nvme_completion));
	q->used_ids = 0;
	memset(q->id_bitmap, 0, sizeof q->id_bitmap);
	memset(q->ids, 0, q->total_ids * sizeof(struct req_id));

	spin_unlock_irqrestore(&q->q_lock, flags);
	rv = full_init ? reuse_local_q_(q) : 0;
	q->dying = false;

out:
	NFOUT;
	return rv;
}

static int reinit_qs(struct device_data *d)
{
	struct nvme_qp *q;
	int qid;
	int rv = -1;

	NFIN;

	/* reinit admin-q */
	if (reinit_q(d->adminq, false) < 0) {
		_NT(trace_nvme_reinit_qs, "Fail to reinit adminq");
		goto out;
	}
	q_async_event(d);

	/* reinit local ioqs */
	for (qid = 0; qid < d->max_ioqs; ++qid) {
		if ((q = d->local_ioq[qid])) {
			bool full_init = qid < d->min_local_ioqs;
			if (reinit_q(q, full_init) < 0) {
				_NT(trace_1_nvme_reinit_qs, "Fail to reinit local-q qid @QID", qid);
				goto out;
			}
		}
	}
	ioqm_init(d);
	_NT(trace_2_nvme_reinit_qs, "dev @SERIAL (@DEVICE_PTR) reinit qs done", d->serial, d);
	rv = 0;

out:
	NFOUT;
	return rv;
}

static void thread_process_q(struct nvme_qp *q, struct list_head *abort_list)
{
	ulong current_time = jiffies;
	struct abort_request *a;
	unsigned long flags;
	int i;
	int n_aborts = 0;

	if (q == NULL)
		return;
	spin_lock_irqsave(&q->q_lock, flags);
	if (q->state == LOCAL_Q_ON || q->state == LOCAL_Q_STOP_NEW_IO) {
		nvmeib_qp_stats_on_offth_iter(q->qp_stats);
		(void)nvmeibs_process_cq(q);
		if (q->used_ids > 0) {
			i = find_first_bit(q->id_bitmap, q->total_ids);
			while (i < q->total_ids && n_aborts < q->dev->max_abort) {
				if (q->ids[i].callback != NULL) {
					if (time_after(current_time,
								q->ids[i].issue_time + nvmeibs_timeout)) {
						_NE(error_nvme_thread_process_q, "timeout ABORT!!! q=@ID_INT id=@IDX", q->id, i);
						_NE(error_1_nvme_thread_process_q, "current=@CURRENT_LONG issue=@ISSUE", current_time, q->ids[i].issue_time);
						if (abort_list != NULL && !q->ids[i].aborted) {
							++n_aborts;
							a = kmalloc(sizeof *a, GFP_ATOMIC);
							if (a != NULL) {
								a->d = q->dev;
								a->qid = q->id;
								a->cmdid = i;
								list_add_tail(&a->link, abort_list);
								q->ids[i].issue_time = current_time;
								q->ids[i].aborted = true;
							}
						}
						else if(!q->dev->need_reset) {
							_NW(warn_nvme_thread_process_q, "Resetting nvme@SEQ due to double timeout", q->dev->seq);
							initaite_dev_rst(trace_dev_rst_nvme_thread_process_q, q->dev);
						}
					}
				}
				i = find_next_bit(q->id_bitmap, q->total_ids, i+1);
			}
		}
	}
	spin_unlock_irqrestore(&q->q_lock, flags);
}

// Status of 8 is: Command Aborted due to SQ Deletion.
static void abort_q_cmds(struct nvme_qp *q)
{
	unsigned long flags;
	int i;

	/* Block comp-intr while aborting */
	spin_lock_irqsave(&q->q_lock, flags);
	/* Allow recursive locking the q as rqp->callback may try to submit new cmd;
	   The new cmd wont be queued as device's reset or remove flags are set */
	q->locking_cpu = smp_processor_id();
	i = find_first_bit(q->id_bitmap, q->total_ids);
	while (i < q->total_ids) {
		if(test_and_clear_bit(i, q->id_bitmap)) {
			struct req_id *rqp = &q->ids[i];
			nvme_callback_t *cb = rqp->callback;
			void *arg = rqp->arg;
			--q->used_ids;
			rqp->callback = NULL;
			if (cb != NULL) {
				q->locking_cpu = -1;
				spin_unlock_irqrestore(&q->q_lock, flags);
				(*cb)(arg, 0x8, 0);
				spin_lock_irqsave(&q->q_lock, flags);
				q->locking_cpu = smp_processor_id();
			}
			else
				_NE(error_1_abort_q_cmds, "aborting command without callback");
		}
		i = find_next_bit(q->id_bitmap, q->total_ids, i+1);
	}
	q->locking_cpu = -1;
	spin_unlock_irqrestore(&q->q_lock, flags);
}

static void abandon_outstanding(struct device_data *d)
{
	struct drive_params *drv;
	struct nvmeibs_nvme_req *req;
	struct bio *bio;
	ulong flags = 0;
	struct nvme_qp *q;
	int qid;

	_NT(trace_0_abandon_outstanding, "dev @SERIAL, abandon outstanding cmds",
		d->serial);

	if (!(d->need_reset || d->reset_pending || d->removed)) {
		_NE(error_nvme_abandon_outstanding, "abandon called but dev @SERIAL rst/rm flags are OFF", d->serial);
		WARN_ON(1);
	}

	/* Before abandoning cmds and maybe later on freeing q, drv or dev:
	 *  1) reject new ioqm alloc/free works for dev, when scheduled won't run.
	 *  2) wait for current work to finish.
	 *
	 * Note:
	 * nvme-remove-drv invalidates QUEUED ioqm requests that will
	 * be scheduled AFTER the dev is back from reset/removed.
	 */
	ioqm_release(d);

	for (qid = 0; qid < d->max_ioqs; ++qid) {
		if ((q = d->local_ioq[qid])) {
			abort_q_cmds(q);
			spin_lock_irqsave(&q->q_lock, flags);
			q->dying = true;
			while ((req = list_first_entry_or_null(&d->remote_iops_q[qid],
					struct nvmeibs_nvme_req, link)) != NULL) {
				list_del_init(&req->link);
				if (req->cb != NULL) {
					spin_unlock_irqrestore(&q->q_lock, flags);
					(*req->cb)(req->arg, 0x8, 0);
					spin_lock_irqsave(&q->q_lock, flags);
				}
			}
		}
		// At this point, we have all the queues locked!
		for (drv = d->drives; drv != NULL; drv = drv->next)
			while ((bio = bio_list_pop(&drv->bio_list[qid])) != NULL)
				bio_endio(bio, -ENXIO);

		if ((q = d->local_ioq[qid])) {
			spin_unlock_irqrestore(&q->q_lock, flags);
			if (!nvmeibs_use_nvme_kwq) {
				local_q_kthread_stop(q);
			}
		}
	}
	if (d->adminq)
		abort_q_cmds(d->adminq);

	for (drv = d->drives; drv != NULL; drv = drv->next)
		nvme_remove_disk(drv);
}

static void free_dsm_freelist(void)
{
	struct nvmeibs_dsm_range *dsm_ptr;
//	struct prpl_list *prpl;

	dsm_ptr = (struct nvmeibs_dsm_range *)xchg(&dsm_freelist, NULL);
	while (dsm_ptr != NULL) {
		struct nvmeibs_dsm_range *next = dsm_ptr->next;
		dma_free_coherent(dsm_ptr->dev, sizeof *dsm_ptr, dsm_ptr, dsm_ptr->dma);
		dsm_ptr = next;
	}

#if 0
	prpl = xchg(&prpl_freelist, NULL);
	while (prpl != NULL) {
		struct prpl_list *next = prpl->next;
		dma_free_coherent(prpl->dev, PAGE_SIZE, prpl, prpl->dma);
		prpl = next;
	}
#endif
}

static int nvmeibs_nvme_thread_func(void *arg)
{
	struct device_data *d;
	u16 sts;
	u32 csts;
	LIST_HEAD(abort_list);
	struct abort_request *a, *n;
	struct drive_params *drv;
	int qid;
	struct nvme_qp *q;

	_NT(trace_nvme_nvmeibs_nvme_thread_func, "nvmeibs: thread started");
	while (!kthread_should_stop()) {
		down_read(&global_lock);
		for (d = device_list; d != NULL; d = d->next) {
			if (!mutex_trylock(&d->dev_lock))
				continue;	/* Will do it next time */
			if (d->removed || (drv = d->drives) == NULL) {
				goto unlock_dev;
			}
			csts = readl(&d->mmio->csts);
			if (csts & NVME_CSTS_CFS) {
				_NE(t0m_nsnvme, "nvmeibs@INT: critical error on @STR csts=@X pci.sts=@X", d->seq, d->serial, csts, sts);
				pci_read_config_word(d->pci_dev, PCI_CONFIG_STS, &sts);
				if (csts == 0xffffffff && sts == 0xffff) {
					_NE(t0l_nsnvme, "nvmeibs@INT: drive @STR is dead, abort ongiong cmds and continue removal on pcie-remove", d->seq, d->serial);
					d->removed = true;
					abandon_outstanding(d);
					goto unlock_dev;
				}
				/* driver still responsive, reset-able */
				initaite_dev_rst(trace_dev_rst_0_nvme_nvmeibs_nvme_thread_func, d);
			}
			/* JH: Testing fix for EXC-789 */
			if (d->need_reset && d->reset_pending) {
				_NW(warn_nvme_nvmeibs_nvme_thread_func, "Conditions for EXC-789 Occurred! Clearing need_reset");
				d->need_reset = false;
			}
			if (d->need_reset && !d->reset_pending) {
				dump_dev_state(trace_1_nvme_nvmeibs_nvme_thread_func, d);
//				dump_dev_state(d);
				if (d->cc & NVME_CC_ENABLE) {
					_NT(trace_2_nvme_nvmeibs_nvme_thread_func, "CC.EN --> 0");
					d->cc &= ~NVME_CC_ENABLE;
					writel(d->cc, &d->mmio->cc);
					abandon_outstanding(d);
				}
				else if (!(csts & NVME_CSTS_RDY)) {
					_NT(trace_3_nvme_nvmeibs_nvme_thread_func, "CSTS.RDY=0, CC EN --> 1");
					d->cc |= NVME_CC_ENABLE;
					writel(d->cc, &d->mmio->cc);
					d->reset_pending = true;
					d->need_reset = false;
				}
			}
			else if(d->reset_pending) {
				_NT(trace_4_nvme_nvmeibs_nvme_thread_func, "Reset pending csts=@CSTS", csts);
				if (csts & NVME_CSTS_RDY) {
					_NT(trace_5_nvme_nvmeibs_nvme_thread_func, "CSTS.RDY=1, reinit qs");
					d->reset_pending = false;
					if (reinit_qs(d) == 0) {
						for (drv = d->drives; drv != NULL; drv = drv->next) {
							if (!drv->frozen)
								nvme_add_disk(drv);
							else
								_NT(trace_6_nvme_nvmeibs_nvme_thread_func, "didn't add disk because it is frozen");
						}
						complete_all(&d->reset_done);
					} else {
						_NT(trace_7_nvme_nvmeibs_nvme_thread_func, "initiate dev @SERIAL (@DEVICE_PTR) rst (reinit-qs)",
							d->serial, d);
						initaite_dev_rst(trace_dev_rst_1_nvme_nvmeibs_nvme_thread_func, d);
					}
					dump_dev_state(trace_8_nvme_nvmeibs_nvme_thread_func, d);
//					dump_dev_state(d);
				}
			}
			else {
				//OM: why do we need this?
				//we have intr-handler and cq-kthread. we only serve Qs that are
				//owned by local and currently we know this only after locking
				//the q and checking the owner bit (and not by the indirect
				//virt-ioqs table). This means that we might be looping
				//d->max_local_ioqs time and serving only 1 q.
				for (qid = 0; qid < d->max_ioqs; ++qid) {
					// YR: TODO: do we need to lock abort_list now?
					if ((q = d->local_ioq[qid]))
						thread_process_q(q, &abort_list);
				}
			}
#if 0 /* DEBUG ONLY */
			dump_internals(d);
#endif
unlock_dev:
			mutex_unlock(&d->dev_lock);
		}
		up_read(&global_lock);
		list_for_each_entry_safe(a, n, &abort_list, link) {
			list_del(&a->link);
			_NE(error_nvme_nvmeibs_nvme_thread_func, "Abort nvme@SEQ q=@QID cmdid=@CMDID", a->d->seq, a->qid, a->cmdid);
			if (!a->d->need_reset && abort_cmd(a->d, a->qid, a->cmdid) != 0) {
				_NW(warn_1_nvme_nvmeibs_nvme_thread_func, "Abort Failed on nvme@SEQ Resetting", a->d->seq);
				initaite_dev_rst(trace_dev_rst_2_nvme_nvmeibs_nvme_thread_func, a->d);
			}
			kfree(a);
		}
		free_dsm_freelist();
		nvmeibs_keep_alive();
		set_current_state(TASK_INTERRUPTIBLE);
		schedule_timeout(HZ - (jiffies + 3 * raw_smp_processor_id()) % HZ);
	}
	_NT(trace_9_nvme_nvmeibs_nvme_thread_func, "nvmeibs: thread done");
	return 0;
}

static int kthread_process_drive_cq(void *arg)
{
	struct nvme_qp *q = arg;
	struct device_data *d = q->dev;
	int qid = q->id - 1;
	bool cont = false;
	int total = 0, i;
	bool enb_irq = false;
	u64 start_ns, busy_ns;
	unsigned long flags;

	set_current_state(TASK_INTERRUPTIBLE);
	complete(&q->th_ready);
	schedule();
	_NT(trace_nvme_kthread_process_drive_cq, "Thread started for drive @SERIAL qid=@QID", d->serial, qid);
	while (!kthread_should_stop()) {
		q = d->local_ioq[qid];
		if (q && READ_ONCE(q->polling)) {

			do {
				cont = false;
				enb_irq = false;
				q = d->local_ioq[qid];
				if (!q || !READ_ONCE(q->polling) || d->removed || d->reset_pending
					|| d->need_reset)
					break;
				spin_lock_irqsave(&q->q_lock, flags);
				nvmeib_qp_stats_on_offth_iter(q->qp_stats);
				start_ns = nvmeib_public_local_clock();
				i = nvmeibs_process_cq(q);
				busy_ns = nvmeib_public_local_clock() - start_ns;
				enb_irq = (q->state == LOCAL_Q_ON ||
						   q->state == LOCAL_Q_STOP_NEW_IO);
				spin_unlock_irqrestore(&q->q_lock, flags);
				cont = i > 0;
				total += i;
				cond_resched(); /* let other threads run */
			} while (cont);

			if (!cont && !d->removed && READ_ONCE(q->polling)) {
				/* The correct sequence to prevent lost-wakeups is: 
				* 1. Set state for interrupt to see
				* 2. Set_current_state(TASK_INTERRUPTIBLE);
				* 3. Write Barrier
				* 4. Enable interrupts
				* 5. Read Barrier
				* 6. Check state if interrupt has changed it to polling mode
				* 7. Schedule if state has not changed, otherwise continue polling
				*/

				/* Step 1: update poll-mode */
				WRITE_ONCE(q->polling, false);

				/* Step 2: set task state to INTERRUPTIBLE */
				set_current_state(TASK_INTERRUPTIBLE);

				/* Step 3: write barrier */
				smp_mb();
				
				/* Step 4: enable interrupts */
				if (enb_irq) {
					spin_lock_irqsave(&q->q_lock, flags);
					local_q_modify_irq(q, LOCAL_Q_IRQ_ENABLE);
					spin_unlock_irqrestore(&q->q_lock, flags);
					_ND(trace_1_nvme_kthread_process_drive_cq, "Switch local IRQ back after @TOTAL completions", total);
					q->irq_debug = 2;
					total = 0;
				}

				/* Step 5: read barrier */
				smp_mb();

				/* we may miss an interrupt in the time until we rearm the interrupts, so we need to check if the CQ is not empty */

				/* [NVMESH-7791]: q_lock must be taken before checking is_cq_empty() */
				spin_lock_irqsave(&q->q_lock, flags);
				if (!is_cq_empty(q)) {
					//_ND(cq_not_empty_nvme_kthread_process_drive_cq, "CQ is not empty, disable interrupts and mark as polling again serial=@SERIAL qid=@QID", d->serial, qid);
					set_current_state(TASK_RUNNING);
					/* disable interrupts */
					local_q_modify_irq(q, LOCAL_Q_IRQ_DISABLE_NOSYNC);
					spin_unlock_irqrestore(&q->q_lock, flags);
					/* mark as polling again */
					WRITE_ONCE(q->polling, true);
					smp_mb();
					/* sleep if needed */
					cond_resched();
					continue;
				}
				spin_unlock_irqrestore(&q->q_lock, flags);
				smp_mb();

				/* Step 6: Check polling again in case interrupt set it to true. This is not a must as
				the interrupt would have changed the state to READY so the schedule() will not really sleep,
				but it's a good practice to check again.
				* Also check kthread_should_stop() to prevent lost wakeup from kthread-stop */
				if (!READ_ONCE(q->polling) && !kthread_should_stop()) {
					/* Step 7: call schedule(), main kthread handles lost interrupts
						(changed from schedule_timeout()) */
					schedule();
				}
				/* This is redundant as the task state is already set to RUNNING after schedule() */
				__set_current_state(TASK_RUNNING);
			}
		}
		cond_resched();
	}
	_NT(trace_2_nvme_kthread_process_drive_cq, "Thread stopped for drive @SERIAL qid=@QID", d->serial, qid);
	return 0;
}

//OL: have separate function for initial distribution of IO queues
static int setup_ioqs(struct device_data *d)
{
	int numqs;
	struct nvme_qp *q;
	int ii, qid;
	int err;
	int qmax = NVME_CAP_MQES(d->cap) + 1;

	/* ---------------------------------------------------------------------- */
	/* set d->max_ioqs                                                        */
	/* ---------------------------------------------------------------------- */
	/* Request N iocqs and iosqs.
	   N does not include acq/asq and is a 0 based value, hence -2 */
	numqs = ((d->max_msix-2) << 16) | (d->max_msix-2);
	numqs = set_feature(d, 0, NVME_FEAT_NUM_QUEUES, numqs);
	/* Min of allocated iocq vs iosq (1's based) and min w/ num-msix for ioqs */
	d->max_ioqs = min(min(numqs & 0xffff, numqs >> 16)+1, d->max_msix-1);
	/* Min w/ num available doorbells for ioqs (-1 for adminq) */
	d->max_ioqs = min(d->max_ioqs, 2048/d->doorbell_stride -1);
	_NT(t0_nvme_setup_ioqs, "d->max_ioqs=@MAX_IOQS", d->max_ioqs);

	if (d->max_local_ioqs > d->max_ioqs)
		_NT(t4_nvme_setup_ioqs, "We've used too many msix, @INT but using @INT",
			d->max_local_ioqs, d->max_ioqs);

	/* ---------------------------------------------------------------------- */
	/* set d->max_local_ioqs                                                  */
	/* ---------------------------------------------------------------------- */
	/* The maximum number of ioqs that local drv can use concurrently is limited
	   to (a) max-ioqs, which already considers available MSI-X entries other than
	   the adminq's, and to (b) the num-possible-cpus.
	   For example: with 32 MSI-X entries and 12 possible-cpus,
	   max-ioqs will be 12 and ioqs will use MSI-X entries 1 to 13 */
	d->max_local_ioqs = min(d->max_ioqs, d->max_local_ioqs);
	_NT(t1_nvme_setup_ioqs, "d->max_local_ioqs=@INT", d->max_local_ioqs);

	/* ---------------------------------------------------------------------- */
	/* set d->min_local_ioqs                                                  */
	/* ---------------------------------------------------------------------- */
	/* set initial value and then try to maximize it as this value determines
	   the num of local-qs we start with */
	_NT(t2_nvme_setup_ioqs,
		"nvmeibs-min_local_nvmeqs=@INT, max_client_rsrc=@INT, nvmeibs-max_local_nvmeqs=@INT",
		nvmeibs_min_local_nvmeqs, max_client_rsrc, nvmeibs_max_local_nvmeqs);
	if (max_client_rsrc){
		d->min_local_ioqs = min(nvmeibs_min_local_nvmeqs, d->max_ioqs);
		if (d->max_ioqs >= 30) {
			d->min_local_ioqs = max(2, d->min_local_ioqs);
			if (d->max_ioqs >= 62)
				d->min_local_ioqs = max(4, d->min_local_ioqs);
		}
		d->max_client_qs = d->max_ioqs - d->min_local_ioqs;
	}
	else {
		/* No qs were requested for RDDA, use all qs for local (& nordda) */
		d->min_local_ioqs = d->max_ioqs;
		d->max_client_qs = 0;
	}
	/* finally, limit to max msix allocated (see setup_adminq()) */
	d->min_local_ioqs = min(d->min_local_ioqs, d->max_local_ioqs);
	_NT(t3_nvme_setup_ioqs, "d->min_local_ioqs=@INT, d->max_client_qs=@INT",
		d->min_local_ioqs, d->max_client_qs);

	/* ---------------------------------------------------------------------- */
	/* alloc qs                                                               */
	/* ---------------------------------------------------------------------- */

	memset(d->local_ioq, 0, sizeof (*d->local_ioq) * d->max_ioqs);

	for (ii = 0; ii < d->max_ioqs; ++ii) {
		qid = 1 + ii;
		if ((q = alloc_qp(d, min(LOCAL_IOQ_DEPTH, qmax))) == NULL) {
			err = -ENOMEM;
			goto out;
		}

		q->id = qid;
		q->sq_doorbell = (void *)d->mmio + 4096 + 2*qid*d->doorbell_stride;
		q->cq_doorbell = (void *)d->mmio + 4096 + (2*qid+1)*d->doorbell_stride;
		q->irq_state = LOCAL_Q_IRQ_NONE;
		q->complete_fn = local_complete_fn;
		q->polling = false;

		if (ii < d->min_local_ioqs) {
			if ((err = local_q_request_irq(q)) < 0)
				goto out;
			if ((err = create_cq(d, qid, q->cq_phys, q->cq_len, qid)) < 0)
				goto out;
			if ((err = create_sq(d, qid, q->sq_phys, q->sq_len, qid)) < 0)
				goto out;
			if (!nvmeibs_use_nvme_kwq) {
				if ((err = local_q_kthread_start_(q)) < 0)
					goto out;
			}
			INIT_WORK(&q->process_cq_work, kthread_process_drive_cq_work_func);
			q->state = LOCAL_Q_ON;
		}
		d->local_ioq[qid - 1] = q;
	}
	ioqm_init(d);
	return 0;
out:
	_NW(warn_nvme_setup_ioqs, "setup_ioqs failed err=@ERR", err);
	if (q != NULL) {
		if (!nvmeibs_use_nvme_kwq) {
			local_q_kthread_stop(q);
		}
		local_q_free_irq(q);
		free_qp(q);
	}
	while (--ii >= 0) {
		if ((q = d->local_ioq[ii])) {
			if (!nvmeibs_use_nvme_kwq) {
				local_q_kthread_stop(q);
			}
			local_q_free_irq(q);
			free_qp(q);
		}
		d->local_ioq[ii] = 0;
	}

	return err;
}

static int create_local_bdev(struct drive_params *drv)
{
	struct device_data *d = drv->dev;
	struct request_queue *rq;
	struct gendisk *hd;

#if KS_HAS_BLK_ALLOC_DISK
#if KS_BLK_ALLOC_DISK_2PARAMS
	if ((hd = blk_alloc_disk(NULL, NUMA_NO_NODE)) == NULL) {
		return -ENOMEM;
	}
#else
	if ((hd = blk_alloc_disk(NUMA_NO_NODE)) == NULL) {
		return -ENOMEM;
	}
#endif
	rq = hd->queue;
#else // KS_HAS_BLK_ALLOC_DISK

#if KS_HAS_NEW_BLK_ALLOC_QUEUE
	if((rq = blk_alloc_queue(nvmeibs_make_request, NUMA_NO_NODE)) == NULL)
		return -ENOMEM;
#else
#	if KS_REQUEST_QUEUE_HAS_REQUEST_FN
	if((rq = blk_alloc_queue(GFP_KERNEL)) == NULL)
		return -ENOMEM;
	blk_queue_make_request(rq, nvmeibs_make_request);
#	else
	if((rq = blk_alloc_queue(NUMA_NO_NODE)) == NULL)
		return -ENOMEM;
#	endif
#endif

#endif // KS_HAS_BLK_ALLOC_DISK

	rq->queue_flags = (NVMESH_QUEUE_FLAG_DEFAULT
#ifdef QUEUE_FLAG_NOMERGES
					| (1 << QUEUE_FLAG_NOMERGES)
#endif
#ifdef QUEUE_FLAG_NONROT
					| (1 << QUEUE_FLAG_NONROT)
#endif
					)
#ifdef QUEUE_FLAG_ADD_RANDOM
					&~(1 << QUEUE_FLAG_ADD_RANDOM)
#endif
					;

/* set only QUEUE_FLAG_DISCARD defined otherwise blk_queue_max_discard_sectors is enough */
#ifdef QUEUE_FLAG_DISCARD
	if (d->trim_supported)
		rq->queue_flags |= (1 << QUEUE_FLAG_DISCARD);
#endif

	_NT(trace_nvme_create_local_bdev, "set block_len=@BLOCK_LEN", drv->block_len);
#if KS_HAS_QUEUE_LIMITS_START_UPDATE
	{
		struct queue_limits lim __attribute__((unused)) = queue_limits_start_update(rq);
		rq->limits.logical_block_size = drv->block_len;
		rq->limits.physical_block_size = drv->block_len;
		rq->limits.max_hw_discard_sectors = UINT_MAX;
		if (drv->metadata)
			rq->limits.max_hw_sectors = PAGE_SIZE >> 9;
		else if (d->max_transfer != 0)
			rq->limits.max_hw_sectors = d->max_transfer >> 9;
		queue_limits_cancel_update(rq);
	}
#else
	blk_queue_logical_block_size(rq, drv->block_len);
	blk_queue_physical_block_size(rq, drv->block_len);
	blk_queue_max_discard_sectors(rq, UINT_MAX);
#if KS_BI_REMAINING
	if (drv->metadata)
		blk_queue_max_hw_sectors(rq, PAGE_SIZE >> 9);
	else if (d->max_transfer != 0)
		blk_queue_max_hw_sectors(rq, d->max_transfer >> 9);
#else
	blk_queue_max_hw_sectors(rq, PAGE_SIZE >> 9);
#endif
#endif
	rq->queuedata = drv;

#if !KS_HAS_BLK_ALLOC_DISK
	if((hd = alloc_disk(0)) == NULL) {
		blk_cleanup_queue(rq);
		return -ENOMEM;
	}
#endif

#if !KS_HAS_BLK_ALLOC_DISK
	// YR: with the new mechanism for allocating block device numbers,
	// better to just leave them both empty for now.
	// If we really insist, we need to start managing the minors with an IDR.
	// YC: In addition in older kernels doesn't affect too much as we setting
	// GENHD_FL_EXT_DEVT laster is register the device under external devt anyway.
	// Also there is a limit on number of minors under 1 major so we should just avoid it if possible.
	hd->major = local_major;
#endif
	hd->first_minor = 0;
	hd->fops = &nvmeibs_fops;
	hd->private_data = drv;
	hd->queue = rq;
#if KS_DRIVERFS_DEV
	hd->driverfs_dev = &d->pci_dev->dev;
#endif
	hd->flags = GENHD_FL_EXT_DEVT; /* Jared: Removed to allow kernel to read GPT for us | GENHD_FL_NO_PART; */
	snprintf(hd->disk_name, DISK_NAME_LEN, "nvme%dn%d", nvme_number_offset+d->seq, drv->nsid);
	set_capacity(hd, drv->size * (drv->block_len/512));
	drv->gendisk = hd;
	drv->info.gendisk = hd;

#if KS_ADD_DISK_INT_RV
       return add_disk(hd);
#else
        add_disk(hd);
	return 0;
#endif
}

/* YR: The following functions have been copied from nvme-core.c:
 * ** nvme_setup_prp_pools
 * ** nvme_release_prp_pools
 * ** iod_list
 * ** nvme_npages
 * ** nvme_alloc_iod
 * ** nvme_map_user_pages
 * ** nvme_unmap_user_pages
 * ** nvme_setup_prps
 * ** nvme_free_iod
 */

/*
 * The nvme_iod describes the data in an I/O, including the list of PRP
 * entries.  You can't see it in this data structure because C doesn't let
 * me express that.  Use nvme_alloc_iod to ensure there's enough space
 * allocated to store the PRP list.
 */
struct nvme_iod {
	unsigned long private;  /* For the use of the submitter of the I/O */
	int npages;     /* In the PRP list. 0 means small pool in use */
	int offset;     /* Of PRP list */
	int nents;      /* Used in scatterlist */
	int length;     /* Of data, in bytes */
	dma_addr_t first_dma;
	struct scatterlist sg[];
};

static int nvmeibs_setup_prp_pools(struct device_data *dev)
{
	struct device *dmadev = &dev->pci_dev->dev;
	dev->prp_page_pool = dma_pool_create("prp list page", dmadev,
			PAGE_SIZE, PAGE_SIZE, 0);
	if (!dev->prp_page_pool)
		return -ENOMEM;

	/* Optimisation for I/Os between 4k and 128k */
	dev->prp_small_pool = dma_pool_create("prp list 256", dmadev,
			256, 256, 0);
	if (!dev->prp_small_pool) {
		dma_pool_destroy(dev->prp_page_pool);
		dev->prp_page_pool = NULL;
		return -ENOMEM;
	}
	return 0;
}

static void nvmeibs_release_prp_pools(struct device_data *dev)
{
	if (dev->prp_page_pool) {
		dma_pool_destroy(dev->prp_page_pool);
		dev->prp_page_pool = NULL;
	}
	if (dev->prp_small_pool) {
		dma_pool_destroy(dev->prp_small_pool);
		dev->prp_small_pool = NULL;
	}
}

static __le64 **iod_list(struct nvme_iod *iod)
{
	return ((void *)iod) + iod->offset;
}

static int nvme_npages(unsigned size)
{
	unsigned nprps = DIV_ROUND_UP(size + PAGE_SIZE, PAGE_SIZE);
	return DIV_ROUND_UP(8 * nprps, PAGE_SIZE - 8);
}

static struct nvme_iod *nvme_alloc_iod(unsigned nseg, unsigned nbytes,
	gfp_t gfp)
{
	struct nvme_iod *iod = kmalloc(sizeof(struct nvme_iod) +
			sizeof(__le64 *) * nvme_npages(nbytes) +
			sizeof(struct scatterlist) * nseg, gfp);

	if (iod) {
		iod->offset = offsetof(struct nvme_iod, sg[nseg]);
		iod->npages = -1;
		iod->length = nbytes;
		iod->nents = 0;
	}

	return iod;
}

static struct nvme_iod *nvmeibs_map_user_pages(struct device_data *dev, int write,
		unsigned long addr, unsigned length)
{
	int i, err, count, nents, offset;
	struct scatterlist *sg;
	struct page **pages;
	struct nvme_iod *iod;

	if (addr & 3)
		return ERR_PTR(-EINVAL);
	if (!length || length > INT_MAX - PAGE_SIZE)
		return ERR_PTR(-EINVAL);

	offset = offset_in_page(addr);
	count = DIV_ROUND_UP(offset + length, PAGE_SIZE);
	pages = kcalloc(count, sizeof(*pages), GFP_KERNEL);
	if (!pages)
		return ERR_PTR(-ENOMEM);

	err = nvmeib_get_user_pages_fast(addr, count, 1, pages);
	if (err < count) {
		count = err;
		err = -EFAULT;
		goto put_pages;
	}

	iod = nvme_alloc_iod(count, length, GFP_KERNEL);
	sg = iod->sg;
	sg_init_table(sg, count);
	for (i = 0; i < count; i++) {
		sg_set_page(&sg[i], pages[i],
				min_t(unsigned, length, PAGE_SIZE - offset),
				offset);
		length -= (PAGE_SIZE - offset);
		offset = 0;
	}
	sg_mark_end(&sg[i - 1]);
	iod->nents = count;

	err = -ENOMEM;
	nents = dma_map_sg(&dev->pci_dev->dev, sg, count,
			write ? DMA_TO_DEVICE : DMA_FROM_DEVICE);
	if (!nents)
		goto free_iod;

	kfree(pages);
	return iod;

free_iod:
	kfree(iod);
put_pages:
	for (i = 0; i < count; i++)
		put_page(pages[i]);
	kfree(pages);
	return ERR_PTR(err);
}

static void nvmeibs_unmap_user_pages(struct device_data *dev, int write,
		struct nvme_iod *iod)
{
	int i;

	dma_unmap_sg(&dev->pci_dev->dev, iod->sg, iod->nents,
			write ? DMA_TO_DEVICE : DMA_FROM_DEVICE);

	for (i = 0; i < iod->nents; i++)
		put_page(sg_page(&iod->sg[i]));
}

static int nvmeibs_setup_prps(struct device_data *dev, __le64 *prp1, __le64 *prp2,
		struct nvme_iod *iod, int total_len, gfp_t gfp)
{
	struct dma_pool *pool;
	int length = total_len;
	struct scatterlist *sg = iod->sg;
	int dma_len = sg_dma_len(sg);
	u64 dma_addr = sg_dma_address(sg);
	int offset = offset_in_page(dma_addr);
	__le64 *prp_list;
	__le64 **list = iod_list(iod);
	dma_addr_t prp_dma;
	int nprps, i;

	*prp1 = cpu_to_le64(dma_addr);
	length -= (PAGE_SIZE - offset);
	if (length <= 0)
		return total_len;

	dma_len -= (PAGE_SIZE - offset);
	if (dma_len) {
		dma_addr += (PAGE_SIZE - offset);
	} else {
		sg = sg_next(sg);
		dma_addr = sg_dma_address(sg);
		dma_len = sg_dma_len(sg);
	}

	if (length <= PAGE_SIZE) {
		*prp2 = cpu_to_le64(dma_addr);
		return total_len;
	}

	nprps = DIV_ROUND_UP(length, PAGE_SIZE);
	if (nprps <= (256 / 8)) {
		pool = dev->prp_small_pool;
		iod->npages = 0;
	} else {
		pool = dev->prp_page_pool;
		iod->npages = 1;
	}

	prp_list = dma_pool_alloc(pool, gfp, &prp_dma);
	if (!prp_list) {
		*prp2 = cpu_to_le64(dma_addr);
		iod->npages = -1;
		return (total_len - length) + PAGE_SIZE;
	}
	list[0] = prp_list;
	iod->first_dma = prp_dma;
	*prp2 = cpu_to_le64(prp_dma);
	i = 0;
	for (;;) {
		if (i == PAGE_SIZE / 8) {
			__le64 *old_prp_list = prp_list;
			prp_list = dma_pool_alloc(pool, gfp, &prp_dma);
			if (!prp_list)
				return total_len - length;
			list[iod->npages++] = prp_list;
			prp_list[0] = old_prp_list[i - 1];
			old_prp_list[i - 1] = cpu_to_le64(prp_dma);
			i = 1;
		}
		prp_list[i++] = cpu_to_le64(dma_addr);
		dma_len -= PAGE_SIZE;
		dma_addr += PAGE_SIZE;
		length -= PAGE_SIZE;
		if (length <= 0)
			break;
		if (dma_len > 0)
			continue;
		BUG_ON(dma_len < 0);
		sg = sg_next(sg);
		dma_addr = sg_dma_address(sg);
		dma_len = sg_dma_len(sg);
	}

	return total_len;
}

static void nvmeibs_free_iod(struct device_data *dev, struct nvme_iod *iod)
{
	const int last_prp = PAGE_SIZE / 8 - 1;
	int i;
	__le64 **list = iod_list(iod);
	dma_addr_t prp_dma = iod->first_dma;

	if (iod->npages == 0)
		dma_pool_free(dev->prp_small_pool, list[0], prp_dma);
	for (i = 0; i < iod->npages; i++) {
		__le64 *prp_list = list[i];
		dma_addr_t next_prp_dma = le64_to_cpu(prp_list[last_prp]);
		dma_pool_free(dev->prp_page_pool, prp_list, prp_dma);
		prp_dma = next_prp_dma;
	}
	kfree(iod);
}

static int submit_user_cmd(struct device_data *d, struct nvme_qp *q,
	struct nvme_admin_cmd __user *ucmd)
{
	struct cmd_result result;
	struct nvme_command *cmd;
	struct nvme_admin_cmd acmd;
	unsigned long flags;
	int len, rv;
	struct nvme_iod *iod = NULL;  /* GCC */
	struct drive_params *drv = NULL;
	__le64 prp1 = 0, prp2 = 0;

	NFIN;

	if (!capable(CAP_SYS_ADMIN)) {
		rv = -EACCES;
		goto out;
	}
	if (copy_from_user(&acmd, ucmd, sizeof(acmd))) {
		rv = -EFAULT;
		goto out;
	}

	if (acmd.opcode == nvme_admin_format_nvm) {
		for (drv = d->drives; drv != NULL && drv->nsid != acmd.nsid; drv = drv->next)
			;
		if (drv == NULL) {
			rv = -ENXIO;
			goto out;
		}
		if (!drv->frozen) {
			pr_warn("nvme%dn%d: Attempt format while not frozen\n", d->seq+1000,
					cpu_to_le32(acmd.nsid));
			rv = -EBUSY;
			goto out;
		}
		d->formatting = true;
	}

	len = acmd.data_len;
	if (len != 0) {
		iod = nvmeibs_map_user_pages(d, acmd.opcode & 1, acmd.addr, len);
		if (IS_ERR(iod)) {
			rv = PTR_ERR(iod);
			goto out;
		}
		len = nvmeibs_setup_prps(d, &prp1, &prp2, iod, len, GFP_KERNEL);
	}
	if (len != acmd.data_len) {
		rv = -ENOMEM;
		goto out;
	}

	init_completion(&result.completion);
	cmd = get_cmd(q, admin_cmd_done, &result, &flags);
	if (cmd == NULL) {
		rv = -ENOMEM;
		goto out;
	}

	cmd->common.opcode = acmd.opcode;
	cmd->common.flags = acmd.flags;
	cmd->common.nsid = cpu_to_le32(acmd.nsid);
	cmd->common.cdw2[0] = cpu_to_le32(acmd.cdw2);
	cmd->common.cdw2[1] = cpu_to_le32(acmd.cdw3);
	cmd->common.dptr_prp1 = prp1;
	cmd->common.dptr_prp2 = prp2;
	cmd->common.cdw10[0] = cpu_to_le32(acmd.cdw10);
	cmd->common.cdw10[1] = cpu_to_le32(acmd.cdw11);
	cmd->common.cdw10[2] = cpu_to_le32(acmd.cdw12);
	cmd->common.cdw10[3] = cpu_to_le32(acmd.cdw13);
	cmd->common.cdw10[4] = cpu_to_le32(acmd.cdw14);
	cmd->common.cdw10[5] = cpu_to_le32(acmd.cdw15);


	nvmeibs_submit_wait(q, cmd, &result, &flags);
	rv = result.status;
	acmd.result = result.result;

	if (acmd.data_len) {
		nvmeibs_unmap_user_pages(d, acmd.opcode & 1, iod);
		nvmeibs_free_iod(d, iod);
	}

	if ((rv >= 0) &&
			copy_to_user(&ucmd->result, &acmd.result, sizeof(acmd.result)))
		rv = -EFAULT;

	if (acmd.opcode == nvme_admin_format_nvm) {
		if (rv == 0)
			rv = re_ident_ns(drv);
		d->formatting = false;
	}
out:
	NFOUT;
	return rv;
}

static int submit_user_cmd_io(struct device_data *d, struct nvme_qp *q,
			   struct nvme_admin_cmd __user *ucmd)
{
	struct cmd_result result;
	struct nvme_command *cmd;
	struct nvme_admin_cmd acmd;
	unsigned long flags;
	int len, rv;
	struct nvme_iod *iod = NULL;  /* GCC */
	struct drive_params *drv = NULL;
	__le64 prp1 = 0, prp2 = 0;
	struct page *md_pg = NULL;
	dma_addr_t meta_dma_addr = 0;
	bool is_write;

	NFIN;
	if (!capable(CAP_SYS_ADMIN)) {
		rv = -EACCES;
		goto out;
	}
	if (copy_from_user(&acmd, ucmd, sizeof(acmd))) {
		rv = -EFAULT;
		goto out;
	}
	is_write = !!(acmd.opcode & 1);
	len = acmd.data_len;
	if (len != 0) {
		iod = nvmeibs_map_user_pages(d, (int)is_write, acmd.addr, len);
		if (IS_ERR(iod)) {
			rv = PTR_ERR(iod);
			goto out;
		}
		len = nvmeibs_setup_prps(d, &prp1, &prp2, iod, len, GFP_KERNEL);
	}
	if (len != acmd.data_len) {
		rv = -ENOMEM;
		goto out;
	}
	if (acmd.metadata_len) {
		for (drv = d->drives; drv != NULL; drv = drv->next) {
			 if (drv->nsid == acmd.nsid) {
				 break;
			 }
		}
		if (drv == NULL) {
			rv = -ENXIO;
			goto out;
		}
		if (!drv->metadata) {
			rv = -EINVAL;
			goto out;
		}
		rv = nvmeib_get_user_pages_fast(acmd.metadata, 1, 1, &md_pg);
		if (rv != 1) {
			if (rv >= 0)
				rv = -EFAULT;
			goto out;
		}
		meta_dma_addr = dma_map_page(&d->pci_dev->dev,
					     md_pg, offset_in_page(acmd.metadata),
					     acmd.metadata_len, is_write ? DMA_TO_DEVICE : DMA_FROM_DEVICE);
		if (dma_mapping_error(&d->pci_dev->dev, meta_dma_addr)) {
			meta_dma_addr = 0;
			rv = -ENOMEM;
			goto out;
		}
	}

	init_completion(&result.completion);
	cmd = get_cmd(q, admin_cmd_done, &result, &flags);
	if (cmd == NULL) {
		rv = -ENOMEM;
		goto out;
	}

	cmd->common.opcode = acmd.opcode;
	cmd->common.flags = acmd.flags;
	cmd->common.nsid = cpu_to_le32(acmd.nsid);
	cmd->common.cdw2[0] = cpu_to_le32(acmd.cdw2);
	cmd->common.cdw2[1] = cpu_to_le32(acmd.cdw3);
	cmd->common.dptr_prp1 = prp1;
	cmd->common.dptr_prp2 = prp2;
	cmd->common.cdw10[0] = cpu_to_le32(acmd.cdw10);
	cmd->common.cdw10[1] = cpu_to_le32(acmd.cdw11);
	cmd->common.cdw10[2] = cpu_to_le32(acmd.cdw12);
	cmd->common.cdw10[3] = cpu_to_le32(acmd.cdw13);
	cmd->common.cdw10[4] = cpu_to_le32(acmd.cdw14);
	cmd->common.cdw10[5] = cpu_to_le32(acmd.cdw15);
	cmd->common.metadata = cpu_to_le64(meta_dma_addr);

	nvmeibs_submit_wait(q, cmd, &result, &flags);
	rv = result.status;
	acmd.result = result.result;

	if (acmd.data_len) {
		nvmeibs_unmap_user_pages(d, (int)is_write, iod);
		nvmeibs_free_iod(d, iod);
	}

	if (meta_dma_addr) {
		dma_unmap_page(&d->pci_dev->dev, meta_dma_addr, acmd.metadata_len,
			       is_write ? DMA_TO_DEVICE : DMA_FROM_DEVICE);
		put_page(md_pg);
	}

	if ((rv >= 0) &&
		copy_to_user(&ucmd->result, &acmd.result, sizeof(acmd.result)))
		rv = -EFAULT;

out:
	NFOUT;
	return rv;
}

static int submit_user_io(struct drive_params *drv, struct nvme_user_io __user *uio)
{
	struct device_data *d = drv->dev;
	struct nvme_qp *q = *d->local_ioq;
	struct cmd_result result;
    struct nvme_command *cmd;
	struct nvme_user_io io;
	unsigned length, meta_len;
	unsigned prp_length;
	int rv = 0;
	struct nvme_iod *iod;
	dma_addr_t meta_dma_addr = 0;
	enum dma_data_direction dma_dir;
	__le64 prp1 = 0, prp2 = 0;
	struct page *page;
	unsigned long flags;

	if (copy_from_user(&io, uio, sizeof(io)))
		return -EFAULT;

	if (io.opcode != nvme_cmd_read && io.opcode != nvme_cmd_write &&
		io.opcode != nvme_cmd_compare)
		return -EINVAL;

	dma_dir = ((io.opcode & 1) ? DMA_TO_DEVICE : DMA_FROM_DEVICE);

	length = (io.nblocks + 1) * drv->block_len;
	meta_len = (io.nblocks + 1) * drv->metadata;
	if (drv->mtdt_extd) {
		length += meta_len;
		meta_len = 0;
	}

	if (meta_len > 0) {
		if ((io.metadata & 3) || !io.metadata)
			return -EINVAL;
		if ((io.metadata & PAGE_MASK) != ((io.metadata + meta_len - 1) & PAGE_MASK)) {
			_NT(trace_nvme_submit_user_io, "io.metadata=@METADATA_LLONG meta_len=@META_LEN", io.metadata, meta_len);
			return -EINVAL; /* Only support 1 page of metadata */
		}
		rv = nvmeib_get_user_pages_fast(io.metadata, 1, 1, &page);
		if (rv < 0)
			return rv;
		else if (rv != 1)
			return -EFAULT;
	}

	iod = nvmeibs_map_user_pages(d, io.opcode & 1, io.addr, length);
	if (IS_ERR(iod))
		return PTR_ERR(iod);

	if (meta_len > 0) {
		meta_dma_addr = dma_map_page(&d->pci_dev->dev,
							page, io.metadata & ~PAGE_MASK, meta_len, dma_dir);
		if (dma_mapping_error(&d->pci_dev->dev, meta_dma_addr)) {
			rv = -ENOMEM;
			goto out;
			meta_dma_addr = 0;
		}
	}

	prp_length = nvmeibs_setup_prps(d, &prp1, &prp2, iod, length, GFP_KERNEL);
	if (length != prp_length) {
		rv = -ENOMEM;
		goto out;
	}

    init_completion(&result.completion);
	cmd = get_cmd(q, admin_cmd_done, &result, &flags);
	if (cmd == NULL) {
		rv = -ENOMEM;
		goto out;
	}

	cmd->rw.opcode = io.opcode;
	cmd->rw.flags = io.flags;
	cmd->rw.nsid = cpu_to_le32(drv->nsid);
	cmd->rw.dptr_prp1 = prp1;
    cmd->rw.dptr_prp2 = prp2;
	cmd->rw.slba = cpu_to_le64(io.slba);
	cmd->rw.length = cpu_to_le16(io.nblocks);
	cmd->rw.control = cpu_to_le16(io.control);
	cmd->rw.dsmgmt = cpu_to_le32(io.dsmgmt);
	cmd->rw.reftag = cpu_to_le32(io.reftag);
	cmd->rw.apptag = cpu_to_le16(io.apptag);
	cmd->rw.appmask = cpu_to_le16(io.appmask);
	cmd->rw.metadata = cpu_to_le64(meta_dma_addr);

    nvmeibs_submit_wait(q, cmd, &result, &flags);
    rv = result.status;

out:
	nvmeibs_unmap_user_pages(d, io.opcode & 1, iod);
	nvmeibs_free_iod(d, iod);

	if (meta_dma_addr) {
		dma_unmap_page(&d->pci_dev->dev, meta_dma_addr, meta_len, dma_dir);
		put_page(page);
	}

	return rv;
}

static int nvmeibs_dev_open(struct inode *inode, struct file *f)
{
	struct device_data *d = container_of(f->private_data, struct device_data,
		cdev);
	f->private_data = d;
	return 0;
}

static int nvmeibs_ioctl(struct block_device *bdev, fmode_t mode,
	unsigned int cmd, unsigned long arg)
{
	long rv = -ENOTTY;
	struct drive_params *drv = bdev->bd_disk->private_data;
	struct device_data *d = drv->dev;

	NFIN;
	mutex_lock(&d->dev_lock);
	if (d->removed) {
		rv = -ENXIO;
		goto unlock;
	}
	switch (cmd) {
	case NVME_IOCTL_ID:
		rv = drv->nsid;
		break;
	case NVME_IOCTL_ADMIN_CMD:
		rv = submit_user_cmd(drv->dev, drv->dev->adminq, (void __user *)arg);
		break;
	case NVME_IOCTL_SUBMIT_IO:
		rv = submit_user_io(drv, (void __user *)arg);
		break;
#ifdef NVME_IOCTL_IO_CMD
	case NVME_IOCTL_IO_CMD:
		rv = submit_user_cmd_io(drv->dev, *drv->dev->local_ioq, (void __user *)arg);
		break;
#endif
	}
unlock:
	mutex_unlock(&d->dev_lock);
	NFOUT;
	return rv;
}

static long nvmeibs_cioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	long rv = -ENOTTY;
	struct device_data *d;

	NFIN;
	switch (cmd) {
	case NVME_IOCTL_ADMIN_CMD:
		d = f->private_data;
		rv = submit_user_cmd(d, d->adminq, (void __user *)arg);
		break;

#ifdef NVME_IOCTL_IO_CMD
	case NVME_IOCTL_IO_CMD:
		d = f->private_data;
		rv = submit_user_cmd_io(d, *d->local_ioq, (void __user *)arg);
		break;
#endif
	}

	NFOUT;
	return rv;
}

static const struct file_operations nvmeibs_cfops = {
	.owner = THIS_MODULE,
	.open = nvmeibs_dev_open,
	.unlocked_ioctl = nvmeibs_cioctl,
	.compat_ioctl = nvmeibs_cioctl,
};

static int create_local_cdev(struct device_data *d)
{
	int rv;
	NFIN;

	if ((rv = nvmeibs_setup_prp_pools(d))) {
		goto out;
	}

	scnprintf(d->cname, sizeof(d->cname), "nvme%d", nvme_number_offset+d->seq);
	d->cdev.minor = MISC_DYNAMIC_MINOR;
	d->cdev.parent = &d->pci_dev->dev;
	d->cdev.name = d->cname;
	d->cdev.fops = &nvmeibs_cfops;

	rv = misc_register(&d->cdev);

out:
	NFOUT;
	return rv;
}

static void destroy_local_cdev(struct device_data *d)
{
	NFIN;

	if (!list_empty(&d->cdev.list))
		misc_deregister(&d->cdev);
	nvmeibs_release_prp_pools(d);

	NFOUT;
}

static int nvmeibs_open(struct BLK_MODE_OPEN_OBJ_T *disk, BLK_MODE_T mode)
{
#if KS_HAS_BLKMODE
	struct drive_params *drv = disk->private_data;
#else
	struct drive_params *drv = disk->bd_disk->private_data;
#endif
	struct device_data *d = drv->dev;

    if (!d)
        return -ENXIO;
    if (!kref_get_unless_zero(&d->kref))
        return -ENXIO;

    return 0;
}

#if KS_HAS_BLKMODE
static void nvmeibs_release(struct gendisk *disk)
#else
#if KS_BLOCK_DEV_DEVICE_CLOSE_VOID
static void nvmeibs_release(struct gendisk *disk, BLK_MODE_T mode)
#else
static int nvmeibs_release(struct gendisk *disk, BLK_MODE_T mode)
#endif
#endif
{
	struct drive_params *drv = disk->private_data;
	struct device_data *d = drv->dev;

    kref_put(&d->kref, nvmeibs_free_drives);
#if !KS_BLOCK_DEV_DEVICE_CLOSE_VOID
	return 0;
#endif
}

static bool nvmeibs_remove1(struct pci_dev *pdev, bool do_shutdown)
{
	struct device_data *d = pci_get_drvdata(pdev);
	struct device_data **dp;
	struct nvme_qp *q;
	int qid;

	if (!d) {
		_NT(trace_nvme_nvmeibs_remove1, "Skipping remove of a device we failed to start...");
		return false;
	}

	_NT(trace_1_nvme_nvmeibs_remove1, "nvmeibs_remove1 (nvme@SEQ)  @DEV_NAME", d->seq, dev_name(&d->pci_dev->dev));

	_NT(trace_2_nvme_nvmeibs_remove1, "destroy local qs");
	end_use_all_local_qs(d);
	_NT(trace_3_nvme_nvmeibs_remove1, "destroy RDDA qs");
	end_use_all_qs(d);

	mutex_lock(&d->dev_lock);
	if (do_shutdown) {
		_NT(trace_4_nvme_nvmeibs_remove1, "shutdown device @DEV_NAME", dev_name(&d->pci_dev->dev));
		_dev_shutdown(d);
	}
	d->removed = true;
	_NT(trace_5_nvme_nvmeibs_remove1, "abandon all outstanding cmds");
	abandon_outstanding(d);
	_NT(trace_6_nvme_nvmeibs_remove1, "free drives");
	free_drives(d);
	mutex_unlock(&d->dev_lock);

	_NT(trace_7_nvme_nvmeibs_remove1, "remove /proc files");
	nvmeib_public_proc_remove(d->proc_smart);
	nvmeib_public_proc_remove(d->proc_log);

	_NT(trace_8_nvme_nvmeibs_remove1, "remove from dev list");
	down_write(&global_lock);
	for (dp = &device_list; *dp != NULL; dp = &(*dp)->next) {
		if (*dp == d) {
			*dp = d->next;
			break;
		}
	}
	up_write(&global_lock);

	_NT(trace_9_nvme_nvmeibs_remove1, "destroy cdev");
	destroy_local_cdev(d);

	for (qid = 0; qid < d->max_ioqs; ++qid) {
		if ((q = d->local_ioq[qid])) {
			if (!nvmeibs_use_nvme_kwq) {
				local_q_kthread_stop(q);
			}

			if (!d->need_reset && !d->removed) {
				destroy_sq(d, q->id);
				destroy_cq(d, q->id);
			}
			local_q_free_irq(q);
		}
	}

	free_dsm_freelist();
	if (d->adminq) {
		_ND(trace_10_nvme_nvmeibs_remove1, "try free admin irq @VECTOR", d->msix_entries[0].vector);
		free_irq(d->msix_entries[0].vector, d->adminq);
	}

	if (d->mmio) {
		if (do_shutdown) {
			_NT(trace_11_nvme_nvmeibs_remove1, "disable device @DEV_NAME", dev_name(&d->pci_dev->dev));
			d->cc &= ~NVME_CC_ENABLE;
			writel(d->cc, &d->mmio->cc);
			wait_for_ready(d, 0);
		} else {
			_NT(trace_12_nvme_nvmeibs_remove1, "Skipping shutdown");
		}
		iounmap(d->mmio);
		iounmap(d->msix_table);
		pci_release_regions(pdev);
	}

	_ND(trace_13_nvme_nvmeibs_remove1, "free_irq() done");
	if (pdev->msix_enabled)
		pci_disable_msix(pdev);
	_ND(trace_14_nvme_nvmeibs_remove1, "msix disabled.");

	if (pci_is_enabled(pdev))
		pci_disable_device(pdev);
	_ND(trace_15_nvme_nvmeibs_remove1, "pci device disabled.");

	pci_set_drvdata(pdev, NULL);
	return free_dev_data(d);
}

static struct nvmeib_ref rm_all;
static LIST_HEAD(rm_all_list);
static DECLARE_COMPLETION(rm_all_comp);

#define WAIT_FREE_DRV_TIMEOUT	30 * HZ

static void __nvmeibs_remove(struct device_data *d)
{
	struct pci_dev *pdev = d->pci_dev;
	bool cancelled;
	DECLARE_COMPLETION_ONSTACK(free_drv_comp);
	unsigned long flags;
	int wait_rv;

	_NT(t0__nvmeibs_remove,
		"pci_dev=@PCI_DEV, @DEV_NAME, @SERIAL, iommu_enabled=@BOOL_YN "
		"called from '@__BUILTIN_RETURN_ADDRESS_FUNC'",
		pdev, dev_name(&pdev->dev), d->serial, nvmeibs_iommu_enabled, __builtin_return_address(0));

	_NT(t1__nvmeibs_remove, "cancelling");
	cancelled = cancel_delayed_work_sync(&d->dwork);
	if (cancelled) {
		if (atomic_dec_and_test(&total_pending))
			complete(&scan_complete);
	}
	_NT(t2__nvmeibs_remove, "cancelled=@CANCELLED total_pending=@TOTAL_PENDING", cancelled, atomic_read(&total_pending));

	if (nvmeibs_iommu_enabled) {
		spin_lock_irqsave(&d->free_drv_comp_lock, flags);
		BUG_ON(d->free_drv_comp);
		d->free_drv_comp = &free_drv_comp;
		spin_unlock_irqrestore(&d->free_drv_comp_lock, flags);
	}

	nvmeibs_remove1(pdev, true);

	if (nvmeibs_iommu_enabled) {
		/* Change for IOMMU: We must wait for all QP DMA memory to be freed before returning from this function.
		 * Otherwise the IOMMU will delete the domain and when the memory is freed later, it will cause a kernel panic */

		_NT(t3__nvmeibs_remove, "pci_dev=@PCI_DEV, d=@DEVICE_PTR, @DEV_NAME, @SERIAL - IOMMU Enabled, waiting for drive to be freed",
		    pdev, d, dev_name(&pdev->dev), d->serial);

		if ((wait_rv = wait_for_completion_timeout(&free_drv_comp, WAIT_FREE_DRV_TIMEOUT)) <= 0) {
			_NW_dmesg(__nvmeibs_remove_wait_fail, "pci_dev=@PCI_DEV, d=@DEVICE_PTR, @DEV_NAME, @SERIAL - Failed (@RV) to wait for drive to be freed. Killing TOMA", pdev, d, dev_name(&pdev->dev), d->serial, wait_rv);

			BUG_NON_PRODUCTION(1257);

			nvmeibs_toma_shut_down();
			if ((wait_rv = wait_for_completion_timeout(&free_drv_comp, WAIT_FREE_DRV_TIMEOUT)) <= 0) {
				_NE_dmesg(__nvmeibs_remove_wait_fail2, "pci_dev=@PCI_DEV, d=@DEVICE_PTR, @DEV_NAME, @SERIAL - Failed (@RV) to wait for drive to be freed after TOMA killed", pdev, d, dev_name(&pdev->dev), d->serial, wait_rv);
				BUG();
			}
		}
	}
}

static void nvmeibs_remove(struct pci_dev *pdev)
{
	struct device_data *d;
	bool found = false;
	bool wait_rm_all = false;

	_NT(trace_0_nvme_nvmeibs_remove,
		"--> nvmeibspci_driver remove: pci_dev=@PCI_DEV, @DEV_NAME",
		pdev, dev_name(&pdev->dev));

	if (!pci_get_drvdata(pdev)) {
		_NT(trace_1_nvme_nvmeibs_remove,
			"Skipping remove of a device we failed to start...");
		goto done;
	}

	/* corner case where module-exit was called but
	   haven't called pci-unregister-driver yet */
	down_write(&global_lock);
	list_for_each_entry(d, &rm_all_list, rm_all_link) {
		if (d->pci_dev == pdev) {
			_NT(trace_2_nvme_nvmeibs_remove,
				"Found in rm-all-list: pci_dev=@PCI_DEV, @DEVICE_PTR, @SERIAL",
				pdev, d, d->serial);
			found = true;
			if (d->remove_wip) {
				wait_rm_all = true;
			}
			else {
				d->remove_wip = true;
			}
			break;
		}
	}
	if (!found) {
		/* common case of pci hot-remove */
		d = pci_get_drvdata(pdev);
	}
	up_write(&global_lock);

	_NT(trace_3_nvme_nvmeibs_remove,
		"pci_dev=@PCI_DEV, found=@BOOL, wait_rm_all=@BOOL",
		pdev, found, wait_rm_all);

	/* we could have waited for the remove of this dev only,
	   but this is already way too corner case to handle */
	if (unlikely(wait_rm_all)) {
		wait_for_completion_interruptible(&rm_all_comp);
	}
	else {
		__nvmeibs_remove(d);
	}

done:
	_NT(trace_4_nvme_nvmeibs_remove,
		"<-- nvmeibspci_driver remove: pci_dev=@PCI_DEV", pdev);

}

static void nvmeibs_remove_work(struct work_struct *work)
{
	struct device_data *d = container_of(work, struct device_data, remove_work);

	_NT(t0_nvmeibs_remove_work, "@SERIAL, remove-work start", d->serial);
	__nvmeibs_remove(d);
	_NT(t1_nvmeibs_remove_work, "remove-work done");

	nvmeib_ref_put(&rm_all);
}

static void remove_all_devices(void)
{
	struct device_data *d;
	int i = 0;
	struct workqueue_struct *rm_wq;

	/* [NVMESH-4373]: Use private work-queue for remove-work
	 * so we don't get complaints about hogging the system-wq
	 */
	if (!(rm_wq = nvmeib_public_alloc_workqueue("nvmeibs_nvme_rm_wq",
		WQ_UNBOUND, WQ_UNBOUND_MAX_ACTIVE)))
	{
		_NW_dmesg(warn_remove_all_devices_rm_wq_fail,
			  "Could not create remove WQ, using system WQ.");
		rm_wq = system_wq;
	}

	_NT(t0_remove_all_devices, "remove all devices...");
	down_write(&global_lock);
	for (d = device_list; d != NULL; d = d->next) {
		_NT(t1_remove_all_devices, "serial=@SERIAL", d->serial);

		/* ensure mutual-exclusive w/ pcie hot-remove/power,
		   if hot-remove already started, skip remove-work.
		   otherwise, in case it does, it shall wait remove-all to complete */
		if (d->remove_wip) {
			_NT(t2_remove_all_devices, "already removing");
			continue;
		}
		list_add_tail(&d->rm_all_link, &rm_all_list);
		d->remove_wip = true;

		_NT(t3_remove_all_devices, "add remove-work (@INT)", i);
		BUG_ON(nvmeib_ref_get(&rm_all) == 0);
		queue_work(rm_wq, &d->remove_work);
		i++;
	}
	up_write(&global_lock);

	_NT(t4_remove_all_devices, "wait all @INT remove-works", i);
	nvmeib_ref_release_start(&rm_all);
	nvmeib_ref_release_wait(&rm_all);
	complete_all(&rm_all_comp);

	if (rm_wq != system_wq) {
		nvmeib_public_flush_workqueue(rm_wq);
		nvmeib_public_destroy_workqueue(rm_wq);
	}
}

#if 0
static inline int get_pci_dev_max_msix(struct device_data *d)
{
#if KS_PCI_MSIX_VEC_COUNT
	NFIN;

	if((d->max_msix = pci_msix_vec_count(d->pci_dev)) <= 0)
		return -EINVAL;
#else
#define msi_control_reg(base)           (base + PCI_MSI_FLAGS)
#define msix_table_size(control)        ((control & PCI_MSIX_FLAGS_QSIZE)+1)
#define multi_msix_capable(control)     msix_table_size((control))
	int pos;
	u16 control;
	NFIN;

	if ((pos = pci_find_capability(d->pci_dev, PCI_CAP_ID_MSIX)) == 0)
		return -EINVAL;
	pci_read_config_word(d->pci_dev, msi_control_reg(pos), &control);
	if ((d->max_msix = multi_msix_capable(control)) <= 0)
		return -EINVAL;
#endif

	NFOUT;
	return 0;
}
#else
static inline int get_pci_dev_max_msix(struct pci_dev *pci_dev)
{
	int max_msix = -EINVAL;

#if KS_PCI_MSIX_VEC_COUNT
	NFIN;
	max_msix = pci_msix_vec_count(pci_dev);
#else
#define msi_control_reg(base)           (base + PCI_MSI_FLAGS)
#define msix_table_size(control)        ((control & PCI_MSIX_FLAGS_QSIZE)+1)
#define multi_msix_capable(control)     msix_table_size((control))
	int pos;
	u16 control;
	NFIN;

	if ((pos = pci_find_capability(pci_dev, PCI_CAP_ID_MSIX))) {
		pci_read_config_word(pci_dev, msi_control_reg(pos), &control);
		max_msix = multi_msix_capable(control);
	}
#endif

	NFOUT;
	return max_msix;
}
#endif


static int is_drive_in_list(const char* devname, const char* ignore_list)
{
	size_t drive_name_len;
	const char* p = ignore_list;
	const char* end;

	end = p + strlen(p);
	while (p != end) {
		drive_name_len = strcspn(p, ",\n");
		if (!drive_name_len)
			return 0;
		// found this device in the module paramater ignore list
		if (strncmp(devname, p, drive_name_len) == 0) {
			return -EINVAL;
		}
		p += drive_name_len + 1;	// skip drive name & comma separator
	}
	return 0;
}

static int should_ignore_disk_pci_addr(const char* devname)
{
	return is_drive_in_list(devname, ignore_disks);
}

static int should_ignore_disk_serial(const char* devname)
{
	return is_drive_in_list(devname, ignore_disks_serials);
}

static void test_done_work(struct work_struct *arg)
{
	struct device_data *d = container_of(to_delayed_work(arg),
							struct device_data, dwork);
	struct drive_params *drv;

	for (drv = d->drives; drv != NULL; drv = drv->next)
		nvme_add_disk(drv);
	if (atomic_dec_and_test(&total_pending)) {
		complete(&scan_complete);
		trigger_distribute_nvme_interrupts();
	}
}

static void read_write_test_sector(struct drive_params *drv);
static void test_sector_done(void *arg, int status, u32 result)
{
	struct drive_params *drv = arg;
	struct device_data *d = drv->dev;
	struct device *dev = &d->pci_dev->dev;

	if (status == 0 || status & NVME_SC_DNR ||
			time_after(jiffies, d->wait_started + nvmeibs_timeout)) {
		if (d->read_test_done || status != 0) {
			dma_unmap_page(dev, d->test_dma, PAGE_SIZE, DMA_BIDIRECTIONAL);
			d->test_dma = 0;
			__free_page(d->test_page);
			d->test_page = 0;
			if (drv->metadata) {
				dma_unmap_page(dev, d->test_meta_dma, PAGE_SIZE, DMA_BIDIRECTIONAL);
				d->test_meta_dma = 0;
				__free_page(d->test_meta_page);
				d->test_meta_page = 0;
			}
			if (status == 0) {
				d->read_test_done = false;
				INIT_DELAYED_WORK(&d->dwork, test_done_work);
				SCHEDULE_DELAYED_WORK(&d->dwork, 0);
			}
			else {
				if (d->read_test_done)
					_NE_dmesg(error_nvme_test_sector_done_write, "ERROR Disk Failed IO Test: Cannot write to nvme@SEQn@NSID, @DISK_ID_STR, err=@ERR",
						d->seq+nvme_number_offset, drv->nsid, drv->id_str, status);
				else
					_NE_dmesg(error_nvme_test_sector_done_read, "ERROR Disk Failed IO Test: Cannot read from nvme@SEQn@NSID, @DISK_ID_STR, err=@ERR",
						d->seq+nvme_number_offset, drv->nsid, drv->id_str, status);
				if ((status & 0xff) == 0x83)
					_NE_dmesg(error_2_nvme_test_sector_done, "ERROR Disk Failed IO Test: Reservation conflict!! @DISK_ID_STR", drv->id_str);
				if (atomic_dec_and_test(&total_pending)) {
					complete(&scan_complete);
					trigger_distribute_nvme_interrupts();
				}
			}
		} else {
			d->read_test_done = true;
			read_write_test_sector(drv);	/* now write */
		}
	}
	else {	/* retry */
		read_write_test_sector(drv);
	}
}

static void read_write_test_sector(struct drive_params *drv)
{
	struct device_data *d = drv->dev;
	struct device *dev = &d->pci_dev->dev;
	unsigned long flags;
	struct nvme_qp *q = d->local_ioq[0];
	struct nvme_command *cmd;

	if (!d->test_dma) {
		if (d->read_test_done) {
			_NE(error_nvme_read_write_test_sector, "Read test buffer disappear");
			return;
		}
		if (!(d->test_page = alloc_page(GFP_KERNEL)))
			goto fail;
		d->test_dma = dma_map_page(dev, d->test_page, 0, PAGE_SIZE,
					   DMA_BIDIRECTIONAL);
		if (dma_mapping_error(dev, d->test_dma)) {
			d->test_dma = 0;
			goto fail;
		}
		if (drv->metadata) {
			if (!(d->test_meta_page = alloc_page(GFP_KERNEL)))
				goto fail;
			d->test_meta_dma = dma_map_page(dev, d->test_meta_page, 0,
							PAGE_SIZE, DMA_BIDIRECTIONAL);
			if (dma_mapping_error(dev, d->test_meta_dma)) {
				d->test_meta_dma = 0;
				goto fail;
			}
		}
	}

	cmd = get_cmd(q, test_sector_done, drv, &flags);
	cmd->rw.opcode = d->read_test_done ? nvme_cmd_write : nvme_cmd_read;
	cmd->rw.nsid = cpu_to_le32(drv->nsid);
	cmd->rw.slba = 0L;
	cmd->rw.length = 0;		/* One sector */
	cmd->rw.dptr_prp1 = cpu_to_le64(d->test_dma);
	if (drv->metadata) {
		if (drv->mtdt_extd && drv->block_len >= PAGE_SIZE)
			cmd->rw.dptr_prp2 = cpu_to_le64(d->test_meta_dma);
		else
			cmd->rw.metadata = cpu_to_le64(d->test_meta_dma);
	}
	submit_cmd(q);
	spin_unlock_irqrestore(&q->q_lock, flags);
	return;
fail:
	if (d->test_dma) {
		dma_unmap_page(dev, d->test_dma, PAGE_SIZE, DMA_BIDIRECTIONAL);
		d->test_dma = 0;
	}
	if (d->test_page) {
		__free_page(d->test_page);
		d->test_page = 0;
	}
	if (d->test_meta_dma) {
		dma_unmap_page(dev, d->test_meta_dma, PAGE_SIZE, DMA_BIDIRECTIONAL);
		d->test_meta_dma = 0;
	}
	if (d->test_meta_page) {
		__free_page(d->test_meta_page);
		d->test_meta_page = 0;
	}
}

static void nvmeibs_probe1(struct work_struct *arg);

static int nvmeibs_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	int max_msix;
	struct device_data *d;
	int bars;
	int err;

	_NT(trace_nvme_nvmeibs_probe, "--> nvmeibspci_driver probe: pci_dev=@PCI_DEV   @DEV_NAME", pdev, dev_name(&pdev->dev));

	if (should_ignore_disk_pci_addr(dev_name(&pdev->dev))) {
		_NW(trace_0_nvme_nvmeibs_probe,
			"disk at @DEV_NAME is filtered-out by pci-addr",
			dev_name(&pdev->dev));
		return -EBADSLT;
	}

	/* Bump device's reference count */
	pci_dev_get(pdev);

	/* get device's MSI-X table size for rsc allocation */
	max_msix = get_pci_dev_max_msix(pdev);
	if (max_msix < 0) {
		err = -EINVAL;
		goto err_put_dev;
	}

	d = alloc_dev_data(max_msix);
	if (!d) {
		err = -ENOMEM;
		goto err_put_dev;
	}

	d->pci_dev = pdev;
	d->max_msix = max_msix;
	d->max_completions = max_completions;
	d->defer_process_io_cq = nvmeibs_defer_process_io_cq;
	d->use_intr_shaper = nvmeibs_use_intr_shaper;
	INIT_DELAYED_WORK(&d->dwork, nvmeibs_probe1);
	INIT_WORK(&d->async_work, async_event_work);
	INIT_WORK(&d->remove_work, nvmeibs_remove_work);
	d->remove_wip = false;
	spin_lock_init(&d->free_drv_comp_lock);
	d->free_drv_comp = NULL;

	down_write(&global_lock);
	d->seq = next_seq++;
	up_write(&global_lock);

	pci_set_drvdata(pdev, d);

	err = pci_enable_device_mem(pdev);
	if (err)
		goto err_free_dev;

	pci_set_master(pdev);
	bars = pci_select_bars(pdev, IORESOURCE_MEM);

	err = pci_request_selected_regions(pdev, bars, "nvmeibs");
	if (err)
		goto err_clear_master;

#if KS_DMA_SET_MASK_AND_COHERENT
	err = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
#else
	err = dma_set_mask(&pdev->dev, DMA_BIT_MASK(64));
	if (!err) {
#	if KS_HAS_GPL_SME_ACTIVE
		err = nvmeib_public_dma_set_coherent_mask(&pdev->dev, DMA_BIT_MASK(64));
#	else
		err = dma_set_coherent_mask(&pdev->dev, DMA_BIT_MASK(64));
#	endif
	}
#endif
	if (err)
		goto err_release_regions;

	d->mmio_phys = pci_resource_start(pdev, 0);
	d->mmio = ioremap(d->mmio_phys, 8192); /* fisrt doorbell is in offset 4096B ('Register Definition') thus, this limits the doorbells to 4096B */
	if (!d->mmio) {
		err = -ENOMEM;
		goto err_release_regions;
	}

	d->cap = readq(&d->mmio->cap);
	d->doorbells = (void __iomem *)d->mmio + 4096;
	d->doorbell_stride = 4 << NVME_CAP_STRIDE(d->cap);
	_ND(trace_1_nvme_nvmeibs_probe, "nvmeibs_probe: seq=@SEQ mmio_phys=@MMIO_PHYS cap=@CAP stride=@STRIDE",
		d->seq, d->mmio_phys, d->cap, d->doorbell_stride);
	_NT(trace_2_nvme_nvmeibs_probe, "csts=@CSTS", readl(&d->mmio->csts));
	_ND(trace_3_nvme_nvmeibs_probe, "Maximum Q size is @SQ_LEN", NVME_CAP_MQES(d->cap) + 1);
	if (d->doorbell_stride > 2048) {
		err = -EINVAL;
		goto err_free_mmio;
	}
	d->cc = (PAGE_SHIFT - 12) << NVME_CC_MPS_SHIFT |
			NVME_CC_IOSQES | NVME_CC_IOCQES;
	writel(d->cc, &d->mmio->cc);

#if 0
	{
		int i;
		d->wait_started = jiffies;
		for (i = 0; (readl(&d->mmio->csts) & NVME_CSTS_RDY) != 0 && i < HZ; i++)
			schedule_timeout(1);
		_ND(nvmeibs_probe_d1, "Waited for @LD/@INT sec. for not ready", jiffies - d->wait_started, HZ);
	}
#endif
	//we need to be sure that the nvme is down before configuring the
	//nvme adming queue
	err = wait_for_ready(d, 0);
	if (err)
		goto err_free_mmio;

	err = setup_adminq(d);
	if(err)
		goto err_free_mmio;

	_ND(trace_4_nvme_nvmeibs_probe, "adminq set up.");
	_ND(trace_5_nvme_nvmeibs_probe, "cc before=@BEFORE csts=@CSTS",
		readl(&d->mmio->cc), readl(&d->mmio->csts));
	d->cc |= NVME_CC_ENABLE;
	writel(d->cc, &d->mmio->cc);
	_ND(trace_6_nvme_nvmeibs_probe, "cc after=@AFTER csts=@CSTS",
		readl(&d->mmio->cc), readl(&d->mmio->csts));
	_ND(trace_7_nvme_nvmeibs_probe, "aqa=@AQA asq=@ASQ acq=@ACQ",
		readl(&d->mmio->aqa), (ulong)readq(&d->mmio->asq), (ulong)readq(&d->mmio->acq));

	d->wait_started = jiffies;
	d->wait_until = d->wait_started + NVMEIB_CAP_TIMEOUT(d->cap)*HZ/2;

	atomic_inc(&total_pending);
	SCHEDULE_DELAYED_WORK(&d->dwork, HZ / 10); /* nvmeibs_probe1 */

	_NT(trace_8_nvme_nvmeibs_probe, "<-- nvmeibspci_driver probe: pci_dev=@PCI_DEV", pdev);
	return 0;

err_free_mmio:
	iounmap(d->mmio);
err_release_regions:
	pci_release_regions(pdev);
err_clear_master:
	pci_clear_master(pdev);
	pci_disable_device(pdev);
	_ND(trace_9_nvme_nvmeibs_probe, "pci device disabled.");
err_free_dev:
	pci_set_drvdata(pdev, NULL);
	if (kref_put(&d->kref, nvmeibs_free_drives))
		_NE(error_nvme_nvmeibs_probe, "object released unexpectedly @PDEV @DEVICE_PTR", pdev, d);
	free_dev_data(d);
	goto out;	// pci_dev_put() will be called by kref

err_put_dev:
	pci_dev_put(pdev);
out:
	_NW(warn_nvme_nvmeibs_probe, "nvmeibs_probe(...): init failed err=@ERR   @DEV_NAME",
	   err, dev_name(&pdev->dev));

	return err;
}

static void nvmeibs_probe1(struct work_struct *arg)
{
	struct device_data *d = container_of(to_delayed_work(arg),
							struct device_data, dwork);
	int err = 0;
	struct drive_params *drv;
	char name_buf[16];
	struct pci_dev *pci_dev = d->pci_dev;
	bool freed = false;

	if ((readl(&d->mmio->csts) & NVME_CSTS_RDY) == 0) {
		if (time_after(jiffies, d->wait_until)) {
			_NE(error_nvme_nvmeibs_probe1, "nvmeibs_probe1(@SERIAL) device not ready. csts=@CSTS  pci_dev=@PCI_DEV",
				d->serial, readl(&d->mmio->csts), pci_dev);
			err = -ENXIO;
			goto errout;
		}
		SCHEDULE_DELAYED_WORK(&d->dwork, 1+HZ/10);
		goto immediateout;
	}
	_NT(trace_nvme_nvmeibs_probe1, "Device ready after @WAIT_TIME msec pci_dev=@PCI_DEV", 1000*(jiffies - d->wait_started)/HZ, pci_dev);

	if ((err = get_device_params(d)) < 0)
		goto errout;
	err = should_ignore_disk_serial(d->serial);
	if (err) {
		_NW(warn_1_nvme_nvmeibs_probe1,
			"disk at @DEV_NAME @SERIAL is filtered-out by serial (@ERR)",
			dev_name(&pci_dev->dev), d->serial, err);
		goto errout;
	}

	_ND(trace_1_nvme_nvmeibs_probe1, "got params");
	err = map_msix_table(d);
	if (err)
		goto errout;

	q_async_event(d);
	_NT(trace_2_nvme_nvmeibs_probe1, "Async events set up");

	down_write(&global_lock);
	d->next = device_list;
	device_list = d;
	up_write(&global_lock);

	if ((err = setup_ioqs(d)) < 0)
		goto errout;
	if (d->drives != NULL && (err = alloc_client_qs(d)) < 0)
		goto errout;
	_ND(trace_3_nvme_nvmeibs_probe1, "io Qs set up");

	snprintf(name_buf, sizeof name_buf, "smart%d", d->seq);
	d->proc_smart =
		nvmeib_public_proc_create(name_buf, nvmeibs_proc_dir, smart_fill_buf, NULL, d);
	snprintf(name_buf, sizeof name_buf, "log%d", d->seq);
	d->proc_log =
		nvmeib_public_proc_create(name_buf, nvmeibs_proc_dir, log_fill_buf, NULL, d);
	if (d->proc_smart == NULL || d->proc_log == NULL)
		_NE(error_1_nvme_nvmeibs_probe1, "Cannot create /proc files");

	if (create_local_cdev(d))
		goto errout; // YR: additional cleanup would probably be needed here.

#if START_FROZEN
	d->frozen = 1;
#endif
	d->wait_started = jiffies;
	for (drv = d->drives; drv != NULL; drv = drv->next) {
		set_info(drv);
		create_local_bdev(drv);

		if(!(drv->info.io_stats = nvmeib_io_stats_create(drv->id_str, VERB_RW_T_RECOV_BITMASK, drv->block_len))) {
			_NE(trace_get_device_params_nomem_iostats, "Failed to create iostats proc for @DEV_NAME",
				drv->id_str);
			err = -ENOMEM;
			goto errout;
		}
	}

	if (d->drives)
		read_write_test_sector(d->drives);
	else if (atomic_dec_and_test(&total_pending)) {
		complete(&scan_complete);
		trigger_distribute_nvme_interrupts();
	}
	/* nvme_add_disk(drv); will be done for all namespaces after the test sector is written */
	_NI(trace_4_nvme_nvmeibs_probe1, "nvme@SEQ: serial=@SERIAL init done pci_dev=@PCI_DEV", d->seq, d->serial, pci_dev);

	goto done;

errout:
	_NW(warn_2_nvme_nvmeibs_probe1, "nvme@SEQ: serial=@SERIAL init failed err=@ERR", d->seq, d->serial, err);
	freed = nvmeibs_remove1(pci_dev, false);
	if (atomic_dec_and_test(&total_pending)) {
		complete(&scan_complete);
		trigger_distribute_nvme_interrupts();
	}

done:
	complete_all(&d->reset_done);
	if (!freed) {
		kref_put(&d->kref, nvmeibs_free_drives);
	}

immediateout:
	return;
}

#define CORE_SERVER_NVMEOF_DISKS_PROC_FRMT_VER 1
static ssize_t nvmeof_fill_buf(void *arg, char *buf, size_t len)
{
	struct external_drive *p;
	size_t filled = 0;

	down_read(&global_lock);
	for (p = external_drives; p != NULL; p = p->next) {
		if (p->info.gendisk == NULL)
			continue;
		filled += scnprintf(buf+filled, len - filled, "%s %.*s/%.32s %d\n",
							p->info.gendisk->disk_name,
							NVMEIB_DISK_MAX_MODEL_STR_SIZE, p->model,
							p->info.disk_id, p->vendor);
		filled += nvmeib_proc_add_txt_proc_epilog(CORE_SERVER_NVMEOF_DISKS_PROC_FRMT_VER, buf + filled, len - filled);
	}
	up_read(&global_lock);
	return filled;
}

static ssize_t nvmeof_chng(void *arg, char *buf, size_t len)
{
	char *devname, *serial = NULL;
	char *model = NULL;
	struct external_drive *p;
	struct external_drive **pp;
	struct block_device *block_dev;
#if KS_HAS_BDEV_FILE_OPEN_BY_PATH
	struct file *bdev_file;
#elif KS_HAS_BDEV_OPEN_BY_PATH
	struct bdev_handle *bdh;
#endif
	u16 vendor = 0;
	int err;

	devname = strsep(&buf, ",\n");
	if (devname == NULL)
		return -EINVAL;
	if (buf)
		serial = strsep(&buf, ",\n");
	if (buf) {
		if ((err = kstrtou16(buf, 0, &vendor)) < 0)
			return err;
	}
	if (serial && serial[0] == '\0')
		serial = NULL;
	model = strsep(&serial, "/");
	if (serial == NULL) {
		serial = model;
		model = "SATA";
	}

#if KS_HAS_BDEV_FILE_OPEN_BY_PATH
	bdev_file = bdev_file_open_by_path(devname, (BLK_OPEN_READ | BLK_OPEN_WRITE), NULL, NULL);
	block_dev = IS_ERR(bdev_file) ? (struct block_device *)bdev_file : file_bdev(bdev_file);
#define BLOCKDEV_RELEASE bdev_fput(bdev_file)
#elif KS_HAS_BDEV_OPEN_BY_PATH
	bdh = bdev_open_by_path(devname, (BLK_OPEN_READ | BLK_OPEN_WRITE), NULL, NULL);
	block_dev = IS_ERR(bdh) ? (struct block_device *)bdh : bdh->bdev;
#define BLOCKDEV_RELEASE bdev_release(bdh)
#else
#if KS_BLKDEV_GET_BY_PATH_HAS_HOLDERS
	block_dev = blkdev_get_by_path(devname, FMODE_READ | FMODE_WRITE, NULL, NULL);
#define BLOCKDEV_RELEASE blkdev_put(block_dev, NULL)
#else
	block_dev = blkdev_get_by_path(devname, FMODE_READ | FMODE_WRITE, NULL);
#define BLOCKDEV_RELEASE blkdev_put(block_dev, FMODE_READ | FMODE_WRITE)
#endif
#endif

	if (IS_ERR(block_dev)) {
		if (strncmp(devname, "/dev/", 5) == 0)
			devname += 5;
		down_read(&global_lock);
		for (p = external_drives; p != NULL; p = p->next) {
			if (strcmp(p->info.gendisk->disk_name, devname) == 0) {
				block_dev = ed2bd(p);
				break;
			}
		}
		up_read(&global_lock);
		if (IS_ERR(block_dev))
			return PTR_ERR(block_dev);
	}
	else if (serial == NULL)
		BLOCKDEV_RELEASE;
	down_write(&global_lock);
	for (pp = &external_drives; (p = *pp) != NULL; pp = &p->next) {
		if (ed2bd(p) == block_dev)
			break;
	}
	if (serial == NULL) {
		if (p != NULL)
			*pp = p->next;
		up_write(&global_lock);
		if (p == NULL)
			return -ENXIO;
		_ND(trace_nvme_nvmeof_chng, "removing @DISK_NAME id=@DISK_ID_STR", p->info.gendisk->disk_name, p->info.disk_id);
		nvmeib_public_proc_remove(p->proc_smart);
		nvmeibs_disk_nvme_remove_disk(&p->info);
		/* wait for all commands to complete */
		kref_put(&p->done_kref, done_kref_release);
		wait_for_completion(&p->done);
		nvmeib_io_stats_free(p->info.io_stats);
		if (p->block_dev)
			BLOCKDEV_RELEASE;
		kfree(p);
	}
	else {
		if (p != NULL) {
			BLOCKDEV_RELEASE;
			up_write(&global_lock);
			return -EEXIST;
		}
		if ((p = kzalloc(sizeof *p, GFP_KERNEL)) == NULL) {
			BLOCKDEV_RELEASE;
			up_write(&global_lock);
			return -ENOMEM;
		}
		strlcpy(p->info.disk_id, serial, NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE);
		strlcpy(p->model, model, NVMEIB_DISK_MAX_MODEL_STR_SIZE);
		p->vendor = vendor;
#if KS_HAS_BDEV_FILE_OPEN_BY_PATH
		p->block_dev = bdev_file;
#elif KS_HAS_BDEV_OPEN_BY_PATH
		p->block_dev = bdh;
#else
		p->block_dev = block_dev;
#endif
		p->info.gendisk = ed2bd(p)->bd_disk;
		init_completion(&p->done);
		kref_init(&p->done_kref);
		_NT(trace_1_nvme_nvmeof_chng, "Added @DISK_NAME id=@DISK_ID_STR", p->info.gendisk->disk_name, p->info.disk_id);
		set_external_info(p);
		p->next = external_drives;
		external_drives = p;
		up_write(&global_lock);
		nvmeibs_disk_nvme_add_disk(&p->info);
	}
	return len;
}

static int format_disk_default(struct nvmeibs_disk_info *di,
	union nvme_format_id format_id, struct nvmeib_new_format_info *info)
{
	struct drive_params *drv = di->drv;
	struct device_data *d = drv->dev;
	struct cmd_result result;
	struct nvme_command *cmd;
	unsigned long flags;
	int err;

	init_completion(&result.completion);
	cmd = get_cmd(d->adminq, admin_cmd_done, &result, &flags);
	if (cmd == NULL)
		return -ENOMEM;

	cmd->common.opcode = nvme_admin_format_nvm;
	cmd->format.nsid = cpu_to_le32(drv->nsid);
	cmd->format.cdw10 = cpu_to_le32(format_id.val);

	nvmeibs_submit_wait(d->adminq, cmd, &result, &flags);

	if (result.status != 0) {
		_NE(error_nvme_format_disk, "nvme@SEQ@NSID: format (@VAL_INT) status = @STATUS", d->seq, drv->nsid, format_id.val, result.status);
		return result.result;
	}
	if ((err = re_ident_ns(drv)) < 0)
		return err;

	memcpy(info->new_dev_file_name, drv->gendisk->disk_name,
		sizeof(drv->gendisk->disk_name));
	info->new_seq = d->seq;
	info->new_n_pblk = drv->size;
	return 0;
}

static int format_disk_ns(struct nvmeibs_disk_info *di,
	union nvme_format_id format_id, struct nvmeib_new_format_info *info)
{
	struct drive_params *drv = container_of(di, struct drive_params, info);
	struct device_data *d = drv->dev;
	struct device *dev = &d->pci_dev->dev;
	struct cmd_result result;
	struct nvme_command *cmd;
	void *ident_buf = NULL;
	dma_addr_t ident_phys;
	struct nvme_id_ns *id_ns;
	u16 *cntlist;
	struct nvme_lbaf format;
	unsigned long flags;
	int size_factor_shift;
	u8 flbas;
	int err;

	NFIN;
	flbas = format_id.id | (format_id.is_inline << 4);
	/* pre_format: allocate buffer for nvme commands */
	ident_buf = dma_alloc_coherent(dev, 4096, &ident_phys, GFP_KERNEL);
	if (ident_buf == NULL) {
		_NE(error_format_1, "Failed to allocate user buffer for nvme command");
		err = -ENOMEM;
		goto out;
	}

	/* step #1 - get the current disk format info */
	_NT(trace_format_1, "step #1 - get the current disk format info");
	if (get_ident(d, drv->nsid, ident_phys) != 0) {
		_NE(error_format_2, "Failed to identify namespace");
		err = -EIO;
		goto out;
	}
	id_ns = ident_buf;
	size_factor_shift =
		id_ns->lbaf[format_id.id].ds - id_ns->lbaf[id_ns->flbas & 0xf].ds;

	/* step #2 - delete the namespace */
	_NT(trace_format_2, "step #2 - delete the namespace "
	   "size_factor_shift=@SECTOR_SHIFT (curr-ds=@INT, req-ds=@INT)",
		size_factor_shift,
		id_ns->lbaf[id_ns->flbas & 0xf].ds,
		id_ns->lbaf[format_id.id].ds);
	init_completion(&result.completion);
	cmd = get_cmd(d->adminq, admin_cmd_done, &result, &flags);
	if (cmd == NULL) {
		_NE(error_format_3, "Failed to get nvme command");
		err = -ENOMEM;
		goto out;
	}
	cmd->common.opcode = nvme_admin_ns_mgmt;
	cmd->common.nsid = cpu_to_le32(drv->nsid);
	cmd->common.cdw10[0] = 1;

	nvmeibs_submit_wait(d->adminq, cmd, &result, &flags);

	if (result.status != 0) {
		_NE(error_format_4, "nvme@SEQ n@NSID: delete_ns status=@STATUS\n",
			d->seq, drv->nsid, result.status);
		err = result.status;
		goto out;
	}
	/* step #3 - create namespace */
	_NT(trace_format_3, "step #3 - create namespace "
						"(flabs=@HEX08={id=@INT, is-inline=@INT}",
		flbas, format_id.id, format_id.is_inline);
	reinit_completion(&result.completion);
	cmd = get_cmd(d->adminq, admin_cmd_done, &result, &flags);
	if (cmd == NULL) {
		_NE(error_format_5, "Failed to get nvme command");
		err = -ENOMEM;
		goto out;
	}
	memset(id_ns, 0, sizeof(*id_ns));
	if (size_factor_shift > 0) {
		id_ns->nsze = cpu_to_le64(drv->size) >> size_factor_shift;
		id_ns->ncap = cpu_to_le64(drv->ncap) >> size_factor_shift;
	}
	else {
		id_ns->nsze = cpu_to_le64(drv->size) << -size_factor_shift;
		id_ns->ncap = cpu_to_le64(drv->ncap) << -size_factor_shift;
	}
	id_ns->flbas = flbas;
	id_ns->dps = drv->dps;
	id_ns->nmic = drv->nmic;
	cmd->common.opcode = nvme_admin_ns_mgmt;
	cmd->identify.dptr_prp1 = cpu_to_le64(ident_phys);
	if ((ident_phys & ~PAGE_MASK) != ((ident_phys + 4095) & ~PAGE_MASK))
		cmd->identify.dptr_prp2 = cpu_to_le64((ident_phys + 4096) & PAGE_MASK);
	cmd->common.cdw10[0] = 0;
	nvmeibs_submit_wait(d->adminq, cmd, &result, &flags);
	if (result.status != 0) {
		_NE(error_format_6, "nvme@SEQ n@NSID: delete_ns status=@STATUS\n",
			d->seq, drv->nsid, result.status);
		err = result.status;
		goto out;
	}
	else {
		_NT(trace_format_3a, "nvme@SEQ n@NSID: new ns_id = @NSID\n", d->seq, drv->nsid, result.result);
		drv->nsid = le32_to_cpu(result.result);
	}
	/* step #4 - attach the namespace */
	_NT(trace_format_4, "step #4 - attach the namespace");
	reinit_completion(&result.completion);
	cmd = get_cmd(d->adminq, admin_cmd_done, &result, &flags);
	if (cmd == NULL) {
		_NE(error_format_7, "Failed to get nvme command");
		err = -ENOMEM;
		goto out;
	}
	memset(ident_buf, 0, 4096);
	cntlist = ident_buf;
	cntlist[0] = cpu_to_le16(1);
	cntlist[1] = cpu_to_le16(d->cntlid);
	cmd->common.opcode = nvme_admin_ns_attach;
	cmd->common.nsid = cpu_to_le32(drv->nsid);
	cmd->identify.dptr_prp1 = cpu_to_le64(ident_phys);
	if ((ident_phys & ~PAGE_MASK) != ((ident_phys + 4095) & ~PAGE_MASK))
		cmd->identify.dptr_prp2 = cpu_to_le64((ident_phys + 4096) & PAGE_MASK);
	cmd->common.cdw10[0] = 0;
	nvmeibs_submit_wait(d->adminq, cmd, &result, &flags);
	if (result.status != 0) {
		_NE(error_format_8, "nvme@SEQ n@NSID: create_ns status=@STATUS",
			d->seq, drv->nsid, result.status);
		err = result.status;
		goto out;
	}
	/* step #5 - reident the namespace */
	_NT(trace_format_5, "step #5 - reident the namespace");
	if((err = get_ident(d, drv->nsid, ident_phys)) != 0) {
		_NE(error_format_9, "nvme@SEQ n@NSID: faile to reident namescape status=@STATUS",
			d->seq, drv->nsid, err);
		goto out;
	}
	id_ns = ident_buf;
	drv->size = le64_to_cpu(id_ns->nsze);
	drv->ncap = le64_to_cpu(id_ns->ncap);
	drv->flbas = id_ns->flbas;
	drv->dps = id_ns->dps;
	drv->nmic = id_ns->nmic;
	format = id_ns->lbaf[id_ns->flbas & 0xf];
	drv->block_len = 1 << format.ds;
	drv->metadata = le16_to_cpu(format.ms);
	/* mc:
	   1 is inline
	   2 is separate
	   3 is both
	   flbas is the current format and if 10 is on it is inline
	*/
	if (drv->metadata != 0)
		drv->mtdt_extd = ((id_ns->mc & 1) && (id_ns->flbas & 0x10));
	else
		drv->mtdt_extd = false;

	err = set_drv_id_str(drv);
	if (err) {
		goto out;
	}
	_NT(trace_format_6,
		"nvme@SEQ: Namespace (@SERIAL) blklen=@BLOCK_LEN nblocks=@NBLOCKS_LLONG"
		" mtds=@MTDS, separated=@YES_NO_STATUS",
		drv->nsid, drv->id_str, drv->block_len, drv->size,
		drv->metadata, drv->mtdt_extd ? "N" : "Y");

	err = 0;

out:
	if (ident_buf)
		dma_free_coherent(dev, 4096, ident_buf, ident_phys);
	NFOUT;
	return err;
}

static struct nvmeibs_disk_info *disk_change_freeze_state(
	const char *disk_name, bool is_freeze)
{
	struct device_data *d;
	struct drive_params *drv = NULL;
	struct nvmeibs_disk_info *di = NULL;
	bool found = false;
	struct completion *wait_for = NULL;

	NFIN;
	down_write(&global_lock);
	BLOCK(drv) {
		for (d = device_list; d != NULL; d = d->next) {
			for (drv = d->drives; drv != NULL; drv = drv->next) {
				if (drv->info.disk_id[0] == '\0')
					continue;
				if (!strncmp(
					disk_name, drv->info.disk_id, sizeof(drv->info.disk_id))) {
					found = true;
					BREAK(drv);
				}
			}
		}
	}
	if (!found) {
		up_write(&global_lock);
		_NE(error_nvme_disk_change_freeze_state, "Cannot freeze disk @DISK_NAME - disk not found", disk_name);
		goto out;
	}
	mutex_lock(&d->dev_lock);
	up_write(&global_lock);
	drv->frozen = is_freeze;
	if (is_freeze) {
		if (!kref_get_unless_zero(&d->kref)) {
			_NE(error_1_nvme_disk_change_freeze_state, "Cannot freeze disk @DISK_NAME - disk is busy", disk_name);
			goto unlock;
		}
		di = &drv->info;
		wait_for = &di->remove_done;
		init_completion(wait_for);
		freeze_wait_add(drv);
		nvme_remove_disk(drv);
	}
	else {
		if (!d->need_reset && !d->removed && !d->reset_pending) {
			nvme_add_disk(drv);
		}
		else
			_NT(trace_nvme_disk_change_freeze_state, "Cannot add unfrozen disk @SERIAL (@DEVICE_PTR) to srv while rst/rm, "
			   "(nr=@NEED_RESET, rm=@REMOVED, rp=@RESET_PENDING)", d->serial, d,
			   d->need_reset, d->removed, d->reset_pending);
	}

unlock:
	mutex_unlock(&d->dev_lock);
	if (!is_freeze)
		kref_put(&d->kref, nvmeibs_free_drives);
	else if (wait_for) {
		_NT(trace_1_nvme_disk_change_freeze_state, "Disk @DISK_NAME - wait for remove completion", disk_name);
		wait_for_completion_interruptible(wait_for);
		_NT(trace_2_nvme_disk_change_freeze_state, "Disk @DISK_NAME - remove complete", disk_name);
		freeze_wait_del(drv);
	}

out:
	NFOUT;
	return di;
}

static struct nvmeibs_disk_info *disk_freeze(const char *disk_name)
{
	return disk_change_freeze_state(disk_name, true);
}

static void disk_unfreeze(const char *disk_name)
{
	disk_change_freeze_state(disk_name, false);
}

corecomm_inj_code(
	struct nvmeibs_disk_info *nvmeib_disk_freeze(const char *disk_name) {
		return disk_freeze(disk_name);
	}
	EXPORT_SYMBOL(nvmeib_disk_freeze);

	void nvmeib_disk_unfreeze(const char *disk_name) {
		disk_unfreeze(disk_name);
	}
	EXPORT_SYMBOL(nvmeib_disk_unfreeze);
);

static int reset_controller(struct nvmeibs_disk_info *di)
{
	int err = 0;
	struct device_data *d = di->dev;
	struct drive_params *drv;

	if (d == NULL)
		return -EINVAL;
	init_completion(&d->reset_done);
	initaite_dev_rst(trace_dev_rst_reset_controller, d);
	/* Add 2 seconds for polling delay in out thread */
	_NT(trace_1_reset_controller, "Will wait @WAIT_TIME sec", (long)(4+NVMEIB_CAP_TIMEOUT(d->cap))/2);
	if (wait_for_completion_timeout(&d->reset_done,
							(4+NVMEIB_CAP_TIMEOUT(d->cap))*HZ/2) <= 0)
		return -ETIMEDOUT;
	mutex_lock(&d->dev_lock);
	for (drv = d->drives; drv != NULL; drv = drv->next)
		if ((err = re_ident_ns(drv)) != 0)
			break;
	mutex_unlock(&d->dev_lock);
	return err;
}

int nvmeibs_nvme_format_disk(const char *disk_name,
	struct nvmeib_format_disk *fd, struct nvmeib_new_format_info *info)
{
	struct nvmeibs_disk_info *di;
	int rv;

	NFIN;

	/* release also changes the disk state into dying */
	_NT(trace_1_nvme_nvmeibs_nvme_format_disk,
		"[1] freezing (remove from target)...");
	if ((di = disk_freeze(disk_name)) == NULL) {
		_NE(error_nvme_nvmeibs_nvme_format_disk,
			"Fail to freeze disk @DISK_NAME", disk_name);
		rv = -1;
	} else {
		_NT(trace_2_nvme_nvmeibs_nvme_format_disk,
			"[2] formatting (format-id={id=@INT, is-inline=@INT}, ns=@BOOL, rst=@BOOL)...",
			fd->format_id.id, fd->format_id.is_inline,
			fd->flag_delete_create_ns, fd->flag_reset_ctrlr);

		rv = fd->flag_delete_create_ns ?
			format_disk_ns(di, fd->format_id, info) :
			format_disk_default(di, fd->format_id, info);

		if (rv < 0)
			_NE(error_1_nvme_nvmeibs_nvme_format_disk,
				"Fail to format disk @DISK_ID_STR (rv @RV)", di->disk_id, rv);
		else {
			nvmeib_io_stats_clear(di->io_stats, 'A');
			nvmeib_io_stats_set_block_size(di->io_stats, di->block_size);
			if (fd->flag_reset_ctrlr)
				rv = reset_controller(di);
		}

		_NT(trace_3_nvme_nvmeibs_nvme_format_disk,
			"[3] unfreezing (add to target)...");
		disk_unfreeze(disk_name);
	}

	NFOUT;
	return rv;
}

void nvmeibs_nvme_fill_disk_info(const struct nvmeibs_disk_info *di,
	struct nvmeib_disk_info *info)
{
	NFIN;
	memcpy(info->disk_id, di->disk_id, sizeof(di->disk_id));
	memcpy(info->dev_name, di->gendisk->disk_name,
		sizeof(di->gendisk->disk_name));
	if (di->status_str)
		memcpy(info->status, di->status_str,
			min(sizeof(info->status), strlen(di->status_str)));
	info->n_blocks = di->blocks;
	info->n_hw_blocks = di->hw_blocks;
	info->block_size = di->block_size;
	info->max_request_size = di->max_request_size;
	info->seq = di->dev ? di->dev->seq : -1;
	info->nsid = di->nsid;
	info->metadata = di->metadata;
	info->vendor_id = nvmeibs_nvme_get_vendor(di);
	memcpy(info->model_str, nvmeibs_nvme_get_model(di->dev), sizeof(info->model_str));
	memcpy(info->native_serial_str, nvmeibs_nvme_get_native_serial(di->dev), sizeof(info->native_serial_str));
	NFOUT;
}

int nvmeibs_nvme_identify_disk(const struct nvmeibs_disk_info *di,
	user_cb cb, void *priv)
{
	struct drive_params *drv = di->drv;
	struct device_data *d = drv->dev;
	struct device *dev = &d->pci_dev->dev;
	dma_addr_t ident_dma;
	void *id_ns;
	int rv;

	NFIN;
	id_ns = dma_alloc_coherent(dev, 4096, &ident_dma, GFP_KERNEL);
	if (id_ns == NULL) {
		rv = -ENOMEM;
		goto out;
	}
	if ((rv = get_ident(d, drv->nsid, ident_dma)) != 0) {
		rv = -EIO;
		goto out;
	}
	cb(id_ns, 4096, priv);
	dma_free_coherent(dev, 4096, id_ns, ident_dma);
	rv = 0;

out:
	NFOUT;
	return rv;
}

struct nvmeibs_q_info *nvmeibs_q_info_get_by_rsc_id(struct nvmeibs_disk_info *di, int rsc_id) {
	int i;
	for (i = 0; i < di->n_qs; ++i) {
		if (di->qs[i].qid == rsc_id) { return &di->qs[i]; }
	}
	return NULL;
}

void nvmeibs_nvme_free_all_nvmeof(void)
{
	struct external_drive *p;

	down_write(&global_lock);
	while (external_drives != NULL) {
		p = external_drives;
		external_drives = p->next;
		up_write(&global_lock);
		nvmeib_public_proc_remove(p->proc_smart);
		nvmeibs_disk_nvme_remove_disk(&p->info);
#if KS_HAS_BDEV_FILE_OPEN_BY_PATH
		bdev_fput(p->block_dev);
#elif KS_HAS_BDEV_OPEN_BY_PATH
		bdev_release(p->block_dev);
#else
#if KS_BLKDEV_GET_BY_PATH_HAS_HOLDERS
		blkdev_put(p->block_dev, NULL);
#else
		blkdev_put(p->block_dev, FMODE_WRITE|FMODE_READ);
#endif
#endif
		nvmeib_io_stats_free(p->info.io_stats);
		kfree(p);
		down_write(&global_lock);
	}
	up_write(&global_lock);

}

#define CORE_SERVER_NVME_QPS_PROC_FRMT_VER 1
ssize_t nvmeibs_nvme_fill_stats_nvme_qps(struct nvmeibs_disk_info *di, char *buf, size_t len)
{
#define BUF_ADD(...) count += scnprintf(buf+count, len-count, __VA_ARGS__)
	struct device_data *d = di ? di->dev : NULL;
	int qid;
	struct nvme_qp *q;
	ssize_t count = 0;

	for (qid = 0; qid < d->max_ioqs; ++qid) {
		if ((q = d->local_ioq[qid]))
			BUF_ADD("qid %03d:\t reads: %-10d\t writes: %d\n", qid, q->nvme_qp_stats.rd_count,
														q->nvme_qp_stats.wr_count);
		else
			BUF_ADD("qid %03d:\t Not init\n", qid);

	}
#undef BUF_ADD
	count += nvmeib_proc_add_yaml_proc_epilog(CORE_SERVER_NVME_QPS_PROC_FRMT_VER, buf + count, len - count);
	return count;
}
EXPORT_SYMBOL(nvmeibs_nvme_fill_stats_nvme_qps);


#ifndef PCI_CLASS_STORAGE_EXPRESS
#define PCI_CLASS_STORAGE_EXPRESS	0x010802
#endif

static const struct pci_device_id nvme_id_table[] = {
	{ PCI_DEVICE_CLASS(PCI_CLASS_STORAGE_EXPRESS, 0xffffff) },
	{ 0, }
};
//MODULE_DEVICE_TABLE(pci, nvme_id_table);

static struct pci_driver nvmeibspci_driver = {
	.name		= "nvmeibs",
	.id_table	= nvme_id_table,
	.probe		= nvmeibs_probe,
	.remove		= nvmeibs_remove,
	.shutdown	= nvmeibs_shutdown,
};

#define CALL_IB

static int __init nvmeibspci_init(void)
{
	int err;

	_NT(trace_0_nvme_nvmeibspci_init, "nvmeibspci_init(): start");

	np_cpus = num_possible_cpus();
	_ND(trace_1_nvme_nvmeibspci_init, "np_cpus @NP_CPUS", np_cpus);
	if (np_cpus < nvmeibs_min_local_nvmeqs) {
		_NW_dmesg(nvmeibspci_init_cpu_less_qs,
		 "min_local_nvmeqs was set to @INT but Number of CPUS is @INT, setting min_local_nvmeqs to @INT",
		 nvmeibs_min_local_nvmeqs, np_cpus, np_cpus);
		nvmeibs_min_local_nvmeqs = np_cpus;
	}

	if (nvmeibs_max_local_nvmeqs > np_cpus || nvmeibs_max_local_nvmeqs == 0) {
		_NT(nvmeibspci_init_max_qs_1,
			"override nvmeibs_max_local_nvmeqs: @INT -> @INT",
			nvmeibs_max_local_nvmeqs, np_cpus);
		nvmeibs_max_local_nvmeqs = np_cpus;
	}
	if (nvmeibs_max_local_nvmeqs < nvmeibs_min_local_nvmeqs) {
		_NT(nvmeibspci_init_max_qs_0,
			"override nvmeibs_max_local_nvmeqs: @INT -> @INT",
			nvmeibs_min_local_nvmeqs, nvmeibs_min_local_nvmeqs);
		nvmeibs_max_local_nvmeqs = nvmeibs_min_local_nvmeqs;
	}

	nvmeibs_proc_dir = proc_mkdir("nvmeibs", NULL);
	if (nvmeibs_proc_dir == NULL) {
		_NE(error_nvme_nvmeibspci_init, "Fail to create /proc/nvmeibs directory");
		return -EEXIST;
	}
	nvmeibs_proc_disks_dir = proc_mkdir("disks", nvmeibs_proc_dir);
	if (nvmeibs_proc_disks_dir == NULL) {
		_NE(error_nvme_nvmeibspci_init_2, "Fail to create /proc/nvmeibs/disks directory");
		return -EEXIST;
	}
	atomic_set(&total_pending, 1);
	nvmeib_ref_init(&rm_all);
#ifdef CALL_IB
	if ((err = nvmeibs_init()) != 0) {
		goto err;
	}
#endif

	local_major = register_blkdev(0, "nvmeibs");

	/* create ioqm wq before registering probe
	   method which can add-disk to srv-layer */
	if (!(ioqm_wq = wq_create(proc_name_format("S", "WQ", "ioqm")))) {
		_NE(error_1_nvme_nvmeibspci_init, "Failed to allocate ioq manager work queue");
		err = -ENOMEM;
		goto err;
	}

	if (nvmeibs_use_nvme_kwq) {
		unsigned int flags = WQ_MEM_RECLAIM | WQ_SYSFS;
		if (nvmeibs_nvme_wq_unbound)
			flags |= WQ_UNBOUND;
		nvmeibs_nvme_wq = nvmeib_public_alloc_workqueue("nvmeibs_nvme", flags, 0);
		if (!nvmeibs_nvme_wq) {
			_NE(error_2_nvme_nvmeibspci_init_d, "Failed to allocate nvmeibs_nvme work queue");
			err = -ENOMEM;
			goto err;
		}
		_NT(trace_nvme_nvmeibspci_init_d, "Created nvmeibs_nvme work queue");
	}

	nvmeibs_kthread = kthread_run(nvmeibs_nvme_thread_func, NULL, "nvmeibs");
	if (IS_ERR(nvmeibs_kthread)) {
		_NE(error_2_nvme_nvmeibspci_init, "Failed to create nvmeibs thread @PTR_ERR", PTR_ERR(nvmeibs_kthread));
		nvmeibs_kthread = NULL;
		err = PTR_ERR(nvmeibs_kthread);
		goto err;
	}

	if((err = pci_register_driver(&nvmeibspci_driver)) < 0)
		goto err;
	_ND(trace_2_nvme_nvmeibspci_init, "nvmeibspci_init(): pci driver registered");
	if (!atomic_dec_and_test(&total_pending)) {
		_NT(trace_3_nvme_nvmeibspci_init, "calling wait_for_completion");
		wait_for_completion(&scan_complete);
	} else
		trigger_distribute_nvme_interrupts();

	/* Only after all disks we've probed were added,
	   we register on all ib ports and start listen
	   to clients - o/w we could have reject clients
	   looking for disks that their probe was not
	   completed yet */
	nvmeibs_nvme_disk_scan_done();
	nvmeof_proc = nvmeib_public_proc_create("nvmeof_disks", nvmeibs_proc_dir,
									nvmeof_fill_buf, nvmeof_chng, NULL);
	dummy_disk_add();
	_NT(trace_4_nvme_nvmeibspci_init, "nvmeibspci_init(): done");
	return 0;
err:
#ifdef CALL_IB
	nvmeibs_exit();
#endif
	if (nvmeibs_kthread)
		kthread_stop(nvmeibs_kthread);
	if (local_major)
		unregister_blkdev(local_major, "nvmeibs");
	if (ioqm_wq)
		wq_destroy(ioqm_wq);
	if (nvmeof_proc)
		nvmeib_public_proc_remove(nvmeof_proc);
	if (nvmeibs_proc_disks_dir)
		remove_proc_entry("disks", nvmeibs_proc_dir);
	if (nvmeibs_proc_dir)
		remove_proc_entry("nvmeibs", NULL);
	if (nvmeibs_use_nvme_kwq && nvmeibs_nvme_wq)
		nvmeib_public_destroy_workqueue(nvmeibs_nvme_wq);

	_NE(error_3_nvme_nvmeibspci_init, "nvmeibspci_init(): err=@INT", err);
	return err;
}


static void __exit nvmeibspci_exit(void)
{
	_NT(trace_nvme_nvmeibspci_exit, "nvmeibspci_exit(): start");

	/* First remove the virtual disks as they use a real disks when local */
	nvmeibs_nvme_free_all_nvmeof();
	remove_all_devices();

	_NT(trace_1_nvme_nvmeibspci_exit, "nvmeibspci_driver unregister ...");
	pci_unregister_driver(&nvmeibspci_driver); /* call .remove method per drv */
	_NT(trace_2_nvme_nvmeibspci_exit, "nvmeibspci_driver unregistered");

	dummy_disk_remove();

#ifdef CALL_IB
	nvmeibs_exit();
#endif

	_NI(trace_3_nvme_nvmeibspci_exit, "Waiting until interrupts distribution script is disabled");
	wait_until_distribute_interrupts_done();

	// driver_remove_file(&nvmeibspci_driver.driver, &driver_attr_disk_info);
	unregister_blkdev(local_major, "nvmeibs");
	if (nvmeibs_kthread != NULL) {
		int err;
		err = kthread_stop(nvmeibs_kthread);
		nvmeibs_kthread = NULL;
		_NT(trace_4_nvme_nvmeibspci_exit, "kthread_stop returned @ERR", err);
	}

	/* ioqm works should be able to run, or at list validate the dev,
	   even after removing devices */
	wq_drain(ioqm_wq);
	wq_destroy(ioqm_wq);
	if (nvmeibs_use_nvme_kwq && nvmeibs_nvme_wq)
		nvmeib_public_destroy_workqueue(nvmeibs_nvme_wq);

	nvmeib_public_proc_remove(nvmeof_proc);
	remove_proc_entry("disks", nvmeibs_proc_dir);
	remove_proc_entry("nvmeibs", NULL);
	_NT(trace_5_nvme_nvmeibspci_exit, "nvmeibspci_exit(): done");
}

MODULE_AUTHOR("Excelero Storage Ltd.");
// MODULE_LICENSE("Proprietery");
MODULE_VERSION("0.1");
module_init(nvmeibspci_init);
module_exit(nvmeibspci_exit);
