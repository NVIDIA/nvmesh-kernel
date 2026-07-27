#include "nvmeibc_block.h" // Must be first for simulator
#include "block/nvmeibc_topology.h"

static inline bool __verify_on_active_msg_is_valid(enum NVMEIBT_CLIENT_MSG_TYPES t)
{	// See documentation in .h file
	return ((t == NVMEIBT_CLIENT_MSG_TR_SWITCH_PRAID_TOPOLOGY) ||
			(t == NVMEIBT_CLIENT_MSG_TR_REGISTER_DISK_SEGMENT_NACK) ||
			(t == NVMEIBT_CLIENT_MSG_TR_REGISTRABLE_DISK_SEGMENT));
}

void nvmeibc_seg_on_active_init(struct nvmeibc_disk_segment *seg)
{
	seg->on_active.msg = NULL;
	seg->on_active.pr_version = 0;
	seg->on_active.tr = NULL;
}

#include "block/controlpath/nvmeibc_b_cp_topo_common.h"
#include "block/nvmeibc_block_common.h"
void nvmeibc_seg_on_active_free(struct nvmeibc_disk_segment *seg)
{
	kfree(seg->on_active.msg);	// Typically NULL
	if (seg->on_active.msg) {
		const struct nvmeibc_subscription_ctx *tr = seg->toma_reg;
		_NT(trace_1_on_active_free, "on active free @DEV_NAME" SEGMENT_FMT " msg={@T_PRV}",
		tr->nt->device_name, tr->ch, tr->r1, tr->seg, seg->on_active.pr_version);
	}
	nvmeibc_seg_on_active_init(seg);
}

int nvmeibc_seg_on_active_schedule(const struct nvmeibc_subscription_ctx *tr,
	const struct nvmeibt_client_msg *pl, int len, struct nvmeibc_disk_segment *seg)
{
	const enum NVMEIBT_CLIENT_MSG_TYPES msg_type = pl->hdr.msg_type;
	nvmeibc_seg_on_active_free(seg);					// Daniel: Todo, this is unsafe. Just deleting prev message
	seg->on_active.msg = kzalloc(len, GFP_ATOMIC);		//EC-1757, malloc
	if (!seg->on_active.msg) {
		_NT(trace_0_on_active_schedule, "No memory for a topology for device @DEV_NAME", tr->nt->device_name);
		return -ENOMEM;
	}
	WARN(!__verify_on_active_msg_is_valid(msg_type), "nvmeibc bug: illegal msg=%x\n", msg_type);
	memcpy(seg->on_active.msg, pl, len);
	seg->on_active.pr_version = nvmeibc_seg_on_active_get_version_calc(seg);
	seg->on_active.tr = tr;

	_NTTR(trace_1_on_active_schedule, "Delaying activation of new raid-topo; Topo @TOPO_DBG_ID, Keeping my lock-id, msg={@T_PRV, seg_uuid=@SEGMENT_UUID}",
	   __get_topo_of_seg(seg)->debug_unique_index,
	   pl->thick.praid_version, pl->thick.disk_segment_uuid);
	return 0;
}

void nvmeibc_seg_on_active_sched_ack(const struct nvmeibc_subscription_ctx *tr, struct nvmeibc_raid1 *pr, struct nvmeibc_disk_segment *seg, struct nvmeibc_topology *t_old)
{
	const enum NVMEIBT_CLIENT_MSG_TYPES msg_type = nvmeibc_seg_on_active_get_msg(seg)->hdr.msg_type;
/* There are 3 options: SWITCH_TOPO, NACK, REGISTRABLE:
   04/Feb/2016 protocol - ACK required only for SWITCH_TOPO.
   ACK should be sent back to only the Toma which SWITCHED_TOPO */
	enum NVMEIBT_CLIENT_MSG_TYPES ack = NVMEIBT_CLIENT_MSG_ILLEGAL;
	WARN(!seg->registration_status, "nvmeibc bug\n");		// Why then this msg is sent???
	if (msg_type == NVMEIBT_CLIENT_MSG_TR_SWITCH_PRAID_TOPOLOGY )
		ack = NVMEIBT_CLIENT_MSG_RT_SWITCH_PRAID_TOPOLOGY_ACK;
	if (ack != NVMEIBT_CLIENT_MSG_ILLEGAL) {
		const int req_si = seg->on_active.tr->seg;			// Toma which requested switch topo
		int si;
		//const int act_si = (seg->toma_reg->seg);			// Segment that was activated
		/* EC-4633: Currently we store only 1 'tr' in seg->on_active.tr, even though a few Tomas can request switch topo. A correct solution would be storing a bitmap of which Tomas requested and answer only to them. Quicker solution is jsut answer to all tomas */
		for (si = 0; si < pr->replicas; si++) {
			struct nvmeibc_disk_segment *dst_seg = &pr->segments[si];
			const enum NVMEIBT_CLIENT_TR_REASON reason = (si == req_si) ? NVMEIBT_CLIENT_RT_REASON_DELAYED_SW_TOPO_ACK : NVMEIBT_CLIENT_RT_REASON_DELAYED_SW_TOPO_UPD;
			//if (si == act_si)
			//	continue;									// Definitely does not need switch Topo Ack.
			_NTTR(t_01_onact_ack, "Scheduling ack msg @ACK (@PROTOCOL_CLIENT_MSG_STR) to seg @SEG", ack, nvmeibt_protocol_client_msg_str(ack), dst_seg->uuid);
			on_topo_free_schedule_seg_msg(dst_seg, t_old, ack, reason);
		}
	}
}

int nvmeibc_seg_on_active_get_version_send(struct nvmeibc_raid1 *pr, struct nvmeibc_disk_segment *seg, enum NVMEIBT_CLIENT_MSG_TYPES msg_type)
{
	int pr_version = pr->version;								// Default
	if (msg_type == NVMEIBT_CLIENT_MSG_RT_REGISTER_DISK_SEGMENT) {
		const int pr_on_act_ver = seg->on_active.pr_version; 	// Dont use!!!!: nvmeibc_seg_on_active_get_version_calc(seg); it might be already free by HEAD
		#if (0)													// Turn on to invoke reproduction of the access after free bug
			const int unsafe_ver = nvmeibc_seg_on_active_get_version_calc(seg);
			WARN((unsafe_ver != pr_on_act_ver), "nvmeibc bug, EC-1757 reproduced: oa_prv=0x%x free_prv=0x%x c_prv=0x%x\n", pr_on_act_ver, unsafe_ver, pr_version);
		#endif
		if (pr_on_act_ver > pr_version) {
			WARN((pr_on_act_ver > (pr_version+10))&&(pr_version!=0), "nvmeibc bug, Toma or EC-1757: oa_prv=0x%x c_prv=0x%x\n", pr_on_act_ver, pr_version);	// Daniel: +10 is arbitrary. Ronen said that in real system probably +3 is enough (at most double degradeness + switch topo). Daniel in simulator uses up to 5 versions ahead
			pr_version = pr_on_act_ver;
		}
		if ((pr_on_act_ver != 0)&&(pr_on_act_ver < pr_version)) {	// Possible: {RW,RW,D,D} Switch topo to {RW,RW,D,W}, but IO takes long time to complete so client does not free topology, and newer topology {RW,RW,W,W} arrives via registrable, before switch topo applied
			_NT(t_03_onact_ack, "reproduction of EC-6371 or EC-1757: @OA_PRV @C_PRV", pr_on_act_ver, pr_version);
		}
	}
	return pr_version;
}

