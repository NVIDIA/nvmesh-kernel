#define TOMA_SANDBOX_BYPASS_REDIRECTS // allow calling real OS I/O functions from this module - must be defined before any other includes
#include "sandbox_nvme.h"
#include "nvmeibt_debug.h"
#include "toma_in_sandbox.h"

static const struct sandbox_nvme_lbaf lbaf_table[SANDBOX_NVME_LBAF_COUNT] = {
	[SANDBOX_NVME_FMT_512_0]  = { .block_size_exp = 9,  .metadata_size = 0 },  /* 512+0 */
	[SANDBOX_NVME_FMT_512_8]  = { .block_size_exp = 9,  .metadata_size = 8 },  /* 512+8 */
	[SANDBOX_NVME_FMT_4096_0] = { .block_size_exp = 12, .metadata_size = 0 },  /* 4096+0 */
	[SANDBOX_NVME_FMT_4096_8] = { .block_size_exp = 12, .metadata_size = 8 },  /* 4096+8 */
};

const struct sandbox_nvme_lbaf *sandbox_nvme_get_lbaf(enum SANDBOX_NVME_FMT_e fmt_idx) {
	BUG_ON(fmt_idx < 0 || fmt_idx >= SANDBOX_NVME_LBAF_COUNT);
	return &lbaf_table[fmt_idx];
}

/*
 * # Disk size requirement
 * Relevant constants involved:
 * - METADATA_PARTITION_RATIO (nvmeibt_params.h) = 0.15 (sandbox) vs 0.005 (production)
 * - journal_data_size_in_pblks (nvmeibt_read_config.c) = 1MB (sandbox) vs 2GB (production)
 * - serjio_db_size_in_pblks (nvmeibt_read_config.c) = 1MB (sandbox) vs 32MB (production)
 * The allocation uses 1MB (256 block) alignment internally via align_pba_s_up_to_blkset().
 * For the metadata partition's usable space (first_usable_pba to last_usable_pba) to contain at least one complete 1MB-aligned region:
 *   - first_usable_pba = metadata_pba_s + 257
 *   - last_usable_pba = metadata_pba_e - 257
 *   - aligned_start = roundup(first_usable_pba, 256)
 *   - aligned_end = rounddown(last_usable_pba + 1, 256) - 1
 *   - Need: aligned_end >= aligned_start + 34 (for disk_metadata partition)
 *
 * With 8192 blocks, metadata partition is 1024 blocks (PBA 512-1535), giving:
 *   - aligned_start = roundup(769, 256) = 1024
 *   - aligned_end = rounddown(1279, 256) - 1 = 1023
 *   - Result: NO usable 1MB-aligned space (end < start)!
 * Required: metadata partition >= 1536 blocks to span two 1MB boundaries.
 * Calculation: floor(n_pblk * 0.15) - 261 >= 1025 => n_pblk >= 8574 blocks
 * Using 10240 blocks (40MB) to provide a comfortable margin.
 */
#define SANDBOX_DEV_DIR TOMA_ROOT_DIR "dev/"			// Location of the virtual /dev directory. We'll create it, and create files in it, at runtime.
static struct sandbox_nvme_device nvme_devices[] = {
	{ 0x1401, "STKD_SN_001", "STKD_MN_001", "nvme" "0n1", SANDBOX_DEV_DIR "nvme0" "n1", true,   2048, SANDBOX_NVME_FMT_4096_0 },
	{ 0x1402, "NVMD_SN_002", "NVMD_NN_002", "nvme1001n1", SANDBOX_DEV_DIR "nvme1001n1", false, 10240, SANDBOX_NVME_FMT_4096_0 },
	{ 0x1403, "NVMD_SN_003", "NVMD_NN_003", "nvme1002n1", SANDBOX_DEV_DIR "nvme1002n1", false, 10240, SANDBOX_NVME_FMT_4096_0 },
};

