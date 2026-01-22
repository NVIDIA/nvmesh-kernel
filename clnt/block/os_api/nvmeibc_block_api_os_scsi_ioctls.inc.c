#pragma push_macro("__FILE_LITERAL__")
#undef __FILE_LITERAL__
#define __FILE_LITERAL__ nvmeibc_block_api_os_scsi_ioctls_inc_c


#ifndef BLK_MAX_CDB
	#if KS_HAS_SCSCI_REQUEST_H
		#include "scsi/scsi_request.h"
	#else
		#include "scsi/scsi_cmnd.h"
		#define BLK_MAX_CDB MAX_COMMAND_SIZE
	#endif
#endif

#include <scsi/sg.h>
#include <scsi/scsi.h>
#include "/usr/include/linux/nvme_ioctl.h"

#define SNTI_TRANSLATION_SUCCESS 0

#define INQUIRY_EVPD_BYTE_OFFSET 1
#define INQUIRY_EVPD_BIT_MASK 1
#define INQUIRY_PAGE_CODE_BYTE_OFFSET 2
#define INQUIRY_CDB_ALLOCATION_LENGTH_OFFSET 3

#define STANDARD_INQUIRY_LENGTH 36
#define ADDITIONAL_STD_INQ_LENGTH 31

#define INQ_STANDARD_INQUIRY_PAGE 0x00
#define INQ_SUPPORTED_VPD_PAGES_PAGE 0x00
#define INQ_UNIT_SERIAL_NUMBER_PAGE 0x80

#define INQ_SERIAL_NUMBER_LENGTH 0x14

#define INQ_NUM_SUPPORTED_VPD_PAGES 2

/* VPD Page Codes */
#define VPD_SUPPORTED_PAGES 0x00
#define VPD_SERIAL_NUMBER 0x80

#define VERSION_SPC_4 0x06

#define GET_U8_FROM_CDB(cdb, index) (cdb[index] << 0)
#define GET_U16_FROM_CDB(cdb, index) ((cdb[index] << 8) | (cdb[index + 1] << 0))

/* Inquiry Helper Macros */
#define GET_INQ_EVPD_BIT(cdb) \
	((GET_U8_FROM_CDB(cdb, INQUIRY_EVPD_BYTE_OFFSET) & \
		INQUIRY_EVPD_BIT_MASK) ? 1 : 0)

#define GET_INQ_PAGE_CODE(cdb)\
	(GET_U8_FROM_CDB(cdb, INQUIRY_PAGE_CODE_BYTE_OFFSET))

#define GET_INQ_ALLOC_LENGTH(cdb) \
	(GET_U16_FROM_CDB(cdb, INQUIRY_CDB_ALLOCATION_LENGTH_OFFSET))

/* Copied from nvme-scsi.c of kernel 3.10: nvme_trans_copy_to_user */
static int nvme_trans_copy_to_user(struct sg_io_hdr *hdr, void *from,
	unsigned long n)
{
	int res = SNTI_TRANSLATION_SUCCESS;
	unsigned long not_copied;
	int i;
	void *index = from;
	size_t remaining = n;
	size_t xfer_len;

	if (hdr->iovec_count > 0) {
		struct sg_iovec sgl;

		for (i = 0; i < hdr->iovec_count; i++) {
			not_copied = copy_from_user(&sgl, hdr->dxferp +
					i * sizeof(struct sg_iovec),
					sizeof(struct sg_iovec));
			if (not_copied)
				return -EFAULT;
			xfer_len = min(remaining, sgl.iov_len);
			not_copied = copy_to_user(sgl.iov_base, index,
					xfer_len);
			if (not_copied) {
				res = -EFAULT;
				break;
			}
			index += xfer_len;
			remaining -= xfer_len;
			if (remaining == 0)
				break;
		}
		return res;
	}
	not_copied = copy_to_user(hdr->dxferp, from, n);
	if (not_copied)
		res = -EFAULT;
	return res;
}

