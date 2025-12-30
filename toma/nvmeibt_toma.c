#include "nvmeibt_debug.h"
#include <stdio.h>
#include <stdlib.h>
#include <linux/limits.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <getopt.h>
#include <sys/file.h>
#include <printf.h>
#include <sys/ucontext.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <dirent.h>
#include <libgen.h>
#include <string.h>
#include <mcheck.h>
#include "interfaces/nvme/nvmeibt_lib_udev_api.h"
#include <time.h>
#include "nvmeibt_read_config.h"
#include "interfaces/network/network_incs.h"
#include "nvmeibt_raft.h"
#include "nvmeibt_topology.h"
#include "nvmeibt_toma.h"
#include "interfaces/nvme/nvmeibt_udev.h"
#include "nvmeibt_recovery.h"
#include "clnt/nvmeibt_client.h"
#include "clnt/nvmeibt_client_protocol.h"
#include "./interfaces/srvr/nvmeibt_srvr_proc.h"
#include "./interfaces/os/nvmeibt_os_signal.h"
#include "nvmeibt_wq.h"
#include "nvmeibt_seg_active.h"
#include "nvmeibt_local_disk.h"
#include "nvmeibt_local_disk_util.h"
#include "nvmeibt_ds_metadata.h"
#include "nvmeibt_rpc.h"
#include "nvmeibt_global.h"
#include "nvmeibt_kafka.h"

/*
 * Overview:
 * The TOpology MAnager is a process that runs on every server node
 * - Most of the time it waits for a message
 * - An event is one of
 *  - (time based for now) Reread the configuration from the management
 *  - A client msg (segment register/unregister, stale lock, node down?, ?)
 *  - A Server node reports of local_hardware
 *  - Timeout
 *    - Periodic (IB, Raft)
 *    - Non-responsive client/server/leader.
 *      - Leader might lose majority
 *  	- A server may lose its leader
 * - TOMA focuses on disk_segments
 *   - The dirty_bits_state of a disk-segment-owner is kept in the global_topology persistency, and must be carefully managed
 *   - Almost all of the communication with the client is disk_segment based
 *
 * State and messaging - Design principles
 * - Every TOMA is only aware of its clients and current leader
 * - The leader is unaware of the other TOMA's clients. The leader
 *   is not even aware of any client-related activity.
 * - Every TOMA is responsible for its own clients' state
 * - The TOMA state is stored and communicated in two places
 *   - The Leader's topology (Every TOMA keeps a copy of it)
 *     - It is distributed using Raft's APPEND_ENTRIES
 *   - The TOMA's applied state
 *     - It is sent to the Leader on every APPEND_ENTRIES_REP
 * - Since this state is "as stateless as possible", it should survive crashes
 *   of the current leader and any other TOMA node.
 * - A TOMA that runs recovery
 *   - is an owner of a local disk_segment's blksets
 *   - As an owner, it is already in sync with all the other owners (dirty-bits, locks, data).
 *   - It recovers all the praid's under_recovery segments.
 *   - It will clean stale locks, and copy the blksets (and state) that it owns
 *     - Stale-locks-recovery: to all the active segments (owner/not).
 *     - Dirty-bits-recovery: to all the under_recovery segments (not owners).
 * - The locks recovery is not communicated to the leader.
 * - The dirty_bits recovery is controlled by (and communicated to) the leader,
 *   - Once ALL the active RAID(1/5/6)'s segment's owners TOMAs are done with a
 *     dirty_bits recovery, the leader will calc and distribute a new topology
 *     where the ownership is distributed among all the active segments.
 *
 *  dirty_bits_states of a disk_segment-owner
 *  - Starts as UNKNOWN all over
 *  - After reading the hardware, transition from UNKNOWN to ALIVE_<STABLE/UNSTABLE> (applied (locally)).
 *  - The leader receives LOCAL_DISKS reports from all TOMAs
 *    - The leader learns which disks are alive and on which node
 *  - Just before the leader decides to enable a PRAID (either all segments are alive or by timeout)
 *    it parses the old topology (was read at startup), calculates a new topology and distributes it
 *    - Read the old state from the global_topology_persistency. The leader starts from there.
 *      - It is mandatory to start where the persistency left off
 *  	  - The owners at the time of shutdown/crash must be restored at startup
 *  	  - If all the segments of a PRAID were recovered, and the shutdown was clean,
 *  	    then all the dirty bits should be off
 *  	    - Otherwise, all the dirty bits are set (since we do not save the dirty bits themselves)
 *      - The state of segments that were read from the topology is not NEW_ anymore
 *  - When saving the disk_segment's shutdown state, all the segments are marked as unstable
 *    - Once we have a clean shutdown, segments might be persisted as stable (no stale locks, dirty_bits map, was owner)
 *  - Any local TOMA can transition to ALIVE_<STABLE/UNSTABLE> or delete a local segment (upon hardware changes)
 *  - The TOMA owner (that recovers others) can transition to owner_recovery_done
 *  - The leader can transition to under_recovery/owner_recoverer/owner_idle
 *  - The leader transitions all the owners of a PRAID to Recoverer, the owners will start recovery
 *    Recoverer + new version --> restart the recovery.
 *    - Better, only if writing to new (revived segments)
 *    - Better, continue, until completing a full cycle with the latest praid_version
 *  - At startup
 *    - If there is no persistency (First time ever).
 *      - All active nodes are OWNER_IDLE. self_owned.
 *    - If dead, then owner is always one of the nodes with OWNER_*
 *  	- If no node with OWNER_*, then "Houston, we have a problem"
 *    - When the leader sees that all active owners finished RECOVERING,
 *      all active nodes (incl. UNDER_RECOVERY_R) are marked as OWNER_IDLE
 *      - Note that on that moment, both their data and their dirty_bits are in sync
 *
 *  In case of an event
 *  - The TOMA uses RAFT to select a king
 *  - The followers report about their drives
 *  - The TOMA learns about the active nodes, drives, disk_segments, raids, volumes, and calculates a topology
 *  - The topology is distributed to all the other nodes
 *  - The clients that previously attached to the changed disk_segments need to unregister
 *    - Otherwise, they are brute-force-disconnected
 *  Back to work
 *  - Clients get the new topology, and verify that all the PRAID's segments are registered with their latest version
 *
 * Introduction to locks & recovery
 *   - Dirty-bit (per blkset)
 *     - Used for efficient recovery from a disk_segment temporary disconnect
 *     - Whenever a data is written by the client to a PRAID with missing segments,
 *  	 the client turns on the dirty_bit (owner + active),
 *  	 and leaves the dirty-bit turned-on, also after the write is done and the lock is released
 *     - It is a piggyback on the write-IO, and should take place right after the ringing the disk's doorbell
 *     - Note that a single dirty bit is insufficient for perfect optimization,
 *  	 in case of multiple segments failure support.
 *  	 - When recovering a segment of a 3-way mirror, while another segment is down, the dirty
 *  	   bit should not be cleared!
 *     - Recovery: Every TOMA scans its (owner) dirty-bits, copies each dirty blkset to all
 *       the active segments (owner/not).
 *     - Clients are not affected by the old value of the dirty_bit. They do affect the new value, after writing.
 *   - Client's reg_lock_id
 *     - The client registers with the active segment's server's TOMA, and gets a segment's reg_lock_id
 *  	 - Register once per disk_segment, even if it owns several segments (replicas).
 *     - NOTE: A client needs to send a REGISTER msg to every disk-segment on every live disk (IB-discovery wise),
 *  	 be it on startup, or if it comes back to life in the middle of work. This is unrelated to the
 *       TOMA-provided topology state.
 *     - Some reg_lock_id can become stale (when clients disconnect)
 *     - Every segment owner (TOMA), runs a recovery of the stale locks that it owns. No Raft leader is involved
 *   - Active-list
 *     - Per client&disk-admin-QP (excluding recovery registrants)
 *     - Currently 160 entries
 *     - The client manages its free-list in the client.
 *     - The client only writes to the active-list
 *  	 - Optimization: Avoid the write to clear an empty entry. It will generate extra work upon
 *  	   stale locks reconstruction
 *     - The value written is the disk's blkno of the beginning of a blkset + length (8 + 8 bytes)
 *     - Ralating the blkno to a blkset is calculated using the segment's beginning and blkset-size
 *     - Naturally, a client has to be registered in order to write to the active-list, although it has no use for the seg_lock_id
 * There are two kinds of recovery
 *   - dirty_bits_recovery
 *   - locks_recovery
 * The owner of a disk_segment locks
 *   - Basically speaking, the owner is adjacent to the segment (in the segment's server locks memory)
 *     - It is moved to a different segment (in run time) when the segment goes down, or is missing
 *     - It is moved back when all the (relevant) TOMAs are done with the recovery-pass
 * Recovery process (dirty/locks):
 *  - Assumptions
 *    - All the owners are in synch dirty-bits (and their data) wise. Only stale locks require owners-sync.
 *      If the dirty bits are not in sync, it is either due to a stale lock (or due to active client locks).
 *    - The recoverer (TOMA) might fail, and leave a work in the middle (incl. its stale locks)
 *    - It works in parallel to standard clients.
 *    - After the recovery, the (blkset) data is identical, dirty bits are in sync, and the locks are free of stale values
 *    - If a dirty_bit/lock is not present in all owners it will be cleared all over.
 *  - When recovering a stale lock, the blkset owner recovers all the other segments
 *  - When recovering a dirty-bit, the blkset owner recovers only the under_recovery serments
 *  - Stale locks recovery is run per TOMA, unknowing what the status of the other TOMAs is.
 *    - It is triggered by a client unregister (voluntary or forced)
 *      - Client Unregister
 *        - No future locks/unlocks/data-IO
 *        - If before/after the data-IO phase, erase all held locks
 *        - Send a list of locks (on this TOMA) that are stale
 *    - An Owner stale lock is recovered
 *    - An active stale lock is reported to the owner.
 *  	- First generate a stale_reg_lock_id in the owner's locks table
 *  	  - This should happen during the unregister, so that the client cannot overide entries
 *  		if it starts working again
 *  	  - The client also assumes that the disk-segment's entries were freed upon unregistration
 *  	  - No need to lock
 *  	- If the owner finds it unlocked, then it sends back a message to erase the stale lock
 *  	- If the owner finds a stale lock then we are done for now (The owner will recover), but we
 *  	  still leave the stale_lock in the owner's (in case the owner crashes)
 *  	- Follow-up on the remaining number of stale_reg_lock_id in active entries
 *  	- Timeout, and resend them to the owner from time to time.
 *  - Runs in a TOMA thread, in userspace (there might be several at the time)
 *  - Each recovery thread registers with the local segment an new reg_lock_id.
 *    - The recovery does not use an active-lock, still it must register, get a reg_lock_id, and mention
 *      the reg_lock_id in its messages
 *  - MAP the local locks memory
 *  - Scan the local locks, and for those requiring recovery. lock atomic, using the reg_lock_id.
 *    - Use IB_LOOPBACK_lock - Since a client (or another TOMA thread) can recover it in parralel
 *  - read a blkset from the /dev/nvme??, send to the other TOMAs (a long msg).
 *    - When recovering a lock, if the remote lock is unlocked, then there is no need to recover the
 *  	data. Just the locks.
 *    - The dirty bit has to be copied to the remote server, so that the dirty bits are synchronized.
 *    - The other TOMA will read the mirror, compare (per blk), and write the modified blks, as needed.
 *  - Release the lock.
 *    - The under_recovery segments, also clean the toma_stale_reg_lock_id from the locks table
 * Dirty bit recovery
 *   - Mechanism
 *     - Every TOMA updates its per_segment dirty_bits_state and PRAID praid_version
 *     - When the recovery pass is completed, it is marked as owner_idle
 *  	 - The recovery pass also updates the dirty_bits on all the peer's segments. At the end of
 *  	   the recovery pass, both the data and the dirty_bits are in sync.
 *     - When the leader marks a new segment as under_recovery,
 *       it increments the praid_version of the praid
 *  	 - When a TOMA sees (in the topology) that its dirty_bits_state is RECOVERER with a new praid_version,
 *  	   its (re)starts a recovery
 *     - When a TOMA sends its APPEND_ENTRIES_REP is also reports of its <segment's dirty_bits_state, praid_version>
 *       This is reported as applied_*, and saved as applied_* in the leader's data structures
 *     - The leader that received all the "recovered" for the current segment_version,
 *       recalc's, and distribute a new topology with owner_idle and the new owners
 *   - Recovering_segment
 *     - Use cases:
 *  	 - When starting following a surprise (not-clean) shutdown and startup of all the disk_segments together
 *  	   - All the dirty bits are set. All the stale_locks too.
 *  	   - Alternatively, save the dirty bits to a file upon shutdown (and restore them)
 *       - A segment is moved from Dead to unstable
 *         - If it was temporarily disconnected, then some dirty bits might have been set
 *  	   - If it is new, then all the dirty bits should be already set
 *   Persistency in the global_topology of the dirty_bits_state
 *   - for every segment keep the last touple of <dirty_bits_state, owner_node>
 *   - idx_of_dirty_bits_recovery_start_point (default=0. Clean shutdown can set it differently)
 * The registration_state of a segment
 *   - Registrable (available for clients registration and use)
 *   - Blocked - Waiting for some clients to unregister. Refusing any registrations
 *   Persistency: None
 * The locks_recovery_state of a disk_segment
 *   - Recovering_locks of (several) stale clients' reg_lock_id
 *     - A "process" is scanning all the locks, recovering those that are not active
 *   Persistency: None.
 *
 *  (client) stale locks_recovery:
 *  - Initialization
 *    - If a segment was shut down cleanly (all clients unregistered, no owner/copy locks, not
 *  	in the middle of stale_locks_recovery), then this STABLE state is written to the disk_segment's
 *      persistency (on the disk_segment's disk)
 *    - If the segment was not shut down properly (UNSTABLE), then at boot time it is communicated to the leader
 *  	- The leader might decide to either
 *  	  1. mark all the owners locks as locked using the toma's toma_stale_reg_lock_id
 *  	  2. (alternatively) not use it as an owner, and run some sort of recovery
 *  	  3. If any of the other copies was shut down cleanly, then there were no copy locks anywhere.
 *  - The Raft leader does not communicate with the clients. It is not even aware of them.
 *  - locks_recovery is private to a specific TOMA. It decides-upon and implements the recovery
 *    of the locks that are owned by this segment. (No other TOMAs are communicated)
 *    - The reason for this is that scanning of the local memory for (a few) stale locks is quick.
 *    - Stale active (secondary) locks can be ignored, since:
 *      - When reading data, they are ignored - only the owner-lock is read in order to verify its release, post read.
 *      - When writing data, it is overridden anyhow.
 *      - Stale-active + released-owner can happen only before/after writing to "both-mirrors",
 *  	  not in the middle.
 *      - So why do we need them?
 *        - In case of owner crash, the new owner will first convert the stale copy locks into owner locks.
 *  - toma_stale_reg_lock_id
 *    - The id 0x00ffffffffff4321 is reserved for toma_stale_reg_lock_id
 *    - When a client encounters the toma_stale_reg_lock_id in the owner's locks, it can
 *  	recover this blkset. Either,
 *  	- write:
 *  	  - lock (cmpxchg) all segments, write on all segments and zero the locks.
 *  	- Read:
 *  	  - read from all copies
 *  	  - if identical - lock (cmpxchg) all segments, clean the value toma_stale_reg_lock_id from the lock
 *  	  - if not identical - lock, reread the owner, write to the others, mark recovered
 *  - The locks-scan-and-recovery process, is yet another client as far as the TOMAs are concerned.
 *    - It has to register on segments, lock locks, and might crash in the middle.
 *  - TOMA and a client communicate on segments
 *    - Whenever a segment is unregistered/disconnected. It is assumed that its protection
 *  	RAID(1/5/6), (but not RAID0/(RAID10-striping)), is paused and the client
 *  	needs to re-registed on this segment (often resulting in reregistration on all segments)
 *    - The client will send a unregister-segment req
 *  	- TOMA will recover the client's segment's locks
 *    - A client can send a stale-lock message (a lock is busy for too long)
 *  	- TOMA might decide that it is a stale lock, and replace it with a toma_stale_reg_lock_id
 *  	  - Now anybody can recover it.
 *  RAID(1/5/6) degraded mode (A segment stopped working)
 *  - Either a server notifies its TOMA that a disk is down, or the Raft leader lost a server for too long.
 *    - Timeouts wise, first the problematic server stops working, and later the TOMA leader decides to drop it.
 *  - The Raft leader sends a new topology (&topology_version) to all of this "PRAID"'s TOMAs
 *    - These TOMAs send a need_to_unregister_segment (+ new topology) to all the clients
 *    - The clients, pause the PRAID, and send unregister-segment for all the changed segments.
 *    - After all the clients unregistered (or brutally disconnected) from a segment, TOMA is willing to accept registrations
 *  	on the said segment with the new praid_version
 *    - A client can work with a PRAID only if at least one of its segments was registered with the latest
 *  	praid_version.
 *      - A disk_segment will only accept registrations with its_praid's current (latest) praid_version.
 *
 * The topology - content & update
 * - Introduction to the configuration
 *   - The config (from the mgmt.), provides the following "hard" configuration
 *     - The nodes, and their NICs
 *     - The Disk, and their disk_segments
 *     - The volumes (block_devices), made of a concatenation of chunks
 *       - A chunk is currently a RAID0 (or RAID10)
 *     - The chunks, and how they are made of disk_segments
 *   - Every TOMA node reports of its disks to the TOMA leader, and it builds the relationships.
 * - The topology_version
 *   - A 64 bits number that is made of a concatenation of
 *     - (MSB 32-bits) Raft's current_term of the leader that calculates the new topology_version
 *     - (LSB 32-bits) increased upon every topology calculation
 *   - When selecting a new Raft leader, only the nodes that have the highest topology_version qualify
 * - The global_topology content - MUST BE PERSISTENT
 *   - For every disk_segment
 *     - node, owner_segment_id (read_from & owner_lock)
 * - Update
 *   - The leader sends the global topology to all servers, and the receiving TOMA
 *     - Saves the topology in persistency (get ready to become the leader)
 *     - Stop (unregister) all the affected segment's clients, and sends them an updated topology
 *     - Run dirty-bits recovery on segments
 *   - A TOMA sends to the leader
 *     - Its hardware (disks (& nics))
 *     - The dirty_bits recovery state of its disk_segments-owner
 *   - The leader gathers the segment's recovery state, and makes changes to the topology (PRAID state)
 * - When a new configuration arrives
 *   - It is read into cur_topo
 *   - The relationships between the entities are re-calculated
 *   - The topology from persistency is re-applied to cur_topo
 *
 * Bootstrap logic
 * - Every TOMA reads its disk_segment's dirty_bits_values and metadata, and sets the segment's
 *   state to NEW_STABLE or NEW_UNSTABLE, and sends to the leader (incl. praid_version)
 * - Leader decides to activate the PRAID
 *   - Parses the global_topology
 *   - Note that old owners with NEW_* are expected to match the praid_version (otherwise
 *     they wouldn't be old owners)
 *   - Old owners, that are NEW_STABLE qualify immediatelly
 *     - If one exists, then all the locks of all the segments in the PRAID start from clean
 *       - Otherwise, all are marked as toma_stale_reg_lock_id
 *     - If one exists, then its dirty bits map is copied to all the owners as is, before the
 *  	 segments are enabled
 *       - Otherwise, all the dirty bits are set
 *   - Old owners, that are NEW_* become owners
 *   - Run recovery
 *     - If stale locks were turned-on or non-owner active segments exist then
 *  	 run a cycle of recovery on all the owners
 *     - Recovery of a blkset step
 *       - If (is_stale_lock_found || (is_recovering_dirty_bits && is_dirty_bit_found)) then
 *  		 recover it
 *
 * Owners' re-shuffle upon topology change
 * - The procedure implies that the clients unregister and reregister on a changed PRAID's disk_segment
 *   using the new praid_version
 * - Only the current owners and fully-recoverd segments (dirty_bits wise) can become owners.
 * - This implies that all the segments are in-sync data-wise (dirty_bits too).
 * - What if a client crashed, left stale locks and non_sync data
 *   - The client left locks on the owner and all the actives (incl. the under_recovery ones),
 *     so there is no read consistency issue
 *   - If another copy is recovered, then the stale lock is not present there --- But, the dirty_bits_recovery
 *  	 recovered the data (actually, it also recovered the stale_lock)
 *
 * Switch-topology & Clients synchronization
 * - For every praid_version, one (or none) of the owners is nominated as "synchronizer".
 * - The role of the synchronizer is to report when all of its registered clients stopped
 *   using the previous praid_version
 * - The synchronizer aborts the synchronization upon reception of a new praid_version
 * - Transition of a disk-segment to dead is easy, since the clients are stack.
 *   - A new topology is calculated, and the synchronizer segment tells all clients to switch_praid_topology
 *   - No need to wait for all clients
 * - Transition from dead to under_recovery
 *   - The leader changes the state to under_recovery, with a nominated synchronizer
 *     - As long as there exists a synchronizer, the owners do not start the recovery
 *   - The synchronizer notifies its registered clients to switch_praid_topology (to write on the under_recovery)
 *   - Once The leader receives the <synchronizer_done, praid_version>, it
 *     - Increases the praid_varion, and removes the synchronizer
 *     - By doing so, the owners can start the recovery
 * - Transition from under_recovery to owner
 *   - The leader nominates two owners for a disk segment
 *   - The leader nominates a synchronizer
 *   - The leader sends the new topology
 *   - Once the synchronizer is done, and notifies the leader, the leader will switch to the
 *     new owner
 * Dirty-bits and stale-locks initialization
 * - The leader decides how to initialize the dirty-bits and stale-locks
 *   - E.g., turn all on/off, read from persistency, copy from another TOMA (segment)
 * - The receiving TOMA does the following
 *   - If new applied praid_version, and not open for clients registeration
 *     then perform the initialization
 *   - During the initialization, clients cannot register
 *   - When done change the applied init-mode to INIT_DONE
 * - Live-cycle:
 *   - Upon activation, the leader gives all segments an init-mode
 *   - All Segments initialize, switch to INIT_DONE, and in-turn, the leader too.
 *   - A segment that comes back to life after being dead is also initialized.
*/