#define NVME_DEVICE_COUNT ARRAY_SIZE(nvme_devices)

static void write_file(const char *path, const char *content) {
	FILE *fp = fopen(path, "w");
	BUG_ON(!fp);
	BUG_ON(fputs(content, fp) == EOF);
	BUG_ON(fclose(fp) != 0);
}

static void create_simulated_locks_file(const char *serial_number) {
	char locks_path[256];	// Locks file is named after the disk_id (serial.nsid). Our sandbox disks use nsid=1.
	const int n = snprintf(locks_path, sizeof(locks_path), TOMA_ROOT_DIR "proc/nvmeibs/locks.%s.1", serial_number);
	int fd = open(locks_path, O_CREAT | O_RDWR, 0644);
	BUG_ON((n < 0) || (n >= (int)sizeof(locks_path)) || (fd < 0));
	BUG_ON(ftruncate(fd, 4096) < 0);		// Extend to one page size for mmap.
	BUG_ON(close(fd) != 0);
	N_Tf(sbu3403, "created locks file @STR", locks_path);
}

// Set up the NVMe disk data. Currently just a static configuration, but ultimately we'll add dynamic modification adding and removing disks.
static void disk_init(const char *dest_path, const char *src_path);
void sandbox_nvme_init(void) {
	N_Tf(sbu3401, "initializing static simulated NVMe disks");
	#define TEST_DATA_BUILD_DIR "99bin/testdata/"
	for (int i = 0; i < (int)NVME_DEVICE_COUNT; ++i) {	// sandbox_nvme_get_device_count
		const struct sandbox_nvme_device *d = &nvme_devices[i];
		const char *template_src = d->stock_disk ? TEST_DATA_BUILD_DIR "disk_stock.img" : TEST_DATA_BUILD_DIR "disk_nvmesh.img";
		disk_init(d->device_path, template_src);
		if (!d->stock_disk)								// Create simulated /proc files for NVMesh disks
			create_simulated_locks_file(d->serial_number);
	}
	#define TARGET_DEVICES_FILE TOMA_ROOT_DIR "var/opt/nvmesh/.target_devices"
	write_file(TARGET_DEVICES_FILE, "nvme,STKD_SN_001,5121,STKD_MN_001,1\n");		// Generate the disk data files.
	N_Tf(sbu3402, "done initializing NVMe disks");
}

const struct sandbox_nvme_device *sandbox_nvme_get_device_by_path(const char *path) {
	for (int i = 0; i < (int)NVME_DEVICE_COUNT; ++i) {
		const struct sandbox_nvme_device *d = &nvme_devices[i];
		if (!strcmp(d->device_path, path)) {
			N_Tf(kdj3946, "found device for path=@STR", path);
			return d;
		}
	}
	return NULL;		// gpt_util checks this flow. Should never occur in real toma
}

unsigned sandbox_nvme_get_device_count(void) { return (unsigned)NVME_DEVICE_COUNT; }
const struct sandbox_nvme_device *sandbox_nvme_get_device_arr(void) { return &nvme_devices[0]; }

const struct sandbox_nvme_device *sandbox_nvme_get_device_by_disk_id(const char *disk_id) {
	const char *dot = strchr(disk_id, '.');		// disk_id format is "SERIAL.NSID" e.g. "NVMD_SN_002.1", We need to match the serial number portion
	const int serial_len = (dot ? (int)(dot - disk_id) : (int)strlen(disk_id));
	for (int i = 0; i < (int)NVME_DEVICE_COUNT; ++i) {
		const struct sandbox_nvme_device *d = &nvme_devices[i];
		if (!strncmp(d->serial_number, disk_id, serial_len))
			return d;
	}
	BUG_ON(true); return NULL;
}

static int sandbox_nvme_open(const struct sandbox_nvme_device *dev) { return open(dev->device_path, O_RDWR); }

