/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

// For documentation, see Header in H file
/*****************************************************************************/
// Includes
#include "../nvmeibc_simu_disk.h"		// Include of c_disk.h modified
#include "../server/nvmeibs_main_sim.h"
#include "nvmeibt_toma_simu.h"
#include "../nvmeibc_block_common.h"	// The includes below are breaking encapsulation concept (Simulator digs into Blocks code). This is done for debug purpose (verify internal states of Block locks).
#include "../uni_framework/range_algorithms.h"
#include "../uni_framework/cond_wait_algorithms.h"
#include "nvmeibs_main.h"
#include "nvmesh_sim.h"

/**************************** msg queue **************************************/
void toma_msg_q__init(struct toma_msg_q_to_client *q, s8 toma_unique_id) {
	q->toma_unique_id = toma_unique_id;
	q->size = 0;
	q->head = q->tail = 0;
	memset(q->msgs, 0, sizeof(q->msgs));
}

int toma_msg_q__get_size(const struct toma_msg_q_to_client *q) {
	int	size = q->tail - q->head;
	if (q->head > q->tail)
		size += TOMA_MSG_Q_MAX_MSGS;
	BUG_ON(size != q->size);
	if (q->size == 0)
		BUG_ON(q->head != q->tail);	// empty
	return q->size;
}

void toma_msg_q__enqueue(struct toma_msg_q_to_client *q, const struct nvmeibc_disk_subscription_params con, u8 *msg, int msg_size) {
	struct toma_msg_q_to_client_elem *elem = &q->msgs[q->tail];
	BUG_ON(toma_msg_q__is_full(q));
	elem->msg_buf  = msg;
	elem->msg_size = msg_size;
	elem->con      = con;
	q->size++;
	q->tail = (q->tail+1) % TOMA_MSG_Q_MAX_MSGS;
}

void toma_msg_q__dequeue(struct toma_msg_q_to_client *q, struct toma_msg_q_to_client_elem *elem) {
	BUG_ON(toma_msg_q__is_empty(q));
	memcpy(elem, &q->msgs[q->head]   , sizeof(*elem));
	memset(      &q->msgs[q->head], 0, sizeof(*elem));
	q->size--;
	q->head = (q->head+1) % TOMA_MSG_Q_MAX_MSGS;
}

u8* toma_msg_q__view_last_msg( struct toma_msg_q_to_client *q) {
	BUG_ON(toma_msg_q__is_empty(q));
	return q->msgs[q->head].msg_buf;
}

int toma_msg_q__flush(struct toma_msg_q_to_client *q, const int msg_count) {
	int		i;
	struct toma_msg_q_to_client_elem elem;
	BUG_ON(msg_count < 0);
	/* we flush the whole queue under lock so that if new msgs arrive, they'll block on the lock & when unlocked, they'll see that no queueing is
	 * enabled anymore. otherwise, they'll go to the clinet *before* the queued msgs, changing the order of msgs !!!.*/
	_ND(trace_toma_simu_toma_msg_q__flush_1, "TOMA @SIMTOMA_UNIQUEID flushing msgs to client begins with @SIZE msgs", q->toma_unique_id, toma_msg_q__get_size(q));
	for (i=0; (msg_count == 0) ? !toma_msg_q__is_empty(q) : i < msg_count ; i++) {
		toma_msg_q__dequeue(q, &elem);
		elem.con.recv_req_cb(NULL, elem.con.arg, elem.msg_buf, elem.msg_size);
		sim_kfree(elem.msg_buf);
	}
	_ND(trace_toma_simu_toma_msg_q__flush_2, "TOMA @SIMTOMA_UNIQUEID flushing msgs - done", q->toma_unique_id);
	return i;
}

/********************** Toma usage of msg queue *******************************/
void tomaSimulator_enable_msg_q_to_client(struct tomaSimulator*_this) {
	spin_lock(&_this->lock);
	_ND(trace_toma_simu_tomaSimulator_enable_msg_q_to_client, "TOMA @SIMTOMA_UNIQUEID: enabling queueing of msgs from to client", _this->uniqueID);
	_this->msg_q_state = toma_msg_queueing_state__enabled;
	spin_unlock(&_this->lock);
}

int tomaSimulator_get_msg_queue_size(struct tomaSimulator *_this){
	int	q_size;
	spin_lock(&_this->lock);
	q_size = toma_msg_q__get_size(&_this->msg_q);
	spin_unlock(&_this->lock);
	return q_size;
}

static void __tomaSimulator_enqueue(struct tomaSimulator *_this, const struct nvmeibc_disk_subscription_params con, u8 *msg, int msg_size){
	spin_lock(&_this->lock);
	if (_this->msg_q_state == toma_msg_queueing_state__enabled) {
		_ND(__tomaSimulator_enqueue_1, "TOMA @SIMTOMA_UNIQUEID: adding msg=@BUF size=@SIZE to queue", _this->uniqueID, msg, msg_size);
		toma_msg_q__enqueue(&_this->msg_q, con, msg, msg_size);
	} else { // By the time we acquired the lock, the queue is disabled. dispatch immediatelty
		_ND(__tomaSimulator_enqueue_2, "TOMA @SIMTOMA_UNIQUEID: calling client to handle message msg=@BUF size=@SIZE to queue", _this->uniqueID, msg, msg_size);
		con.recv_req_cb(NULL, con.arg, msg, msg_size);
		sim_kfree(msg);
	}
	spin_unlock(&_this->lock);
}

int tomaSimulator_flush_msg_to_client(struct tomaSimulator *_this, const int msg_count) {
	int	num_msgs;
	spin_lock(&_this->lock);
	num_msgs = toma_msg_q__flush(&_this->msg_q, msg_count);
	spin_unlock(&_this->lock);
	return num_msgs;
}

void tomaSimulator_disable_msg_q_to_client(struct tomaSimulator*_this) {
	int		num_flushed_msgs;
	spin_lock(&_this->lock);
	_ND(trace_toma_simu_tomaSimulator_disable_msg_q_to_client, "TOMA @TOMA_UNIQUEID to client. num_msgs=@NUM_MSGS", _this->uniqueID, toma_msg_q__get_size(&_this->msg_q));
	if (_this->msg_q_state == toma_msg_queueing_state__enabled) {
		num_flushed_msgs = toma_msg_q__flush(&_this->msg_q, 0);
		_ND(trace_1_toma_simu_tomaSimulator_disable_msg_q_to_client, "TOMA @TOMA_UNIQUEID to client flushed @NUM_FLUSHED_MSGS msgs", _this->uniqueID, num_flushed_msgs);
	}
	_this->msg_q_state = toma_msg_queueing_state__disabled;
	spin_unlock(&_this->lock);
}
/******************** Simulator of toma access to server **********************/
void nvmeibr_ds_metadata_init_EC_lock_and_dirty(struct tomaSimulator* T, struct TstPRaid *pra) {
	struct ramDiskSimulator *ram = serverSimulator_get_ram_by_toma(T);
	const bool is_degraded = (tTopoOfPraid_gen_num_non_readble_segs(pra->tpr) > 0);
	union nvmeibc_dbits_entry dbits = {.all_bits = 0};
	const struct disk_range *seg = &pra->cpr[pra->vsi.segment];
	u32 i, bi, n_locks = (seg->length/LOCKSET_4KS);
	u64 b_start = COMMITTED_ADDR_AS(ram, seg->dlba_start, 4KB, LOCK);
	if (is_degraded)
		dbits.all_bits = nvmeib_dbits_entry_build_unk(-1,-1).all_bits;

	for (i = 0; i < n_locks; i++) {						// Daniel: Todo, this is only correct for full praid cold recovery, not partial!
		bi = i + b_start;
		ram->locks[bi] = 0;
		ram->dbits[bi] = dbits;
		ram->TxIDs[bi] = INITIAL_LAZY_READ_TXID;
	}
}

/********************* Simulator of toma communication ***********************/
const struct tTopoOfPraid* toma_findTopologygBySegUUID(const struct tomaSimulator* simToma, const char* uuid, const struct tTopoOfVolume **tv_ptr, int*segInd){
	return tTopoOfNVMesh_find_r1_by_seg_uuid(simToma->globalTopo, uuid, tv_ptr, segInd);
}

const struct tTopoOfPraid* toma_findTopologygByPraidUUID(const struct tomaSimulator* simToma, const char* uuid, const struct tTopoOfVolume **tv_ptr){
	return tTopoOfNVMesh_find_r1_by_praid_uuid(simToma->globalTopo, uuid, tv_ptr);
}

// Find the connection of the client, in the hash table
static const struct tomaSimulator_socket * findConnectionByHandle(const struct tomaSimulator* simToma, const u64 handle){
	BUG_ON(!spin_is_locked(&simToma->lock));
	for (u32 i=0; i< ARRAY_SIZE(simToma->sock); i++) {
		if (simToma->sock[i].key == handle) {
			return &simToma->sock[i];
		}
	}
	return NULL; // Connection does not exist
}

#define for_each_socket_by_uuid(simToma, _segUUID, sock)                                      \
	BUG_ON(!spin_is_locked(&simToma->lock));                                                  \
	for (sock = (simToma)->sock; sock < (simToma)->sock + ARRAY_SIZE((simToma)->sock); ++sock) \
		if (sock->key && !strcmp(sock->seg_UUID, _segUUID))

static struct tomaSimulator_socket *findConnectionBySegUUID(struct tomaSimulator* simToma, const char* segUUID){
	struct tomaSimulator_socket *sock;
	BUG_ON(!spin_is_locked(&simToma->lock));
	for_each_socket_by_uuid(simToma, segUUID, sock)
		return sock;
	return NULL;
}

static struct tomaSimulator_socket *findConnectionByInstIDSegUUID(struct tomaSimulator* simToma, int inst_id, const char* segUUID){
	struct tomaSimulator_socket *sock;
	BUG_ON(!spin_is_locked(&simToma->lock));
	for_each_socket_by_uuid(simToma, segUUID, sock)
		if (sock->inst_id == inst_id) return sock;
	return NULL;
}

static bool __is_clients_registered_lock(u32 lock_id) {
	const union nvmeib_lock_id max_clnt_possible_lock = nvmeib_stale_bit_mask_ec.lock_id;
	return (LS_UNLOCKED < lock_id) && (lock_id < max_clnt_possible_lock.all);
}

bool tomaSimulator_isSegRegistered(struct tomaSimulator* simToma, const char* segUUID, int inst_id){
	int rv = 0;
	struct tomaSimulator_socket *sock;
	spin_lock(&simToma->lock);
	sock = findConnectionByInstIDSegUUID(simToma, inst_id, segUUID);
	if (sock) rv = __is_clients_registered_lock(sock->lid.all);
	spin_unlock(&simToma->lock);
	return rv;
}

void tomaSimulator_wait_seg_registered(struct tomaSimulator* simToma, const char* segUUID, int inst_id){
	int	is_seg_reg, rounds;
	for (is_seg_reg = false, rounds=0; !is_seg_reg; rounds++) {
		is_seg_reg = tomaSimulator_isSegRegistered(simToma, segUUID, inst_id);
		if (!is_seg_reg) {
			usleep(10);
			BUG_ON(rounds > 100000/*>1 sec*/);
		}
	}
}

static const union nvmeib_lock_id max_ec_lock = { .bits = { .idx_in_praid = 0xf, .lock_id = 0xffffff}};

static int __convert_single_lock_to_stale(struct tomaSimulator* simToma, u64 li_abs, const u32 curr_abandoned_lock, bool isEC){
	int rv = 0;
	struct ramDiskSimulator *ramDisk = serverSimulator_get_ram_by_toma(simToma);
	u64 li_rel = COMMITTED_ADDR(ramDisk, li_abs, LOCK);
	const u32 stale_value = (curr_abandoned_lock | nvmeib_stale_bit_mask_ec.lock_id.all);
	spin_lock(&ramDisk->cmpxchg_lock);
	if (isEC) {	// Check if lock is same id (use lock_id + idx_in_praid, max_ec_lock) and either restore stale ancestor, or store stale in lock and as ancestor
		const u32 curr_abnd_lock_id = ((union nvmeib_lock_id)curr_abandoned_lock).all & max_ec_lock.all;
		const u32 curr_lock_id =      ((union nvmeib_lock_id)ramDisk->locks[li_rel] ).all & max_ec_lock.all;
		if (curr_lock_id == curr_abnd_lock_id) {				// Found our unregistering lock id
			if (simToma->stale_locks[li_rel].all != LS_UNLOCKED) {	// Override stale with previous stale lock
				_NT(trace_toma_simu_convert_single_lock_to_stale, "Unreg : Staled   lock on disk[@TOMA_UNIQUEID].lock[@LSI]: back to ancestor (@LOCK_ENT==>@LOCK_ENT)", simToma->uniqueID, li_abs, ramDisk->locks[li_rel], simToma->stale_locks[li_rel].all);
				ramDisk->locks[li_rel] = simToma->stale_locks[li_rel].all;
			} else if (ramDisk->locks[li_rel] != stale_value) {		// Update TOMA with new stale lock values and store in lock
				_NT(trace_1_toma_simu_convert_single_lock_to_stale, "Unreg : Staled   lock on disk[@TOMA_UNIQUEID].lock[@LSI]: (@LOCK_ENT==>@LOCK_ENT) storing ancestor @LOCK_ENT", simToma->uniqueID, li_abs, ramDisk->locks[li_rel], stale_value, stale_value);
				simToma->stale_locks[li_rel].all = stale_value;
				ramDisk->locks[li_rel] = stale_value;
			} else {											// Already stale - probably should not happen
				_Emerg("Unreg : Staled second time, odd -> Ancestor is 0x%x lock is already stale 0x%x", simToma->stale_locks[li_rel].all, ramDisk->locks[li_rel]);
				BUG_ON(simToma->stale_locks[li_rel].all != ramDisk->locks[li_rel]);
				rv--; // TODO?
			}
			rv++;
		} else {
			_NT(trace_2_toma_simu_convert_single_lock_to_stale, "Unreg : Skipping lock on disk[@RAMDISK_UNIQUEID].lock[@LSI]: @LOCK_ENT (!=@LOCK_ENT)", ramDisk->uniqueID, li_abs, ramDisk->locks[li_rel], curr_abandoned_lock);
		}
	} else {
		if (ramDisk->locks[li_rel] == curr_abandoned_lock)  {
			_NT(trace_3_toma_simu_convert_single_lock_to_stale, "Unreg : Staled   lock on disk[@RAMDISK_UNIQUEID].lock[@LSI]: @LOCK_ENT=cmpxchng(@LOCK_ENT==>@LOCK_ENT)", simToma->uniqueID, li_rel, ramDisk->locks[li_rel], ramDisk->locks[li_rel], stale_value);
			ramDisk->locks[li_rel] = stale_value;
			rv++;
		} else if (ramDisk->locks[li_rel] == stale_value) {
			_NT(trace_4_toma_simu_convert_single_lock_to_stale, "Unreg : Skipping lock on disk[@RAMDISK_UNIQUEID].lock[@LSI]: @LOCK_ENT already stale", ramDisk->uniqueID, li_abs, ramDisk->locks[li_rel]);
		} else {
			_NT(trace_5_toma_simu_convert_single_lock_to_stale, "Unreg : Skipping lock on disk[@RAMDISK_UNIQUEID].lock[@LSI]: @LOCK_ENT (!=@LOCK_ENT)", ramDisk->uniqueID, li_abs, ramDisk->locks[li_rel], curr_abandoned_lock);
		}
	}

	spin_unlock(&ramDisk->cmpxchg_lock);
	return rv;
}

