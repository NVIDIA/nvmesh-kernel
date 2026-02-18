#include <fcntl.h>
#include <sys/stat.h>
#include "nvmeibt_debug.h"
#include "../common/nvmeib_shared.h"
#include "nvmeibt_common.h"
#include "nvmeibt_global.h"
#include "nvmeibt_praid_basics.h"
#include "nvmeibt_ds.h"
#include "../common/nvmeib_hash.h"
#include "nvmeibt_mm_json.h"

// forward compatibility. If updating the structs, add an unpack handler for the old struct version.
// #define MM_STRUCT_VER_2008 2008
// #define MM_STRUCT_VER_2012 2012		// ELECT
#define MM_STRUCT_VER_2067 2067		// Kafka

static void _mm_uuid_binary_to_str(const union nvmeib_uuid *uuid, char *s)
{
	nvmeibt_union_uuid_to_urn_uuid_in_place(uuid, s);
}

static void _mm_seg_from_json(struct mm_segment_conf *seg, struct mm_json_elem *elem)
{
	struct mm_json_kv_pair		*kv;
	struct mm_json_dict			*dict = &(elem->dict);
	char						*s;
	JSON_ASSIGN_AND_CALL_INIT();

	NFIN;
	memset(seg, 0, sizeof(struct mm_segment_conf));
	if (elem->type != JSON_E_DICT)
		return;
	nvmeibt_strlcpy(seg->eyecatcher, "SEG", sizeof(seg->eyecatcher));
	seg->action = 'N';
	seg->type = 1;
	JSON_LOOP_FOR_DICT(kv, dict) {
		JSON_LOOP_ITERATION_START(vbhduie, kv->key);
		s = kv->value->str;
		JSON_ASSIGN_PLAIN(yejd93l, "lbs", seg->lbs, kv->value->num);
		JSON_ASSIGN_PLAIN(basjhd9, "lbe", seg->lbe, kv->value->num);
		JSON_ASSIGN_PLAIN(63bd9ls, "uuid", seg->uuid, *GET_UNION_UUID_OF_URN_UUID_STR(s));
		JSON_ASSIGN_PLAIN(vunsoen, "diskUUID", seg->diskUUID, *GET_UNION_UUID_OF_URN_UUID_STR(s));
		JSON_ASSIGN_PLAIN(pebdtye, "pRaidIndex", seg->pRaidIndex, kv->value->num);
		JSON_ASSIGN_PLAIN(b7hw6xk, "pRaidTypeIndex", seg->pRaidTypeIndex, kv->value->num);
		JSON_ASSIGN_VALIDATE_STR(2cgks9l, "type", "data", s);
		JSON_ASSIGN_PLAIN(aczgje0, "status", seg->action,
			((!strcmp(s, "markedForRebuild_old") || !strcmp(s, "R")) ? 'R' :
			 (!strcmp(s, "markedForRebuild") || !strcmp(s, "replacement") || !strcmp(s, "S")) ? 'S' :
			 (!strcmp(s, "X")) ? 'X' :
			 (!strcmp(s, "normal") || !strcmp(s, "initializing") || !strcmp(s, "booting") ||
			  !strcmp(s, "zeroing") || !strcmp(s, "dead") || !strcmp(s, "under_recovery") || !strcmp(s, "remap") || !strcmp(s, "N")) ? 'N' :
			 -1));	// Default. Worth a warning
		JSON_WARN_and_FIX(cbhsukw, "status", seg->action, -1, 'N', "key=@STR val=@STR", kv->key, s);
/*
		JSON_ASSIGN_OPTIONAL(cvs5kw0, "_id");
		JSON_ASSIGN_OPTIONAL(cvgekww, "diskID");
		JSON_ASSIGN_OPTIONAL(cbhslw0, "nodeUUID");
		JSON_ASSIGN_OPTIONAL(1cfdjs9, "node_id");
		JSON_ASSIGN_OPTIONAL(kmjd783, "volumeName");
		JSON_ASSIGN_OPTIONAL(d4j40ls, "volumeUUID");
		JSON_ASSIGN_OPTIONAL(bcixkea, "pRaidUUID");
		JSON_ASSIGN_OPTIONAL(c46s0ki, "allocationIndex");
		JSON_ASSIGN_OPTIONAL(syke0ld, "zone");
		JSON_ASSIGN_OPTIONAL(nviskrw, "redundancyRatio");
*/
		JSON_ASSIGN_VALIDATE_STR_OPTIONAL(2hdiyx7, "eyecatcher", "SEG", kv->value->str);	// Exists in persistence->JSON
		JSON_LOOP_ITERATION_END(vgrj6du, kv->key);
	}
	JSON_ASSIGN_AND_CALL_VALIDATE(uf5jri3);
	NFOUT;
}

static void _mm_praid_segs_from_json(struct mm_praid_conf *praid, struct mm_json_elem *segs_array_json)
{
	int i;
	if (segs_array_json->type != JSON_E_ARRAY)
		return;
	praid->num_segments = segs_array_json->array.len;
	praid->segments = (struct mm_segment_conf *)NNVMEIBT_BM_CALLOC(vgsjw82,  sizeof(struct mm_segment_conf) * praid->num_segments);
	for (i = 0; i < praid->num_segments; i++)
		_mm_seg_from_json(&praid->segments[i], segs_array_json->array.elements[i]);
}

static void _mm_praid_from_json(struct mm_praid_conf *praid, struct mm_json_elem *elem)
{
	struct mm_json_kv_pair		*kv;
	struct mm_json_dict			*dict = &(elem->dict);
	JSON_ASSIGN_AND_CALL_INIT();

	NFIN;
	memset(praid, 0, sizeof(struct mm_praid_conf)); //including praid->version = 0;
	if (elem->type != JSON_E_DICT)
		return;
	nvmeibt_strlcpy(praid->eyecatcher, "PRD", sizeof(praid->eyecatcher));
	JSON_LOOP_FOR_DICT(kv, dict) {
		JSON_LOOP_ITERATION_START(fvs73k5, kv->key);
		JSON_ASSIGN_CALL(nfi84ol, "diskSegments", _mm_praid_segs_from_json, praid, kv->value);
		JSON_ASSIGN_PLAIN(vbjsol3, "uuid", praid->uuid, *GET_UNION_UUID_OF_URN_UUID_STR(kv->value->str));
		JSON_ASSIGN_PLAIN(6gdkl2k, "activated", praid->activated, kv->value->num);
		JSON_ASSIGN_PLAIN(mrubnfs, "stripeIndex", praid->stripeIndex, kv->value->num);
		JSON_ASSIGN_VALIDATE_STR_OPTIONAL(28jf0kx, "eyecatcher", "PRD", kv->value->str);	// Exists in persistence->JSON
		JSON_ASSIGN_OPTIONAL(28jf0kx, "zone");
		JSON_ASSIGN_PLAIN_OPTIONAL(fzwmqug, "version", praid->version, kv->value->num);	// Exists in persistence->JSON
		JSON_LOOP_ITERATION_END(4vys872, kv->key);
	}
	JSON_ASSIGN_AND_CALL_VALIDATE(iah48fw);
	NFOUT;
}

static void _mm_chunk_praids_from_json(struct mm_chunk_conf *chunk, struct mm_json_elem *praid_arr_json)
{
	int i;
	if (praid_arr_json->type != JSON_E_ARRAY)
		return;
	chunk->num_praids = praid_arr_json->array.len;
	chunk->praids = (struct mm_praid_conf *)NNVMEIBT_BM_CALLOC(vbdh30h, sizeof(struct mm_praid_conf) * chunk->num_praids);
	for (i = 0; i < chunk->num_praids; i++)
		_mm_praid_from_json(&chunk->praids[i], praid_arr_json->array.elements[i]);
}

static void _mm_chunk_from_json(struct mm_chunk_conf *chunk, struct mm_json_elem *elem)
{
	struct mm_json_kv_pair		*kv;
	struct mm_json_dict			*dict = &(elem->dict);
	JSON_ASSIGN_AND_CALL_INIT();

	NFIN;
	memset(chunk, 0, sizeof(struct mm_chunk_conf));
	if (elem->type != JSON_E_DICT)
		return;
	nvmeibt_strlcpy(chunk->eyecatcher, "CHK", sizeof(chunk->eyecatcher));
	JSON_LOOP_FOR_DICT(kv, dict) {
		JSON_LOOP_ITERATION_START(t3i0dli, kv->key);
		JSON_ASSIGN_PLAIN(siemuve, "uuid", chunk->uuid, *GET_UNION_UUID_OF_URN_UUID_STR(kv->value->str));
		JSON_ASSIGN_PLAIN(bijdkep, "vlbs", chunk->vlbs, kv->value->num);
		JSON_ASSIGN_PLAIN(4val4op, "vlbe", chunk->vlbe, kv->value->num);
		JSON_ASSIGN_CALL(vnjsl3o, "pRaids", _mm_chunk_praids_from_json, chunk, kv->value);
		JSON_ASSIGN_VALIDATE_STR_OPTIONAL(29jdkla, "eyecatcher", "CHK", kv->value->str);	// Exists in persistence->JSON
		JSON_LOOP_ITERATION_END(0cbjsiw, kv->key);
	}
	JSON_ASSIGN_AND_CALL_VALIDATE(qiar6bf);
	NFOUT;
}

static void _mm_vol_chunks_from_json(struct mm_vol_conf *vol, struct mm_json_elem *chunks_arr_json)
{
	int		i;

	if (chunks_arr_json->type != JSON_E_ARRAY)
		return;
	vol->num_chunks = chunks_arr_json->array.len;
	vol->chunks = (struct mm_chunk_conf *)NNVMEIBT_BM_CALLOC(vbhdj39, sizeof(struct mm_chunk_conf) * vol->num_chunks);
	for (i = 0; i < vol->num_chunks; i++) {
		_mm_chunk_from_json(&vol->chunks[i], chunks_arr_json->array.elements[i]);
	}
}

static void _mm_lockserver_type_from_json(struct mm_vol_conf *vol, struct mm_json_elem *elem)
{
	struct mm_json_kv_pair		*kv;
	struct mm_json_dict			*dict = &(elem->dict);
	JSON_ASSIGN_AND_CALL_INIT();

	NFIN;
	if (elem->type != JSON_E_DICT)
		return;
	JSON_LOOP_FOR_DICT(kv, dict) {
		JSON_LOOP_ITERATION_START(jdukeos, kv->key);
		JSON_ASSIGN_PLAIN(6bdoles, "type", vol->lockServer_type, kv->value->num);
		JSON_ASSIGN_PLAIN(ogjdn2e, "maxNOwners", vol->lockServer_maxNOwners, kv->value->num);
		JSON_ASSIGN_PLAIN(wfrmvx0, "locksetShift", vol->lockServer_locksetShift, kv->value->num);
		JSON_LOOP_ITERATION_END(ybg0l3d, kv->key);
	}
	JSON_ASSIGN_AND_CALL_VALIDATE(gnkd04l);
	NFOUT;
}

static void _mm_vol_from_json(struct mm_vol_conf *vol, struct mm_json_elem *elem, int64_t kafka_offset, bool is_new_or_upd, bool is_deleteVolumeCompleted)
{
	struct mm_json_kv_pair		*kv;
	struct mm_json_dict			*dict = &(elem->dict);
	char						*s;
	JSON_ASSIGN_AND_CALL_INIT();

	NFIN;
	memset(vol, 0, sizeof(struct mm_vol_conf));
	vol->stripeWidth = 1;		// default, as it could be null
	vol->kafka_offset_or_idx = kafka_offset;		// Default, If arrives from Kafka then use it. From JSON file it is overriden
	nvmeibt_strlcpy(vol->eyecatcher, "VOL", sizeof(vol->eyecatcher));
	if (elem->type != JSON_E_DICT)
		return;
	JSON_LOOP_FOR_DICT(kv, dict) {
		JSON_LOOP_ITERATION_START(uskt4le, kv->key);
		s = kv->value->str;
		if (kv->value->type == JSON_E_NULL) {
			N_Tf(csurm2m, "Skipping key @STR. value=null", kv->key);
			continue;
		}
		JSON_ASSIGN_STR(4nid0f0, "name", vol->name, s);
		JSON_ASSIGN_PLAIN(xmidk25, "uuid", vol->uuid, *GET_UNION_UUID_OF_URN_UUID_STR(s));
		if (!is_deleteVolumeCompleted) {
			JSON_ASSIGN_PLAIN(umgk3o0, "version", vol->version, kv->value->num);
		}
		if (is_new_or_upd) {
			JSON_ASSIGN_OPTIONAL(iv4ghs8, "numberOfMirrors");
			JSON_ASSIGN_OPTIONAL(skq1v0b, "status");
			JSON_ASSIGN_OPTIONAL(laozrnf, "reservation");
			JSON_ASSIGN_OPTIONAL(5g3nauf, "_id");		// Identical to name
			JSON_ASSIGN_PLAIN(bvnsjkf, "blocks", vol->blocks, kv->value->num);
			JSON_ASSIGN_PLAIN(kxme9j5, "blockSize", vol->blockSize, kv->value->num);
			JSON_ASSIGN_PLAIN(cujs03p, "relativeRebuildPriority", vol->relativeRebuildPriority, kv->value->num);
			JSON_ASSIGN_PLAIN_OPTIONAL(og7xne3, "enableCrcCheck", vol->enableCrcCheck, kv->value->num);
			JSON_ASSIGN_PLAIN_OPTIONAL(bnmc903, "use_debug_di", vol->use_debug_di, kv->value->num);
			JSON_ASSIGN_PLAIN_OPTIONAL(byxbdoe, "stripeWidth", vol->stripeWidth, kv->value->num);	// MGMT sends depending on raidType (Dec 24)
			JSON_ASSIGN_PLAIN_OPTIONAL(92locla, "stripeSize", vol->stripeSize, kv->value->num);
			JSON_ASSIGN_PLAIN_OPTIONAL(bnjkx93, "kafka_offset_or_idx", vol->kafka_offset_or_idx, kv->value->num);	// Exists in persistence->JSON
			JSON_ASSIGN_PLAIN(7xj30ls, "action", vol->action, ((!strcmp(s, "markedForDeletion") || s[0] == 'X') ? 'X' : 'N'));
			JSON_ASSIGN_PLAIN(zkw94j2, "RAIDLevel", vol->raidType, (!strcmp(s, "Mirrored RAID-1") ? 1 :
															 !strcmp(s, "Striped RAID-0") ? 0 :
															 !strcmp(s, "Concatenated") ? 0 :
															 !strcmp(s, "Striped & Mirrored RAID-10") ? 1 :
															 !strcmp(s, "Erasure Coding") ? 6 :
															 !strcmp(s, "ELECT") ? 6 : -1));
			JSON_WARN_and_FIX(vtshwis, "RAIDLevel", vol->raidType,  (typeof(vol->raidType))-1, -1, "key=@STR val=@STR", kv->key, s);
			JSON_ASSIGN_CALL(vimrkts, "chunks", _mm_vol_chunks_from_json, vol, kv->value);
			JSON_ASSIGN_CALL(1nis0xa, "lockServer", _mm_lockserver_type_from_json, vol, kv->value);
			JSON_ASSIGN_VALIDATE_STR_OPTIONAL(hsk0xmr, "eyecatcher", "VOL", s);	// Exists in persistence->JSON
			JSON_ASSIGN_OPTIONAL(0ecdaun, "dataBlocks");	// MGMT sends depending on raidType (Dec 24)
			JSON_ASSIGN_OPTIONAL(4bs8o4e, "parityBlocks");	// MGMT sends depending on raidType (Dec 24)
			JSON_LOOP_ITERATION_END(4gt67sk, kv->key);
		}
	}
	JSON_ASSIGN_AND_CALL_VALIDATE(rvh39al);
	NFOUT;
}

