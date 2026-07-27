/* This module is used to hold all of the client's required information
   regarding targets. Namely hold all of the formatted nics of the server
   as a linked list of arnics. Moreover we will hold for each target the
   VxD of the disk side:
   For each volume that holds a disk on the target it will have a nvmeibc_disk_id
   object pointing at the targets nics, and the volume and the target allowing ease
   of access to all three from the same point.
   When we get an update on a known target we will only call rediscovery if we
   added / updated any nic on the target.*/

#include "nvmeibc_targets.h"
#include "nvmeib_event.h"
#include "nvmeibc_volume.h"
#include "nvmeibc_cc_api.h"
#include "main/nvmeibc_main_common.h"
#include "nvmeibc_main.h"
#include "main/utils/nvmeibc_main_block_gen_work_sched.h"

uint nvmeibc_target_nics_query_min_fail_secs = 10;
module_param_named(nvmeibc_target_nics_query_min_fail_secs, nvmeibc_target_nics_query_min_fail_secs, uint, 0644);
MODULE_PARM_DESC(nvmeibc_target_nics_query_min_fail_secs, "Minimum number of seconds of discovery failures before sending a Target NICs query to management, for the whole target");

struct targets_global {
	struct list_head arnics_league;					// List of all the nics num of connections
	struct list_head targets_list;
	struct mutex score_companion_guard;
	const struct nvmeibc_cinst_params_main *cinst;
};

int nvmeibc_target_init_arnics_league(const struct nvmeibc_cinst_params_main *cinst)
{
	struct t_main_clnt_globals * _mg = __get_from_params_main_globals_container(cinst);
	int rv;
	struct targets_global *t;
	NFIN;
	if ((t = kzalloc(sizeof(*t), GFP_KERNEL))) {
		INIT_LIST_HEAD(&t->arnics_league);
		INIT_LIST_HEAD(&t->targets_list);
		mutex_init(&t->score_companion_guard);
		t->cinst = cinst;
		_mg->targets = t;
		rv = 0;
	}
	else {
		_NT(error_target_init_arnics_league, "fail to allocat targets global");
		rv = -1;
	}
	NFOUT;
	return rv;
}

void nvmeibc_target_free_arnic_league(const struct nvmeibc_cinst_params_main *o)
{
	struct t_main_clnt_globals * _mg = __get_from_params_main_globals_container(o);
	struct targets_global *t = _mg->targets;
	struct nvmeibc_arnic_score *score, *tmp;
	char gid[GUID_SIZE];
	int cons;

	NFIN;
	mutex_lock(&t->score_companion_guard);
	list_for_each_entry_safe(score, tmp, &t->arnics_league, link) {
		format_gid_raw(score->gid.raw, gid);
		cons = atomic_read(&score->n_connections);
		_NT(trace_targets_nvmeibc_target_free_arnic_league, "connection of @GID_STR is @CONS", gid, cons);
		if (cons) {
			_NT(trace_1_targets_nvmeibc_target_free_arnic_league, "not all the connections on nic @GID_STR are free", gid);
			WARN_ON(1);
		}
		list_del(&score->link);
		kfree(score);
	}
	mutex_unlock(&t->score_companion_guard);
	kfree(t);
	_mg->targets = NULL;
	NFOUT;
}

static int __attribute__ ((unused))
arnic_score_cmp(void *priv, struct list_head *a, struct list_head *b)
{
	struct nvmeibc_admin_rnic *arnic_a =
		container_of(a, struct nvmeibc_admin_rnic, link);
	struct  nvmeibc_admin_rnic *arnic_b =
		container_of(b, struct nvmeibc_admin_rnic, link);
	int score_a, score_b;
	(void)priv; // is this function used?
	BUG_ON(arnic_a->score == NULL);
	BUG_ON(arnic_a->score == NULL);
	score_a = atomic_read(&arnic_a->score->n_connections);
	score_b = atomic_read(&arnic_b->score->n_connections);
	return (score_a - score_b);
}

