#include "nvmeibt_debug.h"
#include "interfaces/nvme/nvmeibt_nvme_defines.h"
#include "nvmeibt_local_disk_util.h"
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <dirent.h>
#include <unistd.h>
#include <ctype.h>
#include "nvmeibt_local_disk.h"
#include <fcntl.h>
#include <linux/fs.h>

#define MAX_BLOCKS_PER_ZERO_ITTERATION		65535

void nvmeibt_local_disk_util_extract_stripped_dev_name_from_dev_file_name(char *out_stripped_dev_name, char *in_dev_file_name)
{
	char		*ch;
	int			n_digits_after_nvme = 0;
	int			n_digits_after_n = 0;
	int			n_digits_after_p = 0;

	NFIN;
	if (!in_dev_file_name) {
		N_Tf(9abhfy3, "Empty dev_file_name");
		goto out_err;
	}
	// Look for "/dev/nvme<digits>
	if (strncmp(in_dev_file_name, nvme_dev_name_prefix, sizeof(nvme_dev_name_prefix) - 1)) {
		N_Tf(aw1nizh, "Not a nvme dev_file_name=@STR", in_dev_file_name);
		goto out_err;
	}
	ch = in_dev_file_name + strlen(nvme_dev_name_prefix);
	while (*ch >= '0' && *ch <= '9') {
		n_digits_after_nvme++;
		ch++;
	}
	if (n_digits_after_nvme == 0) {
		goto out_err;
	}
	nvmeibt_strlcpy(out_stripped_dev_name, in_dev_file_name + strlen(dev_dir), strlen(nvme_dev_name) + n_digits_after_nvme + 1);
	if (*ch == '\0') {
		goto out_OK;
	} else if (*ch != 'n') {
		goto out_err;
	}
	// Now "n<digits>"
	ch++;
	while (*ch >= '0' && *ch <= '9') {
		n_digits_after_n++;
		ch++;
	}
	if (n_digits_after_n == 0) {
		goto out_err;
	}
	if (*ch == '\0') {
		goto out_OK;
	} else if (*ch != 'p') {
		goto out_err;
	}
	// Now "p<digits>"
	ch++;
	while (*ch >= '0' && *ch <= '9') {
		n_digits_after_p++;
		ch++;
	}
	if (n_digits_after_p == 0) {
		goto out_err;
	}
	if (*ch == '\0') {
		goto out_OK;
	}
out_err:
	nvmeibt_strlcpy(out_stripped_dev_name, "", 1);
out_OK:
	N_Tf(rvhjsuq, "dev_file_name=@STR out_stripped_dev_name=@STR", in_dev_file_name, out_stripped_dev_name);
}

int nvmeibt_local_disk_util_nvme_get_nsid(int fd)
{
	struct stat nvme_stat;
	int rv = 0;

	if (fstat(fd, &nvme_stat) < 0) {
		N_Wf(warn_local_disk_util_nvmeibt_local_disk_util_nvme_get_nsid, "fstat error for fd=@FD", fd);
		rv = -1;
		goto out;
	}

	if (!is_block_device_stat(nvme_stat)) {
		N_Ef(error_local_disk_util_nvmeibt_local_disk_util_nvme_get_nsid, "Error requesting namespace-id from non-block device");
		rv = -1;
		goto out;
	}
	rv = ioctl(fd, NVME_IOCTL_ID);
out:
	return rv;
}


static int nvme_identify(int fd, __u32 nsid, __u32 cdw10, void *data)
{
        struct nvme_admin_cmd cmd = {
                .opcode         = nvme_admin_identify,
                .nsid           = nsid,
                .addr           = (__u64)(uintptr_t) data,
                .data_len       = 0x1000,
                .cdw10          = cdw10,
        };

        return ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd);
}

static int nvme_identify_ctrl(int fd, void *data)
{
	return nvme_identify(fd, 0, 1, data);
}

int nvmeibt_local_disk_util_nvme_identify_ns(int fd, __u32 nsid, BOOL present, void *data)
{
	int cns = present ? NVME_ID_CNS_NS_PRESENT : NVME_ID_CNS_NS;
	return nvme_identify(fd, nsid, cns, data);
}

static int nvme_get_log13(int fd, __u32 nsid, __u8 log_id, __u8 lsp, __u64 lpo,
                 __u16 lsi, __u32 data_len, void *data)
{
	struct nvme_admin_cmd cmd = {
			.opcode         = nvme_admin_get_log_page,
			.nsid           = nsid,
			.addr           = (__u64)(uintptr_t) data,
			.data_len       = data_len,
	};
	__u32 numd = (data_len >> 2) - 1;
    __u16 numdu = numd >> 16, numdl = numd & 0xffff;

	cmd.cdw10 = log_id | (numdl << 16);
    if (lsp) {
			cmd.cdw10 |= lsp << 8;
	}

	cmd.cdw11 = numdu | (lsi << 16);
	cmd.cdw12 = lpo;
	cmd.cdw13 = (lpo >> 32);

	return ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd);
}

static int nvme_get_log(int fd, __u32 nsid, __u8 log_id, __u32 data_len, void *data)
{
        return nvme_get_log13(fd, nsid, log_id, NVME_NO_LOG_LSP, NVME_NO_LOG_LPO,
                              0, data_len, data);
}

int nvmeibt_local_disk_util_get_nvme_smart_log(int fd, struct nvme_smart_log *smart_log)
{
	int rv = 0;
	int nsid = 0xffffffff;

	NFIN;

	rv = nvme_get_log(fd, nsid, NVME_LOG_SMART, sizeof(*smart_log), smart_log);
	N_Tf(trace_local_disk_util_nvmeibt_local_disk_util_get_nvme_smart_log, "rv = @RV", rv);
	if (rv != 0) {
		N_Wf(warn_local_disk_util_nvmeibt_local_disk_util_get_nvme_smart_log, "smart log returned error=@AUTO_ERRNO");
	}

	NFOUT;
    return rv;
}

int nvmeibt_local_disk_util_get_nvme_dev_info(struct nvme_dev_info *nvme_dev, const char *dev_file_name, int fd)
{
	int 	rv = 0;

	NFIN;

	if (nvme_identify_ctrl(fd, &nvme_dev->ctrl) != 0) {
		N_Ef(error_local_disk_util_nvmeibt_local_disk_util_get_nvme_dev_info, "could not identify controller for path=@PATH", dev_file_name);
		rv = -1;
		goto out;
	}

	if ((nvme_dev->nsid = nvmeibt_local_disk_util_nvme_get_nsid(fd)) < 0) {
		N_Ef(error_1_local_disk_util_nvmeibt_local_disk_util_get_nvme_dev_info, "could not get namespace id for path=@PATH", dev_file_name);
		rv = -1;
		goto out;
	}

	if (nvmeibt_local_disk_util_nvme_identify_ns(fd, nvme_dev->nsid, 0, &nvme_dev->ns) != 0) {
		N_Ef(error_2_local_disk_util_nvmeibt_local_disk_util_get_nvme_dev_info, "could not identify namespace for nsid=@NSID at path=@PATH", nvme_dev->nsid, dev_file_name);
		rv = -1;
		goto out;
	}

	nvmeibt_strlcpy(nvme_dev->path, dev_file_name, sizeof(nvme_dev->path));

out:
	NFOUT;
    return rv;
}