#include <sys/time.h>
#include <sys/socket.h>

static bool						toma_is_running_as_a_utility = 0;

/* used to wakeup TOMA from timeout in select() */
static pthread_mutex_t toma_wakeup_mutex;
static int toma_wakeup_pipe[2];
/* used to track pending TOMA wakeups (with ptr==NULL) to avoid
   accumulation of repetitive wakeup notices - only hold one. */
static pthread_mutex_t toma_wakeup_pending_mutex;
static bool toma_wakeup_pending_by_type[NVMEIBT_TOMA_WAKEUP_TYPE_LAST];

static int						epoll_fd = -1;
static struct epoll_event		epoll_events[10000];

enum shutdown_state {
	ds_none,
	ds_me,
	ds_all,
};
static enum shutdown_state shutdown_status = ds_none;
static int shutdown_from_management;

static struct nvmeibt_wq *toma_persistency_wq;
static struct nvmeibt_wq *leader_wq;
static struct nvmeibt_wq *stat_wq;
static struct nvmeibt_wq *recoveries_progress_wq;
static struct nvmeibt_wq *read_disk_from_smart_wq;

static pthread_t toma_main_thread;
//static char executables_dir[PATH_MAX];
static char executable_name[NAME_MAX];	// used by prints to syslog
static char toma_log_dir_name[PATH_MAX];

char toma_cfg_id[PATH_MAX] = "Unknown-Config-Id";
char toma_cfg_name[PATH_MAX] = "Unknown-Config-Name";
char toma_cfg_version[PATH_MAX] = "Unknown-Config-Version";
//
static char toma_nm_transport_lib_path[PATH_MAX];
static int toma_log_file_n = 2;					// Was default value for at least 9 years.
#ifndef FILE_SIZE_BITS
	#define FILE_SIZE_BITS 30					/* 1[GB] default log size unless specified otherwise in make file */
#endif
static long long toma_log_file_size = (1UL << FILE_SIZE_BITS);
static unsigned int toma_bin_log_file_n = 40;
static unsigned int toma_bin_log_file_size_mega = 48;
//
char		nvmeibt_toma_cmdline_arg_input_file_name[PATH_MAX] = "";
char		nvmeibt_toma_cmdline_arg_output_file_name[PATH_MAX] = "";
bool		nvmeibt_is_converting_json_to_persistence = 0;
bool		nvmeibt_is_converting_persistence_to_json = 0;

static int signals_fd = -1;
bool toma_abort_nf = false;
static bool is_need_to_update_the_main_select_fds = true;
int64_t nvmeibt_toma_is_not_reporting_data_segs_gpt_entries = TOMA_DO_NOT_REPORT_DISK_SEGMENT_GPTS_DEFAULT;

/* Times for Status */
static struct timespec last_ib_event_timespec;
struct timespec last_client_event_timespec, last_local_srv_event_timespec;
static struct timespec last_toma_wakeup_event_timespec;
static struct timespec last_wq_event_timespec;
//
static struct timespec last_raft_timeout_timespec;
static struct timespec last_register_timeout_timespec;
static struct timespec last_recovery_timeout_timespec;

bool nvmeibt_use_libibcm = false;
bool nvmeibt_ib_use_srq = false;
static struct nvmeibt_nm_local_node *nw_node = NULL;
char tracing_cgroup[NAME_MAX] = "";

struct udev_event_wq_entry {
	struct nvmeibt_wq_entry 			wq_entry;
	struct nvmeibt_udev_event_info		*udev_event_info;
	char 								op;
	struct nvmeibt_ascii_uuid			ldisk_id;
	struct nvmeibt_ascii_uuid			native_serial;
	int									nsid;
	u32									vendor_id;
};

#if defined(COMPILE_DEBUG)
#	define MOD_STR "debug"
#elif defined(COMPILE_RELEASE)
#	define MOD_STR "release"
#else
#	define MOD_STR "???"
#endif

int64_t nvmeibt_toma_report_target_min_between_secs = REPORT_TARGET_MIN_BETWEEN_SECS_DEFAULT;

bool nvmeibt_toma_is_running_as_a_utility(void)
{
	return toma_is_running_as_a_utility;
}

static int is_mgmt_updates_paused = 0;

struct stat_wq_entry {
	struct nvmeibt_wq_entry 	wq_entry;
	char 						status_filename[PATH_MAX];
	struct nvmeibt_Str			*status_str;
};

enum NVMEIBT_FD_TYPES {
	NVMEIBT_TOMA_FD_TYPE_IB = 1,
	NVMEIBT_TOMA_FD_TYPE_ROCE = 3,
	NVMEIBT_TOMA_FD_TYPE_TOMA_WAKEUP = 4,
	NVMEIBT_TOMA_FD_TYPE_TOMA_SRM_RESEND_TIMER = 7,
	NVMEIBT_TOMA_FD_TYPE_FIFO_COMM = 8,
	NVMEIBT_TOMA_FD_TYPE_SYSTEM_EVENTS = 15,
	NVMEIBT_TOMA_FD_TYPE_UDEV_EVENTS = 16,
	NVMEIBT_TOMA_FD_TYPE_UDP = 17,
	NVMEIBT_TOMA_FD_TYPE_UDP_TIMER = 18,
};

static char *fd_type_str(enum NVMEIBT_FD_TYPES t)
{
	switch (t) {
	case NVMEIBT_TOMA_FD_TYPE_IB: return "IB";
	case NVMEIBT_TOMA_FD_TYPE_ROCE: return "ROCE";
	case NVMEIBT_TOMA_FD_TYPE_TOMA_WAKEUP: return "WAKEUP";
	case NVMEIBT_TOMA_FD_TYPE_TOMA_SRM_RESEND_TIMER: return "SRM";
	case NVMEIBT_TOMA_FD_TYPE_FIFO_COMM: return "FIFO";
	case NVMEIBT_TOMA_FD_TYPE_SYSTEM_EVENTS: return "SYSTEM";
	case NVMEIBT_TOMA_FD_TYPE_UDEV_EVENTS: return "UDEV";
	case NVMEIBT_TOMA_FD_TYPE_UDP: return "UDP";
	case NVMEIBT_TOMA_FD_TYPE_UDP_TIMER: return "UDP_TIMER";
	default : {
		static char	unexpected_val_str[] = "unknown               ";
		sprintf(unexpected_val_str, "unknown(%x)", t);
		return unexpected_val_str;
	}
	}
}

static struct nvmeibt_toma_fds_in_use {
	struct nvmeibt_toma_fd_in_use {
		int						fd;
		enum NVMEIBT_FD_TYPES	fd_type;
	} fds_arr[1024];
	int								n_fds_in_use;
	int								max_fd_no;
} g_fds_in_use;

struct nvmeibt_nm_local_node * nvmeibt_get_nw_node(void)
{
	return nw_node;
}

static void udev_event_wrapper(struct nvmeibt_wq_entry *wq_entry)
{
	NFIN;

	nvmeibt_toma_trigger_wakeup(NVMEIBT_TOMA_WAKEUP_TYPE_WQ, (void *) wq_entry);
	NFOUT;
}

static void udev_event_freer(struct nvmeibt_wq_entry *wq_entry)
{
	struct udev_event_wq_entry *entry;

	NFIN;

	entry = container_of(wq_entry, struct udev_event_wq_entry, wq_entry);
	NNVMEIBT_BM_FREE(trace_toma_udev_event_freer, entry);

	NFOUT;
}

void nvmeibt_toma_mark_is_need_to_update_the_main_select_fds(void)
{
	is_need_to_update_the_main_select_fds = 1;
}

const char *nvmeibt_toma_get_log_dir_name(void)
{
	return (toma_log_dir_name[0] == '\0') ? TOMA_LOG_DIR : toma_log_dir_name;
}

int nvmeibt_toma_get_n_log_file(void)
{
	return toma_log_file_n;
}

long long nvmeibt_toma_get_log_file_max_size(void)
{
	return toma_log_file_size;
}

unsigned int nvmeibt_toma_get_bin_log_file_n(void)
{
	return toma_bin_log_file_n;
}

unsigned int nvmeibt_toma_get_bin_log_file_size(void)
{
	return toma_bin_log_file_size_mega;
}

void nvmeibt_toma_set_main_thread(void)
{
	toma_main_thread = pthread_self();
}

bool nvmeibt_toma_is_main_thread(void)
{
	return pthread_self() == toma_main_thread;
}

static void self_inflicted_death_on_error(void)
{
	pthread_kill(toma_main_thread, SIGTERM);
}

static void free_toma_wakeup(void);
static int toma_wakeup_event(void);

bool is_shutdown_me_only(void)
{
	return shutdown_status == ds_me;
}

bool nvmeibt_toma_is_in_shutdown(void)
{
	return ((shutdown_status != ds_none) || nvmeibt_raft_is_shutdown_triggered());
}

int nvmeibt_send_msg_to_srv(struct km_comm_msg_hdr *msg)
{
	return nvmeib_srvr_api_lib_send_async_msg_to_server(nvmeibt_get_srv_comm(), msg);
}

static const char single_instance_file[] = TOMA_DIR_RUN_NVMESH "/toma.lock";
static int single_instance_fd;
int nvmeibt_toma_is_single_instance(void)
{
	if (nvmeibt_recursive_mkdir_for_path(single_instance_file, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH)) {
		return -1;
	}
	single_instance_fd = NNVMEIBT_OPEN(trace_toma_single_instance, single_instance_file, O_CREAT | O_RDWR, 0666);
	if (single_instance_fd == -1) {
		N_Ef(trace_1_toma_single_instance, "open ('@SINGLE_INSTANCE_FILE') failed, @AUTO_ERRNO", single_instance_file);
		return -1;
	}
	return flock(single_instance_fd, LOCK_EX | LOCK_NB) < 0 ? -1 : 0;
}
void nvmeibt_toma_cleanup_single_instance(void)
{
	/* Unlock and remove single-instance lock file */
	flock(single_instance_fd, LOCK_UN);
	NNVMEIBT_CLOSE(trace_toma_cleanup_single_instance, single_instance_fd);
	NNVMEIBT_UNLINK(trace_1_toma_cleanup_single_instance, single_instance_file, 0); // Does not have equivalent LINK() because open() created the file and linked it
}