static int nvmeibc_block_trans_standard_inq_page(const struct nvmeibc_os_api *os,
		struct sg_io_hdr *hdr, u8 *inq_response, int alloc_len)
{
	int rv, xfer_len;
	NFIN;
	(void)os;
	memset(inq_response, 0, STANDARD_INQUIRY_LENGTH);
	inq_response[2] = VERSION_SPC_4;
	inq_response[3] = 0x02;     /*normaca=0 | hisup=0 */
	inq_response[4] = ADDITIONAL_STD_INQ_LENGTH;
	inq_response[5] = 0; // protect;      /* sccs=0 | acc=0 | tpgs=0 | pc3=0 */
	inq_response[7] = (0x01 << 1);       /* wbus16=0 | sync=0 | vs=0 */
	memcpy( (char*)&inq_response[8], "NVMesh  ", 8);
	memcpy( (char*)&inq_response[16], "NVMesh          " , 16);
	strlcpy((char*)&inq_response[23], __stringify(NVMESH_RELEASE) ,9);
	memcpy( (char*)&inq_response[32], "    ", 4);
	memcpy( (char*)&inq_response[32], __stringify(NVMESH_VERSION), min((size_t)4, strlen(__stringify(NVMESH_VERSION)))); // No:\0
	xfer_len = min(alloc_len, STANDARD_INQUIRY_LENGTH);
	rv = nvme_trans_copy_to_user(hdr, inq_response, xfer_len);
	NFOUT;
	return rv;
}

static int nvmeibc_block_trans_supported_vpd_pages(const struct nvmeibc_os_api *os,
		struct sg_io_hdr *hdr, u8 *inq_response, int alloc_len)
{
	int rv, xfer_len;
	NFIN;
	(void)os;
	memset(inq_response, 0, STANDARD_INQUIRY_LENGTH);
	inq_response[1] = INQ_SUPPORTED_VPD_PAGES_PAGE;
	inq_response[3] = INQ_NUM_SUPPORTED_VPD_PAGES;
	inq_response[4] = INQ_SUPPORTED_VPD_PAGES_PAGE;
	inq_response[5] = INQ_UNIT_SERIAL_NUMBER_PAGE;
	xfer_len = min(alloc_len, STANDARD_INQUIRY_LENGTH);
	rv = nvme_trans_copy_to_user(hdr, inq_response, xfer_len);
	NFOUT;
	return rv;
}

static int nvmeibc_block_trans_unit_serial_pages(const struct nvmeibc_os_api *os,
		struct sg_io_hdr *hdr, u8 *inq_response, int alloc_len)
{
	int rv, xfer_len;
	NFIN;
	memset(inq_response, 0, STANDARD_INQUIRY_LENGTH+1);
	inq_response[1] = INQ_UNIT_SERIAL_NUMBER_PAGE;
	inq_response[3] = INQ_SERIAL_NUMBER_LENGTH;
	memcpy((char*)&inq_response[ 4], &os->dev_uuid[ 0], 8);
	memcpy((char*)&inq_response[12], &os->dev_uuid[ 9], 4);
	memcpy((char*)&inq_response[16], &os->dev_uuid[24], 4);
	memcpy((char*)&inq_response[20], &os->dev_uuid[32], 4); // No: \0 at end
	inq_response[STANDARD_INQUIRY_LENGTH] = '\0';			// Terminating
	xfer_len = min(alloc_len, STANDARD_INQUIRY_LENGTH);
	rv = nvme_trans_copy_to_user(hdr, inq_response, xfer_len);
	NFOUT;
	return rv;
}