int nvmeibt_local_disk_util_nvme_io(int fd, __u8 opcode, __u64 slba, __u16 nblocks, __u16 control,
            __u32 dsmgmt, __u32 reftag, __u16 apptag, __u16 appmask, void *data,
            void *metadata)
{
	struct nvme_user_io io = {
			.opcode         = opcode,
			.flags          = 0,
			.control        = control,
			.nblocks        = nblocks,
			.rsvd           = 0,
			.metadata       = (__u64)(uintptr_t) metadata,
			.addr           = (__u64)(uintptr_t) data,
			.slba           = slba,
			.dsmgmt         = dsmgmt,
			.reftag         = reftag,
			.appmask        = appmask,
			.apptag         = apptag,
	};
	N_Tf(trace_local_disk_util_nvmeibt_local_disk_util_nvme_io, "opcode=@OPCODE flags=@FLAGS_INT control=@CONTROL nblocks=@NBLOCKS slba=@BLOCK_LLONG dsmgmt=@DSMGMT reftag=@REFTAG appmask=@APPMASK apptag=@APPTAG data=@DATA metadata=@METADATA",
		io.opcode, io.flags, io.control, io.nblocks, io.slba, io.dsmgmt, io.reftag, io.appmask, io.apptag, data, metadata);
	return ioctl(fd, NVME_IOCTL_SUBMIT_IO, &io);
}

int nvmeibt_local_disk_util_write_zeroes(int fd, __u64 pba_s, __u64 n_pblk, unsigned int pblk_size)
{
	__u16						control = 0;
	__u32						nsid = nvmeibt_local_disk_util_nvme_get_nsid(fd);
	__u64						n_pblks_for_iteration;
	int							rv = 0;
	struct nvme_passthru_cmd	cmd;

	NFIN;

	if ((int) nsid < 0) {
		N_Wf(warn_local_disk_util_nvmeibt_local_disk_util_write_zeroes, "could not get namespace id for fd=@FD", fd);
		rv = -1;
		goto out;
	}
	while (n_pblk > 0) {
		n_pblks_for_iteration = min(n_pblk, (__u64)MAX_BLOCKS_PER_ZERO_ITTERATION);
		n_pblks_for_iteration = nvmeibt_align_n_blks_to_write_to_blkset(pba_s, n_pblks_for_iteration, pblk_size);

		memset(&cmd, 0, sizeof(cmd));
		cmd.opcode         = 0x08; /*nvme_cmd_write_zeroes*/
		cmd.nsid           = nsid;
		cmd.cdw10          = pba_s & 0xffffffff;
		cmd.cdw11          = pba_s >> 32;
		cmd.cdw12          = ((__u16)n_pblks_for_iteration) | (control << 16);
		cmd.cdw14          = 0;
		cmd.cdw15          = 0;

		N_Tf(trace_local_disk_util_nvmeibt_local_disk_util_write_zeroes, "Calling IOCTL fd=@FD start=@START n_pblks=@N_PBLKS", fd, pba_s, n_pblks_for_iteration);
		rv = ioctl(fd, NVME_IOCTL_IO_CMD, &cmd);
		N_Tf(trace_1_local_disk_util_nvmeibt_local_disk_util_write_zeroes, "IOCTL done");
		if	(rv == -1) {
			break;
		}
		if (rv > 0) {
			N_Tf(error_local_disk_util_nvmeibt_local_disk_util_write_zeroes, "IOCTL returned positive @RV @AUTO_ERRNO. Considering it an error", rv);
			rv = -1;
			break;
		}
		n_pblk -= n_pblks_for_iteration;
		pba_s += n_pblks_for_iteration;
    }

out:
    NFOUT;
    return rv;
}


const char *nvmeibt_local_disk_util_nvme_status_to_string(__u32 status)
{
        switch (status & 0x3ff) {
        case NVME_SC_SUCCESS:                   return "SUCCESS";
        case NVME_SC_INVALID_OPCODE:            return "INVALID_OPCODE";
        case NVME_SC_INVALID_FIELD:             return "INVALID_FIELD";
        case NVME_SC_CMDID_CONFLICT:            return "CMDID_CONFLICT";
        case NVME_SC_DATA_XFER_ERROR:           return "DATA_XFER_ERROR";
        case NVME_SC_POWER_LOSS:                return "POWER_LOSS";
        case NVME_SC_INTERNAL:                  return "INTERNAL";
        case NVME_SC_ABORT_REQ:                 return "ABORT_REQ";
        case NVME_SC_ABORT_QUEUE:               return "ABORT_QUEUE";
        case NVME_SC_FUSED_FAIL:                return "FUSED_FAIL";
        case NVME_SC_FUSED_MISSING:             return "FUSED_MISSING";
        case NVME_SC_INVALID_NS:                return "INVALID_NS";
        case NVME_SC_CMD_SEQ_ERROR:             return "CMD_SEQ_ERROR";
        case NVME_SC_SANITIZE_FAILED:           return "SANITIZE_FAILED";
        case NVME_SC_SANITIZE_IN_PROGRESS:      return "SANITIZE_IN_PROGRESS";
        case NVME_SC_LBA_RANGE:                 return "LBA_RANGE";
        case NVME_SC_CAP_EXCEEDED:              return "CAP_EXCEEDED";
        case NVME_SC_NS_NOT_READY:              return "NS_NOT_READY";
        case NVME_SC_RESERVATION_CONFLICT:      return "RESERVATION_CONFLICT";
        case NVME_SC_CQ_INVALID:                return "CQ_INVALID";
        case NVME_SC_QID_INVALID:               return "QID_INVALID";
        case NVME_SC_QUEUE_SIZE:                return "QUEUE_SIZE";
        case NVME_SC_ABORT_LIMIT:               return "ABORT_LIMIT";
        case NVME_SC_ABORT_MISSING:             return "ABORT_MISSING";
        case NVME_SC_ASYNC_LIMIT:               return "ASYNC_LIMIT";
        case NVME_SC_FIRMWARE_SLOT:             return "FIRMWARE_SLOT";
        case NVME_SC_FIRMWARE_IMAGE:            return "FIRMWARE_IMAGE";
        case NVME_SC_INVALID_VECTOR:            return "INVALID_VECTOR";
        case NVME_SC_INVALID_LOG_PAGE:          return "INVALID_LOG_PAGE";
        case NVME_SC_INVALID_FORMAT:            return "INVALID_FORMAT";
        case NVME_SC_FW_NEEDS_CONV_RESET:       return "FW_NEEDS_CONVENTIONAL_RESET";
        case NVME_SC_INVALID_QUEUE:             return "INVALID_QUEUE";
        case NVME_SC_FEATURE_NOT_SAVEABLE:      return "FEATURE_NOT_SAVEABLE";
        case NVME_SC_FEATURE_NOT_CHANGEABLE:    return "FEATURE_NOT_CHANGEABLE";
        case NVME_SC_FEATURE_NOT_PER_NS:        return "FEATURE_NOT_PER_NS";
        case NVME_SC_FW_NEEDS_SUBSYS_RESET:     return "FW_NEEDS_SUBSYSTEM_RESET";
        case NVME_SC_FW_NEEDS_RESET:            return "FW_NEEDS_RESET";
        case NVME_SC_FW_NEEDS_MAX_TIME:         return "FW_NEEDS_MAX_TIME_VIOLATION";
        case NVME_SC_FW_ACIVATE_PROHIBITED:     return "FW_ACTIVATION_PROHIBITED";
        case NVME_SC_OVERLAPPING_RANGE:         return "OVERLAPPING_RANGE";
        case NVME_SC_NS_INSUFFICENT_CAP:        return "NS_INSUFFICIENT_CAPACITY";
        case NVME_SC_NS_ID_UNAVAILABLE:         return "NS_ID_UNAVAILABLE";
        case NVME_SC_NS_ALREADY_ATTACHED:       return "NS_ALREADY_ATTACHED";
        case NVME_SC_NS_IS_PRIVATE:             return "NS_IS_PRIVATE";
        case NVME_SC_NS_NOT_ATTACHED:           return "NS_NOT_ATTACHED";
        case NVME_SC_THIN_PROV_NOT_SUPP:        return "THIN_PROVISIONING_NOT_SUPPORTED";
        case NVME_SC_CTRL_LIST_INVALID:         return "CONTROLLER_LIST_INVALID";
        case NVME_SC_BP_WRITE_PROHIBITED:       return "BOOT PARTITION WRITE PROHIBITED";
        case NVME_SC_BAD_ATTRIBUTES:            return "BAD_ATTRIBUTES";
        case NVME_SC_WRITE_FAULT:               return "WRITE_FAULT";
        case NVME_SC_READ_ERROR:                return "READ_ERROR";
        case NVME_SC_GUARD_CHECK:               return "GUARD_CHECK";
        case NVME_SC_APPTAG_CHECK:              return "APPTAG_CHECK";
        case NVME_SC_REFTAG_CHECK:              return "REFTAG_CHECK";
        case NVME_SC_COMPARE_FAILED:            return "COMPARE_FAILED";
        case NVME_SC_ACCESS_DENIED:             return "ACCESS_DENIED";
        case NVME_SC_UNWRITTEN_BLOCK:           return "UNWRITTEN_BLOCK";
        default:                                return "Unknown";
        }
}