static void terminate_toma(int rv)
{
	struct nvmeibt_wq		*wq;

	NFIN;
	// Once we get here, there is no way back, and naturally, no work in TOMA's main loop (the events handler)

	//nvmeibt_ib_close_all_listeners();
	nvmeibt_nm_done(nw_node);
	nvmeibt_udev_destroy();

	// Must drain all WQ to avoid having any threads in the air that might try to access the mem_tbls of some disk,
	// as at this point of shutdown we cannot in any way wait for the threads signaling that they finished their
	// work (and decreased the used counter of each memtbl), since we are outside the event loop already.

	NVMEIB_HASH_FOREACH(wq, nvmeibt_global_get_global()->ldisks_wq_hash_by_ldisk_id_str) {
		nvmeibt_wq_drain(wq);
		nvmeibt_wq_destroy(wq);
	}
	nvmeib_hash_tbl_free(nvmeibt_global_get_global()->ldisks_wq_hash_by_ldisk_id_str);

	nvmeibt_wq_drain(read_disk_from_smart_wq);
	nvmeibt_wq_destroy(read_disk_from_smart_wq);
	read_disk_from_smart_wq = NULL;

	nvmeibt_wq_drain(toma_persistency_wq);
	nvmeibt_wq_destroy(toma_persistency_wq);
	toma_persistency_wq = NULL;

	nvmeibt_wq_drain(recoveries_progress_wq);
	nvmeibt_wq_destroy(recoveries_progress_wq);
	recoveries_progress_wq = NULL;

	nvmeibt_wq_drain(leader_wq);
	nvmeibt_wq_destroy(leader_wq);
	leader_wq = NULL;

	nvmeibt_recovery_exit();

	nvmeibt_dumper_exit();
	nvmeibt_rpc_terminate();
	nvmeib_srvr_api_lib_server__detach(nvmeibt_get_srv_comm());	// Stop receiving msgs from server

	if (nvmeibt_global_get_global()) {				// Close other resources, like lock maps
		nvmeibt_local_disk_free_all_resources();
		nvmeibt_local_disk_free_stock_fds();
	}

	free_toma_wakeup();
	nvmeibt_topology_free_resources();
	nvmeibt_server_lib_destroy();
	nvmeibt_toma_cleanup_single_instance();

	nvmeibt_wq_drain(stat_wq);
	nvmeibt_wq_destroy(stat_wq);
	stat_wq = NULL;

	NFOUT;

	nvmeibt_toma_abort_child_processes();	// Here we wait for trace pollers as well. From this point no binary traces prints!
	nvmeibt_close_all_nonstd_fds(1);  /* be verbose */

	if (shutdown_from_management || nvmeibt_raft_is_shutdown_triggered()) {
		// YR: move from execlp to a system and background process and regular exit due to systemd tracking forks
		const int srvr_rv = system("service nvmeshtarget stop &"); // Try to take server down as well.
		if (srvr_rv)
			syslog(LOG_INFO, "TOMA attempt to stop nvmesh target failed, rv=%d\n", rv);
	}
	nvmeib_hash_free_all_tables();

	syslog(LOG_INFO, "TOMA Exit\n");
	fprintf(stderr, "TOMA Exit\n");
	exit(rv);
}

#define MAX_WAIT_RECOVERY_SEC		5
#define MAX_WAIT_KAFKA_SEC			30
#define MAX_WAIT_UNREGISTER_SEC		6
static void attempt_stable_local_shutdown(void)
{
	static int	start_time_sec = 0;
	static BOOL	are_disks_detached = 0;
	static BOOL	is_store_segments_metadata_on_shutdown_launched = 0;
	static BOOL	is_close_all_seg_actives_for_registration_launched = 0;
	struct nvmeibt_node * node;
	NFIN;

	if (!start_time_sec) {
		start_time_sec = nvmeibt_global_get_cur_event_start_time().tv_sec;
	}

	if (!nvmeibt_kafka_is_kafka_done_shutdown()) {
		if (nvmeibt_global_get_cur_event_start_time().tv_sec - start_time_sec > MAX_WAIT_KAFKA_SEC) {
			N_Wf(cv2irmk, "kafka shutdown failed");
		} else {
			N_Tf(cvaormk, "Awaiting kafka shutdown");
			goto out;
		}
	}

	if (!is_close_all_seg_actives_for_registration_launched) {
		nvmeibt_register_close_all_seg_actives_for_registration();
		is_close_all_seg_actives_for_registration_launched = 1;
	}
	if (nvmeibt_register_is_any_registered()) {
		if (nvmeibt_global_get_cur_event_start_time().tv_sec - start_time_sec > MAX_WAIT_UNREGISTER_SEC) {
			N_Wf(gy7n812, "Clients unregister failed");
		} else {
			N_Tf(gy77612, "Awaiting clients unregister");
			goto out;
		}
	}

	/* If we are coming from a graceful shutdown, we can be more patient about waiting for recoveries to finish */
	if (	!are_disks_detached &&
			(nvmeibt_global_get_cur_event_start_time().tv_sec - start_time_sec > MAX_WAIT_RECOVERY_SEC)) {
		NVMEIB_HASH_FOREACH(node, nvmeibt_global_get_global()->nodes_hash_by_uuid) {
			nvmeibt_topology_detach_all_disks_from_node(node);	TODO(Needed?);
		}
		are_disks_detached = 1;
	}

	if (	((shutdown_status != ds_all || nvmeibt_raft_is_raft_shutdownable_now()) &&
			 !nvmeibt_register_is_any_registered())) {
		if (!is_store_segments_metadata_on_shutdown_launched) {
			nvmeibt_seg_active_launch_store_of_all_seg_actives_metadata();
			is_store_segments_metadata_on_shutdown_launched = 1;
		}
		if (!nvmeibt_seg_active_is_any_seg_active_during_metadata_store()) {
			nvmeibt_global_issue_leader_report_praids_status_to_mgmt();
			terminate_toma(0);
		}
	}

out:
	NFOUT;
	return;
}

static void nvmeibt_toma_print_status(void);

int nvmeibt_toma_on_shutdown_me_only(__attribute__((__unused__)) void *args, bool from_mgmt)
{
	NFIN;
	shutdown_from_management += !!from_mgmt;
	N_Tf(trace_toma_on_shutdown_me_only, "shutdown_from_management=@SHUTDOWN_FROM_MANAGEMENT", shutdown_from_management);
	if (shutdown_status == ds_none) {
		getnstimeofday_boot(&nvmeibt_global_get_global()->shutdown_start_time);
		shutdown_status = ds_me;
		nvmeibt_kafka_shutdown();
		nvmeibt_toma_print_status();
		nvmeibt_topology_applied_mark_shutdown_start();
	}
	NFOUT;
	return 0;
}

int nvmeibt_toma_on_shutdown_all(__attribute__((__unused__)) void *args, bool from_mgmt)
{
	NFIN;
	shutdown_from_management += !!from_mgmt;
	N_Tf(trace_toma_on_shutdown_all, "shutdown_from_management=@SHUTDOWN_FROM_MANAGEMENT", shutdown_from_management);
	if (shutdown_status == ds_none) {
		getnstimeofday_boot(&nvmeibt_global_get_global()->shutdown_start_time);
		shutdown_status = ds_all;
		nvmeibt_kafka_shutdown();
		nvmeibt_toma_print_status();
	}
	NFOUT;
//	exit(-1);
	return 0;
}

/*
 * toma_wakeup:
 *
 * TOMA main thread's loop calls select() with timeout in wait for activity in
 * file descriptors. Some events, not directly lined to a file descriptor, may
 * need to interrupt this wait. For that, we use toma_wakeup_pipe() as another
 * file descriptor to get TOMA's attention.
 *
 * In a wakeup event, we send the type followed by a pointer value. TOMA main
 * thread will read the type, and dispatch the suitable callback with the given
 * pointer value as arg.
 *
 * There are currently two users:
 *   NVMEIBT_TOMA_WAKEUP_TYPE_WQ
 */

static void check_if_binding_to_stock_needed(struct nvmeibt_udev_event_info *udev_event_info);

static void udev_event_finalize(struct nvmeibt_wq_entry *wq_entry)
{
	struct udev_event_wq_entry *entry;
	struct nvmeibt_local_disk *local_disk = NULL;

	NFIN;

	entry = container_of(wq_entry, struct udev_event_wq_entry, wq_entry);

	if (wq_entry->is_canceled) {
		// Nothing in this case
		// OL: WRONG, FIX THIS?
	}

	// We need to raise the stock_local_disk version, since we know that all those who were executing on
	// the old active version are done. We raise the active version now so that all new executions
	// start with the next active version.
	local_disk = nvmeibt_local_disk_get_local_disk_by_ldisk_id(&entry->ldisk_id, nvmeibt_global_get_global()->stock_local_disks_hash_by_ldisk_id_str);
	if (local_disk) {
		// If the disk is to be added by this hardware event, then the versioning is taken care of by local_disk_add.
		// This part only takes care of some hardware event which happens on an existing disk.
		local_disk->CHANGE_EVENT_counters.active_zeroing_CHANGE_no = local_disk->CHANGE_EVENT_counters.last_CHANGE_no;
	}

	N_IMf(info_toma_udev_event_finalize,
		  "Handling udev on " LOCAL_DISK_LOG_FMT " op=@OP_CHR", LOCAL_DISK_LOG_obj_ARGS(entry), entry->op);

	if (entry->op == 'r') {
		N_Tf(trace_toma_udev_event_finalize, "Removing stock local disk from path=@PATH", entry->udev_event_info->dev_file_name);
		nvmeibt_local_disk_remove_stock_local_disk_by_dev_file_name(entry->udev_event_info->dev_file_name);
		NVMEIBT_GLOBAL_MARK_REPORT_TARGET_HAS_NEW_DATA(2ncjsie3);
	} else {
		N_Ef(trace_1_toma_udev_event_finalize, "Unsupported udev_event. " LOCAL_DISK_LOG_FMT " op=@OP_CHR", LOCAL_DISK_LOG_obj_ARGS(entry), entry->op);
		nvmeibt_abort(ES_FATAL);
	}

	check_if_binding_to_stock_needed(entry->udev_event_info);

	NFOUT;
}

static void wq_entry_free_after_wakeup(struct nvmeibt_wq_entry *wq_entry) {
	/* finalize() called even if canceled: should test and handle */
	if (wq_entry->finalize)
		wq_entry->finalize(wq_entry);
	if (wq_entry->free)
		wq_entry->free(wq_entry);
}

/* toma_wakeup: callback for WQ */
static void toma_wakeup_wq(void *ptr)
{
	struct nvmeibt_wq_entry *wq_entry = ptr;

	NFIN;

	while (wq_entry->chained)
		wq_entry = wq_entry->chained;		// Actually does only 1 loop for once_wq_entry
	N_Tf(ttwuwq0, "Handling event type=@TYPE_STR is_cancel=@IS_CANCEL", wq_entry->type, wq_entry->is_canceled);
	last_wq_event_timespec = nvmeibt_global_get_cur_event_start_time();
	wq_entry_free_after_wakeup(wq_entry);
	NFOUT;
}

void nvmeibt_toma_wakeup_wq_abort_func(struct nvmeibt_wq_entry *wq_entry)
{
	nvmeibt_toma_trigger_wakeup(NVMEIBT_TOMA_WAKEUP_TYPE_WQ, (void *) wq_entry);
}

static int init_toma_wakeup(void)
{
	int ret = -1;

	NFIN;

	toma_wakeup_pipe[0] = -1;
	toma_wakeup_pipe[1] = -1;

	if (pipe(toma_wakeup_pipe) < 0) {
		N_Ef(trace_toma_init_toma_wakeup, "Failed to create toma wakeup pipe (@AUTO_ERRNO)");
		goto out;
	}
    // error checking for fcntl
    if (fcntl(toma_wakeup_pipe[0], F_SETFL, O_NONBLOCK) < 0) {
		N_Ef(ysbi324, "Failed fcntl(toma_wakeup_pipe[0], F_SETFL, O_NONBLOCK) (@AUTO_ERRNO)");
		goto out;
	}

    if (fcntl(toma_wakeup_pipe[0], F_SETPIPE_SZ, 64*1024*1024) < 0) {
		N_Wf(5vhsd89, "toma_wakeup_pipe[0], F_SETPIPE_SZ, 64*1024*1024 (@AUTO_ERRNO)");
		if (fcntl(toma_wakeup_pipe[0], F_SETPIPE_SZ, 1*1024*1024) < 0) {
			N_Wf(tvsh8i3, "toma_wakeup_pipe[0], F_SETPIPE_SZ, 1*1024*1024 (@AUTO_ERRNO)");
		}
	}

    if (fcntl(toma_wakeup_pipe[1], F_SETPIPE_SZ, 64*1024*1024) < 0) {
		N_Wf(vrghs8j, "fcntl(toma_wakeup_pipe[1], F_SETPIPE_SZ, 64*1024*1024 (@AUTO_ERRNO)");
		if (fcntl(toma_wakeup_pipe[1], F_SETPIPE_SZ, 1*1024*1024) < 0) {
			N_Wf(6vhdk03, "fcntl(toma_wakeup_pipe[1], F_SETPIPE_SZ, 1*1024*1024 (@AUTO_ERRNO)");
		}
	}

	if (pthread_mutex_init(&toma_wakeup_mutex, NULL) < 0) {
		N_Ef(trace_1_toma_init_toma_wakeup, "Failed to create toma wakeup mutex (@AUTO_ERRNO)");
		goto out;
	}

	if (pthread_mutex_init(&toma_wakeup_pending_mutex, NULL) < 0) {
		N_Ef(trace_2_toma_init_toma_wakeup, "Failed to create toma wakeup pending mutex (@AUTO_ERRNO)");
		goto out;
	}

	ret = 0;

out:
	if (ret < 0) {
		NNVMEIBT_CLOSE(trace_3_toma_init_toma_wakeup, toma_wakeup_pipe[0]);
		NNVMEIBT_CLOSE(trace_4_toma_init_toma_wakeup, toma_wakeup_pipe[1]);
	}

	NFOUT;

	return ret;
}

atomic_t n_entries_in_the_toma_wakeup_pipe;

static void free_toma_wakeup(void)
{
	NFIN;
	if (atomic_read(&n_entries_in_the_toma_wakeup_pipe) != 0)
		toma_wakeup_event();		// Wakup up 1 last time to drain wakeup events and clean memory if needed
	NNVMEIBT_CLOSE(trace_toma_free_toma_wakeup, toma_wakeup_pipe[0]);
	NNVMEIBT_CLOSE(trace_1_toma_free_toma_wakeup, toma_wakeup_pipe[1]);
	pthread_mutex_destroy(&toma_wakeup_mutex);
	NFOUT;
}

static const char *toma_wakeup_type_to_str(enum NVMEIBT_TOMA_WAKEUP_TYPE type)
{
	switch (type) {
	case NVMEIBT_TOMA_WAKEUP_TYPE_IB_SA:		return "WAKEUP_TYPE_IB_SA";
	case NVMEIBT_TOMA_WAKEUP_TYPE_WQ:			return "WAKEUP_TYPE_WQ";
	case NVMEIBT_TOMA_WAKEUP_TYPE_NETLINK:		return "WAKEUP_TYPE_NETLINK";
	case NVMEIBT_TOMA_FD_TYPE_LOCAL_SERVER_EVENTS: return "WAKEUP_TYPE_LOCAL_SERVER";
	case NVMEIBT_TOMA_WAKEUP_TYPE_KAFKA:		return "WAKEUP_TYPE_KAFKA";
	default:
		N_Ef(5vwh3js, "Unknown type=@INT", type);
		nvmeibt_abort(ES_FATAL);
		return NULL;  /* appease compiler */
	}
}

static bool toma_wakeup_test_and_set(enum NVMEIBT_TOMA_WAKEUP_TYPE type, bool val)
{
	bool ret;

	NFIN;

	if (pthread_mutex_lock(&toma_wakeup_pending_mutex) != 0) {
		N_Ef(trace_toma_toma_wakeup_test_and_set, "Failed to lock toma wakeup pending mutex (@AUTO_ERRNO)");
		self_inflicted_death_on_error();
	}

	ret = toma_wakeup_pending_by_type[type];
	toma_wakeup_pending_by_type[type] = val;

	if (pthread_mutex_unlock(&toma_wakeup_pending_mutex) != 0) {
		N_Ef(trace_1_toma_toma_wakeup_test_and_set, "Failed to unlock toma wakeup pending mutex (@AUTO_ERRNO)");
		self_inflicted_death_on_error();
	}

	NFOUT;
	return ret;
}

struct __attribute__((aligned(16))) toma_wakeup_args {
	void	*ptr;
	int		type;
};
_Static_assert(sizeof(struct toma_wakeup_args) == 16, "sizeof(struct toma_wakeup_args) != 16, Not sure this is mandatory");