static void _HW_nic_from_json(struct mm_nic_conf *nic, struct mm_json_elem *elem)
{
	struct mm_json_kv_pair		*kv;
	struct mm_json_dict			*dict = &(elem->dict);
	char						*s;
	JSON_ASSIGN_AND_CALL_INIT();

//	NFIN;
	if (elem->type != JSON_E_DICT)
		return;
	nvmeibt_strlcpy(nic->eyecatcher, "NIC", sizeof(nic->eyecatcher));
	JSON_LOOP_FOR_DICT(kv, dict) {
		JSON_LOOP_ITERATION_START(vgsyw94, kv->key);
		s = kv->value->str;
		JSON_ASSIGN_PLAIN(thsuik3, "pkey", nic->pkey, kv->value->num);
		JSON_ASSIGN_PLAIN(1dmopyl, "version", nic->version, kv->value->num);
		JSON_ASSIGN_STR(bhsikws, "guid", nic->sw_gid_str, s);
		JSON_ASSIGN_STR(8nklspw, "nicID", nic->hw_gid_str, s);
		JSON_ASSIGN_PLAIN(b6euhrq, "uuid", nic->uuid, *GET_UNION_UUID_OF_URN_UUID_STR(s));
		JSON_ASSIGN_PLAIN(7xbjwl2, "protocol", nic->protocol, (!strcmp(s, "RoCE") ? 1 :
															   !strcmp(s, "Infiniband") ? 2 :
															   !strcmp(s, "TCP") ? 3 :
															   !strcmp(s, "MULTI") ? 4 : -1));
		JSON_WARN_and_FIX(6eg39od, "protocol", nic->protocol, (typeof(nic->protocol))-1, 1, "key=@STR val=@STR", kv->key, s);
		JSON_LOOP_ITERATION_END(vnjxls0, kv->key);
	}
	JSON_ASSIGN_AND_CALL_VALIDATE(n3bus03);
//	NFOUT;
}

static void _HW_node_nics_from_json(struct mm_node_conf *node, struct mm_json_elem *nics_arr_json)
{
	int		i;
	if (nics_arr_json->type != JSON_E_ARRAY)
		return;
	node->num_nics = nics_arr_json->array.len;
	node->nics = (struct mm_nic_conf *)NNVMEIBT_BM_CALLOC(mem_mgmt_12, sizeof(struct mm_nic_conf) * node->num_nics);
	for (i = 0; i < node->num_nics; i++) {
		_HW_nic_from_json(&node->nics[i], nics_arr_json->array.elements[i]);
	}
}

static void _HW_node_from_json(struct HW_mgmt_conf *conf, struct mm_node_conf *node, struct mm_json_elem *elem)
{
	struct mm_json_kv_pair		*kv;
	struct mm_json_dict			*dict = &(elem->dict);
	JSON_ASSIGN_AND_CALL_INIT();

//	NFIN;
	memset(node, 0, sizeof(struct mm_node_conf));
	if (elem->type != JSON_E_DICT)
		return;
	nvmeibt_strlcpy(node->eyecatcher, "NOD", sizeof(node->eyecatcher));
	JSON_LOOP_FOR_DICT(kv, dict) {
		JSON_LOOP_ITERATION_START(uejsk3i, kv->key);
		JSON_ASSIGN_OPTIONAL(bw8k2k9, "_id");		// Identical to "node_id"
		JSON_ASSIGN_CALL(5njspwt, "nics", _HW_node_nics_from_json, node, kv->value);
		JSON_ASSIGN_PLAIN(4vtaio2, "disks", conf->num_disks, conf->num_disks + kv->value->array.len);
		JSON_ASSIGN_STR(0wnjl25, "node_id", node->node_id, kv->value->str);
		// JSON_ASSIGN_PLAIN(nvjsbej, "version", node->version, kv->value->num);
		JSON_ASSIGN_PLAIN(44ksieh, "uuid", node->uuid, *GET_UNION_UUID_OF_URN_UUID_STR(kv->value->str));
		JSON_LOOP_ITERATION_END(rvshiwl, kv->key);
	}
	JSON_ASSIGN_AND_CALL_VALIDATE(48jsiwl);
//	NFOUT;
}

static void _HW_disk_from_json(struct mm_disk_conf *disk, struct mm_json_elem *elem)
{
	struct mm_json_kv_pair		*kv;
	struct mm_json_dict			*dict = &(elem->dict);
	JSON_ASSIGN_AND_CALL_INIT();

//	NFIN;
	memset(disk, 0, sizeof(*disk));
	if (elem->type != JSON_E_DICT)
		return;
	JSON_LOOP_FOR_DICT(kv, dict) {
		JSON_LOOP_ITERATION_START(onieuv5, kv->key);
		JSON_ASSIGN_PLAIN(6gak3so, "vendorID", disk->vendorID, kv->value->num);
		JSON_ASSIGN_PLAIN(4vhw9l2, "block_size", disk->block_size, kv->value->num);
		JSON_ASSIGN_PLAIN(0cki4lz, "blocks", disk->n_pblks, kv->value->num);
		JSON_ASSIGN_PLAIN(afj58so, "version", disk->version, kv->value->num);
		JSON_ASSIGN_PLAIN(4mio7jd, "uuid", disk->uuid, *GET_UNION_UUID_OF_URN_UUID_STR(kv->value->str));
		JSON_ASSIGN_STR(bagw27s, "diskID", disk->diskID, kv->value->str);
		JSON_ASSIGN_PLAIN_OPTIONAL(pdhkri4, "isOutOfService", disk->isOutOfService, kv->value->num);
		JSON_ASSIGN_PLAIN(5hs8k30, "activeFormatRequestCounter", disk->activeFormatRequestCounter, kv->value->num);
		JSON_LOOP_ITERATION_END(fbuw03l, kv->key);
	}
	JSON_ASSIGN_AND_CALL_VALIDATE(5xx6sjh);
//	NFOUT;
}

static int _HW_add_disks_from_node_json(struct mm_disk_conf *disks, struct mm_node_conf *node, struct mm_json_elem *elem)
{
	int num_added = 0;
	int i;

//	NFIN;
	if (elem->type != JSON_E_DICT)
		return 0;
	for (i=0; i<elem->dict.len; i++) {
		struct mm_json_kv_pair *kv = &elem->dict.elements[i];
		DUMP_kv_TO_LOG(rvskwil, kv);
		if (!strcmp(kv->key, "disks")) {
			int j;
			if (kv->value->type != JSON_E_ARRAY)
				continue;
			for (j=0; j<kv->value->array.len; j++) {
				struct mm_disk_conf *disk = disks+num_added;
				_HW_disk_from_json(disk, kv->value->array.elements[j]);
				disk->origNodeUuid = node->uuid;
				num_added++;
			}
		}
	}
//	NFOUT;
	return num_added;
}

static void _mm_mgmt_conf_from_json(struct mm_mgmt_conf *conf, struct mm_json_elem *elem)
{
	struct mm_json_kv_pair		*kv;
	struct mm_json_dict			*dict = &(elem->dict);
	JSON_ASSIGN_AND_CALL_INIT();

	NFIN;
	if (elem->type != JSON_E_DICT)
		return;
	JSON_LOOP_FOR_DICT(kv, dict) {
		JSON_LOOP_ITERATION_START(tzikow4, kv->key);
		JSON_ASSIGN_PLAIN(6h9wl35, "dbUUID", conf->dbUUID, *GET_UNION_UUID_OF_URN_UUID_STR(kv->value->str));
		JSON_ASSIGN_PLAIN(0kdo34b, "configurationVersion", conf->configurationVersion, kv->value->num);
//		JSON_ASSIGN_OPTIONAL(9nfjw52, "_id");
		JSON_ASSIGN_PLAIN(czzg3j3, "protocolVersion", conf->protocolVersion, kv->value->num);
		JSON_ASSIGN_PLAIN(1mak0fy, "idx", conf->idx, kv->value->num);
		JSON_ASSIGN_VALIDATE_STR(vgsikel, "eyecatcher", "CNF", kv->value->str);
		JSON_ASSIGN_PLAIN(6gbks24, "structVersion", conf->structVersion, kv->value->num);
		JSON_ASSIGN_VALIDATE_STR(6enol0s, "messageType", "fullVolConfig", kv->value->str);
		JSON_ASSIGN_PLAIN(4bjaiwk, "messageTypeVersion", conf->messageTypeVersion, kv->value->num);
		JSON_ASSIGN_PLAIN(axrjw9o, "num_vols", conf->num_vols, kv->value->num);
		JSON_ASSIGN_PLAIN(29js9la, "num_disks", conf->num_disks, kv->value->num);
		JSON_ASSIGN_PLAIN(0cm28js, "num_nodes", conf->num_nodes, kv->value->num);
		JSON_LOOP_ITERATION_END(4vya9k2, kv->key);
	}
	JSON_ASSIGN_AND_CALL_VALIDATE(ysh3klq);
	NFOUT;
}

static void _mm_mgmt_vols_from_json(struct mm_mgmt_conf *conf, struct mm_json_elem *vols_arr_json, int64_t kafka_offset)
{
	int		i;

	if (vols_arr_json->type != JSON_E_ARRAY)
		return;
	conf->num_vols = vols_arr_json->array.len;
	conf->volumes = (struct mm_vol_conf *)NNVMEIBT_BM_CALLOC(vgs9eow,  sizeof(struct mm_vol_conf) * conf->num_vols);
	N_Tf(vsg47sh, "volumes: conf->num_vols=@INT", conf->num_vols);
	for (i = 0; i < conf->num_vols; i++) {
		_mm_vol_from_json(&conf->volumes[i], vols_arr_json->array.elements[i], kafka_offset, 1, 0);
	}
}

static void _HW_mgmt_nodes_and_their_disks_from_json(struct HW_mgmt_conf *conf, struct mm_json_elem *nodes_arr_json)
{
	int		i;
	int		n_disks_added_so_far = 0;

	if (nodes_arr_json->type != JSON_E_ARRAY)
		return;
	conf->num_nodes = nodes_arr_json->array.len;
	conf->nodes = (struct mm_node_conf *)NNVMEIBT_BM_CALLOC(tj03dko,  sizeof(struct mm_node_conf) * conf->num_nodes);
	N_Tf(224vghsvd7, "targets: conf->num_nodes=@INT", conf->num_nodes);
	// The loop over the nodes also calculates the total conf->num_disks, so that we can allocate conf->disks
	for (i = 0; i < conf->num_nodes; i++) {
		_HW_node_from_json(conf, &conf->nodes[i], nodes_arr_json->array.elements[i]);
	}
	// Now that we know conf->num_disks
	conf->disks = (struct mm_disk_conf *)NNVMEIBT_BM_CALLOC(3cgsk29,  sizeof(struct mm_disk_conf) * conf->num_disks);
	for (i = 0; i < conf->num_nodes; i++) {
		n_disks_added_so_far += _HW_add_disks_from_node_json(conf->disks + n_disks_added_so_far, &conf->nodes[i], nodes_arr_json->array.elements[i]);
	}
}

static void handle_mgmt_msg_payload_json(struct mm_mgmt_conf *conf, struct mm_json_elem *root, int64_t kafka_offset, bool is_new_or_upd, bool is_deleteVolumeCompleted)
{
	// Generate a valid conf with one volume
	conf->num_vols = 1;
	conf->volumes = (struct mm_vol_conf *)NNVMEIBT_BM_CALLOC(v6sbsir, sizeof(struct mm_vol_conf)*conf->num_vols);
	_mm_vol_from_json(&conf->volumes[0], root, kafka_offset, is_new_or_upd, is_deleteVolumeCompleted);
}

int nvmeibt_mgmt_msg_json_tree_to_mgmt_conf(struct mm_mgmt_conf *conf, struct mm_json_elem *elem, int64_t kafka_offset, bool is_new_or_upd, bool is_deleteVolumeCompleted)
{
	struct mm_json_kv_pair		*kv;
	struct mm_json_dict			*dict = &(elem->dict);
	JSON_ASSIGN_AND_CALL_INIT();

	NFIN;
	if (elem->type != JSON_E_DICT)
		return 0;
	JSON_LOOP_FOR_DICT(kv, dict) {
		JSON_LOOP_ITERATION_START(vgsjwkr, kv->key);
		JSON_ASSIGN_OPTIONAL(cvgsjhw, "messageType");
		JSON_ASSIGN_OPTIONAL(7inrleb, "messageTypeVersion");
		JSON_ASSIGN_CALL(omkcrx3, "payload", handle_mgmt_msg_payload_json, conf, kv->value, kafka_offset, is_new_or_upd, is_deleteVolumeCompleted);
		JSON_LOOP_ITERATION_END(1rctvsy, kv->key);
	}
	JSON_ASSIGN_AND_CALL_VALIDATE(axlenc7);
	NFOUT;
	return 0;
}

static int JSON_persistence_tree_to_mgmt_conf(struct mm_mgmt_conf *conf, struct mm_json_elem *elem, int64_t kafka_offset)
{
	struct mm_json_kv_pair		*kv;
	struct mm_json_dict			*dict = &(elem->dict);
	JSON_ASSIGN_AND_CALL_INIT();

	NFIN;
	if (!elem) {
		goto out;
	}
	if (elem->type != JSON_E_DICT)
		return 0;
	JSON_LOOP_FOR_DICT(kv, dict) {
		JSON_LOOP_ITERATION_START(f4ts74h, kv->key);
		JSON_ASSIGN_CALL(6xvsn3l, "volumes", _mm_mgmt_vols_from_json, conf, kv->value, kafka_offset);
		JSON_ASSIGN_CALL(vhxdbr3, "mm_mgmt_conf", _mm_mgmt_conf_from_json, conf, kv->value);
		JSON_LOOP_ITERATION_END(c5ahlqp, kv->key);
	}
	JSON_ASSIGN_AND_CALL_VALIDATE(4b5ua92);
out:
	NFOUT;
	return 0;
}