int nvmeibt_local_disk_util_read_smart_info(int seq, struct nvmeibt_local_disk_util_smart_info *smart_info, int disk_fd)
{
	int					smart_fd = -1;
	int					rv = 0;
	char 				file_name[100];
	char				file_data[2048];
	char 				*tok = NULL;
	char 				*delim = "\n";
	char 				*saveptr;
	int 				i, capab_idx = 0;
	struct nvme_id_ns 	ns;
	const struct nvmeibt_disk_flow_params_t *params = NULL;

	NFIN;
	if (seq < 0) {
		// Dummy, already initialized its smart on local_disk_add
		goto out;
	}

	N_Tf(trace_local_disk_util_nvmeibt_local_disk_util_read_smart_info, "Filling smart info from /proc/nvmeibs/smart@SEQ", seq);

	memset(file_data, 0, sizeof(file_data));
	sprintf(file_name, "%s%d", "/proc/nvmeibs/smart", seq);

	smart_fd = NNVMEIBT_OPEN_READ(vgs72k0, file_name, 1);

	if (smart_fd < 0) {
		N_Wf(warn_local_disk_util_nvmeibt_local_disk_util_read_smart_info, "OOPS! Error while opening the file @FILE_NAME, @AUTO_ERRNO", file_name);
		rv = -1;
		goto out;
	}
	N_Tf(trace_2_local_disk_util_nvmeibt_local_disk_util_read_smart_info, "Opened file @FILE_NAME for read.", file_name);

	if (read(smart_fd, file_data, sizeof(file_data) - 1) < 0) {
		N_Wf(warn_1_local_disk_util_nvmeibt_local_disk_util_read_smart_info, "OOPS! Error while reading the file @FILE_NAME, @AUTO_ERRNO", file_name);
		rv = -1;
		goto out;
	}

	tok = strtok_r(file_data, delim, &saveptr);

	while (tok != NULL) {
		if (strstr(tok, "Pci Address=") != NULL) {
			char *val = strstr(tok, "=");
			nvmeibt_strlcpy(smart_info->Pci_Address, val + 1, sizeof(smart_info->Pci_Address));
		} else if (strstr(tok, "Serial Number=") != NULL) {
			char *val = strstr(tok, "=");
			nvmeibt_strlcpy(smart_info->Serial_Number, val + 1, sizeof(smart_info->Serial_Number));
		} else if (strstr(tok, "Vendor=") != NULL) {
			char *val = strstr(tok, "=");
			smart_info->Vendor = (unsigned short)strtoul(val + 1, NULL, 16);
		} else if (strstr(tok, "Model=") != NULL) {
			const char *val = strstr(tok, "=");
			nvmeibt_strlcpy(smart_info->Model, val + 1, sizeof(smart_info->Model));
		} else if (strstr(tok, "Submission Queues=") != NULL) {
			char *val = strstr(tok, "=");
			smart_info->Submission_Queues = (unsigned int)strtoul(val + 1, NULL, 0);
		} else if (strstr(tok, "Completion Queues=") != NULL) {
			char *val = strstr(tok, "=");
			smart_info->Completion_Queues = (unsigned int)strtoul(val + 1, NULL, 0);
		} else if (strstr(tok, "MSIX Interrupts=") != NULL) {
			char *val = strstr(tok, "=");
			smart_info->MSIX_Interrupts = (unsigned int)strtoul(val + 1, NULL, 0);
		} else if (strstr(tok, "Num admin cmds=") != NULL) {
			char *val = strstr(tok, "=");
			const u32 dont_care = (unsigned int)strtoul(val + 1, NULL, 0);
			(void)dont_care;
		} else if (strstr(tok, "Numa Node=") != NULL) {
			char *val = strstr(tok, "=");
			smart_info->Numa_Node = (unsigned int)strtoul(val + 1, NULL, 0);
		} else if (strstr(tok, "Critical Warning=") != NULL) {
			char *val = strstr(tok, "=");
			smart_info->Critical_Warning = (unsigned short)strtoul(val + 1, NULL, 16);
		} else if (strstr(tok, "Temperature=") != NULL) {
		} else if (strstr(tok, "Available Spare=") != NULL) {
			char temp[10];
			char *val = strstr(tok, "=");

			nvmeibt_strlcpy(temp, val + 1, min(sizeof(temp), strlen(val + 1) - 2 + 1/*Avoid last space and %*/));
			smart_info->Available_Spare = (unsigned int)strtoul(temp, NULL, 0);
		} else if (strstr(tok, "Available Spare Threshold=") != NULL) {
			char temp[10];
			char *val = strstr(tok, "=");

			nvmeibt_strlcpy(temp, val + 1, min(sizeof(temp), strlen(val + 1) - 2 + 1/*Avoid last space and %*/));
			smart_info->Available_Spare_Threshold = (unsigned int)strtoul(temp, NULL, 0);
		} else if (strstr(tok, "Percentage Used=") != NULL) {
			char temp[10];
			char *val = strstr(tok, "=");

			nvmeibt_strlcpy(temp, val + 1, min(sizeof(temp), strlen(val + 1) - 2 + 1/*Avoid last space and %*/));
			smart_info->Percentage_Used = (unsigned int)strtoul(temp, NULL, 0);
		} else if (strstr(tok, "Data Units Read=") != NULL) {
		} else if (strstr(tok, "Data Units Written=") != NULL) {
		} else if (strstr(tok, "Host Read Commands=") != NULL) {
		} else if (strstr(tok, "Host Write Commands=") != NULL) {
			char *val = strstr(tok, "=");
			smart_info->Host_Write_Commands = strtoull(val + 1, NULL, 16);
		} else if (strstr(tok, "Controller Busy Time=") != NULL) {
			char *val = strstr(tok, "=");
			smart_info->Controller_Busy_Time = (unsigned int)strtoul(val + 1, NULL, 16);
		} else if (strstr(tok, "Power Cycles=") != NULL) {
			char *val = strstr(tok, "=");
			smart_info->Power_Cycles = (unsigned int)strtoul(val + 1, NULL, 16);
		} else if (strstr(tok, "Power On Hours=") != NULL) {
			char *val = strstr(tok, "=");
			smart_info->Power_On_Hours = (unsigned int)strtoul(val + 1, NULL, 16);
		} else if (strstr(tok, "Unsafe Shutdowns=") != NULL) {
			char *val = strstr(tok, "=");
			smart_info->Unsafe_Shutdowns = (unsigned int)strtoul(val + 1, NULL, 16);
		} else if (strstr(tok, "Media Errors=") != NULL) {
			char *val = strstr(tok, "=");
			smart_info->Media_Errors = (unsigned int)strtoul(val + 1, NULL, 16);
		} else if (strstr(tok, "Number of Error Information Log Entries=") != NULL) {
			char *val = strstr(tok, "=");
			smart_info->Number_of_Error_Information_Log_Entries = strtoull(val + 1, NULL, 16);
		} else if (strstr(tok, "Namespace Id=") != NULL) {
			char *val = strstr(tok, "=");
			smart_info->Namespace_Id = (unsigned int)strtoul(val + 1, NULL, 16);
		} else if (strstr(tok, "format_version=") != NULL) {
			char *val = strstr(tok, "=");
			smart_info->format_version = (unsigned int)strtoul(val + 1, NULL, 16);
		} else if (strstr(tok, "time=") != NULL) {
			char *val = strstr(tok, "=");
			nvmeibt_strlcpy(smart_info->time, val + 1, sizeof(smart_info->time));
		} else {
			N_Ef(error_local_disk_util_nvmeibt_local_disk_util_read_smart_info, "Unexpected token: \"@TOK\"",  tok);
			break;
		}

		tok = strtok_r(NULL, delim, &saveptr);
	}

	// fill metadata_capabilities
	if (nvmeibt_local_disk_util_nvme_identify_ns(disk_fd, smart_info->Namespace_Id, 0, &ns) != 0) {
		N_Ef(error_local_disk_util_nvmeibt_local_disk_util_read_smart_info_2, "could not identify namespace for nsid=@NSID on disk id=@SERIAL_NUMBER", smart_info->Namespace_Id, smart_info->Serial_Number);
		rv = -1;
		goto out;
	}

	params = nvmeibt_disk_flow_params_get(smart_info->Model, true);
	smart_info->format_options[capab_idx++] = '[';

	/* Fill an array of the device format options*/
	for (i = 0; i <= ns.nlbaf; i++) {
		struct nvme_lbaf cur_format = ns.lbaf[i];
		int n_written = 0;

		if (params && params->ignore_metadata && cur_format.ms > 0) {
			N_Tf(trace_5_local_disk_util_nvmeibt_local_disk_util_read_smart_info, "disk=@STR ignoring LBAF @INT+@INT due to ignore_metadata flow parameter",
					smart_info->Serial_Number,
					1<<cur_format.ds, cur_format.ms);
			continue;
		}

		if (params && params->force_512b && cur_format.ds > 9) {
			N_Tf(trace_5_1_local_disk_util_nvmeibt_local_disk_util_read_smart_info, "disk=@STR ignoring LBAF @INT+@INT due to force_512b flow parameter",
					smart_info->Serial_Number,
					1<<cur_format.ds, cur_format.ms);
			continue;
		}

		n_written = snprintf(smart_info->format_options + capab_idx,
					  sizeof(smart_info->format_options) - capab_idx,
					  "{\"dataBS\": %d, \"metaBS\": %d},", 1 << cur_format.ds, cur_format.ms);

		if (n_written < 0) {
			N_Ef(error_1_local_disk_util_nvmeibt_local_disk_util_read_smart_info, "Unable to write format option bs=@DS ms=@MS", 1 << cur_format.ds, cur_format.ms);
			rv = -1;
			goto out;
		}
		capab_idx += n_written;
	}
	N_Tf(trace_3_local_disk_util_nvmeibt_local_disk_util_read_smart_info, "disk=@STR wrote format options", smart_info->Serial_Number);

	/* Replace trailing comma with closing bracket of array */
	smart_info->format_options[max(capab_idx - 1, 1)] = ']';

out:
	NNVMEIBT_CLOSE(trace_4_local_disk_util_nvmeibt_local_disk_util_read_smart_info, smart_fd);

	NFOUT;
	rv = 0;
	return rv;
}