/* request wakeup of TOMA main thread */
int nvmeibt_toma_trigger_wakeup(enum NVMEIBT_TOMA_WAKEUP_TYPE type, void *ptr)
{
	const struct toma_wakeup_args buf = { .ptr = ptr, .type = type};
	int ret = -1;
	N_Tf(trace_toma_nvmeibt_toma_wakeup, "wakeup request type @TOMA_WAKEUP_TYPE_TO_STR ptr @PTR", toma_wakeup_type_to_str(type), ptr);

	if (pthread_mutex_lock(&toma_wakeup_mutex) != 0) {
		N_Ef(trace_1_toma_nvmeibt_toma_wakeup, "Failed to lock toma wakeup mutex (@AUTO_ERRNO)");
		goto out_unlocked;
	}
	if (ptr == NULL && toma_wakeup_test_and_set(type, true)) {
		/*
		 * if wakeup of this type (with ptr==NULL) already pending: skip
		 *
		 * Note the race with toma_wakeup_event(): they may clear the pending
		 * before the actual write() here had occurred. This is ok because they
		 * would still invoke the wakeup callback (for ptr==NULL), just perhaps
		 * twice (and the callback should handle this gracefully).
		 */
		N_Tf(trace_2_toma_nvmeibt_toma_wakeup, "wakeup request skipped due to already pending");
		goto skip;
	}
	if (nvmeibt_write(toma_wakeup_pipe[1], &buf, sizeof(buf)) < 0) {
		N_Ef(tvsjkwi, "Fail to write type @STR to toma wakeup (@AUTO_ERRNO)", toma_wakeup_type_to_str(type));
		goto out;
	}
	atomic_add(1, &n_entries_in_the_toma_wakeup_pipe);
skip:
	ret = 0;
out:
	if (pthread_mutex_unlock(&toma_wakeup_mutex) != 0) {
		N_Ef(trace_5_toma_nvmeibt_toma_wakeup, "Failed to unlock toma wakeup mutex (@AUTO_ERRNO)");
	}
out_unlocked:
	if (ret < 0) {
		if (toma_persistency_wq != NULL)		// If toma main thread shuttind down, this will crash main thread, instead of waiking it up
			self_inflicted_death_on_error();
		else { /* Finalize of wq entry will not be called*/}
	}
	N_Tf(__AUTOID__, "Done");
	return ret;
}

void nvmeibt_toma_trigger_wakeup_handle_err(struct nvmeibt_wq_entry *wq_entry) {
	const int wakeup_rv = nvmeibt_toma_trigger_wakeup(NVMEIBT_TOMA_WAKEUP_TYPE_WQ, (void *)wq_entry);
	if (wakeup_rv < 0) {
		wq_entry_free_after_wakeup(wq_entry);		// Just free the memory, toma main thread cannot wakeup, This is dangerous as finalize/free is called from wq context, and for run once wq this will actually get stuck
	}
}

void nvmeibt_kafka_toma_wakeup_dispatcher(void *ptr);
/* receive wakeup event by TOMA main thread */
static int toma_wakeup_event(void)
{
	int								ret = -1;
	int								iteration_no = 0;
	ssize_t							read_size_rv;
	struct timespec					now, start_timespec;
	static struct toma_wakeup_args	buf;
	static ssize_t					prev_read_buf_n_chars = 0;
	ssize_t							expected_read_size;
	const bool is_shuttind_down = (shutdown_status != ds_none);
	NFIN;

	getnstimeofday_boot(&start_timespec);
	do {
		TODO(This is an interim solution. Need to put everything in one WQ item);
		if (iteration_no) {
			N_Tf(rsvwiow, "iteration=@INT", iteration_no);
		}
		expected_read_size = sizeof(buf) - prev_read_buf_n_chars;
		read_size_rv = read(toma_wakeup_pipe[0], (void *)&buf + prev_read_buf_n_chars, expected_read_size);
		if (read_size_rv < 0) {
#if 0	// EPOLLLT - Level triggered
			if (errno == EINTR) {
				N_Tf(tvghsjw, "Interrupted read. retrying");
			} else if (errno == EAGAIN) {
				N_Tf(tbjhsdj, "Received EAGAIN, done reading");
				ret = 0;
				goto out;
			} else {
				N_Tf(tvdhjq9, "Fail read (@AUTO_ERRNO)");
				ret = -1;
				goto out;
			}
#else	// #if 0	// EPOLLLT - Level triggered
			ret = 0;
			goto out;	// Without EPOLLET - (we are level-triggered)) - quit and this function will be called again if there are items in the pipe
#endif	// #if 0	// EPOLLLT - Level triggered
		} else if (read_size_rv == 0) {
			N_Tf(tvghsdd, "EOF");
			goto out;
		} else {
			prev_read_buf_n_chars += read_size_rv;
		}
		//
		if (prev_read_buf_n_chars == sizeof(buf)) {
			const int in_air_wakeups = atomic_add(-1, &n_entries_in_the_toma_wakeup_pipe);
			prev_read_buf_n_chars = 0;
			N_Tf(trace_2_toma_toma_wakeup_event, "wakeup event type @TOMA_WAKEUP_TYPE_TO_STR ptr @PTR, remaining=@INT", toma_wakeup_type_to_str(buf.type), buf.ptr, in_air_wakeups);

			if (buf.ptr == NULL)
				toma_wakeup_test_and_set(buf.type, false);

			switch (buf.type) {
			case NVMEIBT_TOMA_WAKEUP_TYPE_WQ:
				toma_wakeup_wq(buf.ptr);
				break;
			case NVMEIBT_TOMA_FD_TYPE_LOCAL_SERVER_EVENTS:
			case NVMEIBT_TOMA_WAKEUP_TYPE_NETLINK:
				nvmeibt_server_lib_consume_incomming_srvr_msgs();	// Must take care of clients brute force disconnect during shutdown
				break;
			case NVMEIBT_TOMA_WAKEUP_TYPE_KAFKA:
				nvmeibt_kafka_toma_wakeup_dispatcher(buf.ptr);
				break;
			default:
				N_Ef(na89j3k, "Unfamiliar type=@INT", buf.type);
				break;
			}
			memset(&buf, 0, sizeof(buf));
		} else {
			N_Wf(slso3lp, "Partial read n=@RV_SSIZE_T", read_size_rv);
		}
		getnstimeofday_boot(&now);
		if ((++iteration_no > 100) || (timespec_diff_ns(now, start_timespec) > MSEC_TO_NSEC(50))) {
			N_Tf(a2jn4la, "Spent too much time here. Quitting");
			ret = 1;
			goto out;
		}
	} while (1);
out:
	if ((ret < 0) && !is_shuttind_down)
		terminate_toma(ret);

	NFOUT;
	return ret;
}

int nvmeibt_toma_read_disk_from_stock_driver_add_work(struct nvmeibt_wq_entry *e)
{
	int rv;

	NFIN;
	if (read_disk_from_smart_wq) {
		nvmeibt_wq_addw(read_disk_from_smart_wq, e);
		rv = 0;
	}
	else
		rv = -1;
	NFOUT;
	return rv;
}

int nvmeibt_toma_persistency_add_work(struct nvmeibt_wq_entry *e)
{
	int rv;

	NFIN;
	if (toma_persistency_wq) {
		nvmeibt_wq_addw(toma_persistency_wq, e);
		rv = 0;
	}
	else
		rv = -1;
	NFOUT;
	return rv;
}

int nvmeibt_toma_leader_add_work(struct nvmeibt_wq_entry *e)
{
	int rv;

	NFIN;
	if (leader_wq) {
		nvmeibt_wq_setw(leader_wq, e);
		rv = 0;
	}
	else
		rv = -1;
	NFOUT;
	return rv;
}

int nvmeibt_registrant_disconnect_add_work(struct nvmeibt_local_disk *local_disk, struct nvmeibt_wq_entry *e)
{
	/* registrant_disconnect events should not be skipped due to version mismatch */
	return nvmeibt_local_disk_specific_add_work(local_disk, e);
}

void wakeup_format_event(const struct nvmeibt_ascii_uuid *ldisk_id,
						 unsigned int vendor_id,
						 const char *format_req_disk_obj_uuid_str,
						 unsigned int block_size,
						 unsigned int metadata_size,
						 unsigned int format_request_counter,
						 int64_t boot_time,
						 const struct nvmeibt_urn_uuid *mgmt_DB_urn_uuid,
						 const struct nvmeibt_ascii_uuid *native_serial,
						 int nsid,
						 const char *native_nguid __attribute__((unused)))
{
	struct nvmeibt_local_disk				*local_disk = NULL;
	struct nvmeibt_disk						*disk;
	union nvmeib_uuid						format_req_disk_obj_uuid;
	union nvmeib_uuid						format_mgmt_DB_uuid;

	NFIN;

	if (boot_time != nvmeibt_global_get_startup_timestamp_msec()) {
		N_Tf(nqq99s4, "boot time mismatch, probably old format command @LLD!=@LLD", boot_time, nvmeibt_global_get_startup_timestamp_msec());
		goto out;
	}

	nvmeibt_urn_uuid_str_to_union_uuid(&format_req_disk_obj_uuid, format_req_disk_obj_uuid_str);
	nvmeibt_urn_uuid_to_union_uuid(&format_mgmt_DB_uuid, mgmt_DB_urn_uuid);

	nvmeibt_global_validate_and_upd_mgmt_DB_uuid(&format_mgmt_DB_uuid);
	local_disk = nvmeibt_local_disk_get_local_disk_by_ldisk_id(ldisk_id, nvmeibt_global_get_global()->nvmesh_local_disks_hash_by_ldisk_id_str);
	if (!local_disk) {
		local_disk = nvmeibt_local_disk_get_local_disk_by_ldisk_id(ldisk_id, nvmeibt_global_get_global()->stock_local_disks_hash_by_ldisk_id_str);
		if (!local_disk) {
			local_disk = nvmeibt_local_disk_get_local_disk_by_ldisk_id(ldisk_id, nvmeibt_global_get_global()->formatting_local_disks_hash_by_ldisk_id_str);
			if (local_disk) {
				N_Tf(rcxj29s, "Already formatting disk=@STR. Ignoring", nvmeibt_local_disk_display(local_disk));
			} else {
				N_WTf(fjiu87e, "Unknown disk=@STR. Ignoring", ldisk_id->str);
			}
			goto out;
		}
	}

	if (nvmeibt_local_disk_is_being_deleted(local_disk)) {
		N_Tf(5zxb92k, "Ignoring disk=@STR is_being_deleted", nvmeibt_local_disk_display(local_disk));
		goto out;
	}
	if (metadata_size > 0 && (local_disk->from_config.smart_info.metadata_cap & NVME_NS_MC_SEP_MASK) == 0 && !DISK_ALLOW_INLINE_MD) {
		const struct nvmeibt_disk_flow_params_t *params = NULL;
		params = nvmeibt_disk_flow_params_get(local_disk->from_config.smart_info.Model, true);
		if (!params->force_metadata) {
			N_WTf(sjuhfg4, "Cannot format disk=@STR model=@STR, inline-MD not supported", nvmeibt_local_disk_display(local_disk), local_disk->from_config.smart_info.Model);
			nvmeibt_strlcpy(local_disk->from_config.status, "Error", sizeof(local_disk->from_config.status));
			NVMEIBT_GLOBAL_MARK_REPORT_TARGET_HAS_NEW_DATA(02msd82);
			goto out;
		}
		else {
			N_Tf(ii9ut66, "Format disk=@STR ignoring metadataCapabilities due to force_metadata flow parameter", nvmeibt_local_disk_display(local_disk));
		}
	}

	if (local_disk->is_excluded) {
		N_WTf(djjhfyu, "Cannot format disk=@STR because it excluded", nvmeibt_local_disk_display(local_disk));
		goto out;
	}

	if (strcmp(local_disk->from_config.status, "Ingesting") == 0) {
		N_Wf(trace_2_toma_wakeup_format_event, "Cannot format disk=@STR because it is in state=@STATE_STR", nvmeibt_local_disk_display(local_disk), local_disk->from_config.status);
		goto out;
	}

	if (!nvmeibt_local_disk_is_done_initial_reading_of_local_disk(local_disk)) {
		N_Wf(qqwy763, "disk=@STR is_smart_log_valid=@BOOL is_done_reading_gpt=@BOOL is_mbr_a_valid_pmbr=@BOOL main_gpt.is_valid=@BOOL. Ignoring early format. mgmt will resend after report_target",
			nvmeibt_local_disk_display(local_disk),
			local_disk->is_smart_log_valid, nvmeibt_local_disk_is_done_initial_reading_of_local_disk(local_disk), local_disk->is_mbr_a_valid_pmbr, local_disk->main_gpt.is_valid);
		goto out;
	}

	if (ARE_UUID_EQ(&(local_disk->from_config.disk_metadata.mgmt_db_uuid), &format_mgmt_DB_uuid)) {
		if (!ARE_UUID_EQ(&(local_disk->main_gpt.header.disk_obj_uuid), &format_req_disk_obj_uuid)) {
			N_Tf(gf88iu3, "disk=@STR uuid changed in mgmt, GPT_uuid=@UUID_LE format_CMD_uuid=@UUID_LE. Accepting regardless of format counters",
				 nvmeibt_local_disk_display(local_disk), &local_disk->main_gpt.header.disk_obj_uuid, &format_req_disk_obj_uuid);
		}
		else {
			if (local_disk->active_format_request_counter >= format_request_counter) {
				N_WTf(dkoo045, "Skipping duplicate format req. disk=@STR. format_request_counter=@UINT, active_format_request_counter=@UINT",
					nvmeibt_local_disk_display(local_disk), format_request_counter, local_disk->active_format_request_counter);
				goto out;
			}

			if (local_disk->from_config.disk_metadata.format_request_counter >= format_request_counter) {
				N_Tf(dkoiut6, "Skipping format req. disk=@STR. format_request_counter=@UINT, local_disk->format_request_counter=@UINT message repeated or out of order",
					nvmeibt_local_disk_display(local_disk), format_request_counter, local_disk->from_config.disk_metadata.format_request_counter);
				goto out;
			}
		}
	} else {
		N_Tf(aajii9r, "Got format from different mgmt, Accepting regardless of format counter. disk_mgmt_db_uuid=@UUID_LE current mgmt_db_uuid=@UUID_LE",
			&(local_disk->from_config.disk_metadata.mgmt_db_uuid), nvmeibt_global_get_mgmt_DB_uuid());
	}
	/* We got a new (valid) format command - if it's here it means the format_write_counter is newer, and this is not just a network glitch.
	   We now save the new format parameters in the local disk
	   1. should_relaunch_format_on_the_next_zeroing_finalize
	   2. Pending format - Remember the new format parameters	*/

	// If it holds segments, then delete them
	disk = NNVMEIBT_LOCAL_DISK_GET_DISK(trace_10_toma_wakeup_format_event, local_disk);
	if (nvmeibt_disk_brute_force_del_all_segs_due_to_format(disk) < 0) {
		N_Wf(t_04_toma, "ldisk=@STR has segments. Skipping", nvmeibt_local_disk_display(local_disk));
		goto out;
	}

	// Record the new format parameters in the pending format.
	local_disk->pending_format.ldisk_id = *ldisk_id;
	local_disk->pending_format.native_serial = *native_serial;
	local_disk->pending_format.nsid = nsid;
	local_disk->pending_format.vendor_id = vendor_id;
	local_disk->pending_format.block_size = block_size;
	local_disk->pending_format.metadata_size = metadata_size;
	nvmeibt_strlcpy(local_disk->pending_format.ld_display, nvmeibt_local_disk_display(local_disk), sizeof(local_disk->pending_format.ld_display));
	local_disk->pending_format.disk_obj_uuid = format_req_disk_obj_uuid;
	local_disk->pending_format.format_request_counter = format_request_counter;

	if (local_disk->is_being_formatted) {
		N_Tf(ksixjd7, "disk=@STR format is in progress, setting abort for current format. current state=@STATE_STR format_request_counter=@FORMAT_REQUEST_COUNTER",
			 nvmeibt_local_disk_display(local_disk), local_disk->from_config.status, format_request_counter);
		// Mark current format as needing abort - it will be aborted when no threads are in the air,
		local_disk->should_relaunch_format_on_the_next_zeroing_finalize = true;
		goto out;
	}

	if (nvmeibt_local_disk_launch_disk_format(local_disk) < 0) {
		N_Tf(trace_9_toma_wakeup_format_event, "Failed launch_disk_format(). Possibly need to bind to nvmeibs first");
		goto out;
	}
out:
    NFOUT;
}

void nvmeibt_toma_leader_mark_member_non_responsive(struct nvmeibt_raft_member *member)
{
	NFIN;
	nvmeibt_topology_leader_detach_all_disks_from_raft_member(member);
	member->is_alive_for_topo_start_timespec = TIMESPEC_ZERO;
	NFOUT;
}

void nvmeibt_toma_raft_validity_was_updated(void)
{
	NFIN;
	if (nvmeibt_raft_is_raft_valid()) {
		nvmeibt_register_open_all_eligible_seg_actives_for_use();
	}
	NFOUT;
}

