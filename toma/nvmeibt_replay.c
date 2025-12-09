#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <time.h>
#include <getopt.h>
#include <libgen.h>

#include "nvmeibt_debug.h"
#include "nvmeibt_common.h"
#include "nvmeibt_bm.h"
#include "nvmeibt_raft.h"
#include "nvmeibt_disk.h"
#include "nvmeibt_toma.h"
#include "nvmeibt_topology.h"
#include "nvmeibt_global.h"
#include "interfaces/log/nvmeibt_dumper.h"
#include "nvmeibt_topo_bin.h"
#define TOPO_RECORD             0
#define INFO(x...) ({})		// Just in order to compile with nvmeibt_dumper
/*
 * record-replay: during replay we need a private location for logs/files, to
 * avoid dependency on recording environment and to avoid ovewriting existing
 * data - so we use relative paths.
 */
#ifdef TOMA_REPLAY
#define TOMA_FILE_DIR TOMA_DIR_OPT_NVMESH "/toma_rep"
#else
#define TOMA_FILE_DIR TOMA_DIR_OPT_NVMESH "/toma"
#endif
/*
 * TOMA record-replay is a tool for QA to compare the output of different
 * versions/releases given similar input data. The tool records important
 * TOMA events during runtime, and can replay them as input to a separate
 * run possibly of a different version. Recording live systems is useful
 * to generate a collection of real world scenarios for testing.
 *
 * The primary goal of replay is to validate the behavior and correctness
 * of global topology calculation: we feed the inputs seen from recording
 * and then ask for topology recalculation and compare the outcome to the
 * one recorded.
 *
 * (Therefore, replaying does not replay everything, but instead feeds the
 * recorded data through the functions that had triggered the recording of
 * events originally).
 *
 * The replay starts with (partial) initialization mimicking what the real
 * TOMA service does. It then loops over reading event snippets from dump
 * files and feeding them into the system:
 *
 * . "mgmt": call into nvmeibt_topology_apply_mgmt_config()
 * . "raft": call into nvmeibt_parse_csv_buf()
 * . "topo": call into nvmeibt_topology_calc_topology()
 * . "peer": call into nvmeibt_disk_segment_leader_upd_from_peer_applied()
 * . "disk": call into replay_remove_disk_from_node()
 *
 * The first event in each session is "mgmt" (initial management config),
 * and the second event in each session is "raft" (the raft persistency,
 * including the initial global topology).
 * Within each session, the first event in the second sequence onward is
 * "topo" (new calculated topology), and the second event is "raft".
 *
 * A typical example of one session:
 *                                          ratf#     topology  evnt type
 *                                        ---------- ---------- ---- ----
 *   TOMA_TOPO_RECORD 2019-07-28-11:17:31 0x0000000b_0x0a00000a_0000_mgmt
 *   TOMA_TOPO_RECORD 2019-07-28-11:17:31 0x0000000b_0x0a00000a_0001_raft
 *   TOMA_TOPO_RECORD 2019-07-28-11:17:31 0x0000000b_0x0a00000a_0002_peer
 *   TOMA_TOPO_RECORD 2019-07-28-11:17:31 0x0000000b_0x0a00000a_0003_peer
 *   TOMA_TOPO_RECORD 2019-07-28-11:17:32 0x0000000b_0x0b00000b_0000_topo
 *   TOMA_TOPO_RECORD 2019-07-28-11:17:32 0x0000000b_0x0b00000b_0001_raft
 *   TOMA_TOPO_RECORD 2019-07-28-11:17:32 0x0000000b_0x0b00000b_0002_peer
 *   TOMA_TOPO_RECORD 2019-07-28-11:17:32 0x0000000b_0x0b00000b_0003_peer
 *   TOMA_TOPO_RECORD 2019-07-28-11:17:32 0x0000000b_0x0b00000d_0000_topo
 *   TOMA_TOPO_RECORD 2019-07-28-11:17:33 0x0000000b_0x0b00000d_0001_raft
 *   TOMA_TOPO_RECORD 2019-07-28-11:17:33 0x0000000b_0x0b00000d_0002_peer
 *   TOMA_TOPO_RECORD 2019-07-28-11:17:33 0x0000000b_0x0b00000d_0003_peer
 *   TOMA_TOPO_RECORD 2019-07-28-11:17:33 0x0000000b_0x0b00000d_0004_disk
 *   TOMA_TOPO_RECORD 2019-07-28-11:17:33 0x0000000b_0x0b00000e_0000_topo
 *   ...
 *
 * For events of type "topo", replay recalculates the global topology via
 * nvmeibt_topology_calc_topology(), and then compares the outcome to the
 * payload of the event.
 */

static const char *usage_name;
static const char *usage_line = ""
/* Usage: %s */ "[OPTION...] PATH[...]\n"
"\n"
"Replay TOMA events from recorded sessions, and compare the results\n"
"to the recorded results.\n"
"";

static const char *usage_long = ""
"  PATH[...]          : path(s) to dump file\n"
"  -o, --outdir=DIR   : use DIR as root directory for log/opt\n"
"  -d, --debug        : debug mode (no fork, one path only)\n"
"  -v, --verbose      : verbose progress reporting \n"
"  -h, --help         : print this help message\n"
"";

static BOOL usage_opt_debug = 0;
static BOOL usage_opt_verbose = 0;
static const char *usage_opt_outdir = NULL;

static BOOL replay_parent = 1;  /* true if we are the parent of all replays */

static int watchdog_tick;  /* canary to detect hangs (see replay_watchdog) */

static int replay_cwd_fd;  /* file descriptor for (original) current directory */
static const char **paths_arr;  /* array of paths from which to replay */
static int paths_cnt;  /* count of paths to replay in array */

static int replay_fd;  /* file descriptor for dump file */

static struct nvmeibt_Str *persistency_ctx;  /* persistent/last global topo */
static struct nvmeibt_topology *global_topology;     /* global topology being replayed */
struct nvmeibt_topology		*nvmeibt_global_get_global(void);

/*
 * nvmeibt_replay_is_enabled(): used by TOMA code to decide whether to skip
 * logic that should not execute during replay. For example, TOMA need not
 * communciate with the management server.
 */
static bool replay_is_enabled = 0;

bool nvmeibt_replay_is_enabled(void)
{
	return replay_is_enabled;
}