static void __convert_clnt_owner_locks_to_stale(const struct disk_range *seg, u32 abandoned_lock, struct tomaSimulator* simToma) {
	const int lock_ind_first =  seg->dlba_start		 		  /LOCKSET_4KS;
	const int lock_ind_last	=  (seg->dlba_start+seg->length-1)/LOCKSET_4KS;
	const bool is_EC = (seg->slice_size > 1);
	_NT(trace_toma_simu_convert_clnt_owner_locks_to_stale, "Toma seg=@SEG lid=@LID", seg->ruuid, abandoned_lock);
	for (int i=lock_ind_first; i<=lock_ind_last; i++)
		__convert_single_lock_to_stale(simToma, i, abandoned_lock, is_EC);
	_NT(trace_1_toma_simu_convert_clnt_owner_locks_to_stale, "Toma completed: seg=@SEG lid=@LID", seg->ruuid, abandoned_lock);
}

static void __launch_unreg_to_stale_on_seg(struct tomaSimulator* simToma, struct tomaSimulator_socket *sock, const u32 prev_lock){
	// Step 1. Find the segments configuration (physical location on disc
	const struct tTopoOfVolume *tv = NULL;
	const char *segment_uuid = sock->seg_UUID;
	const struct tTopoOfPraid* latestTopo = toma_findTopologygBySegUUID(simToma, segment_uuid, &tv, NULL);
	int i;
	if (!tv || !latestTopo)
		return;								// Todo: Handle this case. Can happen upon segment relocation?
	for (i=0; i<tv->n_segs; i++)
		if (!strcmp(tv->segs[i].ruuid,segment_uuid))
			break;							// Find config segment by topo segment
	if (i==tv->n_segs) {
		_NT(trace_toma_simu_launch_unreg_to_stale_on_seg, "Weve HIT an unregister of removed segment :)");
		return;		// we didnt find the segment, probably removed
	}
	__convert_clnt_owner_locks_to_stale(&tv->segs[i], prev_lock, simToma);
}

struct workqueue_struct *toma_rcv_wq = NULL; 	// a wq to process all msgs from toma async to the client code. this ensures the msgs are serialized, as is the case assured by toma protocol.

/* Struct which stores incomming msgs to toma (possibly appended by server msg)
   to launch replies asyncronously */
typedef struct {
	struct work_struct 	work_item;					// Toma msgs are all dispatched to the system_wq in order to ensure they execute in order (i.e.: serialized)
	struct serverSimulator *srvr;					// Server/Toma which should answer the message
	u64 handle; 									// Unique identifier of connenction with client on a specific segment.
	struct nvmeibc_disk_toma_send_params p;			// Local copy
} tomaIncomingMessage;

static void tomaIncomingMessage_tostring(const struct work_struct *w) {
	const tomaIncomingMessage* inM = container_of(w, tomaIncomingMessage, work_item);
	struct tomaSimulator *simToma = &inM->srvr->simToma;
	const int toam_ind = simToma->uniqueID;
	const struct nvmeibt_client_msg* msg = (void*)inM->p.buf;
	//const struct tomaSimulator_socket *sock;
	//spin_lock(&simToma->lock);
	//sock = findConnectionByHandle(simToma, handle);
	if (inM) {
		enum NVMEIBT_CLIENT_MSG_TYPES type = be32_to_cpu(msg->hdr.msg_type);
		const u32 lock_id = be32_to_cpu(msg->thick.lock_id);
		_Emerg("Toma %d: Handle=0x%llx, Msg=%s seg=%s c_lid=0x%x\n", toam_ind, inM->handle, nvmeibt_protocol_client_msg_str(type), msg->thick.disk_segment_uuid, lock_id);
	} else {	// Unsubscribe
		_Emerg("Toma %d: Handle=0x%llx, Unsubscribe\n", toam_ind, inM->handle);
	}
}
#define BUG_DUMP_TOMAS_MSGS_WQ() workqueue_dump_works_to_log(toma_rcv_wq, tomaIncomingMessage_tostring); BUG();

static tomaIncomingMessage* tomaIncomingMessage_create(struct serverSimulator *S, u64 handle, struct nvmeibc_disk_toma_send_params *p) {
	tomaIncomingMessage *inM = sim_kzalloc(sizeof(*inM), GFP_KERNEL); // Create a local copy for asyncronous thread execution.
	BUG_ON(handle==0);
	inM->handle = handle;
	inM->srvr = S;
	if (p) {
		inM->p =      *p;
		inM->p.buf = (u8*)sim_kmalloc(p->len_srvr, GFP_KERNEL);
		memcpy(inM->p.buf, p->buf, p->len_srvr);
	}
	_ND(trace_toma_simu_tomaIncomingMessage_create, "Toma @SIMTOMA_UNIQUEID: inMsg=@INMSG, buf=@BUF, handle=@HANDLE", S->simToma.uniqueID, inM, inM->p.buf, handle);
	return inM;
}

static void tomaIncomingMessage_destroy(tomaIncomingMessage* m) {
	sim_kfree(m->p.buf);	// NULL when subscribed/unsubscribe
	sim_kfree(m);
}

static void __on_recov_finish_stop_waiting(struct tomaSimulator *simToma, struct tomaSimulator_socket *sock, const enum NVMEIBT_RECOVERY_TYPE valid_rtype, bool got_clnt_msg, int recov_status);

/* wasExplicit=true means client requested unregistering via message, otherwise toma assumes that client disconnected, and updates its internal states */
static void __simToma_unregister_segment(struct tomaSimulator* simToma, struct tomaSimulator_socket *sock, u32 unreg_lockid, bool wasExplicit){
	const char *segment_uuid = sock->seg_UUID;
	const char* initiator = (wasExplicit ? "Client" : "Toma");							// Initiator of the unregistering
	BUG_ON(!spin_is_locked(&simToma->lock));
	if (!sock){
		if ((wasExplicit)&&(!simToma->f_protoBugsSilent)) {
			_Emerg("Toma %d: client tries to unregister from wrong segment: %s\n", simToma->uniqueID, segment_uuid);
			BUG_NOT_IMPLEMENTED_YET;
		}
	} else { // Unregister the segment
		const u32 curr_lockid = sock->lid.all;
		if (wasExplicit) {
			if (unreg_lockid != curr_lockid) {
				__launch_unreg_to_stale_on_seg(simToma, sock, unreg_lockid);	// Old unregister arrives. Toma does not know this lock-id. Better safe then sorry. Convert it to stale
			}
		}
		_NT(trace_toma_simu_simToma_unregister_segment, "Toma @SIMTOMA_UNIQUEID: removing wait for switch_topo_ack due to unregister on seg uuid @SEGMENT_UUID", simToma->uniqueID, segment_uuid);
		sock->wait_for_switch_topo_ack = false;
		for (u16 i = 0; i < ARRAY_SIZE(sock->rcvrs); ++i) {
			__on_recov_finish_stop_waiting(simToma, sock, i, false /*force stop*/, -1 /* recov_status */);		// does force stop only if needed
		}
		if (curr_lockid == LS_UNLOCKED) {
			if (!wasExplicit)
				_NT(trace_1_toma_simu_simToma_unregister_segment, "Toma @SIMTOMA_UNIQUEID: ignoring, already unregistered: @SEGMENT_UUID lid=@LID. @STR initiative", simToma->uniqueID, segment_uuid, unreg_lockid, initiator);
		} else if ((unreg_lockid == curr_lockid) || (unreg_lockid == SIMULATOR_USE_PREV_LOCK_ID)) {
			_NT(trace_2_toma_simu_simToma_unregister_segment, "Toma @SIMTOMA_UNIQUEID: unregisterring from segment: @SEGMENT_UUID lid=@LID. @STR initiative", simToma->uniqueID, segment_uuid, curr_lockid, initiator);
			sock->lid.all = LS_UNLOCKED;
			if (wasExplicit) {				// since client behaves properly & we have no msg loss, dont start cleanup of lock when UNREGISTER is implicit bcz it will cause races
				__launch_unreg_to_stale_on_seg(simToma, sock, curr_lockid); // Clean clients locks
			} else {
				_NT(trace_3_toma_simu_simToma_unregister_segment, "Toma @SIMTOMA_UNIQUEID: skip conv_2_stale forseg @SEGMENT_UUID lid=@LID, Waiting for client's unreg msg", simToma->uniqueID, segment_uuid, curr_lockid);
			}
		} else
			_NT(trace_4_toma_simu_simToma_unregister_segment, "Toma @SIMTOMA_UNIQUEID: outdated unregister request from segment: @SEGMENT_UUID cur_id=@LID, req_id=@LID. @STR initiative", simToma->uniqueID, segment_uuid, curr_lockid, unreg_lockid, initiator);
	}
}

static inline u32 __gen_unique28bits_lock_id(const struct nvmeibc_disk_subscription_params *con) {	// 28 bits of version V1.2.1+ for EC. Always >= SIMULATOR_LAST_RESERVED_LOCK_ID
	// Daniel: Todo, use union nvmeib_lock_id constructor instead of u64 bit result
	static __concurrent_access u32 cnt = 0;												// Daniel, Todo: Use union nvmeib_lock_id like real toma
	cnt = (cnt+1)&(SIMULATOR_LAST_RESERVED_LOCK_ID-1);				// Cyclic on 4K values.
	return ((u32)(con->arg*SIMULATOR_LAST_RESERVED_LOCK_ID|SIMULATOR_LAST_RESERVED_LOCK_ID|cnt))&max_ec_lock.all;		// Generate unique ID for each client/each volume.
}

static void __fill_toma_message_header(struct tomaSimulator *self, struct nvmeibt_client_msg_header *header, int vol_version){
	header->reason 						= NVMEIBT_CLIENT_TR_REASON_TBD;
	header->protocol_version			= self->protocol_version;
	header->config_version				= 0xFFFFFFFF; /* Todo: fill */
	header->volume_config_version		= vol_version;
	header->cookie 						= nvmeib_get_guid();
	//header->clnt_host_name[0]    = '\0';
}

static void __fill_topo_of_raid_payload(struct tomaSimulator*_this, struct nvmeibt_client_msg *msg, const struct tTopoOfPraid* latestTopo, int vol_version) {
	struct tTopoOfPraid  *T 	 	  	= (struct tTopoOfPraid*)(msg+1);
	int err = 0;
	__fill_toma_message_header(_this, &msg->hdr, vol_version);
	err = pthread_mutex_lock(&_this->globalTopo->lock);
	BUG_ON(err);
	msg->thick.praid_version			= latestTopo->header.praid_version;
	msg->thick.data_length 				= tTopoOfPraid_get_size(latestTopo);
	*T = *latestTopo;
	pthread_mutex_unlock(&_this->globalTopo->lock);
}

/* Create message which delivers latest topology to the client */
static struct nvmeibt_client_msg * __create_nack_or_registrable_message(struct tomaSimulator*_this, int *msg_size, const struct nvmeibc_disk_subscription_params *con, const struct tTopoOfPraid* latestTopo, int vol_version, u64 res_ver, bool isNack){

	struct nvmeibt_client_msg *msg 		= sim_kzalloc(sizeof(struct nvmeibt_client_msg)+sizeof(*latestTopo), GFP_KERNEL);
	msg->hdr.msg_type 					= (isNack ? NVMEIBT_CLIENT_MSG_TR_REGISTER_DISK_SEGMENT_NACK : NVMEIBT_CLIENT_MSG_TR_REGISTRABLE_DISK_SEGMENT);
	msg->thick.lock_id	 				= __gen_unique28bits_lock_id(con);
	msg->thick.reservation_mode_version	= res_ver;
	__fill_topo_of_raid_payload(_this, msg, latestTopo, vol_version);
	*msg_size					= sizeof(struct nvmeibt_client_msg) + tTopoOfPraid_get_size(latestTopo);
	_NT(trace_toma_simu_create_nack_or_registrable_message, "TOMA @SIMTOMA_UNIQUEID new lock_id generated: t1->uuid=@UUID, lid=@LID, r1->version=@T_PRV, @RES_MOD_VER", _this->uniqueID, r1uuid(latestTopo), msg->thick.lock_id, msg->thick.praid_version, res_ver);
	return msg;
}

/* Create message which delivers ack without latest topology to the client */
static struct nvmeibt_client_msg* __create_ack_message(struct tomaSimulator *self, int *msg_size, enum NVMEIBT_CLIENT_MSG_TYPES mt, int vol_version, u64 res_ver, int pl_size){
	const int  buf_size 				= sizeof(struct nvmeibt_client_msg) + pl_size;
	struct nvmeibt_client_msg *msg  	= sim_kzalloc(buf_size,0);
	msg->hdr.msg_type 					= mt;
	{
		msg->thick.praid_version		= ~0; /* Todo: fill */
		msg->thick.lock_id	 			= ~0; /* Todo: fill */
		msg->thick.reservation_mode_version  = res_ver;
		msg->thick.data_length 			= pl_size;
	}
	__fill_toma_message_header(self, &msg->hdr, vol_version);
	*msg_size					= buf_size;
	return msg;
}

void __verify_clients_lock_complain_is_valid(struct tomaSimulator* toma, const struct nvmeibt_client_failed_lock_pl *fl ){
	struct ramDiskSimulator *D = serverSimulator_get_ram_by_toma(toma);
	const u64 lock_ind_abs = fl->disk_blkno_4k/LOCKSET_4KS;
	const u64 lock_ind_rel = COMMITTED_ADDR_AS(D, fl->disk_blkno_4k, 4KB, LOCK);

	enum nvmeibc_block_lock_status status = fl->status;
	enum NVMEIBT_CLIENT_LOCK_OP lock_op	  = fl->lock_op;
	BUG_ON(fl->curr == 0);									// Why would client complain on unlocked lock!
	if (fl->is_problem_here) {
		union nvmeib_lock_id *plid = ((void*)&(D->locks[lock_ind_rel]));
		_NT(trace_toma_simu_verify_clients_lock_complain_is_valid, "disk[@TOMA_UNIQUEID].lock[@LSI] = @ALL Client complains, {curr=@CURR status=@STATUS op=@RV}", toma->uniqueID, lock_ind_abs, plid->all, fl->curr, status, lock_op);
		if (0) {		// Daniel: This test is illegal. Client can purge the lru on "schedule unregister" and ask Toma again about the same lock, while IO in air already knows that lock is safe to take and taking it. Toma will see a complain to lock which is taken or free
			spin_lock(&D->cmpxchg_lock);
			if ((!plid->bits.is_stale) || // Client incorrectly complained. Why??
				(D->locks[lock_ind_rel] < SIMULATOR_LAST_RESERVED_LOCK_ID)) { // Lock since it is part of simulated environment
				WARN(true, "disk[%d].lock[%llu] = 0x%x Client complains, {curr=0x%llx status=%d op=%d}\n", toma->uniqueID, lock_ind_abs, plid->all, fl->curr, status, lock_op);
			}
			spin_unlock(&D->cmpxchg_lock);
		}
	} else {
		_NT(trace_1_toma_simu_verify_clients_lock_complain_is_valid, "@TOMA_UNIQUEID: Client complains, on sibling", toma->uniqueID);
	}
}