#define BDF_MAX_LEN					31 // We don't really know it, but it seems enough
#define BDF_FOR_SLOT_FILE_LEN_DIFF	2 // exclude ".0"

static void extract_BDF_out_of_file_name(char *link_path, char *pcie_bdf, char *bdf_for_slot_file) {
	int		i, len;

	NFIN;
	// Look for the rightmost matching pattern (0000:04:00.0) in .../.../0000:00:01.0/0000:04:00.0/.../...
	pcie_bdf[0] = '\0';
	bdf_for_slot_file[0] = '\0';
	for (i = strlen(link_path); i > 0; i--) {
		if (link_path[i] == ':' && link_path[i + 3] == ':' && link_path[i + 6] == '.' && link_path[i + 8] == '/') {
			len = 8;
			for (i--; (i >= 0) && (link_path[i] != '/'); i--)
				len++;
			if (len > BDF_MAX_LEN) {
				N_Wf(xxyy712, "BDF len>@INT, ignoring", BDF_MAX_LEN);
			} else if (i >= 0) {
				nvmeibt_strlcpy(pcie_bdf, link_path + i + 1, len + 1);	// 0000:87:00.0
				nvmeibt_strlcpy(bdf_for_slot_file, link_path + i + 1, len + (1 - BDF_FOR_SLOT_FILE_LEN_DIFF));	// 0000:87:00
			}
			break;
		}
	}
	NFOUT;
}

enum nvmeibt_disk_type nvmeibt_local_disk_get_stock_disk_type_by_dev_file_name(const char *dev_file_name, const char *udev_devpath)
{
	const char					*dev_name;
	int							i;
	enum nvmeibt_disk_type		disk_type = NVMEIBT_NOT_SUPPORTED_STOCK_DISK_TYPE;

	if (!dev_file_name) {
		N_Tf(iom98jh, "Empty dev_file_name");
		goto out;
	}

	if (IS_PATH_VDISK(dev_file_name)) {
		disk_type = NVMEIBT_VIRTUAL_DISK_TYPE;
		N_Tf(nju876t, "dev_file_name='@STR' disk_type=VIRTUAL", dev_file_name);
		goto out;
	}
	if (strncmp(dev_file_name, dev_dir, sizeof(dev_dir) - 1))
		goto out_not_supported;
	dev_name = dev_file_name + strlen(dev_dir);

	for (i = 0; dev_name[i]; i++) {
		if (dev_name[i] == '/') //subdir
			goto out_not_supported;
	}

	if (strncmp(dev_name, sata_dev_name, sizeof(sata_dev_name) - 1) == 0) {
		dev_name += strlen(sata_dev_name);
		i = 0;
		if (dev_name[i] == 0)
			goto out_not_supported;
		disk_type = NVMEIBT_EXTERNAL_DISK_TYPE;
		N_Tf(vt7uy21, "dev_file_name='@STR' disk_type=EXTERNAL", dev_file_name);
		goto out;
	}
	if (strncmp(dev_name, nvme_dev_name, sizeof(nvme_dev_name) - 1) == 0) {
		/* We verify len < 10 in order to filter out the devices using the nvmeibs driver (nvme1XXXn1 is always 10) */
		if (strlen(dev_name) < 10) {
			if ((udev_devpath == NULL) || (strstr(udev_devpath, "nvme-fabrics") != NULL)) {
				disk_type = NVMEIBT_EXTERNAL_DISK_TYPE;
				N_Tf(juijqws, "dev_file_name='@STR' disk_type=EXTERNAL", dev_file_name);
			}
			else {
				disk_type = NVMEIBT_NVME_DISK_TYPE;
				N_Tf(qw39mr4, "dev_file_name='@STR' disk_type=NVME", dev_file_name);
			}
		}
		else {
			disk_type = NVMEIBT_NVMESH_DISK_TYPE;
			N_Tf(qw39mr9, "dev_file_name='@STR' disk_type=NVMESH", dev_file_name);
		}
		goto out;
	}

out_not_supported:
	N_Tf(qwsoi91, "dev_file_name='@STR' disk_type=NOT_SUPPORTED", dev_file_name);
out:
	return disk_type;
}

bool nvmeibt_local_disk_is_nvmesh_dev_name(const char *dev_file_name, int *output_dev_idx_in_dev_name)
{
	int							output_dev_idx = -1;

	if (!dev_file_name || (strlen(dev_file_name) <= strlen(nvme_dev_name_prefix)))
		N_Ef(WB98J12, "illegal path=@STR", dev_file_name);
	else
		output_dev_idx = atoi(dev_file_name + strlen(nvme_dev_name_prefix));

	*output_dev_idx_in_dev_name = output_dev_idx;
	return (output_dev_idx >= 1000 && output_dev_idx < 1100);
}

