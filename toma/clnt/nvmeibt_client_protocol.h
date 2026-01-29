#ifndef NVMEIBT_CLIENT_PROTOCOL_H
#define NVMEIBT_CLIENT_PROTOCOL_H

#include "../common/nvmeib_shared.h"
#include "../common/compat/kr_incs_compiler_types.h"
/*
 * Introduction:
 *  The primary objective of the Client<-->TOMA protocol is to register
 *  a client on a disk_segment. I.e.,
 *   1. A well-behaved client will lock and write on a disk_segment only
 *  	after it successfully registered with the disk_segment
 *   2. As part of the registration, the client receives a lock_id. The client
 *  	writes this lock_id in the locks table when locking a blkset.
 *  	Note: The client should use the same lock_id for registration on both
 *  	the praid's segments. This lock_id must be taken from one of the
 *  	REGISTER_NACK/REGISTRABLE/REGISTER_ACK responses that TOMA returned
 *
 *  Legend:
 *   RT === Registrant-->Toma
 *   TR === Toma-->Registrant
 *
 *  A typical normal bootstrap use case:
 *   RT_REGISTER_DISK_SEGMENT (praid_version==0, lock_id==0)
 *   TR_REGISTER_DISK_SEGMENT_NACK (due to praid_version mismatch and/or lock_id=0).
 *   - The response includes a lock_id. The client should use the lock_id
 *     that is received from the first NACK in all of its future
 *     RT_REGISTER_DISK_SEGMENT messages, and locks
 *   - Attached is the latest praid's topology)
 *     - Both mirrors are R/W
 *     - Every mirror is self owned
 *   RT_REGISTER_DISK_SEGMENT (praid_version==<the latest>, praid's lock_id)
 *   TR_REGISTER_DISK_SEGMENT_ACK
 *  A typical disk down use case:
 *   TR_UNREGISTER_DISK_SEGMENT (on the disk_segment that is still alive)
 *   RT_UNREGISTER_DISK_SEGMENT (on all the active mirrors)
 *   (Toma waits for all registrants to unregister)
 *   TR_REGISTRABLE_DISK_SEGMENT (Attached is the latest praid's topology)
 *   - includes a new_lock_id that can be used for this praid
 *   - One mirror is R/W, the other one is DEAD
 *   - Owner for both mirrors: the live one
 *   RT_REGISTER_DISK_SEGMENT (praid_version==<the latest>, the praid's new_lock_id)
 *   TR_REGISTER_DISK_SEGMENT_ACK
 *  A typical disk back from DEAD:
 *   TR_SWITCH_PRAID_TOPOLOGY (Attached is the latest praid's topology)
 *   - The old mirror is R/W, the back_from_DEAD is W/O
 *   - Owner for both mirrors: the old-mirror
 *   RT_SWITCH_PRAID_TOPOLOGY_ACK (when the client is done using the prev topo)
 *   (TOMA recovers the disk_segment)
 *   TR_SWITCH_PRAID_TOPOLOGY (Attached is the latest praid's topology)
 *   - The old mirror is R/W, the back_from_DEAD is W/O
 *   - Owner for both mirrors: the old-mirror
 *   - Secondary owner for the recovered segment: the recovered segment
 *     The client should lock the recovered_seg's blksets on both owners (ordered)
 *   RT_SWITCH_PRAID_TOPOLOGY_ACK (when the client is done using the prev topo)
 *   (TOMA received ACK from all registrants)
 *   TR_SWITCH_PRAID_TOPOLOGY (Attached is the latest praid's topology)
 *   - Back to normal, switch back to self-owned
 *   RT_SWITCH_PRAID_TOPOLOGY_ACK (when the client is done using the prev topo)
 *
 * Protocol rules:
 * 1.  A client can use a praid_topology only if all of the praid's
 *     disk_segments, that are active in the current topology, are
 *     registered (using the same lock_id).
 * 2.  The client registers with every disk_segment's TOMA separately.
 * 3.  A topology contains the topology of a praid + its two disk_segments
 * 4.  A topology is attached to register_nack, switch_topo, and registrable.
 * 5.  If the client receives a topology with higher praid_version, then
 *     the client applies all of it, (also on the mirror of the registered
 *     segment)
 * 6.  A registration remains valid also when the praid_version changes.
 *     There is no need to re-register as long as the client does not
 *     unregister
 *     In the latest version, toma on both segments will require a
 *     SWITCH_TOPO_ACK.
 * 7.  When a client unregisters from a disk segment, it must unregister
 *     from all of the mirrored segments
 * 8.  When a client sends an unregister or switch_topo_ack msg to TOMA,
 *     it will no longer use the old topology
 * 9.  When a client receives a CONT (I.e., a remote disk is connected), it
 *     needs to register on all of the disk's disk_segments that reside on
 *     this disk (and belong to attached volumes)
 * 10. (deleted)
 * 11. If the client sends a register req to an already reagistered
 *     disk_segment using the same lock_id then toma ignores it, and returns
 *     the existing lock_id. If the lock_id is new, then TOMA unregisters the
 *     old one (client, beware not to use it) and registers from scratch.
 * 12. The client generates a client_uuid upon startup
 * 13. If TOMA is aware of the client's wish to be registered, and TOMA cannot
 *     currently let the client register, then TOMA will send registrable once
 *     it is ready to accept the registration
 *     Usually, TOMA sends registrable if it peviously sent either
 *     TR_TOMA_NOT_READY or TR_UNREGISTER_DISK_SEGMENT
 * 14. If a client is brutally cut (PAUSE) (IB wise) from a disk, then both the
 *     disk's TOMA and the client assume that the client unregistered from the
 *     disk's disk_segment, and run the "internal" unregister procedure.
 * 15. Note that the topology and available disks are calculated by TOMA based
 *     on the connections between TOMA nodes, which might be different from
 *     the client's connectivity
 * 16. -Removed
 * 17. If the topology contains a primary and a secondary owner then the client
 *     needs to lock them in this order
 * 18. When writing, the client needs to set/clear/ignore the dirty-bits
 *     according to the topology (with 2 way mirror, dirty-bits should be updated
 *     only on the owner-node of the blkset)
 *
 * Protocol for interaction of TOMA with client-recoverer is as follows:
 *
 *  NVMEIBT_CLIENT_MSG_RT_RECOVER_ANNOUNCE
 *    Client announced to TOMA that client is ready to do recovery/rebuild.
 *  NVMEIBT_CLIENT_MSG_TR_RECOVER_START
 *    TOMA tells client to start a recovery task, giving a task-id and info
 *    about the volume and range of locks to process.
 *  NVMEIBT_CLIENT_MSG_TR_RECOVER_ABORT
 *    TOMA tells client to abort a recovery task, giving the task-id of the
 *    affected task.
 *  NVMEIBT_CLIENT_MSG_TR_RECOVER_PING
 *    TOMA asks client to report the status of a task in progress, giving
 *    the task-id of the task.
 *  NVMEIBT_CLIENT_MSG_RT_RECOVER_PROGRESS
 *    Client reports to TOMA about the status of the task. This message is
 *    sent in response to messages START, PING.
 *  NVMEIBT_CLIENT_MSG_RT_RECOVER_FINISH
 *    Client reports to TOMA about a task that ended, either because it has
 *    finished or becaused it was aborted.
 *
 */

