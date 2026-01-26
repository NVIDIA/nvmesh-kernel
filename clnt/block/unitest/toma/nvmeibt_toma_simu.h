#ifndef NVMEIBT_TOMA_SIMU_H
#define NVMEIBT_TOMA_SIMU_H
/* Simulator of the appropriate toma file which implements the full
   functionality of toma needed for client's operation (no more, no less) */
#include "nvmeibt_topology_simu.h"
#include "../uni_framework/unitest_defs.h"
#include "nvmeibs_toma.h"

/**************************** msg queue **************************************/
/* A msg queue to hold msgs that toma sends to client rather than sending them
   immediately, causing delay in msg arrival.
   BEWARE: this API is not locking the queue, so be carefull when using it
   directly from external module. most uses are through TOMA which has a lock
   to protect access to its internal queue object. */
struct nvmeibc_disk_subscription_params;

struct toma_msg_q_to_client_elem {
	int	msg_size;									// the msg buffer size
	u8	*msg_buf;									// the msg buffer
	struct nvmeibc_disk_subscription_params con;	// connection on which the msg will be sent
};						// a queue of msgs that get delayed on their way to the client

struct toma_msg_q_to_client {
	s8 toma_unique_id;
	#define TOMA_MSG_Q_MAX_MSGS		10
	int	size;											// number of msgs now in queue
	int	head, tail;										// index of oldest msg (head) & first unused slow (tail). (head==tail)) => empty, ((head+1)%MAX_MSGS == tail)
	struct toma_msg_q_to_client_elem msgs[TOMA_MSG_Q_MAX_MSGS];	// a queue of msgs that get delayed on their way to the client
};
void    toma_msg_q__init(          struct toma_msg_q_to_client *q, s8 toma_unique_id);
int     toma_msg_q__get_size(const struct toma_msg_q_to_client *q); // returns the number of TOMA msgs now queued, waiting to be sent to clients
#define toma_msg_q__is_empty(                                   q) (toma_msg_q__get_size(q) == 0)
#define toma_msg_q__is_full(                                    q) (toma_msg_q__get_size(q) == TOMA_MSG_Q_MAX_MSGS - 1)
void    toma_msg_q__enqueue (      struct toma_msg_q_to_client *q, const struct nvmeibc_disk_subscription_params con, u8 *msg, int msg_size); // enqueue a msg so its sent later
void    toma_msg_q__dequeue (      struct toma_msg_q_to_client *q, struct toma_msg_q_to_client_elem	*elem);
u8*     toma_msg_q__view_last_msg( struct toma_msg_q_to_client *q); // Unsafe function, view the encoded buffer of last enqueud message (assuming queue not empty)
int     toma_msg_q__flush (        struct toma_msg_q_to_client *q, const int msg_count); // flush the given number of msgs (msg_count) to their destination. if (msg_count == 0) send all msgs that are now in q, including any that are enqueued while we flush (to maintain order). return the number of flushed msgs.

/*************************** Switch-topology class ****************************/
#define SW_TOPO_SEG_INDEX_BITS			(5)								// 4 bits for 16 segs is enough but we use -1 illegal so add sign bit
#if ((1U << SW_TOPO_SEG_INDEX_BITS) <= (N_MAX_RAID_SLICE_LEN<<1))
	#error "Segment index range exceeds swith_topo_options member size"
#endif