int _mm_managementConfiguration_from_json(struct mm_mgmt_conf *conf, struct mm_json_elem *elem)
{
	struct mm_json_kv_pair		*kv;
	struct mm_json_dict			*dict = &(elem->dict);
	JSON_ASSIGN_AND_CALL_INIT();

	NFIN;
	if (elem->type != JSON_E_DICT)
		return 0;
	JSON_LOOP_FOR_DICT(kv, dict) {
		JSON_LOOP_ITERATION_START(vhsjei9, kv->key);
		JSON_ASSIGN_VALIDATE_STR(bjhdk5p, "_id", "1", kv->value->str);
		JSON_ASSIGN_PLAIN(8sk2lqa, "configurationVersion", conf->configurationVersion, kv->value->num);
		JSON_ASSIGN_PLAIN(b6k3os2, "leaderToken", conf->leaderToken, kv->value->num);
		JSON_ASSIGN_PLAIN(vghskmw, "kafkaMessageSequence", conf->kafkaMessageSequence, kv->value->num);
		JSON_ASSIGN_PLAIN(1majsyq, "raftTerm", conf->raftTerm, kv->value->num);
		JSON_ASSIGN_PLAIN(v37jsoq, "stopSendingKeepaliveToken", conf->stopSendingKeepaliveToken, kv->value->num);
		JSON_ASSIGN_OPTIONAL(5jwpsap, "isUnavailable");
		JSON_ASSIGN_PLAIN(vhsj38a, "dbUUID", conf->dbUUID, *GET_UNION_UUID_OF_URN_UUID_STR(kv->value->str));
		JSON_LOOP_ITERATION_END(vge8u42, kv->key);
	}
	JSON_ASSIGN_AND_CALL_VALIDATE(8whsuk2);
	NFOUT;
	return 0;
}

int _HW_managementConfiguration_from_json(struct HW_mgmt_conf *conf, struct mm_json_elem *elem)
{
	struct mm_json_kv_pair		*kv;
	struct mm_json_dict			*dict = &(elem->dict);
	JSON_ASSIGN_AND_CALL_INIT();

	NFIN;
	if (elem->type != JSON_E_DICT)
		return 0;
	JSON_LOOP_FOR_DICT(kv, dict) {
		JSON_LOOP_ITERATION_START(7zn3kms, kv->key);
		JSON_ASSIGN_VALIDATE_STR(vbnxjsk, "_id", "1", kv->value->str);
		JSON_ASSIGN_PLAIN(p2mcuis, "configurationVersion", conf->configurationVersion, kv->value->num);
		JSON_ASSIGN_PLAIN(5bjzdw1, "leaderToken", conf->leaderToken, kv->value->num);
		JSON_ASSIGN_PLAIN_OPTIONAL(vfkel9g, "kafkaMessageSequence", conf->kafkaMessageSequence, kv->value->num);	// MGMT sometime sends (Dec24)
		JSON_ASSIGN_PLAIN_OPTIONAL(sdguclx, "raftTerm", conf->raftTerm, kv->value->num);	// MGMT sometime sends (Dec24)
		JSON_ASSIGN_PLAIN_OPTIONAL(mk0ih5b, "stopSendingKeepaliveToken", conf->stopSendingKeepaliveToken, kv->value->num);	// MGMT sometime sends (Dec24)
		JSON_ASSIGN_OPTIONAL(38sbftw, "isUnavailable");	// MGMT sometime sends (Dec24)
		JSON_ASSIGN_PLAIN(zihrsk3, "dbUUID", conf->dbUUID, *GET_UNION_UUID_OF_URN_UUID_STR(kv->value->str));
		JSON_ASSIGN_OPTIONAL(ryuajnj, "lastReceivedLeaderKeepAlive");	// MGMT sometime sends (Dec24)
		JSON_LOOP_ITERATION_END(5b28l5i, kv->key);
	}
	JSON_ASSIGN_AND_CALL_VALIDATE(vmsi29f);
	NFOUT;
	return 0;
}

int _mm_mgmt_HW_config_payload_json(struct HW_mgmt_conf *conf, struct mm_json_elem *elem)
{
	struct mm_json_kv_pair		*kv;
	struct mm_json_dict			*dict = &(elem->dict);
	JSON_ASSIGN_AND_CALL_INIT();

	NFIN;
	if (elem->type != JSON_E_DICT)
		return 0;
	JSON_LOOP_FOR_DICT(kv, dict) {
		JSON_LOOP_ITERATION_START(dna1k0d, kv->key);
		JSON_ASSIGN_CALL(byrksi3, "managementConfiguration", _HW_managementConfiguration_from_json, conf, kv->value);
		JSON_ASSIGN_CALL(92jksik, "targets", _HW_mgmt_nodes_and_their_disks_from_json, conf, kv->value);
		JSON_LOOP_ITERATION_END(8ja2k0d, kv->key);
	}
	JSON_ASSIGN_AND_CALL_VALIDATE(rnuidp3);
	NFOUT;
	return 0;
}

int nvmeibt_mm_json_tree_to_HW_mgmt_conf(struct HW_mgmt_conf *conf, struct mm_json_elem *elem, int64_t kafka_offset)
{
	struct mm_json_kv_pair		*kv;
	struct mm_json_dict			*dict = &(elem->dict);
	JSON_ASSIGN_AND_CALL_INIT();

	NFIN;
	if (elem->type != JSON_E_DICT)
		return 0;
	if (nvmeibt_offset_and_idx_is_uninitialized(conf->idx)) {
		// The conf source is external, hence did not include a kafka_offset. From now on it is sticky to this conf
		conf->idx = kafka_offset;
	}
	JSON_LOOP_FOR_DICT(kv, dict) {
		JSON_LOOP_ITERATION_START(4b8wk39, kv->key);
		JSON_ASSIGN_VALIDATE_STR(5bjs92l, "messageType", "hardwareConfiguration", kv->value->str);
		JSON_ASSIGN_PLAIN(bxhjjbe, "messageTypeVersion", conf->messageTypeVersion, kv->value->num);
		JSON_ASSIGN_CALL(m8xai3g, "payload", _mm_mgmt_HW_config_payload_json, conf, kv->value);
		JSON_LOOP_ITERATION_END(76nks0l, kv->key);
	}
	JSON_ASSIGN_AND_CALL_VALIDATE(abyz7m5);
	NFOUT;
	return 0;
}

void mm_conf_free_tree(struct mm_mgmt_conf *conf)
{
	int i, j, k;

	NFIN;
	if (!conf) {
		goto out;
	}
	for (i=0; i<conf->num_vols; i++) {
		struct mm_vol_conf *vol = &conf->volumes[i];
		for (j=0; j<vol->num_chunks; j++) {
			struct mm_chunk_conf *chunk = &vol->chunks[j];
			for (k=0; k<chunk->num_praids; k++) {
				struct mm_praid_conf *praid = &chunk->praids[k];
				NNVMEIBT_BM_FREE(mem_mgmt_59, praid->segments);
			}
			NNVMEIBT_BM_FREE(mem_mgmt_60, chunk->praids);
		}
		NNVMEIBT_BM_FREE(mem_mgmt_61, vol->chunks);
	}
	NNVMEIBT_BM_FREE(mem_mgmt_62, conf->volumes);
	NNVMEIBT_BM_FREE(mem_mgmt_63, conf);
out:
	NFOUT;
}

void HW_conf_free_tree(struct HW_mgmt_conf *conf) {
	int i;
	if (!conf)
		return;
	NFIN;
	NNVMEIBT_BM_FREE(ksiuje9, conf->disks);
	for (i = 0; i < conf->num_nodes; i++) {
		struct mm_node_conf *node = &conf->nodes[i];
		NNVMEIBT_BM_FREE(vgsuk39, node->nics);
	}
	NNVMEIBT_BM_FREE(t6neuks, conf->nodes);
	NNVMEIBT_BM_FREE(bidstne, conf);
	NFOUT;
}

void mm_print_vol_conf(struct mm_vol_conf *vol, int (*printf_fn)(void *ctx, const char *fmt, ...), void *s)
{
	int			j, k, l;
	char		uuid[40];

	(*printf_fn)(s, "VOL:  %s, n_blks=0x%lx, raidType=%u, version=%d, action=%c k_offset=%ld\n", vol->name, vol->blocks, vol->raidType, vol->version, vol->action, vol->kafka_offset_or_idx);
	_mm_uuid_binary_to_str(&vol->uuid, uuid);
	(*printf_fn)(s, "      uuid=%s, stripeWidth=%d, stripeSize=%d, (%s)\n", uuid, vol->stripeWidth, vol->stripeSize, vol->eyecatcher);
	for (j=0; j<vol->num_chunks; j++) {
		struct mm_chunk_conf *chunk = &vol->chunks[j];
		(*printf_fn)(s, "      CHK:  vlbs=0x%lx, vlbe=0x%lx\n", chunk->vlbs, chunk->vlbe);
		_mm_uuid_binary_to_str(&chunk->uuid, uuid);
		(*printf_fn)(s, "            uuid=%s (%s)\n", uuid, chunk->eyecatcher);
		for (k=0; k<chunk->num_praids; k++) {
			struct mm_praid_conf *praid = &chunk->praids[k];
			_mm_uuid_binary_to_str(&praid->uuid, uuid);
			(*printf_fn)(s, "            PRD:  uuid=%s, ver=%d, (%s)\n", uuid, praid->version, praid->eyecatcher);
			for (l=0; l<praid->num_segments; l++) {
				struct mm_segment_conf *seg = &praid->segments[l];
				_mm_uuid_binary_to_str(&seg->uuid, uuid);
				(*printf_fn)(s, "                  SEG:  uuid=%s (%s)\n", uuid, seg->eyecatcher);
				_mm_uuid_binary_to_str(&seg->diskUUID, uuid);
				(*printf_fn)(s, "                        diskUUID=%s, lbs=0x%lx, lbe=0x%lx, type=%u\n", uuid, seg->lbs, seg->lbe, seg->type);
				(*printf_fn)(s, "                        pRaidIndex=%d, pRaidTypeIndex=%u, action=%c\n", seg->pRaidIndex, seg->pRaidTypeIndex, seg->action);
			}
		}
	}
}

int nvmeibt_raft_print_status(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx);

void mm_print_conf(struct mm_mgmt_conf *conf, int (*printf_fn)(void *ctx, const char *fmt, ...), void *s)
{
	int i;
	char uuid[40];

	NFIN;
	(*printf_fn)(s, "CNF:  protocolVersion=%u, idx=%lld\n", conf->protocolVersion, conf->idx);
	_mm_uuid_binary_to_str(&conf->dbUUID, uuid);
	(*printf_fn)(s, "      dbUUID=%s, structVersion=%u configVersion=%lld (%s)\n", uuid, conf->structVersion, conf->configurationVersion, conf->eyecatcher);
	for (i=0; i<conf->num_vols; i++) {
		mm_print_vol_conf(&conf->volumes[i], printf_fn, s);
	}
	NFOUT;
}

void HW_print_conf(struct HW_mgmt_conf *conf, int (*printf_fn)(void *ctx, const char *fmt, ...), void *s)
{
	int i, j;
	char uuid[40];

	NFIN;
	(*printf_fn)(s, "CNF:  protocolVersion=%u, idx=%lld\n", conf->protocolVersion, conf->idx);
	_mm_uuid_binary_to_str(&conf->dbUUID, uuid);
	(*printf_fn)(s, "      dbUUID=%s, structVersion=%u configVersion=%lld (%s)\n", uuid, conf->structVersion, conf->configurationVersion, conf->eyecatcher);
	for (i=0; i<conf->num_disks; i++) {
		struct mm_disk_conf *disk = &conf->disks[i];
		(*printf_fn)(s, "DSK:  %s vendorID=%u nodeID=0x%016llx blksize=%u n_pblks=%lu outOfService=%d formatReqCntr=%u version=%u\n",
					 disk->diskID, disk->vendorID, disk->origNodeUuid.ll[0], disk->block_size, disk->n_pblks, disk->isOutOfService, disk->activeFormatRequestCounter, disk->version);
		_mm_uuid_binary_to_str(&disk->uuid, uuid);
		(*printf_fn)(s, "      uuid=%s (%s)\n", uuid, disk->eyecatcher);
	}
	for (i=0; i<conf->num_nodes; i++) {
		struct mm_node_conf *node = &conf->nodes[i];
		_mm_uuid_binary_to_str(&node->uuid, uuid);
		(*printf_fn)(s, "NOD:  %s\n", node->node_id);
		(*printf_fn)(s, "      uuid=%s (%s)\n", uuid, node->eyecatcher);
		for (j=0; j<node->num_nics; j++) {
			struct mm_nic_conf *nic = &node->nics[j];
			_mm_uuid_binary_to_str(&nic->uuid, uuid);
			(*printf_fn)(s, "      NIC:  uuid=%s (%s)\n", uuid, nic->eyecatcher);
			(*printf_fn)(s, "            pkey=%u, protocol=%u, hw_gid_str=%s, sw_gid_str=%s\n", nic->pkey, nic->protocol, nic->hw_gid_str, nic->sw_gid_str);
		}
	}
	NFOUT;
}


// Type-specific functions for packing/unpacking structs with BE/LE swaps
struct _packed_mm_segment_conf {
	char eyecatcher[4];						// 4
	int8_t pRaidIndex;						// 5
	uint8_t pRaidTypeIndex;					// 6
	uint8_t type;							// 7
	char action;							// 8
	uint64_t lbs;							// 16
	uint64_t lbe;							// 24
	char	filler_1[24];					// 48
	union nvmeib_uuid uuid;					// 64
	union nvmeib_uuid diskUUID;				// 80
	char	align[0] __attribute__((aligned(16)));
} __attribute__((__packed__, aligned(16)));

struct _packed_mm_praid_conf {
	char eyecatcher[4];						// 4
	uint8_t num_segments;					// 5
	uint8_t activated;						// 6
	uint8_t stripeIndex;					// 7
	uint8_t reserved_1;						// 8
	uint32_t version;						// 12
	char	filler_1[20];					// 32
	union nvmeib_uuid uuid;					// 48
	char	align[0] __attribute__((aligned(16)));
} __attribute__((__packed__, aligned(16)));

struct _packed_mm_chunk_conf {
	char eyecatcher[4];						// 4
	uint8_t num_praids;						// 5
	uint8_t reserved_1;						// 6
	uint16_t reserved_2;					// 8
	uint64_t vlbs;							// 16
	uint64_t vlbe;							// 24
	char	filler_1[24];					// 48
	union nvmeib_uuid uuid;					// 64
	char	align[0] __attribute__((aligned(16)));
} __attribute__((__packed__, aligned(16)));