#define NVMEIBT_CLIENT_PROTO_VERSION_2_8_0 (0x00020800)	// n_segments is int8_t instead of int
#define NVMEIBT_CLIENT_PROTO_VERSION_202 (0x000B00B6)	// V2.0.2 - Same as previous but with correct reservation version
#define NVMEIBT_CLIENT_PROTO_VERSION NVMEIBT_CLIENT_PROTO_VERSION_2_8_0
#define NVMEIBT_CLIENT_PROTO_VERSION_PREV NVMEIBT_CLIENT_PROTO_VERSION_202
#define N_MAX_RAID_LOCKS          (4)					// Support up to 4-mirror or 3 parities erasure coding

/*************     Generic MSG (header)     ***************/

enum RECOVERY_ATTACH_CMD {
	RECOVERY_ATTACH_CMD_ATTACH	= 1,
	RECOVERY_ATTACH_CMD_DETACH	= 2,
};

enum NVMEIBT_IB_PROTOCOL_SIGNATURE {
	NVMEIBT_IB_PROTOCOL_SIGNATURE_RAFT = (0xb4a1U << 16),
	NVMEIBT_IB_PROTOCOL_SIGNATURE_TOMA = (0xb4a2U << 16),
	NVMEIBT_IB_PROTOCOL_SIGNATURE_TOMA_RECOVERY = (0xb4a3U << 16),
	//out of service
	NVMEIBT_IB_PROTOCOL_SIGNATURE_TOMA_RECOVERY_LOCK = (0xb4a4U << 16),
	NVMEIBT_PROTOCOL_SIGNATURE_CLIENT = (0xb4a5U << 16),
	NVMEIBT_PROTOCOL_SIGNATURE_LOCAL_SERVER = (0xb4a6U << 16),
	NVMEIBT_PROTOCOL_SIGNATURE_REGISTER_TR = (0xb4a7U << 16),
	NVMEIBT_PROTOCOL_SIGNATURE_REGISTER_RT = (0xb4a8U << 16),
	NVMEIBT_PROTOCOL_SIGNATURE_TOMA_REBUILD	= (0xb4aaU << 16),

	NVMEIBT_PROTOCOL_SIGNATURE_RAFT_FRAME_ACK = (0xb4b0U << 16),
	NVMEIBT_PROTOCOL_SIGNATURE_RAFT_FINAL_ACK = (0xb4b1U << 16),
};

static inline bool
is_raft_frame_ack(enum NVMEIBT_IB_PROTOCOL_SIGNATURE type)
{
	return
		type == NVMEIBT_PROTOCOL_SIGNATURE_RAFT_FRAME_ACK ||
		type == NVMEIBT_PROTOCOL_SIGNATURE_RAFT_FINAL_ACK;
}

const char *nvmeibt_ib_protocol_signature_to_str(enum NVMEIBT_IB_PROTOCOL_SIGNATURE signature);

enum NVMEIBT_CLIENT_TR_REASON { // [1..0xFF], TR - measn Toma to client, RT - means client to Toma
	// 4-bits prefix reason, 4-bits sub reason
	// Toma->Client Prefixes types, can be used only in combination with other reason
	NVMEIBT_CLIENT_TR_REASON_PREFIX_FATAL				= (0xF << 4), // TOMA_NOT_READY or UNREGISTER or other messages, Unrecoverable issues
	NVMEIBT_CLIENT_TR_REASON_PREFIX_CONFIG				= (0xC << 4), // TOMA_NOT_READY, Config related issues
	NVMEIBT_CLIENT_TR_REASON_PREFIX_TOPO				= (0xB << 4), // TOMA_NOT_READY, Topology related issues
	NVMEIBT_CLIENT_TR_REASON_PREFIX_MAINTANANCE			= (0xE << 4), // TOMA_NOT_READY, Maintanance operation is going on
	NVMEIBT_CLIENT_TR_REASON_PREFIX_LOCK				= (0xA << 4), // TOMA_NOT_READY, Lock related issues. 0 in V1.3.3
	NVMEIBT_CLIENT_TR_REASON_PREFIX_UNREG_TOPO			= (0xD << 4), // UNREGISTER, Topology change related issues

	// Client->Toma Prefixes types, can be used only in combination with other reason
	NVMEIBT_CLIENT_RT_REASON_GENERIC                    = (0x1 << 4),
	NVMEIBT_CLIENT_RT_REASON_UPDATE_TOPO                = (0x2 << 4),

	/* Fatal reasons: Toma not ready and never will be for this client  */
	NVMEIBT_CLIENT_TR_REASON_UNUSED						= 0,		// Illegal value, never sent
	NVMEIBT_CLIENT_TR_REASON_DELETING_SEG				= 0x1 | NVMEIBT_CLIENT_TR_REASON_PREFIX_FATAL,
	NVMEIBT_CLIENT_TR_REASON_SHUTDOWN					= 0x2 | NVMEIBT_CLIENT_TR_REASON_PREFIX_FATAL,
	NVMEIBT_CLIENT_TR_REASON_NON_LOCAL_DISK				= 0x3 | NVMEIBT_CLIENT_TR_REASON_PREFIX_FATAL,
	NVMEIBT_CLIENT_TR_REASON_PROTO_VERSION_MISMATCH		= 0x4 | NVMEIBT_CLIENT_TR_REASON_PREFIX_FATAL,
	NVMEIBT_CLIENT_TR_REASON_NO_DISK_IN_CONFIG			= 0x5 | NVMEIBT_CLIENT_TR_REASON_PREFIX_FATAL,

	/* Generic msg without reason, like recovery notifications*/
	NVMEIBT_CLIENT_TR_REASON_NONE						= 0x1,