/*
 * Virtual time:
 *
 * In addition to event/messages (from management and from peers), TOMA relies
 * on timeouts and time-differences. The below helpers provide simluated time.
 *
 * The simulated time does not reproduce the original timing (may add later if
 * needed); Instead it wokrs as follows:
 *
 *   . Time begins at second "1".
 *   . Ticks by 1 second with each event.
 *   . Bumped by 60 seconds after "mgmt"/"raft" complete (see below)
 *
 * The simulated time is kept on @topology->cur_event_start_time.
 */

static void replay_time_set(struct timespec ts)
{
	global_topology->cur_event_start_time = ts;
}

static void replay_time_init(void)
{
	struct timespec ts;

	ts.tv_sec = 1;
	ts.tv_nsec= 9000;

	replay_time_set(ts);
}

static void replay_time_tick(int64_t tv_sec)
{
	global_topology->cur_event_start_time.tv_sec += tv_sec;
}

static struct timespec replay_time_get(void)
{
	return global_topology->cur_event_start_time;
}

/*
 * replay_time_adjust(): adjust timestamps after initialized completed
 */
static void replay_time_adjust(void)
{
	struct nvmeibt_praid *praid;

	NFIN;

	/*
	 * For new raid segment, if TOMA reveices peer report saying "inactive",
	 * it will wait a grace-period before marking "degraded" - maybe other
	 * peers will shortly report it working. This "timer" is set at raid's
	 * first topology calculation.
	 * Replay starts from random state, so the first topology calculation
	 * always starts this grace priod, although it (nearly always) was not
	 * so during recording.
	 * To workaround, we force no-grace-period by placing an old timestamp
	 * on all raids, and advancing the virtual time by one minute (currently
	 * the grace period is 20 seconds).
	 * TODO: this is partial solution, because if in recording there really
	 * was a grace period, we won't regenerate it here.
	 */
	XHASHTABLE_FOR_EACH_SAFE(praid, &nvmeibt_global_get_global()->praids_hash) {
		praid->praid_leader.first_activation_attempt_timespec = replay_time_get();
	}

	NFOUT;
}

/*
 * replay_event_mgmt_config(): feed a recorder global configuration that
 * arrived from the management server.
 */
static int replay_event_mgmt_config(const char *in_buf)
{
	int ret = -1;
	struct nvmeibt_Buf buf;

	NFIN;

	INFO(TOPO_RECORD,
		 "****REPLAY TOPO:****REPLAY MGMT:****\n%s\n",
		 in_buf);

	nvmeibt_topology_mark_update_csv_of_config_and_topo_required();

	/*
	 * The second arg indicates the config version, and will be compared
	 * (there) to the version found the csv buffer. The special value -1
	 * tells the callee to trust the csv buf and skip validation.
	 */
	buf.data_buf = (char *)in_buf;
	buf.buf_len = strlen(in_buf);
	N_Ef(cvghvws, "I didn't try to make it work. Either parse the full persistence of pass just the volume-config, what about the HW-config @PTR", &buf);
	if (!nvmeibt_topology_is_HW_config_functional()) {
		N_Ef(error_1_replay_replay_event_mgmt_config, "!nvmeibt_topology_is_HW_config_functional() after parse mgmt config");
		goto out;
	}

	ret = 0;

out:
	NFOUT;
	return ret;
}

/*
 * replay_event_raft_persist(): load the raft persistency (initial
 * global topology)
 *
 * This is done at the beginning, and only once - similar to loading the
 * global topology from persistency store.
 */
static int replay_event_raft_persist(const char *buf)
{
	int ret = -1;

	NFIN;

	nvmeibt_Str_strcpy(persistency_ctx, buf);

	INFO(TOPO_RECORD,
		 "****REPLAY TOPO:****INITIAL RAFT:****\n%s\n",
		 buf);

	if (nvmeibt_Str_strlen(persistency_ctx) == 0) {
		N_Ef(error_replay_replay_event_raft_persist, "Failed to process event raft persist (zero length)");
		goto out;
	}

	nvmeibt_raft_convert_to_leader();
	nvmeibt_raft_get_my_raft()->is_leader_mature = 1;

	ret = 0;

out:
	NFOUT;
	return ret;
}

static int replay_check_toma_retval(enum nvmeibt_add_rv retval)
{
	int ret;

	NFIN;

	switch (retval) {
	case NVMEIBT_ADD_NEW:
	case NVMEIBT_ADD_SKIPPED:
		ret = 0;
		break;
	case NVMEIBT_ADD_FAILED:
	case NVMEIBT_ADD_FAILED_OTHERS_FUNCTIONAL:
		N_Ef(error_replay_replay_check_toma_retval, "Failed to process event (NVMEIBT_ADD_FAILED)");
		ret = -1;
		break;
	default:
		N_Ef(error_1_replay_replay_check_toma_retval, "Failed to process event (unknown @RETVAL)", retval);
		ret = -1;
		break;
	}

	NFOUT;

	return ret;
}

/*
 * replay_event_peer_applied(): feed an update with an applied topology
 * from a peer.
 */
static int replay_event_peer_applied(__attribute__((__unused__)) const char *buf)
{
//	static int serialization = 1;  /* dummy seerialization_version */
	int ret;

	NFIN;

	INFO(TOPO_RECORD,
		 "****REPLAY TOPO:****REPLAY PEER:****\n%s\n",
		 buf);

#if 0
	ret = nvmeibt_disk_segment_leader_upd_from_peer_applied(buf, NULL, serialization++);

#else
	INFO(TOPO_RECORD,
		 "NOT IMPLEMENTED, MUST USE BIN TOPO\n");
	ret = -1;
#endif
	ret = replay_check_toma_retval(ret);

	NFOUT;

	return ret;
}

/*
 * replay_event_remove_disk(): feed an event of disk removal from a node.
 */