// switch_topo API options along with predefined commonly used options
struct switch_topo_options {
	u32	dont_send_msg 	: 1;						// Toma updates topology but does not send anything to client. Simulates as if client could not receive the message
	u32	can_fail	 	: 1;						// If client is unregistered from Toma swich topo cannot be sent. If true - we allow this case. If false, will yield a BUG_ON(). If actually send fails, wait_for_ack is not checked
	u32	wait_for_ack 	: 1;						// set to wait for the processing of this by the client, until toma accepts an ACK. verifies that a switch topo can be sent!!!
	u32	wait_for_ack_drain : 1;						// If the above is 'On' after getting ACK, drain toma messages (verifies clients REG requests according to new Topo are finished) and IO should be enabled
	u32	use_seg_index 	: 1;						// instruct the API to use the segment index given in this options struct rather than deduce it from the raid object state.
	u32	seg_index		: SW_TOPO_SEG_INDEX_BITS;	// the segment index to which the test forces the msg to be sent. If 'can_fail==false' will send the msg even if segment is unregistered (illegal broken msg that real toma would hopefully never send)
	u32 dry_run			: 1; 						// don't modify any bit of the system state; usefull to find out toma courier
} __attribute__ ((packed));

extern struct switch_topo_options SW_TOPO__SILENT, SW_TOPO__NONE, SW_TOPO__WAIT_ACK, SW_TOPO__WAIT_ACK_DR;

struct switch_topo_dest {	// Describe the target {raid, disk, segment} to which we sent SwitchTopo msg.
	struct tTopoOfPraid	*r1;
	int		disk_ind	: 16;
	int		seg_ind		: SW_TOPO_SEG_INDEX_BITS;	// index into the raid members array
} __attribute__ ((packed));

enum uni_recov_caller {
	UNI_RECOV_CALLER_TOMA = 0,
	UNI_RECOV_CALLER_SERJIO,
};

/*****************************************************************************/
/* HashTable of Toma connections (sockets). Each client has a disctinct
   connection for each segment of each volume on each disk on the toma server */
struct tomaSimulator_socket {
	struct nvmeibc_disk_subscription_params  		 con;			// Params of how to send messages to client
	u64									 		 key;			// Keys of hash table. If client
	char								   *seg_UUID;			// UUID's of the segment of this connection (known upon first registeration). Not deleted when client unregisters
	union nvmeib_lock_id 						 lid;			// Becomes false (0) when client unregisters explicitly or we expect him to silently unregisterd without notifying the toma (there was loss of connection). Otherwise stores clients positive lock-id
	u8                                     cuuid[16];			// Erasure coding only: Mapping of LockId->ClntUUID(cid)->JRI->{offset on disk & offset in JMDC}. Using Ram Table LockID-->UUID Toma can returmn UUID which then be used via Serjio to fond Journal Range used by this client.
	bool			 		 should_send_registrable;			// If toma was in a state of NOT_READY or requested unregistration, it should send registrable message in the future
	bool					wait_for_switch_topo_ack;			// indication that we are still waiting for switch_topo ack from clients
	struct sim_toma_recoveries{
		volatile bool is_waiting;
		struct completion wait;
		enum uni_recov_caller recov_caller;
		int recov_status;
		struct nvmeibt_client_msg_pl task;
	} rcvrs[NVMEIBT_RECOVERY_TYPE_NUM_RECOVERIES];			// .task_id = 0 if segment of this raid is not pariticpating in recovery. Otherwise points to recovery start msg which Toma sent to client
	int inst_id;
};

struct t_toma_lock_id_rejections {						// Allows Toma to reject clients registration coz: 1. All lock ids are taken, 2. His lock-id is taken
	union nvmeib_lock_id lid;							// Last clients lock id which was rejected
	u16 n_rejects;										// Debug counter: Amount of times Toma rejected client's registration request
};

/*****************************************************************************/
enum tomaState {
	tomaState_down 		= 0,						// Toma service crashed, will not answer to client
	tomaState_not_ready,							// Toma is running but still cannot serve client
	tomaState_no_free_lock_ids,						// Toma is running but all lock-ids are used. Registered segs continue to operate. New registers are not accepted
	tomaState_running,								// Toma is in normal state.
};

enum toma_msg_queueing_state {
	toma_msg_queueing_state__disabled,				// msgs are sent immediately (i.e.: not queued)
	toma_msg_queueing_state__enabled,				// msgs are queued
};