	/* Non Fatal reasons for declining clients registration request: Toma can overcome it and send registrable to client, in future */
	NVMEIBT_CLIENT_TR_REASON_LOCKID_ALREADY_TAKEN		= 0x4 | NVMEIBT_CLIENT_TR_REASON_PREFIX_LOCK,	// Happens in lock-id wrap-around, Toma suggested this lock-id but will not let register with it
	NVMEIBT_CLIENT_TR_REASON_LOCKID_MESS				= 0x8 | NVMEIBT_CLIENT_TR_REASON_PREFIX_LOCK,
	NVMEIBT_CLIENT_TR_REASON_LOCKID_CLNT_COMPLAIN		= 0x5 | NVMEIBT_CLIENT_TR_REASON_PREFIX_LOCK,
	NVMEIBT_CLIENT_TR_REASON_VOL_VERSION_BEHIND			= 0x1 | NVMEIBT_CLIENT_TR_REASON_PREFIX_CONFIG,
	NVMEIBT_CLIENT_TR_REASON_VOL_VERSION_AHEAD			= 0x2 | NVMEIBT_CLIENT_TR_REASON_PREFIX_CONFIG,
	NVMEIBT_CLIENT_TR_REASON_CONF_CORRUPTED				= 0x8 | NVMEIBT_CLIENT_TR_REASON_PREFIX_CONFIG,
	NVMEIBT_CLIENT_TR_REASON_CONFIG_VERSION_BEHIND		= 0x9 | NVMEIBT_CLIENT_TR_REASON_PREFIX_CONFIG,
	NVMEIBT_CLIENT_TR_REASON_CONFIG_VERSION_AHEAD		= 0xA | NVMEIBT_CLIENT_TR_REASON_PREFIX_CONFIG,
	NVMEIBT_CLIENT_TR_REASON_INVALID_SEG_ID				= 0xB | NVMEIBT_CLIENT_TR_REASON_PREFIX_CONFIG,
	NVMEIBT_CLIENT_TR_REASON_PRAID_VERSION_BEHIND		= 0x1 | NVMEIBT_CLIENT_TR_REASON_PREFIX_TOPO,
	NVMEIBT_CLIENT_TR_REASON_PRAID_VERSION_AHEAD		= 0x2 | NVMEIBT_CLIENT_TR_REASON_PREFIX_TOPO,
	NVMEIBT_CLIENT_TR_REASON_SEG_STATE_NOT_REGISTRABLE	= 0x3 | NVMEIBT_CLIENT_TR_REASON_PREFIX_TOPO,
	NVMEIBT_CLIENT_TR_REASON_PRAID_NOT_ACTIVATED		= 0x4 | NVMEIBT_CLIENT_TR_REASON_PREFIX_TOPO,
	NVMEIBT_CLIENT_TR_REASON_PRAID_NEVER_ACTIVATED		= 0x5 | NVMEIBT_CLIENT_TR_REASON_PREFIX_TOPO,
	NVMEIBT_CLIENT_TR_REASON_SEG_STATE_INITIALIZING		= 0x6 | NVMEIBT_CLIENT_TR_REASON_PREFIX_TOPO,
	NVMEIBT_CLIENT_TR_REASON_SEG_STATE_MD_BROKEN		= 0x7 | NVMEIBT_CLIENT_TR_REASON_PREFIX_MAINTANANCE,
	NVMEIBT_CLIENT_TR_REASON_SEG_STATE_MD_NOT_STORED	= 0x8 | NVMEIBT_CLIENT_TR_REASON_PREFIX_MAINTANANCE,
	NVMEIBT_CLIENT_TR_REASON_SEG_STATE_MD_STORING		= 0x9 | NVMEIBT_CLIENT_TR_REASON_PREFIX_MAINTANANCE,
	NVMEIBT_CLIENT_TR_REASON_BLKS_ZEROING				= 0x3 | NVMEIBT_CLIENT_TR_REASON_PREFIX_MAINTANANCE,
	NVMEIBT_CLIENT_TR_REASON_WAIT_4_SERJIO				= 0x4 | NVMEIBT_CLIENT_TR_REASON_PREFIX_MAINTANANCE,
	NVMEIBT_CLIENT_TR_REASON_UPDATING_RM_VERSION		= 0x5 | NVMEIBT_CLIENT_TR_REASON_PREFIX_MAINTANANCE,
	NVMEIBT_CLIENT_TR_REASON_UNREGISTER_IN_PROGRESS		= 0x2 | NVMEIBT_CLIENT_TR_REASON_PREFIX_MAINTANANCE,

	/* Non Fatal reasons for Tomas request (typically unregister) */
	NVMEIBT_CLIENT_TR_REASON_INIT						= 0x1 | NVMEIBT_CLIENT_TR_REASON_PREFIX_UNREG_TOPO,
	NVMEIBT_CLIENT_TR_REASON_SW_TOPO_W					= 0x2 | NVMEIBT_CLIENT_TR_REASON_PREFIX_UNREG_TOPO,
	NVMEIBT_CLIENT_TR_REASON_SW_TOPO_D					= 0x3 | NVMEIBT_CLIENT_TR_REASON_PREFIX_UNREG_TOPO,
	NVMEIBT_CLIENT_TR_REASON_SW_TOPO_STABLE_UNSAFE		= 0x4 | NVMEIBT_CLIENT_TR_REASON_PREFIX_UNREG_TOPO,
	NVMEIBT_CLIENT_TR_REASON_SW_TOPO_STABLE_SAFE		= 0x5 | NVMEIBT_CLIENT_TR_REASON_PREFIX_UNREG_TOPO,
	NVMEIBT_CLIENT_TR_REASON_SW_TOPO_X					= 0x6 | NVMEIBT_CLIENT_TR_REASON_PREFIX_UNREG_TOPO,
	NVMEIBT_CLIENT_TR_REASON_CUT_REQ					= 0x7 | NVMEIBT_CLIENT_TR_REASON_PREFIX_UNREG_TOPO,
	NVMEIBT_CLIENT_TR_REASON_TBD						= 0xA | NVMEIBT_CLIENT_TR_REASON_PREFIX_UNREG_TOPO,
	NVMEIBT_CLIENT_TR_REASON_AWAITING_CLIENTS_SYNC		= 0xB | NVMEIBT_CLIENT_TR_REASON_PREFIX_UNREG_TOPO,
	NVMEIBT_CLIENT_TR_REASON_INTERNAL_ERR				= 0xC | NVMEIBT_CLIENT_TR_REASON_PREFIX_UNREG_TOPO,
	NVMEIBT_CLIENT_TR_REASON_UPDATING_RESERVATION_MODE_VERSION	= 0xD | NVMEIBT_CLIENT_TR_REASON_PREFIX_UNREG_TOPO,

	/* Reason why Client sends message to Toma */
	NVMEIBT_CLIENT_RT_REASON_DIRECT						= 0x1 | NVMEIBT_CLIENT_RT_REASON_GENERIC,
	NVMEIBT_CLIENT_RT_REASON_INSTRUCTED_UNREG			= 0x3 | NVMEIBT_CLIENT_RT_REASON_GENERIC,
	NVMEIBT_CLIENT_RT_REASON_REREG_ON_IO_FAIL			= 0x4 | NVMEIBT_CLIENT_RT_REASON_GENERIC,
	NVMEIBT_CLIENT_RT_REASON_WARM_UNREGISTER			= 0x5 | NVMEIBT_CLIENT_RT_REASON_GENERIC,
	NVMEIBT_CLIENT_RT_REASON_WARM_REGISTER				= 0x6 | NVMEIBT_CLIENT_RT_REASON_GENERIC,
	NVMEIBT_CLIENT_RT_REASON_UNREG_DISK_PAUSE			= 0x7 | NVMEIBT_CLIENT_RT_REASON_GENERIC,
	NVMEIBT_CLIENT_RT_REASON_REG_DISK_CONT				= 0x8 | NVMEIBT_CLIENT_RT_REASON_GENERIC,
	NVMEIBT_CLIENT_RT_REASON_POISON_RAID				= 0x9 | NVMEIBT_CLIENT_RT_REASON_GENERIC,
	NVMEIBT_CLIENT_RT_REASON_REJECT_REG_ACK				= 0xA | NVMEIBT_CLIENT_RT_REASON_GENERIC,
	NVMEIBT_CLIENT_RT_REASON_UNREG_ALL_XXX				= 0xB | NVMEIBT_CLIENT_RT_REASON_GENERIC,	// Todo: Split those messages for granularity
	NVMEIBT_CLIENT_RT_REASON_REG_ALL_XXX				= 0xC | NVMEIBT_CLIENT_RT_REASON_GENERIC,
	NVMEIBT_CLIENT_RT_REASON_INSTRUCTED_DEAD			= 0x1 | NVMEIBT_CLIENT_RT_REASON_UPDATE_TOPO,	// Other toma said that this Toma is dead (even if this Toma is alive)
	NVMEIBT_CLIENT_RT_REASON_DELAYED_SW_TOPO_ACK		= 0x2 | NVMEIBT_CLIENT_RT_REASON_UPDATE_TOPO,	// Ack to Toma requesting switch topo (Mandatory message)
	NVMEIBT_CLIENT_RT_REASON_DELAYED_SW_TOPO_UPD		= 0x6 | NVMEIBT_CLIENT_RT_REASON_UPDATE_TOPO,	// Ack to Other Tomas, notifying that swtich topo ACK was sent to requester (Not a mandatory)
	NVMEIBT_CLIENT_RT_REASON_INLINE_SW_TOPO				= 0x3 | NVMEIBT_CLIENT_RT_REASON_UPDATE_TOPO,
	NVMEIBT_CLIENT_RT_REASON_REG_ON_PR_UPDATE			= 0x4 | NVMEIBT_CLIENT_RT_REASON_UPDATE_TOPO,	// Clnt Sends REG after NACK/Registrable
	NVMEIBT_CLIENT_RT_REASON_REG_ON_PR_UPDATE_SWTOPO	= 0x5 | NVMEIBT_CLIENT_RT_REASON_UPDATE_TOPO,	// Clnt Sends REG, serving also as SW_TOPO_ACK
};
const char *nvmeibt_protocol_client_msg_reason_str(enum NVMEIBT_CLIENT_TR_REASON reason);