static int replay_event_remove_disk(const char *buf)
{
	struct nvmeibt_urn_uuid disk_id;
	struct nvmeibt_urn_uuid disk_nd;
	struct nvmeibt_urn_uuid node_id;
	union nvmeib_uuid disk_id_h;
	union nvmeib_uuid disk_nd_h;
	union nvmeib_uuid node_id_h;
	struct nvmeibt_disk *disk;
	int ret = 0;

	NFIN;
	INFO(TOPO_RECORD,
		 "****REPLAY TOPO:****REPLAY PEER:****\n%s\n",
		 buf);

	/*
	 * Removal of disk is an internal event, so there isn't a standardized
	 * CSV representation that we can (re)use.
	 * Instead, dumper generates a string to identify the disk, which replay
	 * will use to locate the disk in its @cur_topo data structure.
	 * Dumper writes: disk id, its original node id, and its node leader id.
	 * Replay reads all three, but only uses the disk id to locate the disk;
	 * The other two ids are used for sanity tests only.
	 * Same comment in toma/nvmeibt_dumper.c:nvmeibt_dumper_event_remove_disk()
	 */
	ret = nvmeibt_sscanf_csv_line(buf,
								  'g', &disk_id.str,
								  'g', &disk_nd.str,
								  'g', &node_id.str,
								  '\0');

	if (ret <= 0) {
		N_Ef(error_replay_replay_event_remove_disk, "scan failed on @BUF_STR", buf);
		ret = -1;
		goto out;
	}

	N_Df(debug_replay_replay_event_remove_disk, "remove disk: disk id   @STR", disk_id.str);
	N_Df(trace_1_replay_replay_event_remove_disk, "remove disk: disk node @STR", disk_nd.str);
	N_Df(trace_2_replay_replay_event_remove_disk, "remove disk: node id   @STR", node_id.str);

	nvmeibt_urn_uuid_to_union_uuid(&disk_id_h, &disk_id);
	nvmeibt_urn_uuid_to_union_uuid(&disk_nd_h, &disk_nd);
	nvmeibt_urn_uuid_to_union_uuid(&node_id_h, &node_id);

	ret = -1;

	/* let's find the damn disk ... */
	disk = NNVMEIBT_HASH_GET_OBJ_BY_UUID(trace_replay_replay_event_remove_disk, &nvmeibt_global_get_global()->disks_hash, &disk_id_h, disk);

	if (disk == NULL) {
		N_Ef(euu833n, "disk id @UUID_LE not found in topo disks", nvmeibt_disk_UUID(disk));
		goto out;
	}

	/* sanity checks of the match: compare original and leader node id */
	if (!ARE_UUID_EQ(&disk->from_config.its_original_node_id, &disk_nd_h)) {
		N_Ef(jhii962, "disk original node mismatch @UUID_LE/@UUID_LE", &disk->from_config.its_original_node_id, &disk_nd_h);
		goto out;
	}
	if (!ARE_UUID_EQ(nvmeibt_raft_member_id(disk->leader_its_raft_member), &node_id_h)) {
		N_Ef(djiru83, "disk leader member mismatch @UUID_LE/@UUID_LE", nvmeibt_raft_member_id(disk->leader_its_raft_member), &node_id_h);
		goto out;
	}

	/* found the disk: now proceed to remove it */
	nvmeibt_topology_leader_remove_disk_from_its_current_raft_member(disk);

	ret = 0;

out:
	NFOUT;
	return ret;
}

/*
 * replay_event_global_topo(): initiate a new global topology calculation
 *
 * The new calculation will be based on the aggregate updates thus far, Once
 * done, we will compare the outcome with the global topology form the dump
 * file: we expect them to be the same.
 */
static int replay_event_global_topo(const char *buf)
{
	struct nvmeibt_Buf *topo_buf;
	int ret = -1;
	struct nvmeibt_Str *print_s;

	NFIN;

	INFO(TOPO_RECORD,
		 "****REPLAY TOPO:****RECALC TOPO:****\n%s\n",
		 buf);

	nvmeibt_topology_calc_topology();

	if (1 /*strcmp(buf, nvmeibt_Str_str(csv_ctx)) != 0*/) {
		topo_buf = &(nvmeibt_raft_get_my_raft()->leader_to_commit_wire_topo);
		print_s = NNVMEIBT_STR_ALLOC(u74yhg2);
		NNVMEIBT_STR_RESIZE_BUF(lgoktu8, print_s, 8192);
		nvmeibt_topology_print((nvmeibt_status_printf_fn_type)&nvmeibt_Str_sprintf, print_s, topo_buf, 1);
		NVMEIBT_LONG_TRACE_WRAPPER(uy7654f, "REPLY TOPOLOGY", nvmeibt_Str_str(print_s), nvmeibt_Str_strlen(print_s));

		N_Wf(warn_1_replay_replay_event_global_topo, "********************************************************");
		N_Wf(warn_2_replay_replay_event_global_topo, "***** mismatch in computed and recorded topologies *****");
		N_Wf(warn_3_replay_replay_event_global_topo, "***** recorded len: @STRLEN, computed len: @STRLEN",
			strlen(buf), topo_buf->buf_len);
		N_Wf(warn_4_replay_replay_event_global_topo, "********************************************************");
		N_Wf(warn_5_replay_replay_event_global_topo, "***** recorded topology ********************************");
		N_Wf(warn_6_replay_replay_event_global_topo, "\n\n@BUF_STR", buf);
		N_Wf(warn_7_replay_replay_event_global_topo, "********************************************************");
		N_Wf(warn_8_replay_replay_event_global_topo, "***** computed topology ********************************");
		N_Wf(warn_9_replay_replay_event_global_topo, "\n\n@STR", nvmeibt_Str_str(print_s));
		N_Wf(warn_10_replay_replay_event_global_topo, "********************************************************");
		NNVMEIBT_STR_FREE(ritl982, print_s);
		goto out;
	}

	ret = 0;

out:
	NFOUT;
	return ret;
}

/*
 * replay_free_buf(): free a buffer returned by replay_read_snippet()
 */
static void replay_free_buf(char *buf)
{
	NNVMEIBT_TOMA_FREE(trace_replay_replay_free_buf, buf);
}

/*
 * replay_alloc_buf(): allocate a buffer for replay_read_snippet()
 */
static char *replay_alloc_buf(size_t size)
{
	return NNVMEIBT_TOMA_CALLOC(trace_replay_replay_alloc_buf, 1, size);
}

static void replay_atexit(void)
{
	/*
	 * The replay worker must exit somewhat silently, without invoking
	 * atexit() hooks, pthread calls, and other cleanups that would then
	 * interfere with the parent's expectations. The final cleanup will
	 * be done by the parent.
	 * The replay code itself is careful to call _exit() to ensure that
	 * replay workers terminate silently without side-effects; However,
	 * replay workers may exit from elsewhere instead of here (any of the
	 * TOMA functions we use may decide to exit upon error).
	 * Therefore, only invoke the actual callback if we are the parent.
	 */
	if (replay_parent)
		nvmeibt_toma_abort_child_processes();
}

