#include "nvmeibt_debug.h"
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <signal.h>
#include <stdarg.h>
#include <pthread.h>
#include "nvmeibt_kafka.h"
#include "vol/nvmeibt_block_device.h"
#include "nvmeibt_toma.h"
#include "nvmeibt_raft.h"
#include "nvmeibt_mm_json.h"
#include "nvmeibt_global.h"
#include "nvmeibt_topology.h"
/*
 * The protocol with MGMT is described in:
 * https://nvidia-my.sharepoint.com/:w:/r/personal/tleibo_nvidia_com/_layouts/15/doc2.aspx?sourcedoc=%7B42ddc038-38b6-493c-a722-cd3c63c48d06%7D&action=edit&wdPid=79c0c0e&cid=59845700-e917-4431-a94a-eab1034bff32
 */

/*******************    offset service functions         **********************/
int64_t purify_offset(int64_t offset_with_topic_change_no) {
	union offset_with_topic_change_no u = { .all = offset_with_topic_change_no };
	u.a[7] = u.a[6] = u.a[5];	// Sign extend since the Kafka special values are -1, -2000, etc.
	return u.all;
}

int8_t get_topic_change_no_from_offset(int64_t offset_with_topic_change_no) {
	const union offset_with_topic_change_no u = { .all = offset_with_topic_change_no };
	NTOMA_ASSERT(v4g7yus, (uint64_t)(u.a[7]) <= KAFKA_TOPIC_CHANGE_NO, "topic_change_no=@INT8_TX", u.a[7]);
	return u.a[7];
}

static int64_t glue_topic_change_no_and_offset(int8_t topic_change_no, int64_t offset) {
	union offset_with_topic_change_no u = {.all = offset };
	NTOMA_ASSERT(cyiuskl, (u.a[6] == 0 || u.a[6] == (int8_t)(0xff)), "Illegal offset @INT64_TX. Byte[6] should be unused", u.all);
	u.a[6] = 0;
	u.a[7] = topic_change_no;
	return u.all;
}

static bool is_offset_zero(int64_t offset) {
	return (purify_offset(offset) == 0LL );
}

static bool is_RD_KAFKA_OFFSET_VALID(int64_t offset) {
	const int64_t purified = purify_offset(offset);
	return (purified != RD_KAFKA_OFFSET_INVALID && purified != nvmeibt_offset_and_idx_uninitialized);		// -1 is often used in toma for uninitialized
}

/******************************************************************************/
static char *kafka_event_type_str(enum KAFKA_EVENT_TYPE event_type) {
	switch (event_type) {
	case KAFKA_EVENT_TYPE_UNKNOWN: return "KAFKA_EVENT_TYPE_UNKNOWN";
	case KAFKA_EVENT_TYPE_VOL_ADD: return "KAFKA_EVENT_TYPE_VOL_ADD";
	case KAFKA_EVENT_TYPE_VOL_DEL: return "KAFKA_EVENT_TYPE_VOL_DEL";
	case KAFKA_EVENT_TYPE_VOL_DEL_COMPLETED: return "KAFKA_EVENT_TYPE_VOL_DEL_COMPLETED";
	case KAFKA_EVENT_TYPE_VOL_UPD: return "KAFKA_EVENT_TYPE_VOL_UPD";
	case KAFKA_EVENT_TYPE_TARGET_ADD: return "KAFKA_EVENT_TYPE_TARGET_ADD";
	case KAFKA_EVENT_TYPE_TARGET_DEL: return "KAFKA_EVENT_TYPE_TARGET_DEL";
	case KAFKA_EVENT_TYPE_HW_FULL_CONFIG: return "KAFKA_EVENT_TYPE_HW_FULL_CONFIG";
	case KAFKA_EVENT_TYPE_CMD: return "KAFKA_EVENT_TYPE_CMD";
	default : return "OOPS event_type";
	}
}

/**************************** Certificate storage ****************************/
/* The code below has 2 purposes:
	1. Guarantee atomicity of certificate files. SRE's can update certificate
		files in place. So before initializing Kafka, we copy them aside
		Kafka accesses certs multiple times (Toma has a few producers/consumers)
	2. Actually load certificates to be able to print them on demand.
		Toma does not need the certificate, printing is explicit product request
*/
struct __t_certificate_storage {	// All functions are called from Toma main thread, avoid races, dont call them from kafka thread
	bool is_initialized;
	struct nvmeibt_Str *file_path[4];		// Path of the copied file
	const char* dir;						// Directory to store the files above
	struct nvmeibt_Str *file_content[4];	// Content of the certificate
} _ssl = {0};

void __t_certificate_storage_destroy(struct __t_certificate_storage* s) {
	int i, n_files_to_intercept = (int)ARRAY_SIZE(s->file_content);
	for (i = 0; i < n_files_to_intercept; i++) {
		if (s->file_content[i])
			NNVMEIBT_STR_FREE(titccfasc0, s->file_content[i]);
		if (s->file_path[i]) {
			const int rv = unlink(s->file_path[i]->text_buf);
			if ((rv != 0) && (errno != ENOENT))			// Possibly error during copy of file
				N_Ef(titccfasc1, "Leaking file @STR, rv=@RV. @AUTO_ERRNO", s->file_path[i]->text_buf, rv);
			NNVMEIBT_STR_FREE(titccfasc2, s->file_path[i]);
		}
	}
	if (s->dir)
		rmdir(s->dir);
	memset(s, 0, sizeof(*s));
}

// Replace const char* file path with a different const path, thus not affecting the rest of the flow. Surgical interception
static void __intercept_toma_certificate_copy_file_and_save_content(struct __t_certificate_storage *s, const char **ca, const char **toma_certificate, const char **key_location, const char **key_password) {
	const char ** const orig_path_ptr[4] = {ca, toma_certificate, key_location, key_password};
	const int n_files_to_intercept = (int)ARRAY_SIZE(s->file_content);
	int i = 0, rv = 0;
	if (s->is_initialized)
		__t_certificate_storage_destroy(s);					// Previous initialization retry
	s->dir = TOMA_DIR_RUN_NVMESH "/tls";					// Step 0: Create container directory
	if (mkdir(s->dir, 0777) != 0 && (errno != EEXIST)) {
		N_ETf(titccfasc5, "Error creating certificates dir @STR. @AUTO_ERRNO", s->dir);
		rv = -__LINE__; goto _err;
	}
	for (i = 0; i < n_files_to_intercept; i++) {
		const char* in_path = *orig_path_ptr[i];
		const char* out_path = NULL;
		if (in_path == NULL)
			continue;
		{	// Step 1: Copy file aaaa.crt -> /var dir aaaa.crt_dont_touch for atomicity
			char sys_cmd[1024];
			int cmd_len;
			s->file_path[i] = NNVMEIBT_STR_ALLOC(titccfasc6);
			nvmeibt_Str_sprintf(s->file_path[i], "%s/%s_dont_touch", s->dir, basename(in_path));
			out_path = s->file_path[i]->text_buf;
			unlink(out_path);									// Remove possible file from a previous run.
			cmd_len = snprintf(sys_cmd, sizeof(sys_cmd), "cp %s %s", in_path, out_path);
			if (cmd_len + 1 > (int)sizeof(sys_cmd)) {			// Might be truncated
				rv = -__LINE__; goto _err;
			}
			if (system(sys_cmd) != 0) {							// Copy failed, cannot continue
				rv = -__LINE__; goto _err;
			}
			chmod(out_path, 0444);								// Copy successful, Set as read only to prevent messing with file
			*orig_path_ptr[i] = out_path;						// Inject new path back into input variables
		}
		{	// Step 2: Now load the certificate file into a buffer, to be able to print it
			int fd = NNVMEIBT_OPEN_READ(titccfasc9, out_path, 1);
			if (fd <= 0) {
				N_ETf(titccfasca, "Error while opening the file @STR. @AUTO_ERRNO", out_path);
				rv = -__LINE__; goto _err;
			}
			s->file_content[i] = NNVMEIBT_STR_ALLOC(titccfascb);
			NNVMEIBT_STR_FREAD(titccfascc, s->file_content[i], fd);
			NNVMEIBT_CLOSE(titccfascd, fd);
			if (nvmeibt_Str_strlen(s->file_content[i]) < 100) {
				N_Ef(titccfasce, "Unreasonable len=@SIZE_T of file @STR", nvmeibt_Str_strlen(s->file_content[i]), out_path);
				rv = -__LINE__; goto _err;
			}
		}
	}
_err:
	if (rv) {
		N_Ef(titccfasc9, "Error processing file[@INT]=@STR file, rv_line=@RV! Will not be able to get configuration!", i, *orig_path_ptr[i], rv);
		__t_certificate_storage_destroy(s);	// Cleanup on failure. Do not crash toma as this might be during soft kafka reinit
	} else {
		s->is_initialized = true;
	}
	return;
}

static void __t_certificate_storage_print(const struct __t_certificate_storage *s, int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx) {
	int i, n_files = (int)ARRAY_SIZE(s->file_content);

	(*printf_fn)(printf_ctx, "\n- - - - -   Kafka Certificates   - - - - -\n");
	if (s->file_path[1])		// Print ebpf command so SRE's can easily track certificates usage witout toma logs
		(*printf_fn)(printf_ctx, "%s%s%s", "sudo bpftrace -e 'tracepoint:syscalls:sys_enter_openat   /comm == \"" TOMA_THREAD_NAME "\" && str(args->filename) == \"",
			 s->file_path[1],
			"\"/   { printf(\"\%s: " TOMA_THREAD_NAME " opened certificate: \%s\\n\", strftime(\"\%H:\%M:\%S\", nsecs), str(args->filename));}'\n");
	for (i = 0; i < n_files; i++) {
		const struct nvmeibt_Str *str = s->file_content[i];
		if (str && str->str_len)
			(*printf_fn)(printf_ctx, "%s %u[b]=\n%*s\n\n\n", s->file_path[i], str->str_len, str->str_len, str->text_buf);
	}
}

/*************************           Globals          *************************/
extern int64_t nvmeibt_follower_keep_alive_secs;
extern int64_t nvmeibt_leader_keep_alive_secs;

int64_t nvmeibt_kafka_get_offset_timeout_secs = KAFKA_GET_OFFSET_TIMEOUT_SECS_DEFAULT;

uint64_t							running_producer_msg_sequence_number = 13LL; // No meaning for this num, just for debug
int64_t volatile 					kafka_leader_offset_blocking_incremental_TARGET_updates;
//
static struct nvmeibt_Str			*kafka_bootstrap_servers_str_from_nvmesh_conf = NULL;
static bool							kafka_mtls_ssl__is_enabled = 0;
static bool							kafka_mtls_ssl__is_hostname_verification_disabled = 0;
static struct nvmeibt_Str			*kafka_mtls_ssl__ca = NULL;
static struct nvmeibt_Str			*kafka_mtls_ssl__toma_certificate = NULL;
static struct nvmeibt_Str			*kafka_mtls_ssl__key_location = NULL;
static struct nvmeibt_Str			*kafka_mtls_ssl__key_password = NULL;

static int64_t volatile				kafka_follower_keepalive_token_provided_by_mgmt = -1;
static int64_t volatile				kafka_leader_keepalive_token_provided_by_mgmt = -1;
// Used to affect the main thread
static volatile int				kafka_applied_init_counter = 0;
static volatile int				kafka_requested_init_counter = 1;	// When larger than applied_init_counter, all new produce/consume fails
static volatile int				kafka_applied_init_preserve_state_vars_counter = 0;
static volatile int				kafka_requested_init_preserve_state_vars_counter = 0;

struct timespec					kafka_last_restart_timestamp = {0, 0};
static volatile int64_t			requested_incremental_VOL_updates_consumer_offset = RD_KAFKA_OFFSET_INVALID;   // For a new node, start reading from whatever was committed
static volatile int64_t			requested_incremental_TARGET_updates_consumer_offset = RD_KAFKA_OFFSET_INVALID;   // For a new node, start reading from whatever was committed
static volatile int64_t			requested_incremental_TARGET_updates_consumer_seq_no = -1;   // For a new node, start reading from whatever was committed
static volatile bool			kafka_requested_is_kafka_shutdown = 0;
static volatile bool			kafka_is_done_shutdown = 0;			// Set from kafka thread, read from other threads
static atomic_t					kafka_n_sends_in_the_air;			// inc/dec from kafka thread, print from stats/main thread
//
static unsigned long long		kafka_requested_consuming_leader_VOL_msgs_raft_term = 0;
static unsigned long long		kafka_applied_consuming_leader_VOL_msgs_raft_term = 0;
static unsigned long long		kafka_requested_consuming_leader_TARGET_msgs_raft_term = 0;
static unsigned long long		kafka_applied_consuming_leader_TARGET_msgs_raft_term = 0;
static pthread_mutex_t 			kafka_toma_requested_term_and_offset_mutex = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

#define IS_AWAITING_LEADER_KAFKA_OFFSET_BLOCKING_INCREMENTAL_TARGET_UPDATES(name)	({																								\
	bool		is;																																									\
	int64_t		incremental_TARGET_updates_offset = RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS, leader_committed_by_majority);															\
	is = (purify_offset(kafka_leader_offset_blocking_incremental_TARGET_updates) > purify_offset(incremental_TARGET_updates_offset));												\
	if (is) {																																										\
		N_Tf(name, "Awaiting offset_blocking_incremental_TARGET_updates=@LD>@LD",																									\
			 purify_offset(kafka_leader_offset_blocking_incremental_TARGET_updates), purify_offset(incremental_TARGET_updates_offset));												\
	}																																												\
	(is);																																											\
})

static bool is_consuming_leader_TARGET_msgs(void)
{
	return (kafka_applied_consuming_leader_TARGET_msgs_raft_term != 0);
}

static bool is_consuming_leader_VOL_msgs(void)
{
	return (kafka_applied_consuming_leader_VOL_msgs_raft_term != 0);
}

uint64_t get_next_running_producer_msg_sequence_number(void)
{
	return (running_producer_msg_sequence_number++);
}

int64_t nvmeibt_kafka_get_follower_keepalive_token_provided_by_mgmt(void)
{
	return kafka_follower_keepalive_token_provided_by_mgmt;
}
static void kafka_set_follower_keepalive_token_provided_by_mgmt(int64_t new_token, int64_t keepaliveInterval) {
	N_Tf(kdiw5ma, "token=@INT64_TX-->@INT64_TX keepaliveInterval=@INT64_TX-->@INT64_TX",
		 kafka_follower_keepalive_token_provided_by_mgmt, new_token,
		 nvmeibt_follower_keep_alive_secs, keepaliveInterval);
	if ((new_token > kafka_follower_keepalive_token_provided_by_mgmt) ||
	    ((new_token == kafka_follower_keepalive_token_provided_by_mgmt) && (nvmeibt_follower_keep_alive_secs != keepaliveInterval))) {
		kafka_follower_keepalive_token_provided_by_mgmt = new_token;
		nvmeibt_follower_keep_alive_secs = keepaliveInterval;
		NVMEIBT_GLOBAL_MARK_REPORT_TARGET_HAS_NEW_DATA(vwtg3j6);
	}
}

int64_t nvmeibt_kafka_get_leader_keepalive_token_provided_by_mgmt(void) {	// Called from multiple threads
	return kafka_leader_keepalive_token_provided_by_mgmt;
}

static void kafka_set_leader_keepalive_token_provided_by_mgmt(int64_t new_token, int64_t keepaliveInterval) {
	N_Tf(4vs8skl, "token=@INT64_TX-->@INT64_TX keepaliveInterval=@INT64_TX-->@INT64_TX",
		 kafka_leader_keepalive_token_provided_by_mgmt, new_token, nvmeibt_leader_keep_alive_secs, keepaliveInterval);
	kafka_leader_keepalive_token_provided_by_mgmt = max(kafka_leader_keepalive_token_provided_by_mgmt, new_token);
	nvmeibt_leader_keep_alive_secs = keepaliveInterval;
}

/***************************   Kafka defaults  ********************************/
// https://docs.confluent.io/platform/current/installation/configuration/consumer-configs.html#consumerconfigs_fetch.min.bytes
// Two closely related timeouts (max.poll.interval.ms is pretty new to Kafka) Aimed to rebalance between overloaded brokers
//static int	k_session_timeout_ms = (1 * 45 * 1000);		// session.timeout.ms  (default 45 sec) // For a pure heartbeat thread max inactivity before disconnected
//static int	k_max_poll_interval_ms = (5 * 60 * 1000);	// max.poll.interval.ms // A client consumer is disconnected after such inactivity period
static int	k_heartbeat_interval_ms = (3 * 1000);			// heartbeat.interval.ms (default 3 sec) // When working with a "group", recommended below (k_session_timeout_ms / 3), A client is "disconnected" and dropped from group.id. Resulting in rebalance between the remaining clients
//static int	k_max_poll_records = 500;					// max.poll.records (default 500) // max n records returned per poll
//static int	k_request_timeout_ms = (30 * 1000);			// request.timeout.ms (default 30s) // max client wait for the response of a request
//static int	k_fetch_min_bytes = 1;						// fetch.min.bytes (default 1 char) // Will wait for this number of bytes before returning an answer
//static int	k_fetch_max_bytes = (50 * 1024 * 1024);		// fetch.max.bytes (default 50M) //
//static int	k_fetch_max_wait_ms = 500;					// fetch.max.wait.ms (default 500ms) // If no answer, Configuration property `fetch.wait.max.ms` (500) should be set lower than `socket.timeout.ms` (600) by at least 1000ms to avoid blocking and timing out sub-sequent requests
//static int	k_receive_buffer_bytes = (64 * 1024);		// receive.buffer.bytes (default 64K) // TCP receive buffer . (-1 for OS default)
//static int	k_max_partition_fetch_bytes;				// max.partition.fetch.bytes	(default 1M) // The requestor specifies
//static int	k_isolation_level;							// isolation.level (default read_uncommitted - also returnes aborted and not fully committed)
//static int	k_auto_commit_interval_ms = 5000;			// auto.commit.interval.ms (default 5s) // Afterwards, consumer offsets are auto-committed to Kafka if enable.auto.commit
//static int	k_check_crcs = true;						// check.crcs // (broker-->consumer)
//static long	k_reconnect_backoff_max_ms = 1000;			// reconnect.backoff.max.ms (fefault 1s) // Per Host. The exponential reconnect backouff max
//static long	k_reconnect_backoff_ms = 50;				// reconnect.backoff.ms (default 50ms) // Per Host. backoff initial value
//static long	k_retry_backoff_ms = 100;					// retry.backoff.ms (default 100) // Per topic partition
//static long	k_socket_connection_setup_timeout_max_ms = 30000;	// socket.connection.setup.timeout.max.ms (default 30s) // Max backoff
//static long	k_socket_connection_setup_timeout_ms = 10000;	    // socket.connection.setup.timeout.ms (default 10s) // Initial retry timeout

#define K_DEFAULT_CONSUMER_CONFIG \
	{"socket.timeout.ms",			"600"},				\
	{"enable.auto.commit",			"false"},			/* enable.auto.commit (default true), Kafka commits consumer's offset in the background. We disable it and use explicit commit when we done asyncronously processing the message*/ \
	{"auto.offset.reset",			"earliest"},		/* Start on the msg following the last committed one. Can use "latest".  none: throw exception to the consumer if no previous offset is found for the consumer's group*/ \
	{"bootstrap.servers",			""},				/* Overridden by the value of KAFKA_SERVERS from nvmesh.conf*/ \
	{"group.id",					""},				/* Overriden with machine name. We need that to track offset separately for each Toma/group*/ \
	{"security.protocol",			"ssl"}, 			\
	{"enable.ssl.certificate.verification", "true"}, 	\
	{"ssl.ca.location",				""},				/* Overridden by the value of KAFKA_CA from nvmesh.conf. CA certificate file for verifying the broker's certificate.*/\
	{"ssl.certificate.location",	""},				/* Overridden by the value of KAFKA_CA from nvmesh.conf. Client's certificate */\
	{"ssl.key.location", 			""},				/* Client's key */\
	{"ssl.key.password",			""},				/* Key password, if any. */\
	{"ssl.endpoint.identification.algorithm", "none"}
//	{"debug",							"all"},			// "generic,broker,topic,metadata,feature,queue,msg,protocol,cgrp,security,fetch,interceptor,plugin,consumer,admin,eos,mock,assignor,conf,all"

#define K_DEFAULT_PRODUCER_CONFIG \
	{"socket.timeout.ms",			"600"},				\
	{"bootstrap.servers",			""},				/* Overridden by the value of KAFKA_SERVERS from nvmesh.conf*/\
	{"client.id",					"report_to_mgmt_producer"}, \
	{"security.protocol",			"ssl"},				\
	{"enable.ssl.certificate.verification", "true"},	\
	{"ssl.ca.location",				""},				/* Overridden by the value of KAFKA_CA from nvmesh.conf. CA certificate file for verifying the broker's certificate.*/\
	{"ssl.certificate.location",	""},				/* Overridden by the value of KAFKA_CA from nvmesh.conf. Client's certificate */\
	{"ssl.key.location", 			""},				/* Client's key */\
	{"ssl.key.password",			""},				/* Key password, if any. */\
	{"ssl.endpoint.identification.algorithm", "none"}
//	{"debug",							"all"},		// "generic,broker,topic,metadata,feature,queue,msg,protocol,cgrp,security,fetch,interceptor,plugin,consumer,admin,eos,mock,assignor,conf,all"

/******************************************************************************/
/************************    Service functions     ****************************/
/******************************************************************************/
static int extract_messageType_params_from_json_first_level(const struct mm_json_elem *json_tree_root, struct messageType_params_ctx *messageType_params)
{
	int						i;
	struct mm_json_kv_pair	*root_kv;
	unsigned int			parsed_mask = 0;

	NFIN;
	for (i = 0; i < json_tree_root->dict.len; i++) {
		root_kv = &json_tree_root->dict.elements[i];
		if 			(!strcmp(root_kv->key, "messageType")) {
			if (root_kv->value->type == JSON_E_STR) {
				parsed_mask |= 0x1;
				messageType_params->messageType_len = nvmeibt_strlcpy(messageType_params->messageType, root_kv->value->str, sizeof(messageType_params->messageType));
			} else {
				N_Ef(tcvjs82, "Unexpected @STR=@INT64_TD", root_kv->key, root_kv->value->num);
			}
		} else if	(!strcmp(root_kv->key, "messageTypeVersion")) {
			parsed_mask |= 0x2;
			messageType_params->messageTypeVersion = root_kv->value->num;
		}
	}
	NFOUT;
	if (parsed_mask != 0x3) {
		N_Wf(jeus6db, "Imperfect parsing, missing/excess params mask=@X", parsed_mask);
	}
	return (parsed_mask == 0x3 ? 0 : -1);
}