static inline bool is_reason_recoverable_within_client_subscription(enum NVMEIBT_CLIENT_TR_REASON reason)
{
	if ((reason & NVMEIBT_CLIENT_TR_REASON_PREFIX_FATAL) == NVMEIBT_CLIENT_TR_REASON_PREFIX_FATAL)
		return false;

	if (reason == NVMEIBT_CLIENT_TR_REASON_UNUSED)
		return false;
	return true;
}

enum NVMEIBT_RECOVERY_TYPE { // Important: Recovery type does not define uniquely which type of fix will be executed, coz some recoveries fix more than a single problem
	NVMEIBT_RECOVERY_TYPE_INVALID			= 0,
	NVMEIBT_RECOVERY_TYPE_DIRTY_REBUILD		= 1,			// Mandatory: Rebuild of R1: Fix dbits + commit stale locks, EC: Turn dconvicts on + Fix dbits + Fix stale locks + commit blockset info
	NVMEIBT_RECOVERY_TYPE_STALE_REBUILD		= 2,			// Optional: R1 stale rebuild of entire range, EC stale rebuild of a few locks
	NVMEIBT_RECOVERY_TYPE_EC_COLD			= 3, 			// Mandatory: EC cold recovery.
	NVMEIBT_RECOVERY_TYPE_STALE_LOCKS_PURGE	= 4, 			// Mandatory: Not a remote recovery. Client locally purges its cache of stale locks
	NVMEIBT_RECOVERY_TYPE_EC_DCONVICT_TURNON= 5, 			// Mandatory: EC: Turn on dirty convict on entire segment
	NVMEIBT_RECOVERY_TYPE_EC_JOUR_GC		= 6, 			// Optional:  EC: JournalGarbageCollection. Triggered by a SERJIO request
	NVMEIBT_RECOVERY_TYPE_SCRUBBING			= 7, 			// Optional: Background scrubbing / integrity check
	NVMEIBT_RECOVERY_TYPE_VOID_DUMMY		= 8, 			// For debug Only, Dummy recovery which does not require any action, tests clnt-toma communication
	NVMEIBT_RECOVERY_TYPE_EC_FIX_UNK_BINFO	= 9, 			// Mandatory: EC: Resolve unknown blockset-info, This is only an optimization for first IO to blockset be fast after cold recovery. Dbits recovery does this work, but it might not run after cold recovery.
	NVMEIBT_RECOVERY_TYPE_NUM_RECOVERIES,					// Automatically mount of different possible recoveries, Must be a small number
}  __attribute__ ((packed));

/************************** Caser-Clnt-messages *******************************/
struct nvmeib_ecc_load_t {
	u32	load_level;					// How much client needs/have [1..10]
	u32	n_blocks_have;				// How much (cache entries?) client has / or how much it should have
};

/* msgs between Toma <==> Client (a.k.a Registrant)
 * CT = Client, RT = Registrant to Toma, TR = Toma to Registrant */
enum NVMEIBT_CLIENT_MSG_TYPES {
	NVMEIBT_CLIENT_MSG_ILLEGAL	= 0,		/* As Initialized by kmalloc()*/
	// Client complains to Toma aobut non-responsive server/drive
	NVMEIBT_CLIENT_MSG_CT_FAILED_CMD = 0x30 | NVMEIBT_PROTOCOL_SIGNATURE_CLIENT,
	NVMEIBT_CLIENT_MSG_CT_DI_DETECTED= 0x3C | NVMEIBT_PROTOCOL_SIGNATURE_CLIENT,	// Critical error, data corruption already occured. Take extreme measures

	// Client complains to toma about problematic lock
	NVMEIBT_CLIENT_MSG_CT_STALE_LOCK  = 0x31 | NVMEIBT_PROTOCOL_SIGNATURE_CLIENT,	// Stale lock prevents clients IO, client asks permission to take over, fix the blockset and release stale lock
	NVMEIBT_CLIENT_MSG_CT_FAILED_LOCK = 0x32 | NVMEIBT_PROTOCOL_SIGNATURE_CLIENT,	// Regular non stale is taken for too long. Client asks toma to unregister the other client which holds this lock for too long
	NVMEIBT_CLIENT_MSG_TC_LOCK_CLEANED= 0x33 | NVMEIBT_PROTOCOL_SIGNATURE_CLIENT,	// Toma replies that the client with this lockid finished lock&I/O. Once all segs are done, sync can start

	NVMEIBT_CLIENT_MSG_TC_LOCKID_CACHE_PURGE = 0x38 | NVMEIBT_PROTOCOL_SIGNATURE_CLIENT,
	NVMEIBT_CLIENT_MSG_CT_LOCKID_CACHE_PURGE_ACK = 0x39 | NVMEIBT_PROTOCOL_SIGNATURE_CLIENT,

	// Disk segment registration
	NVMEIBT_CLIENT_MSG_TR_REGISTRABLE_DISK_SEGMENT = 0x49 | NVMEIBT_PROTOCOL_SIGNATURE_REGISTER_TR,
	NVMEIBT_CLIENT_MSG_RT_REGISTER_DISK_SEGMENT = 0x50 | NVMEIBT_PROTOCOL_SIGNATURE_REGISTER_RT,
	NVMEIBT_CLIENT_MSG_TR_REGISTER_DISK_SEGMENT_ACK = 0x51 | NVMEIBT_PROTOCOL_SIGNATURE_REGISTER_TR,
	NVMEIBT_CLIENT_MSG_TR_REGISTER_DISK_SEGMENT_NACK = 0x52 | NVMEIBT_PROTOCOL_SIGNATURE_REGISTER_TR,
	NVMEIBT_CLIENT_MSG_TR_UNREGISTER_DISK_SEGMENT = 0x53 | NVMEIBT_PROTOCOL_SIGNATURE_REGISTER_TR,
	NVMEIBT_CLIENT_MSG_RT_UNREGISTER_DISK_SEGMENT = 0x54 | NVMEIBT_PROTOCOL_SIGNATURE_REGISTER_RT,
	NVMEIBT_CLIENT_MSG_TR_UNREGISTER_DISK_SEGMENT_ACK = 0x55 | NVMEIBT_PROTOCOL_SIGNATURE_REGISTER_TR,
	NVMEIBT_CLIENT_MSG_TR_SWITCH_PRAID_TOPOLOGY = 0x56 | NVMEIBT_PROTOCOL_SIGNATURE_REGISTER_TR,
	NVMEIBT_CLIENT_MSG_RT_SWITCH_PRAID_TOPOLOGY_ACK = 0x57 | NVMEIBT_PROTOCOL_SIGNATURE_REGISTER_RT,