static int __scsi_translate(const struct nvmeibc_os_api *os, struct sg_io_hdr *hdr)
{
	u8 cmd[BLK_MAX_CDB], evpd, page_code, *inq_response;
	int rv = SNTI_TRANSLATION_SUCCESS, alloc_len;
	NFIN;

	if (hdr->cmdp == NULL) {
		rv = -EMSGSIZE;
		goto out;
	}
	if (copy_from_user(cmd, hdr->cmdp, hdr->cmd_len)) {
		rv = -EFAULT;
		goto out;
	}

	if (cmd[0] != INQUIRY) {
		rv = -EINVAL; // YR: TODO: probably not the write SCSI thing to do, don't care for now.
		goto out;
	}

	evpd = GET_INQ_EVPD_BIT(cmd);
	page_code = GET_INQ_PAGE_CODE(cmd);
	alloc_len = GET_INQ_ALLOC_LENGTH(cmd);
	inq_response = kmalloc(STANDARD_INQUIRY_LENGTH+1, GFP_KERNEL);
	if (inq_response == NULL) {
		rv = -ENOMEM;
		goto out;
	}

	if (!evpd) {
		rv = (page_code == INQ_STANDARD_INQUIRY_PAGE) ?
			nvmeibc_block_trans_standard_inq_page(os, hdr, inq_response,
				alloc_len) : -EINVAL;
	} else {
		switch(page_code) {
		case VPD_SUPPORTED_PAGES:
			rv = nvmeibc_block_trans_supported_vpd_pages(os, hdr, inq_response,
				alloc_len);
			break;
		case VPD_SERIAL_NUMBER:
			rv = nvmeibc_block_trans_unit_serial_pages(os, hdr, inq_response,
				alloc_len);
			break;
		default:
			_NT(trace_api_os_scsi_translate, "Unrecognized page_code=@PAGE_CODE", page_code);
			rv = -EINVAL;
		}
	}
	kfree(inq_response);

out:
	_ND(trace_1_api_os_scsi_translate, "rv=@RV", rv);
	NFOUT;
	return rv;
}

static int nvmeibc_block_sg_io(const struct nvmeibc_os_api *os, struct sg_io_hdr __user *u_hdr)
{
	struct sg_io_hdr hdr;
	int rv;
	NFIN;
	if (!capable(CAP_SYS_ADMIN)) {
		rv = -EACCES;
		goto out;
	}
	 if (copy_from_user(&hdr, u_hdr, sizeof(hdr))) {
		 rv = -EFAULT;
		 goto out;
	 }
	 if (hdr.interface_id != 'S' || hdr.cmd_len > BLK_MAX_CDB) {
		rv = -EINVAL;
		goto out;
	 }

	 if ((rv = __scsi_translate(os, &hdr)) < 0)
		 goto out;
	 rv = rv ? SNTI_TRANSLATION_SUCCESS : 0;
	 if (copy_to_user(u_hdr, &hdr, sizeof(sg_io_hdr_t)) > 0)
		 rv = -EFAULT;
out:
	NFOUT;
	return rv;
}

static __attribute__((unused)) int nvmeibc_nvme_ioctl_id(const struct nvmeibc_os_api *os, struct nvme_id_ctrl __user *u_res)
{
	struct nvme_id_ctrl ctrl;
	int rv = 0;
	if (!capable(CAP_SYS_ADMIN)) {
		rv = -EACCES;
		goto out;
	}
	memset(&ctrl, 0, sizeof(ctrl));
	ctrl.vid =   cpu_to_le16(0x1234);
	ctrl.ssvid = cpu_to_le16(0x5678);
	ctrl.ver =   cpu_to_le32(0x00101010);
	strlcpy(ctrl.sn, os->atom.dev_name, sizeof(ctrl.sn));
	strlcpy(ctrl.mn, os->dev_uuid,      sizeof(ctrl.sn));
	// For more info see in nvmeibs: static int get_device_params(struct device_data *d)
	if (copy_to_user(u_res, &ctrl, sizeof(ctrl)) > 0)
		 rv = -EFAULT;
out:
	return rv;
}

#pragma pop_macro("__FILE_LITERAL__")
