#include "common/kr_incs.h"
#include "block/datapath_utils_generic/nvmeibc_block_dp_defs.h"
#include "common/nvmeib_shared.h"
#include "nvmeib_version_shared.h"
#include "common/nvmeib_volume_type.h"
#include "common/nvmeib_str.h"
#include "nvmeibc_management_capi_parse_conf.h"
#include "nvmeibc_management_volume_conf_checks.h"

// The following functions are used to free an allocated configuration message
static void __free_target_config(struct nvmeibc_target_conf *msg)
{
	if (msg->n_disks) {
		kfree(msg->disks);
		msg->n_disks = 0;
		msg->disks = NULL;
	}
	if (msg->n_nics) {
		kfree(msg->nics);
		msg->n_nics = 0;
		msg->nics = NULL;
	}
}

static void __free_praid_conf(struct nvmeibc_praid_conf *msg)
{
	if (msg->n_segments) {
		kfree(msg->segments);
		msg->n_segments = 0;
		msg->segments = NULL;
	}
}

static void __free_chunk_conf(struct nvmeibc_chunk_conf *msg)
{
	int i;
	for (i=0;i<msg->n_praids;i++) {
		__free_praid_conf(&msg->praids[i]);
	}
	kfree(msg->praids);
	msg->n_praids = 0;
	msg->praids = 0;
}

static void __free_volume_config(struct nvmeibc_volume_conf *msg)
{
	int i;
	for (i=0;i<msg->n_chunks;i++) {
		__free_chunk_conf(&msg->chunks[i]);
	}
	kfree(msg->chunks);
	msg->n_chunks = 0;
	msg->chunks = NULL;
	kfree(msg->attachment.referenceIDs);
	msg->attachment.referenceIDs = NULL;
}

void nvmeibc_cc_api_free_config_msg(void *message)
{
	int i;
	struct nvmeib_mgmt_to_client_volume_configuration *msg = message;
	for (i = 0; i < msg->n_volumes; i++) {
		__free_volume_config(&msg->volumes[i]);
	}
	for (i=0;i<msg->n_targets;i++) {
		__free_target_config(&msg->targets[i]);
	}
	kfree(msg->volumes);
	msg->n_volumes = 0;
	msg->volumes = NULL;
	kfree(msg->targets);
	msg->n_targets = 0;
	msg->targets = NULL;
}

// The following functions are used to copy a single volume config from the
// MCS message to be passed on to setup_block_device_generic
static void __copy_nvmeibc_segment_conf(struct nvmeibc_segment_conf *dst,
										const struct nvmeibc_segment_conf *src)
{
	*dst = *src;
}

static void __copy_nvmeibc_nic_conf(struct nvmeibc_nic_conf *dst,
									const struct nvmeibc_nic_conf *src)
{
	*dst = *src;
}

static void __copy_nvmeibc_disk_conf(struct nvmeibc_disk_conf *dst,
									 const struct nvmeibc_disk_conf *src)
{
	*dst = *src;
}
//According to my (Roman) understanding, a segment status has 2 values: [n]ormal and [d]epr (ecated).
//For some reason, when we create the volume configuration copy, we get rid of deprecated segment
//It means that praid->dataBlocks + praid->numberOfMirrors + praid->parityBlocks != praid->n_segments
#define use_seg(seg) (((seg).status[0] == 'n') && ((seg).type == TYPE_DATA || (seg).type == TYPE_PARITY || (seg).type == TYPE_BOTH))

// Client only copies "normal" segments, the return code is the number of
// normal segs, since we filter out deprecated and raftonly segments in python
// finding one of them is considered an error
static int __copy_nvmeibc_praid_conf(struct nvmeibc_praid_conf *dst,
									 const struct nvmeibc_praid_conf *src)
{
	int i, j, n_segs = 0;
	*dst = *src;
	dst->segments = NULL;
	for (i=0;i<src->n_segments;i++) {	// Count how many segments to use (Do not copy depricated segments)
		if (use_seg(src->segments[i]))  // vcg@"segments status is important for segments counting - there is a hidden magic"
			n_segs++;
	}
	if (n_segs) {
		dst->segments = kzalloc(sizeof(*dst->segments) * n_segs, GFP_KERNEL);
		if (!dst->segments) {
			_NT(t18cnmtc, "Could not allocate segments in volume message");
			return -ENOMEM;
		}
	}
	dst->n_segments = n_segs;
	for (i=0,j=0; (i < src->n_segments) && (j < n_segs); i++) {
		const struct nvmeibc_segment_conf *seg = &src->segments[i];
		if (use_seg(*seg)) {
			_NT(t19cnmtc, "Copying segment @SEGMENT_UUID its status is @STATUS_STR", seg->uuid, seg->status);
			__copy_nvmeibc_segment_conf(&dst->segments[j++], seg);
		} else {
			_NT(t16cnmtc, "Filter  segment @SEGMENT_UUID skip status is @STATUS_STR", seg->uuid, seg->status);
		}
	}
	BUG_ON(j!=n_segs);
	return n_segs;
}