	NVMEIBT_CLIENT_MSG_TR_VOLUME_MISMATCH = 0x61 | NVMEIBT_PROTOCOL_SIGNATURE_REGISTER_TR,

	NVMEIBT_CLIENT_MSG_TR_TOMA_NOT_READY = 0x81 | NVMEIBT_PROTOCOL_SIGNATURE_REGISTER_TR,
	NVMEIBT_CLIENT_MSG_TR_INVALID_DISK_SEGMENT_ID = 0x82 | NVMEIBT_PROTOCOL_SIGNATURE_REGISTER_TR,

	//Client recovery/rebuild messages
	NVMEIBT_CLIENT_MSG_RT_RECOVER_ANNOUNCE      = 0x90 | NVMEIBT_PROTOCOL_SIGNATURE_TOMA_REBUILD,
	NVMEIBT_CLIENT_MSG_TR_RECOVER_START			= 0x91 | NVMEIBT_PROTOCOL_SIGNATURE_TOMA_REBUILD,
	NVMEIBT_CLIENT_MSG_TR_RECOVER_ABORT			= 0x92 | NVMEIBT_PROTOCOL_SIGNATURE_TOMA_REBUILD,
	NVMEIBT_CLIENT_MSG_RT_RECOVER_FINISH		= 0x93 | NVMEIBT_PROTOCOL_SIGNATURE_TOMA_REBUILD,
	NVMEIBT_CLIENT_MSG_RT_RECOVER_PROGRESS		= 0x94 | NVMEIBT_PROTOCOL_SIGNATURE_TOMA_REBUILD,
	NVMEIBT_CLIENT_MSG_TR_RECOVER_PING			= 0x95 | NVMEIBT_PROTOCOL_SIGNATURE_TOMA_REBUILD,
};
const char *nvmeibt_protocol_client_msg_str(enum NVMEIBT_CLIENT_MSG_TYPES msg_type);

enum NVMEIBT_PRAID_TYPE {
	NVMEIBT_PRAID_TYPE_UNKNOWN =	0x0,
	NVMEIBT_PRAID_TYPE_JBOD =		0x1 << 0,
	NVMEIBT_PRAID_TYPE_RAID1 =		0x1 << 1,
	NVMEIBT_PRAID_TYPE_RAID5 =		0x1 << 2,
	NVMEIBT_PRAID_TYPE_RAID6 =		0x1 << 3,
	NVMEIBT_PRAID_TYPE_RAID5DP =	0x1 << 4,
};

enum NVMEIBT_CLIENT_MSG_DECODE_RES {
	NVMEIBT_CLIENT_MSG_DECODE_OK,
	NVMEIBT_CLIENT_MSG_DECODE_PROTOCOL_MISMATCH,
	NVMEIBT_CLIENT_MSG_DECODE_PROTOCOL_ERROR
};

static inline bool nvmeibt_protocol_is_praid_type_journalled(const enum NVMEIBT_PRAID_TYPE praid_type)
{
	return !!(praid_type & (NVMEIBT_PRAID_TYPE_RAID5 | NVMEIBT_PRAID_TYPE_RAID6 | NVMEIBT_PRAID_TYPE_RAID5DP));
}

const char *nvmeibt_praid_type_str(const enum NVMEIBT_PRAID_TYPE praid_type);

static inline enum NVMEIBT_IB_PROTOCOL_SIGNATURE nvmeibt_ib_protocol_signature(enum NVMEIBT_CLIENT_MSG_TYPES msg)
{
	return (enum NVMEIBT_IB_PROTOCOL_SIGNATURE)(((unsigned)msg) & 0xffff0000);
}

#define RESERVATION_MODE_IRRELEVANT (0ULL)
static inline bool is_reservation_mode_version_ahead(u64 reservation_mode_version, u64 reference_reservation_mode_version)
{
	return 	((reservation_mode_version != RESERVATION_MODE_IRRELEVANT) &&
			 (reservation_mode_version > reference_reservation_mode_version));
}

/* A msg sent from Toma to Client & From client to Toma*/
struct nvmeibt_client_msg {
	struct nvmeibt_client_msg_header {
		u32		protocol_version __attribute__ ((packed));			// NVMEIBT_CLIENT_PROTO_VERSION
		s32		msg_type __attribute__ ((packed));					// enum NVMEIBT_CLIENT_MSG_TYPES
		s32		reason __attribute__ ((packed));					// enum NVMEIBT_CLIENT_TR_REASON
		char	clnt_host_name[NVMEIB_HOST_NAME_LEN];				// Same as 1.3.2. Deprecated... Client node (for prints & debug only!). Was UUID in version <= v1.0.4, host_name since >=v1.0.5, Deprecated since V1.4
		s32		config_version __attribute__ ((packed));			// Currently unused
		s32		volume_config_version __attribute__ ((packed));		// Configuration version of the volume of this protection raid
		u64		cookie;												// Cookie is used as unique identifier for each msg for debug purpose
		char	reserved_hdr[8];
	} __attribute__ ((packed)) hdr;
	struct nvmeibt_client_thick_volume {
		u64		topology_version __attribute__ ((packed));		// Client does not use this field
		s32		praid_version __attribute__ ((packed));			// Defined by Toma, monotonically increasing generation of protection raid version
		char	disk_segment_uuid[NVMEIB_GID_STR_MAX];			// For historical reasons, client uses this field to verify the communication channel is not corrupted
		u32		reserved;										// In V 1.3.0 and before locks were 64bits
		u32		lock_id __attribute__ ((packed));				// Lock with which client is registered vs toma: union nvmeib_lock_id
		u64		reservation_mode_version __attribute__ ((packed));	// Client<-->Toma: Both supply the max reservation version, client preempts IO if TOMA > Client
		u64		conversation_ind;								// Monotonically increased number by client. Toma replies with the number that client sent it.
		u8		rt_never_reged_on_seg;							// Client->Toma Boolean (on UREG message): If true, client guarantees it never registered/issued IO with this lockid on this seg
		u8		is_REGISTER_for_recovery;						//
		char	reserved_thick[14];
		// Payload: Toma To client: Topology of varying length. Client to Toma: useses the struct below of clients payload
		u32		data_length __attribute__ ((packed));
		char	data[0];
	} __attribute__ ((packed)) thick;
	// data example, for CT_STALE_LOCK, stale_lock_id
} __attribute__ ((packed));

/* Toma and Client can fully decode each others message inplace
 * if decoding failed (unknown version), buf is unchanged and the function will return NULL
 */
struct nvmeibt_client_msg *nvmeibt_client_decode(u8 *buf, s32 len);
enum NVMEIBT_CLIENT_MSG_DECODE_RES nvmeibt_client_decode_new(u8 *buf, s32 len, struct nvmeibt_client_msg **decoded_msg);

