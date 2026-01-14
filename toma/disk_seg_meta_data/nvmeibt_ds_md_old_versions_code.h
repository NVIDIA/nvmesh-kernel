/******************************* Upgrade code *********************************/
enum nvmeit_seg_md_version {
	TOMA_METADATA_VERSION_v3_0_0 = 0x00030001U,
	TOMA_METADATA_VERSION_v2_8_0 = 0x00020800U,
	TOMA_METADATA_VERSION_v2_7_0 = 0x00030000U,
};

#define DS_METADATA_MAGIC_STR  \
		("Disk Segment Metadata db5a320f-c7f4-4e16-940a-dbc2e97a6494")

static int __verify_persistent_md_unused_buf(const struct nvmeibt_seg_active_metadata_ctrl *ctrl)
{
	const struct nvmeibt_disk_segment_metadata_hdr *header = &(ctrl->header);
	int i, rv = 0;
	const char *filler_ptr;
	ssize_t			filler_size;

	filler_size = sizeof(header->__unused);
	for (i = 0; i < filler_size; i++) {
		if (header->__unused[i] != 0) {
			N_Ef(t_10_upgrade_persistent_md, "header->__unused[@IND]=@RV != 0", i, header->__unused[i]);
			rv = -1;
			goto out;
		}
	}
	filler_size = sizeof(*ctrl) - offsetof(typeof(*ctrl), __zeroed_filler_till_4K__);
	filler_ptr = ctrl->__zeroed_filler_till_4K__;
	for (i = 0; i < filler_size; i++) {
		if (filler_ptr[i] != 0) {
			N_Ef(t_11_upgrade_persistent_md, "metadata->__zeroed_filler_till_4K__[@IND]=@RV != 0", i, filler_ptr[i]);
			rv = -1;
			goto out;
		}
	}
out:
	return rv;
}

/******************************* Upgrade code End *****************************/