void nvmeibt_toma_dispatch_received_msg(struct nvmeibt_big_msg *big_msg)
{
	NFIN;
	switch (big_msg->msg_type & 0xffff0000) {
	case NVMEIBT_IB_PROTOCOL_SIGNATURE_RAFT:
		nvmeibt_raft_handle_incoming_message(big_msg);
		break;
	default:
		{
			long long unsigned *t = (long long unsigned *)big_msg;
			N_Ef(trace_toma_nvmeibt_toma_dispatch_received_msg, "Unfamiliar protocol_signature @MSG_TYPE", (unsigned int)big_msg->msg_type);
			TODO(notlttng);
			N_Tf(trace_1_toma_nvmeibt_toma_dispatch_received_msg, "big_msg=@BIG_MSG, mem=@MEM longlongx=@LONGLONGX longlongx=@LONGLONGX longlongx=@LONGLONGX longlongx=@LONGLONGX longlongx=@LONGLONGX longlongx=@LONGLONGX longlongx=@LONGLONGX", t, *(t+0), *(t+1), *(t+2), *(t+3), *(t+4), *(t+5), *(t+6), *(t+7));
		}
		break;
	}
	NNVMEIBT_BM_FREE(trace_2_toma_nvmeibt_toma_dispatch_received_msg, big_msg);
	NFOUT;
}

void nvmeibt_topology_set_mgmt_updates_pause_state(int is_paused)
{
	is_mgmt_updates_paused = is_paused;
}

void nvmeibt_toma_init_mesh(void)
{
	struct nvmeibt_nic		*nic;

	NFIN;
	NVMEIB_HASH_FOREACH(nic, nvmeibt_global_get_global()->nics_hash_by_uuid) {
		// IB & RoCE: we connect also to loopback for locking
		TODO(locks code is obsolete);
		TODO(Remove stale nics);
		nvmeibt_nm_add_remote_nic(nw_node, nic);
	}
	nvmeibt_raft_activate();
	NFOUT;
}

int nvmeibt_toma_send_msg_to_client(struct nvmeibt_registrant_ctx *reg_ctx, int praid_version,
									enum NVMEIBT_CLIENT_MSG_TYPES msg_type, enum NVMEIBT_CLIENT_TR_REASON reason, int data_length, void *data, u64 msg_id)
{
	int							rv = 0;
	struct nvmeibs_toma_client_proc_buf	*msg;
	const int					buf_len = sizeof(*msg) + data_length;

	NFIN;

	msg = NNVMEIBT_BM_ALLOC(trace_toma_nvmeibt_toma_send_msg_to_client, buf_len);
	msg->handle = reg_ctx->client_messaging_handle;
	nvmeibt_client_thick_msg_write(&msg->data,
							 (int)msg_type,
							 (int)reason,
							 reg_ctx->client->net.host_name,
							 min((u32)reg_ctx->client_protocol_version, (u32)NVMEIBT_CLIENT_PROTO_VERSION),	// Downgrade the protocol
							 0xDEADBEAF, //nvmeibt_global_get_global()->mgmt_config_version,
							 nvmeibt_seg_active_active_config_version(reg_ctx->seg_active),
						     RAFT_COMMIT_LIFECYCLE_VAL(TOPO, follower_applied),		TODO(PROBABLY USELESS)
							 praid_version,
							 nvmeibt_union_uuid_to_urn_uuid(&(reg_ctx->seg_uuid)).str,
							 reg_ctx->reg_lock_id.all,
							 (reg_ctx->seg_active ? reg_ctx->seg_active->highest_reservation_mode_version : 0), // Dont use: reg_ctx->reservation_mode_version,
							 reg_ctx->client_conversation_index,
							 0, // Irrelevant: rt_never_reged_on_seg
							 0, // Irrelevant: is_REGISTER_for_recovery
							 data_length,
							 data,
							 msg_id
							);
	rv = nvmeib_srvr_api_lib_send_block_msg_to_client(nvmeibt_get_srv_comm(), msg, buf_len, reg_ctx->client->net.host_name);
	NNVMEIBT_BM_FREE(trace_4_toma_nvmeibt_toma_send_msg_to_client, msg);

	NFOUT;
	return rv;
}

static char management_host[256];
static char management_port[8];

static int read_nvmesh_management_host(void)
{
	int rv = 0;

	NFIN;
	nvmeibt_strlcpy(management_host, TOMA_DIR_RUN_NVMESH "/binary_uds", sizeof(management_host));
	management_port[0] = '\0';
	N_Tf(trace_toma_read_nvmesh_management_host, "management_host=@MANAGEMENT_HOST, management_port=@MANAGEMENT_PORT", management_host, management_port);

	NFOUT;
	return rv;
}

/************************** Signal Handler ************************************/
static volatile sig_atomic_t got_sigusr1 = 0;		// Non critical signal
static volatile sig_atomic_t got_sigusr2 = 0;		// Non critical signal
static volatile int received_sig_no;				// Critical shutting down signal
void toma_sig_handler_fn(int32_t n, uint64_t addr)
{
	if (n == SIGCHLD)
		return; /* do nothing, no logs, got_sigchld = 1;*/
	N_IMf(ttsgh1, "got signal=@INT addr=@LX", n, addr);
	if (n == SIGUSR1) {
		got_sigusr1 = 1; /* SIGUSR1 is used for dumping status */
	} else if (n == SIGUSR2) {
		got_sigusr2 = 1; /* SIGUSR2 is used to start/stop logging */
	} else if (n == SIGHUP) {
		nvmeibt_global_mark_is_reread_nvmesh_conf_required();
	} else {
		received_sig_no = n;
	}
}
/******************************************************************************/

void nvmeibt_local_disk_set_is_periodic_smart_polling_enabled(bool val, bool print_me);

#define TOMA_LOG_MAX_N 10
#define TOMA_LOG_MIN_SIZE (1LL * 1024)
#define TOMA_LOG_MAX_SIZE (16LL * 1024 * 1024 * 1024 - 1)

#define TOMA_BIN_LOG_MIN_N				2
#define TOMA_BIN_LOG_MAX_N				100
#define TOMA_BIN_LOG_MIN_SIZE			1   // 1M
#define TOMA_BIN_LOG_MAX_SIZE			200 // 200M

static int read_cmdl(int argc, char *argv[], bool is_logable)
{
	int op, size_len, rv = 0;
	long i, size_factor = 1;
	char *_argv[argc];

	static struct option long_options[] =
	{
		{"log-dir-name",			required_argument,	0,	'l'},
		{"num-logs",				required_argument,	0,	'n'},
		{"log-size",				required_argument,	0,	's'},
		{"cloud-mode",				required_argument,	0,	'c'},
		{"sm-key",					required_argument,	0,	'k'},
		{"abort",					no_argument,		0,	'a'},
		{"sm-query-burst",			required_argument,	0,	'b'},
		{"use-srq",					required_argument,	0,	'S'},
		{"raft-udp",				no_argument,		0,	'u'},
		{"use-libibcm",				no_argument,		0,	'i'},
		{"num-bin_logs",			required_argument,	0,	'z'},
		{"bin_log-size",			required_argument,	0,	'x'},
		{"cfg-id",					required_argument,	0,	'C'},
		{"cfg-name",				required_argument,	0,	'D'},
		{"cfg-version",				required_argument,	0,	'E'},
		{"nm-transport",			required_argument,	0,	'N'},
		{"tracing-cgroup",			required_argument,	0,	't'},
		//
		{"input-file",				required_argument,	0,	'f'},
		{"output-file",				required_argument,	0,	'F'},
		{"convert-to-persistence",	no_argument,		0,	'p'},
		{"convert-to-json",			no_argument,		0,	'j'},
		{0, 0, 0, 0}
	};

	static const char short_options[] = "l:n:s:c:k:b:a:z:x:C:D:E:vu:N";
	static int long_idx = -1;

	for (i = 0; i < argc; ++i) {
		_argv[i] = trim_whitespace(argv[i]);
	}

	while ((op = getopt_long(argc, _argv, short_options, long_options, &long_idx)) != -1) {
		fprintf(stdout, "optarg=%s\n", optarg);
		if (optarg) {
			optarg = trim_whitespace(optarg);
		}
		switch (op) {
		case 't':
			nvmeibt_strlcpy(tracing_cgroup, optarg, sizeof(tracing_cgroup));
			fprintf(stdout, "TOMA tracing_cgroup is %s\n", tracing_cgroup);
			break;
		case 'N':
			nvmeibt_strlcpy(toma_nm_transport_lib_path, optarg, sizeof(toma_nm_transport_lib_path));
			fprintf(stdout, "TOMA toma_nm_transport_lib_path is %s\n", toma_nm_transport_lib_path);
			break;
		case 'l':
			nvmeibt_strlcpy(toma_log_dir_name, optarg, sizeof(toma_log_dir_name));
			fprintf(stdout, "TOMA log dir is %s\n", toma_log_dir_name);
			break;
		case 'C':
			nvmeibt_strlcpy(toma_cfg_id, optarg, sizeof(toma_cfg_id));
			fprintf(stdout, "TOMA config-id is %s\n", toma_cfg_id);
			break;
		case 'D':
			nvmeibt_strlcpy(toma_cfg_name, optarg, sizeof(toma_cfg_name));
			fprintf(stdout, "TOMA config-name is %s\n", toma_cfg_name);
			break;
		case 'E':
			nvmeibt_strlcpy(toma_cfg_version, optarg, sizeof(toma_cfg_version));
			fprintf(stdout, "TOMA config-version is %s\n", toma_cfg_version);
			break;
		case 'z':
			toma_bin_log_file_n = (unsigned)atoi(optarg);
			if (toma_bin_log_file_n > TOMA_BIN_LOG_MAX_N) {
				toma_bin_log_file_n = TOMA_BIN_LOG_MAX_N;
			}
			else if (toma_bin_log_file_n < TOMA_BIN_LOG_MIN_N) {
				toma_bin_log_file_n = TOMA_BIN_LOG_MIN_N;
			}
			fprintf(stdout, "TOMA n bin logs is %d\n", toma_bin_log_file_n);
			break;
		case 'x':
			toma_bin_log_file_size_mega = (unsigned)atoi(optarg);
			if (toma_bin_log_file_size_mega > TOMA_BIN_LOG_MAX_SIZE) {
				toma_bin_log_file_size_mega = TOMA_BIN_LOG_MAX_SIZE;
			} else if (toma_bin_log_file_size_mega < TOMA_BIN_LOG_MIN_SIZE) {
				toma_bin_log_file_size_mega = TOMA_BIN_LOG_MIN_SIZE;
			}
			fprintf(stdout, "TOMA bin log size is %d[mb]\n", toma_bin_log_file_size_mega);
			break;
		case 'n':
			toma_log_file_n = (unsigned)atoi(optarg);
			if (toma_log_file_n > TOMA_LOG_MAX_N) {
				toma_log_file_n = TOMA_LOG_MAX_N;
			}
			fprintf(stdout, "TOMA n logs is %d\n", toma_log_file_n);
			break;
		case 's':
			size_len = (int)strlen(optarg);
			if (optarg[size_len - 1] == 'K') {
				optarg[size_len - 1] = '\0';
				size_factor = 1024;
			}
			if (optarg[size_len - 1] == 'M') {
				optarg[size_len - 1] = '\0';
				size_factor = 1024 * 1024;
			}
			if (optarg[size_len - 1] == 'G') {
				optarg[size_len - 1] = '\0';
				size_factor = 1024 * 1024 * 1024;
			}
			fprintf(stdout, "optarg=%s\n", optarg);
			toma_log_file_size = strtoll(optarg, NULL, 0) * size_factor;
			if (toma_log_file_size < TOMA_LOG_MIN_SIZE ||
				toma_log_file_size > TOMA_LOG_MAX_SIZE) {
				fprintf(stdout, "TOMA log file must be >= %lld and <= %lld\n",
					TOMA_LOG_MIN_SIZE, TOMA_LOG_MAX_SIZE);
				toma_log_file_size = TOMA_LOG_MAX_SIZE;
			}
			fprintf(stdout, "TOMA logfile size %lld\n", toma_log_file_size);
			break;
		case 'c':
			nvmeibt_local_disk_set_is_periodic_smart_polling_enabled(strcasecmp(optarg, "Yes") != 0 &&
																	 strcasecmp(optarg, "True") != 0 &&
																	 strcmp(optarg, "1") != 0,
																	 false);
			fprintf(stdout, "TOMA cloud-mode is %s\n", optarg);
			N_Tf(467sagnstdout, "TOMA cloud-mode is @STR", optarg);
			break;
		case '4':
		case 'k':
			fprintf(stdout, "Deprecated: Param %c val=%s\n", op, optarg);
			break;
		case 'a':
			toma_abort_nf = true;
			fprintf(stdout, "TOMA will abort on non-fatal error detection\n");
			break;
		case 'u':
			fprintf(stdout, "TOMA will start UDP server for RAFT\n");
			break;
		case 'i':
			nvmeibt_use_libibcm = true;
			fprintf(stdout, "TOMA will use libibcm for IB connections\n");
			break;
		//
		case 'f':
			nvmeibt_strlcpy(nvmeibt_toma_cmdline_arg_input_file_name, optarg, sizeof(nvmeibt_toma_cmdline_arg_input_file_name));
			fprintf(stdout, "TOMA input file_name='%s'\n", nvmeibt_toma_cmdline_arg_input_file_name);
			break;
		case 'F':
			nvmeibt_strlcpy(nvmeibt_toma_cmdline_arg_output_file_name, optarg, sizeof(nvmeibt_toma_cmdline_arg_output_file_name));
			fprintf(stdout, "TOMA output file_name='%s'\n", nvmeibt_toma_cmdline_arg_output_file_name);
			break;
		case 'p':
			nvmeibt_is_converting_json_to_persistence = true;
			fprintf(stdout, "TOMA is converting JSON to persistence\n");
			break;
		case 'j':
			nvmeibt_is_converting_persistence_to_json = true;
			fprintf(stdout, "TOMA is converting persistence to JSON\n");
			break;

		default:
			if (is_logable) {
				N_ETf(ki9sm2k, "Bad command line param: op='@CHAR' optarg=@STR", op, optarg);
			} else {
				fprintf(stdout, "usage: %s\n", argv[0]);
				fprintf(stdout, "\t[-l TOMA log file name]\n");
				fprintf(stdout, "\t[-n number of log files]\n");
				fprintf(stdout, "\t[-s log file size in K or M or G]\n");
				fprintf(stdout, "\t[-c cloud-mode <Yes/No>]\n");
				fprintf(stdout, "\t[-m max sm query rate (per second)\n");
				fprintf(stdout, "\t[-S use_srq]\n");
				fprintf(stdout, "\t[-u|--udp start RAFT UDP]\n");
				fprintf(stdout, "\t[-i|--use-libibcm - Force use of libibcm]\n");
				fprintf(stdout, "\t[-z number of bin log files]\n");
				fprintf(stdout, "\t[-x bin log file size in M]\n");
				//
				fprintf(stdout, "\t[-f file-name (E.g. input for convert)]\n");
				fprintf(stdout, "\t[-p convert-to-persistence]\n");
				fprintf(stdout, "\t[-j convert-to-json]\n");
#ifdef TOMA_DEBUG
				fprintf(stdout, "\t[-a abort on non-fatal error detection]\n");
#endif
			}
			rv = -1;
			break;
		}
	}

	return rv;
}

int print_status_time(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx, const struct timespec ts)
{
	char time_str[32];
	size_t strf_len;
	struct tm	tmp_tm;
	struct timespec ts_real;

	if (timespec_eq(ts, TIMESPEC_ZERO)) {
		(*printf_fn)(printf_ctx, "Not Set      ");
	} else {
		getnstimeofday_convert_boot_to_real(&ts, &ts_real);
		localtime_r(&ts_real.tv_sec, &tmp_tm);
		strf_len = strftime(time_str, sizeof(time_str), "%H:%M:%S", &tmp_tm);
		sprintf(time_str + strf_len, ".%03lld", NSEC_TO_MSEC(ts_real.tv_nsec));
		(*printf_fn)(printf_ctx, "%s", time_str);
	}
	return 0;
}

static void print_open_fds(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx)
{
	int									i;
	struct nvmeibt_toma_fd_in_use		*fds_arr;

	(*printf_fn)(printf_ctx, "\n\t- Open FDs");
	fds_arr = g_fds_in_use.fds_arr;

	for (i = 0; i < g_fds_in_use.n_fds_in_use; i++) {
		if ((i % 8) == 0)
			(*printf_fn)(printf_ctx, "\n\t\t");
		(*printf_fn)(printf_ctx, "%d(%s)    ", fds_arr[i].fd, fd_type_str(fds_arr[i].fd_type));
	}
}

#if 0
/*
The following code prints static memory allocation statistics,
but cannnot be compiled due to a known gcc bug:
	link - https://mail.gnu.org/archive/html/bug-binutils/2017-11/msg00182.html
    bug -  	[Bug ld/22471] New: libraries using version scripts can cause undefined reference to symbol '__bss_start'
In order to get the bss & data sections info 'size nvmeibt_toma' or 'readelf -s/-tnvmeibt_toma' commands can be used
*/

