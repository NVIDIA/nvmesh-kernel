#ifndef NVMEIBT_DUMPER
#define NVMEIBT_DUMPER

/*
 * Dump files are stored in:
 *   TOMA_LOG_DIR/toma-topo-record/TIMESTAMP
 *   (where TIMESTAMP is from the first dump this run)
 */
#define TOMA_DUMPER_SUBDIR       "toma-topo-record"
#define TOMA_DUMPER_DIR          TOMA_LOG_DIR "/" TOMA_DUMPER_SUBDIR

/*
 * Dump file contains multiple records in tihs format:
 *   \n
 *   $(TOMA_DUMPER_HEADER) $(TIME_STAMP) $(SNIPPET_ID)
 *   size=%-7zu raft=0x%-8x topo=0x%-8x event=%-6d type=%s
 *   \n
 *   $(SNIPPET)
 *
 * Where:
 *   TOMA_DUMPER_HEADER: defined below
 *   TIME_STAMP: %Y-%m-%d-%T
 *   SNIPPET_ID: 0x%08x_0x%08x_%04d_%s
 *   SNIPPET: the saved buffer
 */
#define TOMA_DUMPER_HDR          "TOMA_TOPO_RECORD"
#define TOMA_DUMPER_HDRLEN       strlen(TOMA_DUMPER_HDR)

#define TOMA_DUMPER_HDR_FORMAT \
		"\n" \
		TOMA_DUMPER_HDR " %s " "0x%08x_0x%08x_%04d_%s" "\n" \
		"size=%-8zu raft=0x%-8x topo=0x%-8x event=%-6d type=%s" "\n" \
		"\n"

/*
 * SNIPPET_ID position and length:
 *   pos: len(TOMA_DUMPER_HDR) + space + len(timestamp) + space
 *   len: 10 (raft) + _ + 10 (topo) + _ + 6 (event) + _ + 4 (type)
 */
#define SNIPPET_ID_POS (TOMA_DUMPER_HDRLEN + 1 + 19 + 1)
#define SNIPPET_ID_LEN (10 + 1 + 10 + 1 + 4 + 1 + 4)
/*
 * SNIPPET_SIZE position and length:
 *   pos: 4 ('size') + 1 ('=')
 *   len: 8 digits (at most)
 */
#define SNIPPET_SIZE_POS (4 + 1)
#define SNIPPET_SIZE_LEN (8)

/* snippet event types */

#define NVMEIBT_DUMPER_TYPE_MGMT "mgmt"
#define NVMEIBT_DUMPER_TYPE_RAFT "raft"
#define NVMEIBT_DUMPER_TYPE_TOPO "topo"
#define NVMEIBT_DUMPER_TYPE_PEER "peer"
#define NVMEIBT_DUMPER_TYPE_DISK "disk"

/* API to enable/disable recording */

void nvmeibt_dumper_enable(BOOL on);

/*
 * During replay skip work that does not belong to the leader, such as
 * filesystem/disk operations, hardware reading, mcs/mgmt communication,
 * and so on:
 *  - We only need to replay the leader's logic and how it calculates new
 *    topologies based on config and follower reports.
 *  - Decouple replay from the original execution environment (hardware
 *    and management) where recording took place originally.
 */
bool nvmeibt_replay_is_enabled(void);

/*
 * API for TOMA to inform about change of leader: only the leader does any
 * recording (followers do not).
 */
void nvmeibt_dumper_change_of_leader(BOOL leader);

/* API to be called by TOMA code where/when things need to be dumped: */
struct nvmeibt_topology;
struct nvmeibt_disk;
void nvmeibt_dumper_event_mgmt_config(void);
void nvmeibt_dumper_event_raft_persist(void);
void nvmeibt_dumper_event_global_topo(void);
void nvmeibt_dumper_event_peer_applied(const char *config_str);
void nvmeibt_dumper_event_remove_disk(struct nvmeibt_disk *disk);

/* API to init/exit recording subsystem */

int nvmeibt_dumper_init(void);
void nvmeibt_dumper_exit(void);

#endif  /* NVMEIBT_DUMPER */