struct tomaSimulator{    								// Simulator of toma process running on server
	enum tomaState state;
	s8 uniqueID;										// Unique ID of toma, for debugging. Toma leader has the ID 0.
	const struct mongo_db_simu *global_conf;			// Configuration of all volumes
	struct tTopoOfNVMesh *globalTopo;					// Reference to global singletone topology of the entire NVMesh as given by configuration and calculated by toma leader
	bool	f_protoBugsSilent;							// Toma will not alert when client deviates from the protocol (like unregistering from unknown segment). Usefull for debugging client crashes, without toma interfering with its own bugs
	bool	f_unitestBugsSilent;						// Allert when unitest environment deviates from the protocol (for example sending message to client which properly unsubscribed and does not exist anymore)
	bool	f_forbidSendingUnregisterAck;				// If not permitted, simulates as if unregister ack was lost or real Toma's bug made the message not arrive
	struct {	// Todo: Move to dedicated struct: What to expect on in failure 'msg'
		short	expect_io_failure;						// Used to determine that the client sent a NVMEIBT_CLIENT_MSG_CT_FAILED_CMD with the correct value
		u32     fail_mask;								// Shows a mask of unrecoverable slices (where we destroy the slice if needed)
		u32		fix_mask;								// Shows a mask of fixed slices (where readfail succeeded)
	};
	struct t_toma_lock_id_rejections lid_rej;			// Debug counter: Amount of times Toma rejected client's registration request coz all lock-ids are taken
	//struct {											// Hashtable Subscribed segments-> Toma-Clnt
		u32  socks_gen : 16;							// Debug: Each new addition to/removal from hash table increases the generation.
		struct tomaSimulator_socket sock[NVMESH_N_MAX_CLIENTS*16];			// Each segment is communicated over a socket. Supports registration of at most 16 segments per client
		struct spinlock		lock;						// lock serializing access to the object
		pthread_mutex_t		recov_msg_lock;				// Recursive (additional, outer) mutex guarding only the atomicity of recovery start and its immediate message queueing/processing
	//};
	enum toma_msg_queueing_state msg_q_state;			// when set, a msg sent from toma to client should be queued instead of sent to client
	struct toma_msg_q_to_client msg_q;
	u32 recov_task_id;									// Unique recovery task id generator
	u32 protocol_version;
	union nvmeib_lock_id *stale_locks;					// An array of stale locks, one per blockset (used when HTR fails and leaves a new stale, which needs to be replaced with the previous one held here) taken from stale_locks_hash nvmeibt_disk_segment.h:196
	struct completion serjio_done;						// A completion object to be able to wait for serjio to clear/abandon entries when degrading/restoring a segment
	atomic_t num_running_recoveries;
	bool ignore_jour_gc_launch_request;
};

void tomaSimulator_init(   		 	struct tomaSimulator*, const struct mongo_db_simu *, struct tTopoOfNVMesh *, u32 protocol_version);	// Constructor of Toma simulator
void tomaSimulator_destroy(	 	 	struct tomaSimulator*);
void tomaSimulator_disconnect(	 	struct tomaSimulator*);	// Same as client got pause on disk. Toma and client disconnected (like physical unplug of the network cable)
void tomaSimulator_reconnect(	 	struct tomaSimulator*);
void tomaSimulator_onClntDiscovery(	struct tomaSimulator*, uuid_be* cuuid, int jri);		// nvmeibs calls Toma when client connects to disk
void tomaSimulator_onClntDiskRelese(struct tomaSimulator*, u32 jri);						// nvmeibs calls Toma when client does disk_release
void tomaSimulator_onBlocksetRecovered(struct tomaSimulator*, const struct nvmeibc_disk_gen_cmd*);	// Implementation of: nvmeibt_seg_active_handle_blkset_recovered(), nvmeibs calls Toma when hot recovery cleaned a lock
struct nvmeibc_disk_free_jrnl_ents_comp;