struct _packed_mm_vol_conf {
	char eyecatcher[4];						// 4
	uint8_t raidType;						// 5
	uint8_t num_chunks;						// 6
	uint16_t blockSize;						// 8
	uint32_t version;						// 12
	char	filler_0;						// 13
	char action;							// 14
	char res_type;							// 15	// Obsolete Elect
	uint8_t relativeRebuildPriority;		// 16
	uint8_t stripeSize;						// 17
	uint8_t stripeWidth;					// 18
	uint8_t lockServer_type;				// 19
	uint8_t lockServer_maxNOwners;			// 20
	int8_t lockServer_locksetShift;			// 21
	int8_t		enableCrcCheck;				// 22
	int8_t		use_debug_di;				// 23
	char name[26];							// 49
	char	filler_1[15];					// 64
	union nvmeib_uuid uuid;					// 80
	uint64_t blocks;						// 88
	int64_t		kafka_offset_or_idx;		// 96
	char	align[0] __attribute__((aligned(16)));
} __attribute__((__packed__, aligned(16)));

#define COPY_FIELD(x)	memcpy(is_out ? dst->x : src->x, is_out ? src->x : dst->x, sizeof(src->x))
#define SWAP64_FIELD(x)	if (is_out) dst->x = LE_SWAP64(src->x); else src->x = LE_SWAP64(dst->x)
#define SWAP32_FIELD(x)	if (is_out) dst->x = LE_SWAP32(src->x); else src->x = LE_SWAP32(dst->x)
#define SWAP16_FIELD(x)	if (is_out) dst->x = LE_SWAP16(src->x); else src->x = LE_SWAP16(dst->x)
#define SWAP8_FIELD(x)	if (is_out) dst->x = LE_SWAP8(src->x); else src->x = LE_SWAP8(dst->x)

#define SWAP_UUID_FIELD(x) ({					\
	if (is_out) {								\
		dst->x = swap_uuid_LE_BE(&(src->x));	\
	} else {									\
		src->x = swap_uuid_LE_BE(&(dst->x));	\
	}											\
})

#define MEMSET_ZERO_SRC_OR_DST(is_out, src, dst) ({	\
		if (is_out)	memset(dst, 0, sizeof(*dst));	\
		else memset(src, 0, sizeof(*src));			\
})

#define CHECK_ALIGN16(name) ({	\
	if (is_out) { 	\
		NTOMA_ASSERT(name##_out, (uintptr_t)dst % 16 == 0, "dst=@PTR is not 16-byte aligned", dst);		\
	} else { 		\
		NTOMA_ASSERT(name##_in, (uintptr_t)src % 16 == 0, "src=@PTR is not 16-byte aligned", src);	\
	}				\
})

static uint16_t nvmeibt_seg_convert_config_le_be(void *p, struct mm_segment_conf *src, BOOL is_out)
{
	struct _packed_mm_segment_conf *dst = p;

	{ _Static_assert(sizeof(struct mm_segment_conf) == 64, "Struct mm_segment_conf was changed without updating the packing function!"); }
	{ _Static_assert(sizeof(struct _packed_mm_segment_conf) == 80, "Struct _packed_mm_segment_conf was changed without updating the packing function!"); }
	CHECK_ALIGN16(seg_align_dst);
	MEMSET_ZERO_SRC_OR_DST(is_out, src, dst);
	COPY_FIELD(eyecatcher);
	SWAP8_FIELD(pRaidIndex);
	SWAP8_FIELD(pRaidTypeIndex);
	SWAP8_FIELD(type);
	SWAP8_FIELD(action);
	SWAP64_FIELD(lbs);
	SWAP64_FIELD(lbe);
	SWAP_UUID_FIELD(uuid);
	SWAP_UUID_FIELD(diskUUID);

	return sizeof(*dst);
}

static uint16_t nvmeibt_praid_convert_config_le_be(void *p, struct mm_praid_conf *src, BOOL is_out)
{
	struct _packed_mm_praid_conf *dst = p;

	{ _Static_assert(sizeof(struct mm_praid_conf) == 48, "Struct mm_praid_conf was changed without updating the packing function!"); }
	{ _Static_assert(sizeof(struct _packed_mm_praid_conf) == 48, "Struct _packed_mm_praid_conf was changed without updating the packing function!"); }
	CHECK_ALIGN16(praid_align_dst);
	MEMSET_ZERO_SRC_OR_DST(is_out, src, dst);
	COPY_FIELD(eyecatcher);
	SWAP32_FIELD(version);
	SWAP8_FIELD(num_segments);
	SWAP8_FIELD(activated);
	SWAP8_FIELD(stripeIndex);
	SWAP_UUID_FIELD(uuid);

	return sizeof(*dst);
}

static uint16_t nvmeibt_chunk_convert_config_le_be(void *p, struct mm_chunk_conf *src, BOOL is_out)
{
	struct _packed_mm_chunk_conf *dst = p;

	{ _Static_assert(sizeof(struct mm_chunk_conf) == 48, "Struct mm_chunk_conf was changed without updating the packing function!"); }
	{ _Static_assert(sizeof(struct _packed_mm_chunk_conf) == 64, "Struct _packed_mm_chunk_conf was changed without updating the packing function!"); }
	CHECK_ALIGN16(chunk_align_dst);
	MEMSET_ZERO_SRC_OR_DST(is_out, src, dst);
	COPY_FIELD(eyecatcher);
	SWAP8_FIELD(num_praids);
	SWAP64_FIELD(vlbs);
	SWAP64_FIELD(vlbe);
	SWAP_UUID_FIELD(uuid);

	return sizeof(*dst);
}

static uint16_t nvmeibt_vol_convert_config_le_be(void *p, struct mm_vol_conf *src, BOOL is_out)
{
	struct _packed_mm_vol_conf *dst = p;

	{ _Static_assert(sizeof(struct mm_vol_conf) == 112, "Struct mm_vol_conf was changed without updating the packing function!"); }
	{ _Static_assert(sizeof(struct _packed_mm_vol_conf) == 96, "Struct _packed_mm_vol_conf was changed without updating the packing function!"); }
	CHECK_ALIGN16(vol_align_dst);
	MEMSET_ZERO_SRC_OR_DST(is_out, src, dst);
	COPY_FIELD(eyecatcher);
	SWAP8_FIELD(raidType);
	SWAP8_FIELD(num_chunks);
	SWAP16_FIELD(blockSize);
	SWAP32_FIELD(version);
	COPY_FIELD(name);
	SWAP8_FIELD(action);
	SWAP8_FIELD(relativeRebuildPriority);
	SWAP8_FIELD(stripeSize);
	SWAP8_FIELD(stripeWidth);
	SWAP8_FIELD(lockServer_type);
	SWAP8_FIELD(lockServer_maxNOwners);
	SWAP8_FIELD(lockServer_locksetShift);
	SWAP_UUID_FIELD(uuid);
	SWAP64_FIELD(blocks);
	SWAP64_FIELD(kafka_offset_or_idx);
	SWAP8_FIELD(enableCrcCheck);
	SWAP8_FIELD(use_debug_di);
	// chunks	// Used only locally

	return sizeof(*dst);
}

#define MEMCPY_FIELD(_dst, _src)	({memcpy((_dst), (_src), sizeof(_dst));})
uint16_t nvmeibt_raft_member_conf_convert_le_be(struct mm_raft_member_conf *dst, struct mm_raft_member_conf *src)
{
	{ _Static_assert(sizeof(struct mm_raft_member_conf) == 112, "Struct mm_raft_member_conf was changed without updating the packing function!"); }
	NTOMA_ASSERT(raft_member_align_dst, (uintptr_t)dst % 16 == 0, "dst=@PTR is not 16-byte aligned", dst);
	memset(dst, 0, sizeof(*dst));
	MEMCPY_FIELD(dst->eyecatcher, src->eyecatcher);
	MEMCPY_FIELD(dst->hostname, src->hostname);
	dst->uuid = swap_uuid_LE_BE(&(src->uuid));
	dst->kafka_offset = LE_SWAP64(src->kafka_offset);
	return sizeof(*dst);
}

uint16_t nvmeibt_mm_mgmt_convert_config_le_be(void *p, struct mm_mgmt_conf *src, BOOL is_out)
{
	struct _packed_mm_mgmt_conf *dst = p;

	{ _Static_assert(sizeof(struct mm_mgmt_conf) == 224, "Struct mm_mgmt_conf was changed without updating the packing function!"); }
	{ _Static_assert(sizeof(struct _packed_mm_mgmt_conf) == 144, "Struct _packed_mm_mgmt_conf was changed without updating the packing function!"); }
	//CHECK_ALIGN16(mm_mgmt_align_dst);
	MEMSET_ZERO_SRC_OR_DST(is_out, src, dst);
	COPY_FIELD(eyecatcher);
	SWAP16_FIELD(structVersion);
	SWAP16_FIELD(protocolVersion);
	SWAP64_FIELD(configurationVersion);
	SWAP64_FIELD(idx);
	SWAP_UUID_FIELD(dbUUID);
	COPY_FIELD(messageType);
	SWAP64_FIELD(messageTypeVersion);
	SWAP32_FIELD(num_vols);
	SWAP16_FIELD(num_disks);
	SWAP16_FIELD(num_nodes);
	return sizeof(*dst);
}

int nvmeibt_Str_sprintf(struct nvmeibt_Str *this, const char *format, ...);
void serialize_mm_mgmt_conf_to_JSON(struct mm_mgmt_conf *c, bool is_topo_config, struct nvmeibt_Str *JSON_output)
{
	struct nvmeibt_urn_uuid		urn_uuid;

	if (!JSON_output) {
		goto out;
	}
	nvmeibt_Str_sprintf(JSON_output, "\n\"%s\":{", (is_topo_config ? "FULL_TOPO_CONFIG" : "KAFKA_MGMT_CONFIG_FULL"));
	urn_uuid = nvmeibt_union_uuid_to_urn_uuid(&(c->dbUUID));
	nvmeibt_Str_sprintf(JSON_output, "\n\"mm_mgmt_conf\":{\"eyecatcher\":\"%.4s\", \"structVersion\":%u, \"protocolVersion\":%u, \"configurationVersion\":%lld, "
						"\"idx\":%lld, \"dbUUID\":\"%s\", \"messageType\":\"%s\", \"messageTypeVersion\":%lld, \"num_vols\":%d, \"num_disks\":%u, \"num_nodes\":%u},\n"
						"\"volumes\":[%s",
						c->eyecatcher, c->structVersion, c->protocolVersion, c->configurationVersion,
						c->idx, urn_uuid.str, c->messageType, c->messageTypeVersion, c->num_vols, c->num_disks, c->num_nodes,
						(c->num_vols <= 0 ? "]" : ""));
out:;
}

void mm_set_vol_num(void *dst, uint32_t n_vols)
{
	((struct _packed_mm_mgmt_conf *)dst)->num_vols = LE_SWAP32(n_vols);
}

void serialize_vol_conf_to_JSON(struct mm_vol_conf *v, struct nvmeibt_Str *JSON_output)
{
	struct nvmeibt_urn_uuid		urn_uuid;

	if (!JSON_output) {
		goto out;
	}
	urn_uuid = nvmeibt_union_uuid_to_urn_uuid(&(v->uuid));
	nvmeibt_Str_sprintf(JSON_output, "\n\t{\"eyecatcher\":\"%.4s\", \"RAIDLevel\":\"%s\", \"blockSize\":%u, \"version\":%u, "
						"\"name\":\"%s\", \"action\":\"%c\", \"relativeRebuildPriority\":%u, \"stripeSize\":%u, \"stripeWidth\":%u, "
						"\"lockServer\":{\"type\":%u, \"maxNOwners\":%u, \"locksetShift\":%d}, "
						"\"enableCrcCheck\":%d, \"use_debug_di\":%d, \"uuid\":\"%s\", \"blocks\":%llu, "
						"\"kafka_offset_or_idx\":%lld, \"chunks\":[",
						v->eyecatcher,
						(v->raidType == 0 ? "Concatenated" : v->raidType == 1 ? "Mirrored RAID-1" : v->raidType == 6 ? "Erasure Coding" : "UNKNOWN"),
						v->blockSize, v->version,
						nvmeibt_escape_special_characters(v->name).s,
						v->action, v->relativeRebuildPriority, v->stripeSize, v->stripeWidth, v->lockServer_type,
						v->lockServer_maxNOwners, v->lockServer_locksetShift, v->enableCrcCheck, v->use_debug_di, urn_uuid.str, v->blocks,
						v->kafka_offset_or_idx);
out:;
}

void serialize_chunk_conf_to_JSON(struct mm_chunk_conf *c, struct nvmeibt_Str *JSON_output)
{
	struct nvmeibt_urn_uuid		urn_uuid;

	if (!JSON_output) {
		goto out;
	}
	urn_uuid = nvmeibt_union_uuid_to_urn_uuid(&(c->uuid));
	nvmeibt_Str_sprintf(JSON_output, "\n\t\t{\"eyecatcher\":\"%.4s\", \"vlbs\":%llu, \"vlbe\":%llu, \"uuid\":\"%s\", \"pRaids\":[",
						c->eyecatcher, c->vlbs, c->vlbe, urn_uuid.str);
out:;
}

void serialize_praid_conf_to_JSON(struct mm_praid_conf *p, struct nvmeibt_Str *JSON_output)
{
	struct nvmeibt_urn_uuid		urn_uuid;

	if (!JSON_output) {
		goto out;
	}
	urn_uuid = nvmeibt_union_uuid_to_urn_uuid(&(p->uuid));
	nvmeibt_Str_sprintf(JSON_output, "\n\t\t\t{\"eyecatcher\":\"%.4s\", \"version\":%u, \"activated\":%u, \"stripeIndex\":%u, \"zone\":\"%lld\", \"uuid\":\"%s\", \"diskSegments\":[",
						p->eyecatcher, p->version, p->activated, p->stripeIndex, nvmeibt_kafka_get_kafka_mgmt_zone_number(), urn_uuid.str);
out:;
}

void serialize_seg_conf_to_JSON(struct mm_segment_conf *s, struct nvmeibt_Str *JSON_output)
{
	struct nvmeibt_urn_uuid		seg_urn_uuid;
	struct nvmeibt_urn_uuid		disk_urn_uuid;

	if (!JSON_output) {
		goto out;
	}
	seg_urn_uuid = nvmeibt_union_uuid_to_urn_uuid(&(s->uuid));
	disk_urn_uuid = nvmeibt_union_uuid_to_urn_uuid(&(s->diskUUID));
	nvmeibt_Str_sprintf(JSON_output,
						"\n\t\t\t\t{\"eyecatcher\":\"%.4s\", \"pRaidIndex\":%d, \"pRaidTypeIndex\":%u, \"type\":\"data\", \"status\":\"%c\", \"lbs\":%llu, \"lbe\":%llu, \"uuid\":\"%s\", \"diskUUID\":\"%s\"}",
						s->eyecatcher, s->pRaidIndex, s->pRaidTypeIndex, s->action, s->lbs, s->lbe, seg_urn_uuid.str, disk_urn_uuid.str);
out:;
}