static int get_stock_nvme_driver_chardev_file_idx_by_disk_sn(const char *native_serial)
{
	struct dirent		*dir_entry;
	char				disk_sn_from_file[64];
	ssize_t				n_chars;
	int					chardev_file_idx = -1;
	char				*endptr;
	int					fd = -1;
	DIR					*dir = NULL;
	char				class_dir[] = "/sys/class/nvme/";
	size_t				class_dir_len = strlen(class_dir);
	char				link_path[256] = "/sys/class/nvme/";
	char				full_path[256];
	int					i;

	NFIN;

	dir = opendir(class_dir);
	if (!dir) {
		goto out;
	}
	while(1) {
		dir_entry = readdir(dir);
		if (!dir_entry) {
			break;
		}
		if (strncmp(dir_entry->d_name, "nvme", 4) != 0) {
			// N_Tf(gsjerw9, "skipping @STR", dir_entry->d_name);
			continue;
		}
		snprintf(full_path, sizeof(full_path), "%s%.128s", class_dir, dir_entry->d_name);
		n_chars = readlink(full_path, link_path + class_dir_len, sizeof(link_path) - class_dir_len);
		if (n_chars < 0 || n_chars > (ssize_t)(sizeof(link_path) - class_dir_len - strlen("/serial") - 1)) {
			N_Wf(renpi1n, "Failed readlink(@STR) @STR n_chars=@INT @AUTO_ERRNO", full_path, link_path, (int)n_chars);
			continue;
		}
		nvmeibt_strlcpy(link_path + class_dir_len + n_chars, "/serial", sizeof(link_path) - n_chars - class_dir_len - strlen("/serial"));
		fd = NNVMEIBT_OPEN_READ(b2x7xha, link_path, 1);
		if (fd < 0) {
			continue;
		}
		n_chars = pread(fd, disk_sn_from_file, sizeof(disk_sn_from_file), 0);
		NNVMEIBT_CLOSE(jg84lds, fd);
		if (n_chars <= 0) {
			N_Wf(evsxr93, "read @STR failed", link_path);
			continue;
		}
		// Unfortunatelly, there are trailing spaces. Sanitize. Make n_chars=strlen(disk_sn_from_file) exclude the terminating '\0'
		for (i = 0; i < n_chars; i++) {
			if (disk_sn_from_file[i] <= ' ') {
				n_chars = i;
				break;
			}
		}
		disk_sn_from_file[n_chars] = '\0';
		if (strncmp(disk_sn_from_file, native_serial, sizeof(disk_sn_from_file)) == 0) {
			chardev_file_idx = strtol(dir_entry->d_name + strlen("nvme"), &endptr, 10);
			if (endptr == (dir_entry->d_name + strlen("nvme"))) {
				N_Wf(ekqqjd4, "Cannot extract idx from @STR", dir_entry->d_name);
				chardev_file_idx = -1;
				continue;
			}
			break;
		}
	}
out:
	if (dir)
		closedir(dir);
	N_Tf(tvk2okw, "disk_sn=@STR chardev_file_idx=@INT", native_serial, chardev_file_idx);
	NFOUT;
	return chardev_file_idx;
}

static void _get_pcie_slot_from_dev_file_name(const char *dev_file_name, struct nvmeibt_ascii_uuid *native_serial, char *pcie_slot, char *pcie_bdf)
{
	DIR *dir = NULL;
	char block_path[256];
	char link_path[256]="";
	int rc;
	int fd = 0;

	char					line[256];
	__kernel_size_t			line_len;
	bool					is_nvmesh_device;
	int						chardev_file_idx;
	char					nvmesh_device_no;
	const char				nvmesh_smart_prefix[] = "/proc/nvmeibs/smart";
	struct nvmeibt_Str		*file_content_str = NULL;
	const char				PCI_line_key[] = "Pci Address=";
	char					*scan_line_ptr;
	char					*scan_line_end;
	char					*content_end;
	char					bdf_for_slot_file[BDF_MAX_LEN + 1];
	int						PCI_line_key_len = strlen(PCI_line_key);
	int						bdf_len;

	pcie_slot[0] = '\0';
	pcie_bdf[0] = '\0';
	bdf_for_slot_file[0] = '\0';
	// Is nvmesh device
	is_nvmesh_device = nvmeibt_local_disk_is_nvmesh_dev_name(dev_file_name, &chardev_file_idx);
	if (chardev_file_idx < 0) {
		N_Ef(2bxus0w, "Bad dev_file_name='@STR'", dev_file_name);
		goto out;
	}
	//
	N_Tf(4ysiwmc, "@STR is_nvmesh_device=@INT", dev_file_name, is_nvmesh_device);
	if (is_nvmesh_device) {
		// Read from our own, non_standard, smart_file /proc/nvmeibs/smart<i>
		nvmesh_device_no = chardev_file_idx - 1000;		// The '2' in /dev/nvme1002n1
		snprintf(link_path, sizeof(link_path), "%s%d", nvmesh_smart_prefix, nvmesh_device_no);		// /proc/nvmeibs/smart2
		fd = NNVMEIBT_OPEN_READ(tvcjhs3, link_path, 1);
		if (fd < 0) {
			N_ETf(6cvsk40, "Error while opening the file=@STR @AUTO_ERRNO", link_path);
			goto out;
		}
		file_content_str = NNVMEIBT_STR_ALLOC(7643hsd);
		NNVMEIBT_STR_FREAD(ycbsm4j, file_content_str, fd);

		content_end = (char *)(nvmeibt_Str_str(file_content_str) + nvmeibt_Str_strlen(file_content_str));
		scan_line_ptr = (char *)nvmeibt_Str_str(file_content_str);
		while (scan_line_ptr < content_end) {
			// Get a nice, null_terminated line
			scan_line_end = (char *)memchr(scan_line_ptr, '\n', content_end - scan_line_ptr);
			if (!scan_line_end) {
				N_Tf(bue2jhs, "Missing \\n at the end of line '@STR'", scan_line_ptr);
				scan_line_end = content_end;
			}
			line_len = strnlen(scan_line_ptr, scan_line_end - scan_line_ptr);	// Look for '\0' before the '\n'
			memcpy(line, scan_line_ptr, line_len);
			scan_line_ptr = scan_line_ptr + line_len + 1;	// For next line
			line[line_len] = '\0';

			N_Tf(65bfs9k, "line_len=@SIZEOF '@STR'", line_len, line);
			if (line_len < 1) {
				continue;
			}
			if (strncmp(line, PCI_line_key, PCI_line_key_len) == 0) {
				bdf_len = strlen(line + PCI_line_key_len);
				if (bdf_len > BDF_MAX_LEN) {
					N_Wf(xxyy716, "BDF len>@INT, ignoring", BDF_MAX_LEN);
				}
				else {
					nvmeibt_strlcpy(pcie_bdf, line + PCI_line_key_len, bdf_len + 1);	// 0000:87:00.0
					nvmeibt_strlcpy(bdf_for_slot_file, line + PCI_line_key_len, bdf_len + (1 - BDF_FOR_SLOT_FILE_LEN_DIFF));	// 0000:87:00
					N_Tf(vbnjhsd, "@STR BDF=@STR BDF_for_slot_file=@STR", dev_file_name, pcie_bdf, bdf_for_slot_file);
				}
				break;
			}
		}
		block_path[0] = '\0';	// Irelevant for nvmesh devices
	} else {	// A stock driver device such as /dev/nvme2n1
		/*
		 * Seems like the most standard technique is
		 * # ls -l  /sys/class/nvme/nvme2
		 * lrwxrwxrwx 1 root root 0 Apr 26 18:45 /sys/class/nvme/nvme2 -> ../../devices/pci0000:00/0000:00:01.0/0000:04:00.0/nvme/nvme2
		 * We want the last qualifier before "nvme", with & without the function subvalue - I.e. "0000:04:00" & "0000:04:00.0"
		 *
		 * BUT !!!!
		 * [root@nvme31 13:04:16 ~]$ uname -a
		 * Linux nvme31.excelero.com 4.15.0-141-generic #145-Ubuntu SMP Wed Mar 24 18:08:07 UTC 2021 x86_64 x86_64 x86_64 GNU/Linux
		 *
		 * We noticed a bug in the stock NVME driver where the chardev of say nvme0, actually controls the blkdev of nvme1(nvme1n1),
		 *  so an unbind and bind affects unexpected block devices.
		 * In order to overcome this we look for the disk_sn (the disk ID - S3HCNX0JC02021) of the "nvme1n1".from_config->id of nvme1n1 in all
		 *  the content of the files of the abovementioned /sys/devices/pci0000:00/0000:00:01.0/0000:04:00.0/nvme/nvme2/serial
		 */
#ifndef WORKAROUND_FOR_BUG_WHERE_CHARDEV_nvme0_CONTROLS_BLKDEV_nvme1n1
		int		prev_chardev_file_idx = chardev_file_idx;
		chardev_file_idx = get_stock_nvme_driver_chardev_file_idx_by_disk_sn(native_serial->str);
		if (chardev_file_idx < 0) {
			goto out;
		}
		if (chardev_file_idx != prev_chardev_file_idx) {
			N_Wf(cvsj5ja, "Note that chardev nvme@INT actually controls blkdev @STR @STR", chardev_file_idx, dev_file_name, native_serial->str);
		}
#endif	// #ifdef WORKAROUND_FOR_BUG_CONFUSING_CHARDEV_WITH_A_SIBLING_BLKDEV
		snprintf(block_path, sizeof(block_path), "/sys/class/nvme/nvme%d", chardev_file_idx);
		rc = readlink(block_path, link_path, sizeof(link_path));
		if (rc < 0) {
			N_Wf(big7d5w, "Failed readlink(@STR) block_path=@STR @AUTO_ERRNO. Trying /sys/block", link_path, block_path);
		} else {
			link_path[rc] = '\0';
			extract_BDF_out_of_file_name(link_path, pcie_bdf, bdf_for_slot_file);
		}
		if (pcie_bdf[0] == '\0') {	// No success with the previous method
			// Try another method
			/*
			 * # ls -l /sys/block/nvme2n1
			 * lrwxrwxrwx 1 root root 0 Nov  5 15:37 nvme2n1 -> ../devices/pci0000:d7/0000:d7:05.5/pci10004:00/10004:00:01.0/10004:02:00.0/nvme/nvme2/nvme2n1
			 * We want the last qualifier before "nvme", with & without the function subvalue - I.e. "10004:02:00" & "10004:02:00.0"
			 */
			snprintf(block_path, sizeof(block_path), "/sys/block/%s", dev_file_name + 5);
			rc = readlink(block_path, link_path, sizeof(link_path));
			if (rc < 0) {
				N_Ef(fjsi4mw, "Failed readlink(@STR) block_path=@STR @AUTO_ERRNO", link_path, block_path);
				goto out;
			}
			link_path[rc] = '\0';
			extract_BDF_out_of_file_name(link_path, pcie_bdf, bdf_for_slot_file);
		}
	}
	N_Tf(bhsa84k, "block_path=@STR link_path=@STR fd=@INT BDF=@STR BDF_for_slot_file=@STR", block_path, link_path, fd, pcie_bdf, bdf_for_slot_file);

	if (pcie_bdf[0] == '\0') {
		N_Wf(icns47m, "Failed to find bdf for dev_file_name=@STR", dev_file_name);
		goto out;
	}

	// Look in sysfs for the slot whose address file contains our BDF. Each slot is represented by a directory.
	dir = opendir("/sys/bus/pci/slots");
	if (!dir)
		goto out;
	while(1) {
		struct dirent *result = readdir(dir);
		if (!result)
			break;

		if (result->d_type == DT_DIR && result->d_name[0] != '.') {
			char path[512];
			char addr[32]="";
			rc = -1;

			snprintf(path, sizeof(path), "/sys/bus/pci/slots/%s/address", result->d_name);
			fd = open(path, O_RDONLY);
			if (fd < 0) {
				N_Ef(5dimslp, "Failed to open file=@STR @AUTO_ERRNO", path);
				goto out;
			}
			rc = read(fd, addr, sizeof(addr));
			if (rc < 1) {
				N_Tf(czjq83j, "Failed read(@STR) rc=@INT @AUTO_ERRNO", path, rc);
				close(fd);
				continue;
			}
			close(fd);
			addr[rc-1] = 0;		// chop the newline
			if (strcmp(bdf_for_slot_file, addr)==0) {
				nvmeibt_strlcpy(pcie_slot, result->d_name, NVMEIBS_DISKS_CSV_STATUS_LEN);
				break;
			}
		}
	}
	N_Tf(d629dj4, "dev=@STR pcie_slot=@STR", dev_file_name, pcie_slot);

out:
	NNVMEIBT_STR_FREE(0amnfue, file_content_str);
	if (dir)
		closedir(dir);
}