static int nvmeibc_target_add_score_companion(struct targets_global *t,
	struct nvmeibc_admin_rnic *arnic)
{
	struct nvmeibc_arnic_score *score;
	bool found = false;
	int rv = 0;

	NFIN;
	//BUG_ON(arnic == NULL);
	if (arnic == NULL) {
		_NE(e0_targets_nvmeibc_target_add_score_companion, "arnic NULL");
		return 0;
	}

	//BUG_ON(arnic->score != NULL);
	mutex_lock(&t->score_companion_guard);
	list_for_each_entry(score, &t->arnics_league, link) {
		if (!memcmp(score->gid.raw, arnic->ib_gid.raw, 16)) {
			found = true;
			break;
		}
	}
	if (!found) {
		//BUG_ON(arnic->score != NULL);
		if (arnic->score != NULL) {
			_NE(e1_targets_nvmeibc_target_add_score_companion,
				"arnic already has score");
			rv = -ENOMEM;
			goto out;
		}
		score = kzalloc(sizeof(*score), GFP_KERNEL);
		if (!score) {
			_NT(error_targets_nvmeibc_target_add_score_companion, "Unable to allocate memory for score companion");
			rv = -ENOMEM;
			goto out;
		}
		memcpy(score->gid.raw, arnic->ib_gid.raw, 16);
		list_add_tail(&score->link, &t->arnics_league);
		_NT(trace_targets_nvmeibc_target_add_score_companion, "new arnic @ARNIC score=@SCORE", arnic, score);
	} else
		_NT(trace_1_targets_nvmeibc_target_add_score_companion, "existing arnic @ARNIC score = @SCORE", arnic, score);
	arnic->score = score;

out:
	mutex_unlock(&t->score_companion_guard);
	NFOUT;
	return rv;
}

void nvmeibc_target_arnic_new_conn(struct nvmeibc_admin_rnic *arnic)
{
	int conns;

	NFIN;
	BUG_ON(arnic == NULL);
	BUG_ON(arnic->score == NULL);
	conns = atomic_inc_return(&arnic->score->n_connections);
	BUG_ON(conns <= 0);
	_NT(trace_targets_nvmeibc_target_arnic_new_conn, "arnic @IB_GID, n_conns inc to @CONNS", &arnic->ib_gid, conns);
	NFOUT;
}

void nvmeibc_target_arnic_close_conn(struct nvmeibc_admin_rnic *arnic)
{
	int conns;

	NFIN;
	BUG_ON(arnic == NULL);
	BUG_ON(arnic->score == NULL);
	conns = atomic_dec_return(&arnic->score->n_connections);
	BUG_ON(conns < 0);
	_NT(trace_targets_nvmeibc_target_arnic_close_conn, "arnic @IB_GID, n_conns dec to @CONNS", &arnic->ib_gid, conns);
	NFOUT;
}

static struct list_head *get_targets_list(struct targets_global *t);
void nvmeibc_targets_list_to_string(const struct nvmeibc_cinst_params_main *o)
{
	struct targets_global *t = __get_from_params_main_globals_container(o)->targets;
	int target_idx = 0;
	struct nvmeibc_target *target;
	struct nvmeibc_disk_id *disk_id;
	struct nvmeibc_target_nic *nic;
	_NI_dmesg(t_3l_dp_dbg_tools, "Printing target list:");
	list_for_each_entry(target, get_targets_list(t), link) {
		int nic_idx = 0;
		int disk_idx = 0;
		_NI_dmesg(t_3m_dp_dbg_tools, "@TARGET_IDX) Target @NODE_ID_STR", ++target_idx, target->node_id);
		_NI_dmesg(t_3n_dp_dbg_tools, "\tNics:");
		list_for_each_entry(nic, &target->nics, link) {
			_NI_dmesg(t_3o_dp_dbg_tools, "\t@NIC_IDX] N:@NICID", ++nic_idx, (nic?nic->data.nicID:"N/A"));
		}
		_NI_dmesg(t_3p_dp_dbg_tools, "\tDisks:");
		list_for_each_entry(disk_id, &target->disks, dlink) {
			_NI_dmesg(t_3q_dp_dbg_tools, "\t@DISK_IDX] D:@DISK_NAME  V:@DEV_NAME", ++disk_idx, (disk_id?disk_id->name:"N/A"),
			   (disk_id->volume?disk_id->volume->hdr.devname:"N/A"));
		}
	}
}