void serialize_end_of_array_obj_to_JSON(int i, int n_objs_in_arr, bool is_end_of_obj_right_after_end_of_array, struct nvmeibt_Str *JSON_output)
{
	bool		is_end_of_arr = (i >= (n_objs_in_arr - 1));
	if (!JSON_output) {
		goto out;
	}
	nvmeibt_Str_sprintf(JSON_output, "%s%s", (is_end_of_arr ? "\n]" : ","), (is_end_of_arr && is_end_of_obj_right_after_end_of_array ? "}" : ""));
out:;
}

struct mm_mgmt_conf *mm_wire_buf_to_mm_mgmt_conf(const void *in_wire_conf_buf, bool is_topo_config, struct nvmeibt_Str *JSON_output)
{
	struct mm_mgmt_conf *conf;
	const void *wire_in_p = in_wire_conf_buf;
	int i, j, k, l;

	NFIN;
	if (!in_wire_conf_buf) {
		conf = NULL;
		goto out;
	}
	conf = (struct mm_mgmt_conf *)NNVMEIBT_BM_CALLOC(mem_mgmt_30,  sizeof(*conf));
	wire_in_p += nvmeibt_mm_mgmt_convert_config_le_be((void *)wire_in_p, conf, false);
	serialize_mm_mgmt_conf_to_JSON(conf, is_topo_config, JSON_output);
	if (conf->structVersion != MM_STRUCT_VER_2067) {
		N_Wf(djju873, "Bad struct version in packed configuration! eyecatcher=@STR ver=@INT", conf->eyecatcher, conf->structVersion);
		NNVMEIBT_BM_FREE(mem_mgmt_64, conf);
		conf = NULL;
		goto out;
	}
	if (nvmeibt_offset_and_idx_is_uninitialized(conf->idx)) {	// Did not contain a kafka_offset
		N_Ef(evsr34a, "Got a conf buf (not json) with no kafka_offset");
	}
	N_Tf(gvsayum, "n_vols=@INT", conf->num_vols);
	conf->volumes = (struct mm_vol_conf *)NNVMEIBT_BM_CALLOC(mem_mgmt_32,  conf->num_vols*sizeof(struct mm_vol_conf));
	for (i=0; i<conf->num_vols; i++) {
		struct mm_vol_conf *vol = &conf->volumes[i];
		wire_in_p += nvmeibt_vol_convert_config_le_be((void *)wire_in_p, vol, false);
		serialize_vol_conf_to_JSON(vol, JSON_output);
		N_Tf(fjs03kw, "n_chunks=@INT", vol->num_chunks);
		vol->chunks = (struct mm_chunk_conf *)NNVMEIBT_BM_CALLOC(mem_mgmt_33,  vol->num_chunks*sizeof(struct mm_chunk_conf));
		for (j=0; j<vol->num_chunks; j++) {
			struct mm_chunk_conf *chunk = &vol->chunks[j];
			wire_in_p += nvmeibt_chunk_convert_config_le_be((void *)wire_in_p, chunk, false);
			serialize_chunk_conf_to_JSON(chunk, JSON_output);
			N_Tf(kdondtx, "n_praids=@INT", chunk->num_praids);
			chunk->praids = (struct mm_praid_conf *)NNVMEIBT_BM_CALLOC(mem_mgmt_34,  chunk->num_praids*sizeof(struct mm_praid_conf));
			for (k=0; k<chunk->num_praids; k++) {
				struct mm_praid_conf *praid = &chunk->praids[k];
				wire_in_p += nvmeibt_praid_convert_config_le_be((void *)wire_in_p, praid, false);
				serialize_praid_conf_to_JSON(praid, JSON_output);
				N_Tf(jdsfbvh, "n_segs=@INT", praid->num_segments);
				praid->segments = (struct mm_segment_conf *)NNVMEIBT_BM_CALLOC(mem_mgmt_35,  (praid->num_segments + 1)*sizeof(struct mm_segment_conf));	// +1 for replacement segs
				for (l=0; l<praid->num_segments; l++) {
					struct mm_segment_conf *seg = &praid->segments[l];
					wire_in_p += nvmeibt_seg_convert_config_le_be((void *)wire_in_p, seg, false);
					serialize_seg_conf_to_JSON(seg, JSON_output);
					serialize_end_of_array_obj_to_JSON(l, praid->num_segments, 1, JSON_output);
				}
				serialize_end_of_array_obj_to_JSON(k, chunk->num_praids, 1, JSON_output);
			}
			serialize_end_of_array_obj_to_JSON(j, vol->num_chunks, 1, JSON_output);
		}
		serialize_end_of_array_obj_to_JSON(i, conf->num_vols, 0, JSON_output);
	}
	if (JSON_output) {
		nvmeibt_Str_sprintf(JSON_output, "\n}");
	}
	if (strcmp(wire_in_p, EYECATCHER_CNF_END) != 0) {
		N_Wf(sskki93, "Incorrect eyecatcher at end of packed configuration! eyecatcher=@STR should be @STR", (const char *)wire_in_p, EYECATCHER_CNF_END);
	}

out:
	NFOUT;
	return conf;
}

#include "nvmeibt_global.h"
#include "nvmeibt_raft.h"
void serialize_mm_mgmt_conf_itself(struct mm_mgmt_conf *mgmt_conf, char *eyecatcher, uint16_t structVersion, uint16_t protocolVersion, int64_t configurationVersion, const union nvmeib_uuid *dbUUID,
								   char *messageType, int64_t messageTypeVersion, int num_vols, int num_disks, int num_nodes, int64_t idx)
{
	NFIN;
	memset(mgmt_conf, 0, sizeof(*mgmt_conf));
	memcpy(mgmt_conf->eyecatcher, eyecatcher, sizeof(mgmt_conf->eyecatcher));
	mgmt_conf->structVersion = structVersion;
	mgmt_conf->protocolVersion = protocolVersion;
	mgmt_conf->configurationVersion = configurationVersion;
	mgmt_conf->idx = idx;
	mgmt_conf->dbUUID = *dbUUID;
	nvmeibt_strlcpy(mgmt_conf->messageType, messageType, sizeof(mgmt_conf->messageType));
	mgmt_conf->messageTypeVersion = messageTypeVersion;
	mgmt_conf->num_vols = num_vols;
	mgmt_conf->num_disks = num_disks;
	mgmt_conf->num_nodes = num_nodes;
	NFOUT;
}

static int generate_vols_topo_config_wire(void *wire_out_p, uint32_t *total_n_vols, void *end_of_buf_p)
{
	int												i, j;
	int												n_vols, n_chunks, n_praids, n_segs;
	struct nvmeibt_block_device						*blkdev;
	int												total_size;
	struct _packed_mm_vol_conf						*vol_wire;
	struct _packed_mm_praid_conf					dummy_wire_data;

	NFIN;
	n_vols = 0;
	n_chunks = 0;
	n_praids = 0;
	n_segs = 0;

	NVMEIB_HASH_FOREACH(blkdev, nvmeibt_global_get_global()->block_devices_hash_by_uuid) {
		if (NVMEIBT_OBJ_IS_MARKED_OUTDATED(blkdev) || nvmeibt_blkdev_is_being_deleted(blkdev))
			continue;
		n_vols++;
		if (wire_out_p) {
			vol_wire = (typeof(vol_wire))wire_out_p;
			wire_out_p += nvmeibt_vol_convert_config_le_be(wire_out_p, &blkdev->serialized_vol_conf, true);
		}
		n_chunks += blkdev->n_chunks;
		for (i = 0; i < blkdev->n_chunks; i++) {
			struct nvmeibt_chunk *chunk = blkdev->chunks[i];
			if (wire_out_p)
				wire_out_p += nvmeibt_chunk_convert_config_le_be(wire_out_p, &chunk->its_mm_chunk_conf, true);
			n_praids += chunk->n_praids;
			for (j = 0; j < chunk->n_praids; j++) {
				struct nvmeibt_praid			*praid = chunk->praids[j];
				struct _packed_mm_praid_conf	*praid_wire_data;
				size_t							praid_wire_data_len;
				uint8_t							n_praid_segs;

				if (praid && (praid->praid_leader.topo_config_praid_and_segs_wire_conf_buf.buf_len > 0)) {
					praid_wire_data = praid->praid_leader.topo_config_praid_and_segs_wire_conf_buf.data_buf;
					praid_wire_data_len = praid->praid_leader.topo_config_praid_and_segs_wire_conf_buf.buf_len;
				} else {
					// Handling of a praid that exists in config but never got its first topo calculated. This topo_config will be replaced with one that matches its next topo
					struct mm_praid_conf premature_unusable_praid_serialized_conf =
							{.eyecatcher="PRD", .uuid.ll[0]=0xa1a2a3a4a5a6a7a8, .uuid.ll[1]=0, .stripeIndex=ILLEGAL_STRIPE_INDEX, .num_segments=0};
					TODO(remove when topo_config contains only pRAIDs);

					nvmeibt_praid_convert_config_le_be(&dummy_wire_data, &premature_unusable_praid_serialized_conf, true);
					praid_wire_data = &dummy_wire_data;
					praid_wire_data_len = sizeof(dummy_wire_data);
				}
				n_praid_segs = LE_SWAP8(praid_wire_data->num_segments);
				// Copy the already prepared leader wire bufs
				if (wire_out_p) {
					memcpy(wire_out_p, praid_wire_data, praid_wire_data_len);
					wire_out_p += praid_wire_data_len;
					N_Tf(ytcfyiq, "serialized n_praid_segs=@INT", n_praid_segs);
				}
				n_segs += n_praid_segs;
			}
		}
	}
	*total_n_vols = n_vols;
	total_size = (*total_n_vols * sizeof(struct _packed_mm_vol_conf) +
				  n_chunks * sizeof(struct _packed_mm_chunk_conf) +
				  n_praids * sizeof(struct _packed_mm_praid_conf) +
				  n_segs * sizeof(struct _packed_mm_segment_conf) +
				  sizeof(EYECATCHER_CNF_END));
	if (wire_out_p)
		nvmeibt_strlcpy(wire_out_p, EYECATCHER_CNF_END, end_of_buf_p - wire_out_p);
	else
		N_Tf(wjj82bg, "n_vols=@INT n_chunks=@INT n_praids=@INT, n_segs=@INT total_size=@INT",
			 n_vols, n_chunks, n_praids, n_segs, total_size);
	NFOUT;
	return total_size;
}

static int generate_vols_kafka_mgmt_config_wire(void *wire_out_p, uint32_t *total_n_vols, void *end_of_buf_p)
{
	int												n_vols = 0;
	struct nvmeibt_block_device						*vol;
	int												total_size = 0;

	NFIN;
	NVMEIB_HASH_FOREACH(vol, nvmeibt_global_get_global()->block_devices_hash_by_uuid) {
		if (NVMEIBT_OBJ_IS_MARKED_OUTDATED(vol) || nvmeibt_blkdev_is_being_deleted(vol))
			continue;
		n_vols++;
		if (wire_out_p) {
			memcpy(wire_out_p, vol->kafka_mgmt_config_vol_chunks_praids_segs_wire_conf_buf.data_buf, vol->kafka_mgmt_config_vol_chunks_praids_segs_wire_conf_buf.buf_len);
			wire_out_p += vol->kafka_mgmt_config_vol_chunks_praids_segs_wire_conf_buf.buf_len;
		}
		total_size += vol->kafka_mgmt_config_vol_chunks_praids_segs_wire_conf_buf.buf_len;
	}
	total_size += sizeof(EYECATCHER_CNF_END);
	if (wire_out_p) {
		nvmeibt_strlcpy(wire_out_p, EYECATCHER_CNF_END, end_of_buf_p - wire_out_p);
	}
	else {
		N_Tf(hu88qq1, "n_vols=@INT", n_vols);
	}
	NFOUT;
	*total_n_vols = n_vols;
	return total_size;
}

void nvmeibt_mm_jason_leader_topo_config_mm_praid_conf_and_mm_segs_conf_to_wire_buf(struct nvmeibt_praid *praid)
{
	struct nvmeibt_praid_leader		*praid_leader = &(praid->praid_leader);
	void 							*wire_out_p;
	int								i;

	if (NVMEIBT_OBJ_IS_MARKED_OUTDATED(praid)) {
		NNVMEIBT_BUF_FREE(tcvswkr, &(praid_leader->topo_config_praid_and_segs_wire_conf_buf));
		goto out;
	}
	NNVMEIBT_BUF_RESIZE(6dgqk3j, &(praid_leader->topo_config_praid_and_segs_wire_conf_buf),
						sizeof(struct _packed_mm_praid_conf) + praid_leader->serialized_topo_config_praid_and_its_segs_arr_conf.num_segments * sizeof(struct _packed_mm_segment_conf));
	memset(praid_leader->topo_config_praid_and_segs_wire_conf_buf.data_buf, 0, praid_leader->topo_config_praid_and_segs_wire_conf_buf.buf_len);
	wire_out_p = praid_leader->topo_config_praid_and_segs_wire_conf_buf.data_buf;
	wire_out_p += nvmeibt_praid_convert_config_le_be(wire_out_p, &praid_leader->serialized_topo_config_praid_and_its_segs_arr_conf, true);
	for (i = 0; i < praid_leader->serialized_topo_config_praid_and_its_segs_arr_conf.num_segments; i++) {
		wire_out_p += nvmeibt_seg_convert_config_le_be(wire_out_p, &(praid_leader->serialized_topo_config_praid_and_its_segs_arr_conf.segments[i]), true);
	}
out:;
}

void nvmeibt_mm_json_serialize_vol_and_chunks_and_praids_and_segs_kafka_mgmt_config_to_wire(struct nvmeibt_block_device *blkdev, struct mm_vol_conf *vol)
{
	void					*wire_out_p;
	size_t					size;
	struct mm_chunk_conf	*chunk;
	struct mm_praid_conf	*praid;
	int						j, k, l;

	NFIN;
	if (NVMEIBT_OBJ_IS_MARKED_OUTDATED(blkdev)) {
		NNVMEIBT_BUF_FREE(cvaje35, &(blkdev->kafka_mgmt_config_vol_chunks_praids_segs_wire_conf_buf));
		goto out;
	}
	size = sizeof(struct _packed_mm_vol_conf) + vol->num_chunks * sizeof(struct _packed_mm_chunk_conf);
	for (j = 0; j < vol->num_chunks; j++) {
		chunk = &vol->chunks[j];
		size += (chunk->num_praids * sizeof(struct _packed_mm_praid_conf));
		for (k = 0; k < chunk->num_praids; k++) {
			size += (chunk->praids[k].num_segments * sizeof(struct _packed_mm_segment_conf));
		}
	}
	NNVMEIBT_BUF_RESIZE(wfqj4mw, &(blkdev->kafka_mgmt_config_vol_chunks_praids_segs_wire_conf_buf), size);
	memset(blkdev->kafka_mgmt_config_vol_chunks_praids_segs_wire_conf_buf.data_buf, 0, blkdev->kafka_mgmt_config_vol_chunks_praids_segs_wire_conf_buf.buf_len);
	wire_out_p = blkdev->kafka_mgmt_config_vol_chunks_praids_segs_wire_conf_buf.data_buf;
	wire_out_p += nvmeibt_vol_convert_config_le_be(wire_out_p, vol, true);
	for (j = 0; j < vol->num_chunks; j++) {
		chunk = &vol->chunks[j];
		wire_out_p += nvmeibt_chunk_convert_config_le_be(wire_out_p, chunk, true);
		for (k = 0; k < chunk->num_praids; k++) {
			praid = &chunk->praids[k];
			wire_out_p += nvmeibt_praid_convert_config_le_be(wire_out_p, praid, true);
			for (l = 0; l < praid->num_segments; l++) {
				wire_out_p += nvmeibt_seg_convert_config_le_be(wire_out_p, &praid->segments[l], true);
			}
		}
	}
out:
	NFOUT;
}