static int __copy_nvmeibc_chunk_conf(struct nvmeibc_chunk_conf *dst,
									 const struct nvmeibc_chunk_conf *src)
{
	int i, total_number_of_segments = 0;
	*dst = *src;
	dst->praids = NULL;
	if (src->n_praids) {
		dst->praids = kzalloc(sizeof(*dst->praids) * src->n_praids, GFP_KERNEL);
		if (!dst->praids) {
			_NT(t12cnmtc, "Could not allocate protection raids in volume message");
			return -ENOMEM;
		}
	}
	dst->n_praids = src->n_praids;
	for (i=0;i<src->n_praids;i++) {
		const int tmp_nsegs = __copy_nvmeibc_praid_conf(&dst->praids[i], &src->praids[i]);
		const int tmp_calc = dst->praids[i].numberOfMirrors + dst->praids[i].dataBlocks + dst->praids[i].parityBlocks;
		if (tmp_nsegs < 0) {
			kfree(dst->praids);
			return -1;
		}
		total_number_of_segments += tmp_nsegs;
		if (tmp_calc != tmp_nsegs) { // vcfg@"verify the number of segments match the definition"
			_NT(t13cnmtc, "Praid praid_idx=@PRAID_IDX is different than conf. nsegs conf: @TMP_CALC, praid: @TMP_NSEGS", i, tmp_calc, tmp_nsegs);
		}
	}
	return total_number_of_segments;
}

// We copy chunk by chunk by counting the total number of segments
// which is then returned
static int __copy_nvmeibc_volume_conf(struct nvmeibc_volume_conf *dst,
									  const struct nvmeibc_volume_conf *src)
{
	int i, total_number_of_segments=0;
	int		n_segs;
	*dst = *src;

	if (unlikely(!__is_vol_type_valid(dst->type))) { //vcfg@"check volume type is legal"
		WARN(true, "%s: mgmt bug, volume type=0x%x is invalid", dst->name, dst->type);
		return -EINVAL;
	}
	dst->chunks = NULL;
	if (src->n_chunks) {
		dst->chunks = kzalloc(sizeof(*dst->chunks) * src->n_chunks, GFP_KERNEL);
		if (!dst->chunks) {
			_NT(t10cnmtc, "Could not allocate chunks in volume message");
			return -ENOMEM;
		}
	}
	for (i=0;i<src->n_chunks;i++) {
		n_segs = __copy_nvmeibc_chunk_conf(&dst->chunks[i], &src->chunks[i]);
		if (n_segs < 0) {
			kfree(dst->chunks);
			dst->n_chunks = 0;
			dst->chunks = NULL;
			return -1;			// NVMESH-4845: mem leak of praids/segs in prev chunk
		}
		total_number_of_segments += n_segs;
	}
	if (src->attachment.n_ref_ids > 0) {
		const int n_bytes = (int)(src->attachment.n_ref_ids * sizeof(*src->attachment.referenceIDs));	// nvmeibc_volume_ext_blob_size(src->attachment.n_ref_ids)
		dst->attachment.referenceIDs = (struct nvmeibc_reference_id *)kmalloc(n_bytes, GFP_KERNEL);
		if (!dst->attachment.referenceIDs)
			return -ENOMEM;		// NVMESH-4845: mem leak of praids/segs/chunks
		memcpy(dst->attachment.referenceIDs, src->attachment.referenceIDs, n_bytes);
	}
	return total_number_of_segments;
}

static int __copy_nvmeibc_target_conf_no_fliter(struct nvmeibc_target_conf *dst, const struct nvmeibc_target_conf *src)
{
	int i, rv = 0;
	*dst = *src;
	dst->nics = NULL;
	dst->disks = NULL;
	if (src->n_nics) {
		dst->nics = kzalloc(sizeof(*dst->nics) * src->n_nics, GFP_KERNEL);
		if (!dst->nics) {
			_NT(error_cc_api_copy_nvmeibc_target_conf_no_fliter, "Could not allocate volume message");
			rv = -ENOMEM;
			goto _out;
		}
	}
	for (i = 0; i < src->n_nics; i++) {
		__copy_nvmeibc_nic_conf(&dst->nics[i], &src->nics[i]);
	}
	if (src->n_disks) {
		dst->disks = kzalloc(sizeof(*dst->disks) * src->n_disks, GFP_KERNEL);
		if (!dst->disks) {
			_NT(error_1_cc_api_copy_nvmeibc_target_conf_no_fliter, "Could not allocate volume message");
			rv = -ENOMEM;
			goto _out;
		}
	}
	for (i=0;i<src->n_disks;i++) {
		__copy_nvmeibc_disk_conf(&dst->disks[i], &src->disks[i]);
	}
_out:
	if (rv < 0) {
		kfree(dst->nics);
		kfree(dst->disks);
	}
	return rv;
}