static struct list_head *get_targets_list(struct targets_global *t)
{
	nvmeibc_assert_on_main_wq(t->cinst);
	return &t->targets_list;
}

/* Replaces create_channel used in volumes creation */
static void create_target_channel(struct targets_global *t,
	union ib_gid *channel_gid, struct nvmeibc_admin_rnic *arnic)
{
	memcpy(arnic->hw_gid.raw, channel_gid->raw, sizeof(arnic->hw_gid.raw));
	_NT(trace_targets_create_target_channel, "Adding nic @ARNIC", arnic);
	nvmeibc_target_add_score_companion(t, arnic);
}

/* Parses GID in format 0xfe80000000000000ee0d9afffe66f454 */
static void parse_gid_str(union ib_gid *dst, const char *src)
{
	char u64_str[32] = {};
	strncpy(u64_str, src + 2, 16);
	dst->global.subnet_prefix = cpu_to_be64(simple_strtoull(u64_str, NULL, 16));
	strncpy(u64_str, src + 18, 16);
	dst->global.interface_id = cpu_to_be64(simple_strtoull(u64_str, NULL, 16));
}

/* Formats the configuration of the target nic into a usable rnic includes channel creation and rnic allcation */
static void __format_target_nic(struct targets_global *t,
	const struct nvmeibc_nic_conf *src_nic, const char *node_id,
	struct nvmeibc_admin_rnic *current_host)
{
	union ib_gid hw_gid = {};
	union ib_gid sw_gid = {};
	parse_gid_str(&hw_gid, src_nic->nicID);
	if (src_nic->guid[0] == '\0') {	// During upgrade the field might be NULL as default value
		parse_gid_str(&sw_gid, src_nic->nicID);
	} else {
		parse_gid_str(&sw_gid, src_nic->guid);
	}
	_NT(trace_targets_format_target_nic,
		"hw_gid=@GUID_RAW sw_gid=@GUID_RAW, protocol=@PROTOCOL",
		&hw_gid, &sw_gid, src_nic->protocol);
	create_target_channel(t, &hw_gid, current_host);
	current_host->ib_gid = sw_gid;
	// protocol == 0 Infiniband, protocol == 1 RoCE, protocol == 2 TCP, else unknown
	// set priority wrt to protocol, used by arnics_dup() for sorting
	current_host->is_multi_transport = false;
	switch (src_nic->protocol) {
	case PROTOCOL_INFINIBAND:
		current_host->pkey = src_nic->pkey;
		current_host->service_id = NVMEIB_EXCELERO_SERVICE_ID;
		current_host->service_port = 0;
		current_host->link_layer = IB_LINK_LAYER_INFINIBAND;
		current_host->transport_type = RDMA_TRANSPORT_IB;
		current_host->priority.transport = NVMEIB_IB_PORT_PRIORITY;
		break;
	case PROTOCOL_MULTI:
		current_host->is_multi_transport = true;
		FALLTHRU; /* arnics-dup will create both RoCE and TCP arnics */
	case PROTOCOL_ROCE:
		current_host->pkey = 0;
		current_host->service_id = 0;
		current_host->service_port = NVMEIB_EXCELERO_PORT_ID;
		current_host->link_layer = IB_LINK_LAYER_ETHERNET;
		current_host->transport_type = RDMA_TRANSPORT_IB;
		current_host->priority.transport = NVMEIB_ROCE_PORT_PRIORITY;
		break;
	case PROTOCOL_TCP:
		current_host->pkey = 0;
		current_host->service_id = 0;
		current_host->service_port = (u16)nvmeib_get_tcp_base_port_id();
		current_host->link_layer = IB_LINK_LAYER_ETHERNET;
		current_host->transport_type = RDMA_TRANSPORT_IWARP;
		current_host->priority.transport = NVMEIB_TCP_PORT_PRIORITY;
		break;

	default:
		_NE(err_targets_format_target_nic_inv_prot,
				"Invalid protocol @PROTOCOL for nic @GUID_RAW (SIW_GID @GUID_RAW) on node @NICS_NODE_ID",
				src_nic->protocol, &hw_gid, &sw_gid, node_id);
	}