int  tomaSimulator_subscribe_seg(   struct tomaSimulator*, u64 handle, struct nvmeibc_disk_subscription_params  *params, u8* cuuid, int propagated_rv);	// Called by transport layer of client to subscribe a segment
int  tomaSimulator_unsubscri_seg(   struct tomaSimulator*, u64 handle);
void tomaSimulator_recv_clnt_msg(   struct tomaSimulator*, u64 handle, struct nvmeibc_disk_toma_send_params *params);	// Client sends a message to toma via this method
int  tomaSimulator_unreg_raid1( const char r1_uuid[40], int ind_of_dead_seg);	// Update client that one segment died (entering a degraded mode immediately)
int  tomaSimulator_unreg_all(struct tomaSimulator*_this, int inst_id);						// instructs Toma to unregister all segments that are now registered in its DB. Returns number of segments that were unregistered
int  tomaSimulator_send_unreg_to_raid1_increase_reservation_version(const char r1_uuid[40]);

//a thin wrapper; which allows to switch specific segment topology
int  tomaSimulator_switchSegmentTopo(const struct TstPRaid* praid, enum NVMEIBTC_DS_MODE mode, const struct switch_topo_options opts, struct switch_topo_dest *dst);
int  tomaSimulator_switchSegmentTopo_find_courier(const struct TstPRaid* praid, const struct switch_topo_options opts, struct switch_topo_dest *dst); //do nothing; just find the toma courier
int	 tomaSimulator_switchTopo(  const char r1_uuid[40], enum NVMEIBTC_DS_MODE s0, enum NVMEIBTC_DS_MODE s1, const struct switch_topo_options opts);	// Toma leader will update clients regarding the new Raid1 topology. (Raid1 is identified by its uuid).  lock_second=0, no dual lock, otherwise index of segment+1 (1 or 2)
int	 tomaSimulator_switchTopoEC(const char r1_uuid[40], const enum NVMEIBTC_DS_MODE s[N_MAX_RAID_SLICE_LEN],const struct switch_topo_options opts, struct switch_topo_dest *dst);	// Same for Erasure coded volumes
int  tomaSimulator_switchTopo_dummy(int vold_ind, int r1_ind, const struct switch_topo_options opts, struct switch_topo_dest *dst); //send switch topology without modification
void tomaSimulator_waitSwitchTopoAck(  const int toma_index, const char* segUUID);	// used to verify switch topo ack. toma_index - is the result of tomaSimulator_switchTopo()
void tomaSimulator_unsetSwitchTopoWait(const int toma_index, int inst_id, const char* segUUID);	// used to unset wait for switch topo ack, wait must be set!
void tomaSimulator_sendUnregisterMsg(struct tTopoOfPraid* r1, const int segInd, const struct disk_range segs[], int inst_id);	// ATTENTION: Artificial message (not part of client-toma protocol). Force a toma (possibly dead one) to send unregister msg (on a previously unregistered segment)

/****************************** Recovery Stuff ********************************/

struct toma_recovery_args {
	enum NVMEIBT_RECOVERY_TYPE type;			// Which recovery HTR/Cold/Dbits/....
	enum NVMEIBT_CLIENT_MSG_TYPES cmd;			// What to do: Start/Stop/Ping/...
	bool on_start_wait_for_end;					// Blocking wait for the request 'cmd' to complete
	u16 surviving_ram_bmp;						// Cold     recovery only: Default is 0
	enum uni_recov_caller recov_caller;
	bool has_dconv;								// When dbits rebuild is launched, it can cause dconv_turn_on epilog recovery
	struct toma_recovery_ext_args {			// Allows overriding default recovery values such as range and mandatory
		struct {
			bool override;							// If set to true will use the given values for range
			u64 start;
			u64 count;
		} lock_range;
		struct {
			bool override;							// If set to true will use the given value for mandatory
			bool value;
		} is_mandatory;
	} ext_args;
};