/*
 * replay_exit(): clean up before exiting
 */
static void replay_exit(void)
{
	NFIN;

	NNVMEIBT_STR_FREE(trace_replay_replay_exit, persistency_ctx);
	NNVMEIBT_TOMA_FREE(trace_1_replay_replay_exit, global_topology);
	NNVMEIBT_TOMA_FREE(trace_2_replay_replay_exit, paths_arr);

	/*
	 * TODO: should do proper cleanup of all data like nvmeibt_toma.
	 * For now, since we do expect to exit anyway soon, just skip it.
	 *
	 * If we do this, then need to be picky on what cleanups allowed
	 * for the replay workers (forks).
	 */

	NFOUT;
}

/*
 * replay_init(): initialize myself
 * modelled after nvmeibt_toma_init()
 */
static int replay_init(void)
{
	int ret = -1;

	NFIN;

	/* do not record during replay... */
	nvmeibt_dumper_enable(0);

	/* initialize logger and logging */
	atexit(replay_atexit);  /* will call nvmeibt_toma_exit() if needed */

	/* buffer manager */
	if (nvmeibt_bm_create()) {
		N_Ef(error_replay_replay_init, "Failed to create buffer manager");
		goto out;
	}

	/* must come first: earlier use of _T will not output anything */
	prepare_all_traces();

	/* set main thread id */
	nvmeibt_toma_set_main_thread();

	/* allocate and initialize data structures */

	persistency_ctx = NNVMEIBT_STR_ALLOC(trace_replay_replay_init);
	global_topology = NNVMEIBT_TOMA_CALLOC(trace_1_replay_replay_init, 1, sizeof(*global_topology));

	/* mimic initialization steps of the real TOMA service */

	nvmeibt_global_init();
	nvmeibt_common_init();

	if (nvmeibt_raft_one_time_init() < 0) {
		N_Ef(error_2_replay_replay_init, "Failed to initialize raft context (@AUTO_ERRNO)");
		goto out;
	}

	/* mimic time tracking */
	replay_time_init();

	/* all good */
	ret = 0;

out:
	if (ret < 0)
		replay_exit();

	NFOUT;

	return ret;
}

static struct {
	const char *type;
	int (*func)(const char *buf);
} replay_funcs[] = {
	{ NVMEIBT_DUMPER_TYPE_MGMT, replay_event_mgmt_config },
	{ NVMEIBT_DUMPER_TYPE_RAFT, replay_event_raft_persist },
	{ NVMEIBT_DUMPER_TYPE_TOPO, replay_event_global_topo },
	{ NVMEIBT_DUMPER_TYPE_PEER, replay_event_peer_applied },
	{ NVMEIBT_DUMPER_TYPE_DISK, replay_event_remove_disk }
};

#define REPLAY_NTYPES ARRAY_SIZE(replay_funcs)

static int replay_event_func_index(const char *type)
{
	size_t i;

	for (i = 0; i < REPLAY_NTYPES; i++) {
		if (strcmp(type, replay_funcs[i].type) == 0)
			return i;
	}

	return -1;
}

/*
 * replay_parse_snippet_id(): parse a dump snippet id  into its components:
 * raft#, topo#, event#, and type identifier.
 *
 * format dump snippet id  defined in nvmeibt_dumper.h
 */
static int replay_parse_snippet_id(const char *id, int *raft, int *topo,
								   int *event, const char **type)
{
	static int count;

	const char *str;
	char *endp;
	int ret = -1;

	NFIN;

	str = id;
	*raft = strtol(str, &endp, 16);
	if (str == endp || *endp == '\0') {
		N_Wf(warn_replay_replay_parse_snippet_id, "failed to parse snippet id @ID_STR", id);
		goto out;
	}

	str = endp + 1;
	*topo = strtol(str, &endp, 16);
	if (str == endp || *endp == '\0') {
		N_Wf(warn_1_replay_replay_parse_snippet_id, "failed to parse snippet id @ID_STR", id);
		goto out;
	}

	str = endp + 1;
	*event = strtol(str, &endp, 10);
	if (str == endp || *endp == '\0') {
		N_Wf(warn_2_replay_replay_parse_snippet_id, "failed to parse snippet id @ID_STR", id);
		goto out;
	}

	*type = endp + 1;
	if (replay_event_func_index(*type) < 0) {
		/* bad karma: invalid log type */
		N_Wf(warn_3_replay_replay_parse_snippet_id, "invalid log type of @ID_STR", id);
		goto out;
	}

	count++;

	if (usage_opt_verbose) {
		N_Tf(info_replay_replay_parse_snippet_id, "snippet @NUM: type=@STR event=@NUM topo=@TOPO_VER", count, *type, *event, *topo);
	}

	ret = 0;

out:
	NFOUT;
	return ret;
}

/*
 * replay_clear_cache(): cleanup $CWD/TOMA_DIR_OPT_NVMESH/toma/...
 *
 * Important because some TOMA code that processes our input data (like
 * mgmt, topo) tried to read the cache to load permanent state.
 */
static int replay_clear_cache(void)
{
	const char *cwd;
	int ret = -1;

	NFIN;

	cwd = getcwd(NULL, 0);
	if (cwd == NULL)
		goto out;

	if (chdir(TOMA_FILE_DIR) != 0)
		goto out;

	/* clean-up, clean-up, everybody, everywhere */
	ret = system("rm -rf * 2> /dev/null");

out:
	if (ret != 0)
		N_Ef(error_replay_replay_clear_cache, "Failed to clear cache @PATH (error=@ERROR, @AUTO_ERRNO)", TOMA_FILE_DIR, ret);

	if (cwd != NULL) {
		chdir(cwd);
		free((void *) cwd);
	}

	NFOUT;

	return ret;
}

/*
 * replay_open_file(): open input dump file
 */
static int replay_open_file(const char *path)
{
	NFIN;

	replay_fd = NNVMEIBT_OPENAT(trace_replay_replay_open_file, replay_cwd_fd, path, O_RDONLY);

	if (replay_fd < 0) {
		N_Ef(error_replay_replay_open_file, "Failed to open dump file @PATH (@AUTO_ERRNO)", path);
		goto out;
	}

out:
	NFOUT;
	return replay_fd;
}