extern char __data_start[], _edata[], __bss_start[], _end[];
void static_mem_alloc_print_status(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx)
{
	uint64_t			bss_size, data_size;

	bss_size = _end - __bss_start;
	data_size = _edata - __data_start;
	(*printf_fn)(printf_ctx, "STATIC MEMORY ALLOCATION\n");
	(*printf_fn)(printf_ctx, "\tbss: %ll,   data: %ll,   total: %ll\n", bss_size, data_size, bss_size + data_size);
}
#endif

int last_time_print_status(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx)
{
	(*printf_fn)(printf_ctx, "LAST TIME\n");
	(*printf_fn)(printf_ctx, "\t- Events\n");
	(*printf_fn)(printf_ctx, "\t\t- Select: ");
	print_status_time(printf_fn, printf_ctx, nvmeibt_global_get_cur_event_start_time());
	(*printf_fn)(printf_ctx, "\tClient: ");
	print_status_time(printf_fn, printf_ctx, last_client_event_timespec);
	(*printf_fn)(printf_ctx, "\tLocal Server: ");
	print_status_time(printf_fn, printf_ctx, last_local_srv_event_timespec);
	(*printf_fn)(printf_ctx, "\n");

	(*printf_fn)(printf_ctx, "\t\t- IB: ");
	print_status_time(printf_fn, printf_ctx, last_ib_event_timespec);
	(*printf_fn)(printf_ctx, "\n");

	(*printf_fn)(printf_ctx, "\t- Timeouts\n");
	(*printf_fn)(printf_ctx, "\t\t- RAFT: ");
	print_status_time(printf_fn, printf_ctx, last_raft_timeout_timespec);
	(*printf_fn)(printf_ctx, "\tRegister: ");
	print_status_time(printf_fn, printf_ctx, last_register_timeout_timespec);
	(*printf_fn)(printf_ctx, "\tRecovery: ");
	print_status_time(printf_fn, printf_ctx, last_recovery_timeout_timespec);

	print_open_fds(printf_fn, printf_ctx);

	(*printf_fn)(printf_ctx, "\n\tToma start: ");
	print_status_time(printf_fn, printf_ctx, nvmeibt_global_get_startup_timespec());
	(*printf_fn)(printf_ctx, "\n\tToma Wakeup: ");
	print_status_time(printf_fn, printf_ctx, last_toma_wakeup_event_timespec);
	(*printf_fn)(printf_ctx, "\tWQ: ");
	print_status_time(printf_fn, printf_ctx, last_wq_event_timespec);
	(*printf_fn)(printf_ctx, "\n");
	return 0;
}

static void write_stat_wrapper(struct nvmeibt_wq_entry *wq_entry)
{
	struct stat_wq_entry *entry;
	FILE	*status_fptr = NULL;

	NFIN;

	entry = container_of(wq_entry, struct stat_wq_entry, wq_entry);

	if (!(status_fptr = fopen(entry->status_filename, "w+"))) {
		N_ETf(error_toma_write_stat_wrapper, "Error (@AUTO_ERRNO) writing toma status file to @STATUS_FILENAME", entry->status_filename);
		goto out;
	}
	fprintf(status_fptr, "%s", nvmeibt_Str_str(entry->status_str));
	fclose(status_fptr);

out:
	nvmeibt_toma_trigger_wakeup_handle_err(wq_entry);
	NFOUT;
}

static void write_stat_freer(struct nvmeibt_wq_entry *wq_entry)
{
	struct stat_wq_entry *entry = container_of(wq_entry, struct stat_wq_entry, wq_entry);
	NNVMEIBT_STR_FREE(trace_toma_write_stat_freer, entry->status_str);
	NNVMEIBT_BM_FREE(trace_1_toma_write_stat_freer, entry);
}

void print_status_str(enum nvmeibs_toma_status_type status_type, int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx)
{
	struct timespec				now;
	struct tm					timeinfo;
	char						time_str[64];
	int							print_headers = 1;

	NFIN;
	/* Make a nicer time string for inside the file */
	getnstimeofday_real(&now);
	localtime_r(&now.tv_sec, &timeinfo);
	strftime(time_str, sizeof(time_str), "%b %d %X ", &timeinfo);

	if ((status_type == NVMEIBS_TOMA_STATUS_LEADER) || (status_type == NVMEIBS_TOMA_STATUS_ALL_JSON) ||
		(status_type == NVMEIBS_TOMA_STATUS_NM_JSON))
		print_headers = 0;

	if (print_headers) {
		(*printf_fn)(printf_ctx,
			"*******************************************************************************\n"
			"NVMEIBT STATUS: %s %s\n"
			"*******************************************************************************\n",
			nvmeibt_get_my_hostname(), time_str);

		(*printf_fn)(printf_ctx, "TOMA BUILD\n\t- %s, %s, %s, commit-id=%s branch=%s sw_compatibility_ver=%x, client_proto_version=%x\n",
			MOD_STR, __DATE__, __TIME__, GIT_COMMIT_ID, GIT_BRANCH, TOMA_SW_COMPATIBILITY_VER, NVMEIBT_CLIENT_PROTO_VERSION);
	}

	if (nvmeibt_global_get_global()) {		// Protect against early call too print before topology is initialized
		if (status_type == NVMEIBS_TOMA_STATUS_ALL) {
			// static_mem_alloc_print_status(printf_fn, printf_ctx);
			last_time_print_status(printf_fn, printf_ctx);
		}
		if (status_type == NVMEIBS_TOMA_STATUS_ALL || status_type == NVMEIBS_TOMA_STATUS_RAFT)
			nvmeibt_raft_print_status(printf_fn, printf_ctx);
		if (status_type == NVMEIBS_TOMA_STATUS_ALL || status_type == NVMEIBS_TOMA_STATUS_DSEG)
			nvmeibt_seg_active_print_all_seg_actives_status(printf_fn, printf_ctx);
		if (status_type == NVMEIBS_TOMA_STATUS_ALL || status_type == NVMEIBS_TOMA_STATUS_BDEV)
			nvmeibt_block_device_print_blkdevs_status(printf_fn, printf_ctx);
		if (status_type == NVMEIBS_TOMA_STATUS_ALL || status_type == NVMEIBS_TOMA_STATUS_DISK)
			nvmeibt_disk_print_disks_status(printf_fn, printf_ctx);
	}
	if (status_type == NVMEIBS_TOMA_STATUS_ALL || status_type == NVMEIBS_TOMA_STATUS_IB)
		nvmeibt_nm_print_status(nw_node, printf_fn, printf_ctx);
	// if (status_type == NVMEIBS_TOMA_STATUS_ALL || status_type == NVMEIBS_TOMA_STATUS_RTM)
	// 	nvmeibt_rtm_print_status(printf_fn, printf_ctx);
	if (status_type == NVMEIBS_TOMA_STATUS_ALL || status_type == NVMEIBS_TOMA_STATUS_RECOVER)
		nvmeibt_recovery_print_status(printf_fn, printf_ctx);
	if (nvmeibt_global_get_global()) {		// Protect against early call too print before topology is initialized
		if (status_type == NVMEIBS_TOMA_STATUS_ALL || status_type == NVMEIBS_TOMA_STATUS_CFG)
			nvmeibt_read_config_print_status(printf_fn, printf_ctx);
		if (status_type == NVMEIBS_TOMA_STATUS_ALL || status_type == NVMEIBS_TOMA_STATUS_TOPO)
			nvmeibt_topology_print_status(printf_fn, printf_ctx);
		if (status_type == NVMEIBS_TOMA_STATUS_LEADER)
			nvmeibt_leader_print_status(printf_fn, printf_ctx);
		if (status_type == NVMEIBS_TOMA_STATUS_ALL || status_type == NVMEIBS_TOMA_STATUS_LOCAL_DISKS)
			nvmeibt_local_disk_print_status(printf_fn, printf_ctx);
	}
	if (status_type == NVMEIBS_TOMA_STATUS_ALL || status_type == NVMEIBS_TOMA_STATUS_MEM_ALLOC)
		nvmeibt_print_alloc_free_summary_table(printf_fn, printf_ctx);
	if (status_type == NVMEIBS_TOMA_STATUS_ZEROING)
		nvmeibt_block_device_print_zeroing_status(printf_fn, printf_ctx);
	if (status_type == NVMEIBS_TOMA_STATUS_ALL_JSON)
		nvmeibt_raft_print_status_json(printf_fn, printf_ctx);
	if (status_type == NVMEIBS_TOMA_STATUS_ALL || status_type == NVMEIBS_TOMA_STATUS_KAFKA_INFO)
		nvmeibt_kafka_print_status(printf_fn, printf_ctx);
	if (status_type == NVMEIBS_TOMA_STATUS_NM_JSON)
		nvmeibt_nm_print_status_json(nw_node, printf_fn, printf_ctx);

	if (print_headers)
		(*printf_fn)(printf_ctx,
					 "*******************************************************************************\n");
	NFOUT;
}

static void nvmeibt_toma_print_status(void)		/* Used for printing status - We create a seperate file for this */
{
	int							rv;
	struct timespec				now;
	struct tm					timeinfo;
	char						short_time_str[32];
	struct stat_wq_entry		*task = NNVMEIBT_BM_CALLOC(ttps01, sizeof(*task));
	char						*status_filename = task->status_filename;

	task->status_str = NNVMEIBT_STR_ALLOC(ttps02);
	NNVMEIBT_STR_RESIZE_BUF(ttps03, task->status_str, (1 << 25)-128); // ~32[mb]. Maybe consider calculating the needed size.
	task->wq_entry.type = "SAVE_STAT";
	task->wq_entry.execute = write_stat_wrapper;
	task->wq_entry.free = write_stat_freer;

	getnstimeofday_real(&now);
	localtime_r(&now.tv_sec, &timeinfo);
	strftime(short_time_str, sizeof(short_time_str), "%Y_%m_%d_%H_%M_%S", &timeinfo);
	rv = snprintf(status_filename, PATH_MAX, "%s/toma_%s.stat", nvmeibt_toma_get_log_dir_name(), short_time_str);
	if (rv >= (int)sizeof(task->status_filename)) {
		N_Wf(ttps04, "status filename was truncated because too long"); // failed to compile filename string
		write_stat_freer(&task->wq_entry);
	} else if (stat_wq) {
		print_status_str(NVMEIBS_TOMA_STATUS_ALL, (nvmeibt_status_printf_fn_type)&nvmeibt_Str_sprintf, task->status_str);
		N_Tf(ttps05, "Status generated @ZU[kb]", (task->status_str->str_len >> 10));
		nvmeibt_wq_addw(stat_wq, &task->wq_entry);
	} else {
		N_Ef(ttps06, "Unable to add stat offload task to WQ!");
		write_stat_freer(&task->wq_entry);
	}
}

int nvmeibt_toma_get_status_str(enum nvmeibs_toma_status_type status_type, struct nvmeibt_Str *out)
{
	print_status_str(status_type, (nvmeibt_status_printf_fn_type)&nvmeibt_Str_sprintf, out);
	return nvmeibt_Str_strlen(out);
}

static void at_event_end_activities(void)
{
	struct timespec					now;

//	NFIN;
	// nvmeibr_rtm_state_machine();
	if (nvmeibt_toma_is_in_shutdown()) {
		getnstimeofday_boot(&now);
		if (!is_shutdown_me_only() ||
			timespec_diff_ns(now, nvmeibt_global_get_global()->shutdown_start_time) > (nvmeibt_raft_get_effective_heartbeat_timeout_ns() * 15)) {
			nvmeibt_global_get_global()->is_in_shutdown_active_phase = 1;
			attempt_stable_local_shutdown();
		}
	}
	calc_next_wait_for_registrant_timeout();
//	NFOUT;
}

// idle_time_activities() WAS MOVED TO nvmeibt_global.c
static void add_fd_to_select_fds(struct nvmeibt_toma_fds_in_use *fds_in_use, int fd, enum NVMEIBT_FD_TYPES fd_type)
{
	int		new_fd_num;

	N_Tf(trace_toma_add_fd_to_select_fds, "fd=@FD type=@TYPE", fd, fd_type);
	new_fd_num = fds_in_use->n_fds_in_use;
	fds_in_use->max_fd_no = max(fds_in_use->max_fd_no, fd);
	fds_in_use->fds_arr[new_fd_num].fd_type = fd_type;
	fds_in_use->fds_arr[new_fd_num].fd = fd;
	NNVMEIBT_EPOLL_CTL_READ_ADD(hu3naur, epoll_fd, fd, &fds_in_use->fds_arr[new_fd_num]);
	fds_in_use->n_fds_in_use++;
	fds_in_use->fds_arr[fds_in_use->n_fds_in_use].fd = 0;	// Mark the end
	return;
}

static void update_read_and_exception_select_fds(struct nvmeibt_toma_fds_in_use *fds_in_use)
{
	NFIN;
	fds_in_use->max_fd_no = -1;
	fds_in_use->n_fds_in_use = 0;
	add_fd_to_select_fds(fds_in_use, signals_fd, NVMEIBT_TOMA_FD_TYPE_SYSTEM_EVENTS);
	add_fd_to_select_fds(fds_in_use, toma_wakeup_pipe[0], NVMEIBT_TOMA_FD_TYPE_TOMA_WAKEUP);
	add_fd_to_select_fds(fds_in_use, nvmeibt_udev_get_fd(), NVMEIBT_TOMA_FD_TYPE_UDEV_EVENTS);
	add_fd_to_select_fds(fds_in_use, nvmeibt_nm_get_fd(nw_node), NVMEIBT_TOMA_FD_TYPE_IB);
	if (rsrm_faults_get_fd() != -1) {
		add_fd_to_select_fds(fds_in_use, rsrm_faults_get_fd(), NVMEIBT_TOMA_FD_TYPE_FIFO_COMM);
	}

	NFOUT;
}

static void nvmeibt_toma_abort_child_threads_and_mem(bool do_destroy_mem_alloc) {
	static BOOL already_aborting = false;
	if (already_aborting)
		return;
	already_aborting = true;
	nvmeibt_kafka_shutdown();
	nvmeibt_log_snapshotting_shutdown();
	if (do_destroy_mem_alloc) {
		nvmeibt_bm_destroy();
	}
	nvmeibt_join_all_trace_pollers();	// Beyond this point, no more traces
}

void nvmeibt_toma_abort_child_processes(void) {
	nvmeibt_toma_abort_child_threads_and_mem(true);
}

struct nvmeibt_udev_event_info *find_udev_event_info_by_dev_file_name(const char *dev_file_name, bool *is_new)
{
	struct nvmeibt_udev_event_info			*udev_event_info;
	struct nvmeibt_topology		*cur_topo = nvmeibt_global_get_global();

	XDLIST_FOREACH(udev_event_info, &(cur_topo->udev_events_info)) {
		if (!strcmp(dev_file_name, udev_event_info->dev_file_name)) {
			*is_new = false;
			return udev_event_info;
		}
	}

	udev_event_info = NNVMEIBT_BM_CALLOC(hyrb12s, sizeof(struct nvmeibt_udev_event_info));
	XDLIST_ADD_TAIL(&(cur_topo->udev_events_info), udev_event_info);
	*is_new = true;
	return udev_event_info;
}

void nvmeibt_toma_udev_event_processing_end(struct nvmeibt_udev_event_info *udev_event_info)
{
	if (udev_event_info->act_pending == nvmeibt_udev_none) {
		N_Tf(ibi8uy7, "udev event processing ended, dev=@STR action=@INT", udev_event_info->dev_file_name, udev_event_info->act_to_process);
		XDLIST_DEL(&(udev_event_info->udev_event_info_link));
		NNVMEIBT_BM_FREE(answ341, udev_event_info);
	}
	else {
		udev_event_info->act_to_process = udev_event_info->act_pending;
		udev_event_info->act_pending = nvmeibt_udev_none;
		udev_event_info->is_processing = false;
		N_Tf(ikw8uy7, "continue udev event processing, dev=@STR action=@INT", udev_event_info->dev_file_name, udev_event_info->act_to_process);
	}
}

static void check_if_binding_to_stock_needed(struct nvmeibt_udev_event_info *udev_event_info)
{
	struct nvmeibt_local_disk			*local_disk;

	if (IS_SUPPORTED_NOT_NVME_DISK_TYPE(udev_event_info->disk_type) &&
		(local_disk = nvmeibt_local_disk_get_by_dev_file_name(udev_event_info->dev_file_name))) {
		N_Tf(ou92bgh, "binding needed dev=@STR", udev_event_info->dev_file_name);
		nvmeibt_local_disk_mark_is_bind_back_to_stock_needed(local_disk);
		local_disk->its_udev_event_info = udev_event_info;
		nvmeibt_local_disk_propagate_needed_bind_and_excluded_and_takeover_to_controller_local_disks(local_disk);
	}
	else {
		N_Tf(oi98bgh, "binding not needed dev=@STR", udev_event_info->dev_file_name);
		nvmeibt_toma_udev_event_processing_end(udev_event_info);
	}
}