static const struct toma_recovery_args RCVR_INVALID				= {.type = -1,                                  .cmd = NVMEIBT_CLIENT_MSG_TR_RECOVER_START, .on_start_wait_for_end = true,  .recov_caller = UNI_RECOV_CALLER_SERJIO, .has_dconv = 0};
static const struct toma_recovery_args RCVR_STALE_REBUILD		= {.type = NVMEIBT_RECOVERY_TYPE_STALE_REBUILD, .cmd = NVMEIBT_CLIENT_MSG_TR_RECOVER_START, .on_start_wait_for_end = true,  .recov_caller = UNI_RECOV_CALLER_TOMA,   .has_dconv = 0};
static const struct toma_recovery_args RCVR_STALE_REBUILD_PING	= {.type = NVMEIBT_RECOVERY_TYPE_STALE_REBUILD, .cmd = NVMEIBT_CLIENT_MSG_TR_RECOVER_PING,  .on_start_wait_for_end = true,  .recov_caller = UNI_RECOV_CALLER_TOMA,   .has_dconv = 0};
static const struct toma_recovery_args RCVR_STALE_REBUILD_ABORT	= {.type = NVMEIBT_RECOVERY_TYPE_STALE_REBUILD, .cmd = NVMEIBT_CLIENT_MSG_TR_RECOVER_ABORT, .on_start_wait_for_end = true,  .recov_caller = UNI_RECOV_CALLER_TOMA,   .has_dconv = 0};
static const struct toma_recovery_args RCVR_DIRTY_REBUILD		= {.type = NVMEIBT_RECOVERY_TYPE_DIRTY_REBUILD, .cmd = NVMEIBT_CLIENT_MSG_TR_RECOVER_START, .on_start_wait_for_end = true,  .recov_caller = UNI_RECOV_CALLER_TOMA,   .has_dconv = 0};
static const struct toma_recovery_args RCVR_DIRTY_REBUILD_PING	= {.type = NVMEIBT_RECOVERY_TYPE_DIRTY_REBUILD, .cmd = NVMEIBT_CLIENT_MSG_TR_RECOVER_PING,  .on_start_wait_for_end = true,  .recov_caller = UNI_RECOV_CALLER_TOMA,   .has_dconv = 0};
static const struct toma_recovery_args RCVR_DIRTY_REBUILD_ABORT	= {.type = NVMEIBT_RECOVERY_TYPE_DIRTY_REBUILD, .cmd = NVMEIBT_CLIENT_MSG_TR_RECOVER_ABORT, .on_start_wait_for_end = true,  .recov_caller = UNI_RECOV_CALLER_TOMA,   .has_dconv = 0};
static const struct toma_recovery_args RCVR_DIRTY_REBUILD_CONV  = {.type = NVMEIBT_RECOVERY_TYPE_DIRTY_REBUILD, .cmd = NVMEIBT_CLIENT_MSG_TR_RECOVER_START, .on_start_wait_for_end = true,  .recov_caller = UNI_RECOV_CALLER_TOMA,   .has_dconv = 1};
static const struct toma_recovery_args RCVR_EC_JOUR_GC			= {.type = NVMEIBT_RECOVERY_TYPE_EC_JOUR_GC,    .cmd = NVMEIBT_CLIENT_MSG_TR_RECOVER_START, .on_start_wait_for_end = true,  .recov_caller = UNI_RECOV_CALLER_TOMA,   .has_dconv = 0};
static const struct toma_recovery_args RCVR_EC_JOUR_GC_ASYNC	= {.type = NVMEIBT_RECOVERY_TYPE_EC_JOUR_GC,    .cmd = NVMEIBT_CLIENT_MSG_TR_RECOVER_START, .on_start_wait_for_end = false, .recov_caller = UNI_RECOV_CALLER_SERJIO, .has_dconv = 0};
static const struct toma_recovery_args RCVR_EC_FIX_UNK_BINFO	= {.type = NVMEIBT_RECOVERY_TYPE_EC_FIX_UNK_BINFO, .cmd = NVMEIBT_CLIENT_MSG_TR_RECOVER_START, .on_start_wait_for_end = true,  .recov_caller = UNI_RECOV_CALLER_TOMA,   .has_dconv = 0};