/* In case Toma/Client are not able to decode the message, this function will try to extract
 * some usefull information from the message and send it back. Theoretically, this information
 * may help developers to verify the code correctness
 */
struct nvmeibt_client_msg_summary nvmeibt_client_decode_msg_summary(u8 *buf, s32 len);

/* Toma and Client can fully encode msg inplace before sending */
void nvmeibt_client_encode(struct nvmeibt_client_msg* msg);

/* Stale or failed locks report */
enum NVMEIBT_CLIENT_LOCK_OP {					// 1 byte
	NVMEIBT_CLIENT_LOCK_ILLEGAL =	0,
	NVMEIBT_CLIENT_LOCK_RD =		1,			// Read owner lock
	NVMEIBT_CLIENT_LOCK_XCHG =		2,			// Take/Release owner lock
  //NVMEIBT_CLIENT_ACT_WR = 3,					// Write active
	NVMEIBT_CLIENT_BIF_R =			4,			// Binfo failed read
	NVMEIBT_CLIENT_BIF_W =			5,			// Binfo failed write
} __attribute__ ((packed));

struct nvmeibt_client_recovery_generic_header {		// Header for all recovery related messages: Clnt->Toma, Toma->Clnt
	u64 id __attribute__ ((packed));				// Recovery task identifier, uniquely tied to praid topology version in the header of this payload
	s32 type __attribute__ ((packed));				// enum NVMEIBT_RECOVERY_TYPE
	u32 max_batch_size __attribute__ ((packed));	// Limit recovery batch size Units: RLBA blocksets. Max batch size x 128[kb] = ~0.5[Petabyte]. 0 means default.
	u8  effort_percents;							// 1-100. Client should do the rebuild with this relative speed. 100% is max, 1% is slowest. 0 - means field is irrelevant
	u8  reserved[7];
} __attribute__ ((packed));
#define NVMEIBT_CLIENT_PROTOCOL_EFFORT_PERCENTS_DONT_CARE 0
#define NVMEIBT_CLIENT_PROTOCOL_BATCH_SIZE_DONT_CARE 0

struct nvmeibt_client_msg_pl { 								// Optional Payload, send from Client to Toma & Toma to client
	union {
		struct nvmeibt_client_failed_cmd_pl {				// Clnt->Toma: Failed command notification includes the error code and the operation assuming that the msg includes a specific segment
			u64 offset	   __attribute__ ((packed));		// The offset of the command on the disk (not from start of segment) in units of blocks.
			u32 op	       __attribute__ ((packed));		// The failed OP
			u32 fail_map   __attribute__ ((packed));		// The bit map of blocks within the lockset that are unrecoverable -> written unrecoverable (slice is destroyed)
			u32 fix_map    __attribute__ ((packed));		// The bit map of blocks within the lockset that were fixed -> read failed and fixed
			u16 error_code __attribute__ ((packed));		// The nvme error code received on the failed IO
			u16 reserved   __attribute__ ((packed));		//
		} failed_cmd;
		struct nvmeibt_client_failed_lock_pl {				// Clnt->Toma: Problematic (stale) lock report. Sent to all toma's in protection raid.
			u32 lock_op __attribute__ ((packed));			// enum NVMEIBT_CLIENT_LOCK_OP
			s32 status  __attribute__ ((packed));
			u32 is_problem_here  __attribute__ ((packed));	// Unused: Boolean. True if lock was on disk of this Toma (server). False for other Toma's in protection raid
			u32 num_retries  __attribute__ ((packed));		// Number of retries client tried before requesting help
			u64 disk_blkno_4k	__attribute__ ((packed));	// Alligned to blkset size (128KB). Offset of the first block in the problematic blockset from the beggining of disk. This field is meaningless to non local tomas to this segment
			u64 curr __attribute__ ((packed));				// The problematic lock which client wants Toma's response about
			u64 comp __attribute__ ((packed));				// For debug: What client expected to see (Mostly 0=unlocked)
			u64 xchg __attribute__ ((packed));				// For debug: What client were trying to cmpxchng to (it's lock id)
			char problematic_seg_uuid_str[40];				// Segment where this problem was detected, sent for debug only (prefix of 15 uuid letters), can remove it
		} failed_lock;
		struct nvmeibt_cleaned_stalock_info {				// Toma->Clnt. Cleaned_lock response payload
			u32	lock_id; 									// id of lock that TOMA has cleaned to the request of this client.
			u32 reserved;									// In V 1.3.0 and before locks were 64bits
			u8 cuuid[16];									// UUID of the client which left this stale lock (needed to acquire his journal resources)
		} stalock_info;
		struct nvmeibt_lockid_cache_purge_pl {				// Toma<->Clnt. Request/Ack lockid zone purge.
			u64 purge_seqno __attribute__ ((packed));		// sequential number of request
			u64 start_counter __attribute__ ((packed));		// The zone to purge is describe using a range of counters using <start_counter, length>
			u64 length __attribute__ ((packed));			// The zone to purge is describe using a range of counters using <start_counter, length>
			u32 reserverd[36];								// To be same size as nvmeibt_client_recovery_start_pl
		} lockid_cache_purge;
		struct nvmeibt_client_recovery_announce_pl {		// Clnt->Toma: Inform Toma of recovery client availability
			char node_id[NVMEIB_GID_STR_MAX];				// Node UUID where reocverer resides
			s32 capacity __attribute__ ((packed));			// Recovery client capacity
		} recov_announce_unused;							// Currently unused. Local client is by default the recoverer
		struct nvmeibt_client_recovery_start_pl {			// Toma->Clnt: Inform client of a recovery task
			struct nvmeibt_client_recovery_generic_header task;	// Recovery task identifier
			s32 is_mandatory;								// =1, client must successfully clean every required blockset and retry failed sync until success
			s32 do_only_owners;								// =1, Process only blockset that the segment written in the message is their owner (barries the primary lock), =0,
			char praid_id[NVMEIB_GID_STR_MAX];				// volume UUID identifier
			u64 start_lock;									// Start of blkset locks range for recovery. Units: RLBA blocksets
			u64 num_locks;									// Number of blkset locks for Hot recovery
			union {											// Generic payload for recovery start
				char reserved[80];							// Save 80 bytes for future extention
				struct nvmeibt_c_recov_start_cold {
					u32 surviving_ram_bmp; 					// Cold recovery: bitmap for segments, which segs maintained RAM and which lost their RAM. if all lost their ram then this is true cold recovery of 100% of praid
				} cold;
			};
		} __attribute__((__aligned__(8))) recov_start;
		struct nvmeibt_client_recovery_taskid_pl {			// Toma->Clnt: Encapsulate reocvery task id, for msgs: PING/ABORT/Change speed priority msgs
			struct nvmeibt_client_recovery_generic_header task;
		} recov;
		struct nvmeibt_client_recovery_status_pl {			// Clnt->Toma: Update Toma regarding recovery process run by client
			struct nvmeibt_client_recovery_generic_header task;	// Recovery task identifier
			s32 ret_code __attribute__ ((packed));			// Recovery task return code (if applicable)
			s32 praid_version __attribute__ ((packed));		// Praid version respective of this recovery task
			u64 next_unfixed_lock __attribute__ ((packed)); // Regardless of ret_code, stores the minimal addressed of blockset for which (all blockset before were fixed). May be 0 if nothing was fixed. Upon recovery restart this value can be used as start_lock. Units: RLBA blocksets
			u64 num_locks_left __attribute__ ((packed));	// Blkset locks left to scan in this recovery task. May be 0 if nothing was fixed
		} recov_status;
		struct nvmeibt_client_msg_summary {
			u32	protocol_version __attribute__ ((packed));			// NVMEIBT_CLIENT_PROTO_VERSION
			s32	msg_type __attribute__ ((packed));					// enum NVMEIBT_CLIENT_MSG_TYPES
			s32	reason __attribute__ ((packed));					// enum NVMEIBT_CLIENT_TR_REASON
			u32	data_length __attribute__ ((packed));	// Future use: additional payload extention
		} prior_msg_summary;
	};
} __attribute__ ((packed));