	_NT(trace_1_targets_format_target_nic, "nics_node_id=@NICS_NODE_ID src_nic->protocol=@PROTOCOL service_port=@SERVICE_PORT", node_id,
		src_nic->protocol, current_host->service_port);
	strlcpy(current_host->node_id, node_id,
		sizeof(current_host->node_id));
}

/* This function copies all of the nic's fields and creates a new rnic frees the previous one if existed */
static bool __copy_target_nic(struct targets_global *t,
	struct nvmeibc_target_nic *dst_nic,
	const struct nvmeibc_nic_conf *src_nic, const char *node_id)
{
	// Do we really need to hold the configuration details of the nic as well as the host?
	strlcpy(dst_nic->data.nicID, src_nic->nicID, sizeof(dst_nic->data.nicID));
	dst_nic->data.pkey = src_nic->pkey;
	dst_nic->data.protocol = src_nic->protocol;
	__format_target_nic(t, src_nic, node_id, &dst_nic->host);
	return true;
}

/* Target nic holds both the configuration data and the formatted data in ->data and ->host respectivly */
static void __create_target_nic(struct targets_global *t,
	struct nvmeibc_target *dst, struct nvmeibc_target_nic *dst_nic,
	const struct nvmeibc_nic_conf *src_nic)
{
	__copy_target_nic(t, dst_nic, src_nic, dst->node_id);
	list_add(&dst_nic->link, &dst->nics);
	dst_nic->parent = dst;
	list_add(&dst_nic->host.link, &dst->arnics);
	dst_nic->mark_for_deletion = false;
}

/* Updates an existing target in place and returns if the target has been udpated */
static bool __target_update(struct targets_global *targets_db,
	struct nvmeibc_target **target, const struct nvmeibc_target_conf *target_conf)
{
	int i;
	struct nvmeibc_target_nic *nic, *n;
	struct nvmeibc_target *dst = *target;
	bool uuid_changed, must_updt_tgt; // if target is deleted from mgnt and recreated, the uuid changes.
	bool is_update = false, found;	// used to determine if we need to update all disks with new arnics
	list_for_each_entry(nic, &dst->nics, link) { // If a nic is not in the target configuration assume it should be removed
		nic->mark_for_deletion = true;
	}

	_NI(trace_2_targets_target_update,
	    "@EVENT_TAG test to if need update target(@NODE_ID_STR) NICs version(curr=@INT, next=@INT) curr seq=@INT next=@INT",
	    EV_UPDATE_TGT(), dst->node_id, dst->mgmt_nics_version, target_conf->nicsVersion,
	    dst->target_update_sequence, target_conf->targetUpdatesSequence);

	uuid_changed = (strncmp(dst->node_uuid, target_conf->uuid, sizeof(dst->node_uuid) - 1) != 0);
	must_updt_tgt = ((uuid_changed && target_conf->targetUpdatesSequence > dst->target_update_sequence) ||
		(!uuid_changed && target_conf->nicsVersion > dst->mgmt_nics_version));