static void __sendReplyToclient(struct tomaSimulator* simToma,
								const struct nvmeibc_disk_subscription_params stack_con,
								struct nvmeibt_client_msg* msg, int msg_size){
	if (msg) {
		WARN_ON(msg_size > NVMEIB_TOMA_REQ_MAX_LEN);							// Msgs cannot be larger than what transport layer supports
		nvmeibt_client_encode(msg);
		switch (simToma->msg_q_state)  {
		case toma_msg_queueing_state__disabled:
			_ND(trace_toma_simu_sendReplyToclient, "TOMA @SIMTOMA_UNIQUEID invoking client for msg @MSG (cookie=@COOKIE)", simToma->uniqueID, msg, msg->hdr.cookie);
			stack_con.recv_req_cb(NULL, stack_con.arg, (u8*)msg, msg_size);
			sim_kfree(msg);
			break;
		case toma_msg_queueing_state__enabled:
			_ND(trace_toma_simu_sendReplyToclient2, "TOMA @SIMTOMA_UNIQUEID queueing msg @MSG (cookie=@COOKIE)", simToma->uniqueID, msg, msg->hdr.cookie);
			__tomaSimulator_enqueue(simToma, stack_con, (u8*)msg, msg_size);
			break;
		default:
			BUG();
		}
	}
}
static void __sendReplyToclient_unlock(struct tomaSimulator* simToma,
									   const struct nvmeibc_disk_subscription_params *con,
									   struct nvmeibt_client_msg* msg, int msg_size){
	struct nvmeibc_disk_subscription_params stack_con = *con;							// Crucial to store local copy because once we release the mutex, toma sockets can change
	spin_unlock(&simToma->lock);													// Done responding to the message. Ready to process next one
	__sendReplyToclient(simToma, stack_con, msg, msg_size);
}

struct toma_simu_rsp{
	struct nvmeibt_client_msg *msg;
	u32 msg_size;
};

struct toma_simu_rsp const toma_simu_rsp_zero = {.msg=0, .msg_size=0};


struct toma_simu_rsp
__handle_version_mismatch(struct tomaSimulator *simToma,
						  struct tomaSimulator_socket *sock,
						  struct nvmeibt_client_msg_summary const prior_msg_smr,
						  struct nvmeibt_client_msg const * const inMsg  );

struct toma_simu_rsp
__handle_thick_message(struct tomaSimulator *simToma,
					   struct tomaSimulator_socket *sock,
					   struct nvmeibt_client_msg const * const inMsg);

static void __handle_srv_toma_message(struct work_struct *w) {								/* Toma simulator handling of msgs from client*/
	tomaIncomingMessage* inM = container_of(w, tomaIncomingMessage, work_item);
	if (inM->p.len_srvr != inM->p.len_toma) {
		struct nvmeibs_lost_srv_resource_payload *l = (void*)&inM->p.buf[inM->p.len_toma];
		BUG_ON(inM->p.len_srvr != (inM->p.len_toma + sizeof(*l)));
		if (!(inM->srvr->ramDisk.state & ramDisk_down) && inM->srvr->ramDisk.server_disk.di.priv != NULL) {
			nvmeibs_pass_loser_to_serjio(inM->srvr, l);
		} else { /* Due to disk PAUSE server did not get the message */}
	}
	tomaSimulator_recv_clnt_msg(&inM->srvr->simToma, inM->handle, &inM->p);
	tomaIncomingMessage_destroy(inM);
}

void tomaSimulator_recv_clnt_msg(struct tomaSimulator*simToma, u64 handle, struct nvmeibc_disk_toma_send_params *p) {
	//nvmeibt_client_decode decodes in place, which is a problem, cause we cannot check nvmeibt_client_decode_msg_summary function
	struct nvmeibt_client_msg_summary const prior_msg_smr = nvmeibt_client_decode_msg_summary(p->buf, p->len_toma);
	struct nvmeibt_client_msg *inMsg;
	enum NVMEIBT_CLIENT_MSG_DECODE_RES decode_rv;
	struct toma_simu_rsp rsp = toma_simu_rsp_zero;
	struct nvmeibc_disk_subscription_params con;

	decode_rv = nvmeibt_client_decode_new(p->buf, p->len_toma, &inMsg);	// The decoded data of the incoming message
	BUG_ON(decode_rv != NVMEIBT_CLIENT_MSG_DECODE_OK);

	BUG_ON(strcmp(inMsg->hdr.clnt_host_name, utsname()->nodename));

	spin_lock(&simToma->lock);
	if (unlikely(simToma->state == tomaState_down)&&
		((u32)inMsg->hdr.msg_type != NVMEIBT_CLIENT_MSG_RT_UNREGISTER_DISK_SEGMENT)) {
			goto _out; /* A simulator hack. When client disconnects with PAUSE, toma has to implicitly unregister. To avoid this implementation mechanism, dead toma does listen to clients unreg. Thus we also verify that clnt correctly unregistered */
	} else {
		struct tomaSimulator_socket *sock = (void*)findConnectionByHandle(simToma, handle);
		if (!sock) {
			_Emerg("Toma %d: bug - clnt requested %s, on closed socket\n", simToma->uniqueID, nvmeibt_protocol_client_msg_str(inMsg->hdr.msg_type));
			BUG_ON(!simToma->f_unitestBugsSilent);								// Connection should exist, unless unitest generates specific problematic scenarious
			goto _out;
		}

		con = sock->con; // after lock is released, the sock may be dead
		if (simToma->protocol_version < inMsg->hdr.protocol_version){
			rsp = __handle_version_mismatch(simToma, sock, prior_msg_smr, inMsg);
		} else {
			rsp = __handle_thick_message(   simToma, sock, inMsg);
		}
	}
_out:
	spin_unlock(&simToma->lock);
	if (rsp.msg) {
		__sendReplyToclient(simToma, con, rsp.msg, rsp.msg_size);
	}
}

struct toma_simu_rsp
__handle_version_mismatch(struct tomaSimulator *simToma,
					      struct tomaSimulator_socket *sock,
					      struct nvmeibt_client_msg_summary const prior_msg_smr,
					      struct nvmeibt_client_msg const * const inMsg){
	const bool shouldReply = (simToma->state!=tomaState_down);					// Toma cannot reply
	(void)sock;
	if (shouldReply){
		int msg_size = -1;													// Reply message size
		struct nvmeibt_client_msg* outMsg = __create_ack_message(simToma, &msg_size, NVMEIBT_CLIENT_MSG_TR_TOMA_NOT_READY, 0, RESERVATION_MODE_IRRELEVANT, sizeof(prior_msg_smr));
		outMsg->hdr.reason = NVMEIBT_CLIENT_TR_REASON_PROTO_VERSION_MISMATCH;
		outMsg->thick.conversation_ind = inMsg->thick.conversation_ind;
		*(struct nvmeibt_client_msg_summary*)&outMsg->thick.data = prior_msg_smr;
		return (struct toma_simu_rsp){.msg=outMsg, .msg_size=msg_size};
	} else {
		return toma_simu_rsp_zero;
	}
}

static void __on_recov_finish_stop_waiting(struct tomaSimulator *simToma, struct tomaSimulator_socket *sock, const enum NVMEIBT_RECOVERY_TYPE valid_rtype, bool got_clnt_msg, int recov_status) {
	struct sim_toma_recoveries *rr = &sock->rcvrs[valid_rtype];
	struct nvmeibt_client_recovery_start_pl *rst = &rr->task.recov_start;
	const u64 task_id = rst->task.id;
	BUG_ON(!spin_is_locked(&simToma->lock));
	if (task_id) {
		const bool is_user_waiting_for_completion = rr->is_waiting;
		_NT(trace_10_toma_simu_handle_thick_message, "complete waiting for rr=@RR completion=@COMPLETION task=@TASK", rr, &rr->wait, task_id);

		memset(&rr->task, 0, sizeof(rr->task));
		if (is_user_waiting_for_completion){
			WARN(completion_done(&rr->wait), "completion=%p task=0x%llx already done", &rr->wait, task_id);
			rr->recov_status = recov_status;
			complete(&rr->wait);
			if (!got_clnt_msg)
				busy_wait_forever_more(microseconds(20), rr->is_waiting == false);	// Implicit force stop waiting from toma, without receiving client msg. BUG?
		}
		atomic_dec(&simToma->num_running_recoveries);
	}
}

#undef TODO
/******************************************************************************/
union __sim_toma_packed_task_id{
	struct {
		s8 toma;					// Toma global unique index
		u8 sgmnt_lockset_start;		// From which blockset on disk to start. Default (start of segments dlba)
		u8 is_ec;
		u8 type;					// Recovery type
		u16 reserved;
		u16 task_id;				// Unique task id
	};
	u64 all;
};

struct toma_simu_rsp __handle_thick_message(struct tomaSimulator *simToma, struct tomaSimulator_socket *sock, struct nvmeibt_client_msg const * const inMsg)
{
	struct nvmeibt_client_msg* outMsg	 = NULL;												// The header of the reply message
	const bool shouldReply				 = (simToma->state!=tomaState_down);					// Toma cannot reply
	int volVersion = 0, msg_size = -1;													// Reply message size
	const enum NVMEIBT_CLIENT_MSG_TYPES in_type = inMsg->hdr.msg_type;

	_ND(trace_x_toma_simu_handle_thick_message, "Toma @SIMTOMA_UNIQUEID: msg(@MSG_TYPE), cookie=@COOKIE received", simToma->uniqueID, in_type, inMsg->hdr.cookie);