void nvmeibt_mm_json_leader_serialize_baseline_topo_config_to_wire(uint64_t topo_config_version)
{
	struct nvmeibt_Buf								*wire_conf_buf;
	void											*wire_out_p;
	unsigned int									size;
	uint32_t										n_vols;
	struct nvmeibt_topology							*cur_topo = nvmeibt_global_get_global();
	struct mm_mgmt_conf								mgmt_conf;

	NFIN;
	wire_conf_buf = &(nvmeibt_raft_get_my_raft()->leader_to_commit_wire_topo_config_complete);
	wire_out_p = wire_conf_buf->data_buf;

	size = generate_vols_topo_config_wire(NULL, &n_vols, NULL) + sizeof(struct mm_mgmt_conf);
	NNVMEIBT_BUF_RESIZE(xh7610m, wire_conf_buf, size);
	memset(wire_conf_buf->data_buf, 0, wire_conf_buf->buf_len);
	wire_out_p = wire_conf_buf->data_buf;
	cur_topo->leader_config_version = (topo_config_version == -1ULL ? nvmeibt_topology_leader_get_next_config_version() : topo_config_version);
	serialize_mm_mgmt_conf_itself(&mgmt_conf, "CNF", MM_STRUCT_VER_2067, 1, cur_topo->leader_config_version, &cur_topo->mgmt_DB_uuid, "fullVolConfig", 1LL, n_vols, 0, 0,
								  RAFT_COMMIT_LIFECYCLE_VAL(TOPO_CONFIG, leader_to_commit));
	wire_out_p += nvmeibt_mm_mgmt_convert_config_le_be(wire_out_p, &mgmt_conf, 1);
	generate_vols_topo_config_wire(wire_out_p, &n_vols, wire_conf_buf->data_buf + size);

	nvmeibt_topology_mark_update_csv_of_config_and_topo_required();
	N_Tf(nnbh334, "New conf ver=@LLD len=@INT", cur_topo->leader_config_version, size);
	NFOUT;
}

void nvmeibt_mm_json_leader_serialize_kafka_mgmt_config_to_wire(void)
{
	struct nvmeibt_Buf								*wire_conf_buf;
	void											*wire_out_p;
	unsigned int									size;
	uint32_t										n_vols;
	struct nvmeibt_topology							*cur_topo = nvmeibt_global_get_global();
	struct mm_mgmt_conf								mgmt_conf;

	NFIN;
	wire_conf_buf = &(nvmeibt_raft_get_my_raft()->leader_to_commit_wire_kafka_mgmt_config_complete);
	size = generate_vols_kafka_mgmt_config_wire(NULL, &n_vols, NULL) + sizeof(struct _packed_mm_mgmt_conf);
	NNVMEIBT_BUF_RESIZE(viem2ms, wire_conf_buf, size);
	memset(wire_conf_buf->data_buf, 0, wire_conf_buf->buf_len);
	wire_out_p = wire_conf_buf->data_buf;
	serialize_mm_mgmt_conf_itself(&mgmt_conf, "CNF", MM_STRUCT_VER_2067, 1, -1LL, &cur_topo->mgmt_DB_uuid, "fullVolConfig", 1LL, n_vols, 0, 0,
								  RAFT_COMMIT_LIFECYCLE_VAL(KAFKA_MGMT_CONFIG, leader_calculated));
	wire_out_p += nvmeibt_mm_mgmt_convert_config_le_be(wire_out_p, &mgmt_conf, 1);
	generate_vols_kafka_mgmt_config_wire(wire_out_p, &n_vols, wire_conf_buf->data_buf + size);

	N_Tf(fndj392, "New conf k_offset=@INT64_TD len=@INT", RAFT_COMMIT_LIFECYCLE_VAL(KAFKA_MGMT_CONFIG, leader_calculated), size);
	NFOUT;
}

int nvmeibt_mm_json_mark_deleted_in_kafka_mgmt_config_vol_chunks_praids_segs_wire_conf_buf(void *data, size_t len)
{
	void							*wire_p;
	struct _packed_mm_vol_conf		*vol_wire;
	struct _packed_mm_chunk_conf	*chunk_wire;
	struct _packed_mm_praid_conf	*praid_wire;
	struct _packed_mm_segment_conf	*seg_wire;
	int								n_chunks, n_praids;
	int8_t							n_segs;
	int								j, k, l;

	if (!data || len == 0) {
		N_Tf(sajhriq, "data=@PTR len=@SIZE_T", data, len);
		goto out;
	}
	wire_p = data;
	vol_wire = (typeof(vol_wire))wire_p;
	wire_p += sizeof(*vol_wire);
	n_chunks = LE_SWAP8(vol_wire->num_chunks);
	N_Tf(6sgejk5, "vol=@STR", vol_wire->name);
	vol_wire->action = 'X';
	vol_wire->version = LE_SWAP32(VERSION_OF_DELETED_VOL);
	for (j = 0; j < n_chunks; j++) {
		chunk_wire = (typeof(chunk_wire))wire_p;
		wire_p += sizeof(*chunk_wire);
		n_praids = LE_SWAP8(chunk_wire->num_praids);
		for (k = 0; k < n_praids; k++) {
			praid_wire = (typeof(praid_wire))wire_p;
			wire_p += sizeof(*praid_wire);
			n_segs = LE_SWAP8(praid_wire->num_segments);
			for (l = 0; l < n_segs; l++) {
				seg_wire = (typeof(seg_wire))wire_p;
				wire_p += sizeof(*seg_wire);
				seg_wire->action = 'X';
			}
		}
	}
out:
	return 0;
}

/************************   JSON --> persistence   ****************************/

uint32_t							JSON_raft_ctx_crc;
struct nvmeibt_wire_type_len_value	JSON_tlv_raft_members;
struct nvmeibt_wire_type_len_value	JSON_tlv_kafka_mgmt_config;
struct nvmeibt_wire_type_len_value	JSON_tlv_full_topo_config_volumes;
struct nvmeibt_wire_type_len_value	JSON_tlv_topo_full;

static enum NVMEIBT_MEM_TBL_INIT_MODE mem_tbl_init_mode_str_to_enum(const char *init_mode_str)
{
	enum NVMEIBT_MEM_TBL_INIT_MODE	rv;

	if      (strcmp(init_mode_str, "INIT_DONE") == 0)			rv = NVMEIBT_MEM_TBL_INIT_MODE_INIT_DONE;
	else if (strcmp(init_mode_str, "INIT_IRRELEVANT") == 0)		rv = NVMEIBT_MEM_TBL_INIT_MODE_INIT_IRRELEVANT;
	else if (strcmp(init_mode_str, "INIT_REQUIRED") == 0)		rv = NVMEIBT_MEM_TBL_INIT_MODE_INIT_REQUIRED;
	else if (strcmp(init_mode_str, "FIRST_USE_EVER") == 0)		rv = NVMEIBT_MEM_TBL_INIT_MODE_FIRST_USE_EVER;
	else if (strcmp(init_mode_str, "FROM_PERSIST") == 0)		rv = NVMEIBT_MEM_TBL_INIT_MODE_FROM_PERSIST;
	else if (strcmp(init_mode_str, "TURN_ALL_ON") == 0)			rv = NVMEIBT_MEM_TBL_INIT_MODE_TURN_ALL_ON;
	else if (strcmp(init_mode_str, "TURN_ALL_OFF") == 0)		rv = NVMEIBT_MEM_TBL_INIT_MODE_TURN_ALL_OFF;
	else if (strcmp(init_mode_str, "UNUSED_0") == 0)			rv = NVMEIBT_MEM_TBL_INIT_MODE_UNUSED_0;
	else if (strcmp(init_mode_str, "UNKNOWN") == 0)				rv = NVMEIBT_MEM_TBL_INIT_MODE_UNKNOWN;
	else {
		N_Ef(i7dh4rn, "BAD input '@STR'", init_mode_str);
		rv = NVMEIBT_MEM_TBL_INIT_MODE_UNKNOWN;
	}
	N_Tf(cshgw28, "@STR: rv=@INT @STR", init_mode_str, rv, mem_tbl_init_mode_str(rv));
	return rv;
}

static enum NVMEIBT_SEGMENT_DIRTY_BITS_STATE dirty_bits_state_str_to_enum(const char *dirty_bits_str)
{
	enum NVMEIBT_SEGMENT_DIRTY_BITS_STATE	rv;

	if      (strcmp(dirty_bits_str, "OWNER_IDLE") == 0)					rv = NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_IDLE;
	else if (strcmp(dirty_bits_str, "DEAD") == 0)						rv = NVMEIBT_SEG_DIRTY_BITS_STATE_DEAD;
	else if (strcmp(dirty_bits_str, "X_ZERO") == 0)						rv = NVMEIBT_SEG_DIRTY_BITS_STATE_X_ZERO;
	else if (strcmp(dirty_bits_str, "X_DONE") == 0)						rv = NVMEIBT_SEG_DIRTY_BITS_STATE_X_DONE;
	else if (strcmp(dirty_bits_str, "UNDER_RECOVERY_R") == 0)			rv = NVMEIBT_SEG_DIRTY_BITS_STATE_UNDER_RECOVERY_R;
	else if (strcmp(dirty_bits_str, "OWNER_RECOVERER") == 0)			rv = NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_RECOVERER;
	else if (strcmp(dirty_bits_str, "EC_COLD_RECOVERER") == 0)			rv = NVMEIBT_SEG_DIRTY_BITS_STATE_EC_COLD_RECOVERER;
	else if (strcmp(dirty_bits_str, "EC_COLD_RECOVERER_DONE") == 0)		rv = NVMEIBT_SEG_DIRTY_BITS_STATE_EC_COLD_RECOVERER_DONE;
	else if (strcmp(dirty_bits_str, "OWNER_RECOVERED") == 0)			rv = NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_RECOVERED;
	else if (strcmp(dirty_bits_str, "OWNER_RECOVERER_DONE") == 0)		rv = NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_RECOVERER_DONE;
	else if (strcmp(dirty_bits_str, "UNDER_RECOVERY_I") == 0)			rv = NVMEIBT_SEG_DIRTY_BITS_STATE_UNDER_RECOVERY_I;
	else if (strcmp(dirty_bits_str, "ALIVE_STABLE") == 0)				rv = NVMEIBT_SEG_DIRTY_BITS_STATE_ALIVE_STABLE;
	else if (strcmp(dirty_bits_str, "ALIVE_UNSTABLE") == 0)				rv = NVMEIBT_SEG_DIRTY_BITS_STATE_ALIVE_UNSTABLE;
	else if (strcmp(dirty_bits_str, "UNKNOWN") == 0)					rv = NVMEIBT_SEG_DIRTY_BITS_STATE_UNKNOWN;
	else if (strcmp(dirty_bits_str, "UNUSED_0") == 0)					rv = NVMEIBT_SEG_DIRTY_BITS_STATE_UNUSED_0;
	else {
		N_Ef(3892nsd, "BAD input '@STR'", dirty_bits_str);
		rv = NVMEIBT_SEG_DIRTY_BITS_STATE_UNKNOWN;
	}
	N_Tf(bhej4i2, "@STR: rv=@INT @STR", dirty_bits_str, rv, dirty_bits_state_str(rv));
	return rv;
}

static enum PRAID_REGISTRANTS_SYNC_CMD praid_registrants_sync_cmd_str_to_enum(const char *sync_cmd_str)
{
	enum PRAID_REGISTRANTS_SYNC_CMD	rv;

	if      (strcmp(sync_cmd_str, "STABLE") == 0)							rv = PRAID_REGISTRANTS_SYNC_CMD_STABLE;
	else if (strcmp(sync_cmd_str, "DELETE") == 0)							rv = PRAID_REGISTRANTS_SYNC_CMD_DELETE;
	else if (strcmp(sync_cmd_str, "SWITCH_TOPO_U") == 0)					rv = PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_U;
	else if (strcmp(sync_cmd_str, "EC_COLD_RECOVERY_R") == 0)				rv = PRAID_REGISTRANTS_SYNC_CMD_EC_COLD_RECOVERY_R;
	else if (strcmp(sync_cmd_str, "STABLE_I") == 0)							rv = PRAID_REGISTRANTS_SYNC_CMD_STABLE_I;
	else if (strcmp(sync_cmd_str, "RESET_REGISTRANTS") == 0)				rv = PRAID_REGISTRANTS_SYNC_CMD_RESET_REGISTRANTS;
	else if (strcmp(sync_cmd_str, "SWITCH_TOPO_I") == 0)					rv = PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_I;
	else if (strcmp(sync_cmd_str, "SWITCH_TOPO_W") == 0)					rv = PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_W;
	else if (strcmp(sync_cmd_str, "SWITCH_TOPO_D") == 0)					rv = PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_D;
	else if (strcmp(sync_cmd_str, "SW_TOPO_STABLE_UNSAFE") == 0)			rv = PRAID_REGISTRANTS_SYNC_CMD_SW_TOPO_STABLE_UNSAFE;
	else if (strcmp(sync_cmd_str, "SW_TOPO_STABLE_SAFE") == 0)				rv = PRAID_REGISTRANTS_SYNC_CMD_SW_TOPO_STABLE_SAFE;
	else if (strcmp(sync_cmd_str, "DEPRECATED_SWITCH_TOPO_DX") == 0)		rv = PRAID_REGISTRANTS_SYNC_CMD_DEPRECATED_SWITCH_TOPO_DX;
	else if (strcmp(sync_cmd_str, "SWITCH_TOPO_X") == 0)					rv = PRAID_REGISTRANTS_SYNC_CMD_SWITCH_TOPO_X;
	else if (strcmp(sync_cmd_str, "EC_COLD_RECOVERY_I") == 0)				rv = PRAID_REGISTRANTS_SYNC_CMD_EC_COLD_RECOVERY_I;
	else if (strcmp(sync_cmd_str, "UNUSED_0") == 0)							rv = PRAID_REGISTRANTS_SYNC_CMD_UNUSED_0;
	else if (strcmp(sync_cmd_str, "UNKNOWN") == 0)							rv = PRAID_REGISTRANTS_SYNC_CMD_UNKNOWN;
	else {
		N_Ef(5xgqk3d, "BAD input '@STR'", sync_cmd_str);
		rv = PRAID_REGISTRANTS_SYNC_CMD_UNKNOWN;
	}
	N_Tf(n2aukef, "@STR: rv=@INT @STR", sync_cmd_str, rv, praid_registrants_sync_cmd_str(rv));
	return rv;
}

