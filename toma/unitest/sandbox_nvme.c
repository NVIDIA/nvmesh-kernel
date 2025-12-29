#define TOMA_SANDBOX_BYPASS_REDIRECTS // allow calling real OS I/O functions from this module

#include "sandbox_nvme.h"
#include "nvmeibt_debug.h"
#include "toma_in_sandbox.h"

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

// Forward declarations

static int disk_init(const char *dest_path, const char *src_path);
static int copy_file(const char *source_path, const char *dest_path);
static int copy_between_fds(int srcfd, int dstfd);

// Constants

// Where the build system puts the uncompressed disk image templates.
#define TEST_DATA_BUILD_DIR "99bin/testdata/"

static const char stock_disk_template_file_path[] = TEST_DATA_BUILD_DIR "disk_stock.img";
static const char local_disk_template_file_path[] = TEST_DATA_BUILD_DIR "disk_nvmesh.img";

#define TARGET_DEVICES_FILE TOMA_ROOT_DIR "var/opt/nvmesh/.target_devices"
#define DISKS_CSV_FILE TOMA_ROOT_DIR "proc/nvmeibs/disks.csv"						// Emulates the work of kernel server.

// Location of the virtual /dev directory. We'll create it, and create files in it, at runtime.
#define SANDBOX_DEV_DIR TOMA_ROOT_DIR "dev/"

static struct sandbox_nvme_device nvme_devices[] = {
	{ 0x1401, "STKD_SN_001", "STKD_MN_001", "nvme0n1", SANDBOX_DEV_DIR "nvme0n1", true, 2048 },
	{ 0x1402, "NVMD_SN_002", "NVMD_NN_002", "nvme1001n1", SANDBOX_DEV_DIR "nvme1001n1", false, 2000 },
	{ 0x1403, "NVMD_SN_003", "NVMD_NN_003", "nvme1002n1", SANDBOX_DEV_DIR "nvme1002n1", false, 2000 },
};

#define NVME_DEVICE_COUNT (sizeof(nvme_devices) / sizeof(nvme_devices[0]))

static void mkdir_if_not_exists(const char *path);
static void write_file(const char *path, const char *content);

/// Set up the NVMe disk data. Currently just a static configuration,
/// but ultimately we'll add dynamic modification adding and removing disks.
void sandbox_nvme_init(void)
{
	N_Tf(sbu3401, "initializing static simulated NVMe disks");
	mkdir_if_not_exists(SANDBOX_DEV_DIR);

	// Create mock NVMe block device files.
	for (int i = 0; i < (int)NVME_DEVICE_COUNT; ++i) {
		struct sandbox_nvme_device *d = &nvme_devices[i];
		const char *template_src = d->stock_disk ? stock_disk_template_file_path :
							   local_disk_template_file_path;
		if (disk_init(d->device_path, template_src) != 0) {
			N_Ef(kdj3994, "failed to initialize disk @INT", i);
		}
	}

	// Create the parent directories if needed.
	mkdir_if_not_exists(TOMA_ROOT_DIR "proc");
	mkdir_if_not_exists(TOMA_ROOT_DIR "proc/nvmeibs");
	mkdir_if_not_exists(TOMA_ROOT_DIR "var");
	mkdir_if_not_exists(TOMA_ROOT_DIR "var/opt");
	mkdir_if_not_exists(TOMA_ROOT_DIR "var/opt/nvmesh");

	// Generate the disk data files.
	write_file(TARGET_DEVICES_FILE, "nvme,STKD_SN_001,5121,STKD_MN_001,1\n");
	write_file(DISKS_CSV_FILE, "id,blocks,block_size,max_request_size,seq,nsid,dev_name,metadata,status,vendor\n"
				   "NVMD_SN_002.1,2000,4096,32,1,1,/dev/nvme1001n1,8,Ok,5122\n"
				   "NVMD_SN_003.1,2000,4096,32,0,1,/dev/nvme1002n1,8,Ok,5123\n");
	N_Tf(sbu3402, "done initializing NVMe disks");
}

static void mkdir_if_not_exists(const char *path)
{
	if (mkdir(path, 0755) != 0) {
		if (errno != EEXIST) {
			N_Ef(gjl3965, "mkdir failed for path=@STR error=@STR", path, strerror(errno));
		}
	}
}