	switch (in_type) { 																			// Act according to input message from the client
		case (NVMEIBT_CLIENT_MSG_RT_REGISTER_DISK_SEGMENT) : {
			const struct tTopoOfVolume *tv	 		= NULL;
			int si = 0;
			const struct tTopoOfPraid* latestTopo 	= toma_findTopologygBySegUUID(simToma,inMsg->thick.disk_segment_uuid, &tv, &si);	// Find the raid of the requested segment
			if (tv) {
				volVersion = tv->version;
				if ((inMsg->thick.reservation_mode_version != RESERVATION_MODE_IRRELEVANT) &&
					(inMsg->thick.reservation_mode_version > tv->reservation_version)) {
					((struct tTopoOfVolume *)tv)->reservation_version = inMsg->thick.reservation_mode_version;
				}
			}
			if (!shouldReply){
			} else if (simToma->state==tomaState_not_ready){ 										// Toma is not ready to serve client. Mark it to future registrable
				outMsg = __create_ack_message(simToma, &msg_size, NVMEIBT_CLIENT_MSG_TR_TOMA_NOT_READY, volVersion, RESERVATION_MODE_IRRELEVANT, 0);
				if (sock->seg_UUID[0] == 0) { 									// New segment
					strcpy(sock->seg_UUID, inMsg->thick.disk_segment_uuid);			// Do the registration
				} else if (strcmp(sock->seg_UUID, inMsg->thick.disk_segment_uuid)) {
					_Emerg("Toma %d: bug - 2 segments on the same connection\n", simToma->uniqueID);
					BUG_NOT_IMPLEMENTED_YET;
				}
				/*if ((enum NVMEIBTC_DS_MODE)latestTopo->s[si].access_mode == NVMEIBTC_DS_MODE_DEAD){
					// Daniel Todo: If this segment has state DEAD, toma will not let client to register and answer TOMA_NOT_READY (Reason = SEG_STATE_NOT_REGISTRABLE)
				} else*/
				sock->should_send_registrable = 1;									// REGISTRABLE code
			}
			else if (!latestTopo) {																	// Request to register to unknown segment
				outMsg = __create_ack_message(simToma, &msg_size, NVMEIBT_CLIENT_MSG_TR_INVALID_DISK_SEGMENT_ID, volVersion, RESERVATION_MODE_IRRELEVANT, 0);
				_NT(trace_toma_simu_handle_thick_message, "Toma @SIMTOMA_UNIQUEID: Sending INVALID_DISK_SEGMENT answer to @SEG. inV=@INV", simToma->uniqueID, inMsg->thick.disk_segment_uuid, inMsg->hdr.volume_config_version);
			}
			else if (inMsg->hdr.volume_config_version != volVersion) { 							// Return Volume missmatch message from toma
				outMsg = __create_ack_message(simToma, &msg_size, NVMEIBT_CLIENT_MSG_TR_VOLUME_MISMATCH, volVersion, tv->reservation_version, 0);
				_NT(trace_1_toma_simu_handle_thick_message, "Toma @SIMTOMA_UNIQUEID: Sending VOLUME_MISMATCH answer to @SEG. inV=@INV, myV=@MYV", simToma->uniqueID, inMsg->thick.disk_segment_uuid, inMsg->hdr.volume_config_version, volVersion);
				//((struct nvmeibt_client_msg*)&messageBuf[0])->volume_config_version		= volVersion;
				if (inMsg->hdr.volume_config_version > volVersion) {
					BUG_ON(strcmp(sock->seg_UUID, inMsg->thick.disk_segment_uuid));			// Daniel, just to make sure no bug in toma simulator.
					_NT(trace_2_toma_simu_handle_thick_message, "Toma @TOMA_UNIQUEID: mark seg @SEGMENT_UUID sock[@SOCK] as REGISTERABALE msg", simToma->uniqueID, inMsg->thick.disk_segment_uuid, (sock-simToma->sock));
					/*if ((enum NVMEIBTC_DS_MODE)latestTopo->s[si].access_mode == NVMEIBTC_DS_MODE_DEAD){
						// Daniel Todo: If this segment has state DEAD, toma will not let client to register and answer TOMA_NOT_READY (Reason = SEG_STATE_NOT_REGISTRABLE)
					} else*/
					sock->should_send_registrable = true;
				} else {
					// Daniel: I talked to Ronen at 28/09/2017. He said that in this case Toma will not send registrable coz client is behind
				}
			}
			else if ((inMsg->thick.praid_version < latestTopo->header.praid_version) || (inMsg->thick.lock_id==0)){ // Return NACK message from toma, since client holds wrong raid1 version or no lock ID
				const char *seg_uuid				= sock->seg_UUID;
				if (sock->lid.all) {
					if (sock->lid.all != inMsg->thick.lock_id) {	// inMsg->lock_id is 0 or different
						_Emerg("Toma %d: bug EXC-1320 - seg %s registered! Ignorring reg request, t_lid=0x%x, c_lid=0x%x!\n", simToma->uniqueID, inMsg->thick.disk_segment_uuid, sock->lid.all, inMsg->thick.lock_id);
						BUG_DUMP_TOMAS_MSGS_WQ();
						/* Do not reply. Real tomas would brutally cut the client, because they assume we had an implicit unregister of client which preceeded the request for NACK */
					} else {
						outMsg = __create_nack_or_registrable_message(simToma, &msg_size, &sock->con, latestTopo, volVersion, tv->reservation_version, true);
					}
				} else if ((seg_uuid[0]!= 0) && strcmp(seg_uuid, inMsg->thick.disk_segment_uuid)) {
					_Emerg("Toma %d: bug - Sending NACK on wrong segment: %s, %s\n", simToma->uniqueID, seg_uuid, inMsg->thick.disk_segment_uuid);
					BUG_NOT_IMPLEMENTED_YET;
				} else
					outMsg = __create_nack_or_registrable_message(simToma, &msg_size, &sock->con, latestTopo, volVersion, tv->reservation_version, true);
			} else if (inMsg->thick.praid_version > latestTopo->header.praid_version) {
				_Emerg("Toma %d: Clients is inventing fictitious topology! Ben Zona, maniak!\n", simToma->uniqueID);
				// Daniel Todo: Real Toma sends TOMA_NOT_READY (Reason = RAID1_VERSION_BEHIND)
				BUG_NOT_IMPLEMENTED_YET;
			} else if (simToma->state == tomaState_no_free_lock_ids) {
				outMsg = __create_nack_or_registrable_message(simToma, &msg_size, &sock->con, latestTopo, volVersion, tv->reservation_version, true);
				outMsg->hdr.reason = NVMEIBT_CLIENT_TR_REASON_LOCKID_ALREADY_TAKEN;				// Fix-up the NACK
				outMsg->thick.lock_id = 0;
				simToma->lid_rej.n_rejects++;
				if (simToma->lid_rej.n_rejects == 1)
					simToma->lid_rej.lid.all = inMsg->thick.lock_id;
				else {
					BUG_ON(simToma->lid_rej.lid.all == inMsg->thick.lock_id);					// Client must change his lock id if Toma rejected. How comeshe tries to register with the same lid. This will create a busy loop
					if (simToma->lid_rej.n_rejects == 3)
						simToma->state = tomaState_running;										// After 3 times messing with client, it is enough and we switch back. Otherwise will have busy messaging loop reg/nack/reg/nack
				}
			} else if (latestTopo->s[si].access_mode == NVMEIBTC_DS_MODE_DEAD) {
				outMsg = __create_ack_message(simToma, &msg_size, NVMEIBT_CLIENT_MSG_TR_TOMA_NOT_READY, volVersion, RESERVATION_MODE_IRRELEVANT, 0);
				outMsg->hdr.reason = NVMEIBT_CLIENT_TR_REASON_SEG_STATE_NOT_REGISTRABLE;
			} else {  /* Return REG ACK msg. Important: inMsg->praid_version == latestTopo->header.praid_version, but can change during the run of this function! */
				outMsg = __create_ack_message(simToma, &msg_size, NVMEIBT_CLIENT_MSG_TR_REGISTER_DISK_SEGMENT_ACK, volVersion, tv->reservation_version, 0);
				outMsg->thick.praid_version			= inMsg->thick.praid_version;				// Might already be < latestTopo->header.praid_version, because leader spreading a new topology
				outMsg->thick.lock_id  				= inMsg->thick.lock_id;						// Copy existing lock ID
				BUG_ON(outMsg->thick.lock_id==0);												// Illegal to register with empty lock id, previous if-else should have dealt with that
				if (sock->seg_UUID[0] == 0) { 								// New segment
					strcpy(sock->seg_UUID, inMsg->thick.disk_segment_uuid);
					_NT(trace_3_toma_simu_handle_thick_message, "Toma @SIMTOMA_UNIQUEID: registering client first time to segment: @SEG, lid=@LID", simToma->uniqueID, inMsg->thick.disk_segment_uuid, outMsg->thick.lock_id);
				} else if (strcmp(sock->seg_UUID, inMsg->thick.disk_segment_uuid)) {
					_Emerg("Toma %d: bug - 2 segments on the same connection: %s, %s\n", simToma->uniqueID, sock->seg_UUID, inMsg->thick.disk_segment_uuid);
					BUG_NOT_IMPLEMENTED_YET;
				} else if (sock->lid.all == LS_UNLOCKED){								// Known segment re-registration (client previously unregistered and now registers again)
					_NT(trace_4_toma_simu_handle_thick_message, "Toma @SIMTOMA_UNIQUEID: registering client to segment: @SEG, lid=@LID", simToma->uniqueID, inMsg->thick.disk_segment_uuid, outMsg->thick.lock_id);
					BUG_ON(unlikely(sock->wait_for_switch_topo_ack));
				} else {                                                                        // Already registered
					if (sock->lid.all != inMsg->thick.lock_id) { 				// Clnt tries to register with a new lock id without unregisterring first. This is illegal!
						pr_alert("Toma %d: Bug EXC-2628(LOCKID_MESS) re-register seg=%s, lid=0x%x => 0x%x. Old lock left behind\n", simToma->uniqueID, inMsg->thick.disk_segment_uuid, sock->lid.all, inMsg->thick.lock_id);
						BUG_DUMP_TOMAS_MSGS_WQ(); // Real Toma is going to send NOT_READY with reason LOCKID_MESS and IO will be stuck for 30[sec]! not acceptable
					} else { // sock->lid.all == inMsg->thick.lock_id,  This is not a bug. Example: raid sends registration requests on both segments. Both tomas return NACKs with new topology. Both NACK's are converted by block.c to 2 registration requests, totally sending 4 requests (2 to each segment).
						_NT(trace_5_toma_simu_handle_thick_message, "Toma @SIMTOMA_UNIQUEID: ignoring. Client tries to re-register to registered segment: @SEG with identical lid=@LID", simToma->uniqueID, inMsg->thick.disk_segment_uuid, inMsg->thick.lock_id);
					}
				}
				sock->lid.all = outMsg->thick.lock_id;									// Set to true with lock id
			}
			if (outMsg)
				outMsg->thick.conversation_ind = inMsg->thick.conversation_ind;
			break;
		}
		case (NVMEIBT_CLIENT_MSG_RT_UNREGISTER_DISK_SEGMENT) : { 								// No ack will be sent by Toma to the client
			//const struct tTopoOfPraid* latestTopo 	= toma_findTopologygBySegUUID(simToma,inMsg->thick.disk_segment_uuid, NULL, NULL); //<-- May be null when shrinking a volume.
			__simToma_unregister_segment(simToma, sock, inMsg->thick.lock_id, !inMsg->thick.rt_never_reged_on_seg);
			if (shouldReply && !simToma->f_forbidSendingUnregisterAck) {
				outMsg = __create_ack_message(simToma, &msg_size, NVMEIBT_CLIENT_MSG_TR_UNREGISTER_DISK_SEGMENT_ACK, inMsg->hdr.volume_config_version, RESERVATION_MODE_IRRELEVANT, 0);
				outMsg->thick.praid_version = inMsg->thick.praid_version; // Might be != latestTopo->header.praid_version;
				outMsg->thick.lock_id		= inMsg->thick.lock_id;
				outMsg->thick.conversation_ind = inMsg->thick.conversation_ind;
			}
			break;
		}
		case (NVMEIBT_CLIENT_MSG_RT_SWITCH_PRAID_TOPOLOGY_ACK) : {
			// No need to answer the client. Update internal states (can switch to next topology and start recovery only when all the clients ack'ed)
			const char *seg_uuid = inMsg->thick.disk_segment_uuid;
			const struct tTopoOfPraid* latestTopo = toma_findTopologygBySegUUID(simToma, seg_uuid, NULL, NULL);
			const int cl_version = inMsg->thick.praid_version;
			const u32 client_id = inMsg->thick.lock_id;
			const int my_version = latestTopo->header.praid_version;
			const char *prnt_msg = (sock->wait_for_switch_topo_ack) ? "removing wait_swtopo" : "not waiting for wait_swtopo";
			// Test for: (enum NVMEIBT_CLIENT_TR_REASON)(inMsg->hdr.reason) == NVMEIBT_CLIENT_RT_REASON_DELAYED_SW_TOPO_UPD
			BUG_ON(!latestTopo);														// What da fuck?? unitest timing bug
			BUG_ON(sock->wait_for_switch_topo_ack && (cl_version != my_version));		// Client must give the correct ack on the correct version (allow bypass of old switch topo acks)! BUG possibly occurs if unitest environment updates the topology while Toma simulator is waiting for switch Topo.
			if (sock->lid.all)
				BUG_ON(sock->lid.all != client_id);
			_NT(trace_6_toma_handle_thick_message, "Toma @SIMTOMA_UNIQUEID: @STR c_lid=@C_LID, @C_PRV, seg=@SEG", simToma->uniqueID, prnt_msg, client_id, cl_version, seg_uuid);
			sock->wait_for_switch_topo_ack = false;
			break;
		}
		case (NVMEIBT_CLIENT_MSG_RT_RECOVER_PROGRESS): {
			const void* payload = inMsg->thick.data;
			const struct nvmeibt_client_recovery_status_pl *rpl = &((struct nvmeibt_client_msg_pl *)payload)->recov_status;
			_NT(trace_8_toma_simu_handle_thick_message, "Toma @SIMTOMA_UNIQUEID: Clnt-Recovery progress. rv=@RET_CODE, taskid=@TASKID, tasktype=@TASKTYPE, num_left=@NUM_LEFT", simToma->uniqueID, rpl->ret_code, rpl->task.id, rpl->task.type, rpl->num_locks_left);
			BUG_ON(rpl->ret_code != 0);
			break;
		}
		case (NVMEIBT_CLIENT_MSG_CT_LOCKID_CACHE_PURGE_ACK): {	// Subcase of recovery finish
			const void* payload = inMsg->thick.data;
			const struct nvmeibt_lockid_cache_purge_pl *rpl = &((struct nvmeibt_client_msg_pl *)payload)->lockid_cache_purge;
			const enum NVMEIBT_RECOVERY_TYPE valid_rtype = NVMEIBT_RECOVERY_TYPE_STALE_LOCKS_PURGE;
			struct nvmeibt_client_recovery_start_pl *rst = &sock->rcvrs[valid_rtype].task.recov_start;
			if (rst->task.id) {									// Else if '0' then Toma canceled its purge request implicitly and locally(without notifying client, so client still purges and sends ACK)
				BUG_ON(rst->task.id != rpl->purge_seqno);		// Running 2 recoveries in parallel?
				__on_recov_finish_stop_waiting(simToma, sock, valid_rtype, true, 0 /* recov_status */);
			}
			break;
		}
		case (NVMEIBT_CLIENT_MSG_RT_RECOVER_FINISH): {
			const void* payload = inMsg->thick.data;
			const struct nvmeibt_client_recovery_status_pl *rpl = &((struct nvmeibt_client_msg_pl *)payload)->recov_status;
			const enum NVMEIBT_RECOVERY_TYPE rec_type = (rpl->task.type);
			const union __sim_toma_packed_task_id packed_task_id = {.all = rpl->task.id};
			const enum NVMEIBT_RECOVERY_TYPE valid_rtype = nvmeibt_recovery_type_cast(rec_type);
			struct nvmeibt_client_recovery_start_pl *rst = &sock->rcvrs[valid_rtype].task.recov_start;
			const bool stop_waiting = (rpl->task.id == rst->task.id);
			if (rpl->task.id) {
				if (rst->task.id) {
					if (rpl->ret_code == 0) {					// Client finished successfully the recovery we expect
						BUG_ON(rst->task.id != rpl->task.id);
						BUG_ON(packed_task_id.type != rec_type);
						BUG_ON(rpl->next_unfixed_lock != (rst->start_lock + rst->num_locks));
					} else if (rpl->task.id == rst->task.id) {	// Client failed the recovery we expect
						BUG_ON(rpl->next_unfixed_lock >= (rst->start_lock + rst->num_locks));
					} else if (rpl->praid_version != 0) {		 // Old Client msg of stopping unrelated/previous recovery
					} else { // Client rejected query about recovery or refused to take on another task while previous is runinng
						BUG_ON(rpl->next_unfixed_lock != 0);
					}
				} else if (rpl->praid_version != 0) { // Old Client msg of stopping unrelated/previous recovery
					BUG_ON(rpl->ret_code == 0);
				} else { // Client rejected query about recovery or refused to take on another task while previous is runinng
					BUG_ON(rpl->next_unfixed_lock != 0);
					BUG_ON(rpl->ret_code == 0);
				}
			}
			_NT(trace_9_toma_simu_handle_thick_message, "Toma @SIMTOMA_UNIQUEID: Clnt-Recovery finish. rpl{rv=@RET_CODE, tid=@TID, num_left=@NUM_LEFT}, rst={tid=@TID, type=@TYPE}, stop_waiting=@STOP_WAITING", simToma->uniqueID, rpl->ret_code, rpl->task.id, rpl->num_locks_left, rst->task.id, rst->task.type, stop_waiting);
			if (rec_type == NVMEIBT_RECOVERY_TYPE_DIRTY_REBUILD) {
				struct ramDiskSimulator *D = serverSimulator_get_ram_by_toma(simToma);
				int i, start = packed_task_id.sgmnt_lockset_start + (int)rst->start_lock, end = start + (int)rst->num_locks;
				spin_lock(&D->cmpxchg_lock);
				if (rpl->ret_code == 0) {
					for (i = start; i < end; i++)
						if (!packed_task_id.is_ec) { // EC recovery TODO: skip indexes of non-owner
							BUG_ON(D->dbits[i].all_bits);
						}
				}
				spin_unlock(&D->cmpxchg_lock);
			}

			if (stop_waiting)
				__on_recov_finish_stop_waiting(simToma, sock, valid_rtype, true, rpl->ret_code);

			break;
		}
		case (NVMEIBT_CLIENT_MSG_CT_STALE_LOCK): {
			const struct nvmeibt_client_failed_lock_pl *complain = (void*)&inMsg->thick.data;
			struct nvmeibt_cleaned_stalock_info* reply_pl;
			__verify_clients_lock_complain_is_valid(simToma, complain);								// Curently toma does do anything for this message and cleans locks on unregister
			outMsg = __create_ack_message(simToma, &msg_size, NVMEIBT_CLIENT_MSG_TC_LOCK_CLEANED, inMsg->hdr.volume_config_version, RESERVATION_MODE_IRRELEVANT, sizeof(*reply_pl));
			outMsg->thick.praid_version = inMsg->thick.praid_version; // Might be != latestTopo->header.praid_version;
			outMsg->thick.lock_id		= inMsg->thick.lock_id;
			outMsg->thick.conversation_ind = inMsg->thick.conversation_ind;
			reply_pl    = (void*)&outMsg[1];
			reply_pl->lock_id = (u32)complain->curr;							// Toma always answers that status if OK to use
			if (1) {
				const union nvmeib_lock_id l = {.all = (reply_pl->lock_id & max_ec_lock.all), };
				if (l.all > SIMULATOR_LAST_RESERVED_LOCK_ID)
					memcpy(reply_pl->cuuid, sock->cuuid, 16);			// Old stale lock of this client
				else if (l.bits.lock_id == SIMULATOR_OLD_FORGOTTEN_LOCK_ID)
					memset(reply_pl->cuuid, 0, 16);						// NULL UUID
				else if (l.bits.lock_id == SIMULATOR_CLNT_ALLIEN_LOCK_NO_J) {
					memcpy(reply_pl->cuuid, SIMULATOR_ALIEN_CLIENT__get_uuid(), 16);
				} else 												// Stale lock generated by other simulated client
					memcpy(reply_pl->cuuid, SIMULATOR_OTHER_CLIENT__get_uuid(), 16);
			}
			break;
		}

		case (NVMEIBT_CLIENT_MSG_CT_FAILED_LOCK): {
			const struct nvmeibt_client_failed_lock_pl *c = (void*)&inMsg->thick.data;
			_Emerg("op=%d: cmpxchng=0x%llx->0x%llx addr=%x num_retry=0x%x\n", c->lock_op, c->comp, c->xchg, (u32)c->disk_blkno_4k, c->num_retries);
			break;
		}

		// ------------- Unsuported messages ---------------------
		case (NVMEIBT_CLIENT_MSG_CT_FAILED_CMD): {
			const struct nvmeibt_client_msg_pl *pl = (const struct nvmeibt_client_msg_pl *)&inMsg->thick.data;
			const struct nvmeibt_client_failed_cmd_pl* failed_cmd = &pl->failed_cmd;
			if (failed_cmd->error_code) { 													// Verify that we expect a report of this IO failure
				BUG_ON((int)((signed short)failed_cmd->error_code) != simToma->expect_io_failure);
				BUG_ON(failed_cmd->fail_map != simToma->fail_mask);							// Must be empty for now
				BUG_ON(failed_cmd->fix_map != simToma->fix_mask);							// Must be full for now
				simToma->expect_io_failure = 0;
				simToma->fail_mask = 0;
				simToma->fix_mask = 0;
			}
			break;
		}
		case (NVMEIBT_CLIENT_MSG_CT_DI_DETECTED): {
			const struct nvmeibt_client_msg_pl *pl = (const struct nvmeibt_client_msg_pl *)&inMsg->thick.data;
			const struct nvmeibt_client_failed_cmd_pl* failed_cmd = &pl->failed_cmd;
			_NT(trace_11_toma_simu_handle_thick_message, "Toma @SIMTOMA_UNIQUEID: got di on @DLBA", simToma->uniqueID, failed_cmd->offset);
			break;
		}
		default: {
			_Emerg("Toma %d: unsupported message: %s\n", simToma->uniqueID, nvmeibt_protocol_client_msg_str(inMsg->hdr.msg_type));
			BUG_NOT_IMPLEMENTED_YET;
			break;
		}
	}
	return (struct toma_simu_rsp){.msg=outMsg, .msg_size=msg_size};
}