static int replay_do_read(int fd, char *buf, size_t size)
{
	size_t pos = 0;
	int n;

	do {
		n = read(fd, &buf[pos], size);
		if (n < 0) {
			N_Ef(replay_do_read_error_1,
				 "Failed to read from dump file (@AUTO_ERRNO)");
			return -1;
		} else if (n == 0 && pos > 0) {
			N_Ef(replay_do_read_error_2, "Failed to read from dump file (EOF)");
			return -1;
		} else if (n == 0 && pos == 0) {
			N_Tf(replay_do_read_trace, "Reached end of dump file (EOF)");
			return 0;
		}
		size -= n;
		pos += n;
	} while (size > 0);

	return pos;
}

static int replay_readline(int fd, char *buf, size_t size)
{
	int n, cnt = 0;
	char tmp[16];

	if (size == 0)
		return -1;

	if (buf == NULL && size <= 16)
		buf = tmp;

	while (size > 0) {
		n = replay_do_read(fd, &buf[cnt], 1);
		if (n < 0)
			return -1;
		if (n == 0 && cnt > 0)  /* early EOF (no newline) is error */
			return -1;
		if (n == 0 /* && cnt == 0 */)
			return 0;
		if (buf[cnt++] == '\n')
			break;
		size--;
	}

	buf[cnt - 1] = '\0';
	return cnt;
}

/*
 * replay_read_snippet(): read dump file, return next header/snippet
 *
 * We alternate between two modes: header (mode==0), and snippet (mode==1).
 *
 *   . header mode: read snippet header (see nvmeibt_dumper.h for details)
 *     which is of fixed size and return a pointer to the snippet id.
 *
 *   . snippet mode: parse the snippet size from the previous header, read
 *     in the contents into an allocated buffer, and return the buffer.
 *
 * The buffer size is (at least) the file size plus one, ensuring the string
 * is properly NULL terminated. The buffer returned from snippet-mode must be
 * freed using a call to replay_free_buf().
 */
static char *replay_read_snippet(void)
{
	static char hdr1[256];
	static char hdr2[256];
	static char snippet_id[SNIPPET_ID_LEN + 1];
	static BOOL mode = 0;

	size_t size;
	char *buf = NULL;
	char *ptr;
	int ret;

	NFIN;

	if (mode == 0) {
		memset(hdr1, 0, 256);
		memset(hdr2, 0, 256);

		ret = replay_readline(replay_fd, NULL, 1);
		if (ret < 0)
			goto mismatch;
		else if (ret == 0)
			goto out;

		ret = replay_readline(replay_fd, hdr1, 256);
		if (ret <= 0 || ret >= 256)
			goto mismatch;

		ret = replay_readline(replay_fd, hdr2, 256);
		if (ret <= 0 || ret >= 256)
			goto mismatch;

		ret = replay_readline(replay_fd, NULL, 1);
		if (ret != 1)
			goto mismatch;

		/* sanity: validate the header prefix (1) */
		if (memcmp(hdr1, TOMA_DUMPER_HDR, TOMA_DUMPER_HDRLEN)) {
			N_Ef(error_replay_replay_read_snippet, "Snippet header (1) mismatch @HDR1", hdr1);
			goto out;
		}

		/* sanity: validate the header prefix (2) */
		if (memcmp(hdr2, "size=", 5)) {
			N_Ef(error_1_replay_replay_read_snippet, "Snippet header (2) mismatch @HDR2", hdr2);
			goto out;
		}

		nvmeibt_strlcpy(snippet_id, &hdr1[SNIPPET_ID_POS], sizeof(snippet_id));
		N_Tf(trace_replay_replay_read_snippet, "      ... snippet @SNIPPET_ID", snippet_id);

		buf = snippet_id;

		mode = 1;
	} else {
		hdr2[SNIPPET_SIZE_POS + SNIPPET_SIZE_LEN] = '\0';
		size = strtol(&hdr2[SNIPPET_SIZE_POS], &ptr, 10);

		if (ptr == NULL) {
			N_Ef(error_2_replay_replay_read_snippet, "Header with bad size for snippet @SNIPPET_ID", snippet_id);
			goto out;
		}

		if (size == 0)
			N_Ef(error_3_replay_replay_read_snippet, "Header with zero size for snippet @SNIPPET_ID", snippet_id);

		buf = replay_alloc_buf(size + 1);
		if (buf == NULL) {
			N_Ef(error_4_replay_replay_read_snippet, "Failed to allocate snippet buffer @SNIPPET_ID", snippet_id);
			goto out;
		}

		buf[size] = '\0';

		ret = replay_do_read(replay_fd, buf, size);
		if (ret <= 0) {
			N_Ef(error_5_replay_replay_read_snippet, "Failed to read contents of snippet @SNIPPET_ID (@AUTO_ERRNO)", snippet_id);
			replay_free_buf(buf);
			buf = NULL;
			goto out;
		}

		mode = 0;
	}

out:
	NFOUT;
	return buf;

mismatch:
	/* common error msg from mode==0 certain failures */
	N_Ef(error_6_replay_replay_read_snippet, "Snippet header mismatch: '@HDR1'/'@HDR2'", hdr1, hdr2);
	goto out;
}

/*
 * replay_read_first(): first dump file is management config
 */
static int replay_read_first(int *raft, int *topo, int *event)
{
	const char *ty;
	const char *id;
	char *buf = NULL;
	int r, t, e;
	int ret = -1;

	NFIN;

	/* first dump snippet expected NVMEIBT_DUMPER_TYPE_MGMT */

	id = replay_read_snippet();
	if (id == NULL) {
		N_Ef(error_replay_replay_read_first, "Missing first snippet (@STR)", NVMEIBT_DUMPER_TYPE_MGMT);
		goto out;
	}

	ret = replay_parse_snippet_id(id, &r, &t, &e, &ty);
	if (ret < 0) {
		N_Ef(error_1_replay_replay_read_first, "Failed to parse first snippet @ID_STR", id);
		goto out;
	}

	ret = -1;

	if (strcmp(ty, NVMEIBT_DUMPER_TYPE_MGMT) != 0) {
		N_Ef(error_2_replay_replay_read_first, "First dump snippet of wrong type @ID_STR", id);
		goto out;
	}

	buf = replay_read_snippet();
	if (buf == NULL)
		goto out;

	if (buf[0] == '\0') {
		N_Ef(error_3_replay_replay_read_first, "Zero size for dump snippet @ID_STR", id);
		goto out;
	}

	ret = replay_event_mgmt_config(buf);
	if (ret < 0)
		N_Ef(error_4_replay_replay_read_first, "Failed to apply mgmt config from snippet @ID_STR", id);

	*raft = r;
	*topo = t;
	*event = e;

out:
	if (buf != NULL)
		replay_free_buf(buf);

	NFOUT;
	return ret;
}