struct key_val_strs {
	char	*key;
	char	*val;
};

static struct timespec		kafka_last_consume_timespec = TIMESPEC_ZERO;
static int64_t				kafka_mgmt_zone_number = -1;

int64_t nvmeibt_kafka_get_kafka_mgmt_zone_number(void)
{
	return kafka_mgmt_zone_number;
}

bool nvmeibt_kafka_is_mgmt_zone_specified(void)
{
	return (kafka_mgmt_zone_number >= 0);
}

static int generate_topic_name_using_zone(char *dst, size_t dst_size, const char *base_name, bool is_producer) {
	size_t len = 0;
	if (nvmeibt_kafka_is_mgmt_zone_specified()) {
		len = snprintf(dst, dst_size, "zone%ld%s", kafka_mgmt_zone_number, base_name);
	} else if (is_producer) {
		len = snprintf(dst, dst_size, "default%s",                         base_name);
	} else {
		len = nvmeibt_strlcpy(dst, base_name, dst_size);	// DHSH: I am not even sure this line can be called
	}
	return (len < dst_size);
}

static bool is_waiting_for_reinit(void)
{
	return ((kafka_applied_init_counter < kafka_requested_init_counter) || (kafka_applied_init_preserve_state_vars_counter < kafka_requested_init_preserve_state_vars_counter));
}

static void check_if_kafka_init_preserve_state_vars_required(rd_kafka_resp_err_t err) {
	struct timespec						now;
	if (err == RD_KAFKA_RESP_ERR_NO_ERROR)
		return;
	switch (err) {
	case RD_KAFKA_RESP_ERR__FATAL:
	case RD_KAFKA_RESP_ERR__SSL:
	case RD_KAFKA_RESP_ERR__AUTHENTICATION:
	case RD_KAFKA_RESP_ERR_TOPIC_AUTHORIZATION_FAILED:
	case RD_KAFKA_RESP_ERR_GROUP_AUTHORIZATION_FAILED:
	case RD_KAFKA_RESP_ERR_CLUSTER_AUTHORIZATION_FAILED:
	case RD_KAFKA_RESP_ERR_UNSUPPORTED_SASL_MECHANISM:
	case RD_KAFKA_RESP_ERR_ILLEGAL_SASL_STATE:
	case RD_KAFKA_RESP_ERR_TRANSACTIONAL_ID_AUTHORIZATION_FAILED:
	case RD_KAFKA_RESP_ERR_SASL_AUTHENTICATION_FAILED:
	case RD_KAFKA_RESP_ERR_DELEGATION_TOKEN_AUTHORIZATION_FAILED:
		break;			// Fatal or security error. Restart kafka.
	case RD_KAFKA_RESP_ERR__TRANSPORT:
	case RD_KAFKA_RESP_ERR_BROKER_NOT_AVAILABLE:
	case RD_KAFKA_RESP_ERR_NOT_COORDINATOR:
	case RD_KAFKA_RESP_ERR_COORDINATOR_NOT_AVAILABLE:
	case RD_KAFKA_RESP_ERR__ALL_BROKERS_DOWN:
	case RD_KAFKA_RESP_ERR__TIMED_OUT:
	case RD_KAFKA_RESP_ERR__PARTITION_EOF:		// No more messages in consumer partition
	case RD_KAFKA_RESP_ERR__WAIT_COORD:
	case RD_KAFKA_RESP_ERR__WAIT_CACHE:
	case RD_KAFKA_RESP_ERR__DESTROY:
		N_Tf(__AUTOID__, "kafka err[@INT]='@STR'", err, rd_kafka_err2str(err));
		return;		// Definitely ignore transient network errors.
	default:
		// return;	Should we ignore errors that do not look like security related
		break;
	}
	N_Wf(__AUTOID__, "kafka err[@INT]='@STR'", err, rd_kafka_err2str(err));
	getnstimeofday_boot(&now);
	if ((timespec_diff_ns(now, kafka_last_restart_timestamp) > SEC_TO_NSEC(30)) && !is_waiting_for_reinit()) {
		N_IMf(hu8a475, "Marking kafka soft init required");
		kafka_requested_init_preserve_state_vars_counter++;
	}
}

static void __print_partitions_list(const rd_kafka_topic_partition_list_t *pl)
{
	int i, n_part = (pl ? pl->cnt : 0);
	for (i = 0; i < n_part; ++i) {
		const rd_kafka_topic_partition_t *p = &pl->elems[i];
		N_Tf(cvbz84k21, "@INT) topic=@STR part[@INT].offset=@LD, err=@INT", i, p->topic, p->partition, p->offset, p->err);
	}
}

static void error_event_cb(rd_kafka_t *rk, int err, const char *reason, __attribute__((__unused__)) void *opaque) {
	if (err == RD_KAFKA_RESP_ERR__FATAL) {
		char errstr[512];
		err = rd_kafka_fatal_error(rk, errstr, sizeof(errstr));
		N_Ef(u8u8nu1, "@STR: err[@INT]=@STR errstr=@STR reason=@STR", rd_kafka_name(rk), err, rd_kafka_err2str(err), errstr, reason);
	} else {
		N_Wf(u8u8nh4, "@STR: err[@INT]=@STR not_fatal reason=@STR",   rd_kafka_name(rk), err, rd_kafka_err2str(err),         reason);
	}
	// Anyhow, something failed, and we do not need to wait for its completion, Can we commit it? Good question. We do not
	check_if_kafka_init_preserve_state_vars_required(err);
}

// Rebalancing callback is not needed according to current design as in consumer topic has 1 partition only, and consumer group there contains only 1 Toma
static void consumer_cb_on_rebalance(rd_kafka_t *rk, rd_kafka_resp_err_t err, rd_kafka_topic_partition_list_t *pl, __attribute__((__unused__)) void *opaque) {
	N_Tf(u8u8nh6, "@STR: err[@INT]=@STR n_part=@INT", rd_kafka_name(rk), (int)err, rd_kafka_err2str(err), (pl ? pl->cnt : 0));
	switch (err) {
	case RD_KAFKA_RESP_ERR__ASSIGN_PARTITIONS:
		// rd_kafka_assign(rk, pl); // We use manual assignment so no need to do that. The callback just notifies us that a rebalance occurred
		break;
	case RD_KAFKA_RESP_ERR__REVOKE_PARTITIONS:
		if (0) check_if_kafka_init_preserve_state_vars_required(err);	// Our partitions were revoked (due to rebalance, timeout, etc). Since we use manual assignment, we need to trigger reinit to properly re-assign
		break;
	default:
		break;
	}
}

void consumer_cb_on_offset_commit(rd_kafka_t *rk, rd_kafka_resp_err_t err, rd_kafka_topic_partition_list_t *pl, __attribute__((__unused__)) void *opaque) {
	N_Tf(u8u8nh7, "@STR: err[@INT]=@STR n_part=@INT", rd_kafka_name(rk), (int)err, rd_kafka_err2str(err), (pl ? pl->cnt : 0));
	__print_partitions_list(pl);
}

static rd_kafka_conf_t *alloc_and_init_kafka_conf(const struct key_val_strs *kv_array, size_t n_kv, const char *group_id_str,
												  void (*dr_msg_cb)(rd_kafka_t *,const rd_kafka_message_t *, void *opaque))
{
	rd_kafka_resp_err_t			k_err	__attribute__((__unused__));
	char						errstr[512];
	rd_kafka_conf_t				*conf = NULL;
	rd_kafka_conf_res_t			conf_res;
	int							i;

	NFIN;
	conf = rd_kafka_conf_new();
	for (i = 0; i < (int)n_kv; i++) {
		const char *key = kv_array[i].key;
		const char *val = kv_array[i].val;

		if (strcmp(key, "security.protocol") == 0) {
			if (!(kafka_mtls_ssl__is_enabled))
				continue;
		} else if (strcmp(key, "enable.ssl.certificate.verification") == 0) {
			if (!(kafka_mtls_ssl__is_enabled))
				continue;
		} else if (strcmp(key, "ssl.endpoint.identification.algorithm") == 0) {
			if (!(kafka_mtls_ssl__is_enabled) || !(kafka_mtls_ssl__is_hostname_verification_disabled))
				continue;
		}
		if (strlen(val) == 0) {		// Generate a unique value only if there is no default (a default remains as is)
			if (strcmp(key, "group.id") == 0) {
				val = group_id_str;
			} else if (strcmp(key, "client.id") == 0) {
				// In most cases every machine has its own group (for committed & stored offsets)
				val = (char *)nvmeibt_get_my_hostname();
			} else if (strcmp(key, "bootstrap.servers") == 0) {
				val = (char *)nvmeibt_Str_str(kafka_bootstrap_servers_str_from_nvmesh_conf);
			} else if (strcmp(key, "ssl.ca.location") == 0) {
				if (!(kafka_mtls_ssl__is_enabled))
					continue;
				val = (char *)nvmeibt_Str_str(kafka_mtls_ssl__ca);
			} else if (strcmp(key, "ssl.certificate.location") == 0) {
				if (!(kafka_mtls_ssl__is_enabled))
					continue;
				val = (char *)nvmeibt_Str_str(kafka_mtls_ssl__toma_certificate);
			} else if (strcmp(key, "ssl.key.location") == 0) {
				if (!(kafka_mtls_ssl__is_enabled))
					continue;
				val = (char *)nvmeibt_Str_str(kafka_mtls_ssl__key_location);
			} else if (strcmp(key, "ssl.key.password") == 0) {
				if (!(kafka_mtls_ssl__is_enabled))
					continue;
				val = (char *)nvmeibt_Str_str(kafka_mtls_ssl__key_password);
			} else {
				N_Wf(dcz6h2p, "key=@STR val=''", key);
			}
		}
		//
		N_Tf(hsjue3k, "rd_kafka_conf_set key='@STR' val='@STR'", key, val);
		conf_res = rd_kafka_conf_set(conf, key, val, errstr, sizeof(errstr));
		if (conf_res != RD_KAFKA_CONF_OK) {
			N_Ef(vvnyeis, "Failed rd_kafka_conf_set key='@STR' val='@STR' '@STR'", key, val, errstr);
			if (strcmp("auto.offset.reset", key) != 0) {
				goto out_err;
			}
		}
	}
	if (dr_msg_cb) {	// Install a delivery-error callback for producers only.
		rd_kafka_conf_set_dr_msg_cb(conf, dr_msg_cb);
		N_Tf(vfhjiek, "CB=@PTR", dr_msg_cb);
	} else {			// For consumers, install rebalance callback
		rd_kafka_conf_set_rebalance_cb(    conf, consumer_cb_on_rebalance);
		rd_kafka_conf_set_offset_commit_cb(conf, consumer_cb_on_offset_commit);
	}
	rd_kafka_conf_set(conf, "debug", "security,broker,protocol", errstr, sizeof(errstr));	// Write to stderr (or stdout). Anyway, it is lost.
	rd_kafka_conf_set_error_cb(conf, error_event_cb);
	goto out;
out_err:
	if (conf) {
		rd_kafka_conf_destroy(conf);
		conf = NULL;
	}
out:
	NFOUT;
	return conf;
}

/******************************************************************************/
/************                   PRODUCERS                       ***************/
/******************************************************************************/
static rd_kafka_t* __create_kafka_new_obj(enum rd_kafka_type_t who, rd_kafka_conf_t **cfg, const char *topic_name, rd_kafka_topic_t **topic_pptr) {
	char errstr[512];
	rd_kafka_t *rv = NULL;
	const bool is_producer = (who == RD_KAFKA_PRODUCER);
	rd_kafka_topic_conf_t* topic_conf = rd_kafka_topic_conf_new();
	if (is_producer)
		*topic_pptr = NULL;									// Topic was not created yet
	if (*cfg == NULL)
		goto _out;										// Invalid obj configuration
	rv = rd_kafka_new(who, *cfg, errstr, sizeof(errstr));
	if (!rv) {
		N_Ef(tkckno0, "Failed to create new @STR[@STR]: @STR", (is_producer ? "prod" : "cons"), topic_name, errstr);
		goto _out;										// Invalid obj configuration
	}
	*cfg = NULL;	// Configuration object is now owned, and freed, by the 'rv' instance.
	// rd_kafka_conf_set_log_cb(rv, logger);
	rd_kafka_set_log_level(rv, LOG_NOTICE);
	N_Tf(tkckno1, "@STR[@STR]=@PTR, topic=@STR", (is_producer ? "prod" : "cons"), rd_kafka_name(rv), rv, topic_name);

	if (!is_producer) {	// For consumers, attach polling method (high-level consumer API)
		rd_kafka_resp_err_t k_err = rd_kafka_poll_set_consumer(rv);	// Enable high-level consumer mode
		if (k_err) {
			N_Wf(tkckno2, "Failed poll_set_consumer() err='@STR'", rd_kafka_err2str(k_err));
			rd_kafka_destroy(rv);
			rv = NULL;
		} else {}	// High-level consumers don't need rd_kafka_topic_t handles, Topic assignment is done via rd_kafka_assign() or rd_kafka_subscribe()
	} else {		// Attach a topic (for producers only)
		*topic_pptr = rd_kafka_topic_new(rv, topic_name, topic_conf);		// Maybe use rd_kafka_topic_conf_set() ?
		if (!*topic_pptr) {
			N_Ef(tkckno3, "Failed kafka_topic_new: @STR, destroying kafka obj", topic_name);
			rd_kafka_destroy(rv);
			rv = NULL;
		} else {
			topic_conf = NULL;	// topic destroys conf as per rdkafka.h
		}
	}
_out:
	if (*cfg) {
		rd_kafka_conf_destroy(*cfg);
		*cfg = NULL;
	}
	if (topic_conf)
		rd_kafka_topic_conf_destroy(topic_conf);
	return rv;
}

enum KAFKA_OUTGOING_MSG_STATE {
	KAFKA_OUTGOING_MSG_STATE_NOT_SENT = 'N',
	KAFKA_OUTGOING_MSG_STATE_SENT_TO_KAFKA = 'S',
	KAFKA_OUTGOING_MSG_STATE_ACCEPTED_BY_KAFKA = 'A',
	KAFKA_OUTGOING_MSG_STATE_REJECTED_BY_KAFKA = 'X',
};
static bool KAFKA_OUTGOING_MSG_STATE_got_cb(enum KAFKA_OUTGOING_MSG_STATE s) {
	return (s == KAFKA_OUTGOING_MSG_STATE_ACCEPTED_BY_KAFKA) || (s == KAFKA_OUTGOING_MSG_STATE_REJECTED_BY_KAFKA);
}

struct kafka_outgoing_msg {
	enum NVMEIBT_KAFKA_OUTGOING_MSGS_PRIORITY		out_priority;
	int 											n_send_retries;
	char											unique_key[NVMEIBT_KAFKA_MAX_UNIQUE_KEY_LEN];
	char											*val;
	size_t											val_len;
	struct xdlist									kafka_outgoing_msgs_link;
	enum KAFKA_OUTGOING_MSG_STATE					kafka_outgoing_msg_state;
};

// Per-message delivery callback (triggered by poll() or flush())
// when a message has been successfully delivered or permanently failed delivery (after retries).
// Function runs in kafka main thread the thread that.
static void all_producers_msg_to_mgmt_delivery_cb(rd_kafka_t *k, const rd_kafka_message_t *k_msg, void *opaque) {
	struct kafka_outgoing_msg *msg = (struct kafka_outgoing_msg *)k_msg->_private;
	const int n_in_air = atomic_add(-1, &kafka_n_sends_in_the_air);
	NTOMA_ASSERT(tmiiakm1, ((n_in_air >= 0) && msg), "in_air_km=@INT, msgptr=@PTR. Memory corruption", n_in_air, msg);
	(void)opaque;	// We use static vars instead of generic opaque context. If needed set with rd_kafka_conf_set_opaque()
	N_Tf(jsnewij1, "@STR: k_handle=@PTR, in_air=@INT, msgptr=@PTR, err=@INT", rd_kafka_name(k), k, n_in_air, msg, k_msg->err);
	if (k_msg->err) {
		msg->kafka_outgoing_msg_state = KAFKA_OUTGOING_MSG_STATE_REJECTED_BY_KAFKA;
		N_Wf(tvsjhgr, "@STR: k_handle=@PTR, in_air=@INT, msgptr=@PTR, err=@STR, Message delivery failed", rd_kafka_name(k), k, n_in_air, msg, rd_kafka_err2str(k_msg->err));
	} else {
		msg->kafka_outgoing_msg_state = KAFKA_OUTGOING_MSG_STATE_ACCEPTED_BY_KAFKA;
	}
}

static struct t_producer_impl {
	rd_kafka_t       *msg_to_mgmt_producer;
	rd_kafka_topic_t *msg_to_mgmt_producer_topic;
} k_high_priority, k_low_priority, k_keepalive;

static int producer_send_msg(struct t_producer_impl *k, struct kafka_outgoing_msg *msg) {
	rd_kafka_topic_t *k_topic = k->msg_to_mgmt_producer_topic;
	const char 			*key = (msg->unique_key[0] == '\0') ? NULL : (char*)msg->unique_key;
	const size_t		key_len = key ? (strnlen(key, sizeof(msg->unique_key)) + 1) : 0;
	char 				*val = msg->val;
	size_t				val_len = msg->val_len;
	int	n_in_air, err;

	N_Tf(rbsuik4, "msgptr=@PTR, attempt=@INT", msg, msg->n_send_retries);
	if ((!k_topic) || is_waiting_for_reinit())
		return -1;

	if (val[val_len - 1] == '\0')
		val_len -= 1;	// Seems as if the string terminating \0 is driving MGMT JSON parser crazy
	n_in_air = atomic_add(1, &kafka_n_sends_in_the_air);       // If a msg is about to be sent, we know the n_sends_in_the_air was already increased
	err = rd_kafka_produce(k_topic, RD_KAFKA_PARTITION_UA, RD_KAFKA_MSG_F_COPY, (void*)val, val_len, key, key_len, (void*)msg);
	if (err == 0) {
		N_Tf(b5v9skq, "@STR: produced key=@STR msgptr=@PTR, in_air_km=@INT", rd_kafka_topic_name(k_topic), key, msg, n_in_air);
		NVMEIBT_LONG_TRACE_WRAPPER(tvsh875, "", val, val_len);
		return 0;
	}
	n_in_air = atomic_add(-1, &kafka_n_sends_in_the_air);
	NTOMA_ASSERT(tmiiakm0, (n_in_air >= 0), "in_air_km=@INT is negative. Memory corruption", n_in_air);
	N_Wf(n58skal, "@STR: Failed to produce to kafka msg to err='@STR', in_air_km=@INT  (@AUTO_ERRNO)", rd_kafka_topic_name(k_topic), rd_kafka_err2name(rd_kafka_last_error()), n_in_air);
	check_if_kafka_init_preserve_state_vars_required(RD_KAFKA_RESP_ERR__FATAL);
	return -1;
}

static int producer_init(struct t_producer_impl *k, const char *topic_str, const struct key_val_strs *kv_arr, int n_kv) {
	rd_kafka_conf_t *conf = alloc_and_init_kafka_conf(kv_arr, n_kv, NULL, all_producers_msg_to_mgmt_delivery_cb);
	k->msg_to_mgmt_producer = __create_kafka_new_obj(RD_KAFKA_PRODUCER, &conf, topic_str, &k->msg_to_mgmt_producer_topic);
	return (k->msg_to_mgmt_producer ? 0 : -1);
}

static void producer_close(struct t_producer_impl *k) {
	if (k->msg_to_mgmt_producer) {
		N_Tf(uz2xhnk, "@STR @STR", rd_kafka_name(k->msg_to_mgmt_producer), rd_kafka_topic_name(k->msg_to_mgmt_producer_topic));
		rd_kafka_flush(k->msg_to_mgmt_producer, 0 /*Timeout*/);		// Timeout not needed as we waited for atomic inair messages to be zero
		N_Tf(ft6w38u, "rd_kafka_flush ended");
		rd_kafka_topic_destroy(k->msg_to_mgmt_producer_topic);		// Not mandatory, cleanup will happen automatically
		N_Tf(ft61s8u, "rd_kafka_topic_destroy ended");
		k->msg_to_mgmt_producer_topic = NULL;
		rd_kafka_destroy(k->msg_to_mgmt_producer);
		k->msg_to_mgmt_producer = NULL;
	}
}

static int high_priority_msg_to_mgmt_producer_init(void) {
	const struct key_val_strs kv[] = { K_DEFAULT_PRODUCER_CONFIG };
	char str[128];
	generate_topic_name_using_zone(str, sizeof(str), ".management.priority.1.0.0", 1);
	return producer_init(&k_high_priority, str, kv, ARRAY_SIZE(kv));
}

static int low_priority_msg_to_mgmt_producer_init(void) {
	const struct key_val_strs kv[] = { K_DEFAULT_PRODUCER_CONFIG };
	char str[128];
	if (!nvmeibt_kafka_is_mgmt_zone_specified()) {
		N_Tf(usbcsjn, "Skipping !nvmeibt_kafka_is_mgmt_zone_specified()");
		return 0;
	}
	generate_topic_name_using_zone(str, sizeof(str), ".management.low.1.0.0", 1);
	return producer_init(&k_low_priority, str, kv, ARRAY_SIZE(kv));
}

static int keepalive_msg_to_mgmt_producer_init(void) {
	const struct key_val_strs kv[] = { K_DEFAULT_PRODUCER_CONFIG };
	return producer_init(&k_keepalive, "default.management.keepalive.1.0.0", kv, ARRAY_SIZE(kv));
}

/******************************************************************************/
/**********************    OUTGOING MSGS QUEUE       **************************/
/******************************************************************************/
/* All outgoing msgs first go into an internal queue, and only the main thread
 * actually sends them to kafka producer queue.
 * Upon producer callback, the message is marked as is_accepted_by_kafka,
 * and the message can be deleted (again in the main thread)
 * If Kafka is disconnected, and reconnected, all the messages that are still
 * in the queue are resent
 */
static struct kafka_outgoing_msgs_queue_t {
	XDLIST_DECLARE(, struct kafka_outgoing_msg, kafka_outgoing_msgs_link) list;
	pthread_mutex_t mutex;								// Protect the list add/insert
	struct kafka_outgoing_msg *last_sent_in_list;		// Todo: Remove me and use 2 lists (to_send, awaiting_completion). and Accessed only via kafka thread, no neet to protect. Used instead of 2 lists implementation (to_send, already_sent)
} komq = {XDLIST_INIT(komq.list), PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP, NULL};

#define KAFKA_OUTGOING_MSGS_QUEUE_LOCK(name) ({												\
	const int rv_mut = pthread_mutex_lock(&komq.mutex);										\
	NTOMA_ASSERT(name ## _assert, rv_mut == 0, "failed KAFKA_OUTGOING_MSGS_QUEUE_LOCK(%m)");\
})