// Copies all normal segments into a copy of the message, then sorts all unique
// disks and targets and copies only the required ones thus the dst message
// is an exact minimal message with only relevant segments targets
// and only relevent disks on these targets
int __copy_nvmeib_mgmt_to_client_volume_config(
	      struct nvmeib_mgmt_to_client_volume_configuration *dst,   // 1 result volume
	const struct nvmeib_mgmt_to_client_volume_configuration  src[], // array from which 'vol_i' is extracted
	const int vol_i)
{
	int rv=0, i, n_segs;
	NFIN;

	*dst = *src;
	dst->n_volumes = 0;
	dst->volumes = NULL;
	dst->n_targets = 0;
	dst->targets = NULL;
	dst->volumes = kzalloc(sizeof(*dst->volumes), GFP_KERNEL);

	if (!dst->volumes) {
		_NT(t01cnmtc, "Could not allocate volume message");
		rv = -ENOMEM;
		goto _out;
	}
	dst->n_volumes = 1;

	// One volume at a time
	n_segs = __copy_nvmeibc_volume_conf(dst->volumes, &src->volumes[vol_i]);
	if (!n_segs) { // We allow an empty volume
		//vcfg@"volume may have 0 segments"
		_NT(t02cnmtc, "Volume configuration has no segments, no targets will be added");
		goto _out;
	}

	if (n_segs < 0) { //the error was reported by the previous function
		rv = n_segs;
		goto _out;
	}

	// vcfg@"assume - the targets are unique - python mcs script responsibility"
	if (src->n_targets) {
		dst->targets = kzalloc(sizeof(*dst->targets) * src->n_targets,
							   GFP_KERNEL);
		if (!dst->targets) {
			_NT(t03cnmtc, "Could not allocate volume message");
			rv = -ENOMEM;
			goto _out;
		}
		dst->n_targets = src->n_targets;
	}	// Copy all targets without filtering
	for (i=0;i<src->n_targets;i++) {
		rv = __copy_nvmeibc_target_conf_no_fliter(&dst->targets[i],
												  &src->targets[i]);
	}
_out:
	NFOUT;
	return rv;
}

struct nvmeib_mgmt_to_client_update_targets_nics *
nvmeib_mgmt_to_client_update_targets_nics_clone( const struct nvmeib_mgmt_to_client_update_targets_nics *src)
{
	int rv=0, i;

	struct nvmeib_mgmt_to_client_update_targets_nics* dst = 0;
	NFIN;

	dst = kzalloc(sizeof(*dst), GFP_KERNEL);
	if (!dst){
		_NT(t00cnmt_utn, "Could not allocate get_target_nics");
		rv = -ENOMEM;
		goto _out;
	}

	*dst = *src;
	dst->n_targets = 0;
	dst->targets = NULL;
	if (src->n_targets) {
		dst->targets = kzalloc(sizeof(*dst->targets) * src->n_targets, GFP_KERNEL);
		if (!dst->targets) {
			_NT(t01cnmt_utn, "Could not allocate targets");
			rv = -ENOMEM;
			goto _out;
		}
		dst->n_targets = src->n_targets;
	}

	for (i=0;i<src->n_targets;i++) {
		rv = __copy_nvmeibc_target_conf_no_fliter(&dst->targets[i], &src->targets[i]);
		if (rv){
			_NT(t02cnmt_utn, "Could not allocate get_target_nics");
			goto _out;
		}
	}
_out:
	if (rv){
		nvmeibc_cc_api_free_update_targets_nics(dst);
		dst = NULL;
	}
	NFOUT;
	return dst;
}

void nvmeibc_cc_api_free_update_targets_nics(struct nvmeib_mgmt_to_client_update_targets_nics *msg)
{
	int i = 0;
	if (!msg) {
		return;
	}

	for (i=0;i<msg->n_targets;i++) {
		__free_target_config(&msg->targets[i]);
	}
	kfree(msg->targets);
	msg->n_targets = 0;
	msg->targets = NULL;
	kfree(msg);
}

static inline bool __is_rva_in_range(void *rva, u32 unit_size, void *rva_start, void *rva_end)
{
	return (void *)rva >= (void *)rva_start && (void *)rva < (void *)rva_end && (void *)rva + unit_size <= (void *)rva_end;
}