/*
 * replay_read_second(): second dump file is raft persistency
 */
static int replay_read_second(int *raft, int *topo, int *event)
{
	const char *ty;
	const char *id;
	char *buf = NULL;
	int r, t, e;
	int ret = -1;

	NFIN;

	/* second dump snippet expected NVMEIBT_DUMPER_TYPE_RAFT */

	id = replay_read_snippet();
	if (id == NULL) {
		N_Ef(error_replay_replay_read_second, "Missing second snippet (@STR)", NVMEIBT_DUMPER_TYPE_RAFT);
		goto out;
	}

	ret = replay_parse_snippet_id(id, &r, &t, &e, &ty);
	if (ret < 0) {
		N_Ef(error_1_replay_replay_read_second, "Failed to parse second snippet @ID_STR", id);
		goto out;
	}

	ret = -1;

	if (strcmp(ty, NVMEIBT_DUMPER_TYPE_RAFT) != 0) {
		N_Ef(error_2_replay_replay_read_second, "Second dump snippet wrong type @ID_STR", id);
		goto out;
	}

	if (r != *raft || t != *topo || e != *event + 1) {
		N_Ef(error_3_replay_replay_read_second, "Second dump snippet mismatch @ID_STR (@RAFT/@TOPOLOGY_INT/@EVENT ~ @RAFT/@TOPO_INT/@EVENT)",
			id, r, t, e, *raft, *topo, *event);
		goto out;
	}

	buf = replay_read_snippet();
	if (buf == NULL)
		goto out;

	ret = replay_event_raft_persist(buf);

	*raft = r;
	*topo = t;
	*event = e;

	ret = 0;

out:
	if (buf != NULL)
		replay_free_buf(buf);

	NFOUT;
	return ret;
}

/*
 * replay_next_snippet(): process one dump snippet
 */
static int replay_next_snippet(const char **id,
							   int *raft, int *topo, int *event,
							   const char **type)
{
	int snippet_raft, snippet_topo, snippet_event;
	int ret = 0;

	NFIN;

	*id = replay_read_snippet();
	if (*id == NULL) {
		N_Tf(trace_replay_replay_next_snippet, "Completed all dump snippets");
		goto out;
	}

	ret = replay_parse_snippet_id(*id, &snippet_raft, &snippet_topo, &snippet_event, type);
	if (ret < 0) {
		N_Ef(error_replay_replay_next_snippet, "Failed to parse snippet @ID_STR", *id);
		goto out;
	}

	ret = -1;

	if ((snippet_raft < *raft) || (snippet_topo < *topo) || (snippet_event != 0 && snippet_event != *event + 1)) {
		N_Ef(error_1_replay_replay_next_snippet, "Dump snippet indices mismatch @ID_STR (@SNIPPET_RAFT/@SNIPPET_TOPO/@SNIPPET_EVENT ~ @RAFT/@TOPO_INT/@EVENT)",
			*id, snippet_raft, snippet_topo, snippet_event, *raft, *topo, *event);
		goto out;
	}

	/* if event count is 0, it must be a "topo" event */
	if (snippet_event == 0 && strcmp(*type, NVMEIBT_DUMPER_TYPE_TOPO) != 0) {
		N_Ef(error_2_replay_replay_next_snippet, "Dump snippet event mismatch @ID_STR (@SNIPPET_RAFT/@SNIPPET_TOPO/@SNIPPET_EVENT)", *id, snippet_raft, snippet_topo, snippet_event);
		goto out;
	}

	/*
	 * All "topo" events must either:
	 * - keep last topo version and bump event count, -or-
	 * - bump topo version and reset the event count
	 */
	if (strcmp(*type, NVMEIBT_DUMPER_TYPE_TOPO) == 0) {
		if (snippet_topo == *topo && snippet_event != *event + 1) {
			N_Ef(error_3_replay_replay_next_snippet, "Event for same \"topo\" dump expected @EVENT (@SNIPPET_EVENT)", *event + 1, snippet_event);
			goto out;
		} else if (snippet_topo != *topo && snippet_event != 0) {
			N_Ef(error_4_replay_replay_next_snippet, "Event for new \"topo\" dump expected 0 (@SNIPPET_EVENT)", snippet_event);
			goto out;
		}
	}

	*raft = snippet_raft;
	*topo = snippet_topo;
	*event = snippet_event;

	ret = 0;

out:
	NFOUT;
	return ret;
}

/* replay statistics */
static struct {
	int n_sessions;
	int n_sequences;
	int n_events;
	int n_per_type[REPLAY_NTYPES];
} replay_stat;

static void replay_stat_reset(void)
{
	memset(&replay_stat, 0, sizeof(replay_stat));
}

/* replay_stat_tick: increment the counter of given event type */
static void replay_stat_tick(const char *type)
{
	int i;

	i = replay_event_func_index(type);
	replay_stat.n_per_type[i]++;
	replay_stat.n_events++;
}

static void replay_stat_print(void)
{
	char line[128];
	int pos = 0;
	size_t i;

	N_Tf(trace_replay_replay_stat_print, "    totals:  sessions @N_SESSIONS, sequences @N_SEQUENCES, events @N_EVENTS",
		 replay_stat.n_sessions, replay_stat.n_sequences, replay_stat.n_events);

	for (i = 0; i < REPLAY_NTYPES; i++) {
		pos += snprintf(line + pos, sizeof(line) - 1 - pos,
						"%s %s %d", i == 0 ? "" : ", ",
						replay_funcs[i].type,
						replay_stat.n_per_type[i]);
	}

	N_Tf(trace_1_replay_replay_stat_print, "    types:  @LINE", line);
}

/*
 * replay_loop(): read and process one dump file
 */