void nvmeibt_local_disk_util_set_attention_LED(const char *pcie_slot, int value)
{
	char path[256];
	char buf[32];
	int len;
	int fd;

	if (pcie_slot[0] == 0)
		return;

	len = sprintf(buf, "%u\n", value);
	sprintf(path, "/sys/bus/pci/slots/%s/attention", pcie_slot);
	fd = open(path, O_WRONLY);
	if (fd >= 0) {
		write(fd, buf, len);
		close(fd);
	}
}

static void get_serial_num_from_ldisk_id(char *ser_num, char *ldisk_id, int max_ser_num_len)
{
	int				i;

	nvmeibt_strlcpy(ser_num, ldisk_id, max_ser_num_len);

	// cat namespace_id
	for (i = 0; ser_num[i] != 0; i++) {
		if (ser_num[i] == '.') {
			ser_num[i] = 0;
			break;
		}
	}
}

static void	nvmeibt_local_disk_generate_ldisk_id_from_native_serial_and_nsid(struct nvmeibt_local_disk_config *f)
{
	if (f->ldisk_id.str[0] == '\0') {
		// Generate from_config->ldisk_id from serial & nsid
		snprintf(f->ldisk_id.str, sizeof(f->ldisk_id.str), "%.24s.%d", f->native_serial.str, f->nsid);
	}
}