void nvmeibt_toma_process_waiting_udev_events(void)
{
	struct nvmeibt_local_disk			*stock_local_disk;
	struct nvmeibt_udev_event_info		*udev_event_info;

	XDLIST_FOREACH_SAFE(udev_event_info, &(nvmeibt_global_get_global()->udev_events_info)) {
		if (!udev_event_info->is_processing) {
			udev_event_info->is_processing = true;
			if (udev_event_info->act_to_process == nvmeibt_udev_add) {
				N_Tf(t_fl_toma, "Adding new stock local disk from path=@PATH", udev_event_info->dev_file_name);
				if (nvmeibt_local_disk_add_from_stock_driver(udev_event_info) < 0 ) {
					N_Ef(t_fm_toma, "Error adding disk from stock, from path=@PATH", udev_event_info->dev_file_name);
					nvmeibt_toma_udev_event_processing_end(udev_event_info);
				}
			}
			else { // nvmeibt_udev_del
				stock_local_disk = nvmeibt_stock_local_disk_get_by_dev_file_name(udev_event_info->dev_file_name, 0);

				if (stock_local_disk) {
					struct udev_event_wq_entry *udev_event_task = NNVMEIBT_BM_CALLOC(t_fn_toma, sizeof(*udev_event_task));

					N_Tf(jui8r4x, "Deleting stock local disk from path=@PATH", udev_event_info->dev_file_name);
					udev_event_task->wq_entry.type = "UDEV_EVENT";
					udev_event_task->wq_entry.execute = udev_event_wrapper;
					udev_event_task->wq_entry.finalize = udev_event_finalize;
					udev_event_task->wq_entry.abort = nvmeibt_toma_wakeup_wq_abort_func;
					udev_event_task->wq_entry.free = udev_event_freer;
					udev_event_task->op = 'r';

					// Raise version of disk to stop all existing local_disk related tasks.
					stock_local_disk->CHANGE_EVENT_counters.last_CHANGE_no++;
					udev_event_task->ldisk_id = *nvmeibt_local_disk_UUID(stock_local_disk);
					udev_event_task->native_serial = stock_local_disk->from_config.native_serial;
					udev_event_task->nsid = stock_local_disk->from_config.nsid;
					udev_event_task->vendor_id = nvmeibt_local_disk_vendor_id(stock_local_disk);
					udev_event_task->udev_event_info = udev_event_info;

					// put this message to a queue for the given disk parameters to be executed when it's time comes. (when there are no actives left.)
					if (nvmeibt_local_disk_specific_add_work(stock_local_disk, &udev_event_task->wq_entry) != 0) {
						N_Ef(zvq93kq, "Unable to add stock event for disk=@STR to WQ", nvmeibt_local_disk_display(stock_local_disk));
						NNVMEIBT_BM_FREE(t_fq_toma, udev_event_task);
						nvmeibt_abort(ES_FATAL);
					}
				} else {
					N_Tf(nckd59l, "dev_file=@PATH. No stock_local_disk", udev_event_info->dev_file_name);
					check_if_binding_to_stock_needed(udev_event_info);
				}
			}
		}
	}
}

/**
 * Reads the disks info from the stock driver directly:
 * * Disk characteristics
 * * Format characteristics for each disks
 *
 * @author max (2/19/18)
 *
 * @return int
 */
static void __attribute__((used)) read_disks_info_from_stock_driver(void)
{
	struct udev							*udev = NULL;
	struct udev_enumerate				*enumerate = NULL;
	struct udev_list_entry				*devices, *dev_list_entry;
	struct udev_device					*dev;
	const char							*path;
	enum nvmeibt_disk_type				disk_type;
	const char							*dev_file_name;
	struct nvmeibt_udev_event_info		*udev_event_info;
	bool								is_new;

	NFIN;

	udev = udev_new();
	if (!udev) {
		N_Ef(trace_no_udev_ctx, "Cannot create udev context");
		goto out;
	}

	enumerate = udev_enumerate_new(udev);
	if (!enumerate) {
		N_Ef(trace_no_enumerate, "Cannot create udev context");
		goto out;
	}

	/* scan all block devices for nvme/sata/vdisk */
	udev_enumerate_add_match_subsystem(enumerate, "block");
    udev_enumerate_add_match_property(enumerate, "DEVTYPE", "disk");
	udev_enumerate_scan_devices(enumerate);

	devices = udev_enumerate_get_list_entry(enumerate);
	if (!devices) {
		N_Ef(trace_no_devices, "Failed to get device list");
		goto out;
	}

	udev_list_entry_foreach(dev_list_entry, devices) {

		path = udev_list_entry_get_name(dev_list_entry);
		dev = udev_device_new_from_syspath(udev, path);

		dev_file_name = udev_device_get_devnode(dev);
		if (!dev_file_name) {
			N_Wf(mn8ub5v3, "dev_file_name empty path=@STR", path);
			goto loop_continue;
		}
		if (strlen(dev_file_name) > NVMEIBT_LOCAL_DISK_DEV_FILE_NAME_LEN) {
			N_Wf(mn8ub5v2, "dev_file_name=@STR is too long", dev_file_name);
			goto loop_continue;
		}
		disk_type = nvmeibt_local_disk_get_stock_disk_type_by_dev_file_name(dev_file_name, udev_device_get_devpath(dev));
		if (IS_SUPPORTED_STOCK_DISK_TYPE(disk_type)) {
			udev_event_info = find_udev_event_info_by_dev_file_name(dev_file_name, &is_new);
			if (!is_new) {
				N_Wf(sdfg765c, "dev_file_name=@STR somehow already exists", dev_file_name);
				goto loop_continue;
			}
			udev_event_info->disk_type = disk_type;
			nvmeibt_strlcpy(udev_event_info->dev_file_name, dev_file_name, sizeof(udev_event_info->dev_file_name));
			udev_event_info->act_to_process = nvmeibt_udev_add;
			udev_event_info->act_pending = nvmeibt_udev_none;
			udev_event_info->is_processing = false;
			N_Tf(jui8ua2, "new udev info, dev_file_name=@STR action=@INT", udev_event_info->dev_file_name, nvmeibt_udev_add);
		}
loop_continue:
		udev_device_unref(dev);
	}

out:
	if (udev)
		udev_unref(udev);
	if (enumerate)
		udev_enumerate_unref(enumerate);

	NFOUT;
}

/* receive udev event by TOMA main thread */
static void toma_wakeup_udev_event(void)
{
	struct nvmeibt_udev_event			e;
	struct nvmeibt_udev_event_info		*udev_event_info;
	enum nvmeibt_disk_type				disk_type;
	bool								is_new;

	NFIN;
	/* Make the call to receive the device. select() ensured that this will not block. */
	disk_type = nvmeibt_udev_get_event(&e);

	if (e.action != nvmeibt_udev_none) {
		udev_event_info = find_udev_event_info_by_dev_file_name(e.dev_file_name, &is_new);
		if (is_new) {
			udev_event_info->disk_type = disk_type;
			nvmeibt_strlcpy(udev_event_info->dev_file_name, e.dev_file_name, sizeof(udev_event_info->dev_file_name));
			udev_event_info->act_to_process = e.action;
			udev_event_info->act_pending = nvmeibt_udev_none;
			udev_event_info->is_processing = false;
			N_Tf(jui8uy7, "first udev info, dev_file_name=@STR action=@INT", udev_event_info->dev_file_name, udev_event_info->act_to_process);
		}
		else {
			if ((udev_event_info->is_processing) && (udev_event_info->act_to_process == nvmeibt_udev_add) && (e.action == nvmeibt_udev_add)) {
				// We cannot ignore 'add' events because it can be a trigger for additional attempt to work with the disk
				udev_event_info->act_pending = nvmeibt_udev_add;
				N_Tf(jui87yv, "additional add attempt dev=@STR", udev_event_info->dev_file_name);
			}
			else {
				if (udev_event_info->act_pending == nvmeibt_udev_none) {
					if (udev_event_info->act_to_process != e.action) {
						udev_event_info->act_pending = e.action;
						N_Tf(jui8uy0, "second udev info, dev=@STR action=@INT", udev_event_info->dev_file_name, udev_event_info->act_pending);
					}
					else {
						N_Tf(jui8uy9, "second udev info, dev=@STR same action, ignoring", udev_event_info->dev_file_name);
					}
				}
				else {
					if (udev_event_info->act_to_process == e.action) {
						N_Tf(jui8uy2, "third udev info, dev=@STR pending action=@INT canceled", udev_event_info->dev_file_name, udev_event_info->act_pending);
						udev_event_info->act_pending = nvmeibt_udev_none;
					}
					else {
						N_Tf(jui8uys, "third udev info, dev=@STR same action, ignoring", udev_event_info->dev_file_name);
					}
				}
			}
		}
	}

	nvmeibt_udev_put_event(&e);
	NFOUT;
}

void nvmeibt_abort(enum nvmeibt_error_severity es)
{
	static int	is_aborting = 0;
	extern bool toma_abort_nf;

	NFIN;
	if (!is_aborting) {
		is_aborting = 1;
		{
			int i;
			for (i = 0; i < 1000; i++) {
				N_Tf(vsghvfs, "---------------------------------------------------- Just filling the log, to avoid buffering 1 ------------------------------------------------");
			}
		}
		nvmeibt_toma_print_status();
		print_stack();
		N_ETf(trace_toma_nvmeibt_abort, "Toma Aborting. Please Look for previous " TOMA_ERR_STR "and/or " TOMA_WARN_STR ". Error code: 1080.");
		{
			int i;
			for (i = 0; i < 100; i++) {
				N_Tf(3vx9sfs, "---------------------------------------------------- Just filling the log, to avoid buffering 2 ------------------------------------------------");
			}
		}
		nvmeibt_toma_abort_child_threads_and_mem(false);	// Do not free memory to be able to debug the core file
		if (es == ES_FATAL || toma_abort_nf) {
			fprintf(stderr, "Fatal Error detected\n");
			abort();
		} else {
			fprintf(stderr, "Toma Non-Fatal Error detected\n");
		}
	}
	NFOUT;
}

static int nvmeibt_toma_init(int argc, char *argv[])
{
	char *last_slash_in_mypath;
	int rv;
	BOOL success = 0;
	time_t		cur_time_t;
	char		cur_time_t_str_no_newline[32];

	NFIN;
	/* ------------------------------------------- */
	/* START OF ALL STUFF THAT IS PTHREADS HOSTILE */
	/* ------------------------------------------- */
	/* extract basename of our executable */
	last_slash_in_mypath = strrchr(argv[0], '/');
	if (last_slash_in_mypath) {
		nvmeibt_strlcpy(executable_name, last_slash_in_mypath+1, sizeof(executable_name));
	} else {
		nvmeibt_strlcpy(executable_name, argv[0], sizeof(executable_name));
	}
	/* buffer manager */
	if (nvmeibt_bm_create()) {
		N_Ef(err_3_toma_nvmeibt_toma_init, "Failed to create buffer manager");
		goto out;
	}
	if (1) {				// Just varify that full stats dump can operate at any moment startign from this point, Even when some objects are uninitialized
		struct nvmeibt_Str		*dummy_print = NNVMEIBT_STR_ALLOC(ufkwl42);
		print_status_str(NVMEIBS_TOMA_STATUS_ALL, (nvmeibt_status_printf_fn_type)&nvmeibt_Str_sprintf, dummy_print);
		NNVMEIBT_STR_FREE(ctah81k, dummy_print);
	}

	signals_fd = init_signal_handling(executable_name);
	if (!nvmeibt_toma_is_running_as_a_utility()) {
		rsrm_faults_init_fifo_comm();
	}
	/* ----------------------------------------- */
	/* END OF ALL STUFF THAT IS PTHREADS HOSTILE */
	/* ----------------------------------------- */
	/* start our logger */
	time(&cur_time_t);
	nvmeibt_strlcpy(cur_time_t_str_no_newline, ctime(&cur_time_t), 25);
	N_IMf(trace_toma_nvmeibt_toma_init, // Do not change this trace id!!! it is used when filtering toma restarts, change will require to filter by both old and new id
		  "now='@STR' Starting TOMA (@STR), compiled @STR, @STR",
		  cur_time_t_str_no_newline, MOD_STR, __DATE__, __TIME__);
	N_IMf(trace_1_toma_nvmeibt_toma_init,
		  "commit-id=@STR branch=@STR build=@STR buildNumber=@STR sw_compatibility_ver=@X "
		  "client_proto_version=@X dictionary_checksum=@X",
		  GIT_COMMIT_ID, GIT_BRANCH, BUILD_VERSION_FOR_MGMT, BUILD_NUMBER_FOR_MGMT, TOMA_SW_COMPATIBILITY_VER,
		  NVMEIBT_CLIENT_PROTO_VERSION, (unsigned int)NVMEIB_DICTIONARY_CKSUM);
	{
		int i;
		struct nvmeibt_Str *cmdline_str = NNVMEIBT_STR_ALLOC(trace_2_toma_nvmeibt_toma_init);
		for (i = 0; i < argc; i++) {
			nvmeibt_Str_sprintf(cmdline_str, " %s", argv[i]);
		}
		N_IMf(trace_3_toma_nvmeibt_toma_init, "@STR", nvmeibt_Str_str(cmdline_str));
		NNVMEIBT_STR_FREE(trace_4_toma_nvmeibt_toma_init, cmdline_str);
	}
	if (!nvmeibt_toma_is_running_as_a_utility()) {
		if ((rv = nvmeibt_udev_create()) < 0) {
			N_Ef(trace_5_toma_nvmeibt_toma_init, "Can't create udev: rv=@RV", rv);
			goto out;
		}
		N_Tf(trace_8_toma_nvmeibt_toma_init, "udev_fd=@UDEV_FD",  rv);
	}

	nvmeibt_debug_init_tracer_sections();

	if (!nvmeibt_toma_is_running_as_a_utility()) {
		if ((rv = rsrm_init_work_tmq()) < 0) {
			N_Ef(rrtt978, "Can't create srm: rv=@RV", rv);
			goto out;
		}

		if (!(nw_node = nvmeibt_nm_init(toma_nm_transport_lib_path))) {
			N_Ef(ddii965, "Failed to start listeners");
			goto out;
		}
	}

	/* any attempts to use _T before this point will not output anything. */
	prepare_all_traces();
	nvmeibt_global_init();
	nvmeibt_common_init();
	read_rpc_config_from_persist(true);				// Durinng parameters settings checks global topology, so it must be initialized
	log_snapshotting_set_active_log_levels("High");	// Only after reading the RPC config, since we overide the persist
	/* set main thread id */
	nvmeibt_toma_set_main_thread();
	/* create internal toma wakeup */
	if (init_toma_wakeup() < 0) {
		N_Ef(fjju887, "Failed to setup internal toma wakeup");
		goto out;
	}
	// initialize raft
	if (nvmeibt_raft_one_time_init() != 0) {
		N_Ef(qqwo009, "Failed to do raft one time init");
		goto out;
	}
	if (nvmeibt_toma_is_running_as_a_utility()) {
		success = 1;
		goto out;
	}
	/* create work-queues */
	// Init local_disk wqs hash table.
	nvmeibt_global_get_global()->ldisks_wq_hash_by_ldisk_id_str = NVMEIB_HASH_CREATE(y92jiak, HASH_MIN_LOG2_OF_N_ARR_ENTRIES, "ldisk_wq_hash", -1);
	toma_persistency_wq = nvmeibt_wq_create("Persistency_io");
	if (!toma_persistency_wq) {
		N_Ef(fkitu66, "Failed to create wq persistency-offload");
		goto out;
	}
	recoveries_progress_wq = nvmeibt_wq_create("Recoveries");
	if (!recoveries_progress_wq) {
		N_Ef(trace_11_toma_nvmeibt_toma_init, "Failed to create wq recoveries-offload");
		goto out;
	}
	leader_wq = nvmeibt_wq_create("Toma_leader");
	if (!leader_wq) {
		N_Ef(skfji76, "Failed to create wq leader-offload");
		goto out;
	}
	stat_wq = nvmeibt_wq_create("Toma_stat");
	if (!stat_wq) {
		N_Ef(dkiyjgt, "Failed to create wq stat-offload");
		goto out;
	}
	read_disk_from_smart_wq = nvmeibt_wq_create("ReadDiskSmart");
	if (!read_disk_from_smart_wq) {
		N_Ef(ddko094, "Failed to create wq Read_disk_from_smart");
		goto out;
	}
	/*
	 * first read the management host and port -
	 * without it we have a total no go
	 */
	if (read_nvmesh_management_host() < 0) {
		N_Ef(cmkhjy7, "Failed to read management location");
		goto out;
	}
	/* set random seed to pid * current time */
	srand48(getpid() * time(NULL));

	/* initialize dumper (recording) subsystem */
	if (nvmeibt_dumper_init() < 0) {
		N_Ef(fkiyu88, "Failed to start dumper (recording) subsystem");
		goto out;
	}
	/* initialize recovery/rebuild subsystem */
	if (nvmeibt_recovery_init() < 0) {
		N_Ef(ttt6985, "Failed to start recovery/rebuild subsystem");
		goto out;
	}
	if (nvmeibt_local_disk_one_time_init() < 0) {
		N_Ef(6fgs8j3, "Failed local_disk_one_time_init()");
		goto out;
	}
	if (nvmeibt_seg_active_global_scrubbing_one_time_init() < 0) {
		N_Ef(6bdmclk, "Failed global_scrubbing_one_time_init()");
		goto out;
	}
	if (nvmeibt_global_reread_nvmesh_conf_as_needed() < 0) {
		N_Ef(tcvsj39, "Failed to read&parse '.nvmesh.conf'");
		goto out;
	}
	nvmeibt_server_lib_create();
	read_disks_info_from_stock_driver();
	(void)nvmeib_srvr_api_lib_server_connect(nvmeibt_get_srv_comm());

	if (nvmeibt_topology_probe_local_hardware(NVMEIBT_CSV_TYPE_LOCAL_NICS) < 0)
		nvmeibt_abort(ES_FATAL);	// Failed reading hardware config.
	if (!nvmeibt_toma_is_running_as_a_utility()) {
		if (nvmeibt_raft_read_persistence_and_upd_committed(toma_persistency_file_name, NULL) != 0) {
			N_Ef(cc77ru5, "Failed raft_read_toma_state_from_persistency()");
			goto out;
		}
	}
	success = 1;
out:
	if (!success){
		N_Ef(yuit867, "Bailing out...");
	}
	NFOUT;
	return success ? 0 : -1;
}