/** Convert RVA (relative virtual address from base) to typed pointer. Use when deserializing config from a flat buffer. */
#define NVMEIBC_MGMT_RVA_TO_PTR(base, rva) ((__typeof__(rva))(base + (uint64_t)(rva)))
#define NVMEIBC_MGMT_PTR_ASSIGN_OR_RETURN(lhs, rexpr, unit_size, rva_start, rva_end)                              \
	({                                                                                                        \
		__auto_type _tmp = (rexpr);                                                                       \
		if (!__is_rva_in_range(_tmp, unit_size, rva_start, rva_end)) {                                    \
			_NE(__AUTOID__, "RVA local_cl validation: _tmp=@PTR unit_size=@UINT start=@PTR end=@PTR", \
			    _tmp, unit_size, rva_start, rva_end);                                                 \
			return -EINVAL;                                                                           \
		}                                                                                                 \
		(lhs) = _tmp;                                                                                     \
	})

int nvmeibc_setup_volume_configuration(struct nvmeib_mgmt_to_client_volume_configuration *conf, size_t serialized_config_len)
{
	int								i, j, k;
	void							*base_ptr = conf, *rva_end = (u8*)conf + serialized_config_len;
	struct nvmeibc_volume_conf		*vol;
	struct nvmeibc_chunk_conf		*chunk;
	struct nvmeibc_praid_conf		*praid;
	struct nvmeibc_target_conf		*target;

	NVMEIBC_MGMT_PTR_ASSIGN_OR_RETURN(conf->volumes, NVMEIBC_MGMT_RVA_TO_PTR(base_ptr, conf->volumes), sizeof(struct nvmeibc_volume_conf), base_ptr, rva_end);
	_NT(rvasukw, "local_cl n_volumes=@INT conf->volumes=@PTR", conf->n_volumes, conf->volumes);
	for (i = 0; i < conf->n_volumes; i++) {	// A single message for each volume released after attach attempt
		vol = &(conf->volumes[i]);
		NVMEIBC_MGMT_PTR_ASSIGN_OR_RETURN(vol->chunks, NVMEIBC_MGMT_RVA_TO_PTR(base_ptr, vol->chunks), sizeof(struct nvmeibc_chunk_conf), base_ptr, rva_end);
		_NT(6xmig5a, "local_cl vol=@STR n_chunks=@INT vol->chunks=@PTR", vol->name, vol->n_chunks, vol->chunks);
		for (j = 0; j < vol->n_chunks; j++) {
			chunk = &(vol->chunks[j]);
			NVMEIBC_MGMT_PTR_ASSIGN_OR_RETURN(chunk->praids, NVMEIBC_MGMT_RVA_TO_PTR(base_ptr, chunk->praids), sizeof(struct nvmeibc_praid_conf), base_ptr, rva_end);
			_NT(vt83k3c, "local_cl chunk=@STR n_praids=@INT chunk->praids=@PTR", chunk->uuid, chunk->n_praids, chunk->praids);
			for (k = 0; k < chunk->n_praids; k++) {
				praid = &(chunk->praids[k]);
				NVMEIBC_MGMT_PTR_ASSIGN_OR_RETURN(praid->segments, NVMEIBC_MGMT_RVA_TO_PTR(base_ptr, praid->segments), sizeof(struct nvmeibc_segment_conf), base_ptr, rva_end);
				_NT(vkw02ap3, "local_cl praid=@STR n_segments=@INT praid->segments=@PTR", praid->uuid, praid->n_segments, praid->segments);
			}
		}
	}
	NVMEIBC_MGMT_PTR_ASSIGN_OR_RETURN(conf->targets, NVMEIBC_MGMT_RVA_TO_PTR(base_ptr, conf->targets), sizeof(struct nvmeibc_target_conf), base_ptr, rva_end);
	_NT(03j8els, "local_cl n_targets=@INT conf->targets=@PTR", conf->n_targets, conf->targets);
	for (i = 0; i < conf->n_targets; i++) {
		target = &(conf->targets[i]);
		NVMEIBC_MGMT_PTR_ASSIGN_OR_RETURN(target->disks, NVMEIBC_MGMT_RVA_TO_PTR(base_ptr, target->disks), sizeof(struct nvmeibc_disk_conf), base_ptr, rva_end);
		NVMEIBC_MGMT_PTR_ASSIGN_OR_RETURN(target->nics, NVMEIBC_MGMT_RVA_TO_PTR(base_ptr, target->nics), sizeof(struct nvmeibc_nic_conf), base_ptr, rva_end);
		_NT(unrnsj7, "local_cl target=@STR n_disks=@INT n_nics=@INT target->nics=@PTR target->disks=@PTR", target->node_id, target->n_disks, target->n_nics, target->nics, target->disks);
	}

	return 0;
}