int nvmeibs_toma_intercept_msg(struct serverSimulator *S, u64 handle, struct nvmeibc_disk_toma_send_params *p) {
	tomaIncomingMessage *inM = tomaIncomingMessage_create(S, handle, p); // Create a local copy for asyncronous thread execution.
	INIT_WORK(&inM->work_item, __handle_srv_toma_message);
	queue_work(toma_rcv_wq, &inM->work_item);	// dispatch to toma_wq so all these msgs are processed in strict order
	return 0;
}

/* Yuri: Once again a hack. Here I use handle, look up tr rb_tree, get the tr, from that toplologies, from that device, from that cips, from that private block instance data, from that inst_id. */
static int tomaSimulator_inst_id_from_handle(u64 handle) {
	struct nvmeibc_subscription_ctx *__get_tr(u64);
	void __put_tr(struct nvmeibc_subscription_ctx *tr);
	int inst_id;
	struct nvmeibc_subscription_ctx * tr = __get_tr(handle);
	inst_id = nvmeibc_cinst_get_blok_inst_num(tr->nt->nd->cips);
	__put_tr(tr);
	return inst_id;
}

int tomaSimulator_subscribe_seg(struct tomaSimulator* simToma, u64 handle, struct nvmeibc_disk_subscription_params *params, u8* cuuid, int rv) {
	// if (simToma->state == tomaState_down) return -ENOMEM;	// Real implementation is done on client side only and deffers the subscription, so we as well do it right away. However you can uncomment this to generate a different error simulating client side failure due to no memory
	spin_lock(&simToma->lock);
	if (simToma->state == tomaState_down) {
		rv = -EAGAIN; /* This usecase is not an error for upper software layer*/
	}
	simToma->socks_gen++;							// Change to hashtable
	for (u32 i = 0; i < ARRAY_SIZE(simToma->sock); ++i){ //find first unused socket
		BUG_ON(((i+1) == (int)ARRAY_SIZE(simToma->sock) && simToma->sock[i].key));
		if (!simToma->sock[i].key){
			simToma->sock[i].con = *params;
			simToma->sock[i].key = handle;
			simToma->sock[i].inst_id = tomaSimulator_inst_id_from_handle(handle);
			memcpy(simToma->sock[i].cuuid, cuuid, 16);
			break;
		}
	}
	spin_unlock(&simToma->lock);
	_NT(trace_toma_simu_tomaSimulator_subscribe_seg, "Toma @SIMTOMA_UNIQUEID: subscribe to segment with handle=@HANDLE, rv=@RV", simToma->uniqueID, handle, rv);
	return rv;
}

static inline bool __is_toma_recovery_in_flight(const struct sim_toma_recoveries * rr){
	return rr->task.recov_start.task.id != 0 && rr->recov_caller == UNI_RECOV_CALLER_TOMA;
}

static void ___toma_clean_socket(struct tomaSimulator* simToma, int conIndex) {
	struct tomaSimulator_socket *sock = &simToma->sock[conIndex];
	struct sim_toma_recoveries *rr = array_find_if (sock->rcvrs, __is_toma_recovery_in_flight);
	if (rr){
		_NE_dmesg(error_toma_simu_toma_clean_socket,  "Unsubscribe while recovery is running: @TASK_ID", rr->task.recov_start.task.id);
		WARN(!simToma->f_protoBugsSilent,       "Unsubscribe while recovery is running: 0x%llx\n", rr->task.recov_start.task.id);
	}

	BUG_ON(!spin_is_locked(&simToma->lock));
	sock->seg_UUID[0] 							= 0;	// Remove the segment name
	sock->lid.all								= LS_UNLOCKED;
	sock->should_send_registrable				= false;
	sock->wait_for_switch_topo_ack				= false;
	for (u16 i = 0; i < ARRAY_SIZE(sock->rcvrs); ++i) {
		__on_recov_finish_stop_waiting(simToma, sock, i, false /*force stop*/, -1 /* recov_status */);		// does force stop only if needed
	}
	sock->key 									= 0;   	// Delete the key from the hash
	memset(&sock->con, 0, sizeof(sock->con));
}

static void __tomaSimulator_unsubscri_seg_cb(struct work_struct *w) {
	tomaIncomingMessage *inM = container_of(w, tomaIncomingMessage, work_item);
	struct tomaSimulator* simToma 	 = &inM->srvr->simToma;
	const struct tomaSimulator_socket *sock = NULL;
	const u64 handle = inM->handle;
	//if (simToma->state == tomaState_down)				// Respond even when toma is down for book-keeping. In real system this action is done locally on client side and not in toma
	spin_lock(&simToma->lock);
	BUG_ON(!(sock = findConnectionByHandle(simToma, handle)));// Connection should exist
	_NT(trace_toma_simu_tomaSimulator_unsubscri_seg_cb_0, "Toma @SIMTOMA_UNIQUEID: unsubscribe from segment @SEGMENT_UUID with handle @HANDLE and t_lid=@LID", simToma->uniqueID, sock->seg_UUID, handle, sock->lid.all);
	if ((simToma->state != tomaState_down) && (sock->lid.all)) { // Client and toma are connected
		const struct tTopoOfPraid* latestTopo = toma_findTopologygBySegUUID(simToma, sock->seg_UUID, NULL, NULL);
		if (latestTopo == NULL) {
			do_once(pr_alert("Toma %d: Bug EXC-1920 - client did not unregister from downgraded raid1\n", simToma->uniqueID));
		} else if (simToma->f_protoBugsSilent == false) {
			do_once(pr_alert("Toma %d: Attemp to unsubscribe from seg %s before unregistering!\n", simToma->uniqueID, sock->seg_UUID));
		}
		// Todo: unregister the client here.
	}
	_NT(trace_toma_simu_tomaSimulator_unsubscri_seg_cb, "Toma @SIMTOMA_UNIQUEID: unsubscribed from segment @SEGMENT_UUID with handle @HANDLE", simToma->uniqueID, sock->seg_UUID, handle);
	___toma_clean_socket(simToma,(sock-simToma->sock));
	simToma->socks_gen++;							// Change to hashtable
	spin_unlock(&simToma->lock);
	tomaIncomingMessage_destroy(inM);
}

int tomaSimulator_unsubscri_seg(struct tomaSimulator *simToma, u64 handle) {
	tomaIncomingMessage *inM = tomaIncomingMessage_create(container_of(simToma, struct serverSimulator, simToma), handle, NULL);
	INIT_WORK(&inM->work_item, __tomaSimulator_unsubscri_seg_cb);
	queue_work(toma_rcv_wq, &inM->work_item);	// dispatch to toma_wq so all these msgs are processed in strict order
	return 0;
}

/**************** Simulator of toma internal representation ******************/
static struct tomaSimulator* tomasNetwork[NVMESH_N_PHYS_DISKS] = {0};	// Internal array of all initialized tomas. Each can access other tomas, simulating a network. pointer to the leader, which makes all decisions regarding topology.
static struct tomaSimulator* tomaLeader	 = NULL;					// Pointer to the currently elected leader (from the array above). Initialized as the first toma.
static int numTomasInNetwork = 0;

struct tomaSimulator* tomaSimulator_getToma_by_disk_id(u32 i) {
	BUG_ON(i >= NVMESH_N_PHYS_DISKS);
	return tomasNetwork[i];
}

void tomaSimulator_dump(struct tomaSimulator* toma) {
	u32 n_sgmnts = 0;
	BUG_ON(!spin_is_locked(&toma->lock));
	for (u32 i=0; i<ARRAY_SIZE(toma->sock); i++){
		if (toma->sock[i].key){
			n_sgmnts += 1;
			_NT(trace_toma_simu_tomaSimulator_dump, "@SOCK_IDX)\tuuid=@SEGMENT_UUID, c_lid=@C_LID", i, toma->sock[i].seg_UUID, toma->sock[i].lid.all);
		}
	}
	_NT(trace_1_toma_simu_tomaSimulator_dump, "Toma @TOMA_UNIQUEID: @N_SGMNTS segs", toma->uniqueID, n_sgmnts);
}

void tomaNetwork_dump(void) {
	int	i;
	for (i=0; i<numTomasInNetwork; i++){
		struct tomaSimulator* toma = tomasNetwork[i];
		spin_lock(&toma->lock);
		tomaSimulator_dump(toma);
		spin_unlock(&toma->lock);
	}
}

void tomaSimulator_set_stale_lock(struct tomaSimulator *_this, u64 addr, u64 lock_id){
	struct ramDiskSimulator *ssd = serverSimulator_get_ram_by_toma(_this);
	const u64 i = COMMITTED_ADDR_AS(ssd, addr, 4KB, LOCK);
	spin_lock(&_this->lock);
	_this->stale_locks[i].all = (u32)lock_id;
	spin_unlock(&_this->lock);
}

void tomaSimulator_unset_stale_lock(struct tomaSimulator*_this, u64 addr){
	struct ramDiskSimulator *ssd = serverSimulator_get_ram_by_toma(_this);
	const u64 i = COMMITTED_ADDR_AS(ssd, addr, 4KB, LOCK);
	spin_lock(&_this->lock);
	_this->stale_locks[i].all = LS_UNLOCKED;
	spin_unlock(&_this->lock);
}

bool tomaSimulator_has_stale_lock(struct tomaSimulator*_this, u64 addr){
	struct ramDiskSimulator *ssd = serverSimulator_get_ram_by_toma(_this);
	const u64 i = COMMITTED_ADDR_AS(ssd, addr, 4KB, LOCK);
	bool rv;
	spin_lock(&_this->lock);
	rv = (_this->stale_locks[i].bits.is_stale);
	spin_unlock(&_this->lock);
	return rv;
}

void tomaSimulator_verify_no_locks(struct tomaSimulator* _this){
	int i, nTaken;
	for (i=0, nTaken=0; i<RAMDISK_DATA_LOCK_SIZE; i++){	// Verify that all the locks were released
		if (_this->stale_locks[i].all != LS_UNLOCKED) {
			const u64 is_stale = _this->stale_locks[i].bits.is_stale;
			_Emerg("TOMA:%d, lock %d is %s 0x%x\n", _this->uniqueID, i, (is_stale ? "stale" : "locked by"), _this->stale_locks[i].all);
			nTaken++;
			BUG_ON(nTaken);
		}
	}
}

int tomaSimulator_clear_all_locks(struct tomaSimulator* _this){
	int i, nTaken;
	for (i=0, nTaken=0; i<RAMDISK_DATA_LOCK_SIZE; i++){	// Verify that all the locks were released
		if (_this->stale_locks[i].all != LS_UNLOCKED) {
			_this->stale_locks[i].all = LS_UNLOCKED;
			nTaken++;
		}
	}
	return nTaken;
}

bool tTopoOfPraid_verify_identical_locks(const struct tTopoOfPraid* r1){
	int i,							 disk_ids[N_MAX_RAID_SLICE_LEN] = {[0 ... N_MAX_RAID_SLICE_LEN-1] = -1};
	struct tomaSimulator			   *tomas[N_MAX_RAID_SLICE_LEN] = {[0 ... N_MAX_RAID_SLICE_LEN-1] = NULL};
	const struct tomaSimulator_socket 	*sock[N_MAX_RAID_SLICE_LEN] = {[0 ... N_MAX_RAID_SLICE_LEN-1] = NULL};
	u32 							 lock_ids[N_MAX_RAID_SLICE_LEN] = {[0 ... N_MAX_RAID_SLICE_LEN-1] = -1};
	if (r1->header.n_segments<=1)
		return true;													// No need to verify non mirrored volumes

	/* Yuri: @TODO: rewrite */
	tTopoOfPraid_getdisks_by_order(r1, disk_ids);
	for (i=0; i<r1->header.n_segments; i++) {
		tomas[i] 	= tomasNetwork[disk_ids[i]];
		spin_lock(&tomas[i]->lock);
		sock[i] 	= findConnectionBySegUUID(tomas[i], r1->s[i].uuid);
		lock_ids[i] = sock[i]->lid.all;
		spin_unlock(&tomas[i]->lock);
	}
	for (i=1; i<r1->header.n_segments; i++) {
		if (lock_ids[i-1]!=lock_ids[i]) {
			tomaNetwork_dump();
			BUG();
			return false;
		}
	}
	return true;
}