	if (must_updt_tgt) {
		_NT(trace_2_targets_target_update_,
		    "going to update target(@NODE_ID_STR) NICs version(curr=@INT, next=@INT) curr seq=@INT next=@INT",
		    dst->node_id, dst->mgmt_nics_version,
		    target_conf->nicsVersion, dst->target_update_sequence,
		    target_conf->targetUpdatesSequence);
		dst->mgmt_nics_version = target_conf->nicsVersion;
		dst->target_update_sequence = target_conf->targetUpdatesSequence;
		if (uuid_changed)
			strlcpy(dst->node_uuid, target_conf->uuid, sizeof(dst->node_uuid));

		// Daniel should I wrap this part as a separate function?
		// Roman: of course, because there 2 unrelated algorithm here: find and update
		for (i = 0; i < target_conf->n_nics; i++) {
			struct nvmeibc_nic_conf *src_nic = &target_conf->nics[i];
			found = false;
			list_for_each_entry(nic, &dst->nics, link) {
				if (!strcmp(src_nic->nicID, nic->data.nicID)) {
					// Found nic, update it's structure if required
					if (src_nic->pkey != nic->data.pkey || src_nic->protocol != nic->data.protocol) {
						_NI(trace_targets_target_update,
						    "@EVENT_TAG node: @NODE_ID_STR Updating nic @NIC (pkey=@PKEY-->@PKEY, protocol=@PROTOCOL-->@PROTOCOL)",
						    EV_UPDATE_TGT_NIC(), target_conf->node_id, nic, nic->data.pkey, src_nic->pkey,
						    nic->data.protocol, src_nic->protocol);
						is_update |= __copy_target_nic(targets_db, nic, src_nic, target_conf->node_id);
					}
					found = true;
					nic->mark_for_deletion = false; // Nic is still in use do not delete it
				}
			}
			if (!found) {  // New nic in existing target allocate and add it
				if (unlikely(!(nic = kzalloc(sizeof(*nic), GFP_KERNEL)))) {
					_NT(error_targets_target_update, "No memory for nic");
				} else {
					_NI(trace_1_targets_target_update,
					    "@EVENT_TAG Adding NEW nic node: @NODE_ID_STR @NIC (pkey=@PKEY, protocol=@PROTOCOL)",
					    EV_NEW_NIC(), target_conf->node_id, nic, src_nic->pkey, src_nic->protocol);
					__create_target_nic(targets_db, dst, nic, src_nic);
					is_update = true;
				}
			}
		}

		list_for_each_entry_safe(nic, n, &dst->nics, link) {
			if (nic->mark_for_deletion) { // If we didn't find this is remove it
				_NI(trace_3_targets_target_update,
				    "@EVENT_TAG Deleting from: @NODE_ID_STR nic @NIC (pkey=@PKEY, protocol=@PROTOCOL)",
				    EV_DEL_NIC(), target_conf->node_id, nic, nic->data.pkey, nic->data.protocol);
				list_del(&nic->link);
				list_del(&nic->host.link);
				kfree(nic);
				is_update = true; 		  // All disks should update the nic list
			}
		}
	}

	if (is_update) {				// Each existing disk should update it's list
		struct nvmeibc_disk_id *disk;
		const char *disk_node_id = dst->node_id;
		list_for_each_entry(disk, &dst->disks, dlink) {
			nvmeibc_disk_set_next_config(disk, disk_node_id);
		}
	}

	return is_update;
}

static struct nvmeibc_target*
nvmeibc_find_target_by_node_id(struct targets_global *targets_db, const char* node_id)
{
	struct nvmeibc_target *found = NULL;
	list_for_each_entry(found, get_targets_list(targets_db), link) {
		if (!strcmp(node_id, found->node_id)) {
			return found;
		}
	}
	return NULL;
}