#define KAFKA_OUTGOING_MSGS_QUEUE_UNLOCK(name) ({											\
	const int rv_mut = pthread_mutex_unlock(&komq.mutex);									\
	NTOMA_ASSERT(name ## _assert, rv_mut == 0, "failed KAFKA_OUTGOING_MSGS_QUEUE_UNLK(%m)");\
})

void nvmeibt_kafka_outgoing_msgs_queue_add(const char *unique_key, const char *val, size_t val_len, enum NVMEIBT_KAFKA_OUTGOING_MSGS_PRIORITY out_priority) {
	struct kafka_outgoing_msg *msg;
	NFIN;
	msg = NNVMEIBT_BM_CALLOC(tcdvask, sizeof(*msg));
	msg->val = NNVMEIBT_BM_ALLOC(4rbs8734, val_len);
	if (unique_key)
		nvmeibt_strlcpy(msg->unique_key, unique_key, sizeof(msg->unique_key));
	else
		msg->unique_key[0] = '\0';
	memcpy(msg->val, val, val_len);
	msg->val_len = val_len;
	msg->kafka_outgoing_msg_state = KAFKA_OUTGOING_MSG_STATE_NOT_SENT;
	msg->out_priority = out_priority;
	XDLIST_INIT_LINK(&(msg->kafka_outgoing_msgs_link), NULL);
	KAFKA_OUTGOING_MSGS_QUEUE_LOCK(v754kmw);					// This function can be called from any thread
	XDLIST_ADD_TAIL(&komq.list, msg);
	KAFKA_OUTGOING_MSGS_QUEUE_UNLOCK(5bys83j);
	NFOUT;
}

static void kafka_outgoing_msgs_queue_process_msgs_accepted_by_kafka(void) {	// Func called from kafka thread
	struct kafka_outgoing_msg *msg;
	int n_deleted = 0, n_retried = 0;
	KAFKA_OUTGOING_MSGS_QUEUE_LOCK(5vbsd8j);	// against _add(), get_num_msgs()
	XDLIST_FOREACH_SAFE(msg, &komq.list) {	// Start from the oldest message
		const enum KAFKA_OUTGOING_MSG_STATE m_state = msg->kafka_outgoing_msg_state;
		if ((n_deleted + n_retried) > 10)
			goto out;				// Processed enough for this cycle, dont hold lock for too long
		if (!KAFKA_OUTGOING_MSG_STATE_got_cb(m_state)) {
			N_Tf(osmriw3, "msgptr=@PTR Not accepted yet. msg_status=@CHAR", msg, m_state);
			goto out;
		}
		if (msg == komq.last_sent_in_list)
			komq.last_sent_in_list = NULL;
		XDLIST_DEL(&(msg->kafka_outgoing_msgs_link));
		if (msg->kafka_outgoing_msg_state == KAFKA_OUTGOING_MSG_STATE_REJECTED_BY_KAFKA) {
			msg->kafka_outgoing_msg_state = KAFKA_OUTGOING_MSG_STATE_NOT_SENT;
			msg->n_send_retries++;
			XDLIST_ADD_TAIL(&komq.list, msg);
			n_retried++;
		} else {		// Accepted
			NNVMEIBT_BM_FREE(nf58ewl, msg->val);
			NNVMEIBT_BM_FREE(mvidi03, msg);
			n_deleted++;
		}
	}
	if (XDLIST_EMPTY(&komq.list)) {
		komq.last_sent_in_list = NULL;
	}
out:
	if ((n_deleted + n_retried) != 0)
		N_Tf(5bs902l, "n_deleted=@INT, n_resched=@INT, n_remaining=@INT", n_deleted, n_retried, XDLIST_N_ELEMNTS(&komq.list));
	KAFKA_OUTGOING_MSGS_QUEUE_UNLOCK(hdi39j8);
}

static void kafka_outgoing_msgs_queue_rewind_for_resend(void) {	// Func called from kafka thread
	komq.last_sent_in_list = NULL;
}

static int kafka_outgoing_msgs_queue_get_num_msgs(void) {	// Called NOT from kafka thread
	int rv;
	//KAFKA_OUTGOING_MSGS_QUEUE_LOCK(5vbsqq1);			// No need to lock, eventual consistency of list size.
	rv = XDLIST_N_ELEMNTS(&komq.list);
	//KAFKA_OUTGOING_MSGS_QUEUE_UNLOCK(5vbsqq2);
	return rv;
}

static void kafka_outgoing_msgs_queue_send_pending_msgs_to_kafka_producer(void) {
	struct kafka_outgoing_msg *msg, *end;
	// Currently send all NEW msgs, In the future, we might want to send only one/some of them at a time
	// Del does not occur in parallel (since it is in the same thread. Add can occur, but only after last_outgoing_msg_in_queue, so we do not care
	KAFKA_OUTGOING_MSGS_QUEUE_LOCK(26gqwu4);	// against _add() from TOMA's main thread
	end = XDLIST_LAST(&komq.list);
	if (!komq.last_sent_in_list) {	// The last sent was already removed. Start from the current first
		msg = XDLIST_FIRST(&komq.list);
	} else if (komq.last_sent_in_list == end) {	// We already sent the last one
		msg = NULL;
	} else {  // There are msgs between the last_sent and the end
		msg = XDLIST_LOOP_NEXT(komq.last_sent_in_list, &komq.list);
	}
	KAFKA_OUTGOING_MSGS_QUEUE_UNLOCK(26gqwu5);
	for (; komq.last_sent_in_list != end;
			komq.last_sent_in_list = msg,
			msg = XDLIST_LOOP_NEXT(msg, &komq.list)) { // msg is never the last, and only add (after the last) can occur in parallel, so no need to lock
		const enum KAFKA_OUTGOING_MSG_STATE m_state = msg->kafka_outgoing_msg_state;
		if ((m_state == KAFKA_OUTGOING_MSG_STATE_NOT_SENT) || (m_state == KAFKA_OUTGOING_MSG_STATE_REJECTED_BY_KAFKA)) {
			msg->kafka_outgoing_msg_state = KAFKA_OUTGOING_MSG_STATE_SENT_TO_KAFKA;
			if (msg->out_priority == NVMEIBT_KAFKA_OUTGOING_MSGS_PRIORITY_HIGH) {
				if (producer_send_msg(&k_high_priority, msg) != 0)
					break;
			} else if (msg->out_priority == NVMEIBT_KAFKA_OUTGOING_MSGS_KEEPALIVE) {
				if (producer_send_msg(&k_keepalive, msg) != 0)
					break;
			} else {
				if (msg->out_priority != NVMEIBT_KAFKA_OUTGOING_MSGS_PRIORITY_LOW)
					N_Ef(26gqwu6, "msgptr=@PTR OOPS out_priority=@INT, redirecting to low priority", msg, msg->out_priority);
				if (producer_send_msg(&k_low_priority, msg) != 0)
					break;
			}
		} else if (m_state == KAFKA_OUTGOING_MSG_STATE_ACCEPTED_BY_KAFKA) {
			continue; // komq.last_sent_in_list was rewinded, and we are during retransmission, just skip it
		} else {		// Already sent, unknown state - BUG
			N_Ef(26gqwu7, "msgptr=@PTR skipping. msg_status=@CHAR", msg, msg->kafka_outgoing_msg_state);
		}
	}
}

int nvmeibt_kafka_generic_log_msg_to_mgmt_send(const char *unique_key, char *header, char *str, enum NVMEIBT_KAFKA_OUTGOING_MSGS_PRIORITY priority) {
#if 0	// If we decide to send a genericLogMsg to MGMT
	static uint32_t		unique_key_counter = 0x60060000;
	static struct nvmeibt_Str				*json_payload = NULL;
	char									key[NVMEIBT_KAFKA_MAX_UNIQUE_KEY_LEN];
	NFIN;
	if (!json_payload)
		json_payload = NNVMEIBT_STR_ALLOC(54bs7lq);
	nvmeibt_Str_reuse(json_payload);
	// Make sure that we have a key
	if (unique_key) {
		nvmeibt_strlcpy(key, unique_key, sizeof(key));
	} else {
		sprintf(key, "unique_key:%x", ++unique_key_counter);
	}
	nvmeibt_Str_sprintf(json_payload, "{" KAFKA_PRODUCER_MSG_HEADER_FMT "\"header\": \"%s\", \"errText\": \"%s\"}", KAFKA_PRODUCER_MSG_HEADER_VAR("genericLogMsg", 1), header, str);
	nvmeibt_kafka_outgoing_msgs_queue_add(NULL /*key*/, nvmeibt_Str_str(json_payload), nvmeibt_Str_strlen(json_payload) + 1, priority);
	NFOUT;
#else	// #if 0	// If we decide to send a genericLogMsg to MGMT
	(void)unique_key; (void)priority;
	N_Tf(sk6bwk4, "Not sending generic_log_msg to MGMT:--- @STR -- @STR", header, str);
#endif	// #if 0	// If we decide to send a genericLogMsg to MGMT
	return 0;
}

/******************************************************************************/
/************                   CONSUMERS                       ***************/
/******************************************************************************/
#define CONSUMER_DEFAULT_INIT {{0}, NULL, 0 /*zero partition*/, 0, RD_KAFKA_OFFSET_INVALID, RD_KAFKA_OFFSET_INVALID}
static struct t_consumer_impl {
	char topic_name[128];					// Topic name for high-level consumer API
	rd_kafka_t *consumer;
	int32_t consumer_partition;
	int32_t	cnt_zero_consecutive_consumes;
	int64_t consumer_offset;				// Latest received message
	int64_t	offset_committed;				// ACK'ed to kafka
} k_CMD = CONSUMER_DEFAULT_INIT, k_HW_full_config = CONSUMER_DEFAULT_INIT,		// Each Toma consumes such queue
  k_incremental_VOL_updates = CONSUMER_DEFAULT_INIT, k_incremental_TARGET_updates = CONSUMER_DEFAULT_INIT;	// Only leader consumes from here

static void __consumer_stop_on_raft(struct t_consumer_impl *k) {
	if (k->consumer) {
		rd_kafka_assign(k->consumer, NULL);	// Stop consuming by unassigning all partitions
		N_Tf(yzbh7dk, "@STR: raft is stopping to receive incremental updates, consumer_offset=@LD", rd_kafka_name(k->consumer), purify_offset(k->consumer_offset));
	}
}

static void consumer_close(struct t_consumer_impl *k) {
	rd_kafka_resp_err_t			k_err;
	if (k->consumer) {
		N_Tf(vbsjdy3, "@STR @STR", rd_kafka_name(k->consumer), k->topic_name);
		rd_kafka_assign(k->consumer, NULL);		// Unassign all partitions (stops consumption)
		N_Tf(vbsjdy30, "rd_kafka_assign(NULL) ended");
		k_err = rd_kafka_consumer_close(k->consumer);
		N_Tf(vbsjdy34, "close ended");
		if (k_err)
			N_Ef(vbsjdy33, "close err (consumer='@STR' err=@STR", rd_kafka_name(k->consumer), rd_kafka_err2str(k_err));
		rd_kafka_destroy(k->consumer);
		N_Tf(vbsjdy35, "destroy ended");
		k->consumer = NULL;
	}
}

static rd_kafka_resp_err_t __consumer_assign_partition_and_offset(struct t_consumer_impl* k, int64_t start_offset) {
	rd_kafka_topic_partition_list_t *pl = rd_kafka_topic_partition_list_new(1);
	rd_kafka_resp_err_t k_err;
	rd_kafka_topic_partition_list_add(pl, k->topic_name, k->consumer_partition)->offset = purify_offset(start_offset);
	k_err = rd_kafka_assign(k->consumer, pl);
	rd_kafka_topic_partition_list_destroy(pl);
	return k_err;
}

static int consumer_start_from_last_committed_offset(const char *name, struct t_consumer_impl* k) {
	int64_t calc_offset = RD_KAFKA_OFFSET_STORED;	// Default: use stored/committed offset, RD_KAFKA_OFFSET_STORED to let rdkafka fetch the committed offset automatically. If no offset is committed, auto.offset.reset config will be used (set to "earliest")
	const int32_t k_partition = k->consumer_partition;
	const bool already_have_starting_point = is_RD_KAFKA_OFFSET_VALID(k->consumer_offset);
	rd_kafka_resp_err_t k_err;
	if (already_have_starting_point)
		calc_offset = (k->consumer_offset + 1);			// Non purified
	N_Tf(90elhjt2, "@STR: initial_offset=@LD", name, purify_offset(calc_offset));
	k_err = __consumer_assign_partition_and_offset(k, calc_offset);	// Assign partition - rdkafka will resolve RD_KAFKA_OFFSET_STORED to actual committed offset
	if (k_err != RD_KAFKA_RESP_ERR_NO_ERROR) {
		N_Wf(cvniev8, "@STR Failed assign partition, offset=@LD err='@STR'", name, purify_offset(calc_offset), rd_kafka_err2str(k_err));
		return -1;
	}

	if (!already_have_starting_point) {		// Now query the actual position to update our tracking variable, After assignment, we can query what offset we'll actually start from
		rd_kafka_topic_partition_list_t *pl = rd_kafka_topic_partition_list_new(1);
		int64_t low_wm = 0, high_wm = 0;
		const rd_kafka_resp_err_t k_err_watermark = rd_kafka_query_watermark_offsets(k->consumer, k->topic_name, k_partition, &low_wm, &high_wm, 100 /* timeout_ms*/);
		rd_kafka_topic_partition_list_add(pl, k->topic_name, k_partition);
		// k_err = rd_kafka_position(k->consumer, pl);					// gets the current position (next offset to read), alwasy return RD_KAFKA_OFFSET_INVALID, because we dont give it enough time to fetch metadata.
		k_err = rd_kafka_committed(k->consumer, pl, nvmeibt_kafka_get_offset_timeout_secs*1000);	// returns "committed + 1"
		N_Tf(3vx723k, "@STR committed=@LD, Watermark [@LD..@LD] commit_err='@STR' wm_err='@STR'", name, pl->elems[0].offset, low_wm, high_wm, rd_kafka_err2str(k_err), rd_kafka_err2str(k_err_watermark));
		if ((k_err == RD_KAFKA_RESP_ERR_NO_ERROR) && (pl->elems[0].offset >= 0L)) {	// May return RD_KAFKA_OFFSET_INVALID if queue just created and was never read from
			calc_offset = pl->elems[0].offset;
			if ((k_err_watermark == RD_KAFKA_RESP_ERR_NO_ERROR) && ((calc_offset < low_wm) || (calc_offset > high_wm)))
				N_Wf(minwusk, "@STR Kafka error. commited offset @LD is NOT in watermarks [@LD..@LD]", name, calc_offset, low_wm, high_wm);		// This is a valid, When kafka client connets, broker will respond “offset out of range, and "auto.offset.reset" will take the earliest message
		} else {
			calc_offset = RD_KAFKA_OFFSET_BEGINNING;	// Now default is use beginning as fallback
			k_err = __consumer_assign_partition_and_offset(k, calc_offset);
			if (k_err != RD_KAFKA_RESP_ERR_NO_ERROR) {
				N_Wf(cvniev81, "@STR Failed to reset to beginning, err='@STR'", name, rd_kafka_err2str(k_err));
				return -1;
			}
		}
		if (calc_offset > 0)
			k->consumer_offset = glue_topic_change_no_and_offset(KAFKA_TOPIC_CHANGE_NO, calc_offset - 1);	// Not mandatory: As If previous message was read
		rd_kafka_topic_partition_list_destroy(pl);
	}
	N_Tf(3vx723k3, "@STR: consumer_offset=@LD (next will be @LD)", name, purify_offset(k->consumer_offset), purify_offset(calc_offset));
	return 0;
}

void nvmeibt_kafka_new_kafka_mgmt_zone_number_received(int64_t zone_number) {
	if (kafka_mgmt_zone_number != zone_number) {
		if (kafka_mgmt_zone_number >= 0) {
			N_Ef(ismye6c, "mgmt_zone_number=@INT64_TX got @INT64_TX", kafka_mgmt_zone_number, zone_number);
		}
		kafka_mgmt_zone_number = zone_number;
		N_Tf(rvzjivm, "mgmt_zone_number=@INT64_TX", kafka_mgmt_zone_number);
		kafka_requested_init_counter++;	// Stop all new activity, and reinit (using the zone now)
	}
}

static int consumer_read_msg_from_kafka(struct t_consumer_impl *k, struct messageType_params_ctx *out_msg, struct mm_json_elem **out_json_tree_root) {
	rd_kafka_message_t					*k_msg = NULL;
	int									rv;		// -1:err msg, 0:consumed, 1:stop reading
	NTOMA_ASSERT(fvwu2ic, k->consumer, "k_consumer=NULL");

	if (is_waiting_for_reinit())
		return 1;

	k_msg = rd_kafka_consumer_poll(k->consumer, 0 /* non-blocking*/);
	if (!k_msg) {
		N_Df(cvbz84k, "(@STR) returned NULL", rd_kafka_name(k->consumer));
		if ((++k->cnt_zero_consecutive_consumes % 1024) == 0) {		// Periodically check if we still have partition assignment
			rd_kafka_topic_partition_list_t *pl = NULL;
			const rd_kafka_resp_err_t err = rd_kafka_assignment(k->consumer, &pl);
			const int n_part = (pl ? pl->cnt : 0);
			if ((err == RD_KAFKA_RESP_ERR_NO_ERROR) && (n_part != 1)) {
				N_Wf(cvbz84k2, "(@STR) unexpected num partitions=@INT will reinit", rd_kafka_name(k->consumer), n_part);
				__print_partitions_list(pl);
				check_if_kafka_init_preserve_state_vars_required(RD_KAFKA_RESP_ERR__FATAL);
			}
			if (pl) rd_kafka_topic_partition_list_destroy(pl);
		}
		return 1;
	}
	k->cnt_zero_consecutive_consumes = 0;
	N_Tf(fhs8lad, "(@STR) returned k_msg(err=@STR, k_offset=@LD)", rd_kafka_name(k->consumer), rd_kafka_err2str(k_msg->err), k_msg->offset);
	if (k_msg->err == RD_KAFKA_RESP_ERR_NO_ERROR) {
		const int64_t new_offset = glue_topic_change_no_and_offset(KAFKA_TOPIC_CHANGE_NO, k_msg->offset);
		if (strstr((char *)(k_msg->payload), "assphrase")) { // Don't print passphrases to log
			N_IMf(hueom23, "Encrypt msg received, don't print !");
		} else {
			NVMEIBT_LONG_TRACE_WRAPPER(vgsurjk, "", (char *)(k_msg->payload), k_msg->len);
		}
		// Parse as much as possible in this thread, and not in TOMA's main thread
		*out_json_tree_root = parse_json_txt_into_kv_tree(k_msg->payload, k_msg->len);
		if (!*out_json_tree_root) {
			char msg[MGMT_LOG_MSG_MSG_LEN];
			snprintf(msg, sizeof(msg), "Failed parsing of msg from MGMT %.200s", (char*)k_msg->payload);
			nvmeibt_kafka_generic_log_msg_to_mgmt_send(NULL, NULL, msg, NVMEIBT_KAFKA_OUTGOING_MSGS_PRIORITY_HIGH); // no-op; for future proof
			rv = -1;
			goto out;
		}
		rv = extract_messageType_params_from_json_first_level(*out_json_tree_root, out_msg);
		getnstimeofday_boot(&kafka_last_consume_timespec);
		rv = 0;
		k->consumer_offset = new_offset;	// Decision: Update offset only if message is well formatetd. Can change it. Decided by Ronen: Change-Id: I37dad733615fdacd58d144245d306f78d2133eb7
	} else {
		rv = 1;
		check_if_kafka_init_preserve_state_vars_required(k_msg->err);
	}
out:
	if (k_msg)
		rd_kafka_message_destroy(k_msg);	// Done with this message
	return rv;
}

static int parse_name_and_uuid(struct mm_json_elem *root, struct name_and_uuid_params_ctx *name_and_uuid_params)
{
	int						i, j;
	struct mm_json_kv_pair	*root_kv;
	struct mm_json_kv_pair	*payload_kv;
	unsigned int			parsed_mask = 0;
	int						rv;

	NFIN;
	for (i = 0; i < root->dict.len; i++) {
		root_kv = &root->dict.elements[i];
		if (!strcmp(root_kv->key, "payload")) {
			N_Tf(4cs64ha, "parsing payload");
			for (j = 0; j < root_kv->value->dict.len; j++) {
				payload_kv = &root_kv->value->dict.elements[j];
				if 			(!strcmp(payload_kv->key, "nodeID")) {
					parsed_mask |= 0x1;
					nvmeibt_strlcpy(name_and_uuid_params->hostname, payload_kv->value->str, sizeof(name_and_uuid_params->hostname));
				} else if	(!strcmp(payload_kv->key, "uuid")) {
					parsed_mask |= 0x2;
					nvmeibt_urn_uuid_str_to_union_uuid(&(name_and_uuid_params->uuid), payload_kv->value->str);
				} else if	(!strcmp(payload_kv->key, "targetsInZone")) {
					parsed_mask |= 0x4;
					name_and_uuid_params->n_members_total_before_add_del = payload_kv->value->num;
				} else if	(!strcmp(payload_kv->key, "targetUpdatesSequence")) {
					parsed_mask |= 0x8;
					name_and_uuid_params->targets_updates_sequence = payload_kv->value->num;
				} else {
					if (payload_kv->value->type == JSON_E_STR) {
						N_Ef(0an3hja, "Unexpected @STR=@STR", payload_kv->key, payload_kv->value->str);
					} else {
						N_Ef(7vcbkje, "Unexpected @STR=@INT64_TD", payload_kv->key, payload_kv->value->num);
					}
				}
			}
			break;
		}
	}
	// N_Tf(rbzi3l2, "hostname=@STR uuid=@UUID_LE n_members_total_before_add_del=@INT targets_updates_sequence=@LLD", name_and_uuid_params->hostname, &(name_and_uuid_params->uuid), name_and_uuid_params->n_members_total_before_add_del, name_and_uuid_params->targets_updates_sequence);
	NFOUT;
	rv = (parsed_mask == 0xf ? 0 : -1);
	if (rv < 0) {
		N_Ef(vb6kiem, "Failed to find the exact fields");
	}
	return rv;
}

/******************************************************************************/
/*********************             CMD_consumer           *********************/
/******************************************************************************/
atomic_t			CMD_consumer_n_msgs_awaiting_toma_processing;		// Ronen Hod: This is a simple criteria. Commit is not mandatory or urgent. It is used only on the next restart, and it is an optimization.
struct keepAliveToken_params_ctx {
	char			nodeID[64];
	int64_t			zone_number;
	int64_t			token;
	uint64_t		keepaliveInterval;
};

