#include "nvmeibs_serjio_gen_cmd_handlers.h"
#include "nvmeib_utils.h"
#include "nvmeibs_defs.h"
#include "nvmeibs_serjio_deps.h"
#include "nvmeibs_serjio.h"
#include "nvmeibs_trace.h"


struct gen_get_jmdc_params
{
	const struct nvmeib_gen_cmd_param *p;
	union nvmeib_gen_cmd_rsp *rsp;
	int dest_hdr_idx;
	int dest_ent_idx;
	int dest_ent_block_idx;
	int dirty_rng_cnt;
	unsigned rng_cnt;
	unsigned ent_cnt;
	unsigned ent_block_cnt;
	size_t rng_data_serialized_sz;
	struct sg_mapping_iter dest_jmdc_miter;
};

static DECLARE_SERJIO_RNG_CB_FN(gen_get_jmdc)
{
	int rv = 0;
	unsigned i;
	struct gen_get_jmdc_params *fn_params = ctx;
	const struct nvmeib_gen_cmd_param *cmd_params = fn_params->p;
	struct nvmeib_get_jmdc_rng_data *rng_data = &cmd_params->jmdc_get.rng_data[fn_params->dest_hdr_idx];
	struct nvmeib_jrnl_ent_md *dest_ent_md =
		&cmd_params->jmdc_get.ent_md[fn_params->dest_ent_idx];
	struct sg_mapping_iter *dest_jmdc_miter = &fn_params->dest_jmdc_miter;
	union jblock_md *dest_jmdc;
	
	NFIN;
	(void)ent_md_sz;
	(void)n_abnd_ents_in_seg;

	_ND(trace_serjio_gen_cmd_handlers_gen_get_jmdc, "range range_idx: @JRNL_RNG_IDX N: @BINJE client_uuid: @CLIENT_UUID dirty_ents: "
	NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE_TRACE
	" start_lba: @START_CLSECT size_lba: @SIZE_CLSECT",
	   range_idx, range_binje, &client_uuid, dirty_ents_in_seg_bmp, start_clsect, size_clsect);
	/* Fill in range data in header source buffer */
	memset(rng_data, 0, sizeof(*rng_data));
	rng_data->client_uuid = client_uuid;
	rng_data->rng_idx = range_idx;
	rng_data->rng_start_lba = start_clsect;
	rng_data->rng_size_lba = size_clsect;
	rng_data->rng_gen_id = gen_id;
	rng_data->only_dirty_ents = cmd_params->jmdc_get.get_dirty_only;
	rng_data->rng_ent_offset = fn_params->dest_ent_idx;
	rng_data->rng_ent_block_offset = fn_params->dest_ent_block_idx;
	rng_data->binje = range_binje;
	rng_data->num_ents = num_entries;
	rng_data->num_dirty_ents = 0;
	bitmap_copy(rng_data->dirty_ents_bmp, dirty_ents_in_seg_bmp, num_entries);
	bitmap_copy(rng_data->abnd_ents_bmp, abnd_ents_in_seg_bmp, num_entries);
	fn_params->rng_data_serialized_sz = sizeof(*rng_data);

	if (n_dirty_ents_in_seg > 0)
		fn_params->dirty_rng_cnt++;
	else if (cmd_params->jmdc_get.get_dirty_only)
		goto out;
	fn_params->dest_hdr_idx++;
	fn_params->rng_cnt++;

	if (cmd_params->jmdc_get.get_len_only) {
		_ND(trace_1_serjio_gen_cmd_handlers_gen_get_jmdc, "get_len_only");
		goto out;
	}

	if ((fn_params->dest_ent_idx + n_dirty_ents_in_seg) * sizeof(*dest_ent_md) >
		cmd_params->jmdc_get.ent_md_len) {
		_NE(error_serjio_gen_cmd_handlers_gen_get_jmdc, "NVMEIB_GET_JMDC - entries metadata buffer is too small");
		rv = -ENOMEM;
		goto out;
	}

	if ((fn_params->dest_ent_idx + n_dirty_ents_in_seg) * sizeof(*dest_jmdc) >
		cmd_params->jmdc_get.jmdc_sink.local.size) {
		_NE(error_1_serjio_gen_cmd_handlers_gen_get_jmdc, "NVMEIB_GET_JMDC - entries data buffer is too small");
		rv = -ENOMEM;
		goto out;
	}

	for (i = 0; i < num_entries; i++) {
		int j;
		bool is_dirty = test_bit(i, dirty_ents_in_seg_bmp);
		union jblock_md *jmdc_ent = (*get_jmdc_ent_fn)(rng_handle, i);
		if (is_dirty) {
			rng_data->num_dirty_ents++;
			BUG_ON(!nvmeib_is_jmd_io_entry(jmdc_ent[0]));
		}
		if (!rng_data->only_dirty_ents || is_dirty) {
			*dest_ent_md = ent_md[i];
			dest_ent_md++;
			for (j = 0; j < (int)rng_data->binje; ++j) {
				BUG_ON(dest_jmdc_miter->consumed > dest_jmdc_miter->length);
				if (dest_jmdc_miter->consumed == dest_jmdc_miter->length) {
					/* Either both miter.consumed and miter.length are 0 (not mapped yet) or
					 * miter.consumed == miter.length (mapping is now full)
					 */
					sg_miter_next(dest_jmdc_miter);
					/* sg_miter_next sets miter.consumed to miter.length assuming that the 
					 * default use-case is just to fill the whole mapped area.
					 * Instead we are filling it up sizeof(*dest_jmdc) bytes at a time,
					 * so we initialise it to 0 and then increase it until it reaches miter.length */
					dest_jmdc_miter->consumed = 0;
				}
				dest_jmdc = dest_jmdc_miter->addr + dest_jmdc_miter->consumed;
				dest_jmdc->raw = jmdc_ent[j].raw;
				dest_jmdc_miter->consumed += sizeof(*dest_jmdc);
				fn_params->dest_ent_block_idx++;
				fn_params->ent_block_cnt++;
			}
			fn_params->dest_ent_idx++;
			fn_params->ent_cnt++;
		}
	}

out:
	NFOUT;
	return rv;
}