void tomaSimulator_init(struct tomaSimulator* _this, const struct mongo_db_simu *conf, struct tTopoOfNVMesh *topo, u32 protocol_version){
	int i, nMaxRegSegs = ARRAY_SIZE(_this->sock);
	if (toma_rcv_wq == NULL) {		// initialization of the module
		BUG_ON(!(toma_rcv_wq = create_singlethread_workqueue(TOMA_THREAD_NAME)));
	}
	memset(_this,0,sizeof(*_this));
	_this->uniqueID			= numTomasInNetwork++;
	_this->state			= tomaState_running;
	_this->global_conf 		= conf;
	_this->globalTopo 		= topo;
	_this->expect_io_failure = 0;
	_this->fail_mask         = 0;
	_this->fix_mask          = 0;
	spin_lock_init(&_this->lock);
	if (1) {
		pthread_mutexattr_t recov_msg_lock_attr;
		memset(&recov_msg_lock_attr, 0, sizeof(recov_msg_lock_attr));
		BUG_ON(pthread_mutexattr_settype(&recov_msg_lock_attr, PTHREAD_MUTEX_RECURSIVE));
		pthread_mutex_init(&_this->recov_msg_lock, &recov_msg_lock_attr);
	}
	init_completion(&_this->serjio_done);
	atomic_set(&_this->num_running_recoveries, 0);
	for (i=0; i<nMaxRegSegs; i++){
		_this->sock[i].seg_UUID = (char*)sim_kzalloc(40,0);
		for (u16 j = 0; j < ARRAY_SIZE(_this->sock[i].rcvrs); ++j){
			init_completion(&_this->sock[i].rcvrs[j].wait);
		}
	}

	_this->msg_q_state = toma_msg_queueing_state__disabled;
	toma_msg_q__init(&_this->msg_q, _this->uniqueID);
	_this->recov_task_id = 0;
	tomasNetwork[_this->uniqueID] = _this;								// Sign-in to tomas network
	if (_this->uniqueID == 0)
		tomaLeader = _this;												// Election.
	_this->protocol_version = protocol_version;
	_this->stale_locks = sim_kzalloc(sizeof(*_this->stale_locks)*RAMDISK_DATA_LOCK_SIZE, GFP_KERNEL);
	if (_this->stale_locks == NULL) {
		_NE_dmesg(error_toma_simu_tomaSimulator_init, "Failed to allocate stale_locks array");
	}
	_this->ignore_jour_gc_launch_request = false;
}

void tomaSimulator_destroy(struct tomaSimulator* _this){
	int nMaxCons 	= ARRAY_SIZE(_this->sock);
	int i, nOpen	= 0;
	BUG_ON(tomaSimulator_get_msg_queue_size(_this) != 0);				// if there are queued msgs then they were lost. each test case needs to end with no msgs queued !!!
	for (i=0, nOpen=0; i<nMaxCons; i++){ 								// Verify all clients gracefully unregistered from all their segments
		struct tomaSimulator_socket *sock = &_this->sock[i];
		if (sock->lid.all){
			_Emerg("Toma %d: segment still registered to %s with t_lid=0x%x\n", _this->uniqueID, sock->seg_UUID, sock->lid.all);
			nOpen++;
		}
		BUG_ON(nOpen);
	}
	for (i=0, nOpen=0; i<nMaxCons; i++){ 								// Verify that there are no more open connections with clients
		if (_this->sock[i].key != 0) {
			_Emerg("Toma %d: %dth Connection left open. Key:0x%llx\n", _this->uniqueID, i, _this->sock[i].key);
			nOpen++;
		}
		BUG_ON(nOpen);
	}
	pthread_mutex_destroy(&_this->recov_msg_lock);
	spin_lock_destroy(&_this->lock);
	for (i=0; i<nMaxCons; i++)
		sim_kfree(_this->sock[i].seg_UUID);

	sim_kfree(_this->stale_locks);

	tomasNetwork[_this->uniqueID] = NULL;								// Sign-out of tomas network
	if (tomaLeader == _this)
		tomaLeader = NULL; 												// No leader, when all tomas die
	memset(_this, 0, sizeof(*_this));
	numTomasInNetwork--;
	if (numTomasInNetwork == 0) {
		destroy_workqueue(toma_rcv_wq);
		toma_rcv_wq = NULL;
	}
}

void tomaSimulator_disconnect(struct tomaSimulator* _this){
	spin_lock(&_this->lock);
	_this->state = tomaState_down;
	spin_unlock(&_this->lock);
}

void tomaSimulator_reconnect(struct tomaSimulator* _this){

	spin_lock(&_this->lock);
	_this->state = tomaState_running;
	spin_unlock(&_this->lock);
	tomaSimulator_send_registrables(_this);
}

void tomaSimulator_onClntDiscovery(	struct tomaSimulator* _this, uuid_be* cuuid, int jri) {
	// Todo: Save this to persistancy (as done with topology.
	// Todo: Fill his CID when client registers to segment to make lockid to CID map
	(void)_this;	(void)cuuid;	(void)jri;
}

void tomaSimulator_onClntDiskRelese(struct tomaSimulator* _this, u32 jri) {
	(void)_this, (void)jri;
	//struct ramDiskSimulator *D = serverSimulator_get_ram_by_toma(_this);	// Todo: Toma finds the disk and problematic locks by CID
	// Todo: Start EC recovery here:
	// 1. Via D->J2D find the journal chunk of this CID
	//
//if (1) {
//	u32 i, n_chunks = ARRAY_SIZE(D->ec);
//	for (i = 0; i < n_chunks; i++) {
//		jchunk_simu *j2d = &D->ec[i];
//		if (j2d->cid == cid) {
//			j2d->is_under_recovery = true;
//			break;
//		}
//	}
//	//BUG_ON(i < n_chunks);											// should be a chunk per registered client, in EC only
//}
	// 2. Filter them, Sort them
	// 3. Launch recovery
	// 4. Assume recovery finished.
}
int nvmeibs_toma_report_event_blkset_recovered(struct nvmeibs_disk_info *di, const char *ds_uuid, u64 blkset_num,
	u64 blkset_slba, u64 pre_recov_lock_val, struct nvmeibs_async_cookie_params *cookie) {
	(void)blkset_num;
	(void)cookie; /* Used to ensure sync wait for toma response in real system, in symulator it is sync anyway so who cares? */
	if (tomaLeader) {
		struct tomaSimulator *toma = &as_serverSimulator(di)->simToma;
		struct ramDiskSimulator *ssd = serverSimulator_get_ram_by_toma(toma);
		struct tomaSimulator_socket *sock;
		u64 const li_abs = blkset_slba/LOCKSET_4KS;
		u64 const li_rel = COMMITTED_ADDR_AS(ssd, blkset_slba, 4KB, LOCK);

		union nvmeib_lock_id is_read_mask = {.bits = {.is_read = 1 }};
		union nvmeib_lock_id expected = {.all = 0};		// Expected value of lock in RAM
		spin_lock(&toma->lock);
		for_each_socket_by_uuid(toma, ds_uuid, sock) {
			BUG_ON(!sock);
			expected.all = sock->lid.all;			// Hot recovery is currently holding this lock while notifying blockset recovered that it finished cleaning the journal
			//spin_lock(&ssd->cmpxchg_lock);
			BUG_ON(expected.all != LS_UNLOCKED && (ssd->locks[li_rel] & (~is_read_mask.all)) != expected.all);	// is_read can be false or true
			//spin_unlock(&ssd->cmpxchg_lock);
			if (toma->stale_locks[li_rel].all != LS_UNLOCKED) {
				_NT(trace_nvmeibs_toma_report_event_blkset_recovered, "Stale lock recovery completed on disk[@TOMA_UNIQUEID].lock[@LSI]: removing ancestor from history: @LOCK_ENT", toma->uniqueID, li_abs, toma->stale_locks[li_rel].all);
				if (toma->stale_locks[li_rel].all != (u32)pre_recov_lock_val) {
					_NE_dmesg(error2_nvmeibs_toma_report_event_blkset_recovered, "We recovered a stale lock: @LOCK_ENT, but had a value @LOCK_ENT in TOMA", (u32)pre_recov_lock_val, toma->stale_locks[li_rel].all);
					BUG();
				}
				toma->stale_locks[li_rel].all = LS_UNLOCKED;
				BUG_ON(!(ssd->locks[li_rel] & is_read_mask.all));	// Verify is_read==true, due to HTR
			}
		}
		spin_unlock(&toma->lock);
	} else {
		BUG(); /* Recovery, no toma, should not happen */
	}
	return 0;
}

void tomaSimulator_waitProtoEnd(struct tomaSimulator* _this){
	NFIN;
	wq_drain(toma_rcv_wq); (void)_this;// Here messages like: RT_RECOVER_FINISH, CT_STALE_LOCK, can remain and any newly scheduled messages
	NFOUT;
}

/* Can only be called if we know that we will not send a switch topo ack or unregister from a segment */
void tomaSimulator_unsetSwitchTopoWait(const int toma_index, int inst_id, const char* segUUID){
	struct tomaSimulator *liveToma = tomasNetwork[toma_index];
	struct tomaSimulator_socket *sock;
	spin_lock(&liveToma->lock);
	sock = findConnectionByInstIDSegUUID(liveToma, inst_id, segUUID);
	BUG_ON(!sock);
	// If this toma is not waiting for this segment the usage is wrong
	BUG_ON(!sock->wait_for_switch_topo_ack);
	sock->wait_for_switch_topo_ack = 0;
	spin_unlock(&liveToma->lock);

}

void tomaSimulator_verifyNoSwitchTopoWait(struct tomaSimulator *toma) {
	for (u32 i=0; i<ARRAY_SIZE(toma->sock); i++){
		if (toma->sock[i].key){
			BUG_ON(toma->sock[i].wait_for_switch_topo_ack);
		}
	}
}

void tomaNetwork_verifyNoSwitchTopoWait(void) {
	int	i;
	for (i=0; i<numTomasInNetwork; i++)
		tomaSimulator_verifyNoSwitchTopoWait(tomasNetwork[i]);
}

void tomaSimulator_waitSwitchTopoAck(const int toma_index, const char* segUUID){
	struct tomaSimulator *liveToma = tomasNetwork[toma_index];
	struct tomaSimulator_socket *sock;
	int timeout_useconds = 0, print_timer = 0, b_wait = 100;
	bool wait_active = true;
	if (toma_index < 0) {
		_ND(trace_toma_simu_tomaSimulator_waitSwitchTopoAck, "Toma @LIVETOMA_UNIQUEID: will not wait since switch topo was not sent for segment @SEG", liveToma->uniqueID, segUUID);
		return;
	}
	while (wait_active) {
		if (wait_active)
			_ND(trace_1_toma_simu_tomaSimulator_waitSwitchTopoAck, "Toma @LIVETOMA_UNIQUEID: found unacked switch topo for segment @SEG, still waiting", liveToma->uniqueID, segUUID);
		else {
			break;
		} // print waiting every 5 seconds of wait
		if (print_timer >= 5000000) {
			_Emerg("Toma %d: waiting for switch topo ACK for %d seconds\n", liveToma->uniqueID, timeout_useconds/1000000);
			spin_lock(&liveToma->lock);
			for (u32 i=0;i < ARRAY_SIZE(liveToma->sock);i++){
				if (liveToma->sock[i].key && liveToma->sock[i].wait_for_switch_topo_ack){
					_Emerg("Toma %d: found wait_for_switch_topo_ack for segment index %d, %s\n", liveToma->uniqueID, i, liveToma->sock[i].seg_UUID);
				}
			}
			spin_unlock(&liveToma->lock);
			print_timer = 0;
		} // 11 seconds timeout
		BUG_ON(timeout_useconds > 11000000);
		udelay(b_wait);
		timeout_useconds += b_wait;
		print_timer += b_wait;
		schedule();
		spin_lock(&liveToma->lock);
		wait_active = false;
		for_each_socket_by_uuid(liveToma, segUUID, sock)
			wait_active |= sock->wait_for_switch_topo_ack;
		spin_unlock(&liveToma->lock);
	}
	_ND(trace_2_toma_simu_tomaSimulator_waitSwitchTopoAck, "Toma @LIVETOMA_UNIQUEID: segment @SEG switch topo ack recieved within @MILISECONDS", liveToma->uniqueID, segUUID, timeout_useconds/1000);
}


void tomaSimulator_protoBugs(bool permitClient, bool permitUnitest){
	int i;
	for (i=0; i<numTomasInNetwork; i++) {
		tomasNetwork[i]->f_protoBugsSilent		= permitClient;
		tomasNetwork[i]->f_unitestBugsSilent 	= permitUnitest;
	}
}

void tomaSimulator_permitUnregAcks(bool permit){
	int i;
	for (i=0; i<numTomasInNetwork; i++)
		tomasNetwork[i]->f_forbidSendingUnregisterAck = !permit;
}

// Force expectation for client to send a failure command message to toma simulator
void __tomaSimulator_expectIOFailure(struct tomaSimulator* _this, short error_code,
								   u32 fix_mask, u32 fail_mask) {
	BUG_ON(_this->expect_io_failure);
	BUG_ON(_this->fail_mask);
	BUG_ON(_this->fix_mask);
	BUG_ON(((u32)(~fix_mask & fail_mask) != fail_mask) && ((fix_mask & fail_mask) != fail_mask));
	_this->expect_io_failure = error_code;
	_this->fail_mask = fail_mask;
	_this->fix_mask  = fix_mask;
}

void tomaSimulator_expectIOFailure(struct tomaSimulator* _this, short error_code,
								   const u32 failed, const u32 fixed) {
	if (!error_code) { // No real failure just generics
		__tomaSimulator_expectIOFailure(_this, error_code, 0, 0);
	} else { // Real failure
		__tomaSimulator_expectIOFailure(_this, error_code, fixed, failed);
	}
}

void tomaSimulator_verifyIOFailure(void){
	int i;
	for (i=0; i<numTomasInNetwork; i++)
		BUG_ON(tomasNetwork[i]->expect_io_failure);
}

void tomaSimulator_clean_rejected_lids(struct tomaSimulator	*T) {
	BUG_ON(T->lid_rej.n_rejects == 0);
	T->lid_rej.n_rejects = 0;
	T->lid_rej.lid.all = 0;
}

/* Toma initiates message to client (not replying to message).
   Can insert artificial lock-id to mess up with the client a bit :-) */
#define tomaSimulator_sendMsg(t,vol_ver,res_ver,r,l,m) __tomaSimulator_sendMsg(t, vol_ver, res_ver, r, l, m, SIMULATOR_USE_CURRENT_LOCK_ID)
static int __tomaSimulator_sendMsg(struct tomaSimulator*_this, int vol_version, const u64 res_ver, const struct tTopoOfPraid* r1, const int liveSegInd, enum NVMEIBT_CLIENT_MSG_TYPES msg_type, u32 lock_id){
	const struct tomaSimulator_socket *sock;
	const int 	 msg_size 	  = sizeof(struct nvmeibt_client_msg) + tTopoOfPraid_get_size(r1);
	struct nvmeibt_client_msg *clnt_msg;
	bool need_to_relock = false;

	BUG_ON(!spin_is_locked(&_this->lock));
	for_each_socket_by_uuid(_this, r1->s[liveSegInd].uuid, sock) {
		clnt_msg = sim_kzalloc(sizeof(struct nvmeibt_client_msg)+sizeof(*r1), GFP_KERNEL); // Buffer of Switch topo or unregister message with place for N segments. Might need only one
		if (need_to_relock) spin_lock(&_this->lock);
		clnt_msg->hdr.msg_type 			= msg_type;
		clnt_msg->thick.lock_id			= (lock_id == SIMULATOR_USE_CURRENT_LOCK_ID) ? sock->lid.all : lock_id;       // Can use artificial lock-id to use in this message (possibly to mess up with the client a bit)
		clnt_msg->thick.reservation_mode_version = res_ver;
		__fill_topo_of_raid_payload(_this, clnt_msg, r1, vol_version);
		_NT(trace_1_toma_simu_tomaSimulator_sendMsg, "Toma @TOMA_UNIQUEID: sending msg (@BUF) to client @PROTOCOL_CLIENT_MSG_STR: ", _this->uniqueID, clnt_msg, nvmeibt_protocol_client_msg_str(clnt_msg->hdr.msg_type));
		clnt_msg->thick.conversation_ind = (u64)0;				// Does not exist, Initiated by Toma, not a reply to clients conversation
		__sendReplyToclient_unlock(_this, &sock->con, clnt_msg, msg_size);
		need_to_relock = true;
	}
	return 1;
}