int sandbox_nvme_format_disk(const char* disk_id, enum SANDBOX_NVME_FMT_e fmt_idx) {
	struct sandbox_nvme_device *dev = (struct sandbox_nvme_device *)sandbox_nvme_get_device_by_disk_id(disk_id);	// Mutable
	const struct sandbox_nvme_lbaf *lbaf = sandbox_nvme_get_lbaf(fmt_idx);
	const int fd = sandbox_nvme_open(dev);
	int rv = 0;
	N_Tf(fmt3948, "format disk serial=@STR fmt_idx=@INT @INT+@INT[b]", dev->serial_number, fmt_idx, 1 << lbaf->block_size_exp, lbaf->metadata_size);
	BUG_ON(fd <= 0);
	// Erase disk content (simulate NVMe format behavior)
	// Note: The NVMESH_FORMATTED_DISK header is written by Toma's production code (format_disk_wrapper) after receiving the format reply, not by the sandbox's format simulation.
	if (ftruncate(fd, 0) != 0) {
		N_Ef(fmt_trunc, "format disk truncate failed serial=@STR err=@AUTO_ERRNO", dev->serial_number);
		rv = -1;
	} else if (ftruncate(fd, (off_t)(dev->size_in_blocks * (1UL << lbaf->block_size_exp))) != 0) {
		N_Ef(fmt_expand, "format disk expand failed serial=@STR err=@AUTO_ERRNO", dev->serial_number);
		rv = -1;
	}
	close(fd);
	if (rv == 0) {
		dev->current_format_idx = (uint8_t)fmt_idx;
		N_Tf(fmt_done, "format disk complete serial=@STR", dev->serial_number);
	}
	return rv;
}

int sandbox_nvme_zero_disk_area(const char* disk_id, size_t start_block, size_t num_blocks) {
	const struct sandbox_nvme_device *dev = sandbox_nvme_get_device_by_disk_id(disk_id);
	const struct sandbox_nvme_lbaf *lbaf = sandbox_nvme_get_lbaf(dev->current_format_idx);
	const size_t block_size = (size_t)(1U << lbaf->block_size_exp);
	const size_t chunk_bytes = (1 << 20);		// Write units of 1[mb]
	void *zero_buf = calloc(1, chunk_bytes);
	size_t total_bytes = num_blocks  * block_size;
	size_t offset =      start_block * block_size;
	const int fd = sandbox_nvme_open(dev);
	BUG_ON((fd < 0) || !zero_buf || (chunk_bytes % block_size));
	N_Tf(__AUTOID__, "@STR io[@CHAR] offset=@ZX[blk] len=@INT[blk]", dev->serial_number, 'Z', start_block, num_blocks);
	while (total_bytes > 0) {
		const size_t write_bytes = MIN(total_bytes, chunk_bytes);
		const ssize_t w = pwrite(fd, zero_buf, write_bytes, (off_t)offset);
		BUG_ON((w < 0) || ((size_t)w != write_bytes));
		offset += write_bytes;
		total_bytes -= write_bytes;
	}
	free(zero_buf);
	close(fd);
	return 0;
}

int sandbox_nvme_io_to_disk(const char* disk_id, size_t start_block, size_t num_bytes, void *data, bool is_read) {
	const struct sandbox_nvme_device *dev = sandbox_nvme_get_device_by_disk_id(disk_id);
	const struct sandbox_nvme_lbaf *lbaf = sandbox_nvme_get_lbaf(dev->current_format_idx);
	const off_t offset = (off_t)start_block * (1 << lbaf->block_size_exp);
	const int fd = sandbox_nvme_open(dev);
	ssize_t n_done_bytes = (is_read ? pread( fd, data, num_bytes, offset) :
									  pwrite(fd, data, num_bytes, offset));
	N_Tf(__AUTOID__, "@STR io[@CHAR] offset=@ZX[blk] len=@INT[blk], done=@INT[b]", dev->serial_number, (is_read ? 'R' : 'W'), start_block, (num_bytes >> lbaf->block_size_exp), (int)n_done_bytes);
	if ((n_done_bytes == 0) && is_read)
		n_done_bytes = num_bytes;			// Todo: Read beyond eof is considered success. Maybe change this decision
	close(fd);
	BUG_ON(n_done_bytes != (ssize_t)num_bytes);			// Do not allow IO failure for now
	return (n_done_bytes == (ssize_t)num_bytes) ? 0 : -1;
}