int  tomaSimulator_recoverThing(      const struct tTopoOfPraid *r1, const struct disk_range *seg, const struct toma_recovery_args args);	// Instruct toma to send recovery request/abort/cancel message to client
int  tomaSimulator_recoverThingStatus(const struct tTopoOfPraid *r1, const struct disk_range *seg, const struct toma_recovery_args args, int *recov_status);	// Same as above, but also returns the recovery result, if waited for.
int  tomaSimulator_JGC_launch(const char *seg_uuid);	// Serjio instruct toma to send garabge collection recovery start
void tomaSimulator_send_registrables(  struct tomaSimulator*);  // Force the toma to send registrable message for all the relevant segments (messages are much like response by NACK)
bool tomaSimulator_isSegRegistered(    struct tomaSimulator*, const char* segUUID, int inst_id); // Query the Toma whether segment is registered with her
void tomaSimulator_wait_seg_registered(struct tomaSimulator*, const char* segUUID, int inst_id); // wait until a segment is registered with the specified Toma.

void tomaSimulator_waitProtoEnd( 	   struct tomaSimulator*);	// Wait until live messages between clients and this toma terminate (important to wait before destroying the toma)
void tomaSimulator_protoBugs(bool permitClient, bool permitUnitest);	// Set values of protocol bugs guards
void tomaSimulator_permitUnregAcks(bool permit);		// Set values of the flag
void tomaSimulator_expectIOFailure(struct tomaSimulator*, short error_code, const u32 failed, const u32 fixed);// Force expectation
void tomaSimulator_verifyIOFailure(void);				// Verify that no toma expects failure commands
void tomaSimulator_clean_rejected_lids(struct tomaSimulator	*);  // Clean Tomas state of rejected lock-ids.
void tomaSimulator_enable_msg_q_to_client( struct tomaSimulator*);  // enable queueing of msgs sent from TOMA to clients - msgs wont arrive at clients from this TOMA from this point on.
void tomaSimulator_disable_msg_q_to_client(struct tomaSimulator*);	// disable queueing of msgs sent from TOMA to clients. queued msgs will be flushed to the appropriate clients.
int  tomaSimulator_get_msg_queue_size(struct tomaSimulator *_this); // get the number of msgs enqueued for clients
int  tomaSimulator_flush_msg_to_client(struct tomaSimulator*, const int n_msgs);	// Flush up to 'n_msgs' in the queue to client without disabling the queue. use amount 0 to signify all

// TOMA previous locks logic
void tomaSimulator_set_stale_lock(  struct tomaSimulator* , u64 addr, u64 lock_id);
void tomaSimulator_unset_stale_lock(struct tomaSimulator* , u64 addr);
bool tomaSimulator_has_stale_lock(  struct tomaSimulator* , u64 addr);
void tomaSimulator_verify_no_locks( struct tomaSimulator*);
int  tomaSimulator_clear_all_locks( struct tomaSimulator*);

// cold recovery
void nvmeibr_ds_metadata_init_EC_lock_and_dirty(struct tomaSimulator*, struct TstPRaid *);

void tomaSimulator_dump(                  struct tomaSimulator *);
void tomaSimulator_verifyNoSwitchTopoWait(struct tomaSimulator *);		// verify that no segment has an indication of waiting for SwitchTopoAck
struct tomaSimulator*  tomaSimulator_getToma_by_disk_id(u32 disk_index);
// Actions for Toma leader / Entire network
void tomaNetwork_dump(void);
void tomaNetwork_verifyNoSwitchTopoWait(void);			// verify that no Toma has has an indication of waiting for SwitchTopoAck on any segment
void tomaNetwork_swap_2tomas(struct tomaSimulator *t1, struct tomaSimulator *t2); // swaps tomas connections (used to simulate disk removal and reinsert into different target)

#endif // NVMEIBT_TOMA_H