int nvmeibs_serjio_gen_cmd_handle_get_jmdc_op(struct nvmeibs_disk_info *di,
				   const struct nvmeib_gen_cmd_param *p, union nvmeib_gen_cmd_rsp *rsp)
{
	int rv;
	struct gen_get_jmdc_params get_jmdc_params = {0};
	NFIN;
	get_jmdc_params.p = p;
	get_jmdc_params.rsp = rsp;
	
	/* Memsets the mapping-iterator (miter) and sets up the internal page-iterator
	 * miter.consumed == miter.length = 0 and miter.addr == NULL after this
	 */
	sg_miter_start(&get_jmdc_params.dest_jmdc_miter, 
		       get_jmdc_params.p->jmdc_get.jmdc_sink.local.sgt.sgl, 
			get_jmdc_params.p->jmdc_get.jmdc_sink.local.sgt.nents,
		       SG_MITER_TO_SG);
	/* Skips through the page-iterator by the offset, but still doesn't map anything.
	 * miter.consumed == miter.length = 0 and miter.addr == NULL after this also
	 */
	sg_miter_skip(&get_jmdc_params.dest_jmdc_miter,
		      get_jmdc_params.p->jmdc_get.jmdc_sink.local.offset);
	if (p->jmdc_get.get_by_client_uuid) {
		rv = nvmeibs_serjio_call_for_client_range(
			di, p->jmdc_get.client_uuid, NVMEIB_EC_INVALID_JOURNAL_BINJE,
			 p->jmdc_get.seg_uuid, gen_get_jmdc, &get_jmdc_params);
	} else {
		rv = nvmeibs_serjio_call_for_each_assigned_range(di, p->jmdc_get.start_rng,
			p->jmdc_get.num_rng, p->jmdc_get.seg_uuid,
			gen_get_jmdc, &get_jmdc_params);
	}
	sg_miter_stop(&get_jmdc_params.dest_jmdc_miter);

	nvmeibs_serjio_get_jrnl_clsects(di, &rsp->jmdc_get.jrnl_data.jrnl_start_lba,
									&rsp->jmdc_get.jrnl_data.jrnl_len_lba);

	memcpy(rsp->jmdc_get.jrnl_data.serjio_boot_id,
		   nvmeibs_serjio_get_boot_id(di), NVMEIB_GID_STR_MAX);
	rsp->jmdc_get.jrnl_data.num_ents = get_jmdc_params.ent_cnt;
	rsp->jmdc_get.jrnl_data.lba_shift = nvmeibs_disk_info_get_block_shift(di);
	rsp->jmdc_get.jrnl_data.num_ents_rng = NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE; /* For v2.3, this is max */
	rsp->jmdc_get.jrnl_data.num_dirty_rng = (u16)get_jmdc_params.dirty_rng_cnt;
	rsp->jmdc_get.jrnl_data.num_rng = (u16)get_jmdc_params.rng_cnt;
	rsp->jmdc_get.jrnl_data.num_ent_blocks = get_jmdc_params.ent_block_cnt;
	rsp->jmdc_get.hdr_len = get_jmdc_params.rng_cnt * get_jmdc_params.rng_data_serialized_sz;
	rsp->jmdc_get.ents_md_len = get_jmdc_params.ent_cnt * sizeof(struct nvmeib_jrnl_ent_md);
	rsp->jmdc_get.ents_len = get_jmdc_params.ent_block_cnt * sizeof(union jblock_md);
	NFOUT;
	return rv;
}