static void write_file(const char *path, const char *content)
{
	FILE *fp = fopen(path, "w");
	if (!fp) {
		N_Ef(fsd3964, "failed to open file path=@STR error=@STR", path, strerror(errno));
		return;
	}
	if (fputs(content, fp) == EOF) {
		N_Ef(fsd3965, "write error path=@STR error=@STR", path, strerror(errno));
	} else {
		N_Tf(fsd5640, "wrote path=@STR", path);
	}

	if (fclose(fp) != 0) {
		N_Ef(fsd5441, "failed to close file path=@STR error=@STR", path, strerror(errno));
	}
}

/// Given the device node path (e.g. `_root/dev/nvme0n1`), get the sandbox device definition struct, or NULL if none.
struct sandbox_nvme_device *sandbox_nvme_get_device_by_path(const char *path)
{
	int i;
	for (i = 0; i < (int)NVME_DEVICE_COUNT; ++i) {
		struct sandbox_nvme_device *d = &nvme_devices[i];
		if (!strcmp(d->device_path, path)) {
			N_Tf(kdj3946, "found device for path=@STR", path);
			return d;
		}
	}

	N_Tf(uti3345, "no device for path=@STR", path);
	return NULL;
}

struct udev *udev_new(void)
{
	int i;
	struct udev *u = (struct udev *)calloc(1, sizeof(*u));
	u->ref++;

	N_Tf(dfi1053, "udev_new");

	assert(sizeof(u->ent) / sizeof(u->ent[0]) >= NVME_DEVICE_COUNT);

	for (i = 0; i < (int)NVME_DEVICE_COUNT; ++i) {
		u->ent[i].name = nvme_devices[i].device_path;
		u->ent[i].path = nvme_devices[i].device_name;
		if (i > 0) {
			u->ent[i - 1].next = &u->ent[i];
		}
	}

	return u;
}

struct udev_device *udev_device_new_from_syspath(struct udev *u, const char *path)
{
	struct udev_device *d = malloc(sizeof(*d));
	int i;

	for (i = 0; i < (int)NVME_DEVICE_COUNT; ++i) {
		if (!strcmp(nvme_devices[i].device_name, path)) {
			d->e = &u->ent[i];
			return d;
		}
	}

	N_Ef(dsf3494, "no device found for path=@STR", path);
	return NULL;
}

/************************************* disk image *****************************/

/**
 * Create a disk image at the given path.
 * We can do this a couple different ways: either generate it programmatically on the fly,
 * or use some predefined example disk image.
 */
static int disk_init(const char *dest_path, const char *src_path)
{
	// const off_t sector_size = 4096;
	// const off_t size_bytes = 2000 * sector_size;
	// int templatefd;
	// int fd;

	N_Tf(lkg3946, "creating block device=@STR", dest_path);

	// **Initial implementation**
	// Copy the template disk image.
	unlink(dest_path);
	// This .img file is generated by Wentao's GPT util for test purposes.
	// It is 2000 blocks of 4k, so 8 MB, and we probably don't want to commit it directly to Git,
	// especially since it's mostly zero bytes.
	copy_file(src_path, dest_path);
	// TODO modify the serial number in the GPT metadata so each disk is unique?

	// **Alternative implementation**
	// If we want to build the GPT on the fly using our GPT writing code.
	// Create a sparse file for the disk image
	// unlink(path);
	// fd = open(path, O_CREAT | O_WRONLY, 0644);
	// if (fd < 0) {
	//     SANDBOX_PRINT("ERROR: open disk image %s failed: %s", path, strerror(errno));
	//     return 1;
	// }
	// if (ftruncate(fd, size_bytes) != 0) {
	//     SANDBOX_PRINT("ERROR: ftruncate disk image %s to %jd failed: %s", path, (intmax_t) size_bytes, strerror(errno));
	//     close(fd);
	//     return 1;
	// }
	// close(fd);
	return 0;
}

static int copy_file(const char *source_path, const char *dest_path)
{
	int srcfd;
	int dstfd;
	int err;

	srcfd = open(source_path, O_RDONLY);
	if (srcfd < 0) {
		N_Ef(fgg3345, "open source file failed name=@STR err=@STR\n", source_path, strerror(errno));
		return 1;
	}

	dstfd = open(dest_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (dstfd < 0) {
		N_Ef(fggo3978, "open destination file failed name=@STR err=@STR\n", dest_path, strerror(errno));
		close(srcfd);
		return 1;
	}

	err = copy_between_fds(srcfd, dstfd);

	close(srcfd);
	close(dstfd);
	return err;
}

static int copy_between_fds(int srcfd, int dstfd)
{
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