struct udev *udev_new(void) {		// Todo: This is udev simulator, unrelated to nvme, should be in os simulator
	int i;
	struct udev *u = (struct udev *)calloc(1, sizeof(*u));
	u->ref++;
	N_Tf(dfi1053, "udev_new");
	BUG_ON(ARRAY_SIZE(u->ent) != NVME_DEVICE_COUNT);
	for (i = 0; i < (int)NVME_DEVICE_COUNT; ++i) {
		u->ent[i].name = nvme_devices[i].device_path;
		u->ent[i].path = nvme_devices[i].device_name;
		if (i > 0)  u->ent[i - 1].next = &u->ent[i];		// Emulate linked list with our array
	}
	return u;
}

struct udev_device *udev_device_new_from_syspath(struct udev *u, const char *path) {
	struct udev_device *d = malloc(sizeof(*d));
	for (int i = 0; i < (int)NVME_DEVICE_COUNT; ++i) {
		if (!strcmp(nvme_devices[i].device_name, path)) {
			d->e = &u->ent[i];
			return d;
		}
	}
	BUG_ON(true); N_Ef(dsf3494, "no device found for path=@STR", path);
	return NULL;
}

#include "interfaces/nvme/nvmeibt_nvme_defines.h"
#include "../common/nvmeib_shared.h"
int nvme_ioctl_admin_cmd(const char *path, int fd, va_list ap) {
	const struct sandbox_nvme_device *nvme_dev = sandbox_nvme_get_device_by_path(path);
	struct nvme_admin_cmd *cmd =  va_arg(ap, struct nvme_admin_cmd*);
	if (!nvme_dev) {		// gpt_util tests use files. We presume the failure is because the device is not NVMe.
		errno = ENOTTY;
		N_Wf(sbioctnv1, "ioctl:nvme:admin: no device found for path=@STR", path);
		return -1;
	}
	N_Df(sbioctnv, "ioctl:nvme:admin opcode=@INT", cmd->opcode);
	if (cmd->opcode == nvme_admin_identify) {
		if (cmd->nsid == 0) {			// NSID 0 is special - controller identify command.
			struct nvme_id_ctrl *idctrl = (void*)cmd->addr;
			BUG_ON(cmd->data_len != sizeof(*idctrl));
			memset(idctrl, 0, cmd->data_len);
			idctrl->vid = nvme_dev->vendor_id;
			snprintf(idctrl->sn, sizeof(idctrl->sn), "%s", nvme_dev->serial_number);
			snprintf(idctrl->mn, sizeof(idctrl->mn), "%s", nvme_dev->model_number);
			snprintf(idctrl->fr, sizeof(idctrl->fr), "0.0.1");
			N_Tf(sbk3456, "ioctl:nvme:id controller fd=@INT reporting sn=@STR mn=@STR", fd, idctrl->sn, idctrl->mn);
		} else {						// NSID > 0 is the NVME storage namespace query.
			// Note that LBAF { ms, ds, rp } are defined in NVM-Express-NVM-Command-Set-Specification-Revision-1.2-2025.08.01
			// Figure 116: LBA Format Data Structure, NVM Command Set Specific (PDF p. 91).
			struct nvme_id_ns *response = (void*)cmd->addr;
			enum SANDBOX_NVME_FMT_e i;
			BUG_ON(cmd->data_len < sizeof(*response));
			memset(response, 0, cmd->data_len);

			// Populate supported LBA formats from the sandbox LBA format table
			response->nlbaf = SANDBOX_NVME_LBAF_COUNT - 1;  // Number of supported LBA formats minus 1
			for (i = 0; i < SANDBOX_NVME_LBAF_COUNT; i++) {
				const struct sandbox_nvme_lbaf *lbaf = sandbox_nvme_get_lbaf(i);
				response->lbaf[i].ds = lbaf->block_size_exp;
				response->lbaf[i].ms = lbaf->metadata_size;
			}
			response->flbas = nvme_dev->current_format_idx;					// Set current format based on device's format index
			response->mc = NVME_NS_MC_INLINE_MASK | NVME_NS_MC_SEP_MASK;	// Set metadata capabilities: both inline and separate metadata are supported by the device. Note: Toma will only use separate metadata (DISK_ALLOW_INLINE_MD == 0).
			response->nsze = nvme_dev->size_in_blocks;
			N_Tf(sbk5443, "ioctl:nvme:id storage ns=@INT fd=@INT flbas=@INT nlbaf=@INT mc=@INT nsze=@INT64_TD", cmd->nsid, fd, response->flbas, response->nlbaf, response->mc, response->nsze);
		}
	} else if (cmd->opcode == nvme_admin_get_log_page) {
		struct nvme_smart_log *fill =  (void*)cmd->addr;
		BUG_ON(cmd->data_len != sizeof(*fill));
		memset(fill, 0, cmd->data_len);										// Todo: do not support those counters yet
	}
	return 0;
}