#define is_type_mandatory(type) ((type == NVMEIBT_RECOVERY_TYPE_EC_COLD) || (type == NVMEIBT_RECOVERY_TYPE_DIRTY_REBUILD) || ((seg->slice_size > 1) && (type == NVMEIBT_RECOVERY_TYPE_STALE_REBUILD)))// || (type == NVMEIBT_RECOVERY_TYPE_STALE_REBUILD))
#define is_ec_non_jour_rec(seg, type) ((seg->slice_size > 1) && (type != NVMEIBT_RECOVERY_TYPE_EC_COLD) && (type != NVMEIBT_RECOVERY_TYPE_EC_JOUR_GC))

int tomaSimulator_JGC_launch(const char *seg_uuid) {
	if (tomaLeader) {
		struct ramDiskSimulator *ram = serverSimulator_get_ram_by_toma(tomaLeader);
		if (ram->use_ec_stale_locks) { // Do only if we are in EC
			int si;
			const struct tTopoOfVolume *tv;
			const struct tTopoOfPraid *pr = tTopoOfNVMesh_find_r1_by_seg_uuid(tomaLeader->globalTopo, seg_uuid, &tv, &si);
			const struct disk_range *seg;
			if (!pr) {/* It is legal corner case - serjio calls launch jgc on deleted segment, matter of removal timing */
				_NE_dmesg(error_1_toma_simu_tomaSimulator_JGC_launch, "JGC on unknown segment");
				return -ENOENT;
			}
			seg = &tv->segs[si];
			if (pr)
				return tomaSimulator_recoverThing(pr, seg, RCVR_EC_JOUR_GC_ASYNC);
			else
				BUG();
		}
	} else {
		_NE_dmesg(error_toma_simu_tomaSimulator_JGC_launch, "JGC launch request without leader");
	}
	return -EINVAL;
}

static inline int __recov_cmd_get_payload_size(enum NVMEIBT_CLIENT_MSG_TYPES cmd) {
	switch (cmd) {
		case NVMEIBT_CLIENT_MSG_TR_RECOVER_ABORT:
		case NVMEIBT_CLIENT_MSG_TR_RECOVER_PING:       return sizeof(struct nvmeibt_client_recovery_taskid_pl);
		case NVMEIBT_CLIENT_MSG_TC_LOCKID_CACHE_PURGE: return sizeof(struct nvmeibt_lockid_cache_purge_pl);
		case NVMEIBT_CLIENT_MSG_TR_RECOVER_START:      return sizeof(struct nvmeibt_client_recovery_start_pl);
		default: BUG();
	}
	return -1;
}

int tomaSimulator_recoverThingStatus(const struct tTopoOfPraid *r1, const struct disk_range *seg, const struct toma_recovery_args sim_args, int *recov_status) {
	struct nvmeibt_client_msg *header;
	struct tomaSimulator_socket *sock;
	int msg_size;
	const char *seg_uuid = seg->ruuid;							// can use: r1->s[(seg->stripe_index % seg->replicas)].uuid;
	struct tomaSimulator *toma = tomasNetwork[seg->node_id];
	const struct tTopoOfVolume *tv	 		= NULL;
	struct nvmeibt_client_msg_pl *rpl;
	const enum NVMEIBT_RECOVERY_TYPE valid_rtype = nvmeibt_recovery_type_cast(sim_args.type); // our tests checks invalid input too; toma simulator will propogate the invalid arguments to the client, but internally will manage the conversation state on correct type (invalid);
	const bool do_start_recov = ((sim_args.cmd == NVMEIBT_CLIENT_MSG_TR_RECOVER_START)||(sim_args.cmd == NVMEIBT_CLIENT_MSG_TC_LOCKID_CACHE_PURGE));
	struct sim_toma_recoveries *rr = NULL;

	spin_lock(&toma->lock);
	/* Yuri: @TODO - get rid of this crap */
	sock = (void*)findConnectionBySegUUID(toma, seg_uuid);
	if (!sock) {											// Client unsubscribed from Toma?
		spin_unlock(&toma->lock);
		return -1;
	}
	if (valid_rtype == NVMEIBT_RECOVERY_TYPE_EC_JOUR_GC){
		const u64 cold_task_id = sock->rcvrs[NVMEIBT_RECOVERY_TYPE_EC_COLD   ].task.recov_start.task.id;
		const u64 jgc_task_id =  sock->rcvrs[NVMEIBT_RECOVERY_TYPE_EC_JOUR_GC].task.recov_start.task.id;
		if ((cold_task_id)||(jgc_task_id)) { //not enough; cold/jgc recovery may be still executed by other toma
			_NT(t_a1_tsrt, "skipping JGC execution, since cold recovery is running; toma=@TOMA_UNIQUEID, cold_recovery=@TASK, jgc_recovery=@TASK", toma->uniqueID, cold_task_id, jgc_task_id);
			spin_unlock(&toma->lock);
			return 0;
		}
		if (toma->ignore_jour_gc_launch_request && sim_args.recov_caller == UNI_RECOV_CALLER_SERJIO) {
			//not enough; cold recovery may be still executed by other toma
			_NT(t_a2_tsrt, "skipping JGC execution, since ignore_jour_gc_launch_request==true; toma=@TOMA_UNIQUEID, jgc_recovery=@TASK", toma->uniqueID, jgc_task_id);
			spin_unlock(&toma->lock);
			return 0;
		}
	}
	rr = &sock->rcvrs[valid_rtype];
	tv = tTopoOfNVMesh_find_vol_by_praid(toma->globalTopo, r1);
	header = __create_ack_message(toma, &msg_size, sim_args.cmd, tv->version, tv->reservation_version, __recov_cmd_get_payload_size(sim_args.cmd));
	header->thick.lock_id			= sock->lid.all;
	header->thick.praid_version 	= r1->header.praid_version;
	strcpy(header->thick.disk_segment_uuid, seg_uuid);

	rpl = (void*)&header->thick.data;
	pthread_mutex_lock(&toma->recov_msg_lock);
	if (do_start_recov) {		// initiate recovery information
		const int TOMA_SEG_UUID_LEN = sizeof(rpl->recov_start.praid_id);
		union __sim_toma_packed_task_id packed_task_id = {
			.toma = toma->uniqueID,
			.sgmnt_lockset_start = (seg->dlba_start / LOCKSET_4KS),
			.is_ec = !!(seg->slice_size > 1),
			.type = sim_args.type,
			.reserved = 0,
			.task_id = (++toma->recov_task_id),
		};

		rpl->recov_start.task.type      = sim_args.type;
		rpl->recov_start.task.id 	    = packed_task_id.all;
		// mandatory if recovering db or stale locks (for EC) - override allows ignoring mandatory recoveries to not retry them on failure
		rpl->recov_start.is_mandatory   = sim_args.ext_args.is_mandatory.override ? sim_args.ext_args.is_mandatory.value : is_type_mandatory(sim_args.type); 							//mandatory if recovering db or stale locks (for EC)
		strlcpy(rpl->recov_start.praid_id, r1->header.uuid, TOMA_SEG_UUID_LEN);
		if (sim_args.type == NVMEIBT_RECOVERY_TYPE_EC_COLD) {
			rpl->recov_start.cold.surviving_ram_bmp = (u32)sim_args.surviving_ram_bmp;
		} else { /* Here sim_kzalloc initialized the union to 0 */ }
		if (sim_args.ext_args.lock_range.override) {
			rpl->recov_start.start_lock = sim_args.ext_args.lock_range.start;
			rpl->recov_start.num_locks	 = sim_args.ext_args.lock_range.count;
		} else {
			rpl->recov_start.start_lock = 0 / LOCKSET_4KS;
			rpl->recov_start.num_locks	 = seg->length / LOCKSET_4KS;
		}
		rpl->recov_start.do_only_owners = is_ec_non_jour_rec(seg, sim_args.type); // Real Toma puts 'true' always. We prefer to put false coz launching many recoveires at once is difficult. Hot EC recoveries cannot operate on non owners so we must set owners

		_ND(t_a3_tsrt, "setup new wait rr=@RR completion=@COMPLETION task=@TASK is_sync=@IS_SYNC", rr, &rr->wait, rpl->recov_start.task.id, sim_args.on_start_wait_for_end);
		completion_verify_not_waiting(&rr->wait);
		BUG_ON(rr->task.recov_start.task.id);	// Recovery is already running. Illegal for Toma to launch 2 recoveries on the same praid, Fix your unitest!
		rr->task = *rpl;
		rr->wait.done = false;
		rr->is_waiting = sim_args.on_start_wait_for_end;
		rr->recov_caller = sim_args.recov_caller;
		atomic_inc(&toma->num_running_recoveries);
	} else {
		rpl->recov.task.id =   rr->task.recov_start.task.id;
		rpl->recov.task.type = sim_args.type;
	}
	{
		const u64 task_id = rr->task.recov_start.task.id;
		_ND(t_a4_tsrt, "sending message toma=@TOMA_UNIQUEID cmd=@CMD type=@TR_RECOV_TYPE", toma->uniqueID, sim_args.cmd, sim_args.type);
		__sendReplyToclient_unlock(toma, &sock->con, header, msg_size);
		pthread_mutex_unlock(&toma->recov_msg_lock);
		if (do_start_recov && sim_args.on_start_wait_for_end){
			_ND(t_a5_tsrt, "waiting for rr=@RR task=@TASK", rr, task_id);
			wait_for_completion(&rr->wait);
			_ND(t_a6_tsrt, "waiting for rr=@RR task=@TASK - done", rr, task_id);
			rr->is_waiting = false;
			*recov_status = rr->recov_status;
		}
	}
	return 0;
}

int tomaSimulator_recoverThing(const struct tTopoOfPraid *r1, const struct disk_range *seg, const struct toma_recovery_args sim_args) {
	int dummy_recov_status;
	return tomaSimulator_recoverThingStatus(r1, seg, sim_args, &dummy_recov_status);
}

	/*************** Switch-topology class and Toma messages **********************/
struct switch_topo_options	SW_TOPO__SILENT		= {.dont_send_msg = true,  .can_fail = false, .wait_for_ack=false, .wait_for_ack_drain = false, .use_seg_index=false, .dry_run=false};
struct switch_topo_options	SW_TOPO__NONE 		= {.dont_send_msg = false, .can_fail = false, .wait_for_ack=false, .wait_for_ack_drain = false, .use_seg_index=false, .dry_run=false};
struct switch_topo_options	SW_TOPO__WAIT_ACK 	= {.dont_send_msg = false, .can_fail = false, .wait_for_ack=true,  .wait_for_ack_drain = false, .use_seg_index=false, .dry_run=false};
struct switch_topo_options	SW_TOPO__WAIT_ACK_DR= {.dont_send_msg = false, .can_fail = false, .wait_for_ack=true,  .wait_for_ack_drain = true , .use_seg_index=false, .dry_run=false};

void tomaSimulator_sendUnregisterMsg(struct tTopoOfPraid* r1, const int segInd, const struct disk_range *segs, int inst_id){
	struct tomaSimulator *deadToma = tomasNetwork[segs[segInd].node_id], *liveToma = tomasNetwork[segs[segInd^1].node_id];
	struct tomaSimulator_socket *sock;
	u32 lock_id;
	spin_lock(&liveToma->lock);	// Get the lock-ID from the live Toma
	sock = findConnectionByInstIDSegUUID(liveToma, inst_id, r1->s[segInd^1].uuid);
	BUG_ON(!sock);
	lock_id = sock->lid.all;
	spin_unlock(&liveToma->lock);
	spin_lock(&deadToma->lock);
	__tomaSimulator_sendMsg(deadToma, -1 /* Todo: Vol version */, RESERVATION_MODE_IRRELEVANT /* Todo: res version*/, r1, segInd, NVMEIBT_CLIENT_MSG_TR_UNREGISTER_DISK_SEGMENT, lock_id);
}


int tomaSimulator_switchSegmentTopo( const struct TstPRaid* praid, enum NVMEIBTC_DS_MODE mode, const struct switch_topo_options opts, struct switch_topo_dest *dst){
	struct tTopoOfPraid* r1 = praid->tpr;
	enum NVMEIBTC_DS_MODE segments_mode[N_MAX_RAID_SLICE_LEN] = {[0 ... N_MAX_RAID_SLICE_LEN-1] = NVMEIBTC_DS_MODE_RW};
	for (s32 index = 0; index < r1->header.n_segments; ++index){
		if (index == praid->vsi.segment){
			segments_mode[index] = mode;
		} else {
			segments_mode[index] = r1->s[index].access_mode;
		}
	}
	return tomaSimulator_switchTopoEC(praid->tpr->header.uuid, segments_mode, opts, dst);
}

int tomaSimulator_switchSegmentTopo_find_courier( const struct TstPRaid* praid, const struct switch_topo_options opts, struct switch_topo_dest *dst){
	struct switch_topo_options opts2 = opts;
	opts2.dry_run = 1;
	return tomaSimulator_switchTopoEC(praid->tpr->header.uuid, NULL, opts2, dst);
}

/* For back compatibility */
int tomaSimulator_switchTopo(const char r1_uuid[40], enum NVMEIBTC_DS_MODE s0, enum NVMEIBTC_DS_MODE s1, const struct switch_topo_options opts){
	const enum NVMEIBTC_DS_MODE s[N_MAX_RAID_SLICE_LEN] = {s0, s1, [2 ... N_MAX_RAID_SLICE_LEN-1] = NVMEIBTC_DS_MODE_RW};
	return tomaSimulator_switchTopoEC(r1_uuid, s, opts, NULL);
}

// send the msg & wait for the ack (when requested)
static int tomaSimulator_sendSwitchTopoMsg(struct tomaSimulator *toma, int vol_version, u64 res_ver, int disk_ind, struct tTopoOfPraid *r1, int seg_ind,const struct switch_topo_options opts)
{
	struct tomaSimulator_socket *sock;
	int	rv;
	_ND(trace_toma_simu_tomaSimulator_sendSwitchTopoMsg, "Toma @TOMA_UNIQUEID: added wait for switch ack on seg @SEG (ind @SI), toma_seg_index @T_PRV", toma->uniqueID, r1->s[seg_ind].uuid, seg_ind, r1->header.praid_version);
	rv = tomaSimulator_sendMsg(toma, vol_version, res_ver, r1, seg_ind, NVMEIBT_CLIENT_MSG_TR_SWITCH_PRAID_TOPOLOGY);	// Request the alive toma to send update its clients with switch topology.
	if (!rv) {
		_ND(trace_1_toma_simu_tomaSimulator_sendSwitchTopoMsg, "Toma @TOMA_UNIQUEID: switch topo msg not sent for segment, raid1 version @T_PRV, removing client from wait for switch topo ack list", toma->uniqueID, r1->header.praid_version);
		spin_lock(&toma->lock);
		for_each_socket_by_uuid(toma, r1->s[seg_ind].uuid, sock)
			sock->wait_for_switch_topo_ack = false;
		spin_unlock(&toma->lock);
		return -1;
	}
	if (opts.wait_for_ack) {
		tomaSimulator_waitSwitchTopoAck(disk_ind, r1->s[seg_ind].uuid);     // Wait for the async switch to terminate (regardless of the expected wait)
		if (opts.wait_for_ack_drain)
			tomaSimulator_waitProtoEnd(NULL);
	}
	return 0; // success
}

