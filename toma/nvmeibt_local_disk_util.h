#ifndef NVMEIBT_LOCAL_DISK_UTIL
#define NVMEIBT_LOCAL_DISK_UTIL

#include "nvmeibt_common.h"

struct nvmeibt_local_disk_util_smart_info {
	char Pci_Address[32];
	char diskID[ASCII_UUID_MAX_STR_LEN];
	unsigned long long blocks;
	unsigned int block_size;
	unsigned int metadata_size;
	unsigned int metadata_cap;	// 0x2 if separate buffer, 0x1 if inline buffer, 0x3 if both are supported
	char Serial_Number[24];
	char Model[NVMEIB_DISK_MAX_MODEL_STR_SIZE+1];
	unsigned short Vendor;
	unsigned int Submission_Queues;
	unsigned int Completion_Queues;
	unsigned int MSIX_Interrupts;
	unsigned int Numa_Node;
	unsigned short Critical_Warning;
	unsigned int Available_Spare;
	unsigned int Available_Spare_Threshold;
	unsigned int Percentage_Used;
	unsigned long long Host_Write_Commands;
	unsigned int Controller_Busy_Time;
	unsigned int Power_Cycles;
	unsigned int Power_On_Hours;
	unsigned int Unsafe_Shutdowns;
	unsigned int Media_Errors;
	unsigned long long Number_of_Error_Information_Log_Entries;
	unsigned int Namespace_Id;
	char format_options[256];
	unsigned int format_version;
	char time[128];
};

struct nvme_smart_log;
struct nvme_dev_info;
struct nvmeibt_local_disk_config;

void nvmeibt_local_disk_util_extract_stripped_dev_name_from_dev_file_name(char *out_stripped_dev_name, char *in_dev_file_name);
int nvmeibt_local_disk_util_get_nvme_smart_log(int fd, struct nvme_smart_log *smart_log);
int nvmeibt_local_disk_util_get_nvme_dev_info(struct nvme_dev_info *nvme_dev, const char *dev_file_name, int fd);
int nvmeibt_local_disk_util_nvme_identify_ns(int fd, __u32 nsid, BOOL present, void *data);
int nvmeibt_local_disk_util_nvme_get_nsid(int fd);
const char* nvmeibt_local_disk_util_nvme_status_to_string(uint32_t status);
int nvmeibt_local_disk_util_nvme_io(int fd, __u8 opcode, __u64 slba, __u16 nblocks, __u16 control,
                                    __u32 dsmgmt, __u32 reftag, __u16 apptag, __u16 appmask, void *data, void *metadata);
int nvmeibt_local_disk_util_write_zeroes(int fd, __u64 pba_s, __u64 n_pblk, unsigned int pblksize);
int nvmeibt_local_disk_util_read_smart_info(int seq, struct nvmeibt_local_disk_util_smart_info *smart_info, int disk_fd);
enum nvmeibt_disk_type nvmeibt_local_disk_get_stock_disk_type_by_dev_file_name(const char *dev_file_name, const char *udev_devpath);
bool nvmeibt_local_disk_is_nvmesh_dev_name(const char *dev_path, int *output_dev_idx_in_dev_name);
BOOL nvmeibt_local_disk_util_fill_local_disk_devinfo_and_smart_from_nvme_driver(struct nvmeibt_local_disk_config *from_config, const char *dev_file_name, int fd);
void nvmeibt_local_disk_util_set_attention_LED(const char *pcie_slot, int value);
BOOL nvmeibt_local_disk_util_fill_local_disk_devinfo_and_smart_from_udev(struct nvmeibt_local_disk_config *from_config, const char *path, int fd);
BOOL nvmeibt_local_disk_util_fill_devinfo_for_vdisk(struct nvmeibt_local_disk_config *from_config, const char *path, int fd);
#endif // #ifndef NVMEIBT_LOCAL_DISK_UTIL