/******************************************************************************/

int parse_raft_ctx_JSON(struct mm_json_dict *dict)
{
	struct mm_json_kv_pair		*kv;
	int							rv = 0;
	JSON_ASSIGN_AND_CALL_INIT();

	NFIN;
	JSON_LOOP_FOR_DICT(kv, dict) {
		JSON_LOOP_ITERATION_START(ryjsilw, kv->key);
		JSON_ASSIGN_PLAIN(7yhe9ow, "current_term", nvmeibt_raft_get_my_raft()->current_term, kv->value->num);
		JSON_ASSIGN_PLAIN(ysbj7nd, "last_rx_append_entries_term", nvmeibt_raft_get_my_raft()->last_rx_append_entries_term, kv->value->num);
		JSON_ASSIGN_CALL(4kstiwl,  "kafka_mgmt_zone_number", nvmeibt_kafka_new_kafka_mgmt_zone_number_received, kv->value->num);
		JSON_ASSIGN_CALL(vbsh8wl,  "voted_for_raft_member_uuid", nvmeibt_raft_set_voted_for_uuid, GET_UNION_UUID_OF_URN_UUID_STR(kv->value->str));
		JSON_ASSIGN_CALL(y1pxbgw,  "mgmt_DB_uuid", nvmeibt_global_validate_and_upd_mgmt_DB_uuid, GET_UNION_UUID_OF_URN_UUID_STR(kv->value->str));
		JSON_ASSIGN_PLAIN(cfgs8i3, "raft_ctx_crc", JSON_raft_ctx_crc, (uint32_t)kv->value->num);
		JSON_ASSIGN_CALL(6g8akmw,  "guaranteed_sw_ver", nvmeibt_raft_set_guaranteed_sw_ver, (uint32_t)(kv->value->num));
		JSON_ASSIGN_CALL(rvghsui,  "calculated_append_entries_rep_time_ns", nvmeibt_raft_leader_set_calculated_append_entries_rep_time_ns_USED_ONLY_BY_JSON_PARSER, kv->value->num);
		JSON_ASSIGN_CALL(5gs8l5m,  "calculated_topo_calc_time_ns", nvmeibt_raft_leader_set_calculated_topo_calc_time_ns_USED_ONLY_BY_JSON_PARSER, kv->value->num);
		JSON_LOOP_ITERATION_END(5ak30ol, kv->key);
	}
	JSON_ASSIGN_AND_CALL_VALIDATE(vgskwoa);
	NFOUT;
	return rv;
}

int parse_generic_tlv_JSON(struct mm_json_dict *dict, struct nvmeibt_wire_type_len_value *tlv)
{
	struct mm_json_kv_pair					*kv;
	int										rv = 0;
	JSON_ASSIGN_AND_CALL_INIT();

	NFIN;
	JSON_LOOP_FOR_DICT(kv, dict) {
		JSON_LOOP_ITERATION_START(krueove, kv->key);
		// The JSON is LE. Need to convert to BE (sometimes only an int out of int64)
		JSON_ASSIGN_PLAIN(v84io29, "type", tlv->UNUSED_tlv_type, LE_SWAP8((int8_t)(kv->value->num)));
		JSON_ASSIGN_PLAIN(aq9pk4i, "len", tlv->tlv_len, LE_SWAP32((uint32_t)(kv->value->num)));
		JSON_ASSIGN_PLAIN(o04byxa, "idx", tlv->tlv_idx, LE_SWAP64(kv->value->num));
		JSON_ASSIGN_PLAIN(vhsn89q, "seq_no", tlv->seq_no, LE_SWAP64(kv->value->num));
		JSON_ASSIGN_PLAIN(vhsn89q, "crc", tlv->tlv_crc, LE_SWAP32((uint32_t)(kv->value->num)));
		JSON_LOOP_ITERATION_END(vah9wol, kv->key);
	}
	JSON_ASSIGN_AND_CALL_VALIDATE(vsdh74j);
	NFOUT;
	return rv;
}

int parse_raft_member_JSON(struct mm_json_dict *dict)
{
	struct mm_json_kv_pair		*kv;
	int							rv = 0;
	int64_t						n = -1;
	int64_t						kafka_offset = -1;
	char						hostname[NVMEIB_HOST_NAME_LEN];
	char						eyecatcher[5];
	union nvmeib_uuid			uuid = nvmeib_uuid_null_val;
	JSON_ASSIGN_AND_CALL_INIT();

	NFIN;
	JSON_LOOP_FOR_DICT(kv, dict) {
		JSON_LOOP_ITERATION_START(2miusbt, kv->key);
		JSON_ASSIGN_PLAIN(rj3kis5, "n", n, kv->value->num);
		JSON_ASSIGN_STR(ujksl3n,   "eyecatcher", eyecatcher, kv->value->str);
		JSON_ASSIGN_PLAIN(a7bhdtq, "kafka_offset", kafka_offset, kv->value->num);
		JSON_ASSIGN_STR(5bh39l0,   "hostname", hostname, kv->value->str);
		JSON_ASSIGN_PLAIN(uhspwb5, "uuid", uuid, *GET_UNION_UUID_OF_URN_UUID_STR(kv->value->str));
		JSON_LOOP_ITERATION_END(n4uajfo, kv->key);
	}
	JSON_ASSIGN_AND_CALL_VALIDATE(763bns8);
	NTOMA_ASSERT(gus0mn3, strcmp(eyecatcher, "MMB") == 0, "eycatcher=@STR", eyecatcher);
	{	// Build a dummy node
		struct mm_node_conf			dummy_node_conf = {0};

		nvmeibt_strlcpy(dummy_node_conf.eyecatcher, "NOD", sizeof(dummy_node_conf.eyecatcher));
		dummy_node_conf.version = -1;
		dummy_node_conf.num_nics = 0;
		dummy_node_conf.nics = NULL;
		nvmeibt_strlcpy(dummy_node_conf.node_id, hostname, min(sizeof(hostname), sizeof(dummy_node_conf.node_id)));
		dummy_node_conf.uuid = uuid;

		nvmeibt_node_add(&dummy_node_conf, CONFIG_TAG_OUTDATED + 2);
	}
	nvmeibt_raft_add_member(hostname, n, &uuid, 0, kafka_offset, CONFIG_TAG_OUTDATED + 2);
	NFOUT;
	return rv;
}

int parse_raft_members_array_JSON(struct mm_json_elem *root)
{
	int							i;
	int							rv = 0;

	NFIN;
	if (root->type != JSON_E_ARRAY) {
		N_ETf(xnj29wl, "raft_members is not an array type=@INT", root->type);
	}
	for (i = 0; i < root->array.len; i++) {
		parse_raft_member_JSON(&(root->array.elements[i]->dict));
	}
	NFOUT;
	return rv;
}

static int imitate_I_am_the_leader(void)
{
	const union nvmeib_uuid			*voted_for_uuid;
	struct nvmeibt_raft_member		*my_immitated_member;
	int								rv = 0;

	NFIN;
	voted_for_uuid = nvmeibt_raft_get_voted_for_uuid();
	my_immitated_member = nvmeibt_raft_get_member_by_id(voted_for_uuid);
	nvmeibt_raft_get_my_raft()->my_member = my_immitated_member;
	my_immitated_member->is_me = 1;
	nvmeibt_raft_set_leader_uuid(voted_for_uuid);
	//
	nvmeibt_raft_leader_copy_committed_persist_and_wire_buf_sections_into_separate_to_commit_bufs();
	nvmeibt_raft_convert_to_leader();
	NFOUT;
	return rv;
}

int apply_json_topo_hdr(struct mm_json_dict *dict)
{
	struct mm_json_kv_pair							*kv;
	int												rv = 0;
	struct nvmeibt_topology_serialized_topo_header	header = {0};
	JSON_ASSIGN_AND_CALL_INIT();

	NFIN;
	JSON_LOOP_FOR_DICT(kv, dict) {
		JSON_LOOP_ITERATION_START(bt83kwo, kv->key);
		JSON_ASSIGN_PLAIN(kjwo2si, "sw_ver", header.sw_ver, kv->value->num);
		JSON_ASSIGN_PLAIN(zc5s0o2, "topo_len", header.topo_len, kv->value->num);
		JSON_ASSIGN_PLAIN(hdnvk3k,  "praids_num", header.praids_num, kv->value->num);
		JSON_LOOP_ITERATION_END(j5wlox3, kv->key);
	}
	N_Tf(cvgahew, "topo_hdr sw_ver=@INT64_TD topo_len=@INT64_TD praids_num=@INT64_TD",
		 header.sw_ver, header.topo_len, header.praids_num);
	JSON_ASSIGN_AND_CALL_VALIDATE(9vuwmrd);
	N_Tf(vgshgwu, "Actually, Nothing to apply in the header. Used for validations");
	NFOUT;
	return rv;
}

int apply_json_topo_segs_committed(struct mm_json_array *arr)
{
	struct mm_json_kv_pair						*kv;
	struct mm_json_dict							*seg_dict;
	int											i;
	int											rv = 0;
	struct nvmeibt_serialized_seg_leader_topo	topo = {0};

	NFIN;
	for (i = 0; i < arr->len; i++) {
		JSON_ASSIGN_AND_CALL_INIT();
		seg_dict = &(arr->elements[i]->dict);
		JSON_LOOP_FOR_DICT(kv, seg_dict) {
			JSON_LOOP_ITERATION_START(vgs7wi9, kv->key);
			JSON_ASSIGN_STR(moneios,   "eyecatcher", topo.eyecatcher, kv->value->str);
			JSON_ASSIGN_PLAIN(7h40sl3, "uuid", topo.uuid, *GET_UNION_UUID_OF_URN_UUID_STR(kv->value->str));
			JSON_ASSIGN_PLAIN(vsgai2q, "praid_version_major", topo.praid_version_major, kv->value->num);
			JSON_ASSIGN_PLAIN(hinsk3l, "praid_version_minor", topo.praid_version_minor, kv->value->num);
			JSON_ASSIGN_PLAIN(1ninude, "has_ram_survived", topo.leader_seg_flags.has_ram_survived, kv->value->num);
			JSON_ASSIGN_PLAIN(8dj4owo, "is_newly_added_seg", topo.leader_seg_flags.is_newly_added_seg, kv->value->num);
			JSON_ASSIGN_PLAIN(ksl3vk3, "is_owner_ram_recoverable_stable", topo.leader_seg_flags.is_owner_ram_recoverable_stable, kv->value->num);
			JSON_ASSIGN_PLAIN(69dlj3n, "is_drive_write_error", topo.leader_seg_flags.is_drive_write_error, kv->value->num);
			JSON_ASSIGN_PLAIN(cimnep2, "dirty_bits_state", topo.dirty_bits_state, dirty_bits_state_str_to_enum(kv->value->str));
			JSON_ASSIGN_PLAIN(8x90wls, "dirty_bits_init_mode", topo.dirty_bits_init_mode, mem_tbl_init_mode_str_to_enum(kv->value->str));
			JSON_ASSIGN_PLAIN(skmvixu, "stale_locks_init_mode", topo.stale_locks_init_mode, mem_tbl_init_mode_str_to_enum(kv->value->str));
			JSON_ASSIGN_PLAIN(3ns9sl2, "txid_init_mode", topo.txid_init_mode, mem_tbl_init_mode_str_to_enum(kv->value->str));
			JSON_ASSIGN_PLAIN(mhnpdxe, "seg_idx", topo.seg_idx, kv->value->num);
			JSON_ASSIGN_PLAIN(esi302l, "owner_idx", topo.owner_idx, kv->value->num);
			JSON_ASSIGN_PLAIN(y38ixl3, "secondary_owner_idx", topo.secondary_owner_idx, kv->value->num);
			JSON_ASSIGN_PLAIN(sniwl2pw, "is_registrants_synchronizer", topo.is_registrants_synchronizer, kv->value->num);
			JSON_LOOP_ITERATION_END(usulern, kv->key);
		}
		JSON_ASSIGN_AND_CALL_VALIDATE(7s9xkef);
		N_Tf(xnisme3, "eyecatcher=@STR uuid=@UUID_LE praid_version_major=@INT praid_version_minor=@INT has_ram_survived=@UINT is_newly_added_seg=@UINT "
			 "is_owner_ram_recoverable_stable=@INT is_drive_write_error=@INT dirty_bits_state=@INT dirty_bits_init_mode=@INT "
			 "stale_locks_init_mode=@INT txid_init_mode=@INT seg_idx=@INT secondary_owner_idx=@INT is_registrants_synchronizer@BOOL",
			 topo.eyecatcher, &(topo.uuid), topo.praid_version_major, topo.praid_version_minor, topo.leader_seg_flags.has_ram_survived, topo.leader_seg_flags.is_newly_added_seg,
			 topo.leader_seg_flags.is_owner_ram_recoverable_stable, topo.leader_seg_flags.is_drive_write_error, topo.dirty_bits_state, topo.dirty_bits_init_mode,
			 topo.stale_locks_init_mode, topo.txid_init_mode, topo.seg_idx, topo.secondary_owner_idx, topo.is_registrants_synchronizer);
		nvmeibt_seg_follower_upd_committed_seg_topo(&topo);
		nvmeibt_disk_segment_get_disk_segment_by_id(&(topo.uuid))->seg_mgmt.its_disk = (struct nvmeibt_disk *)-1;    // Pass the disk existance test during serialization

	}
	NFOUT;
	return rv;
}