BOOL nvmeibt_local_disk_util_fill_local_disk_devinfo_and_smart_from_nvme_driver(struct nvmeibt_local_disk_config *from_config, const char *dev_file_name, int fd)
{
	int i, capab_idx = 0;
	BOOL rv = true;
	struct nvme_dev_info nvme_drive;
	struct nvme_smart_log smart_log;
	BOOL	is_already_read_from_local_disks_csv = (strlen(from_config->dev_file_name) > 0);
	const struct nvmeibt_disk_flow_params_t *params = NULL;
	BUILD_BUG_ON(sizeof(from_config->ldisk_id.str) <= sizeof(nvme_drive.ctrl.sn));

	NFIN;

	if (nvmeibt_local_disk_util_get_nvme_dev_info(&nvme_drive, dev_file_name, fd) < 0) {
		N_Ef(error_local_disk_util_nvmeibt_local_disk_util_fill_local_disk_config_from_nvme_driver, "failed reading nvme device info from path=@PATH fd=@FD", dev_file_name, fd);
		goto out;
	}

	if (nvmeibt_local_disk_util_get_nvme_smart_log(fd, &smart_log) < 0) {
		N_Ef(error_1_local_disk_util_nvmeibt_local_disk_util_fill_local_disk_config_from_nvme_driver, "failed reading device smart log from path=@PATH", dev_file_name);
		goto out;
	}

	if (!is_already_read_from_local_disks_csv) {
		from_config->pblk_size = 1 << nvme_drive.ns.lbaf[(nvme_drive.ns.flbas & 0x0f)].ds;
		from_config->metadata_n_bytes = nvme_drive.ns.lbaf[(nvme_drive.ns.flbas & 0x0f)].ms;
		// Sanitize the disk sn to avoid leading/trailing spaces.
		nvmeib_copy_str_trim_spaces(from_config->native_serial.str, nvme_drive.ctrl.sn, sizeof(from_config->native_serial.str), sizeof(nvme_drive.ctrl.sn));
		snprintf(from_config->ldisk_id.str, sizeof(from_config->ldisk_id.str), "%.59s.%d", from_config->native_serial.str, nvme_drive.nsid);

		from_config->n_hw_pblk = from_config->n_pblk = nvme_drive.ns.nsze;
		from_config->vendor = nvme_drive.ctrl.vid;
		from_config->max_request_size = (1 << nvme_drive.ctrl.mdts);

		from_config->nsid = nvme_drive.nsid;
		sprintf(from_config->dev_file_name, "%.*s", NVMEIBT_LOCAL_DISK_DEV_FILE_NAME_LEN - 1, nvme_drive.path);
	}
	nvmeibt_strlcpy(from_config->smart_info.Serial_Number, from_config->native_serial.str, sizeof(from_config->smart_info.Serial_Number));
	from_config->smart_info.Namespace_Id = nvme_drive.nsid;
	nvmeibt_local_disk_generate_ldisk_id_from_native_serial_and_nsid(from_config);
	nvmeibt_strlcpy(from_config->smart_info.diskID, from_config->ldisk_id.str, sizeof(from_config->smart_info.diskID));
	from_config->smart_info.blocks = from_config->n_pblk;
	from_config->smart_info.block_size = from_config->pblk_size;
	from_config->smart_info.metadata_size = from_config->metadata_n_bytes;
	from_config->smart_info.metadata_cap = nvme_drive.ns.mc;

	from_config->smart_info.Vendor = nvme_drive.ctrl.vid;

	{ _Static_assert (sizeof(from_config->smart_info.Model) > sizeof(nvme_drive.ctrl.mn), "Model field is too short");}
	{ _Static_assert (NVMEIB_DISK_MAX_MODEL_STR_SIZE == sizeof(nvme_drive.ctrl.mn), "Model str size is incorrect");}
	nvmeib_copy_str_trim_spaces(from_config->smart_info.Model, nvme_drive.ctrl.mn,
			sizeof(from_config->smart_info.Model), sizeof(nvme_drive.ctrl.mn));
	from_config->smart_info.format_options[capab_idx++] = '[';

	params = nvmeibt_disk_flow_params_get(from_config->smart_info.Model, true);
	/* Fill an array of device format options*/
	for (i = 0; i <= nvme_drive.ns.nlbaf; i++) {
		struct nvme_lbaf cur_format = nvme_drive.ns.lbaf[i];
		int n_written = 0;

		if (params && params->ignore_metadata && cur_format.ms > 0) {
			N_Tf(trace_3_local_disk_util_nvmeibt_local_disk_util_fill_local_disk_config_from_nvme_driver,
					"disk=@STR ignoring LBAF @INT+@INT due to ignore_metadata flow parameter",
					from_config->smart_info.Serial_Number,
					1<<cur_format.ds, cur_format.ms);
			continue;
		}

		if (params && params->force_512b && cur_format.ds > 9) {
			N_Tf(trace_3_1_local_disk_util_nvmeibt_local_disk_util_fill_local_disk_config_from_nvme_driver, "disk=@STR ignoring LBAF @INT+@INT due to force_512b flow parameter",
					from_config->smart_info.Serial_Number,
					1<<cur_format.ds, cur_format.ms);
			continue;
		}

		n_written = snprintf(from_config->smart_info.format_options + capab_idx,
					  sizeof(from_config->smart_info.format_options) - capab_idx,
					  "{\"dataBS\": %d, \"metaBS\": %d},", 1 << cur_format.ds, cur_format.ms);

		if (n_written < 0) {
			N_Ef(ca9k3bo, "Unable to write format option for disk=@STR bs=@DS ms=@MS", nvmeibt_local_disk_config_display(from_config), 1 << cur_format.ds, cur_format.ms);
			rv = false;
			goto out;
		}
		capab_idx += n_written;
	}
	N_Tf(215ncmu, "disk=@STR wrote format options", nvmeibt_local_disk_config_display(from_config));

	/* Replace trailing comma with closing bracket of array */
	from_config->smart_info.format_options[max(capab_idx - 1, 1)] = ']';

	from_config->smart_info.Critical_Warning = smart_log.critical_warning;
	from_config->smart_info.Available_Spare = smart_log.avail_spare;
	from_config->smart_info.Available_Spare_Threshold = smart_log.spare_thresh;
	from_config->smart_info.Percentage_Used = smart_log.percent_used;
	from_config->smart_info.Host_Write_Commands = nvmeibt_host_writes_int128_to_uint64(smart_log.host_writes);
	from_config->smart_info.Power_Cycles = nvmeibt_host_writes_int128_to_uint64(smart_log.power_cycles);
	from_config->smart_info.Power_On_Hours = nvmeibt_host_writes_int128_to_uint64(smart_log.power_on_hours);
	from_config->smart_info.Unsafe_Shutdowns = nvmeibt_host_writes_int128_to_uint64(smart_log.unsafe_shutdowns);
	from_config->smart_info.Media_Errors = nvmeibt_host_writes_int128_to_uint64(smart_log.media_errors);
	from_config->smart_info.Number_of_Error_Information_Log_Entries = nvmeibt_host_writes_int128_to_uint64(smart_log.num_err_log_entries);

	N_Tf(xcir1zm, "Read local disk=@STR vendor=@VENDOR model=@STR dev=@DEV_FILE_NAME pblk_size=@PBLK_SIZE n_pblk=@N_PBLK metadata_size=@METADATA_SIZE format_options=@FORMAT_OPTIONS",
		 nvmeibt_local_disk_config_display(from_config), from_config->vendor, from_config->smart_info.Model, from_config->dev_file_name,
		from_config->pblk_size, from_config->n_pblk,
		from_config->metadata_n_bytes,
		from_config->smart_info.format_options);

	_get_pcie_slot_from_dev_file_name(dev_file_name, &(from_config->native_serial), from_config->pcie_slot, from_config->pcie_bdf);
	nvmeibt_local_disk_util_set_attention_LED(from_config->pcie_slot, 15);
out:
	NFOUT;
	return rv;
}

#include <scsi/sg.h>
#include <scsi/scsi.h>
#include "nvmeibt_debug.h"
#include "nvmeibt_common.h"
#include <arpa/inet.h>
#include <sys/sysmacros.h>

static void udev_get_env_string(char *env_buf, char *pattern, char *out, int len)
{
	char *p_start, *p_end;
	p_start = strstr(env_buf, pattern);
	if (p_start) {
		int i;

		p_start += strlen(pattern);
		p_end = strchr(p_start, '\n');
		memset(out, 0, len);
		for (i=0; i<p_end-p_start; i++) {
			if (p_start[i] == '_')
				out[i] = ' ';
			else
				out[i] = p_start[i];
		}
		out[p_end-p_start] = 0;
	}
}

static unsigned int vendor_id_from_string(char *model)
{
	typedef struct { char *name; unsigned int vendor_id; } name_pair;
	name_pair vendor_name_to_id[] = {
			{ "HGST", 0x1c58 },
			{ "Samsung", 0x144d },
			{ "Intel", 0x8086 },
			{ "Micron", 0x1344 },
			{ "Memblaze", 0x1c5f },
			{ "Toshiba", 0x1179 },
			{ "SanDisk", 0x15b7 },
			{ "WesternDigital", 0x1b96},
			{ "Kingston", 0x2646},
	};
	int array_len = sizeof(vendor_name_to_id) / sizeof(name_pair);
	int i;
	int model_len = strlen(model);

	for (i=0; i<array_len; i++) {
		name_pair *np = &vendor_name_to_id[i];
		int vlen = strlen(np->name);

		if (model_len > vlen && strncasecmp(np->name, model, vlen) == 0)
			return np->vendor_id;
	}
	return 0;
}

BOOL nvmeibt_local_disk_util_fill_devinfo_for_vdisk(
	struct nvmeibt_local_disk_config *from_config, const char *path, int fd)
{
	uint64_t size;
	const char *vdisk_name;