static int parse_updateTomaKeepaliveToken(struct mm_json_elem *root, struct keepAliveToken_params_ctx *out_keepAliveToken_params, bool is_updateTomaKeepaliveToken_msg)
{
	int						i, j;
	struct mm_json_kv_pair	*root_kv;
	struct mm_json_kv_pair	*payload_kv;
	unsigned int			parsed_mask = 0;
	int						rv;

	NFIN;
	for (i = 0; i < root->dict.len; i++) {
		root_kv = &root->dict.elements[i];
		if (!strcmp(root_kv->key, "payload")) {
			N_Tf(8x03498, "parsing payload");
			for (j = 0; j < root_kv->value->dict.len; j++) {
				payload_kv = &root_kv->value->dict.elements[j];
				if 			(!strcmp(payload_kv->key, "nodeID")) {
					parsed_mask |= 0x1;
					nvmeibt_strlcpy(out_keepAliveToken_params->nodeID, payload_kv->value->str, sizeof(out_keepAliveToken_params->nodeID));
				} else if	(!strcmp(payload_kv->key, "zone")) {
					parsed_mask |= 0x2;
					out_keepAliveToken_params->zone_number = atoll(payload_kv->value->str);
				} else if	(!strcmp(payload_kv->key, "token")) {
					parsed_mask |= 0x4;
					out_keepAliveToken_params->token = payload_kv->value->num;
				} else if	(!strcmp(payload_kv->key, "keepaliveInterval")) {
					parsed_mask |= 0x8;
					out_keepAliveToken_params->keepaliveInterval = payload_kv->value->num;
				} else {
					if (payload_kv->value->type == JSON_E_STR) {
						N_Ef(cvmau3j, "Unexpected @STR=@STR", payload_kv->key, payload_kv->value->str);
					} else {
						N_Ef(362has7, "Unexpected @STR=@INT64_TD", payload_kv->key, payload_kv->value->num);
					}
				}
			}
			break;
		}
	}
	rv = (is_updateTomaKeepaliveToken_msg ? (parsed_mask == 0xF ? 0 : -1) : (parsed_mask == 0xC ? 0 : -1));
	if (rv < 0) {
		N_Ef(jsuwmna, "Failed to find the exact fields");
	}
	NFOUT;
	return rv;
}

struct resend_report_disk_ctx {
	char			ldiskID[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];
	unsigned int	vendor;
	int				reappearingCounter;
	bool			reappearingOutOfSync;
};

struct send_praid_report_ctx {
	char			praid_uuid[40];
	int				lastKnownVersion_major;
	int				lastKnownVersion_minor;
	uint64_t		lastKnownVersion_raft_term;
};

struct generic_CMD_params_ctx {
	char							generic_uuid[40];	// E.g., disk_obj_guid
	struct nvmeibt_ascii_uuid		ldisk_id;
	char							formatType[32];
	union nvmeib_uuid				volumeUUID;
	uint64_t						reservationVersion;
	int64_t							bootTime;
	unsigned int					vendor;
	int								tomaToken;
	int								formatRequestCounter;
	int								blockSize;
	int								metadataSize;
	struct nvmeibt_urn_uuid			dbUUID;
	struct resend_report_disk_ctx	disks_to_report[NVMEIBT_MAX_N_DISKS_PER_NODE];
	int								n_disks_to_report;
	struct send_praid_report_ctx	praids_to_report[NVMEIBT_MAX_N_PRAIDS];
	int								n_praids_to_report;
	int								encryptionCommandIndex;
	int								slot;
	int								keySize;
	char							passphrase[PASSPHRASE_MAX_LEN];
	char							newPassphrase[PASSPHRASE_MAX_LEN];
	struct nvmeibt_ascii_uuid		native_serial;
	int								nsid;
	char							native_nguid[32];
};

static int parse_CMD(struct mm_json_elem *root, struct generic_CMD_params_ctx *CMD_params) {
	// Somewhat slopy. Parse all the commands parameters at once
	int						i, j, k, l;
	struct mm_json_kv_pair	*root_kv;
	struct mm_json_kv_pair	*payload_kv;
	struct mm_json_kv_pair	*kv;
	struct mm_json_elem		*arr;
	struct mm_json_dict		*drive_json_dict, *praid_json_dict;
	int						rv = 0;

	NFIN;
	CMD_params->n_disks_to_report = 0;
	CMD_params->n_praids_to_report = 0;
	for (i = 0; i < root->dict.len; i++) {
		root_kv = &root->dict.elements[i];
		if (!strcmp(root_kv->key, "payload")) {
			N_Tf(657sniw, "parsing payload");
			for (j = 0; j < root_kv->value->dict.len; j++) {
				payload_kv = &(root_kv->value->dict.elements[j]);
				if (!strcmp(payload_kv->key, "drives")) {
					arr = payload_kv->value;
					if (arr->type != JSON_E_ARRAY) {
						N_Ef(bi3jsia, "@STR is supposed to be array", payload_kv->key);
						rv = -1;
						continue;
					}
					if (arr->array.len >= NVMEIBT_MAX_N_DISKS_PER_NODE) {
						N_Wf(f67fbhw, "@STR arr.len=@INT", payload_kv->key, arr->array.len);
					}
					CMD_params->n_disks_to_report = arr->array.len;
					for (k = 0; k < arr->array.len; k++) {
						drive_json_dict = &(arr->array.elements[k]->dict);
						for (l = 0; l < drive_json_dict->len; l++) {
							kv = &(drive_json_dict->elements[l]);
							if	(!strcmp(kv->key, "diskID")) {
								nvmeibt_strlcpy(CMD_params->disks_to_report[k].ldiskID, kv->value->str, sizeof(CMD_params->disks_to_report[k].ldiskID));
							} else if	(!strcmp(kv->key, "vendor")) {
								CMD_params->disks_to_report[k].vendor = kv->value->num;	// Such as 0x144d
							} else if	(!strcmp(kv->key, "reappearingCounter")) {
								CMD_params->disks_to_report[k].reappearingCounter = kv->value->num;
							} else if	(!strcmp(kv->key, "reappearingOutOfSync")) {
								CMD_params->disks_to_report[k].reappearingOutOfSync = kv->value->num;
							} else {
								if (kv->value->type == JSON_E_STR) {
									N_Ef(cbheujw, "Unexpected @STR=@STR", kv->key, kv->value->str);
								} else {
									N_Ef(xvhajk2, "Unexpected @STR=@INT64_TD", kv->key, kv->value->num);
								}
							}
						}
						N_Tf(rvchs8k,
							 "diskID=@STR vendor=@INT reappearingCounter=@INT reappearingOutOfSync=@BOOL",
							 CMD_params->disks_to_report[k].ldiskID, CMD_params->disks_to_report[k].vendor, CMD_params->disks_to_report[k].reappearingCounter, CMD_params->disks_to_report[k].reappearingOutOfSync);
					}
				} else if (!strcmp(payload_kv->key, "pRaids")) {
					arr = payload_kv->value;
					if (arr->type != JSON_E_ARRAY) {
						N_Ef(4vhdj56, "@STR is supposed to be array", payload_kv->key);
						rv = -1;
						continue;
					}
					if (arr->array.len >= NVMEIBT_MAX_N_PRAIDS) {
						N_Wf(fnbekof, "@STR arr.len=@INT", payload_kv->key, arr->array.len);
					}
					for (k = 0; k < arr->array.len; k++) {
						praid_json_dict = &(arr->array.elements[k]->dict);
						for (l = 0; l < praid_json_dict->len; l++) {
							kv = &(praid_json_dict->elements[l]);
							if (!strcmp(kv->key, "uuid")) {
								nvmeibt_strlcpy(CMD_params->praids_to_report[k].praid_uuid, kv->value->str, sizeof(CMD_params->praids_to_report[k].praid_uuid));
							} else if (!strcmp(kv->key, "lastKnownVersion")) {
							   // "lastKnownVersion": "<major,minor,raftTerm>"
							   sscanf(kv->value->str, "<%d,%d,%lu>",
									  &(CMD_params->praids_to_report[k].lastKnownVersion_major), &(CMD_params->praids_to_report[k].lastKnownVersion_minor), &(CMD_params->praids_to_report[k].lastKnownVersion_raft_term));
							} else {
								if (kv->value->type == JSON_E_STR) {
									N_Ef(ctvsauj, "Unexpected @STR=@STR", kv->key, kv->value->str);
								} else {
									N_Ef(nai3stz, "Unexpected @STR=@INT64_TD", kv->key, kv->value->num);
								}
							}
						}
				   }
				} else if (!strcmp(payload_kv->key, "tomaToken")) {
					CMD_params->tomaToken = payload_kv->value->num;
				} else if (!strcmp(payload_kv->key, "diskID")) {
					nvmeibt_strlcpy(CMD_params->ldisk_id.str, payload_kv->value->str, sizeof(CMD_params->ldisk_id.str));
				} else if (!strcmp(payload_kv->key, "uuid")) {
					nvmeibt_strlcpy(CMD_params->generic_uuid, payload_kv->value->str, sizeof(CMD_params->generic_uuid));
				} else if (!strcmp(payload_kv->key, "vendor")) {
					CMD_params->vendor = payload_kv->value->num;	// Such as 0x144d
				} else if (!strcmp(payload_kv->key, "formatRequestCounter")) {
					CMD_params->formatRequestCounter = payload_kv->value->num;
				} else if (!strcmp(payload_kv->key, "blockSize")) {
					CMD_params->blockSize = payload_kv->value->num;
				} else if (!strcmp(payload_kv->key, "metadataSize")) {
					CMD_params->metadataSize = payload_kv->value->num;
				} else if (!strcmp(payload_kv->key, "dbUUID")) {
					nvmeibt_strlcpy(CMD_params->dbUUID.str, payload_kv->value->str, sizeof(CMD_params->dbUUID.str));
				} else if (!strcmp(payload_kv->key, "formatType")) {
					nvmeibt_strlcpy(CMD_params->formatType, payload_kv->value->str, sizeof(CMD_params->formatType));
				} else if (!strcmp(payload_kv->key, "volumeID")) {
					// Do nothing, we don't need this param
				} else if (!strcmp(payload_kv->key, "volumeName")) {
					// Do nothing, we don't need this param
				} else if (!strcmp(payload_kv->key, "volumeUUID")) {
					nvmeibt_urn_uuid_to_union_uuid(&CMD_params->volumeUUID,
												   (struct nvmeibt_urn_uuid *)(payload_kv->value->str));
				} else if (!strcmp(payload_kv->key, "reservationMode")) {
					// Do nothing, we don't need this param
				} else if (!strcmp(payload_kv->key, "reservationVersion")) {
					CMD_params->reservationVersion = payload_kv->value->num;
				} else if (!strcmp(payload_kv->key, "encryptionCommandIndex")) {
					CMD_params->encryptionCommandIndex = payload_kv->value->num;
				} else if (!strcmp(payload_kv->key, "slot")) {
					CMD_params->slot = payload_kv->value->num;
				} else if (!strcmp(payload_kv->key, "currentSlot")) {
					CMD_params->slot = payload_kv->value->num;
				} else if (!strcmp(payload_kv->key, "keySize")) {
					CMD_params->keySize = payload_kv->value->num;
				} else if (!strcmp(payload_kv->key, "passphrase")) {
					nvmeibt_strlcpy(CMD_params->passphrase, payload_kv->value->str, sizeof(CMD_params->passphrase));
				} else if (!strcmp(payload_kv->key, "currentPassphrase")) {
					nvmeibt_strlcpy(CMD_params->passphrase, payload_kv->value->str, sizeof(CMD_params->passphrase));
				} else if (!strcmp(payload_kv->key, "newPassphrase")) {
					nvmeibt_strlcpy(CMD_params->newPassphrase, payload_kv->value->str, sizeof(CMD_params->newPassphrase));
				} else if (!strcmp(payload_kv->key, "bootTime")) {
					CMD_params->bootTime = payload_kv->value->num;
				} else if (!strcmp(payload_kv->key, "serial")) {
					nvmeibt_strlcpy(CMD_params->native_serial.str, payload_kv->value->str, sizeof(CMD_params->native_serial.str));
				} else if (!strcmp(payload_kv->key, "nsid")) {
					CMD_params->nsid = payload_kv->value->num;
				} else if (!strcmp(payload_kv->key, "nguid")) {
					nvmeibt_strlcpy(CMD_params->native_nguid, payload_kv->value->str, sizeof(CMD_params->native_nguid));
				} else {
					if (payload_kv->value->type == JSON_E_STR) {
						N_Ef(ct326bd, "Unexpected @STR=@STR", payload_kv->key, payload_kv->value->str);
					} else {
						N_Ef(meiyzx5, "Unexpected @STR=@INT64_TD", payload_kv->key, payload_kv->value->num);
					}
				}
			}
			break;	// Do we need to break after parsing the payload? Probably meaningless
		}
	}
	N_Tf(4vsdywb,
		 LOCAL_DISK_LOG_FMT " vendor=@INT uuid=@STR tomaToken=@INT formatType=@STR formatRequestCounter=@INT blockSize=@INT metadataSize=@INT dbUUID=@STR",
		 LOCAL_DISK_LOG_obj_ARGS(CMD_params), CMD_params->vendor, CMD_params->generic_uuid, CMD_params->tomaToken, CMD_params->formatType, CMD_params->formatRequestCounter, CMD_params->blockSize,
		 CMD_params->metadataSize, CMD_params->dbUUID.str);
	NFOUT;
	return rv;
}
// Every TOMA has a private commands queue for incoming cmds from the MGMT
static int CMD_consumer_init(bool is_full_init) {
	char group_id_str[HOST_NAME_MAX + 1 + 10];
	const struct key_val_strs k_conf_kv[] = {
		K_DEFAULT_CONSUMER_CONFIG, {"client.id", "" /* Overriden with machine name */ }
	};
	rd_kafka_conf_t *k_conf;
	struct t_consumer_impl *k = &k_CMD;
	int rv = -1;
	NFIN;
	if (is_full_init) {
		k->offset_committed = k->consumer_offset = RD_KAFKA_OFFSET_INVALID;
		atomic_set(&CMD_consumer_n_msgs_awaiting_toma_processing, 0);
	}
	snprintf(group_id_str,  sizeof(group_id_str),  "CMD_%s", nvmeibt_get_my_hostname());			// Dont change it! Mgmt relies on it to remove old produced messages
	snprintf(k->topic_name, sizeof(k->topic_name), "%s.TOMA.commands.1.0.0", nvmeibt_get_my_hostname());
	k_conf = alloc_and_init_kafka_conf(k_conf_kv, ARRAY_SIZE(k_conf_kv), group_id_str, NULL);
	k->consumer = __create_kafka_new_obj(RD_KAFKA_CONSUMER, &k_conf, k->topic_name, NULL);
	if (k->consumer) {	// Local node, so continue from prev message
		consumer_start_from_last_committed_offset("CMD", k);
		rv = 0;
	}
	NFOUT;
	return rv;
}

static void mark_CMD_k_msg_for_kafka_commit(int64_t kafka_offset, bool is_called_by_toma) {
	N_Tf(vbdsk30, "Done k_offset=@LD", purify_offset(kafka_offset));
	if (is_called_by_toma) {
		atomic_add(-1, &CMD_consumer_n_msgs_awaiting_toma_processing);
	}
}

void nvmeibt_kafka_mark_CMD_k_msg_for_kafka_commit_by_toma(int64_t kafka_offset) {
	mark_CMD_k_msg_for_kafka_commit(kafka_offset, 1);
}

static void __wakeup_toma_params_free(struct kafka_wakeup_params *wap) {
	switch (wap->event_type) {
	case KAFKA_EVENT_TYPE_HW_FULL_CONFIG:
		HW_conf_free_tree((struct HW_mgmt_conf*)wap->event_data);
		break;
	case KAFKA_EVENT_TYPE_VOL_ADD:
	case KAFKA_EVENT_TYPE_VOL_DEL:
	case KAFKA_EVENT_TYPE_VOL_DEL_COMPLETED:
	case KAFKA_EVENT_TYPE_VOL_UPD:
		mm_conf_free_tree((struct mm_mgmt_conf *)wap->event_data);
		break;
	case KAFKA_EVENT_TYPE_TARGET_ADD:
	case KAFKA_EVENT_TYPE_TARGET_DEL:
	case KAFKA_EVENT_TYPE_CMD:
		NNVMEIBT_BM_FREE(tbsi84l, wap->event_data);
		break;
	default:
		N_Ef(vsh398a, "*******************************   FIX ME   ****************************** conf=@PTR event_type=@INT", wap, wap->event_type);
		break;
	}
	NNVMEIBT_BM_FREE(2kzx0oe, wap);
}

static void __wakeup_toma_main_tread(struct kafka_wakeup_params *wap) {
	const int wakeup_rv = nvmeibt_toma_trigger_wakeup(NVMEIBT_TOMA_WAKEUP_TYPE_KAFKA, wap);
	if (wakeup_rv < 0)
		__wakeup_toma_params_free(wap);	// Just free the memory, toma main thread cannot wakeup
}

static int CMD_consume(void) {
	struct messageType_params_ctx		messageType_params;
	struct mm_json_elem 				*json_tree_root = NULL;
	int									rv;		// -1: err, 0:consumed something, 1:OK_skipped
	struct generic_CMD_params_ctx		*CMD_params;
	bool								commit_it_now = 0;

	if (!k_CMD.consumer) {
		N_Tf(y788u22, "Not initialized");
		return 1;
	}
	if (atomic_read(&CMD_consumer_n_msgs_awaiting_toma_processing) > 0) {
		// In order to have 100% control of the offset of the consumed CMDs, we run one at a time
		N_Tf(kd94md6, "Skipping is_CMD_processing_and_blocking_other_CMDs");
		return 2000;
	}
	rv = consumer_read_msg_from_kafka(&k_CMD, &messageType_params, &json_tree_root);
	if (rv != 0) {
		if (rv != 1)				// 1 is default if no message arrived, dont clutter logs
			N_Tf(y7k1u22, "rv=@INT", rv);
		goto out;
	}
	if (strcmp(messageType_params.messageType, "updateTomaKeepaliveToken") == 0) {
		struct keepAliveToken_params_ctx kap; // The token-update messages are internal to toma_kafka. No need for wakeup
		commit_it_now = 1;
		rv = parse_updateTomaKeepaliveToken(json_tree_root, &kap, 1);
		if (strcmp(kap.nodeID, nvmeibt_get_my_hostname()) != 0) {
			N_Ef(5a82nas, "OOOOPS, this msg nodeID='@STR' != @STR", kap.nodeID, nvmeibt_get_my_hostname());
			rv = -1;
			goto out;
		}
		nvmeibt_kafka_new_kafka_mgmt_zone_number_received(kap.zone_number);
		kafka_set_follower_keepalive_token_provided_by_mgmt(kap.token, kap.keepaliveInterval);
		rv = 0;
		goto out;
	}
	// All other CMDs, are handled by TOMA's main thread from wakeup. They receive the parsed json tree
	CMD_params = NNVMEIBT_BM_CALLOC(uzxhn2k, sizeof(*CMD_params));
	parse_CMD(json_tree_root, CMD_params);
	if (CMD_params->tomaToken && (CMD_params->tomaToken < nvmeibt_kafka_get_follower_keepalive_token_provided_by_mgmt())) {
		N_Tf(koo0o09, "old msg received (token @INT<@INT), skipping", CMD_params->tomaToken, nvmeibt_kafka_get_follower_keepalive_token_provided_by_mgmt());
		commit_it_now = 1;
		rv = 0;
		NNVMEIBT_BM_FREE(uzxhn2k1, CMD_params);
	} else {
		struct kafka_wakeup_params *wap = NNVMEIBT_BM_CALLOC(sueklwl, sizeof(*wap));
		wap->messageType_params = messageType_params;
		wap->event_type = KAFKA_EVENT_TYPE_CMD;
		wap->event_data = CMD_params;
		wap->kafka_offset = k_CMD.consumer_offset;
		atomic_add(1, &CMD_consumer_n_msgs_awaiting_toma_processing);
		__wakeup_toma_main_tread(wap);
	}
out:
	if (commit_it_now)
		mark_CMD_k_msg_for_kafka_commit(k_CMD.consumer_offset, 0);	// Progress the offset. Avoid re-reading already processed messages
	nvmeibt_mm_json_free_kv_tree(json_tree_root);      // No other consumers
	return rv;
}

/******************************************************************************/
/*********************       HW_full_config_consumer      *********************/
/******************************************************************************/
// All the TOMAs consume from the same queue.
// We do not really care that this is a queue, and we only consume the last message (config)
static int64_t		HW_full_config_consumer_highest_version_of_msg_received_to_date = RD_KAFKA_OFFSET_INVALID;
static int64_t		HW_full_config_consumer_offset_of_highest_version_of_msg_received_to_date = RD_KAFKA_OFFSET_INVALID;
static int64_t		HW_full_config_consumer_offset_submitted_to_toma = RD_KAFKA_OFFSET_INVALID;
static int64_t		HW_full_config_consumer_offset_committed_by_toma = RD_KAFKA_OFFSET_INVALID;

static int HW_full_config_consumer_init(bool is_full_init) {
	const char topic_str_base[] = ".TOMA.hardwareConfiguration.1.0.0";
	char group_id_str[HOST_NAME_MAX + 1 + 20];			// Every TOMA makes its own commits
	const struct key_val_strs k_conf_kv[] = {
		K_DEFAULT_CONSUMER_CONFIG, {"client.id", "HW_config_consumer"},	// All machines use the same client, each has its own group, so there is no load balancing
	};
	rd_kafka_conf_t	*k_conf;
	struct t_consumer_impl *k = &k_HW_full_config;
	int							rv = -1;
	NFIN;
	if (!nvmeibt_kafka_is_mgmt_zone_specified()) {
		N_Tf(6wvj5ks, "No zone yet");
		rv = 1;
		goto out;
	}

	if (is_full_init) {
		k->consumer_offset = RD_KAFKA_OFFSET_INVALID;
		HW_full_config_consumer_offset_submitted_to_toma = RD_KAFKA_OFFSET_INVALID;
		HW_full_config_consumer_offset_committed_by_toma = RD_KAFKA_OFFSET_INVALID;
	}
	snprintf(group_id_str, sizeof(group_id_str), "HW_%s", nvmeibt_get_my_hostname());		// 1 queue for all Toma's but each machine in its own group_id. From each group.id only 1 consumer can read. Dont change it! Mgmt relies on it to remove old produced messages!
	generate_topic_name_using_zone(k->topic_name, sizeof(k->topic_name), topic_str_base, 0);
	k_conf = alloc_and_init_kafka_conf(k_conf_kv, ARRAY_SIZE(k_conf_kv), group_id_str, NULL);
	k->consumer = __create_kafka_new_obj(RD_KAFKA_CONSUMER, &k_conf, k->topic_name, NULL);
	if (k->consumer) {	// Local node, so continue from prev message
		consumer_start_from_last_committed_offset("HW_config", k);
		rv = 0;
	}
out:
	NFOUT;
	return rv;
}