static int replay_loop(const char *path)
{
	int raft = 0, topo = 0, event = 0;
	int r, t, e;
	const char *type;
	const char *id;
	char *buf = NULL;
	int i, ret = -1;

	NFIN;

	if (replay_clear_cache() < 0)
		goto out;

	if (replay_open_file(path) < 0)
		goto out;

	replay_stat_reset();

	replay_time_tick(1);
	if (replay_read_first(&raft, &topo, &event) < 0)
		goto out;
	replay_stat_tick("mgmt");

	replay_time_tick(1);
	if (replay_read_second(&raft, &topo, &event) < 0)
		goto out;
	replay_stat_tick("raft");

	replay_time_adjust();
	replay_time_tick(60);  /* see comment on "Virtual time" above */

	replay_stat.n_sessions += 1;
	replay_stat.n_sequences += 1;

	nvmeibt_topology_leader_clear_recalc_required();

	r = raft;
	t = topo;
	e = event;

	while (1) {
		ret = -1;
		buf = NULL;

		if (replay_next_snippet(&id, &r, &t, &e, &type) < 0) {
			N_Ef(error_replay_replay_loop, "Failed to read contents of snippet @ID_STR (@AUTO_ERRNO)", id);
			goto out;
		}
		if (id == NULL)
			break;

		buf = replay_read_snippet();
		if (buf == NULL) {
			N_Ef(error_1_replay_replay_loop, "Failed to read contents of snippet @ID_STR (@AUTO_ERRNO)", id);
			goto out;
		}

		if (buf[0] == '\0') {
			N_Ef(error_2_replay_replay_loop, "Zero size for dump snippet @ID_STR", id);
			goto out;
		}

		replay_time_tick(1);

		i = replay_event_func_index(type);
		ret = replay_funcs[i].func(buf);

		if (ret < 0) {
			N_Ef(error_3_replay_replay_loop, "Failed to replay session");
			goto out;
		}

		replay_free_buf(buf);

		replay_stat_tick(type);

		replay_stat.n_events += 1;
		if (t != topo)
			replay_stat.n_sequences += 1;
		if (r != raft)
			replay_stat.n_sessions += 1;

		raft = r;
		topo = t;
		event = e;

		watchdog_tick++;
	}

	ret = 0;

out:
	if (buf != NULL)
		replay_free_buf(buf);

	replay_stat_print();

	NFOUT;
	return ret;
}

/*
 * replay_usage(): display usage messages
 * NOTE: called before replay_init(), must not use _Ef/_Wf/_Tf
 */
static void replay_usage(int ret)
{
	printf("usage: %s %s\n", usage_name, usage_line);
	printf("%s\n", usage_long);
	exit(ret);
}

/*
 * replay_parse_add_path(): helper to add path arg to the list
 * NOTE: called before replay_init(), must not use _Ef/_Wf/_Tf
 */
static int replay_parse_add_path(const char *path)
{
	static int paths_tot = 0;

	if (paths_cnt == paths_tot) {
		paths_tot += 16;
		paths_arr = NNVMEIBT_TOMA_REALLOC(trace_replay_replay_parse_add_path, paths_arr, paths_tot * sizeof(*paths_arr));
	}

	paths_arr[paths_cnt++] = path;

	return 0;
}

/*
 * replay_parse_args(): parse command line args
 * NOTE: called before replay_init(), must not use _Ef/_Wf/_Tf
 */
static int replay_parse_args(int argc, char *argv[])
{
	const struct option longopts[] = {
		{ "outdir", required_argument, 0, 'o' },
		{ "debug", no_argument, 0, 'd' },
		{ "verbose", no_argument, 0, 'v' },
		{ "help", no_argument, 0, 'h' },
		{ NULL, 0, 0, 0 }
	};

	const char shortopts[] = "dho:v";

	int opt, ret = 0;
	char *copy;

	/* find how we are called */

	copy = NNVMEIBT_TOMA_MALLOC(trace_replay_replay_parse_args, strlen(argv[0]) + 1);

	nvmeibt_strlcpy(copy, argv[0], strlen(argv[0]) + 1);
	usage_name = basename(copy);

	/* collect user's wishes */

	do {
		opt = getopt_long(argc, argv, shortopts, longopts, NULL);

		switch (opt) {
		case -1:
			/* last one */
			break;
		case 'o':
			usage_opt_outdir = optarg;
			break;
		case 'd':
			usage_opt_debug = 1;
			break;
		case 'v':
			usage_opt_verbose = 1;
			break;
		case 'h':
			replay_usage(0);	/* FALLTHROUGH */
			/* not reached */
			break;
		default:
			replay_usage(1);
			/* not reached */
			break;
		}
	} while (opt != -1);

	/* collects paths to process */

	while (ret == 0 && optind < argc) {
		ret = replay_parse_add_path(argv[optind++]);
		if (ret < 0)
			return ret;
	}

	/* require at least one path */
	if (paths_cnt == 0)
		replay_usage(1);

	/* require exactly one path in debug mode */
	if (usage_opt_debug && paths_cnt != 1)
		replay_usage(1);

	return 0;
}

/*
 * replay_goto_outdir(): save current cwd and move to output dir
 * NOTE: called before replay_init(), must not use _Ef/_Wf/_Tf
 */
static int replay_goto_outdir(void)
{
	replay_cwd_fd = open(".", O_RDONLY);
	if (replay_cwd_fd < 0) {
		fprintf(stderr, "Failed to open current directory (%m)\n");
		return -1;
	}

	if (chdir(usage_opt_outdir)) {
		fprintf(stderr, "Failed to chdir to '%s' (%m)\n", usage_opt_outdir);
		return -1;
	}

	if (nvmeibt_recursive_mkdir(TOMA_LOG_DIR, 0755) < 0) {
		fprintf(stderr, "Failed to mkdir %s at '%s' (%m)\n",
				TOMA_LOG_DIR, usage_opt_outdir);
		return -1;
	}

	if (nvmeibt_recursive_mkdir(TOMA_FILE_DIR, 0755) < 0) {
		fprintf(stderr, "Failed to mkdir %s at '%s' (%m)\n",
				TOMA_FILE_DIR, usage_opt_outdir);
		return -1;
	}

	return 0;
}

/*
 * replay_watchdog_fire(): watchdog timer to detect stalls
 *
 * The main TOMA code uses pthreads - which is incompatible with our use
 * of fork() with replay workers. In normal runs, workers terminate using
 * _exit() to avoid unexpected issues (see replay_main).
 *
 * However, if things go wrong, the TOMA code may call nvmeibt_abort() to
 * terminate directly (for instance if the data we feed is bad or too old).
 * In particular, it will call nvmeibt_logger_exit() which will hang, due
 * to the confused pthread state in the forked replay worker.
 *
 * We use the watchdog below to detect such stall. The watchdog is called
 * once a second, and checks whether the replay has progressed. If not, it
 * will terminate, using _exit().
 */