	NFIN;
	nvmeibt_strlcpy(from_config->dev_file_name, path, sizeof(from_config->dev_file_name));
	nvmeibt_local_disk_util_extract_stripped_dev_name_from_dev_file_name(from_config->stripped_dev_file_name, from_config->dev_file_name);

	if (ioctl(fd, BLKBSZGET, &from_config->pblk_size) < 0)
		return false;
	if (ioctl(fd, BLKGETSIZE64, &size) < 0)
		return false;
	from_config->n_hw_pblk = from_config->n_pblk = size / from_config->pblk_size;

	vdisk_name = kbasename(path);
	sprintf(from_config->smart_info.Serial_Number, "%s", vdisk_name);
	sprintf(from_config->native_serial.str, "%s", vdisk_name);
	sprintf(from_config->ldisk_id.str, "%s", vdisk_name);
	from_config->smart_info.Namespace_Id = 0;
	sprintf(from_config->smart_info.diskID, "%s", vdisk_name);

	from_config->pcie_slot[0] = 0;
	from_config->metadata_n_bytes = 0;
	from_config->max_request_size = 64;
	from_config->nsid = 0;

	from_config->smart_info.Vendor = VDISK_VENDOR;
	from_config->vendor = VDISK_VENDOR;
	sprintf(from_config->smart_info.Model, VDISK_MODEL);
	from_config->smart_info.blocks = from_config->n_pblk;
	from_config->smart_info.block_size = from_config->pblk_size;
	from_config->smart_info.metadata_size = from_config->metadata_n_bytes;
	from_config->smart_info.metadata_cap = 0;

	snprintf(from_config->smart_info.format_options,
		 sizeof(from_config->smart_info.format_options),
		 "[ {\"dataBS\": %d, \"metaBS\": %d} ]", from_config->pblk_size,
		 from_config->metadata_n_bytes);

	from_config->smart_info.Critical_Warning = 0;
	from_config->smart_info.Available_Spare = 0;
	from_config->smart_info.Available_Spare_Threshold = 0;
	from_config->smart_info.Percentage_Used = 0;
	from_config->smart_info.Host_Write_Commands = 0;
	from_config->smart_info.Power_Cycles = 0;
	from_config->smart_info.Power_On_Hours = 0;
	from_config->smart_info.Unsafe_Shutdowns = 0;
	from_config->smart_info.Media_Errors = 0;
	from_config->smart_info.Number_of_Error_Information_Log_Entries = 0;

	NFOUT;
	return true;
}

BOOL nvmeibt_local_disk_util_fill_local_disk_devinfo_and_smart_from_udev(struct nvmeibt_local_disk_config *from_config, const char *path, int fd)
{
	BOOL rv = true;
	BOOL	is_already_read_from_local_disks_csv = (strlen(from_config->dev_file_name) > 0);
	struct stat st;
	char udev_info_path[64];
	int udev_info_fd;
	char *udev_info_buf = NULL;
	int stat_rv;

	NFIN;
	nvmeibt_strlcpy(from_config->dev_file_name, path, sizeof(from_config->dev_file_name));
	nvmeibt_local_disk_util_extract_stripped_dev_name_from_dev_file_name(from_config->stripped_dev_file_name, from_config->dev_file_name);

	memset(&st, 0, sizeof(st));
	stat_rv = stat(path, &st);
	snprintf(udev_info_path, sizeof(udev_info_path), "/run/udev/data/b%d:%d", major(st.st_rdev), minor(st.st_rdev));
	if (stat(udev_info_path, &st) < 0) {
		N_Ef(error_1_local_disk_util_nvmeibt_local_disk_util_fill_local_disk_config_from_udev, "failed getting udev device info from path=@PATH fd=@FD", udev_info_path, stat_rv);
		rv = false;
		goto out;
	}

	udev_info_fd = open(udev_info_path, O_RDONLY);
	udev_info_buf = (char *)malloc(st.st_size + 1);
	read(udev_info_fd, udev_info_buf, st.st_size);
	udev_info_buf[st.st_size] = 0;
	close(udev_info_fd);

	if (!is_already_read_from_local_disks_csv) {
		uint64_t size;
		if (ioctl(fd, BLKSSZGET, &from_config->pblk_size) < 0) {
			rv = false;
			goto out;
		}
		if (ioctl(fd, BLKGETSIZE64, &size) < 0) {
			rv = false;
			goto out;
		}
		if (from_config->pblk_size == 0) {
			rv = false;
			goto out;
		}
		from_config->n_hw_pblk = from_config->n_pblk = size / from_config->pblk_size;
		N_Tf(error_2_local_disk_util_nvmeibt_local_disk_util_fill_local_disk_config_from_udev, "ioctl SG_IO path=@PATH blksize @INT", path, from_config->pblk_size);

		from_config->pcie_slot[0] = 0;
		from_config->metadata_n_bytes = 0;
		udev_get_env_string(udev_info_buf, "E:ID_SERIAL_SHORT=", from_config->ldisk_id.str, sizeof(from_config->ldisk_id.str));
		from_config->max_request_size = 64;

		from_config->nsid = 0;
	}
	if (from_config->pblk_size<0 || from_config->pblk_size>4096) {
		N_Ef(error_3_fill_local_disk_config_from_udev, "Invalid blocksize reported by disk @PATH, aborting.", path);
		rv = false;
		goto out;
	}
	get_serial_num_from_ldisk_id(from_config->smart_info.Serial_Number, from_config->ldisk_id.str, sizeof(from_config->smart_info.Serial_Number));
	nvmeibt_strlcpy(from_config->native_serial.str, from_config->smart_info.Serial_Number, sizeof(from_config->native_serial.str));
	from_config->smart_info.Namespace_Id = from_config->nsid;

	nvmeibt_strlcpy(from_config->smart_info.diskID, from_config->ldisk_id.str, sizeof(from_config->smart_info.diskID));
	from_config->smart_info.blocks = from_config->n_pblk;
	from_config->smart_info.block_size = from_config->pblk_size;
	from_config->smart_info.metadata_size = from_config->metadata_n_bytes;
	from_config->smart_info.metadata_cap = 0;

	udev_get_env_string(udev_info_buf, "E:ID_MODEL=", from_config->smart_info.Model, sizeof(from_config->smart_info.Model));
	from_config->vendor = vendor_id_from_string(from_config->smart_info.Model);
	from_config->smart_info.Vendor = from_config->vendor;

	snprintf(from_config->smart_info.format_options, sizeof(from_config->smart_info.format_options),
			"[ {\"dataBS\": %d, \"metaBS\": %d} ]", from_config->pblk_size, from_config->metadata_n_bytes);

	from_config->smart_info.Critical_Warning = 0;
	from_config->smart_info.Available_Spare = 0;
	from_config->smart_info.Available_Spare_Threshold = 0;
	from_config->smart_info.Percentage_Used = 0;
	from_config->smart_info.Host_Write_Commands = 0;
	from_config->smart_info.Power_Cycles = 0;
	from_config->smart_info.Power_On_Hours = 0;
	from_config->smart_info.Unsafe_Shutdowns = 0;
	from_config->smart_info.Media_Errors = 0;
	from_config->smart_info.Number_of_Error_Information_Log_Entries = 0;

	N_Tf(trace_1_local_disk_util_nvmeibt_local_disk_util_fill_local_disk_config_from_udev, "Read local disk=@STR vendor=@VENDOR, dev=\"@DEV_FILE_NAME\" pblk_size=@PBLK_SIZE n_pblk=@N_PBLK metadata_size=@METADATA_SIZE format_options=@FORMAT_OPTIONS",
		 nvmeibt_local_disk_config_display(from_config), from_config->vendor, from_config->dev_file_name,
		from_config->pblk_size, from_config->n_pblk,
		from_config->metadata_n_bytes,
		from_config->smart_info.format_options);
out:
	if (udev_info_buf)
		free(udev_info_buf);
	NFOUT;
	return rv;
}