static void mark_HW_full_config_k_msg_for_kafka_commit(int64_t kafka_offset, bool is_called_by_toma, bool is_this_offset_a_good_starting_point_after_the_next_boot)
{
	N_Tf(7vsso4l, "Done k_offset=@LD", purify_offset(kafka_offset));
	if (is_called_by_toma) {
		if (is_this_offset_a_good_starting_point_after_the_next_boot) {
			HW_full_config_consumer_offset_committed_by_toma = max(HW_full_config_consumer_offset_committed_by_toma, kafka_offset);
		} else {
			N_Wf(3178bsm, "k_offset=@LD was ignored. Hopefully recoverable", purify_offset(kafka_offset));
			HW_full_config_consumer_offset_submitted_to_toma = HW_full_config_consumer_offset_committed_by_toma;	// release HW_full_config_consume()
		}
	}
}

static int HW_full_config_consume(void) {
	struct messageType_params_ctx		highest_version_messageType_params;
	struct mm_json_elem 				*json_tree_root = NULL;
	struct HW_mgmt_conf					*highest_HW_mgmt_conf = NULL;
	int									rv;		// -1: err, 0:consumed something, 1:OK_skipped

	if (!k_HW_full_config.consumer) {
		N_Tf(y788u33, "Not initialized");
		return 1;
	}
	if (purify_offset(HW_full_config_consumer_offset_committed_by_toma) < purify_offset(HW_full_config_consumer_offset_submitted_to_toma)) {
		// In order to have 100% control of the offset of the consumed HW_full_configs, we run one at a time
		N_Tf(tvs84kw, "Skipping, offset_committed_by_toma=@LD < offset_submitted_to_toma=@LD", purify_offset(HW_full_config_consumer_offset_committed_by_toma), purify_offset(HW_full_config_consumer_offset_submitted_to_toma));
		return 1;
	}
	// We are only interested in the last (highest) configuration. Due to reordering (multi-mgmt) it may not have the highest kafka offset, so we need to read the entire queue, use latest and reset the offset to the last msg in the queue
	while (1) {
		struct messageType_params_ctx messageType_params;
		struct HW_mgmt_conf *conf;
		// Prepare for the next iteration
		nvmeibt_mm_json_free_kv_tree(json_tree_root);
		json_tree_root = NULL;

		rv = consumer_read_msg_from_kafka(&k_HW_full_config, &messageType_params, &json_tree_root);
		if (rv == 1) { // the queue is empty or cannot be read now
			break;
		}
		if (rv < 0) { // bad message
			continue;
		}
		N_Tf(6wphucs, "msg received");
		conf = NNVMEIBT_BM_CALLOC(4vs6k9s,  sizeof(*conf));	//	Fully parse it (in order to get payload->configurationVersion)
		nvmeibt_mm_json_tree_to_HW_mgmt_conf(conf, json_tree_root, HW_full_config_consumer_offset_of_highest_version_of_msg_received_to_date);	// Do as much processing as possible before TOMA's main thread

		N_Tf(iqhfy6s, "Received configurationVersion=@INT64_TX", conf->configurationVersion);
		if (conf->configurationVersion < HW_full_config_consumer_highest_version_of_msg_received_to_date) {
			N_Tf(6sbk3l5, "Received older H/W config version=@INT64_TX<@INT64_TX, skipping", conf->configurationVersion, HW_full_config_consumer_highest_version_of_msg_received_to_date);
			HW_conf_free_tree(conf);
			continue;
		}
		// Adopt the new highest ever
		HW_conf_free_tree(highest_HW_mgmt_conf);
		highest_HW_mgmt_conf = conf;
		HW_full_config_consumer_highest_version_of_msg_received_to_date = highest_HW_mgmt_conf->configurationVersion;
		HW_full_config_consumer_offset_of_highest_version_of_msg_received_to_date = k_HW_full_config.consumer_offset;
		highest_version_messageType_params = messageType_params;
	}	// while()
	if (highest_HW_mgmt_conf) {	// If we received a higher than ever before
		struct kafka_wakeup_params *wakeup_params = NNVMEIBT_BM_CALLOC(djaioqk, sizeof(*wakeup_params));
		wakeup_params->messageType_params = highest_version_messageType_params;
		wakeup_params->event_type = KAFKA_EVENT_TYPE_HW_FULL_CONFIG;
		wakeup_params->event_data = highest_HW_mgmt_conf;
		wakeup_params->kafka_offset = HW_full_config_consumer_offset_of_highest_version_of_msg_received_to_date;
		HW_full_config_consumer_offset_submitted_to_toma = HW_full_config_consumer_offset_of_highest_version_of_msg_received_to_date;
		__wakeup_toma_main_tread(wakeup_params);
	}
	nvmeibt_mm_json_free_kv_tree(json_tree_root);
	return rv;
}

/******************************************************************************/
/***  incremental_VOL_updates_consumer (add/del VOLUME & updateLeaderKeepaliveToken)  ***/
/******************************************************************************/
static int incremental_VOL_updates_consumer_init(bool is_full_init) {
	const char topic_str_base[] = ".leader.incrementalUpdates.1.0.0";
	char group_id_str[32];	// All leaders commit/store/consume using the same group_id
	const struct key_val_strs k_conf_kv[] = {
		K_DEFAULT_CONSUMER_CONFIG, {"client.id", "" /* Overriden with machine name */ }
	};
	rd_kafka_conf_t	*k_conf;
	struct t_consumer_impl *k = &k_incremental_VOL_updates;
	int							rv = -1;
	NFIN;
	if (!nvmeibt_kafka_is_mgmt_zone_specified()) {
		rv = 1;
		goto out;
	}
	if (is_full_init) {
		k->consumer_offset = RD_KAFKA_OFFSET_INVALID;
	} else if (is_RD_KAFKA_OFFSET_VALID(k->consumer_offset)) {
		// Reinit requested_incremental_VOL_updates_consumer_offset only if we succeeded to read a message from the topic
		// If we didn't even start, use the initial value
		requested_incremental_VOL_updates_consumer_offset = k->consumer_offset + 1;
	}
	snprintf(group_id_str, sizeof(group_id_str), "LEADER_%ld", kafka_mgmt_zone_number);		// Dont change it! Mgmt relies on it to remove old produced messages
	generate_topic_name_using_zone(k->topic_name, sizeof(k->topic_name), topic_str_base, 0);
	k_conf = alloc_and_init_kafka_conf(k_conf_kv, ARRAY_SIZE(k_conf_kv), group_id_str, NULL);
	k->consumer = __create_kafka_new_obj(RD_KAFKA_CONSUMER, &k_conf, k->topic_name, NULL);
	if (k->consumer)		// Leader node (could change), so come up from leader persistency, not latest queue msg
		rv = 0;
out:
	NFOUT;
	return rv;
}

static int incremental_VOL_updates_consume(void) {
	struct messageType_params_ctx		msg_param;
	struct mm_json_elem 				*json_tree_root = NULL;
	int									rv;		// -1: err, 0:consumed something, 1:OK_skipped
	bool								is_delVolCompleted = false, is_new_or_updateVol = false;
	enum KAFKA_EVENT_TYPE				k_event = KAFKA_EVENT_TYPE_UNKNOWN;

	if (!is_consuming_leader_VOL_msgs()) {
		N_Tf(hasume5, "Not is_consuming_leader_VOL_msgs, skipping");
		return 0;
	}
	NTOMA_ASSERT(rvar6oa, k_incremental_VOL_updates.consumer, "No incremental_VOL_updates_consumer");
	rv = consumer_read_msg_from_kafka(&k_incremental_VOL_updates, &msg_param, &json_tree_root);
	if (rv != 0) {
		if (rv != 1)
			N_Tf(juu889k, "rv=@INT", rv);		// 1 is default if no message arrived, dont clutter logs
		goto out;
	}
	if (strcmp(msg_param.messageType, "addVolume"            ) == 0) { k_event = KAFKA_EVENT_TYPE_VOL_ADD;           is_new_or_updateVol = 1; }
	if (strcmp(msg_param.messageType, "deleteVolume"         ) == 0) { k_event = KAFKA_EVENT_TYPE_VOL_DEL; }
	if (strcmp(msg_param.messageType, "deleteVolumeCompleted") == 0) { k_event = KAFKA_EVENT_TYPE_VOL_DEL_COMPLETED; is_delVolCompleted =  1; }
	if (strcmp(msg_param.messageType, "updateVolume"         ) == 0) { k_event = KAFKA_EVENT_TYPE_VOL_UPD;           is_new_or_updateVol = 1; }
	if (strcmp(msg_param.messageType, "updateLeaderKeepaliveToken") == 0) {
		struct keepAliveToken_params_ctx keepAliveToken_params;			// The token-update messages are internal to toma_kafka. No need for wakeup
		rv = parse_updateTomaKeepaliveToken(json_tree_root, &keepAliveToken_params, 0);
		kafka_set_leader_keepalive_token_provided_by_mgmt(keepAliveToken_params.token, keepAliveToken_params.keepaliveInterval);
		rv = 0;
	} else if (k_event != KAFKA_EVENT_TYPE_UNKNOWN) {
		struct mm_mgmt_conf *mgmt_conf = NNVMEIBT_BM_CALLOC(rygaj4l,  sizeof(*mgmt_conf));					// Parse them just the same, although deleteVolume has just two fields
		struct kafka_wakeup_params *wakeup_params = NNVMEIBT_BM_CALLOC(5vsyc8e, sizeof(*wakeup_params));
		rv = nvmeibt_mgmt_msg_json_tree_to_mgmt_conf(mgmt_conf, json_tree_root, k_incremental_VOL_updates.consumer_offset, is_new_or_updateVol, is_delVolCompleted);	// Do as much processing as possible before TOMA's main thread
		wakeup_params->messageType_params = msg_param;
		wakeup_params->event_type = k_event;
		wakeup_params->event_data = (void *)mgmt_conf;
		wakeup_params->kafka_offset = k_incremental_VOL_updates.consumer_offset;
		wakeup_params->kafka_raft_term_when_started_consuming_leader_msgs = kafka_applied_consuming_leader_VOL_msgs_raft_term;
		__wakeup_toma_main_tread(wakeup_params);
	} else {
		N_Ef(ajk348z, "Unexpected messageType=@STR", msg_param.messageType);
		rv = -1;
	}
out:
	nvmeibt_mm_json_free_kv_tree(json_tree_root);
	return rv;
}