/* Returns: disk index (which is also toma-index) bcz we have a single disk/toma per server < 0 when failed*/
int tomaSimulator_switchTopoEC(const char r1_uuid[40], const enum NVMEIBTC_DS_MODE s[N_MAX_RAID_SLICE_LEN], const struct switch_topo_options opts, struct switch_topo_dest *dst){
	const struct tTopoOfVolume *tv = NULL;
	struct tTopoOfPraid* r1  = (struct tTopoOfPraid*)toma_findTopologygByPraidUUID(tomaLeader, r1_uuid, &tv);
	struct tomaSimulator *liveToma = NULL;
	struct tomaSimulator_socket *sock;
	int err = 0;
	int liveSegInd[N_MAX_RAID_SLICE_LEN+1], deadSegInd[N_MAX_RAID_SLICE_LEN+1], i;
	int liveDskInd[N_MAX_RAID_SLICE_LEN+1], deadDskInd[N_MAX_RAID_SLICE_LEN+1];
	int	sentMsgs = 0;
	int	disk_ind, seg_ind;

	if (dst)
		memset(dst, 0, sizeof(*dst));
	err = pthread_mutex_lock(&tomaLeader->globalTopo->lock);
	BUG_ON(err);
	if (!opts.dry_run)
		tTopoOfPraid_incVer(r1);                                            // Monotonically increase Raid version
	tTopoOfPraid_getdisks_by_status(r1, liveSegInd, deadSegInd, liveDskInd, deadDskInd);	// Find the alive toma which can contact clients (leader is possibly not connected directly to client)
	if (s) {															// If NULL, don't update topo. Used for injecting specific topologies in unitest, like force-locks-on-read param.
		for (i = 0; i < r1->header.n_segments; i++)
			r1->s[i].access_mode = s[i];                                // Update the status of each segment
		tTopoOfPraid_update_segs_by_access_mode(r1, tv->locks_scheme);
	}
	pthread_mutex_unlock(&tomaLeader->globalTopo->lock);
	if (unlikely(opts.dont_send_msg == true)) {
		BUG_ON(dst);													// Cannot fill it, wrong testing scenario
		goto out;
	}
	if (unlikely(opts.use_seg_index)) {
		 BUG_ON(opts.seg_index >= r1->header.n_segments);
		 seg_ind = opts.seg_index;
		 for (disk_ind=-1, i=0;i<N_MAX_RAID_SLICE_LEN; i++) {	// search segment index in both arrays to get the disk index
			 if (liveSegInd[i] == seg_ind) {disk_ind = liveDskInd[i];	break;}
			 if (deadSegInd[i] == seg_ind) {disk_ind = deadDskInd[i];	break;}
		 }
		 BUG_ON(disk_ind == -1);
		 liveToma = tomasNetwork[disk_ind];
		 spin_lock(&liveToma->lock);
		 for_each_socket_by_uuid(liveToma, r1->s[seg_ind].uuid, sock) {
			BUG_ON(!sock);                                              // Trying to update an unsubscribed segment must find issue in unittest
			sentMsgs = 0;
			if ((!sock->lid.all)&&(opts.can_fail)) {
				_ND(trace_toma_simu_tomaSimulator_switchTopoEC, "Toma @LIVETOMA_UNIQUEID: segment @SEG forced to sw-topo but not registered. Bailing out with can_fail", liveToma->uniqueID, r1->s[seg_ind].uuid);
				spin_unlock(&liveToma->lock);
				goto out;
			}
			if (opts.wait_for_ack && !opts.dry_run)
				sock->wait_for_switch_topo_ack = true;
		 }
		 if (!opts.dry_run){
			if (tomaSimulator_sendSwitchTopoMsg(liveToma, tv->version, tv->reservation_version, disk_ind, r1, seg_ind, opts) == 0)
				sentMsgs++;
		} else {
			sentMsgs++;
			spin_unlock(&liveToma->lock);
		}
		 goto out;
	}
	for (sentMsgs=0, i=0; (sentMsgs < 1) && (liveDskInd[i]>=0); i++) {						// Only the first Live toma sends switch topo requests. Todo: Should all live toma's send ???
		disk_ind = liveDskInd[i];
		seg_ind = liveSegInd[i];
		liveToma = tomasNetwork[disk_ind];
		spin_lock(&liveToma->lock);
		if (liveToma->state != tomaState_running) {                      // Toma was deliberately put in tomaState_not_ready state by the unitest environment
			spin_unlock(&liveToma->lock);
			continue;
		}
		if (s && (s[seg_ind] == NVMEIBTC_DS_MODE_DEAD)) {
			spin_unlock(&liveToma->lock);
			continue;
		}
		/* First run - turn on all wait_for_switch_topo_ack */
		for_each_socket_by_uuid(liveToma, r1->s[seg_ind].uuid, sock) {
			if (!sock->lid.all) {
				continue;
			}
			if (!opts.dry_run){
				//PAY ATTENTION: opts.wait_for_ack is not used :-(
				sock->wait_for_switch_topo_ack = true;
			}
		}
		/* Second run - send messages */
		if (!opts.dry_run){
			if (tomaSimulator_sendSwitchTopoMsg(liveToma, tv->version, tv->reservation_version, disk_ind, r1, seg_ind, opts) < 0) {
				spin_unlock(&liveToma->lock);
				continue;
			}
		}
		sentMsgs++;
	}
	BUG_ON(i<1);
out:
	if (likely(sentMsgs)) {
		if (dst) {
			dst->r1 = r1;
			dst->disk_ind = disk_ind;
			dst->seg_ind  = seg_ind;
		}
		return disk_ind;
	} else {
		BUG_ON((!opts.can_fail) && opts.wait_for_ack); 		// we cannot ask to wait for SwitchTopoAck when nothing is sent.
	}
	return -ENOENT;	// no segment can be used to send msg
}

int tomaSimulator_switchTopo_dummy(int vold_ind, int r1_ind, const struct switch_topo_options opts, struct switch_topo_dest *dst){			// Find the alive toma which can contact clients (leader is possibly not connected directly to client)
	struct tTopoOfPraid* r1  = tTopoOfVolume_getRaid1(&tomaLeader->globalTopo->vols[vold_ind], r1_ind);
	return tomaSimulator_switchTopoEC(r1uuid(r1), NULL, opts, dst);
}

int tomaSimulator_unreg_raid1( const char r1_uuid[40], int ind_of_dead_seg){
	int err = 0;
	const struct tTopoOfVolume *tv = NULL;
	struct tTopoOfPraid* r1  = (struct tTopoOfPraid*)toma_findTopologygByPraidUUID(tomaLeader, r1_uuid, &tv);
	int liveSegInd[N_MAX_RAID_SLICE_LEN+1], deadSegInd[N_MAX_RAID_SLICE_LEN+1], i, rv = 0;
	int liveDskInd[N_MAX_RAID_SLICE_LEN+1], deadDskInd[N_MAX_RAID_SLICE_LEN+1];
	BUG_ON(r1->header.n_segments<2);									// Not applicable for non mirrored volumes
	err = pthread_mutex_lock(&tomaLeader->globalTopo->lock);
	BUG_ON(err);
	tTopoOfPraid_incVer(r1);											// Monotonically increase Raid version
	r1->s[ind_of_dead_seg  ].access_mode = NVMEIBTC_DS_MODE_DEAD;		// Update the status of each segment
	tTopoOfPraid_update_segs_by_access_mode(r1, tv->locks_scheme);
	tTopoOfPraid_getdisks_by_status(r1, liveSegInd, deadSegInd, liveDskInd, deadDskInd);	// Find the alive toma which can contact clients (leader is possibly not connected directly to client)
	pthread_mutex_unlock(&tomaLeader->globalTopo->lock);

	for (i=0; liveDskInd[i]>=0; i++) {									// All Live toma sends switch unregister requests.
		struct tomaSimulator *liveToma = tomasNetwork[liveDskInd[i]];
		struct tomaSimulator_socket *sock;
		spin_lock(&liveToma->lock);
		BUG_ON(liveToma->state != tomaState_running);
		for_each_socket_by_uuid(liveToma, r1->s[liveSegInd[i]].uuid, sock)
			__simToma_unregister_segment(liveToma, sock, SIMULATOR_USE_PREV_LOCK_ID, false);
		rv = tomaSimulator_sendMsg(liveToma, tv->version, tv->reservation_version, r1, liveSegInd[i], NVMEIBT_CLIENT_MSG_TR_UNREGISTER_DISK_SEGMENT); BUG_ON(!rv);	// Request the alive toma to send update its clients with switch topology.
	}
	tomaSimulator_waitProtoEnd(NULL);							// Wait for the async switch to terminate
	return rv;
}

int tomaSimulator_send_unreg_to_raid1_increase_reservation_version(const char r1_uuid[40]){
	struct tTopoOfVolume *tv = NULL;
	struct tTopoOfPraid* r1  = (struct tTopoOfPraid*)toma_findTopologygByPraidUUID(tomaLeader, r1_uuid, (const struct tTopoOfVolume **)&tv);
	int i, rv = 0;
	int err = 0;
	BUG_ON(r1->header.n_segments<2);									// Not applicable for non mirrored volumes
	err = pthread_mutex_lock(&tomaLeader->globalTopo->lock);
	BUG_ON(err);
	tTopoOfPraid_incVer(r1);											// With or without this addition?
	tv->reservation_version++;
	pthread_mutex_unlock(&tomaLeader->globalTopo->lock);
	for (i=0; i<r1->header.n_segments; i++) {									// All Live toma sends switch unregister requests.
		struct tomaSimulator *liveToma = tomasNetwork[i];
		struct tomaSimulator_socket *sock;
		spin_lock(&liveToma->lock);
		if (liveToma->state != tomaState_running){	BUG(); 				// What ???
			spin_unlock(&liveToma->lock);
			continue;
		}
		for_each_socket_by_uuid(liveToma, r1->s[i].uuid, sock)
			__simToma_unregister_segment(liveToma, sock, SIMULATOR_USE_PREV_LOCK_ID, true);
		rv = tomaSimulator_sendMsg(liveToma, tv->version, tv->reservation_version, r1, i, NVMEIBT_CLIENT_MSG_TR_UNREGISTER_DISK_SEGMENT); BUG_ON(!rv);	// Request the alive toma to send update its clients with switch topology.
	}
	tomaSimulator_waitProtoEnd(NULL);							// Wait for the async switch to terminate
	return rv;
}


int tomaSimulator_unreg_all(struct tomaSimulator*_this, int inst_id) {
	const int conn_table_size = ARRAY_SIZE(_this->sock);
	int n_unreg=0, i;
	spin_lock(&_this->lock);
	for (i=0; i < conn_table_size; i++) {
		struct tomaSimulator_socket *sock = &_this->sock[i];
		if (sock->inst_id == inst_id && __is_clients_registered_lock(sock->lid.all)) {
			__launch_unreg_to_stale_on_seg(_this , sock, sock->lid.all);
			sock->lid.all = LS_UNLOCKED;
			n_unreg++;
		}
	}
	spin_unlock(&_this->lock);
	_ND(trace_toma_simu_tomaSimulator_unreg_all, "Toma @TOMA_UNIQUEID: @N_UNREG segments removed @INST_ID", _this->uniqueID, n_unreg, inst_id);
	return n_unreg;
}

void tomaSimulator_send_registrables(struct tomaSimulator* _this) {
	u32 gen;
	spin_lock(&_this->lock);
__begin_cache_generation:
	gen = _this->socks_gen;
	spin_unlock(&_this->lock);

	for (u32 i = 0; i < ARRAY_SIZE(_this->sock); i++) { // If during this loop generation changes, restart the function.
		struct tomaSimulator_socket *sock = &_this->sock[i];
		spin_lock(&_this->lock);
		if (_this->socks_gen != gen) {	// We cannot trust nCons in loop, restart function
			_Emerg("Toma %d: Restarting REGISTRABLE: %d->%d \n", _this->uniqueID, gen, _this->socks_gen);
			goto __begin_cache_generation;
		}
		if (sock->key && sock->should_send_registrable) {
			const struct tTopoOfVolume *tv = NULL;
			int msg_size = -1;
			const struct tTopoOfPraid* latestTopo = toma_findTopologygBySegUUID(_this, sock->seg_UUID, &tv, NULL);
			const struct nvmeibc_disk_subscription_params *con = &sock->con;
			struct nvmeibt_client_msg* msg = __create_nack_or_registrable_message(_this, &msg_size, con, latestTopo, tv->version, tv->reservation_version, false);

			sock->should_send_registrable = false;
			_ND(trace_toma_simu_tomaSimulator_send_registrables, "Toma @TOMA_UNIQUEID: Sending REGISTRABLE: sock[@SOCK_IDX], uuid=@SEGMENT_UUID, @T_PRV", _this->uniqueID, i, sock->seg_UUID, latestTopo->header.praid_version);
			msg->thick.conversation_ind = (u64)(~0);		// Todo: Save the original registration request conversation index and put it here
			__sendReplyToclient_unlock(_this, con, msg, msg_size);
		} else {
			spin_unlock(&_this->lock);
		}
	}
}

/* Swaps all sockets between two tomas (locks must be taken before calling this function)
   Future requirement will be to swap only the related sockets that are drive dependant */
static void __toma_swap_connections(struct tomaSimulator *t1, struct tomaSimulator *t2) {
	struct tomaSimulator t3 = *t1;
	BUG_ON(atomic_read(&t1->num_running_recoveries));
	BUG_ON(atomic_read(&t2->num_running_recoveries));
	memcpy(t1->sock, t2->sock, sizeof(t3.sock));
	memcpy(t2->sock, t3.sock , sizeof(t3.sock));
	for (u32 i = 0; i < ARRAY_SIZE(t1->sock); ++i){
		for (u32 j = 0; j < ARRAY_SIZE(t1->sock[i].rcvrs); ++j){
			reinit_completion(&t1->sock[i].rcvrs[j].wait);
			reinit_completion(&t2->sock[i].rcvrs[j].wait);
		}
	}
	_ND(trace_toma_simu_toma_swap_connections, "@T1_UNIQUEID <--> @T2_UNIQUEID", t1->uniqueID, t2->uniqueID);
}

void tomaNetwork_swap_2tomas(struct tomaSimulator *t1, struct tomaSimulator *t2)
{
	if (t1->uniqueID > t2->uniqueID) { spin_lock(&t2->lock); spin_lock(&t1->lock);}
	else {							   spin_lock(&t1->lock); spin_lock(&t2->lock);}
	__toma_swap_connections(t1, t2);
	if (t1->uniqueID > t2->uniqueID) { spin_unlock(&t2->lock); spin_unlock(&t1->lock);}
	else {							   spin_unlock(&t1->lock); spin_unlock(&t2->lock);}
}

/*****************************************************************************/
// EOF.