/*****************           NetLink to local_clnt           ******************/
// Note that since the local_client is run on the same machine. There are no issues of Endianess

enum NVMEIBT_TOMA_TO_LOCAL_CLNT_MSG_TYPE {
	TOMA_TO_LOCAL_CLNT_MSG_TYPE_UNKNOWN = 0,
	TOMA_TO_LOCAL_CLNT_MSG_TYPE_ATTACH = 75645342,
	TOMA_TO_LOCAL_CLNT_MSG_TYPE_DETACH = 75645343
};

struct nvmeibt_toma_to_local_client_attach_params {
	enum NVMEIBT_TOMA_TO_LOCAL_CLNT_MSG_TYPE			msg_type;
	char						vol_name[32];
	union nvmeib_uuid			vol_uuid;
	size_t						serialized_config_len;
} __attribute__((__packed__));

struct nvmeibt_toma_to_local_client_msg {
	union {
		struct nvmeibt_toma_to_local_client_attach_params	attach_params;
	} payload;
	char		data[0];
} __attribute__((__packed__));

/******************************************************************************/

static inline const char *nvmeibt_recov_type_to_3str(enum NVMEIBT_RECOVERY_TYPE t)
{
	switch (t) {
	case NVMEIBT_RECOVERY_TYPE_DIRTY_REBUILD:		return "DBT";
	case NVMEIBT_RECOVERY_TYPE_STALE_REBUILD:		return "HOT";
	case NVMEIBT_RECOVERY_TYPE_EC_COLD:				return "CLD";
	case NVMEIBT_RECOVERY_TYPE_EC_JOUR_GC:		 	return "JGC";
	case NVMEIBT_RECOVERY_TYPE_STALE_LOCKS_PURGE:	return "PRG";
	case NVMEIBT_RECOVERY_TYPE_EC_DCONVICT_TURNON:	return "CNV";
	case NVMEIBT_RECOVERY_TYPE_SCRUBBING:	        return "SCR";
	case NVMEIBT_RECOVERY_TYPE_VOID_DUMMY:	        return "DUM";
	case NVMEIBT_RECOVERY_TYPE_EC_FIX_UNK_BINFO:	return "TXI";
	default: return "???";
	}
}

static inline
enum NVMEIBT_RECOVERY_TYPE nvmeibt_recovery_type_cast(s32 task_type){
	if ((NVMEIBT_RECOVERY_TYPE_INVALID <= task_type) && (task_type < NVMEIBT_RECOVERY_TYPE_NUM_RECOVERIES))
		return (enum NVMEIBT_RECOVERY_TYPE)task_type;
	return NVMEIBT_RECOVERY_TYPE_INVALID;
}

/*************     PRAID topology     ***************/

enum NVMEIBT_PRAID_IS_PRIMARY_OWNER_LOCK_MOVING {
	NVMEIBT_PRAID_IS_PRIMARY_OWNER_LOCK_MOVING_UNKNOWN =0,	// Never used.
	NVMEIBT_PRAID_IS_PRIMARY_OWNER_LOCK_MOVING_NO =		1,	// All clnts see primary owner on the same seg (reads might view locks)
	NVMEIBT_PRAID_IS_PRIMARY_OWNER_LOCK_MOVING_YES =	2,	// Clnts might see different primarty owners, cannot view locks on read
} __attribute__ ((packed));

union io_perms_bitfield {
	struct {
		u8	is_io_R				: 1;	// 0
		u8	is_io_W				: 1;	// 1
		u8	is_cold_recovery	: 1;	// 2 - is cold recovery allowed
		u8	is_hot_recovery		: 1;	// 3 - is hot recovery allowed
		u8  is_jgc_recovery     : 1;	// 4 - is garbage collection allowed
		u8  reserved 			: 3;
	} bits;
	u8	all;
};

struct nvmeibt_client_topo_praid {
	char	uuid[NVMEIB_GID_STR_MAX];					// praid uuid, unused
	__concurrent_access s32		praid_version __attribute__ ((packed));		// Already exists in the header, be it.
	u32		topo_checksum;								// Checksum of topology. Must be identical regardless of whihc toma generates it. Designed to catch bugs where different topo use same praid version
	u8		blkset_sync_safety;	// enum NVMEIBT_PRAID_IS_PRIMARY_OWNER_LOCK_MOVING, Daniel, incorrect location, should be per segment field
	u8		reserved_3[3];
	u8  	io_perms;			// union io_perms_bitfield
	int8_t	n_segments;
	u8		reserved_protection_raid[14];
	char	segs[0];	// Array of struct nvmeibt_client_topo_disk_segment
} __attribute__ ((packed));

/*************     DISK_SEGMENT topology     ***************/
enum NVMEIBTC_DS_MODE {					/* Access mode of a segment */
	NVMEIBTC_DS_MODE_INVALID = 0,		/* Value set by kmalloc() */
	NVMEIBTC_DS_MODE_RW = 1,			/* Can Read and write to segment */
	NVMEIBTC_DS_MODE_W = 2,				/* Can write, reads from parity/mirror*/
	NVMEIBTC_DS_MODE_DEAD = 3,			/* Cant use, read from parity */
	NVMEIBTC_DS_MODE_W_NO_DIRTY = 4,	/*  Fully recovered. A qualified owner. No dirty, waiting to become an owner using SW_TOPOs/UNREG, Same as W for me (clnt) but, all dbits were turned off, other client can Read, so must be careful */
	NVMEIBTC_DS_MODE_W_IS_DIRTY = 5,	/*  Assume this seg is completely dirty. After seg replacement even if dbits are not turned on, the new seg does not have correct data */
} __attribute__ ((packed));				// Size of 1[byte]
static inline const char *nvmeibt_client_topo_seg_access_mode_to_str(s32 access_mode)
{	// Note: Those strings must be 3 characters & are visible to customers! Dont touch!
	switch ((enum NVMEIBTC_DS_MODE)access_mode) {
	case NVMEIBTC_DS_MODE_RW:			return "RW ";	// Can/Should Read/Write to this seg
	case NVMEIBTC_DS_MODE_W_NO_DIRTY:	return "W+ ";	// Seg holds correct data but it is better not to read from it, unless there is no choice
	case NVMEIBTC_DS_MODE_W:			return "W  ";	// Seg might not hold latest data (dbits may mark areas for rebuild). Reading may lead to data corruption
	case NVMEIBTC_DS_MODE_W_IS_DIRTY:	return "W- ";	// Seg might not hold latest data + dbits markers cannot be trusted. Reading will lead to data corruption
	case NVMEIBTC_DS_MODE_DEAD:			return "N/A";	// Not Available - cannot be used for any IO
	case NVMEIBTC_DS_MODE_INVALID:		return "???";	// Unknown, poisoned
	default: 							return "ERR";	// Should never happen
	}
}