static struct nvmeibc_target*
nvmeibc_find_target(struct targets_global *targets_db, const struct nvmeibc_target_conf *target_conf)
{
	//we should only use node_id to query for target, since uuid may change when management deletes and recreate target
	return nvmeibc_find_target_by_node_id(targets_db, target_conf->node_id);
}

struct nvmeibc_target *allocate_new_target(struct targets_global *t,
	const struct nvmeibc_target_conf *src);
/* Used for searching existing targets in the client memory
   Must be called on main workqueue*/
bool nvmeibc_target_update_or_create(const struct nvmeibc_cinst_params_main *cints,
	const struct nvmeibc_target_conf *target_conf, struct nvmeibc_target **target)
{
	struct targets_global *targets_db = __get_from_params_main_globals_container(cints)->targets;
	struct nvmeibc_target *found = nvmeibc_find_target(targets_db, target_conf);
	bool is_update = true;

	if (found){
		is_update = __target_update(targets_db, &found, target_conf);
	} else {
		found = allocate_new_target(targets_db, target_conf);
	}

	if (found && list_empty(&found->nics)){
		//found still can be null, if allocation failed
		const struct nvmeibc_target_nics_query query = nvmeibc_target_get_nics_query(found);
		nvmeibc_cc_api_query_target_nics(targets_db->cinst, query);
	}
	*target = found;
	return is_update;
}

void nvmeibc_target_update(const struct nvmeibc_cinst_params_main *cints,
	const struct nvmeibc_target_conf *target_conf)
{
	struct targets_global *targets_db = __get_from_params_main_globals_container(cints)->targets;
	struct nvmeibc_target *found = nvmeibc_find_target(targets_db, target_conf);
	struct t_main_clnt_globals * _mg = __get_from_params_main_globals_container(cints);
	struct nvmeibc_control_api *cc_api = &_mg->cc_api;

	if (!found){
		return;
	}
	__target_update(targets_db, &found, target_conf);
	/*
	 * Special case: If the cache isn't fully replayed, it indicates that the "updateTargetNics" message
	 * is received by the client without a preceding "getTargetNics" request initiated by the app. In this case,
	 * we need to log the time. Otherwise, the cdisk will trigger a "getTargetNics" to the management
	 * since the time hasn't been set.
	 */
	if (!cc_api->is_mcs_cache_replayed_completed)
		nvmeibc_target_nics_query_was_sent(found);

}

struct nvmeibc_target* nvmeibc_target_should_send_nics_query(const struct nvmeibc_cinst_params_main * cinst, struct nvmeibc_target_nics_query* query)
{
	struct targets_global *targets_db = __get_from_params_main_globals_container(cinst)->targets;
	struct nvmeibc_target *target = nvmeibc_find_target_by_node_id(targets_db, query->node_id);

	if (target){
		const ulong passed = jiffies - target->nics_query_cache.at;
		const uint query_period = nvmeibc_target_nics_query_min_fail_secs;
		if (query_period * HZ <= passed) {
			return target;
		}
		if (target->nics_query_cache.sent_nics_version < query->nicsVersion) {
			return target;
			/* else target->mgmt_nics_version > query->nicsVersion
			 *      and this means that driver complains, while we are rolling out the update inside the module
			 *      The driver will wait a little
			 */

		}
	}
	return NULL;
}

void nvmeibc_target_nics_query_was_sent(struct nvmeibc_target* target) {
	target->nics_query_cache.sent_nics_version = target->mgmt_nics_version;
	target->nics_query_cache.at = jiffies;
}


bool nvmeibc_target_fill_nics_query_by_node_id(const struct nvmeibc_cinst_params_main * cinst, struct nvmeibc_target_nics_query* query)
{
	struct targets_global *targets_db = __get_from_params_main_globals_container(cinst)->targets;
	struct nvmeibc_target *found = nvmeibc_find_target_by_node_id(targets_db, query->node_id);

	if (!found){
		return false;
	}

	BUILD_BUG_ON(sizeof(query->nodeUUID) != sizeof(found->node_uuid));
	strlcpy(query->nodeUUID, found->node_uuid, sizeof(query->nodeUUID));
	query->nicsVersion = found->mgmt_nics_version;
	return true;
}