/******************************************************************************/
/********    incremental_TARGET_updates_consumer  (add/del TARGET)   **********/
/******************************************************************************/
XDLIST_DECLARE(, struct kafka_wakeup_params, kafka_raft_members_sorted_msgs_queue_link)			kafka_raft_members_sorted_msgs_queue = XDLIST_INIT(kafka_raft_members_sorted_msgs_queue);
#define FREE_RAFT_MEMBERS_WAKEUP_PARAMS(name, wakeup_p)			\
		NNVMEIBT_BM_FREE(name ## _1, wakeup_p->event_data);		\
		NNVMEIBT_BM_FREE(name ## _2, wakeup_p)

static int64_t		last_sent_to_toma_targets_updates_seq_no = -1;
void nvmeibt_kafka_set_last_sent_to_toma_targets_updates_seq_no(int64_t seq_no)
{
	N_Tf(rcdbw3t, "set_last_sent_to_toma_targets_updates_seq_no @INT64_TD-->@INT64_TD", last_sent_to_toma_targets_updates_seq_no, seq_no);
	last_sent_to_toma_targets_updates_seq_no = seq_no;
}

static void kafka_raft_members_sorted_msgs_queue_init(void) {
	struct kafka_wakeup_params	*wakeup_params;
	// Clean old leftovers
	XDLIST_FOREACH_SAFE(wakeup_params, &kafka_raft_members_sorted_msgs_queue) {
		XDLIST_DEL(&(wakeup_params->kafka_raft_members_sorted_msgs_queue_link));
		FREE_RAFT_MEMBERS_WAKEUP_PARAMS(xcvsjwl, wakeup_params);
	}
}

static void kafka_raft_members_sorted_msgs_queue_send_all_sequential_to_toma(void)
{
	struct kafka_wakeup_params	*wakeup_params;
	XDLIST_FOREACH_SAFE(wakeup_params, &kafka_raft_members_sorted_msgs_queue) {
		const int64_t expected_seq_no = is_offset_zero(wakeup_params->kafka_offset) ? ((int64_t)1 /*MGMT start*/) : (last_sent_to_toma_targets_updates_seq_no + 1);
		if (wakeup_params->seq_no != expected_seq_no) {
			N_Wf(hwuscri, "First sequence_no=@INT64_TD last_accepted_targets_updates_sequence=@INT64_TD", wakeup_params->seq_no, last_sent_to_toma_targets_updates_seq_no);
			break;	// The next sequential msg is missing. Try again later
		}
		// Accept it
		XDLIST_DEL(&(wakeup_params->kafka_raft_members_sorted_msgs_queue_link));
		nvmeibt_kafka_set_last_sent_to_toma_targets_updates_seq_no(wakeup_params->seq_no);
		__wakeup_toma_main_tread(wakeup_params);
	}
	if (!XDLIST_EMPTY(&kafka_raft_members_sorted_msgs_queue)) {
		N_Wf(rcaj1kn, "Queue not empty after processing");
	}
}

static bool is_raft_members_wakeup_params_OK(struct kafka_wakeup_params *wakeup_params, int seq_no_for_comparison, bool is_LessEqual) {
	if (is_LessEqual ? (wakeup_params->seq_no <= seq_no_for_comparison) : (wakeup_params->seq_no == seq_no_for_comparison)) {
		N_Wf(ak3nxyp, "Ignoring duplicate sequence_no=@INT64_TD (k_offset=@LD)", wakeup_params->seq_no, purify_offset(wakeup_params->kafka_offset));
		FREE_RAFT_MEMBERS_WAKEUP_PARAMS(vtsie4m, wakeup_params);
		return false;
	}
	return true;
}

static bool kafka_raft_members_sorted_msgs_queue_add(struct kafka_wakeup_params *wakeup_params)
{
	struct kafka_wakeup_params			*queue_entry;
	bool								is_added_to_TOMA_wakeup_queue = 1;	// We already sent such seq_no to TOMA

	if (!is_raft_members_wakeup_params_OK(wakeup_params, last_sent_to_toma_targets_updates_seq_no, 1)) {
		is_added_to_TOMA_wakeup_queue = 0;
		goto out;
	}
	// Add (sorted)
	XDLIST_FOREACH_SAFE(queue_entry, &kafka_raft_members_sorted_msgs_queue) {
		if (!is_raft_members_wakeup_params_OK(wakeup_params, queue_entry->seq_no, 0)) {
			is_added_to_TOMA_wakeup_queue = 0;
			goto out;
		}
		if (queue_entry->seq_no > wakeup_params->seq_no) {
			// Once we reached a higher seq, put just before
			NXDLIST_ADD_CHECK(xc53olA, &kafka_raft_members_sorted_msgs_queue,
							  queue_entry->kafka_raft_members_sorted_msgs_queue_link.prev, &(queue_entry->kafka_raft_members_sorted_msgs_queue_link),
							  wakeup_params);
			goto out;
		}
	}
	// Might be that the queue was emptied (All were sent), or that the new seq_no is higher than any in the queue
	NXDLIST_ADD_TAIL_CHECK(bcyh6j2, &kafka_raft_members_sorted_msgs_queue, wakeup_params);
out:
	kafka_raft_members_sorted_msgs_queue_send_all_sequential_to_toma();
	return is_added_to_TOMA_wakeup_queue;
}

/******************************************************************************/
static int incremental_TARGET_updates_consumer_init(bool is_full_init) {
	const char topic_str_base[] = ".leader.incrementalTargetUpdates.1.0.0";
	char group_id_str[32];	// All leaders commit/store/consume using the same group_id
	const struct key_val_strs k_conf_kv[] = {
		K_DEFAULT_CONSUMER_CONFIG, {"client.id", "" /* Overriden with machine name */ }
	};
	rd_kafka_conf_t	*k_conf;
	struct t_consumer_impl *k = &k_incremental_TARGET_updates;
	int							rv = -1;
	NFIN;
	if (!nvmeibt_kafka_is_mgmt_zone_specified()) {
		rv = 1;
		goto out;
	}
	if (is_full_init) {
		last_sent_to_toma_targets_updates_seq_no = -1;
		k->consumer_offset = RD_KAFKA_OFFSET_INVALID;
	} else {
		if (is_RD_KAFKA_OFFSET_VALID(k->consumer_offset)) {
			// Reinit requested_incremental_VOL_updates_consumer_offset only if we succeeded to read a message from the topic
			// If we didn't even start, use the initial value
			requested_incremental_TARGET_updates_consumer_offset = k->consumer_offset + 1;
		}
		requested_incremental_TARGET_updates_consumer_seq_no = last_sent_to_toma_targets_updates_seq_no;
	}
	snprintf(group_id_str, sizeof(group_id_str), "LEADER_%ld", kafka_mgmt_zone_number);			// Dont change it! Mgmt relies on it to remove old produced messages
	generate_topic_name_using_zone(k->topic_name, sizeof(k->topic_name), topic_str_base, 0);
	k_conf = alloc_and_init_kafka_conf(k_conf_kv, ARRAY_SIZE(k_conf_kv), group_id_str, NULL);
	k->consumer = __create_kafka_new_obj(RD_KAFKA_CONSUMER, &k_conf, k->topic_name, NULL);
	if (k->consumer)	// Leader node (could change), so come up from leader persistency, not latest queue msg
		rv = 0;
out:
	NFOUT;
	return rv;
}

static int incremental_TARGET_updates_consume(void) {
	struct messageType_params_ctx		msg_param;
	struct mm_json_elem 				*json_tree_root = NULL;
	int									rv;		// -1: err, 0:consumed something, 1:OK_skipped
	enum KAFKA_EVENT_TYPE				k_event = KAFKA_EVENT_TYPE_UNKNOWN;

	if (!is_consuming_leader_TARGET_msgs()) {
		N_Tf(64bs83j, "Not is_consuming_leader_TARGET_msgs, skipping");
		return 0;
	}
	NTOMA_ASSERT(uincy31, k_incremental_TARGET_updates.consumer, "No incremental_TARGET_updates_consumer");
	if (IS_AWAITING_LEADER_KAFKA_OFFSET_BLOCKING_INCREMENTAL_TARGET_UPDATES(x9jso2s)) {
		rv = 2000;
		goto out;
	}
	//
	rv = consumer_read_msg_from_kafka(&k_incremental_TARGET_updates, &msg_param, &json_tree_root);
	if (rv != 0) {
		if (rv != 1)				// 1 is default if no message arrived, dont clutter logs
			N_Tf(gt66712, "rv=@INT", rv);
		goto out;
	}

	if (strcmp(msg_param.messageType, "addTarget"   ) == 0) { k_event = KAFKA_EVENT_TYPE_TARGET_ADD; }
	if (strcmp(msg_param.messageType, "deleteTarget") == 0) { k_event = KAFKA_EVENT_TYPE_TARGET_DEL; }
	if (k_event == KAFKA_EVENT_TYPE_UNKNOWN) {
		N_Ef(5xvvwj2, "Unexpected messageType=@STR", msg_param.messageType);
		rv = -1;
	} else {
		struct name_and_uuid_params_ctx *data = NNVMEIBT_BM_CALLOC(bxhs83j, sizeof(*data));
		struct kafka_wakeup_params *wakeup_params = NNVMEIBT_BM_CALLOC(c6shnse, sizeof(*wakeup_params));		// Only add/delete_target are done one-by-one, and need to wait for the leader to commit the change with the majority
		rv = parse_name_and_uuid(json_tree_root, data);
		wakeup_params->messageType_params = msg_param;
		wakeup_params->event_type = k_event;
		wakeup_params->event_data = (void *)data;
		wakeup_params->kafka_offset = k_incremental_TARGET_updates.consumer_offset;
		wakeup_params->kafka_raft_term_when_started_consuming_leader_msgs = kafka_applied_consuming_leader_TARGET_msgs_raft_term;
		wakeup_params->seq_no = data->targets_updates_sequence;
		XDLIST_INIT_LINK(&(wakeup_params->kafka_raft_members_sorted_msgs_queue_link), NULL);
		if (kafka_raft_members_sorted_msgs_queue_add(wakeup_params)) {
			NVMEIBT_KAFKA_SET_LEADER_KAFKA_OFFSET_BLOCKING_INCREMENTAL_TARGET_UPDATES(rxc30nw, k_incremental_TARGET_updates.consumer_offset);	// Stop processing further msgs
		}
	}
out:
	nvmeibt_mm_json_free_kv_tree(json_tree_root);
	return rv;
}

/******************************************************************************/
/******************************   Generic   ***********************************/
/******************************************************************************/
#define APPEND_TO_JSON_IF_NON_ZERO(json_payload, var_key, str_key) do {															\
	int n_running = NVMEIBT_GLOBAL_GET_N_TASKS_COUNTER(n_running_##var_key);													\
	int n_pending = NVMEIBT_GLOBAL_GET_N_TASKS_COUNTER(n_pending_##var_key);													\
	nvmeibt_Str_sprintf((json_payload), " \"nRunning" #str_key "\": %d, \"nPending" #str_key "\": %d,", n_running, n_pending);	\
} while(0)

static void rebuild_stats_to_json(struct nvmeibt_Str *json_payload)
{
	nvmeibt_Str_sprintf(json_payload, "\"rebuildStats\": { ");

	APPEND_TO_JSON_IF_NON_ZERO(json_payload, dirty_rebuild, DirtyRebuild);
	APPEND_TO_JSON_IF_NON_ZERO(json_payload, stale_rebuild, StaleRebuild);
	APPEND_TO_JSON_IF_NON_ZERO(json_payload, txid_rebuild, TxidRebuild);
	APPEND_TO_JSON_IF_NON_ZERO(json_payload, cold_recovery, ColdRecovery);
	APPEND_TO_JSON_IF_NON_ZERO(json_payload, JGC_rebuild, JGCRebuild);
	APPEND_TO_JSON_IF_NON_ZERO(json_payload, scrubbing, Scrubbing);

	nvmeibt_Str_chop_last_char(json_payload); // To remove the last comma; if no rebuilds exist, the last char is a space to be safely removed.
	nvmeibt_Str_sprintf(json_payload, "}"); // rebuildStats ends
}

void send_keepalive_msgs_as_needed(void)
{
	static struct timespec		last_follower_keepalive_ts = TIMESPEC_ZERO;
	static struct timespec		last_leader_keepalive_ts = TIMESPEC_ZERO;
	static struct timespec		now;
	static struct nvmeibt_Str	*json_payload = NULL;

	if (!json_payload) {
		json_payload = NNVMEIBT_STR_ALLOC(4vc7usk);
	}
	getnstimeofday_boot(&now);
	// Follower (node) keepalive
	if (now.tv_sec - last_follower_keepalive_ts.tv_sec > nvmeibt_follower_keep_alive_secs) {
		char unique_key[NVMEIBT_KAFKA_MAX_UNIQUE_KEY_LEN];
		snprintf(unique_key, sizeof(unique_key), "%s.TOMA.keepalive", nvmeibt_get_my_hostname());		// Exact key format agreed with Mgmt-Team. Dont touch!
		nvmeibt_Str_reuse(json_payload);
		nvmeibt_Str_sprintf(json_payload, "{" KAFKA_PRODUCER_MSG_HEADER_FMT
							"\"keepaliveInterval\": %lld, "
							"\"payload\": {\"zone\": \"%ld\", \"leaderUUID\": \"%s\", \"bootTime\": %lld, \"featureCompatibilityVersion\": \"%ld\", \"tomaSoftwareVersion\": \"%ld\", "
							"\"version\": \"%s\", \"buildNumber\": \"%s\", ",
							KAFKA_PRODUCER_MSG_HEADER_VAR("keepalive", 2),
							nvmeibt_follower_keep_alive_secs, kafka_mgmt_zone_number,
							nvmeibt_leader_host_name(), nvmeibt_global_get_startup_timestamp_msec(),
							TOMA_SW_COMPATIBILITY_VER >> 16, TOMA_SW_COMPATIBILITY_VER & 0xFFFF, BUILD_VERSION_FOR_MGMT, BUILD_NUMBER_FOR_MGMT);
		rebuild_stats_to_json(json_payload);
		nvmeibt_Str_sprintf(json_payload, "}}");
		N_Tf(jsghw7b, "Sending follower keep_alive to management, seconds from last update=@LLD msg=@STR", now.tv_sec - last_follower_keepalive_ts.tv_sec, nvmeibt_Str_str(json_payload));
		nvmeibt_kafka_outgoing_msgs_queue_add(unique_key, nvmeibt_Str_str(json_payload), nvmeibt_Str_strlen(json_payload) + 1, NVMEIBT_KAFKA_OUTGOING_MSGS_KEEPALIVE);
		last_follower_keepalive_ts = now;
	}
	// Leader keepalive
	if (nvmeibt_raft_is_leader_ever_committed_by_majority() && ((now.tv_sec - last_leader_keepalive_ts.tv_sec) > (long)nvmeibt_leader_keep_alive_secs) && nvmeibt_raft_is_leader()) {
		char unique_key[NVMEIBT_KAFKA_MAX_UNIQUE_KEY_LEN];
		snprintf(unique_key, sizeof(unique_key), "%s.TOMA.leaderKeepalive", nvmeibt_get_my_hostname());		// Exact key format agreed with Mgmt-Team. Dont touch!
		nvmeibt_Str_reuse(json_payload);
		nvmeibt_Str_sprintf(json_payload, "{" KAFKA_PRODUCER_MSG_HEADER_FMT_L
							"\"keepaliveInterval\": %lld, \"payload\": {\"raftTerm\": %lld, \"zone\": \"%ld\", \"featureCompatibilityVersion\": \"%ld\", \"tomaSoftwareVersion\": \"%ld\", \"version\": \"%s\", \"buildNumber\": \"%s\"}}",
							KAFKA_PRODUCER_MSG_HEADER_VAR_L("leaderKeepalive", 1),
							nvmeibt_leader_keep_alive_secs, nvmeibt_raft_get_current_term(),
							kafka_mgmt_zone_number, nvmeibt_raft_get_guaranteed_sw_ver() >> 16, nvmeibt_raft_get_guaranteed_sw_ver() & 0xFFFF, BUILD_VERSION_FOR_MGMT, BUILD_NUMBER_FOR_MGMT);
		N_Tf(fbdsiuh, "Sending leader keep_alive to management, seconds from last update=@LLD msg=@STR", now.tv_sec - last_follower_keepalive_ts.tv_sec, nvmeibt_Str_str(json_payload));
		nvmeibt_kafka_outgoing_msgs_queue_add(unique_key, nvmeibt_Str_str(json_payload), nvmeibt_Str_strlen(json_payload) + 1, NVMEIBT_KAFKA_OUTGOING_MSGS_KEEPALIVE);
		last_leader_keepalive_ts = now;
	}
}

void nvmeibt_kafka_req_stop_consuming_leader_VOL_msgs(void) {
	N_Tf(trvgh9x2, "");
	kafka_requested_consuming_leader_VOL_msgs_raft_term = 0;
}

void nvmeibt_kafka_req_stop_consuming_leader_TARGET_msgs(void) {
	N_Tf(trvgh9x1, "");
	kafka_requested_consuming_leader_TARGET_msgs_raft_term = 0;
}

void nvmeibt_kafka_req_start_consuming_leader_VOL_msgs(int64_t kafka_offset_VOL, unsigned long long raft_term)
{
	// TOMA request runs in the TOMA thread, and only marks for the kafka thread
	N_Tf(trvgh9x, "k_offset=@LD  raft_term=@LLX", purify_offset(kafka_offset_VOL), raft_term);
	pthread_mutex_lock(&(kafka_toma_requested_term_and_offset_mutex));
	requested_incremental_VOL_updates_consumer_offset = kafka_offset_VOL;
	kafka_requested_consuming_leader_VOL_msgs_raft_term = raft_term;
	pthread_mutex_unlock(&(kafka_toma_requested_term_and_offset_mutex));
}

void nvmeibt_kafka_req_start_consuming_leader_TARGET_msgs(int64_t kafka_offset_TARGET, int64_t seq_no_TARGET, unsigned long long raft_term)
{
	// TOMA request runs in the TOMA thread, and only marks for the kafka thread
	N_Tf(usmek2l, "k_offset=@LD raft_term=@LLX", purify_offset(kafka_offset_TARGET), raft_term);
	pthread_mutex_lock(&(kafka_toma_requested_term_and_offset_mutex));
	requested_incremental_TARGET_updates_consumer_offset = kafka_offset_TARGET;
	requested_incremental_TARGET_updates_consumer_seq_no = seq_no_TARGET;
	kafka_requested_consuming_leader_TARGET_msgs_raft_term = raft_term;
	pthread_mutex_unlock(&(kafka_toma_requested_term_and_offset_mutex));
}

static int kafka_apply_stop_consuming_leader_VOL_msgs(bool is_stop_due_to_leader_change) {
	if (kafka_applied_consuming_leader_VOL_msgs_raft_term) {
		kafka_applied_consuming_leader_VOL_msgs_raft_term = 0;
		__consumer_stop_on_raft(&k_incremental_VOL_updates);
	}
	if (is_stop_due_to_leader_change)
		k_incremental_VOL_updates.consumer_offset = RD_KAFKA_OFFSET_INVALID;
	return 0;
}

static int kafka_apply_stop_consuming_leader_TARGET_msgs(bool is_stop_due_to_leader_change) {
	if (kafka_applied_consuming_leader_TARGET_msgs_raft_term) {
		kafka_applied_consuming_leader_TARGET_msgs_raft_term = 0;
		__consumer_stop_on_raft(&k_incremental_TARGET_updates);
	}
	if (is_stop_due_to_leader_change)
		k_incremental_TARGET_updates.consumer_offset = RD_KAFKA_OFFSET_INVALID;
	return 0;
}

static void fix_start_offset_if_topic_was_reset(int64_t *offset, int8_t topic_change_no_from_v_3_3_persistence) {
	int8_t topic_change_no_from_offset;
	if (!is_RD_KAFKA_OFFSET_VALID(*offset)) {
		N_Tf(ji9908a, "offset is illegal, don't fix");
		return;
	}

	topic_change_no_from_offset = get_topic_change_no_from_offset(*offset);
	N_Tf(nchfi42, "in_offset=@INT64_TX topic_change_no_from_v_3_3_persistence=@INT8_TX", *offset, topic_change_no_from_v_3_3_persistence);
	if (topic_change_no_from_offset == KAFKA_TOPIC_CHANGE_NO)
		return;

	// We almost ignore the transition from 3.3 to 3.4. Indeed (raft_members_ctx->v3_3_kafka_topic_change_no == 1) from persistence,
	//  but since v3.4 uses the same topics, we do not need to start reading the topics from BEGINNING.
	if (topic_change_no_from_v_3_3_persistence == KAFKA_TOPIC_CHANGE_NO && topic_change_no_from_offset == 0)
		return;
	N_Tf(a0fub3m, "Fixing offset to RD_KAFKA_OFFSET_BEGINNING");
	*offset = RD_KAFKA_OFFSET_BEGINNING;
}

static int kafka_apply_consuming_leader_msgs_as_needed(void) {
	int						rv = 0;
	rd_kafka_resp_err_t		k_err;
	unsigned long long		sampled_req_VOL_raft_term;
	unsigned long long		sampled_req_TARGET_raft_term;
	int64_t					sampled_req_offset_VOL;
	int64_t					sampled_req_offset_TARGET;
	bool					is_leader_VOL_msgs_raft_term_same;
	bool					is_leader_TARGET_msgs_raft_term_same;

	// The leader/candidate reads from 2 queues, and they are not enabled/disabled together
	is_leader_VOL_msgs_raft_term_same = (kafka_applied_consuming_leader_VOL_msgs_raft_term == kafka_requested_consuming_leader_VOL_msgs_raft_term);
	is_leader_TARGET_msgs_raft_term_same = (kafka_applied_consuming_leader_TARGET_msgs_raft_term == kafka_requested_consuming_leader_TARGET_msgs_raft_term);
	if (is_leader_VOL_msgs_raft_term_same && is_leader_TARGET_msgs_raft_term_same) {
		N_Df(3vsh48s, "No changes in consuming leader queues, skipping");
		return 0;
	}
	NFIN;
	// raft_term changed. We need to either stop consuming leader msgs or start from scratch
	// Get all the related values
	pthread_mutex_lock(&(kafka_toma_requested_term_and_offset_mutex));
	sampled_req_VOL_raft_term = kafka_requested_consuming_leader_VOL_msgs_raft_term;
	sampled_req_TARGET_raft_term = kafka_requested_consuming_leader_TARGET_msgs_raft_term;
	sampled_req_offset_VOL = requested_incremental_VOL_updates_consumer_offset;
	sampled_req_offset_TARGET = requested_incremental_TARGET_updates_consumer_offset;
	nvmeibt_kafka_set_last_sent_to_toma_targets_updates_seq_no(requested_incremental_TARGET_updates_consumer_seq_no);
	pthread_mutex_unlock(&(kafka_toma_requested_term_and_offset_mutex));
	//
	if (!is_leader_VOL_msgs_raft_term_same) {	// Need to stop/start
		if (is_consuming_leader_VOL_msgs()) {    // Stop if we have a new raft term (also true for (new_term==0)==stop)
			kafka_apply_stop_consuming_leader_VOL_msgs(1);
		} else if (sampled_req_VOL_raft_term > 0) {
			struct t_consumer_impl *k = &k_incremental_VOL_updates;
			if (!is_RD_KAFKA_OFFSET_VALID(sampled_req_offset_VOL)) {
				N_Ef(ggy1218, "Invalid VOL offset=@LD", purify_offset(sampled_req_offset_VOL));
				nvmeibt_abort(ES_FATAL);
			}
			N_Tf(6visumr, "starting VOL consumption with raft_term=@LLX", sampled_req_VOL_raft_term);
			kafka_applied_consuming_leader_VOL_msgs_raft_term = sampled_req_VOL_raft_term;
			k->offset_committed = RAFT_COMMIT_LIFECYCLE_VAL(KAFKA_MGMT_CONFIG, follower_committed);
			fix_start_offset_if_topic_was_reset(&sampled_req_offset_VOL,
												nvmeibt_tlv_get_v_3_3_kafka_topic_change_no(&(nvmeibt_raft_get_my_raft()->follower_to_commit_persist_and_wire_buf_full->kafka_mgmt_config_ctx)));
			k_err = __consumer_assign_partition_and_offset(k, purify_offset(sampled_req_offset_VOL));
			N_Tf(evweyha, "rd_kafka_assign(VOL_updates_consumer_offset=@LD)", purify_offset(sampled_req_offset_VOL));
			if (k_err) {
				N_Wf(cvn4do8, "Failed rd_kafka_assign err='@STR'", rd_kafka_err2str(k_err));
				rv = -1;
			}
		}
	}
	if (!is_leader_TARGET_msgs_raft_term_same) {	// Need to stop/start
		if (is_consuming_leader_TARGET_msgs()) {    // Stop if we have a new raft term (also true for (new_term==0)==stop)
			kafka_apply_stop_consuming_leader_TARGET_msgs(1);
		} else if (sampled_req_TARGET_raft_term > 0) {
			struct t_consumer_impl *k = &k_incremental_TARGET_updates;
			if (!is_RD_KAFKA_OFFSET_VALID(sampled_req_offset_TARGET)) {
				N_Ef(ggy1214, "Invalid TARGET offset=@LD", purify_offset(sampled_req_offset_TARGET));
				nvmeibt_abort(ES_FATAL);
			}
			N_Tf(yvbo3le, "starting TARGET consumption with raft_term=@LLX", sampled_req_TARGET_raft_term);
			kafka_raft_members_sorted_msgs_queue_init();
			kafka_applied_consuming_leader_TARGET_msgs_raft_term = sampled_req_TARGET_raft_term;
			k->offset_committed = RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS, follower_committed);
			NVMEIBT_KAFKA_SET_LEADER_KAFKA_OFFSET_BLOCKING_INCREMENTAL_TARGET_UPDATES(rygba82, nvmeibt_offset_and_idx_uninitialized);	// A new leader starts from committed and is not in the middle of adding a target node to raft
			fix_start_offset_if_topic_was_reset(&sampled_req_offset_TARGET,
												nvmeibt_tlv_get_v_3_3_kafka_topic_change_no(&(nvmeibt_raft_get_my_raft()->follower_to_commit_persist_and_wire_buf_full->raft_members_ctx)));
			k_err = __consumer_assign_partition_and_offset(k, purify_offset(sampled_req_offset_TARGET));
			N_Tf(psiwjrn, "rd_kafka_assign(TARGET_updates_consumer_offset=@INT64_TD)", purify_offset(sampled_req_offset_TARGET));
			if (k_err) {
				N_Wf(vybsi4l, "Failed rd_kafka_assign err='@STR'", rd_kafka_err2str(k_err));
				rv = -1;
			}
		}
	}
	NFOUT;
	return rv;
}

static void kafka_poll_all_producers_in_order_to_get_their_cb(int timeout_ms) {
	if (k_high_priority.msg_to_mgmt_producer) {
		const int n_events_served = rd_kafka_poll(k_high_priority.msg_to_mgmt_producer, timeout_ms);
		if (n_events_served > 0)													// Otherwise clutters the log
			N_Tf(bse3kjb, "Poll k_high_priority.msg_to_mgmt_producer n_events_served=@INT", n_events_served);
	}
	if (k_low_priority.msg_to_mgmt_producer) {
		const int n_events_served = rd_kafka_poll(k_low_priority.msg_to_mgmt_producer, timeout_ms);
		if (n_events_served > 0)
			N_Tf(uxjdn3k, "Poll k_low_priority.msg_to_mgmt_producer n_events_served=@INT", n_events_served);
	}
	if (k_keepalive.msg_to_mgmt_producer) {
		const int n_events_served = rd_kafka_poll(k_keepalive.msg_to_mgmt_producer, timeout_ms);
		if (n_events_served > 0)													// Otherwise clutters the log
			N_Tf(bse3k8a, "Poll k_keepalive.msg_to_mgmt_producer n_events_served=@INT", n_events_served);
	}
}

static void kafka_commit_done_offsets_of_all_consumer_queues(void);
static void kafka_close_all_blocking(void) {
	NFIN;
	kafka_commit_done_offsets_of_all_consumer_queues();	// Commit whatever we can (An optimization)
	for (int msec = 100; atomic_read(&kafka_n_sends_in_the_air) > 0; msec++) {		// Do not close things when still in use, Linear backoff
		N_Tf(jsnewij, "n_sends_in_the_air=@INT. Waiting @INT[msec]", atomic_read(&kafka_n_sends_in_the_air), msec);
		kafka_poll_all_producers_in_order_to_get_their_cb(min(msec, 1000));			// After 90[sec] start polling at 1[hz]
		kafka_commit_done_offsets_of_all_consumer_queues();	// Commit whatever we can (An optimization)
	}
	N_Tf(5nduq93, "n_sends_in_the_air=0. Closing.");
	producer_close(&k_high_priority);
	producer_close(&k_low_priority);
	producer_close(&k_keepalive);
	consumer_close(&k_incremental_VOL_updates);
	consumer_close(&k_incremental_TARGET_updates);
	consumer_close(&k_HW_full_config);
	consumer_close(&k_CMD);
	kafka_apply_stop_consuming_leader_VOL_msgs(0);  // Do not affect the requested_is_raft_leader. Turn off the applied_is_raft_leader and close everything
	kafka_apply_stop_consuming_leader_TARGET_msgs(0);  // Do not affect the requested_is_raft_leader. Turn off the applied_is_raft_leader and close everything
	if (rd_kafka_wait_destroyed(2000) != 0)	// Since destroy is async. We want a clean shutdown
		N_Ef(__AUTOID___, "Failed wait for kafka destroy. May stuck on next kafka restart");
	NFOUT;
}

static int kafka_init(bool is_full_init) {
	int						rv = 0;
	NFIN;
	NVMEIBT_KAFKA_SET_LEADER_KAFKA_OFFSET_BLOCKING_INCREMENTAL_TARGET_UPDATES(rubs7vd, nvmeibt_offset_and_idx_uninitialized);
	// Init the producers and the consumers
	if ( (incremental_VOL_updates_consumer_init(is_full_init) < 0) ||
		 (incremental_TARGET_updates_consumer_init(is_full_init) < 0) ||
		 (HW_full_config_consumer_init(is_full_init) < 0) ||
		 (CMD_consumer_init(is_full_init) < 0) ||
		 (high_priority_msg_to_mgmt_producer_init() < 0) ||
		 (low_priority_msg_to_mgmt_producer_init() < 0) ||
		 (keepalive_msg_to_mgmt_producer_init() < 0)) {
		rv = -1;
	}
	N_IMf(kamweuj, "k_heartbeat_interval_ms=@INT, k_client_id=@STR", k_heartbeat_interval_ms, nvmeibt_get_my_hostname());
	NFOUT;
	return rv;
}

static int kafka_commit_by_offset_async(struct t_consumer_impl *k, const int64_t offset) {
	rd_kafka_resp_err_t rv = RD_KAFKA_RESP_ERR_NO_ERROR;
	if (k->consumer) {
		rd_kafka_topic_partition_list_t *offsets = rd_kafka_topic_partition_list_new(1);
		N_Tf(76hd89e, "@STR: Commiting k_offset=@LD", rd_kafka_name(k->consumer), purify_offset(offset));
		rd_kafka_topic_partition_list_add(offsets, k->topic_name, k->consumer_partition);
		offsets->elems[0].offset = purify_offset(offset) + 1;	// The API says "last_consumed(processed) + 1"
		rv = rd_kafka_commit(k->consumer, offsets, 1 /*async*/);
		NTOMA_ASSERT(mdvewks, rv == RD_KAFKA_RESP_ERR_NO_ERROR, "@STR: rd_kafka_commit(@INT64) rv=@INT '@STR'", k->topic_name, offsets->elems[0].offset, rv, rd_kafka_err2str(rv));
		rd_kafka_topic_partition_list_destroy(offsets);
		k->offset_committed = offset;
	}
	return (rv != RD_KAFKA_RESP_ERR_NO_ERROR);
}

static void kafka_commit_done_offsets_of_all_consumer_queues(void) {
	// Every queue has its own story. They differ in
	// - Strict ordering
	// - Completion per msg or per bulk of messages
	// Some CMDs are handled immediately (updKeepaliveToken)
	// Some CMDs are sent to TOMA, and handled by the TOMA thread.
	// CMDs are not guaranteed to finish in-order by TOMA (One format might take longer than the other)
	// We only commit if there are no CMDs awaiting_toma_processing
	const int64_t CMD_kafka_offset_to_commit = k_CMD.consumer_offset;			// Might be ahead, but not committed because TOMA is still processing an older CMD
	if (k_CMD.offset_committed != CMD_kafka_offset_to_commit) {
		if (atomic_read(&CMD_consumer_n_msgs_awaiting_toma_processing) == 0) {  // Otherwise an older msg did not yet finish processing
			kafka_commit_by_offset_async(&k_CMD, CMD_kafka_offset_to_commit);	// If nothing is processed by TOMA (and naturally all the immediate ones finished processing), we can commit the latest
		} else {
			N_Tf(bs7i2ja, "Skipping Commit k_offset=@LD CMD_consumer_n_msgs_awaiting_toma_processing=@INT", purify_offset(CMD_kafka_offset_to_commit), atomic_read(&CMD_consumer_n_msgs_awaiting_toma_processing));
		}
	}
	{ // All HW_full_config updated are handled by TOMA, (in order)
		const int64_t offset_to_commit = HW_full_config_consumer_offset_committed_by_toma - 1;	// Commit all but the last one
		if (purify_offset(offset_to_commit) >= 0 && (purify_offset(offset_to_commit) > purify_offset(k_HW_full_config.offset_committed)))
			kafka_commit_by_offset_async(&k_HW_full_config, offset_to_commit);
	}
	if (is_consuming_leader_VOL_msgs()) {
		// VOL updates are handled by toma (in order) (VOL), Tokens are handled immediately by the kafka code
		const int64_t offset_to_commit = RAFT_COMMIT_LIFECYCLE_VAL(KAFKA_MGMT_CONFIG, leader_committed_by_majority);
		if (purify_offset(offset_to_commit) > purify_offset(k_incremental_VOL_updates.offset_committed)) {
			N_Tf(vnd8oel, "VOL: Commiting k_offset=@INT64_TD latest=@INT64_TD", purify_offset(offset_to_commit), purify_offset(k_incremental_VOL_updates.consumer_offset));
			kafka_commit_by_offset_async(&k_incremental_VOL_updates, offset_to_commit);
		}
	}
	if (is_consuming_leader_TARGET_msgs()) {
		// All incremental_TARGET_updates are handled by TOMA
		// They might be handled out-of-order offset-wise, since we have a queue that reorders them according to seq-no
		// - We only commit (the highest offset ever, already committed by the raft-majority) if the seq_no committed by the raft_majority is equal to the last one we submitted to toma, and the queue is empty
		const int64_t offset_to_commit = RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS, leader_committed_by_majority);	// If we decide to commit
		if (purify_offset(offset_to_commit) > purify_offset(k_incremental_TARGET_updates.offset_committed)) {
			const int64_t incremental_TARGET_update_seq_no_committed_by_raft_majority = RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS_SEQ_NO, leader_committed_by_majority);
			if ((incremental_TARGET_update_seq_no_committed_by_raft_majority == last_sent_to_toma_targets_updates_seq_no) && XDLIST_EMPTY(&kafka_raft_members_sorted_msgs_queue)) {
				kafka_commit_by_offset_async(&k_incremental_TARGET_updates, offset_to_commit);
			}
		}
	}
}

static void *nvmeibt_kafka_main_thread(void *args __attribute__((__unused__))) {
	unsigned long long		nsec_sleep_when_producer_msgs_in_the_air = MSEC_TO_NSEC(5);
	unsigned long long		nsec_sleep_when_nothing_to_do = MSEC_TO_NSEC(10);
	unsigned long long		nsec_sleep_when_CMD_awaiting_toma_processing = 100000;
	//
	struct timespec			prev_last_kafka_consume_timespec;
	long					max_nsec_to_comply_with_kafka_heartbeat = MSEC_TO_NSEC(k_heartbeat_interval_ms) / 3;
	struct timespec			cur_sleep_timespec = TIMESPEC_ZERO;
	int						tmp_kafka_requested_init_counter;
	int						consume_rv;
	bool					is_consume_skipped_due_to_awaiting_toma_processing = 0;
	int						n_is_consume_skipped_due_to_awaiting_toma_processing = 0;
	bool					is_full_init;
	bool					is_init_preserve_state_vars_counter;

	NFIN;
	nanosleep(&(struct timespec){0, 100}, NULL);
	while (1) {
		N_Df(8sh34bh, "Loop start");
		getnstimeofday_boot(&(nvmeibt_global_get_global()->kafka_last_activity_time));
		if (is_consume_skipped_due_to_awaiting_toma_processing) {
			n_is_consume_skipped_due_to_awaiting_toma_processing++;
		} else {
			n_is_consume_skipped_due_to_awaiting_toma_processing = 1;	// Start with 1 since we increment only on the beginning of the next iteration
		}
		is_consume_skipped_due_to_awaiting_toma_processing = 0;
		//
		// Avoid the various init in parallel to consume
		tmp_kafka_requested_init_counter = kafka_requested_init_counter;
		is_full_init = (kafka_applied_init_counter < tmp_kafka_requested_init_counter);
		is_init_preserve_state_vars_counter = (!is_full_init && (kafka_applied_init_preserve_state_vars_counter < kafka_requested_init_preserve_state_vars_counter));
		if (is_full_init || is_init_preserve_state_vars_counter) {
			if (kafka_applied_init_counter) {		// Already initialized
				kafka_close_all_blocking(); 		// Drain all outgoing messages
				kafka_outgoing_msgs_queue_rewind_for_resend();
			}
			if (kafka_requested_is_kafka_shutdown) {
				N_Tf(jsnm3m2, "requested_is_kafka_shutdown");
				kafka_is_done_shutdown = 1;
				break;
			} else if (kafka_init(is_full_init) == 0) {
				kafka_applied_init_counter = tmp_kafka_requested_init_counter;
				kafka_applied_init_preserve_state_vars_counter = kafka_requested_init_preserve_state_vars_counter;
			} else {
				N_Ef(juew822, "Kafka init failed");
				nanosleep(&(struct timespec){5, 0}, NULL); // 5 sec
				continue;
			}
			getnstimeofday_boot(&kafka_last_restart_timestamp);
		}
		send_keepalive_msgs_as_needed();
		// Here, everything must be properly initialized and stable (no free() etc)
		kafka_outgoing_msgs_queue_send_pending_msgs_to_kafka_producer();
		//
		kafka_poll_all_producers_in_order_to_get_their_cb(0 /* non_blocking*/);
		// Sample all the consumer queues. First is CMD, last are incremental updates
		// If anything consumed, restart the loop. Sleep only if nothing was consumed
		prev_last_kafka_consume_timespec = kafka_last_consume_timespec;
		consume_rv = CMD_consume();
		is_consume_skipped_due_to_awaiting_toma_processing |= (consume_rv >= 2000);
		if (!timespec_eq(kafka_last_consume_timespec, prev_last_kafka_consume_timespec)) {
			N_Tf(ckaoek5, "Consumed CMD");
			continue;
		}
		if (nvmeibt_kafka_is_mgmt_zone_specified()) {
			HW_full_config_consume();
			if (!timespec_eq(kafka_last_consume_timespec, prev_last_kafka_consume_timespec)) {
				N_Tf(rvsyuwk, "Consumed HW_full_config");
				continue;
			}
			kafka_apply_consuming_leader_msgs_as_needed();
			consume_rv = incremental_TARGET_updates_consume();
			is_consume_skipped_due_to_awaiting_toma_processing |= (consume_rv >= 2000);
			if (!timespec_eq(kafka_last_consume_timespec, prev_last_kafka_consume_timespec)) {
				N_Tf(5vd82lr, "Consumed incremental_TARGET_updates");
				continue;
			}
			consume_rv = incremental_VOL_updates_consume();
			is_consume_skipped_due_to_awaiting_toma_processing |= (consume_rv >= 2000);
			if (!timespec_eq(kafka_last_consume_timespec, prev_last_kafka_consume_timespec)) {
				N_Tf(cvbsj82, "Consumed incremental_VOL_updates");
				cur_sleep_timespec = timespec_from_nsec(nsec_sleep_when_producer_msgs_in_the_air);
				nanosleep(&cur_sleep_timespec, NULL);
				continue;
			}
		}
		kafka_commit_done_offsets_of_all_consumer_queues();
		kafka_outgoing_msgs_queue_process_msgs_accepted_by_kafka();
		//
		// Decide whether to sleep in beetween iterations and for how long
		if (!timespec_eq(kafka_last_consume_timespec, prev_last_kafka_consume_timespec)) {
			// If we consume anything we go to the loop start immediately (see "continue")
			N_Ef(2vxnmyo, "We just consumed something unknown, no need to sleep");
			cur_sleep_timespec = TIMESPEC_ZERO;
		} else if (is_consume_skipped_due_to_awaiting_toma_processing) {
			cur_sleep_timespec = timespec_from_nsec(min(nsec_sleep_when_nothing_to_do, nsec_sleep_when_CMD_awaiting_toma_processing * n_is_consume_skipped_due_to_awaiting_toma_processing));
		} else {
			N_Df(vbtuskr, "nothing_to_do");
			cur_sleep_timespec = timespec_from_nsec(nsec_sleep_when_nothing_to_do);
		}
		NTOMA_ASSERT(rcvayre, cur_sleep_timespec.tv_nsec < SEC_TO_NSEC(1), "Bad ts=@LD", cur_sleep_timespec.tv_nsec);
		cur_sleep_timespec = timespec_min(cur_sleep_timespec, timespec_from_nsec(max_nsec_to_comply_with_kafka_heartbeat));
		N_Tf(vsdfhgv, "Sleeping @LD ns", cur_sleep_timespec.tv_nsec);
		nanosleep(&cur_sleep_timespec, NULL);
	}
	NFOUT;
	return NULL;
}

#define my_strncmp(__s1, __s2_Str, __len) ({						\
	int			__rv__;												\
	if (!__s1 || !__s2_Str) {										\
		__rv__ = (!!__s1 - !!__s2_Str);								\
	} else {														\
		__rv__ = (strncmp(__s1, nvmeibt_Str_str(__s2_Str), __len));	\
	}																\
	__rv__;															\
})