int nvme_ioctl_get_size(const char *path, va_list ap) {
	const struct sandbox_nvme_device *nvme_dev = sandbox_nvme_get_device_by_path(path);
	int *block_size = va_arg(ap, int*);
	if (nvme_dev) {
		*block_size = (1 << sandbox_nvme_get_lbaf(nvme_dev->current_format_idx)->block_size_exp);
	} else {
		*block_size = 4096;  // gpt_util tests use files. Default for non-NVMe devices
	}
	return 0;
}

/************************************* disk image *****************************/
static int copy_between_fds(int srcfd, int dstfd) {
	char buf[4096];
	ssize_t n;
	while ((n = read(srcfd, buf, sizeof(buf))) > 0) {
		ssize_t written = 0;
		while (written < n) {
			ssize_t w = write(dstfd, buf + written, n - written);
			if (w < 0) {
				N_Ef(fsd3434, "write to fd=@INT failed: @STR\n", dstfd, strerror(errno));
				return 1;
			}
			written += w;
		}
	}

	if (n < 0) {
		N_Ef(fsd5532, "read from fd=@INT failed: @STR\n", srcfd, strerror(errno));
		return 1;
	}
	return 0;
}

static int copy_file(const char *source_path, const char *dest_path) {
	const int srcfd = open(source_path, O_RDONLY);
	const int dstfd = open(dest_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	int err;
	BUG_ON((srcfd < 0) || (dstfd < 0));
	err = copy_between_fds(srcfd, dstfd);
	close(srcfd);
	close(dstfd);
	return err;
}

static void disk_init(const char *dest_path, const char *src_path) {		//Create a disk image at the given path. We can do this a couple different ways: either generate it programmatically on the fly, or use some predefined example disk image.
	// const off_t size_bytes = 2000 * 4096;
	N_Tf(lkg3946, "creating block device=@STR", dest_path);
	unlink(dest_path);				// Copy the template disk image.
	copy_file(src_path, dest_path);	// This .img file is generated by Wentao's GPT util for test purposes. It is 2000 blocks of 4k, so 8 MB, and we probably don't want to commit it directly to Git, especially since it's mostly zero bytes.
	// TODO modify the serial number in the GPT metadata so each disk is unique?
	// **Alternative implementation**: If we want to build the GPT on the fly using our GPT writing code, call sandbox_nvme_format_disk()
}