int apply_json_topo_praids_committed(struct mm_json_array *arr)
{
	struct mm_json_kv_pair					*kv;
	struct mm_json_dict						*praid_dict;
	int										i;
	int										rv = 0;
	struct nvmeibt_praid_serialized_topo	topo = {0};

	NFIN;
	for (i = 0; i < arr->len; i++) {
		JSON_ASSIGN_AND_CALL_INIT();
		praid_dict = &(arr->elements[i]->dict);
		JSON_LOOP_FOR_DICT(kv, praid_dict) {
			JSON_LOOP_ITERATION_START(jsi0wp2, kv->key);
			JSON_ASSIGN_STR(nucnelt,   "eyecatcher", topo.eyecatcher, kv->value->str);
			JSON_ASSIGN_PLAIN(1bcuxap, "uuid", topo.uuid, *GET_UNION_UUID_OF_URN_UUID_STR(kv->value->str));
			JSON_ASSIGN_PLAIN(0msnis3, "praid_version_major", topo.praid_version_major, kv->value->num);
			JSON_ASSIGN_PLAIN(qreniul, "praid_version_minor", topo.praid_version_minor, kv->value->num);
			JSON_ASSIGN_PLAIN(5h39jdo, "leader_did_all_segs_sync_registrants", topo.leader_did_all_segs_sync_registrants, kv->value->num);
			JSON_ASSIGN_PLAIN(mks93nf, "registrants_sync_cmd", topo.registrants_sync_cmd, praid_registrants_sync_cmd_str_to_enum(kv->value->str));
			JSON_ASSIGN_PLAIN(cosl4ls, "is_activated", topo.is_activated, kv->value->num);
			JSON_ASSIGN_PLAIN(lme9len, "segs_num", topo.segs_num, kv->value->num);
			JSON_ASSIGN_CALL(i6chrwe, "segments", apply_json_topo_segs_committed, &(kv->value->array));
			JSON_LOOP_ITERATION_END(5vs92lr, kv->key);
		}
		JSON_ASSIGN_AND_CALL_VALIDATE(jsol3oc);
		N_Tf(imcr5nb, "eyecatcher=@STR uuid=@UUID_LE praid_version_major=@INT praid_version_minor=@INT "
			 "leader_did_all_segs_sync_registrants=@BOOL registrants_sync_cmd=@INT is_activated=@BOOL segs_num=@INT",
			 topo.eyecatcher, &(topo.uuid), topo.praid_version_major, topo.praid_version_minor,
			 topo.leader_did_all_segs_sync_registrants, topo.registrants_sync_cmd, topo.is_activated, topo.segs_num);
		nvmeibt_praid_upd_committed_topo(&topo);
	}
	NFOUT;
	return rv;
}

static int apply_topo_json_tree_root(struct mm_json_dict *dict)
{
	struct mm_json_kv_pair		*kv;
	int							rv = 0;
	JSON_ASSIGN_AND_CALL_INIT();

	NFIN;
	JSON_LOOP_FOR_DICT(kv, dict) {
		JSON_LOOP_ITERATION_START(nskx93k, kv->key);
		JSON_ASSIGN_CALL(cmilsir, "topo_hdr", apply_json_topo_hdr, &(kv->value->dict));
		JSON_ASSIGN_CALL(65hbl20, "praids", apply_json_topo_praids_committed, &(kv->value->array));
		JSON_LOOP_ITERATION_END(jk3cw94, kv->key);
	}
	JSON_ASSIGN_AND_CALL_VALIDATE(vvyh4k2);
	NFOUT;
	return rv;
}

static int apply_parsed_JSON_tree_to_toma_objects(struct mm_json_elem *root)
{
	struct mm_json_kv_pair				*kv;
	int									rv = 0;
	struct mm_json_dict					*dict = &(root->dict);
	struct mm_mgmt_conf					*kafka_conf = NULL;
	struct mm_mgmt_conf					*topo_conf = NULL;
	//
	struct mm_json_elem					*raft_ctx_json_tree_root = NULL;
	struct mm_json_elem					*tlv_raft_members_json_tree_root = NULL;
	struct mm_json_elem					*raft_members_json_tree_root = NULL;
	struct mm_json_elem					*tlv_kafka_mgmt_config_json_tree_root = NULL;
	struct mm_json_elem					*kafka_mgmt_config_json_tree_root = NULL;
	struct mm_json_elem					*tlv_full_topo_config_volumes_json_tree_root = NULL;
	struct mm_json_elem					*full_topo_config_volumes_json_tree_root = NULL;
	struct mm_json_elem					*tlv_full_topo_json_tree_root = NULL;
	struct mm_json_elem					*full_topo_json_tree_root = NULL;
	JSON_ASSIGN_AND_CALL_INIT();

	NFIN;
	JSON_LOOP_FOR_DICT(kv, dict) {
		JSON_LOOP_ITERATION_START(hu3baut, kv->key);
		JSON_ASSIGN_PLAIN(5vsh2w3, "buf_sw_ver", nvmeibt_global_get_global()->persistent_toma_software_version, (int32_t)(kv->value->num));
		JSON_ASSIGN_PLAIN(6sb30o3, "raft_ctx", raft_ctx_json_tree_root, kv->value);
		JSON_ASSIGN_PLAIN(msio05o, "tlv_raft_members", tlv_raft_members_json_tree_root, kv->value);
		JSON_ASSIGN_PLAIN(sjhe040, "raft_members", raft_members_json_tree_root, kv->value);
		JSON_ASSIGN_PLAIN(pawnvgy, "tlv_KAFKA_MGMT_CONFIG_FULL", tlv_kafka_mgmt_config_json_tree_root, kv->value);
		JSON_ASSIGN_PLAIN(4vx6k20, "KAFKA_MGMT_CONFIG_FULL", kafka_mgmt_config_json_tree_root, kv->value);
		JSON_ASSIGN_PLAIN(cvhz8l3, "tlv_FULL_TOPO_CONFIG_VOLUMES", tlv_full_topo_config_volumes_json_tree_root, kv->value);
		JSON_ASSIGN_PLAIN(ubswqap, "FULL_TOPO_CONFIG", full_topo_config_volumes_json_tree_root, kv->value);
		JSON_ASSIGN_PLAIN(7vnke9d, "tlv_TOPO_FULL", tlv_full_topo_json_tree_root, kv->value);
		JSON_ASSIGN_PLAIN(ndjkc9l, "FULL_TOPO", full_topo_json_tree_root, kv->value);
		JSON_LOOP_ITERATION_END(vbux9le, kv->key);
	}
	JSON_ASSIGN_AND_CALL_VALIDATE(4bhxp4a);
	//
	rv |= parse_raft_ctx_JSON(&(raft_ctx_json_tree_root->dict));
	//
	rv |= parse_generic_tlv_JSON(&(tlv_raft_members_json_tree_root->dict), &JSON_tlv_raft_members);
	SET_RAFT_COMMIT_LIFECYCLE_VAL(ru8zxl3, RAFT_MEMBERS,        follower_committed, nvmeibt_tlv_get_idx(&JSON_tlv_raft_members)); // For nvmeibt_topology_serialize_conf_and_topo_if_needed()
	SET_RAFT_COMMIT_LIFECYCLE_VAL(5z9qpfj, RAFT_MEMBERS_SEQ_NO, follower_committed, nvmeibt_tlv_get_seq_no(&JSON_tlv_raft_members));		// For nvmeibt_topology_serialize_conf_and_topo_if_needed()
	N_Tf(vsgs4ta, "n_raft_members=@LLD", raft_members_json_tree_root->array.len);
	rv |= parse_raft_members_array_JSON(raft_members_json_tree_root);
	//
	rv |= parse_generic_tlv_JSON(&(tlv_kafka_mgmt_config_json_tree_root->dict), &JSON_tlv_kafka_mgmt_config);
	SET_RAFT_COMMIT_LIFECYCLE_VAL(ysn82la, KAFKA_MGMT_CONFIG, follower_committed, nvmeibt_tlv_get_idx(&JSON_tlv_kafka_mgmt_config));
	kafka_conf = (struct mm_mgmt_conf *)NNVMEIBT_BM_CALLOC(16wkixt,  sizeof(*kafka_conf));
	if (kafka_mgmt_config_json_tree_root)														// NULL if there are no volumes.
		JSON_persistence_tree_to_mgmt_conf(kafka_conf, kafka_mgmt_config_json_tree_root, -1);
	//
	rv |= parse_generic_tlv_JSON(&(tlv_full_topo_config_volumes_json_tree_root->dict), &JSON_tlv_full_topo_config_volumes);
	SET_RAFT_COMMIT_LIFECYCLE_VAL(zoqhy4d, TOPO_CONFIG, follower_committed, nvmeibt_tlv_get_idx(&JSON_tlv_full_topo_config_volumes));
	topo_conf = (struct mm_mgmt_conf *)NNVMEIBT_BM_CALLOC(osmirb3,  sizeof(*topo_conf));
	if (full_topo_config_volumes_json_tree_root)												// NULL if there are no volumes.
		JSON_persistence_tree_to_mgmt_conf(topo_conf, full_topo_config_volumes_json_tree_root, -1);
	//
	rv |= parse_generic_tlv_JSON(&(tlv_full_topo_json_tree_root->dict), &JSON_tlv_topo_full);
	SET_RAFT_COMMIT_LIFECYCLE_VAL(2miyahw, TOPO, follower_committed, nvmeibt_tlv_get_idx(&JSON_tlv_topo_full));
	//
	//
	imitate_I_am_the_leader();
	// Apply the conf only after applying raft & raft_members, when we can pretend to be the leader
	nvmeibt_read_config_apply_vol_mgmt_conf(kafka_conf, CONFIG_TAG_OUTDATED + 2, 1, KAFKA_EVENT_TYPE_VOL_ADD, 0);
	nvmeibt_read_config_apply_vol_committed_topo_conf(topo_conf, CONFIG_TAG_OUTDATED + 2);
	if (full_topo_json_tree_root)
		apply_topo_json_tree_root(&(full_topo_json_tree_root->dict));
	//	Serialize everything before generating the full persist_and_wire
	nvmeibt_topology_reset_due_to_convert_to_leader();	// committed-->baseline before serialization
	//
	nvmeibt_mm_json_leader_serialize_baseline_topo_config_to_wire((uint64_t)topo_conf->configurationVersion);
	//
	nvmeibt_topology_leader_serialize_baseline_topo_to_wire();
	nvmeibt_raft_leader_generate_leader_to_commit_wire_raft_members_buf();
	nvmeibt_mm_json_leader_serialize_kafka_mgmt_config_to_wire();
	//
	mm_conf_free_tree(kafka_conf);
	mm_conf_free_tree(topo_conf);
	return rv;
}

int nvmeibt_mm_json_read_JSON_and_generate_persist_and_wire(char *JSON_file_name)
{
	struct mm_json_elem						*json_tree_root = NULL;
	struct nvmeibt_Str						*JSON_buf = NULL;
	size_t									JSON_len;
	int		 								fd = -1;
	int										rv = 0;
	struct nvmeibt_persist_and_wire_buf		*persist_and_wire_buf;

	// Read the JSON file
	fd = NNVMEIBT_OPEN_READ(vctsjw9, JSON_file_name, 1);
	if (fd < 0) {
		N_ETf(ybhskw9, "Error while opening the file @STR. @AUTO_ERRNO", JSON_file_name);
		goto out;
	}
	JSON_buf = NNVMEIBT_STR_ALLOC(cbrai2g);
	JSON_len = NNVMEIBT_STR_FREAD(bsj39lw, JSON_buf, fd);
	NNVMEIBT_CLOSE(tmmjrfp1, fd);
	if (JSON_len < 10 || (JSON_len != nvmeibt_Str_strlen(JSON_buf))) {
		N_Ef(cxgw9ph, "Failed reading @STR JSON_len=@SIZE_T strlen=@SIZE_T", JSON_file_name, JSON_len, nvmeibt_Str_strlen(JSON_buf));
		goto out;
	}
	N_Tf(b8ski4x, "@STR JSON_len=@SIZE_T", JSON_file_name, JSON_len);

	// parse the JSON into a kv_tree
	json_tree_root = parse_json_txt_into_kv_tree(nvmeibt_Str_str(JSON_buf), nvmeibt_Str_strlen(JSON_buf));
	if (!json_tree_root) {
		N_Ef(cbjw9k3, "Failed parsing JSON");
		goto out;
	}

	// JSON tree to conf
	if (json_tree_root->type != JSON_E_DICT) {
		N_ETf(6qiznhb, "Bad JSON structure, invalid configuration received from management!");
		goto out;
	}
	if (json_tree_root->dict.len != 10) {
		N_WTf(cn0al3r, "dict.len=@INT", json_tree_root->dict.len);
//		goto out;
	}

	apply_parsed_JSON_tree_to_toma_objects(json_tree_root);

	// Generate persist&wire buf
	raft_leader_regenerate_the_to_commit_persist_and_wire_bufs_as_needed();
	// Compare the input & output CRCs
	persist_and_wire_buf = nvmeibt_raft_get_my_raft()->leader_to_commit_persist_and_wire_buf_full_complete;
	if (JSON_raft_ctx_crc != persist_and_wire_buf_get_raft_ctx_crc(persist_and_wire_buf)) {
		N_Wf(vghs2jh, "JSON_raft_ctx_crc=@UINT != persist_and_wire_buf->raft_ctx.raft_ctx_crc=@UINT", JSON_raft_ctx_crc, persist_and_wire_buf->raft_ctx.raft_ctx_crc);
	}
	if (memcmp(&JSON_tlv_raft_members, &(persist_and_wire_buf->raft_members_ctx), sizeof(JSON_tlv_raft_members)) != 0) {
		N_Wf(cubnak2, "tlv_RAFT_MEMBERS differ");
		DUMP_TLV(cfdygwe, &JSON_tlv_raft_members);
		DUMP_TLV(jkziout, &(persist_and_wire_buf->raft_members_ctx));
	}
	if (memcmp(&JSON_tlv_kafka_mgmt_config, &(persist_and_wire_buf->kafka_mgmt_config_ctx), sizeof(JSON_tlv_kafka_mgmt_config)) != 0) {
		N_Wf(bjsm8c7, "tlv_KAFKA_MGMT_CONFIG differ");
		DUMP_TLV(nakpq0d, &JSON_tlv_kafka_mgmt_config);
		DUMP_TLV(bsimw59, &(persist_and_wire_buf->kafka_mgmt_config_ctx));
	}
	if (memcmp(&JSON_tlv_full_topo_config_volumes, &(persist_and_wire_buf->topo_config_ctx), sizeof(JSON_tlv_full_topo_config_volumes)) != 0) {
		N_Wf(nj5is02, "tlv_TOPO_CONFIG differ");
		DUMP_TLV(diem7vy, &JSON_tlv_full_topo_config_volumes);
		DUMP_TLV(0mfin3x, &(persist_and_wire_buf->topo_config_ctx));
	}
	if (memcmp(&JSON_tlv_topo_full, &(persist_and_wire_buf->topo_ctx), sizeof(JSON_tlv_topo_full)) != 0) {
		N_Wf(3c7sk0g, "tlv_TOPO_FULL differ");
		DUMP_TLV(yn5ns5i, &JSON_tlv_topo_full);
		DUMP_TLV(2nqpfjw, &(persist_and_wire_buf->topo_ctx));
	}
out:
	nvmeibt_mm_json_free_kv_tree(json_tree_root);
	NNVMEIBT_STR_FREE(__AUTOID__, JSON_buf);
	NFOUT;
	return rv;
}