static void replay_watchdog_fire(sigval_t sigval __attribute__((unused)))
{
	static int last_tick = 0;

	NFIN;

	if (watchdog_tick == last_tick) {
		fprintf(stderr, "%s %s (%ld) %s[%d]: %s\n",
				get_8_plus_3_char_str_of_now(),
				__FILENAME__, get_my_tid(), __FUNCTION__, __LINE__,
				"REPLAY WATCHDOG TIMEOUT DETECTED ... ABORTING\n");
		_exit(1);
	}

	last_tick = watchdog_tick;

	NFOUT;
}

/*
 * replay_watchdog_init(): initialize the watchdog timer
 */
static int replay_watchdog_init(void)
{
    struct itimerspec ts;
    struct sigevent se;
	timer_t ti;

    memset (&se, 0, sizeof(struct sigevent));
	se.sigev_notify = SIGEV_THREAD;
	se.sigev_notify_function = replay_watchdog_fire;
	se.sigev_notify_attributes = NULL;

    if (timer_create(CLOCK_MONOTONIC, &se, &ti) < 0) {
		N_Ef(error_replay_replay_watchdog_init, "failed to create watchdog timer (@AUTO_ERRNO)");
		return -1;
	}

	ts.it_value.tv_sec = 1;
	ts.it_value.tv_nsec = 0;
	ts.it_interval.tv_sec = 1;
	ts.it_interval.tv_nsec = 0;

    if (timer_settime (ti, 0, &ts, NULL) < 0) {
		N_Ef(error_1_replay_replay_watchdog_init, "failed to arm watchdog timer (@AUTO_ERRNO)");
		return -1;
    }

	return 0;
}

/*
 * replay_main(): loop over recording paths and fork replay workers
 */
static int replay_main(int argc, char *argv[])
{
	pid_t pid = getpid();  /* default mark parent (appease compiler) */
	const char *path;
	int cnt;
	int ret;

	replay_is_enabled = 1;

	nvmeibt_start_all_trace_pollers(0);

	ret = replay_parse_args(argc, argv);
	if (ret < 0)
		goto out;

	ret = replay_goto_outdir();
	if (ret < 0)
		goto out;

	/* NOTE: must not use _Ef/_Wf/_Tf before this call */

	ret = replay_init();
	if (ret < 0) {
		N_Ef(error_replay_replay_main, "Failed to initialize");
		goto out;
	}

	/* in debug mode, avoid fork() to allow easy work with debugger */
	if (usage_opt_debug) {
		path = paths_arr[0];

		N_Tf(trace_replay_replay_main, "Replaying session from directory '@PATH'", path);
		ret = replay_loop(path);
		N_Tf(trace_1_replay_replay_main, "Replay status @RET_INT", ret);

		goto out;
	}

	for (cnt = 0; cnt < paths_cnt; cnt++) {
		int status;

		ret = 1;

		path = paths_arr[cnt];
		N_Tf(trace_2_replay_replay_main, "Replaying session #@CNT from directory '@PATH'", cnt, path);

		pid = fork();

		if (pid < 0) {
			/* bad luck */
			N_Ef(error_1_replay_replay_main, "Failed to fork replay worker (@AUTO_ERRNO)");
			break;
		} else if (pid == 0) {
			/* child */
			ret = replay_watchdog_init();  /* see replay_watchdog_fire() */
			if (ret < 0) {
				N_Ef(error_2_replay_replay_main, "Failed to start watchdog timer");
				goto out;
			}

			N_Tf(trace_3_replay_replay_main, "  |-> Replay worker with pid @GETPID start", getpid());
			replay_parent = 0;  /* mark ourselves child, to skip atexit() */
			ret = replay_loop(path);
			N_Tf(trace_4_replay_replay_main, "  |-> Replay worker with pid @GETPID finish", getpid());
			break;
		} else {
			/* parent */
			N_Tf(trace_5_replay_replay_main, "  |-> Wait for replay worker with pid @PID", pid);
			ret = waitpid(pid, &status, 0);

			if (ret < 0) {
				ret = 1;
				N_Ef(error_3_replay_replay_main, "  |-> Failed wait for replay worker @CNT (pid=@PID) (@AUTO_ERRNO)",
					cnt, pid);
			} else if (WIFEXITED(status)) {
				ret = WEXITSTATUS(status);
				N_Tf(trace_6_replay_replay_main, "  |-> Replay worker @CNT (pid=@PID) reported status @RET_INT",
					 cnt, pid, ret);
			} else if (WIFSIGNALED(status)) {
				ret = WTERMSIG(status);
				N_Tf(trace_7_replay_replay_main, "  |-> Replay worker @CNT (pid=@PID) received signal @RET_INT",
					 cnt, pid, ret);
			} else {
				/* cannot happen with waitpid(2), but play defensive */
				ret = 1;
				N_Ef(error_4_replay_replay_main, "  |=> Unexpected retval 0 from waitpid (@AUTO_ERRNO)");
			}

			if (ret != 0) {
				N_Ef(error_5_replay_replay_main, "  |-> Failed status for replay worker @CNT (pid=@PID)",
					cnt, pid);
				break;
			}
		}
	}

out:
	replay_exit();

	/* convert negative error to (bad) exit code */
	if (ret < 0)
		ret = -ret;

	/*
	 * The replay worker must exit somewhat silently, without invoking e.g.
	 * atexit() hooks, pthread calls, and other cleanups - that would stall
	 * the replay worker (for example, pthread is incompatible with fork).
	 * The final cleanup will be done by the parent.
	 *
	 * Note: the replay worker may exit from elsewhere instead of here
	 * (any of the TOMA functions we use may decide to exit upon error).
	 * We handle this by using our own atexit() handler.
	 *
	 * Note: one exception is nvmeibt_abort(), called on harsh errors in
	 * TOMA code; it will do the cleanups and stall the replay worker. This
	 * will be detected by the watchdog timer (see replay_watchdog_*).
	 */
	if (pid == 0)
		_exit(ret);

	exit(ret);

	/* NOT REACHED */
}

/*
 * if compiled with TOMA_REPLAY, this is main().
 * otherwise, introduce a dummy to avoid fatal warnings about unused.
 */
#ifdef TOMA_REPLAY
int main(int argc, char *argv[])
{
	return replay_main(argc, argv);
}
#else
static int __attribute__((unused)) dummy(int argc, char *argv[])
{
	return replay_main(argc, argv);
}
#endif /* TOMA_REPLAY */