static int __attribute__ ((used)) run(int argc, char *argv[])
{
	int64_t 						epoll_timeout_ms;
	int64_t							now_millisec;
	int64_t							pselect_time_ms;
	struct timespec					last_idle_time_activities_time = TIMESPEC_ZERO;
	int64_t							time_since_last_idle_time_activities_nsec;
	int 							n_fds_returned;
	int 							i, fd;
	int								rv = -1;
	BOOL							did_pselect_allow_time_to_receive_append_entries = false;
	int 							raft_timeout_skip_ctr = 0;
	int 							read_cmdl_rv;
	struct nvmeibt_toma_fd_in_use	*trigger_fd;
	//
	struct nvmeibt_Str				*out_str;
	int								output_file_fd;
	int								n_written = 0;

	/* read command line */
	read_cmdl_rv = read_cmdl(argc, argv, 0);
	toma_is_running_as_a_utility = (nvmeibt_is_converting_json_to_persistence || nvmeibt_is_converting_persistence_to_json);
	nvmeibt_start_all_trace_pollers(nvmeibt_toma_is_running_as_a_utility());
	if (read_cmdl_rv < 0) {
		read_cmdl(argc, argv, 1);
	}
	// mcheck_pedantic(NULL);	// glibc's check alloc/free
	/* first thing register at_exit */
	atexit(nvmeibt_toma_abort_child_processes);
	/* only one TOMA instance alllowed */
	if (!nvmeibt_toma_is_running_as_a_utility() && nvmeibt_toma_is_single_instance() < 0) {
		N_ETf(trace_toma_run, "TOMA instance is already running...");
		goto exit;
	}
	if (nvmeibt_toma_init(argc, argv) < 0) {
		N_Ef(trace_1_toma_run, "Failed to initialize");
		goto exit;
	}
	if (nvmeibt_toma_is_running_as_a_utility()) {
		if (nvmeibt_is_converting_json_to_persistence) {
			if (nvmeibt_toma_cmdline_arg_input_file_name[0] && nvmeibt_toma_cmdline_arg_output_file_name[0]) {
				struct nvmeibt_persist_and_wire_buf		*out_persist_and_wire_buf;
				nvmeibt_mm_json_read_JSON_and_generate_persist_and_wire(nvmeibt_toma_cmdline_arg_input_file_name);
				out_persist_and_wire_buf = nvmeibt_raft_get_my_raft()->leader_to_commit_persist_and_wire_buf_full;
				output_file_fd = NNVMEIBT_OPEN(wmtuc7d, nvmeibt_toma_cmdline_arg_output_file_name, O_CREAT | O_WRONLY | O_TRUNC, 0755);
				if (output_file_fd >= 0) {
					n_written = NNVMEIBT_PWRITE(0m2huea, output_file_fd, out_persist_and_wire_buf,
												persist_and_wire_buf_get_total_len(out_persist_and_wire_buf), 0, 0);
					NNVMEIBT_CLOSE(x82oq0p, output_file_fd);
				}
			} else {
				N_Ef(va6734as9i234j, "");
			}
		} else if (nvmeibt_is_converting_persistence_to_json) {
			if (nvmeibt_toma_cmdline_arg_input_file_name[0] && nvmeibt_toma_cmdline_arg_output_file_name[0]) {
				out_str = NNVMEIBT_STR_ALLOC(vgsywje);
				nvmeibt_Str_sprintf(out_str, "{\n");
				nvmeibt_raft_read_persistence_and_upd_committed(nvmeibt_toma_cmdline_arg_input_file_name, out_str);
				nvmeibt_Str_sprintf(out_str, "\n}\n");
				output_file_fd = NNVMEIBT_OPEN(hd82k49, nvmeibt_toma_cmdline_arg_output_file_name, O_CREAT | O_WRONLY | O_TRUNC, 0755);
				if (output_file_fd >= 0) {
					n_written = NNVMEIBT_PWRITE(cbu9o2p, output_file_fd, nvmeibt_Str_str(out_str), nvmeibt_Str_strlen(out_str), 0, 0);
					NNVMEIBT_CLOSE(bhsykro, output_file_fd);
				}
			} else {
				N_Ef(macvgh3, "Missing files '@STR' '@STR'", nvmeibt_toma_cmdline_arg_input_file_name, nvmeibt_toma_cmdline_arg_output_file_name);
			}
		} else {
			N_Ef(r0wlsl8, "OOPS! Unexpected state");
		}
		N_IMf(v2d7ai3, "n_written=@INT input_file='@STR' output_file='@STR'", n_written, nvmeibt_toma_cmdline_arg_input_file_name, nvmeibt_toma_cmdline_arg_output_file_name);
	}
	if (nvmeibt_toma_is_running_as_a_utility()) {
		goto out;
	}
	/* let's go to work... */
	if (nvmeibt_kafka_launch() < 0) {
		goto exit;
	}

	do {
		struct timespec now;
		int select_errno;

		if (is_need_to_update_the_main_select_fds) {
			if (epoll_fd > 0)
				NNVMEIBT_CLOSE(skxu7ct, epoll_fd);
			epoll_fd = NNVMEIBT_EPOLL_CREATE1(0);
			update_read_and_exception_select_fds(&g_fds_in_use);
			is_need_to_update_the_main_select_fds = false;
		}
		// Calculate the epoll_timeout
		getnstimeofday_boot(&now);
		now_millisec = timespec_to_msec(now);
		epoll_timeout_ms = 1000;	// Start from max of 1 sec. Probably the other timeouts will pull it down

		if (nvmeibt_topology_is_HW_config_functional()) {
			const int64_t raft_diff_ms =     timespec_diff_ms(nvmeibt_raft_get_next_timeout_timespec(),     now);
			const int64_t register_diff_ms = timespec_diff_ms(nvmeibt_register_get_next_timeout_timespec(), now);
			const int64_t recovery_diff_ms = timespec_diff_ms(nvmeibt_recovery_get_next_timeout_timespec(), now);
			N_Tf(hdt6g4d, "timeouts-ms: raft=@LLD register=@LLD recovery=@LLD", raft_diff_ms, register_diff_ms, recovery_diff_ms);
			epoll_timeout_ms = min(epoll_timeout_ms, raft_diff_ms);
			epoll_timeout_ms = min(epoll_timeout_ms, register_diff_ms);
			epoll_timeout_ms = min(epoll_timeout_ms, recovery_diff_ms);
			epoll_timeout_ms = max(epoll_timeout_ms, 1);	// Clip to 1ms..1sec
		}
		N_Tf(trace_9_toma_run, "epoll_timeout=@LLU", epoll_timeout_ms);
		n_fds_returned = epoll_wait(epoll_fd, epoll_events, g_fds_in_use.max_fd_no + 1, epoll_timeout_ms);
		select_errno = errno;

		getnstimeofday_boot(&(nvmeibt_global_get_global()->cur_event_start_time));
		pselect_time_ms = timespec_to_msec(nvmeibt_global_get_cur_event_start_time());
		pselect_time_ms -= now_millisec;
		did_pselect_allow_time_to_receive_append_entries = (pselect_time_ms > 1);
		if (!did_pselect_allow_time_to_receive_append_entries) {
			if (raft_timeout_skip_ctr == 25) {
				raft_timeout_skip_ctr = 0;
				did_pselect_allow_time_to_receive_append_entries = 1; // force in case we skipped to many iterations due to to slow select timeouts.
			}
			raft_timeout_skip_ctr++;
		}
		else {
			raft_timeout_skip_ctr = 0;
		}
		N_Tf(trace_10_toma_run, "pselect_time=@LLD n_fds_returned=@N_FDS_RETURNED "
			 "did_pselect_allow_time_to_receive_append_entries=@DID_PSELECT_ALLOW_TIME_FOR_MSGS raft_timeout_skip_ctr=@RAFT_TIMEOUT_SKIP_CTR",
			 pselect_time_ms, n_fds_returned, did_pselect_allow_time_to_receive_append_entries, raft_timeout_skip_ctr);
		if (pselect_time_ms > epoll_timeout_ms + 100) {
			N_Tf(trace_end_of_life_toma_run_0, "select timeout was @LLD, "
				"select spent @LLD.  select() did not respect, big time, "
				"the timeout provided to it", epoll_timeout_ms, pselect_time_ms);
			//nvmeibt_abort(ES_FATAL);
		}
		// ensure signal events are handled before rest of events
		for (i = 0; i < n_fds_returned; i++) {
			trigger_fd = (struct nvmeibt_toma_fd_in_use *)epoll_events[i].data.ptr;
			if (trigger_fd->fd == signals_fd)
				handle_sig_fd(signals_fd, toma_sig_handler_fn);
		}
		nvmeibt_nm_rsrm_resend_acks(nw_node);
		// If the epoll_timeout was too short || (returned due to n_fds_returned after a very short time)
		if ((shutdown_status == ds_none) && (n_fds_returned == -1 || received_sig_no)) {
			N_Tf(trace_11_toma_run, "select() errno=@ERRNO_STR received_sig_no=@RECEIVED_SIG_NO shutdown_status=@SHUTDOWN_STATUS", strerror(select_errno), received_sig_no, shutdown_status);
			if ((select_errno > 0 && select_errno != EINTR) || received_sig_no) {
				nvmeibt_toma_on_shutdown_me_only(0, false);
				received_sig_no = 0;
			}
		} else if (n_fds_returned > 0) {
			// Handle the events
			for (i = 0; i < n_fds_returned; i++) {
				trigger_fd = (struct nvmeibt_toma_fd_in_use *)epoll_events[i].data.ptr;
				if (trigger_fd->fd == signals_fd) // already processed
					continue;
				fd = trigger_fd->fd;
				N_Tf(trace_16_toma_run, "read_select event fd=@FD type=@TYPE", fd, trigger_fd->fd_type);
				switch (trigger_fd->fd_type) {
				case NVMEIBT_TOMA_FD_TYPE_IB:
					if (nvmeibt_topology_is_HW_config_functional()) {
						//_Tf("NVMEIBT_TOMA_FD_TYPE_IB\n");
						last_ib_event_timespec = nvmeibt_global_get_cur_event_start_time();
						nvmeibt_nm_process_toma_requests(nw_node);
					}
					break;
				case NVMEIBT_TOMA_FD_TYPE_TOMA_WAKEUP:
					last_toma_wakeup_event_timespec = nvmeibt_global_get_cur_event_start_time();
					toma_wakeup_event();
					break;
				case NVMEIBT_TOMA_FD_TYPE_TOMA_SRM_RESEND_TIMER:
					rv = nw_node ? nvmeibt_nm_rsrm_send_timer(nw_node) : rsrm_resend_timer();
					break;
				case NVMEIBT_TOMA_FD_TYPE_SYSTEM_EVENTS:
					break;	// Handled earlier
				case NVMEIBT_TOMA_FD_TYPE_UDEV_EVENTS:
					toma_wakeup_udev_event();
					break;
				case NVMEIBT_TOMA_FD_TYPE_FIFO_COMM:
					nvmeibt_nm_rsrm_faults_handle_fifo_com(nvmeibt_get_nw_node());
					break;
				default:
					N_Ef(trace_17_toma_run, "Unsupported FD type: @FD_TYPE", trigger_fd->fd_type);
					break;
				}
			}
		}

		// Handle the expired timeouts
		getnstimeofday_boot(&now);	// Do not re-sample "now". as we do not handle incoming messages
		now_millisec = timespec_to_msec(now);
		//_Tf("Select timeout\n");
		if (timespec_ge(now, nvmeibt_raft_get_next_timeout_timespec())) {
			last_raft_timeout_timespec = now;
			nvmeibt_nm_process_toma_requests(nw_node);
			if (nvmeibt_raft_timeout_occurred(did_pselect_allow_time_to_receive_append_entries, 0) < 0) {
				N_Tf(trace_22_toma_run, "Failed to handle raft timeout...");
				rv = -1;
				goto out;
			}
		}
		if (timespec_ge(now, nvmeibt_recovery_get_next_timeout_timespec())) {
			last_recovery_timeout_timespec = now;
			if (nvmeibt_recovery_timeout_occurred() < 0) {
				N_Tf(trace_25_toma_run, "Failed to handle recovery timeout...");
				rv = -1;
				goto out;
			}
		}
		if (timespec_ge(now, nvmeibt_register_get_next_timeout_timespec())) {
			last_register_timeout_timespec = now;
			if (nvmeibt_register_timeout_occurred() < 0) {
				N_Tf(trace_26_toma_run, "Failed to handle register timeout...");
				rv = -1;
				goto out;
			}
		}

		if (got_sigusr1) {
			nvmeibt_toma_print_status();
			nvmeibt_flush_all_traces();
			got_sigusr1 = 0;
		}
		if (got_sigusr2) {
			N_Tf(trace_28_toma_run, "Toggle logging state (0)");
			nvmeibt_flush_all_traces();
			nvmeibt_toggle_logging();
			N_Tf(trace_29_toma_run, "Toggle logging state (1)");
			got_sigusr2 = 0;
		}
		at_event_end_activities();
		time_since_last_idle_time_activities_nsec = timespec_diff_ns(now, last_idle_time_activities_time);
		if (time_since_last_idle_time_activities_nsec > MSEC_TO_NSEC(100) || (n_fds_returned == 0 && time_since_last_idle_time_activities_nsec > MSEC_TO_NSEC(50))) {
			last_idle_time_activities_time = now;
			nvmeibt_global_idle_time_activities();
		}
	} while (1);
out:
	NNVMEIBT_CLOSE(skxudt5, epoll_fd);
exit:
	N_IMf(trace_30_toma_run, "exiting...");
	NFOUT;

	nvmeibt_toma_abort_child_processes();
	return rv;
}

extern int gpt_util_main(int argc, char *argv[]);

static int run_dummy_empty(int argc, char *argv[])
{
	(void) argc; (void)argv;
	fprintf(stderr, "Running dummy util: n_args=%d, exe=%s\n", argc, argv[0]);
	return 0;
}

struct {
	char *name;
	int (*func)(int argc, char **argv);
} toma_subprogs[] = {
		{ TOMA_THREAD_NAME, run },
		{ "gpt_util", gpt_util_main },
		{ "dummy", run_dummy_empty },
};

int main(int argc, char *argv[]) {
	const char *base = strrchr(argv[0], '/') + 1;		// not using basename() since it may alter the argument
	int i, arg_shift = 0, subprog = 0;
	for (i=0; i<ARRAY_SIZE(toma_subprogs); i++) {		// Select sub program by exe name
		if (strcmp(base, toma_subprogs[i].name)==0) {
			subprog = i;
			break;
		}
	}
	if ((subprog == 0) && (argc > 1)) {
		for (i=0; i<ARRAY_SIZE(toma_subprogs); i++) {	// Select sub program by first argument
			if (strcmp(argv[1], toma_subprogs[i].name)==0) {
				subprog = i;
				arg_shift = 1;
				break;
			}
		}
	}
	return toma_subprogs[subprog].func(argc-arg_shift, argv+arg_shift);
}

