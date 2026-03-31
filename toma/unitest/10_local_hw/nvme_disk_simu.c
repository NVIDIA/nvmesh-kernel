/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#define TOMA_SANDBOX_BYPASS_REDIRECTS // allow calling real OS I/O functions from this module - must be defined before any other includes
#include "nvmeibt_debug.h"
#include "nvme_disk_simu.h"
#include "../toma_in_sandbox.h"
#include "../13_mgmt/mongodb_simu.h"		// Disk conf
#include <sys/stat.h>

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

uint64_t sandbox_nvme_get_n_blocks(const struct sandbox_nvme_device *dev) {
	const unsigned block_size = 1U << sandbox_nvme_get_lbaf(dev->current_format_idx)->block_size_exp;
	BUG_ON(dev->size_in_bytes % block_size != 0);
	return dev->size_in_bytes / block_size;
}

/* Disk size requirement: https://github.com/NVIDIA/nvmesh-documentation/blob/3.4.0-rc1/NVMesh%203.4.0%20User%20Guide.md#4k8-formatting
 * Relevant constants (redefined for sandbox): METADATA_PARTITION_RATIO, journal_data_size_in_pblks, serjio_db_size_in_pblks
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
	{ NULL, {NULL, 0}, 0x1402, NULL, "DISK_MOD_01", "nvme1001n1", SANDBOX_DEV_DIR "nvme1001n1", false, (32768ULL << 12), SANDBOX_NVME_FMT_4096_0 },	/* 128MB */
	{ NULL, {NULL, 0}, 0x1403, NULL, "DISK_MOD_02", "nvme1002n1", SANDBOX_DEV_DIR "nvme1002n1", false, (32768ULL << 12), SANDBOX_NVME_FMT_4096_0 },	/* 128MB */
	{ NULL, {NULL, 0}, 0x1401, NULL, "DISK_MOD_03", "nvme" "0n1", SANDBOX_DEV_DIR "nvme0" "n1", true,  0,                SANDBOX_NVME_FMT_4096_0 },	/* size derived from stock image at init */
};

#define NVME_DEVICE_COUNT ARRAY_SIZE(nvme_devices)

static void write_file(const char *path, const char *content) {
	FILE *fp = fopen(path, "w");
	BUG_ON(!fp);
	BUG_ON(fputs(content, fp) == EOF);
	BUG_ON(fclose(fp) != 0);
}

static void create_simulated_locks_file(const struct sandbox_nvme_device *d) {
	char locks_path[256];	// Locks file is named after the disk_id (serial.nsid). Our sandbox disks use nsid=1.
	const int n = snprintf(locks_path, sizeof(locks_path), TOMA_ROOT_DIR "proc/nvmeibs/locks.%s.%d", d->serial_number, 1 + 0 * d->conf->name_space_id);	// Todo: Create locks at correct time: name space became 1
	int fd = open(locks_path, O_CREAT | O_RDWR, 0644);
	BUG_ON((n < 0) || (n >= (int)sizeof(locks_path)) || (fd < 0));
	BUG_ON(ftruncate(fd, 4096) < 0);		// Extend to one page size for mmap.
	BUG_ON(close(fd) != 0);
	N_Tf(sbu3403, "created locks file @STR", locks_path);
}