static inline bool is_str_eq_yes(const char* s) {
	return (s && ((strncasecmp(s, "true", 4) == 0) || (strncasecmp(s, "yes", 3) == 0)));
}

void nvmeibt_kafka_upd_from_nvmesh_conf(void) {
	// Just read all the latest values
	const char *is_enabled_str = 						nvmeibt_global_nvmesh_conf_get_val_by_key("KAFKA_TLS_ENABLED");
	const char *is_hostname_verification_disabled_str = nvmeibt_global_nvmesh_conf_get_val_by_key("KAFKA_TOMA_DISABLE_HOSTNAME_VERIFICATION");
	const char *bootstrap_servers_str =					nvmeibt_global_nvmesh_conf_get_val_by_key("KAFKA_SERVERS");
	const char *ca =									nvmeibt_global_nvmesh_conf_get_val_by_key("KAFKA_CA");
	const char *toma_certificate =						nvmeibt_global_nvmesh_conf_get_val_by_key("KAFKA_TOMA_CERTIFICATE");
	const char *key_location =							nvmeibt_global_nvmesh_conf_get_val_by_key("KAFKA_TOMA_KEY");
	const char *key_password =							nvmeibt_global_nvmesh_conf_get_val_by_key("KAFKA_TOMA_KEY_PASSPHRASE");
	const bool is_enabled =								is_str_eq_yes(is_enabled_str);
	const bool is_hostname_verification_disabled =		is_str_eq_yes(is_hostname_verification_disabled_str);
	bool is_any_kafka_field_different;

	NFIN;
	// First time ALLOC
	if (!kafka_bootstrap_servers_str_from_nvmesh_conf) {
		kafka_bootstrap_servers_str_from_nvmesh_conf = NNVMEIBT_STR_ALLOC(rxcvq98);
		kafka_mtls_ssl__ca = NNVMEIBT_STR_ALLOC(hd6smck);
		kafka_mtls_ssl__toma_certificate = NNVMEIBT_STR_ALLOC(2aocbgs);
		kafka_mtls_ssl__key_location = NNVMEIBT_STR_ALLOC(noqi6x2);
		kafka_mtls_ssl__key_password = NNVMEIBT_STR_ALLOC(3zy10be);
	}
	if (is_enabled) {
		__intercept_toma_certificate_copy_file_and_save_content(&_ssl, &ca, &toma_certificate, &key_location, &key_password);
	}
	is_any_kafka_field_different =
		is_enabled != kafka_mtls_ssl__is_enabled ||
		is_hostname_verification_disabled != kafka_mtls_ssl__is_hostname_verification_disabled ||
		my_strncmp(bootstrap_servers_str, kafka_bootstrap_servers_str_from_nvmesh_conf, PATH_MAX) ||
		my_strncmp(ca, kafka_mtls_ssl__ca, 8192) ||
		my_strncmp(toma_certificate, kafka_mtls_ssl__toma_certificate, 8192) ||
		my_strncmp(key_location, kafka_mtls_ssl__key_location, PATH_MAX) ||
		my_strncmp(key_password, kafka_mtls_ssl__key_password, 8192);
	if (!is_any_kafka_field_different) {
		N_Tf(fsk5nb3, "No change");
		goto out;
	}
	// Copy everything. If the value could not be read from nvmesh.conf, then leave the old value intact
	kafka_mtls_ssl__is_enabled = (is_enabled_str ? is_enabled : kafka_mtls_ssl__is_enabled);
	kafka_mtls_ssl__is_hostname_verification_disabled = (is_hostname_verification_disabled_str ? is_hostname_verification_disabled : kafka_mtls_ssl__is_hostname_verification_disabled);
	if (bootstrap_servers_str) {
		nvmeibt_Str_strcpy(kafka_bootstrap_servers_str_from_nvmesh_conf, bootstrap_servers_str);
	}
	if (ca) {
		nvmeibt_Str_strcpy(kafka_mtls_ssl__ca, ca);
	}
	if (toma_certificate) {
		nvmeibt_Str_strcpy(kafka_mtls_ssl__toma_certificate, toma_certificate);
	}
	if (key_location) {
		nvmeibt_Str_strcpy(kafka_mtls_ssl__key_location, key_location);
	}
	if (key_password) {
		nvmeibt_Str_strcpy(kafka_mtls_ssl__key_password, key_password);
	}
	//
	if (!kafka_bootstrap_servers_str_from_nvmesh_conf) {
		N_Ef(tcvawqg, "!kafka_bootstrap_servers_str_from_nvmesh_conf");
		nvmeibt_abort(ES_FATAL);
	}
	// Log
	N_Tf(1b9wk2n, "KAFKA_TLS_ENABLED=@BOOL KAFKA_CA=@STR KAFKA_TOMA_CERTIFICATE=@STR",
		 kafka_mtls_ssl__is_enabled, nvmeibt_Str_str(kafka_mtls_ssl__ca), nvmeibt_Str_str(kafka_mtls_ssl__toma_certificate));
	N_Tf(1b9wu85, "KAFKA_TOMA_KEY=@STR KAFKA_TOMA_KEY_PASSPHRASE=@STR KAFKA_TOMA_DISABLE_HOSTNAME_VERIFICATION=@BOOL",
		 nvmeibt_Str_str(kafka_mtls_ssl__key_location), nvmeibt_Str_str(kafka_mtls_ssl__key_password), kafka_mtls_ssl__is_hostname_verification_disabled);
	N_Tf(cbwyk29, "kafka_bootstrap_servers_str_from_nvmesh_conf='@STR'", nvmeibt_Str_str(kafka_bootstrap_servers_str_from_nvmesh_conf));
	//
	N_IMf(he82475, "Marking kafka soft init required");
	kafka_requested_init_preserve_state_vars_counter++; // The main loop will re-init
	//
out:
	NFOUT;
}

int nvmeibt_kafka_launch(void) {
	int						rv = 0;
	pthread_t kafka_maintenance_thread_tid;
	NFIN;
	//nvmeibt_kafka_upd_from_nvmesh_conf();	// No need, was already called by nvmeibt_toma_init(), we did not reread nvmesh conf since then
	// Launch the nvmeibt_kafka_maintenance_thread (consumer & trigger callbacks)
	getnstimeofday_boot(&(nvmeibt_global_get_global()->kafka_last_activity_time));
	if (pthread_create(&kafka_maintenance_thread_tid, NULL, nvmeibt_kafka_main_thread, NULL) == 0) {
		pthread_setname_np(kafka_maintenance_thread_tid, "kafka_main");
	} else {
		N_Ef(4aikrbw, "failed to launch the nvmeibt_kafka_maintenance_thread (@AUTO_ERRNO)");
		rv = -1;
	}
	NFOUT;
	return rv;
}

void nvmeibt_kafka_shutdown(void)
{
	NFIN;
	kafka_requested_is_kafka_shutdown = 1;
	kafka_requested_init_counter++;	// Stop all new activity
	__t_certificate_storage_destroy(&_ssl);	// This cannot guarantee deletion, as exe may crash
	NFOUT;
}

bool nvmeibt_kafka_is_kafka_done_shutdown(void)
{
	return kafka_is_done_shutdown;
}

/******************************************************************************/
/*************** The incoming msg handler in TOMA main thread  ****************/
/******************************************************************************/

static void toma_incremental_vol_update_handler(struct mm_mgmt_conf *mgmt_conf, enum KAFKA_EVENT_TYPE event_type, int64_t kafka_offset)
{
	NFIN;
	if (!nvmeibt_raft_is_leader()) {
		N_Tf(sk4i2nw, "Not a leader, probably an old msg. Skipping");
		goto out;
	}
	if (event_type == KAFKA_EVENT_TYPE_VOL_ADD || event_type == KAFKA_EVENT_TYPE_VOL_UPD) {
		nvmeibt_read_config_apply_vol_mgmt_conf(mgmt_conf, ++(nvmeibt_global_get_global()->config_tag), 1, event_type, 0);	// Naturally, comming from mgmt (leader's role)
	} else if (event_type == KAFKA_EVENT_TYPE_VOL_DEL) {
		nvmeibt_read_config_vol_mark_vol_and_segs_for_removal(mgmt_conf, 1);	// Naturally, comming from mgmt (leader's role)
	} else if (event_type == KAFKA_EVENT_TYPE_VOL_DEL_COMPLETED) {
		nvmeibt_read_config_vol_removed_from_mgmt(mgmt_conf, 1);	// Naturally, comming from mgmt (leader's role)
	} else {
		N_Ef(rcsvsau, "Unknown event_type=@STR", kafka_event_type_str(event_type));
	}
	//
	//
	nvmeibt_topology_setup_relationships();
	/* dumper: log this change-of-configuration event */
	TODO(nvmeibt_dumper_event_mgmt_config());
	//
	nvmeibt_topology_mark_update_csv_of_config_and_topo_required();
	//
	// The (KAFKA_MGMT_CONFIG, leader_calculated) should be exactly the offset that was already read from kafka, and not just more than the currently distributed
	// The serialization of mgmt_config, already ignores (removes) the OUTDATED blkdevs, so if we actually garbage_collect (delete) them it has no effect
	SET_RAFT_COMMIT_LIFECYCLE_VAL(cbasjh3, KAFKA_MGMT_CONFIG, leader_calculated, kafka_offset);
	// Don't build the new kafka_mgmt_config_to_wire here. It will be done after topo calc
	// If we'll decide to build it here anyway, we must update the (KAFKA_MGMT_CONFIG, leader_to_commit) val
	// nvmeibt_mm_json_leader_serialize_kafka_mgmt_config_to_wire();
out:
	NFOUT;
}

static int toma_incremental_target_update_handler(struct name_and_uuid_params_ctx *add_del_member_params, enum KAFKA_EVENT_TYPE event_type, int64_t kafka_offset) {
	if (!nvmeibt_raft_is_leader() && (event_type != KAFKA_EVENT_TYPE_TARGET_ADD)) {
		N_Tf(tvajhwi, "Not a leader, probably an old msg. Skipping. k_offset=@INT64_TD", purify_offset(kafka_offset));
		return 0;
	}
	if (add_del_member_params->targets_updates_sequence <= RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS_SEQ_NO, leader_calculated)) {
		N_Tf(cbteo3m, "seq_no=@INT64_TD already applied", add_del_member_params->targets_updates_sequence);
		return 0;
	}
	// N_Tf(cgayh2n, "hostname=@STR uuid=@UUID_LE n_members_total_before_add_del=@INT targets_updates_sequence=@LLD", add_del_member_params->hostname, &(add_del_member_params->uuid), add_del_member_params->n_members_total_before_add_del, name_and_uuid_params->targets_updates_sequence);
	if (event_type == KAFKA_EVENT_TYPE_TARGET_ADD) {
		nvmeibt_raft_add_member(add_del_member_params->hostname, add_del_member_params->n_members_total_before_add_del, &(add_del_member_params->uuid), 1, kafka_offset, -1);
	} else if (event_type == KAFKA_EVENT_TYPE_TARGET_DEL) {
		nvmeibt_raft_del_member(add_del_member_params->hostname, add_del_member_params->n_members_total_before_add_del, &(add_del_member_params->uuid), 1, kafka_offset);
	} else {
		N_Ef(smxih52, "Unknown event_type=@STR", kafka_event_type_str(event_type));
	}
	//
	nvmeibt_topology_mark_update_csv_of_config_and_topo_required();
	SET_RAFT_COMMIT_LIFECYCLE_VAL(cbsj50w, RAFT_MEMBERS,        leader_calculated, kafka_offset);	// When I read an incremental update, it is certainly not committed yet, and I play the role of the leader (elected or first member)
	SET_RAFT_COMMIT_LIFECYCLE_VAL(vbsi4kl, RAFT_MEMBERS_SEQ_NO, leader_calculated, add_del_member_params->targets_updates_sequence);  // When I read an incremental update, it is certainly not committed yet, and I play the role of the leader (elected or first member)
	nvmeibt_topology_serialize_conf_and_topo_if_needed();	// Leader: The equivalent of end of calc_topo. Needs to serialize the raft members to commit
	nvmeibt_raft_get_my_raft()->applied_raft_members_seq_no = RAFT_COMMIT_LIFECYCLE_VAL(RAFT_MEMBERS_SEQ_NO, leader_calculated);
	return 0;
}

static int toma_HW_full_config_handler(struct HW_mgmt_conf **conf_ptr, int64_t kafka_offset) {
	struct HW_mgmt_conf *conf = *conf_ptr;
	static int				config_tag = 0;
	int						rv = -1;
	static int64_t			max_configurationVersion = -1;	// The max we have seen in this instance of TOMA
	NFIN;
	if (conf->configurationVersion <= max_configurationVersion) {
		N_Wf(fniz6q3, "Received configurationVersion=@INT64_TD prev max_configurationVersion=@INT64_TD", conf->configurationVersion, max_configurationVersion);
		if (conf->configurationVersion < max_configurationVersion) {
			rv = -1;
			goto out;
		} else {
			// Same configurationVersion, possibly two MGMT instances updated it, and found out that nothing changed
		}
	}
	{	//	Just print the config to log
		struct nvmeibt_Str *conf_str = NNVMEIBT_STR_ALLOC(u8u7de4);
		HW_print_conf(conf, (nvmeibt_status_printf_fn_type)&nvmeibt_Str_sprintf, conf_str);
		NVMEIBT_LONG_TRACE_WRAPPER(dmii4k0, "HW_CONFIG", nvmeibt_Str_str(conf_str), nvmeibt_Str_strlen(conf_str));
		NNVMEIBT_STR_FREE(jsdu1ha, conf_str);
	}
	HW_conf_free_tree(nvmeibt_global_get_global()->HW_mgmt_conf);	// HACK: Race condition here with printing proc thread! may crash it by freeing during a print
	nvmeibt_global_get_global()->HW_mgmt_conf = conf;				// HACK !!! used for proc printing by direct pointer (in multithreaded access) Fix ME
	*conf_ptr = NULL;												// Stole ownership of pointer in the line above
	nvmeibt_read_config_apply_HW_full_config_mgmt_conf(conf, ++config_tag, 1);
	//
	nvmeibt_topology_setup_relationships(); TODO(Only handle those that are related to H/W);
	nvmeibt_node_locate_my_node();
	if (!nvmeibt_topology_is_HW_config_functional()) {
		rv = 0; /* Don't return error in this case because the node absence may be temporary */
	} else {
		rv = 0;
		nvmeibt_toma_init_mesh();
		TODO(nvmeibt_dumper_event_mgmt_config());
	}
out:
	mark_HW_full_config_k_msg_for_kafka_commit(kafka_offset, 1, (conf->configurationVersion >= max_configurationVersion));    // GOOD/BAD config. We do not want to reread. Possibly not commit
	max_configurationVersion = max(max_configurationVersion, conf->configurationVersion);
	NFOUT;
	return rv;
}