enum NVMEIBTC_DS_OWNER_MODE {            /* Type of owners in protection raid */
	NVMEIBTC_DS_OWNER_MODE_INVALID = 0,  // Illegal, kzalloced()
	NVMEIBTC_DS_OWNER_MODE_NO_OWNER = 1, // Do not lock this segment at all, either it is dead or toma said it is not part of the locking server
	NVMEIBTC_DS_OWNER_MODE_PRIMARY = 2,  // Typical: {RW,RW} both segments are owners
  //NVMEIBTC_DS_OWNER_MODE_ACTIVE  = 3,
	NVMEIBTC_DS_OWNER_MODE_SECONDARY= 4, // {RW,W} Dual lock mode, RW segment is owner, W segment is secondary owner
	NVMEIBTC_DS_OWNER_MODE_COPY_OWNER = 5,// In EC/R1, copy of owner lock
} __attribute__ ((packed));				// Size of 1[byte]

static inline bool nvmeibtc_ds_owner_mode_is_valid(enum NVMEIBTC_DS_OWNER_MODE t)
{
	return (t == NVMEIBTC_DS_OWNER_MODE_PRIMARY) || (t == NVMEIBTC_DS_OWNER_MODE_SECONDARY) || (t == NVMEIBTC_DS_OWNER_MODE_COPY_OWNER);
}

static inline char nvmeibtc_ds_owner_mode_to_chr(enum NVMEIBTC_DS_OWNER_MODE t)
{	// Note: Those strings must be 1 characters & are visible to customers! Dont touch!
	switch (t) {
	case NVMEIBTC_DS_OWNER_MODE_PRIMARY:   return 'O';
	case NVMEIBTC_DS_OWNER_MODE_COPY_OWNER:return 'C';
	case NVMEIBTC_DS_OWNER_MODE_SECONDARY: return 'S';
	default: 							   return '?';
	}
}

struct nvmeibt_client_topo_disk_seg_owner { // Client that wished to write to a segment must take lock described by the struct below
	char	seg_uuid[NVMEIB_GID_STR_MAX-1];	// uuid of the segment which holds a lock
	char	mode;							// Type of the owner enum NVMEIBTC_DS_OWNER_MODE
} __attribute__ ((packed));					// Size of NVMEIB_GID_STR_MAX[bytes]

struct nvmeibt_client_topo_disk_segment {
	char	uuid[NVMEIB_GID_STR_MAX];
	s32		unused23 __attribute__ ((packed));
	s32		access_mode __attribute__ ((packed)); /* enum NVMEIBTC_DS_MODE */
	char	reserved[40];				// Backward compatibility
	struct nvmeibt_client_topo_disk_seg_owner owners[N_MAX_RAID_LOCKS]; // If not all the segments are used, uuid is "", mode is NVMEIBTC_DS_OWNER_MODE_NO_OWNER
	/* Note: Lock server mapping support up to 4 locks (4-mirrring or 3 parity erasure coding) */
} __attribute__ ((packed));

struct tTopoOfPraid{ 												 // Toma topology of protection raid, send as payload to client (only the needed amount of segments is sent
	struct nvmeibt_client_topo_praid		header;					 // Note: if segment is non mirrored then some of 'header' field are unused
	struct nvmeibt_client_topo_disk_segment	s[N_MAX_RAID_SLICE_LEN]; // Note: Only the first needed N segments are sent, so payload can be smaller than size of the whole struct
};

static inline int nvmeibt_tTopoOfPraid_len(struct tTopoOfPraid *T)
{
	return (T ? (void *)(&(T->s[T->header.n_segments])) - (void *)T : 0);
}

void nvmeibt_client_reset_seg_topo_owners(struct nvmeibt_client_topo_disk_segment *seg_topo);

/* Toma <--> Client, Create and encode msg for thick volumes with payload
   (except toma praid topo payload). */
void nvmeibt_client_thick_msg_write(struct nvmeibt_client_msg *m,
									s32			msg_type,
									s32			reason,
									const char	*clnt_host_name,
									u32	 		protocol_version,
									s32			config_version,
									s32			volume_config_version,
									u64			topology_version,
									s32			praid_version,
									const char	*disk_segment_uuid,
									u32			lock_id,
									u64			reservation_mode_version,
									u64    		conversation_ind,
									u8			rt_never_reged_on_seg,
									u8			is_REGISTER_for_recovery,
									u32			data_length,
									void		*data,
									u64			msg_id);

/* Decodes the message inplace and breaks the struct to fields
   If unable to do so (unknown version) will return null*/
enum NVMEIBT_CLIENT_MSG_DECODE_RES
nvmeibt_client_thick_msg_read(u8 *buf, s32 len,
							  enum NVMEIBT_CLIENT_MSG_TYPES	*msg_type,
							  s32	*reason,
							  char	*clnt_host_name,
							  size_t	clnt_host_name_size,
							  u32	*protocol_version,
							  s32	*config_version,
							  s32	*volume_config_version,
							  u64	*msg_id,
							  u64	*topology_version,
							  s32	*praid_version,
							  char	*disk_segment_uuid,
							  size_t	disk_segment_uuid_size,
							  u32	*lock_id,
  							  u64	*reservation_mode_version,
							  u64   *conversation_ind,
							  u8	*rt_never_reged_on_seg,
							  u8	*is_REGISTER_for_recovery,
							  u32	*data_length,
							  void	**data,
							  struct nvmeibt_client_msg **pcl)__attribute__ ((warn_unused_result));

void nvmeibt_client_topo_praid_write(struct nvmeibt_client_topo_praid *r,
									 const char	*uuid,
									 s32		praid_version,
									 u8			blkset_sync_safety,
									 u8			io_perms,
									 int8_t		n_segments);

void nvmeibt_client_topo_praid_read(const struct nvmeibt_client_topo_praid 	*pr,
									char									*uuid,
									size_t									uuid_size,
									s32										*praid_version,
									u8										*blkset_sync_safety,
									u8										*io_perms,
									int8_t									*n_segments
								   );


void nvmeibt_client_topo_disk_segment_write(struct nvmeibt_client_topo_disk_segment *d,
											const char	*uuid,
											s32			access_mode
										   );

void nvmeibt_client_topo_disk_segment_read(struct nvmeibt_client_topo_disk_segment *d,
										   char										*uuid,
										   size_t									uuid_size,
										   s32										*access_mode,
										   char										*uuid_of_primary_owner_segment,
										   size_t									uuid_of_primary_owner_segment_size,
										   char										*uuid_of_secondary_owner_segment,
										   size_t									uuid_of_secondary_owner_segment_size
										  );

/******************** Toma owner-scheme ***************************************/
int8_t get_owner_idx_by_owner_scheme_type(int8_t owner_no, s32 type, int8_t idx_in_praid, int8_t n_segments);

void nvmeibt_client_topo_disk_segment_upd_owners(struct nvmeibt_client_topo_disk_segment *praid_segs,
												 int8_t n_segments,
												 const char *uuid_of_primary_owner_segment,
												 const char *uuid_of_secondary_owner_segment,
												 s32 type, int8_t max_n_owners, int8_t idx_in_praid);

#endif	// #ifndef NVMEIBT_CLIENT_PROTOCOL_H