static void nvmeibc_target_remove(struct nvmeibc_target *target)	// Todo: rename, and move del_list to here
{
	struct nvmeibc_target_nic *nic, *n;
	struct nvmeibc_admin_rnic *rnic, *r;
	list_for_each_entry_safe(rnic, r, &target->arnics, link) {
		list_del(&rnic->link);
	}
	list_for_each_entry_safe(nic, n, &target->nics, link) {
		list_del(&nic->link);
		kfree(nic);
	}
	list_del(&target->link);
	_ND(trace_targets_nvmeibc_target_remove, "Target @NODE_ID_STR is going to be freed", target->node_id);
	kfree(target);
}

/* Upon disk_id clean up if the target no longer holds any additional disk_ids
   it releases all of the allcated memory and the target object */
void nvmeibc_target_remove_disk_id(struct nvmeibc_disk_id *disk_id)	// Todo: rename, and move del_list to here
{
	struct nvmeibc_target *target = disk_id->target;
	list_del(&disk_id->dlink);
	_ND(trace_targets_nvmeibc_target_remove_disk_id, "Target @NODE_ID_STR is tested to be removed", target->node_id);
	if (target && list_empty(&target->disks)) {// destroy the target
		_NI(info_targets_nvmeibc_target_remove_disk_id,
		    "@EVENT_TAG Target @NODE_ID_STR has no more disks, removing it", EV_REMOVE_TARGET(),
		    target->node_id);
		nvmeibc_target_remove(target);
	} else { // nothing to do
		_ND(trace_1_targets_nvmeibc_target_remove_disk_id, "Target @NODE_ID_STR is not empty", target->node_id);
	}
}

/* Allocates a new target object to hold all of the nic information and disk_ids */
struct nvmeibc_target *allocate_new_target(struct targets_global *t,
	const struct nvmeibc_target_conf *src)
{
	int nic_idx;
	struct nvmeibc_target *dst = kzalloc(sizeof(*dst), GFP_KERNEL);
	if (!dst) {
		return NULL;
	}

	_NT(t1_allocate_new_target, "Allocating target(@NODE_ID_STR) NICs version(curr=@INT)",
		src->node_id, src->nicsVersion);

	dst->mgmt_nics_version = src->nicsVersion;
	dst->target_update_sequence = 0; // target update sequence is only set from updateTargetNics
	dst->nics_query_cache.sent_nics_version = -1;

	INIT_LIST_HEAD(&dst->nics);
	INIT_LIST_HEAD(&dst->arnics);
	INIT_LIST_HEAD(&dst->disks);
	strlcpy(dst->node_id, src->node_id, sizeof(dst->node_id));
	BUILD_BUG_ON(sizeof(dst->node_uuid) != sizeof(src->uuid));
	strlcpy(dst->node_uuid, src->uuid, sizeof(dst->node_uuid));

	for (nic_idx = 0; nic_idx < src->n_nics; nic_idx++) {
		const struct nvmeibc_nic_conf *src_nic = &src->nics[nic_idx];
		struct nvmeibc_target_nic *nic;

		_NT(t0_allocate_new_target,
			"nic-idx=@NIC_IDX nicID=@NICID guid=@STR pkey=@INT protocol=@INT", nic_idx, src_nic->nicID, src_nic->guid, src_nic->pkey, src_nic->protocol);

		if (!(nic = kzalloc(sizeof(*nic), GFP_KERNEL))) {
			_NT(e0_allocate_new_target, "Failed to alloc");
			// For now we keep as much of the target info as we managed, do we need any addtional work?
			break;
		}

		__create_target_nic(t, dst, nic, src_nic);
	}
	list_add(&dst->link, get_targets_list(t));
	return dst;
}