void nvmeibt_kafka_send_encrypt_cmd_response(const char *vol_name, const struct nvmeibt_urn_uuid *vol_uuid,
											 int encrypt_idx, enum ENCRYPT_CMD_RESPONSE error_code,
											 bool is_retryable, const char *error_str) {
	static struct nvmeibt_Str				*json_payload = NULL;
	if (!json_payload)
		json_payload = NNVMEIBT_STR_ALLOC(i877ud3);
	nvmeibt_Str_reuse(json_payload);
	nvmeibt_Str_sprintf(json_payload, "{" KAFKA_PRODUCER_MSG_HEADER_FMT
			"\"payload\":{\"volumeName\": \"%s\", \"volumeUUID\": \"%s\", \"encryptionCommandIndex\": %d, \"result\": %d, \"retryable\": \"%s\", \"error\": \"%s\"}}",
			KAFKA_PRODUCER_MSG_HEADER_VAR("encryptionCommandResponse", 1),
			vol_name, vol_uuid->str,
			encrypt_idx, error_code, is_retryable ? "true" : "false", error_str);
	//char unique_key[NVMEIBT_KAFKA_MAX_UNIQUE_KEY_LEN] = "Encript_res_";
	//nvmeibt_strlcpy(unique_key + strlen(unique_key), vol_uuid->str, sizeof(unique_key) - strlen(unique_key));
	nvmeibt_kafka_outgoing_msgs_queue_add(NULL /*unique_key*/, nvmeibt_Str_str(json_payload), nvmeibt_Str_strlen(json_payload) + 1, NVMEIBT_KAFKA_OUTGOING_MSGS_PRIORITY_HIGH);
}

static bool start_encrypt_action(struct generic_CMD_params_ctx *CMD_params,
								 char *encrypt_cmd, char *encrypt_args, char *old_passphrase, char *new_passphrase, int64_t kafka_offset)
{
	union nvmeib_uuid					*vol_uuid = &CMD_params->volumeUUID;
	int									encrypt_idx = CMD_params->encryptionCommandIndex;
	struct nvmeibt_block_device			*vol;
	struct nvmeibt_encrypt_params		*encrypt_params = NULL;
	bool								rv = 1;
	char								shadow_vol_name[32];
	int									n_written;

	NFIN;
	N_Tf(nzzxgt6, "cmd='@STR', args='@STR'", encrypt_cmd, encrypt_args);
	if (CMD_params->bootTime != nvmeibt_global_get_startup_timestamp_msec()) {
		const struct nvmeibt_urn_uuid urn_uuid = nvmeibt_union_uuid_to_urn_uuid(vol_uuid);
		nvmeibt_kafka_send_encrypt_cmd_response("", &urn_uuid, encrypt_idx, ENCRYPT_CMD_RESPONSE_MANUAL_ACTION_NEEDED, 1, "Boot time mismatch");
		goto out;
	}
	vol = nvmeibt_block_device_get_block_device_by_id(vol_uuid);
	if (!vol) {
		const struct nvmeibt_urn_uuid urn_uuid = nvmeibt_union_uuid_to_urn_uuid(vol_uuid);
		nvmeibt_kafka_send_encrypt_cmd_response("Not_found", &urn_uuid, encrypt_idx, ENCRYPT_CMD_RESPONSE_TOMA_ERR, 0, "Volume doesn't exist");
		goto out;
	}
	if (encrypt_idx <= vol->encrypt_idx) {
		nvmeibt_kafka_send_encrypt_cmd_response(vol->from_config.client_blkdev_name, &vol->urn_uuid, encrypt_idx, ENCRYPT_CMD_RESPONSE_TOMA_ERR, 0, "Old or duplicate command");
		goto out;
	}
	if (vol->encrypt_params) {
		nvmeibt_kafka_send_encrypt_cmd_response(vol->from_config.client_blkdev_name, &vol->urn_uuid, encrypt_idx, ENCRYPT_CMD_RESPONSE_TOMA_ERR, 0, "Prev command didn't complete");
		goto out;
	}
	if (NVMEIBT_OBJ_IS_MARKED_OUTDATED(vol)) {
		nvmeibt_kafka_send_encrypt_cmd_response(vol->from_config.client_blkdev_name, &vol->urn_uuid, encrypt_idx, ENCRYPT_CMD_RESPONSE_TOMA_ERR, 0, "Volume already deleted");
		goto out;
	}
	//
	n_written = snprintf(shadow_vol_name, sizeof(shadow_vol_name), "e_%s", vol->from_config.client_blkdev_name);
	if (n_written >= (int)sizeof(shadow_vol_name)) {
		N_Ef(cbs7uuj2, "vol_name='@STR' is too long", vol->from_config.client_blkdev_name);
		nvmeibt_kafka_send_encrypt_cmd_response(vol->from_config.client_blkdev_name, &vol->urn_uuid, encrypt_idx, ENCRYPT_CMD_RESPONSE_TOMA_ERR, 0, "Volume name is too long");
		goto out;
	}
	//
	encrypt_params = NNVMEIBT_TOMA_CALLOC(uwwxc91, 1, sizeof(struct nvmeibt_encrypt_params));
	encrypt_params->kafka_offset = kafka_offset;
	vol->encrypt_idx = encrypt_idx;
	vol->encrypt_params = encrypt_params;
	encrypt_params->encrypt_idx = encrypt_idx;
	//	passphrases and their related args
	snprintf(encrypt_params->old_passphrase, sizeof(encrypt_params->old_passphrase), "%s", old_passphrase);
	snprintf(encrypt_params->new_passphrase, sizeof(encrypt_params->new_passphrase), "%s", new_passphrase);
	encrypt_params->old_passphrase_file_name[0] = '\0';
	if (old_passphrase[0]) {
		snprintf(encrypt_params->old_passphrase_file_name, sizeof(encrypt_params->old_passphrase_file_name),
				 PASSPHRASE_DIR_NAME "/old_passphrase_%s", shadow_vol_name);
	}
	encrypt_params->new_passphrase_file_name[0] = '\0';
	if (new_passphrase[0]) {
		snprintf(encrypt_params->new_passphrase_file_name, sizeof(encrypt_params->new_passphrase_file_name),
				 PASSPHRASE_DIR_NAME "/new_passphrase_%s", shadow_vol_name);
	}
	//
	if (old_passphrase[0] && new_passphrase[0]) {
		snprintf(encrypt_params->exec_ctx.executable_str, sizeof(encrypt_params->exec_ctx.executable_str),
				 "cryptsetup %s --key-file=%.256s /dev/nvmesh/%s %.256s",
				 encrypt_args, encrypt_params->old_passphrase_file_name, shadow_vol_name, encrypt_params->new_passphrase_file_name);
	} else {
		snprintf(encrypt_params->exec_ctx.executable_str, sizeof(encrypt_params->exec_ctx.executable_str), "cryptsetup %s --key-file=%.256s /dev/nvmesh/%s",
				 encrypt_args, (old_passphrase[0] ? encrypt_params->old_passphrase_file_name : encrypt_params->new_passphrase_file_name), shadow_vol_name);
	}
	nvmeibt_attach_vol_for_encryption(vol, shadow_vol_name, encrypt_params);
	rv = 0;

out:
	if (rv && encrypt_params) {	// Future proof. Cannot happen for now
		NNVMEIBT_TOMA_FREE(3xfa7j2, encrypt_params);
	}
	NFOUT;
	return rv;
}

static bool encrypt_command_request_response(struct generic_CMD_params_ctx *CMD_params) {
	union nvmeib_uuid					*vol_uuid = &CMD_params->volumeUUID;
	int									encrypt_idx = CMD_params->encryptionCommandIndex;
	struct nvmeibt_block_device			*vol;
	NFIN;
	vol = nvmeibt_block_device_get_block_device_by_id(vol_uuid);
	if (!vol) {
		const struct nvmeibt_urn_uuid urn_uuid = nvmeibt_union_uuid_to_urn_uuid(vol_uuid);
		nvmeibt_kafka_send_encrypt_cmd_response("Not_found", &urn_uuid, encrypt_idx, ENCRYPT_CMD_RESPONSE_TOMA_ERR, 0, "Volume doesn't exist");
	} else if (vol->encrypt_idx == NVMEIBT_BLOCK_DEVICE_UNINITIALIZED_ENCRYPT_IDX) {
		nvmeibt_kafka_send_encrypt_cmd_response(vol->from_config.client_blkdev_name, &vol->urn_uuid, encrypt_idx, ENCRYPT_CMD_RESPONSE_MANUAL_ACTION_NEEDED, 0, "Vol index is uninitialized");
	} else if (encrypt_idx > vol->encrypt_idx) {
		vol->encrypt_idx = encrypt_idx; // Ignore all commands with less encrypt_idx
		nvmeibt_kafka_send_encrypt_cmd_response(vol->from_config.client_blkdev_name, &vol->urn_uuid, encrypt_idx, ENCRYPT_CMD_RESPONSE_UNSEEN, 1, "Index has never been seen");
	}
	NFOUT;
	return 1;
}

static void toma_CMD_handler(struct generic_CMD_params_ctx *CMD_params, int64_t kafka_offset, struct messageType_params_ctx *messageType_params)
{
	int				i;
	bool			commit_now = 1;
	char			encrypt_args[MAX_EXEC_WITH_ARGS_STR_LEN];

	NFIN;
	if (strcmp(messageType_params->messageType, "formatDrive") == 0) {
		wakeup_format_event(&(CMD_params->ldisk_id), CMD_params->vendor, CMD_params->generic_uuid, CMD_params->blockSize,
							CMD_params->metadataSize, CMD_params->formatRequestCounter, CMD_params->bootTime, &(CMD_params->dbUUID),
							&(CMD_params->native_serial), CMD_params->nsid, CMD_params->native_nguid);
		TODO(Make sure that when this is done, the format will go all the way even if we boot, and there is no need for resend of format CMD by MGMT);
	} else if (strcmp(messageType_params->messageType, "reservationModeChange") == 0) {
		nvmeibt_block_device_reservation_mode_change(&CMD_params->volumeUUID, CMD_params->reservationVersion);
	} else if (strcmp(messageType_params->messageType, "resendReport") == 0) {
		for (i = 0; i < CMD_params->n_disks_to_report; i++) {
			struct resend_report_disk_ctx	*dsk = &(CMD_params->disks_to_report[i]);
			nvmeibt_local_disk_mark_is_specific_disk_report_req(dsk->ldiskID, dsk->reappearingCounter);
		}
	} else if (strcmp(messageType_params->messageType, "initEncryption") == 0) {
		// since cryptsetup did not autodetect sector size in versions <2.5.0 we force it to 4096, note the block autodetection is enable in 2.5.0 and later.
		#define LUKS_ARGS "--verbose --force-password --pbkdf-force-iterations 1000 --pbkdf-memory 100 --pbkdf-parallel 1"
		snprintf(encrypt_args, MAX_EXEC_WITH_ARGS_STR_LEN, "luksFormat --sector-size=4096 " LUKS_ARGS " --key-slot=%d --key-size=%d", CMD_params->slot, CMD_params->keySize);
		commit_now = start_encrypt_action(CMD_params, "init_enc", encrypt_args, "", CMD_params->passphrase, kafka_offset);
	} else if (strcmp(messageType_params->messageType, "rotatePassphrase") == 0) {
		snprintf(encrypt_args, MAX_EXEC_WITH_ARGS_STR_LEN, "luksChangeKey " LUKS_ARGS " --key-slot=%d", CMD_params->slot);
		commit_now = start_encrypt_action(CMD_params, "rotate_pass", encrypt_args, CMD_params->passphrase, CMD_params->newPassphrase, kafka_offset);
	} else if (strcmp(messageType_params->messageType, "deletePassphrase") == 0) {
		snprintf(encrypt_args, MAX_EXEC_WITH_ARGS_STR_LEN, "luksRemoveKey --verbose");
		commit_now = start_encrypt_action(CMD_params, "del_pass", encrypt_args, CMD_params->passphrase, "", kafka_offset);
	} else if (strcmp(messageType_params->messageType, "addPassphrase") == 0) {
		snprintf(encrypt_args, MAX_EXEC_WITH_ARGS_STR_LEN, "luksAddKey " LUKS_ARGS " --key-slot=%d", CMD_params->slot);
		commit_now = start_encrypt_action(CMD_params, "add_pass", encrypt_args, CMD_params->passphrase, CMD_params->newPassphrase, kafka_offset);
	} else if (strcmp(messageType_params->messageType, "testPassphrase") == 0) {
		snprintf(encrypt_args, MAX_EXEC_WITH_ARGS_STR_LEN, "open --verbose --test-passphrase /dev/nvmesh/d_<vol_name>");
		commit_now = start_encrypt_action(CMD_params, "test_pass", encrypt_args, CMD_params->passphrase, "", kafka_offset);
	} else if (strcmp(messageType_params->messageType, "encryptionRequestResponse") == 0) {
		commit_now = encrypt_command_request_response(CMD_params);
	} else if (strcmp(messageType_params->messageType, "sendPRaidReport") == 0) {
		N_Ef(rbasdrf78fh2, "******************** Need to send a pRAID report for a specific pRAID");
		for (i = 0; i < CMD_params->n_praids_to_report; i++) {
			struct send_praid_report_ctx	*prd = &(CMD_params->praids_to_report[i]);
			N_Ef(stamvuk, "uuid=@STR lastKnownVersion_major=@INT lastKnownVersion_minor=@INT lastKnownVersion_raft_term=@LU",
				 prd->praid_uuid, prd->lastKnownVersion_major, prd->lastKnownVersion_minor, prd->lastKnownVersion_raft_term);
		}
	} else if (strcmp(messageType_params->messageType, "--- shutdown_me ---") == 0) {
		N_Ef(p53ksmnz753bh, "******************** Use handle_update_state_shutdown() in the commands consumer");
		TODO(handle_update_state_shutdown(););
	} else if (strcmp(messageType_params->messageType, "--- shutdown_all ---") == 0) {
		N_Ef(48js83ols0ksi, "******************** Use on_shutdown_me_only() and on_shutdown_all() in the commands consumer");
		TODO(on_shutdown_me_only() and on_shutdown_all());
	} else if (strcmp(messageType_params->messageType, "--- LED ---") == 0) {
		N_Ef(rtwyuwbldsigvbh, "******************** Use led_command_cb() in the commands consumer");
		TODO(led_command_cb(led_msg->payload.ldiskID, led_msg->payload.vendorID, led_msg->payload.ledValue););
	} else {
		N_Ef(rvzqi2m, "Unsupported messageType='@STR'", messageType_params->messageType);
	}
	// All other CMDs, are handled by TOMA's main thread from wakeup. They receive the parsed json tree
	if (commit_now)
		mark_CMD_k_msg_for_kafka_commit(kafka_offset, 1);
	NFOUT;
}

static inline bool is_event_type_incremental(enum KAFKA_EVENT_TYPE event_type)
{
	return (event_type == KAFKA_EVENT_TYPE_VOL_ADD || event_type == KAFKA_EVENT_TYPE_VOL_DEL  || event_type == KAFKA_EVENT_TYPE_VOL_DEL_COMPLETED || event_type == KAFKA_EVENT_TYPE_VOL_UPD ||
			event_type == KAFKA_EVENT_TYPE_TARGET_ADD || event_type == KAFKA_EVENT_TYPE_TARGET_DEL);
}

void nvmeibt_kafka_toma_wakeup_dispatcher(struct kafka_wakeup_params *wakeup_params) {
	NFIN;
	if (	(is_event_type_incremental(wakeup_params->event_type) &&
			 (wakeup_params->kafka_raft_term_when_started_consuming_leader_msgs != nvmeibt_raft_get_current_term()) &&
			 (nvmeibt_raft_get_my_raft()->n_raft_members > 0))) {
		N_Wf(kslwm42, "Got an event from old raft_term=@LLX != cur_raft_term=@LLX. Ignoring", wakeup_params->kafka_raft_term_when_started_consuming_leader_msgs, nvmeibt_raft_get_current_term());
		goto out;
	}
	if (kafka_requested_is_kafka_shutdown) { 	// Unlikely, during shutdown do not process commands but do clean wakeup work-queue
		goto out;
	}
	N_Tf(nsjsd94, "event_type=@STR", kafka_event_type_str(wakeup_params->event_type));
	switch (wakeup_params->event_type) {
	case KAFKA_EVENT_TYPE_UNKNOWN:
		N_Ef(4nslowk, "KAFKA_EVENT_TYPE_UNKNOWN");
		break;
	case KAFKA_EVENT_TYPE_VOL_ADD:
	case KAFKA_EVENT_TYPE_VOL_DEL:
	case KAFKA_EVENT_TYPE_VOL_DEL_COMPLETED:
	case KAFKA_EVENT_TYPE_VOL_UPD:
		toma_incremental_vol_update_handler((struct mm_mgmt_conf *)wakeup_params->event_data, wakeup_params->event_type, wakeup_params->kafka_offset);
		break;
	case KAFKA_EVENT_TYPE_TARGET_ADD:
	case KAFKA_EVENT_TYPE_TARGET_DEL:
		toma_incremental_target_update_handler((struct name_and_uuid_params_ctx *)wakeup_params->event_data, wakeup_params->event_type, wakeup_params->kafka_offset);
		break;
	case KAFKA_EVENT_TYPE_HW_FULL_CONFIG:
		toma_HW_full_config_handler((struct HW_mgmt_conf **)&wakeup_params->event_data, wakeup_params->kafka_offset);
		break;
	case KAFKA_EVENT_TYPE_CMD:
		toma_CMD_handler((struct generic_CMD_params_ctx *)wakeup_params->event_data, wakeup_params->kafka_offset, &(wakeup_params->messageType_params));
		break;
	default:
		N_Ef(64ba9j2, "*******************************   FIX ME   ****************************** conf=@PTR", wakeup_params);
		break;
	}
out:
	__wakeup_toma_params_free(wakeup_params); // Free it. Used or not
	NFOUT;
}

int nvmeibt_kafka_print_status(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx) {
	struct tm					timeinfo;
	char						time_str[64];

	#define bool_YN(b)  ((b) ? 'Y' : 'N')
	localtime_r(&kafka_last_restart_timestamp.tv_sec, &timeinfo);
	strftime(time_str, sizeof(time_str), "%b %d %X ", &timeinfo);
	(*printf_fn)(printf_ctx, "\n- - - - -   Kafka Stats   - - - - -\n");
	(*printf_fn)(printf_ctx, "librdkafka version=0x%x, %s\n", rd_kafka_version(), rd_kafka_version_str());
	//(*printf_fn)(printf_ctx,"\tbroker.version=%s\n", rd_kafka_conf_get((rd_kafka_t*)rk, "broker.version.fallback", NULL, NULL) ? "?" : "?");
	(*printf_fn)(printf_ctx, "Last init at: %s\nparams:\n", time_str);
	(*printf_fn)(printf_ctx, "\tmtls_ssl_enabled=%c\n\thost_verification_disabled=%c\n", bool_YN(kafka_mtls_ssl__is_enabled), bool_YN(kafka_mtls_ssl__is_hostname_verification_disabled));
	if (kafka_mtls_ssl__is_enabled) {
		(*printf_fn)(printf_ctx, "\tssl_cach_path=%*s\n", kafka_mtls_ssl__ca->str_len, kafka_mtls_ssl__ca->text_buf);
		(*printf_fn)(printf_ctx, "\tssl_cert_path=%*s\n", kafka_mtls_ssl__toma_certificate->str_len, kafka_mtls_ssl__toma_certificate->text_buf);
		(*printf_fn)(printf_ctx, "\tssl_key__path=%*s\n", kafka_mtls_ssl__key_location->str_len, kafka_mtls_ssl__key_location->text_buf);
		(*printf_fn)(printf_ctx, "\tssl_pass_path=%*s\n", kafka_mtls_ssl__key_password->str_len, kafka_mtls_ssl__key_password->text_buf);
	}
	(*printf_fn)(printf_ctx, "Shutdown: req=%c, done=%c\n", bool_YN(kafka_requested_is_kafka_shutdown), bool_YN(nvmeibt_kafka_is_kafka_done_shutdown()));
	(*printf_fn)(printf_ctx, "In-air sent num_msgs=%d, msg_list_size=%d\n", atomic_read(&kafka_n_sends_in_the_air), kafka_outgoing_msgs_queue_get_num_msgs());
	(*printf_fn)(printf_ctx, "In-air cmds awaiting=%d\n", atomic_read(&CMD_consumer_n_msgs_awaiting_toma_processing));
	(*printf_fn)(printf_ctx, "full_init{applied=%d,init=%d}\nsoft_init{applied=%d,init=%d}\n", kafka_applied_init_counter, kafka_requested_init_counter, kafka_applied_init_preserve_state_vars_counter, kafka_requested_init_preserve_state_vars_counter);
	if (kafka_bootstrap_servers_str_from_nvmesh_conf)
		(*printf_fn)(printf_ctx, "\nservers=%*s\n", kafka_bootstrap_servers_str_from_nvmesh_conf->str_len, kafka_bootstrap_servers_str_from_nvmesh_conf->text_buf);
	(*printf_fn)(printf_ctx, "KeepAlive Mgmt token={Leader=%ld, Follow=%ld}\n", kafka_leader_keepalive_token_provided_by_mgmt, kafka_follower_keepalive_token_provided_by_mgmt);
	(*printf_fn)(printf_ctx, "Leaders Raft-Term:\n\tVolume={req=%ld, apply=%ld}\n\tTarget={req=%ld, apply=%ld}\n", kafka_requested_consuming_leader_VOL_msgs_raft_term, kafka_applied_consuming_leader_VOL_msgs_raft_term, kafka_requested_consuming_leader_TARGET_msgs_raft_term, kafka_applied_consuming_leader_TARGET_msgs_raft_term);
	if (kafka_mtls_ssl__is_enabled) {
		__t_certificate_storage_print(&_ssl, printf_fn, printf_ctx);
	}

	(*printf_fn)(printf_ctx, "\n- - - - -   Kafka debugging tips   - - - - -\n");
	(*printf_fn)(printf_ctx, "cd /opt/kafka/bin\n");
	(*printf_fn)(printf_ctx, "./kafka-consumer-groups.sh  --bootstrap-server <machine>:9092 --describe --group managements-group\n");
	(*printf_fn)(printf_ctx, "./kafka-console-consumer.sh --bootstrap-server <machine>:9092 --topic zone1.management.priority.1.0.0 | grep updatePRaidReport | jq .\n");
	(*printf_fn)(printf_ctx, "./kafka-console-consumer.sh --bootstrap-server <machine>:9092 --topic zone1.leader.incrementalUpdates.1.0.0 --from-beginning\n");
	(*printf_fn)(printf_ctx, "./kafka-dump-log.sh --print-data-log --files /var/lib/kafka/zone1.leader.incremental*/*.log\n");
	return 0;
}