// Set up the NVMe disk data. Currently just a static configuration, but ultimately we'll add dynamic modification adding and removing disks.
static void disk_init_stock(struct sandbox_nvme_device *dev);
static void disk_init_zeroed(const struct sandbox_nvme_device *dev);
void sandbox_nvme_init(void) {
	const struct sb_disk_conf *mongodb_disks = sb_cluster_get_const_conf()->live->disks;
	N_Tf(sbu3401, "initializing static simulated NVMe disks");
	for (int i = 0; i < (int)NVME_DEVICE_COUNT; ++i) {	// sandbox_nvme_get_device_count
		struct sandbox_nvme_device *d = &nvme_devices[i];
		d->conf = &mongodb_disks[i];
		d->serial_number = d->conf->serial;
		((struct sb_disk_conf *)d->conf)->local_nvme = d;
		if (d->stock_disk) {
			disk_init_stock(d);
		} else {
			disk_init_zeroed(d);
			create_simulated_locks_file(d);
		}
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

struct sandbox_nvme_device *sandbox_nvme_get_device_by_disk_id(const char *disk_id) {
	const char *dot = strchr(disk_id, '.');		// disk_id format is "SERIAL.NSID", We need to match the serial number portion
	const int serial_len = (dot ? (int)(dot - disk_id) : (int)strnlen(disk_id, 32));
	for (int i = 0; i < (int)NVME_DEVICE_COUNT; ++i) {
		struct sandbox_nvme_device *d = &nvme_devices[i];
		if (!strncmp(d->serial_number, disk_id, serial_len))
			return d;
	}
	BUG_ON(true); return NULL;
}

const struct sandbox_nvme_device *sandbox_nvme_get_device_by_full_path(const char *path) {
	for (int i = 0; i < (int)NVME_DEVICE_COUNT; ++i) {
		const struct sandbox_nvme_device *d = &nvme_devices[i];
		if (!strncmp(d->device_path, path, 128))
			return d;
	}
	BUG_ON(true); return NULL;
}

struct sandbox_nvme_device *sandbox_nvme_get_device_by_ram_mmap(const void *addr) {
	BUG_ON(addr == NULL);
	for (int i = 0; i < (int)NVME_DEVICE_COUNT; ++i) {
		struct sandbox_nvme_device *d = &nvme_devices[i];
		if (d->ram.addr == addr)
			return d;
	}
	return NULL;
}

static int sandbox_nvme_open(const struct sandbox_nvme_device *dev) { return open(dev->device_path, O_RDWR); }

int sandbox_nvme_format_disk(const char* disk_id, enum SANDBOX_NVME_FMT_e fmt_idx) {
	struct sandbox_nvme_device *dev = sandbox_nvme_get_device_by_disk_id(disk_id);	// Mutable
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
	} else if (ftruncate(fd, (off_t)dev->size_in_bytes) != 0) {
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
	size_t total_bytes = num_blocks  * block_size;
	size_t offset =      start_block * block_size;
	const int fd = sandbox_nvme_open(dev);
	BUG_ON(fd < 0);
	N_Tf(__AUTOID__, "@STR io[@CHAR] offset=@ZX[blk] len=@INT[blk]", dev->serial_number, 'Z', start_block, num_blocks);
	BUG_ON(fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, (off_t)offset, (off_t)total_bytes) != 0);
	close(fd);
	return 0;
}

int sandbox_nvme_io_to_disk(const char* disk_id, size_t start_block, size_t num_bytes, void *data, bool is_read) {
	const struct sandbox_nvme_device *dev = sandbox_nvme_get_device_by_disk_id(disk_id);
	const struct sandbox_nvme_lbaf *lbaf = sandbox_nvme_get_lbaf(dev->current_format_idx);
	const off_t offset = (off_t)start_block * (1 << lbaf->block_size_exp);
	const int fd = sandbox_nvme_open(dev);
	ssize_t n_done_bytes;
	BUG_ON((uint64_t)(offset + num_bytes) > dev->size_in_bytes);
	n_done_bytes = (is_read ? pread( fd, data, num_bytes, offset) :
								  pwrite(fd, data, num_bytes, offset));
	N_Tf(__AUTOID__, "@STR io[@CHAR] offset=@ZX[blk] len=@INT[blk], done=@INT[b]", dev->serial_number, (is_read ? 'R' : 'W'), start_block, (num_bytes >> lbaf->block_size_exp), (int)n_done_bytes);
	close(fd);
	BUG_ON(n_done_bytes != (ssize_t)num_bytes);
	return (n_done_bytes == (ssize_t)num_bytes) ? 0 : -1;
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
			response->nsze = sandbox_nvme_get_n_blocks(nvme_dev);
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

static void disk_init_stock(struct sandbox_nvme_device *dev) {
	const char* src_path = "99bin/testdata/disk_stock.img";
	struct stat st;
	unlink(dev->device_path);
	copy_file(src_path, dev->device_path);
	BUG_ON(stat(dev->device_path, &st) != 0);
	dev->size_in_bytes = (uint64_t)st.st_size;
	N_Tf(lkg3946, "creating sandbox stock disk device=@STR from=@STR size=@INT64_TD", dev->device_path, src_path, (int64_t)dev->size_in_bytes);
	BUG_ON(dev->size_in_bytes == 0);
	BUG_ON(dev->size_in_bytes % (1U << sandbox_nvme_get_lbaf(dev->current_format_idx)->block_size_exp) != 0);
}

static void disk_init_zeroed(const struct sandbox_nvme_device *dev) {
	const off_t size_bytes = (off_t)dev->size_in_bytes;
	int fd;

	BUG_ON(!dev);
	N_Tf(lkg3947, "creating zeroed sparse block device=@STR size=@INT64_TD", dev->device_path, (int64_t)size_bytes);
	unlink(dev->device_path);
	fd = open(dev->device_path, O_CREAT | O_RDWR, 0644);
	BUG_ON(fd < 0);
	BUG_ON(ftruncate(fd, size_bytes) != 0);
	BUG_ON(close(fd) != 0);
}
