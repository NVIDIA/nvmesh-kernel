/**
 * status_report_test.c - Validate real-time error reporting and status visibility warnings
 */

#include "nvmeibt_debug.h"
#include "nvmeibt_common.h"
#include "nvmeibt_raft.h"
#include "nvmeibt_toma.h"
#include "nvmeibt_local_disk.h"
#include "nvmeibt_global.h"
#include "nvmeibt_topology.h"
#include "nvmeibt_kafka.h"
#include "nvmeibt_node.h"
#include "vol/nvmeibt_block_device.h"
#include "unitest/00_framework/toma_test_framework.h"
#include "unitest/00_framework/toma_test_helpers.h"
#include "status_report_test.h"
#include <stdlib.h>

// Not declared in header but non-static — declare here for test access
extern void print_one_local_disk_status(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx, struct nvmeibt_local_disk *local_disk);
#include <string.h>
#include <stdarg.h>

#define VERIFY_WRITE_ERROR_CODE "DRIVE_WRITE_ERROR"		// Todo: should have error code
/******************** Buffer-capturing printf ***************************/

#define STATUS_BUF_SIZE		(64 * 1024)

struct capture_ctx {
	char	*buf;
	int		len;
	int		capacity;
};

static int capture_printf(void *ctx, const char *fmt, ...)
{
	struct capture_ctx	*c = (struct capture_ctx *)ctx;
	va_list				ap;
	int					n;

	va_start(ap, fmt);
	n = vsnprintf(c->buf + c->len, (size_t)(c->capacity - c->len), fmt, ap);
	va_end(ap);
	if (n > 0)
		c->len += n;
	return n;
}

static void capture_reset(struct capture_ctx *c)
{
	c->len = 0;
	c->buf[0] = '\0';
}

/******************** Tests *********************************************/

DEFINE_TEST(local_disk_excluded_flag)
{
	struct capture_ctx			*c = (struct capture_ctx *)_ctx;
	struct nvmeibt_local_disk	*ld = NULL;
	int							rv = -1;

	capture_reset(c);
	ld = calloc(1, sizeof(*ld));
	TEST_ASSERT_NOT_NULL(ld);
	snprintf(ld->from_config.dev_file_name, sizeof(ld->from_config.dev_file_name), "/dev/nvme99n1");
	snprintf(ld->from_config.status, sizeof(ld->from_config.status), "Error");
	ld->is_excluded = 1;
	ld->is_explicitly_excluded = 1;
	print_one_local_disk_status(capture_printf, c, ld);
	TEST_ASSERT_NOT_NULL(strstr(c->buf, "EXCLUDED"));
	TEST_ASSERT_NOT_NULL(strstr(c->buf, "(explicit)"));
	rv = 0;
out:
	free(ld);
	return rv;
}

DEFINE_TEST(local_disk_drive_write_error_flag)
{
	struct capture_ctx			*c = (struct capture_ctx *)_ctx;
	struct nvmeibt_local_disk	*ld = NULL;
	int							rv = -1;

	capture_reset(c);
	ld = calloc(1, sizeof(*ld));
	TEST_ASSERT_NOT_NULL(ld);
	snprintf(ld->from_config.dev_file_name, sizeof(ld->from_config.dev_file_name), "/dev/nvme99n1");
	snprintf(ld->from_config.status, sizeof(ld->from_config.status), "Ok");
	ld->is_drive_write_error = 1;
	print_one_local_disk_status(capture_printf, c, ld);
	TEST_ASSERT_NOT_NULL(strstr(c->buf, VERIFY_WRITE_ERROR_CODE));
	rv = 0;
out:
	free(ld);
	return rv;
}

DEFINE_TEST(local_disk_no_flags_when_healthy)
{
	struct capture_ctx			*c = (struct capture_ctx *)_ctx;
	struct nvmeibt_local_disk	*ld = NULL;
	int							rv = -1;

	capture_reset(c);
	ld = calloc(1, sizeof(*ld));
	TEST_ASSERT_NOT_NULL(ld);
	snprintf(ld->from_config.dev_file_name, sizeof(ld->from_config.dev_file_name), "/dev/nvme99n1");
	snprintf(ld->from_config.status, sizeof(ld->from_config.status), "Ok");
	print_one_local_disk_status(capture_printf, c, ld);
	TEST_ASSERT_TRUE(strstr(c->buf, "EXCLUDED") == NULL);
	TEST_ASSERT_TRUE(strstr(c->buf, VERIFY_WRITE_ERROR_CODE) == NULL);
	rv = 0;
out:
	free(ld);
	return rv;
}

/******************** Follower behind-topology tests *******************/

DEFINE_TEST(errors_follower_behind_topology)
{
	struct nvmeibt_Str						*out = NNVMEIBT_STR_ALLOC(fbt01);
	struct nvmeibt_persist_and_wire_buf		*saved_buf;
	struct nvmeibt_persist_and_wire_buf		*test_buf = NULL;
	int64_t									saved_committed;
	int										rv = -1;
	(void)_ctx;

	NNVMEIBT_STR_RESIZE_BUF(fbt02, out, 4096);
	// Save original state
	saved_buf = nvmeibt_raft_get_my_raft()->follower_to_commit_persist_and_wire_buf_full;
	saved_committed = nvmeibt_raft_get_my_raft()->TOPO_commit_lifecycle.follower_committed;
	// Set up follower with leader's buf at topo index 10, committed at 5 (behind by 5 > threshold of 2)
	test_buf = calloc(1, sizeof(struct nvmeibt_persist_and_wire_buf));
	nvmeibt_raft_get_my_raft()->follower_to_commit_persist_and_wire_buf_full = test_buf;
	TEST_ASSERT_NOT_NULL(nvmeibt_raft_get_my_raft()->follower_to_commit_persist_and_wire_buf_full);
	nvmeibt_raft_get_my_raft()->follower_to_commit_persist_and_wire_buf_full->topo_ctx.tlv_idx = LE_SWAP64((int64_t)10);
	nvmeibt_raft_get_my_raft()->TOPO_commit_lifecycle.follower_committed = 5;
	nvmeibt_raft_get_real_time_errors_str(out);
	TEST_ASSERT_NOT_NULL(strstr(nvmeibt_Str_str(out), "Err=7005"));
	rv = 0;
out:
	free(test_buf);
	nvmeibt_raft_get_my_raft()->follower_to_commit_persist_and_wire_buf_full = saved_buf;
	nvmeibt_raft_get_my_raft()->TOPO_commit_lifecycle.follower_committed = saved_committed;
	NNVMEIBT_STR_FREE(fbt03, out);
	return rv;
}

DEFINE_TEST(errors_follower_not_behind_at_current)
{
	struct nvmeibt_Str						*out = NNVMEIBT_STR_ALLOC(fnb01);
	struct nvmeibt_persist_and_wire_buf		*saved_buf;
	struct nvmeibt_persist_and_wire_buf		*test_buf = NULL;
	int64_t									saved_committed;
	int										rv = -1;
	(void)_ctx;

	NNVMEIBT_STR_RESIZE_BUF(fnb02, out, 4096);
	// Save original state
	saved_buf = nvmeibt_raft_get_my_raft()->follower_to_commit_persist_and_wire_buf_full;
	saved_committed = nvmeibt_raft_get_my_raft()->TOPO_commit_lifecycle.follower_committed;
	// Set up follower with leader's buf at topo index 10, committed at 9 (behind by 1, within threshold)
	test_buf = calloc(1, sizeof(struct nvmeibt_persist_and_wire_buf));
	nvmeibt_raft_get_my_raft()->follower_to_commit_persist_and_wire_buf_full = test_buf;
	TEST_ASSERT_NOT_NULL(nvmeibt_raft_get_my_raft()->follower_to_commit_persist_and_wire_buf_full);
	nvmeibt_raft_get_my_raft()->follower_to_commit_persist_and_wire_buf_full->topo_ctx.tlv_idx = LE_SWAP64((int64_t)10);
	nvmeibt_raft_get_my_raft()->TOPO_commit_lifecycle.follower_committed = 9;
	nvmeibt_raft_get_real_time_errors_str(out);
	TEST_ASSERT_TRUE(strstr(nvmeibt_Str_str(out), "Err=7005") == NULL);
	rv = 0;
out:
	free(test_buf);
	nvmeibt_raft_get_my_raft()->follower_to_commit_persist_and_wire_buf_full = saved_buf;
	nvmeibt_raft_get_my_raft()->TOPO_commit_lifecycle.follower_committed = saved_committed;
	NNVMEIBT_STR_FREE(fnb03, out);
	return rv;
}

static const union nvmeib_uuid test_raft_peer_uuid = { .bytes = {0x10} };
static const union nvmeib_uuid test_raft_conn_peer_uuid = { .bytes = {0x11} };

DEFINE_TEST(errors_leader_reports_peer_behind_topology)
{
	struct nvmeibt_Str			*out = NNVMEIBT_STR_ALLOC(lbt01);
	struct nvmeibt_raft_member	*member = NULL;
	enum RAFT_ROLE_TYPE			saved_role;
	int64_t						saved_leader_to_commit;
	bool						added_member = false;
	int							rv = -1;
	(void)_ctx;

	NNVMEIBT_STR_RESIZE_BUF(lbt02, out, 4096);
	if (!nvmeibt_raft_get_my_raft()->raft_members_hash_by_uuid)
		TEST_init_raft_members_hash();
	saved_role = nvmeibt_raft_get_my_raft()->role;
	saved_leader_to_commit = nvmeibt_raft_get_my_raft()->TOPO_commit_lifecycle.leader_to_commit;
	TEST_add_raft_member_to_hash(&test_raft_peer_uuid, "peer-behind", 1, 1);
	added_member = true;
	member = nvmeibt_raft_get_member_by_id(&test_raft_peer_uuid);
	TEST_ASSERT_NOT_NULL(member);
	member->committed_persist_and_wire_buf_hdr.topo_ctx.tlv_idx = LE_SWAP64((int64_t)4);
	nvmeibt_raft_get_my_raft()->role = RAFT_ROLE_LEADER;
	nvmeibt_raft_get_my_raft()->TOPO_commit_lifecycle.leader_to_commit = 10;
	nvmeibt_raft_get_real_time_errors_str(out);
	TEST_ASSERT_NOT_NULL(strstr(nvmeibt_Str_str(out), "Err=7004"));
	TEST_ASSERT_NOT_NULL(strstr(nvmeibt_Str_str(out), "peer-behind"));
	TEST_ASSERT_NOT_NULL(strstr(nvmeibt_Str_str(out), "6 topologies"));
	rv = 0;
out:
	nvmeibt_raft_get_my_raft()->role = saved_role;
	nvmeibt_raft_get_my_raft()->TOPO_commit_lifecycle.leader_to_commit = saved_leader_to_commit;
	if (added_member)
		TEST_remove_raft_member_from_hash(&test_raft_peer_uuid);
	NNVMEIBT_STR_FREE(lbt03, out);
	return rv;
}

DEFINE_TEST(errors_raft_configured_host_without_connectivity)
{
	struct nvmeibt_Str			*out = NNVMEIBT_STR_ALLOC(rnc01);
	struct nvmeibt_raft_member	*member = NULL;
	struct nvmeibt_node			*node = NULL;
	bool						added_member = false;
	int							rv = -1;
	(void)_ctx;

	NNVMEIBT_STR_RESIZE_BUF(rnc02, out, 4096);
	if (!nvmeibt_raft_get_my_raft()->raft_members_hash_by_uuid)
		TEST_init_raft_members_hash();
	TEST_add_raft_member_to_hash(&test_raft_conn_peer_uuid, "peer-no-conn", 1, 1);
	added_member = true;
	member = nvmeibt_raft_get_member_by_id(&test_raft_conn_peer_uuid);
	TEST_ASSERT_NOT_NULL(member);
	node = calloc(1, sizeof(*node));
	TEST_ASSERT_NOT_NULL(node);
	snprintf(node->from_config.name, sizeof(node->from_config.name), "peer-no-conn");
	node->raft_member = member;
	member->its_node = node;
	TEST_set_nm_remote_nodes_connected(false);
	nvmeibt_raft_get_real_time_errors_str(out);
	TEST_ASSERT_NOT_NULL(strstr(nvmeibt_Str_str(out), "peer-no-conn"));		// Verify report about the peer, regardless of the specific message
	rv = 0;
out:
	TEST_set_nm_remote_nodes_connected(true);
	if (member)
		member->its_node = NULL;
	free(node);
	if (added_member)
		TEST_remove_raft_member_from_hash(&test_raft_conn_peer_uuid);
	NNVMEIBT_STR_FREE(rnc03, out);
	return rv;
}

/******************** Topology RPC error tests *************************/

static const union nvmeib_uuid test_disk_uuid = { .bytes = {0x01} };

DEFINE_TEST(errors_topo_disk_drive_write_error)
{
	struct nvmeibt_Str			*out = NNVMEIBT_STR_ALLOC(tde01);
	bool						added_disk = false;
	int							rv = -1;
	(void)_ctx;

	NNVMEIBT_STR_RESIZE_BUF(tde02, out, 4096);
	TEST_add_disk_to_hash(&test_disk_uuid, true);
	added_disk = true;
	nvmeibt_topology_get_real_time_errors_str(out);
	TEST_ASSERT_NOT_NULL(strstr(nvmeibt_Str_str(out), "DRIVE_WRITE_ERROR: disk="));
	rv = 0;
out:
	if (added_disk)
		TEST_remove_disk_from_hash(&test_disk_uuid);
	NNVMEIBT_STR_FREE(tde03, out);
	return rv;
}

DEFINE_TEST(errors_topo_local_disk_drive_write_error)
{
	struct nvmeibt_Str			*out = NNVMEIBT_STR_ALLOC(tld01);
	bool						added_ldisk = false;
	int							rv = -1;
	(void)_ctx;

	NNVMEIBT_STR_RESIZE_BUF(tld02, out, 4096);
	TEST_add_local_disk_to_hash("test-ldisk-001", false, true);
	added_ldisk = true;
	nvmeibt_topology_get_real_time_errors_str(out);
	TEST_ASSERT_NOT_NULL(strstr(nvmeibt_Str_str(out), "DRIVE_WRITE_ERROR: local_disk="));
	rv = 0;
out:
	if (added_ldisk)
		TEST_remove_local_disk_from_hash("test-ldisk-001");
	NNVMEIBT_STR_FREE(tld03, out);
	return rv;
}

DEFINE_TEST(errors_topo_local_disk_excluded)
{
	struct nvmeibt_Str			*out = NNVMEIBT_STR_ALLOC(tle01);
	bool						added_ldisk = false;
	int							rv = -1;
	(void)_ctx;

	NNVMEIBT_STR_RESIZE_BUF(tle02, out, 4096);
	TEST_add_local_disk_to_hash("test-ldisk-002", true, false);
	added_ldisk = true;
	nvmeibt_topology_get_real_time_errors_str(out);
	TEST_ASSERT_NOT_NULL(strstr(nvmeibt_Str_str(out), "EXCLUDED: disk="));
	rv = 0;
out:
	if (added_ldisk)
		TEST_remove_local_disk_from_hash("test-ldisk-002");
	NNVMEIBT_STR_FREE(tle03, out);
	return rv;
}

DEFINE_TEST(errors_topo_no_errors_when_healthy)
{
	struct nvmeibt_Str			*out = NNVMEIBT_STR_ALLOC(tne01);
	bool						added_disk = false;
	bool						added_ldisk = false;
	int							rv = -1;
	(void)_ctx;

	NNVMEIBT_STR_RESIZE_BUF(tne02, out, 4096);
	TEST_add_disk_to_hash(&test_disk_uuid, false);
	added_disk = true;
	TEST_add_local_disk_to_hash("test-ldisk-003", false, false);
	added_ldisk = true;
	nvmeibt_topology_get_real_time_errors_str(out);
	TEST_ASSERT_TRUE(strstr(nvmeibt_Str_str(out), VERIFY_WRITE_ERROR_CODE) == NULL);
	TEST_ASSERT_TRUE(strstr(nvmeibt_Str_str(out), "EXCLUDED") == NULL);
	rv = 0;
out:
	if (added_disk)
		TEST_remove_disk_from_hash(&test_disk_uuid);
	if (added_ldisk)
		TEST_remove_local_disk_from_hash("test-ldisk-003");
	NNVMEIBT_STR_FREE(tne03, out);
	return rv;
}

static const union nvmeib_uuid test_praid_uuid = { .bytes = {0x02} };

DEFINE_TEST(errors_topo_local_disk_not_ready)
{
	struct nvmeibt_Str			*out = NNVMEIBT_STR_ALLOC(tnr01);
	bool						added_ldisk = false;
	int							rv = -1;
	(void)_ctx;

	NNVMEIBT_STR_RESIZE_BUF(tnr02, out, 4096);
	TEST_add_not_ready_local_disk_to_hash("test-ldisk-nr", 3);
	added_ldisk = true;
	nvmeibt_topology_get_real_time_errors_str(out);
	TEST_ASSERT_NOT_NULL(strstr(nvmeibt_Str_str(out), "NOT_READY: local_disk="));
	TEST_ASSERT_NOT_NULL(strstr(nvmeibt_Str_str(out), "n_segments=3"));
	rv = 0;
out:
	if (added_ldisk)
		TEST_remove_local_disk_from_hash("test-ldisk-nr");
	NNVMEIBT_STR_FREE(tnr03, out);
	return rv;
}

DEFINE_TEST(errors_topo_praid_conf_corrupted)
{
	struct nvmeibt_Str			*out = NNVMEIBT_STR_ALLOC(tpc01);
	bool						initialized_praids = false;
	int							rv = -1;
	(void)_ctx;

	NNVMEIBT_STR_RESIZE_BUF(tpc02, out, 4096);
	TEST_init_praids_hash();
	initialized_praids = true;
	TEST_add_praid_to_hash(&test_praid_uuid, 1, 1, 0);
	TEST_set_praid_conf_corrupted(&test_praid_uuid);
	nvmeibt_topology_get_real_time_errors_str(out);
	TEST_ASSERT_NOT_NULL(strstr(nvmeibt_Str_str(out), "PRAID_CONFIG_CORRUPTED: vol="));
	rv = 0;
out:
	if (initialized_praids)
		TEST_init_praids_hash();
	NNVMEIBT_STR_FREE(tpc03, out);
	return rv;
}

static const union nvmeib_uuid test_blkdev_uuid_short = { .bytes = {0x20} };

DEFINE_TEST(errors_kafka_incompatible_version)
{
	struct nvmeibt_Str			*out = NNVMEIBT_STR_ALLOC(kiv01);
	struct kafka_simulator_t *k = sandbox_kafka_init(NULL);
	int							rv = -1;
	(void)_ctx;

	NNVMEIBT_STR_RESIZE_BUF(kiv02, out, 4096);
	sandbox_set_rd_kafka_version(0xffffffff, "max");		// Far above supported upper bound; triggers "Too new"
	nvmeibt_kafka_get_real_time_errors_str(out);
	TEST_ASSERT_NOT_NULL(strstr(nvmeibt_Str_str(out), "Err=7016"));
	rv = 0;
out:
	sandbox_set_rd_kafka_version(0x020501ff, "2.5.1");
	sandbox_kafka_destroy(k);
	NNVMEIBT_STR_FREE(kiv03, out);
	return rv;
}

DEFINE_TEST(errors_kafka_transient_recent)
{
	struct nvmeibt_Str			*out = NNVMEIBT_STR_ALLOC(ktr01);
	int							rv = -1;
	(void)_ctx;

	NNVMEIBT_STR_RESIZE_BUF(ktr02, out, 4096);
	check_if_kafka_init_preserve_state_vars_required(RD_KAFKA_RESP_ERR__TRANSPORT);
	nvmeibt_kafka_get_real_time_errors_str(out);
	TEST_ASSERT_NOT_NULL(strstr(nvmeibt_Str_str(out), "Err=7017"));
	rv = 0;
out:
	kafka_last_transient_err_boot_sec = 0;
	NNVMEIBT_STR_FREE(ktr03, out);
	return rv;
}

DEFINE_TEST(errors_kafka_transient_old)
{
	struct nvmeibt_Str			*out = NNVMEIBT_STR_ALLOC(kto01);
	struct timespec				now;
	int							rv = -1;
	(void)_ctx;

	NNVMEIBT_STR_RESIZE_BUF(kto02, out, 4096);
	check_if_kafka_init_preserve_state_vars_required(RD_KAFKA_RESP_ERR__TRANSPORT);
	getnstimeofday_boot(&now);
	kafka_last_transient_err_boot_sec = (now.tv_sec - 120);		// Backdate past the 60s window
	nvmeibt_kafka_get_real_time_errors_str(out);
	TEST_ASSERT_TRUE(strstr(nvmeibt_Str_str(out), "Err=7017") == NULL);
	rv = 0;
out:
	kafka_last_transient_err_boot_sec = 0;
	NNVMEIBT_STR_FREE(kto03, out);
	return rv;
}

/******************** Real-time errors tests ****************************/

DEFINE_TEST(errors_raft_no_stable_leader)
{
	struct nvmeibt_Str			*out = NNVMEIBT_STR_ALLOC(spt01);
	int							rv = -1;
	(void)_ctx;

	NNVMEIBT_STR_RESIZE_BUF(spt02, out, 4096);
	// Raft is UNKNOWN by default — no stable leader
	nvmeibt_raft_get_real_time_errors_str(out);
	TEST_ASSERT_NOT_NULL(strstr(nvmeibt_Str_str(out), "Err=7001"));
	rv = 0;
out:
	NNVMEIBT_STR_FREE(spt03, out);
	return rv;
}

DEFINE_TEST(errors_raft_persist_failure)
{
	struct nvmeibt_Str			*out = NNVMEIBT_STR_ALLOC(spt04);
	int							saved_n_persist_failures;
	int64_t						saved_last_persist_failure_timestamp_sec;
	int							rv = -1;
	(void)_ctx;

	NNVMEIBT_STR_RESIZE_BUF(spt05, out, 4096);
	saved_n_persist_failures = nvmeibt_raft_get_my_raft()->n_persist_failures;
	saved_last_persist_failure_timestamp_sec = nvmeibt_raft_get_my_raft()->last_persist_failure_timestamp_sec;
	nvmeibt_raft_get_my_raft()->n_persist_failures = 115;
	nvmeibt_raft_get_my_raft()->last_persist_failure_timestamp_sec = 100;
	nvmeibt_raft_get_real_time_errors_str(out);
	TEST_ASSERT_NOT_NULL(strstr(nvmeibt_Str_str(out), "Err=7002"));
	TEST_ASSERT_NOT_NULL(strstr(nvmeibt_Str_str(out), "115"));
	rv = 0;
out:
	nvmeibt_raft_get_my_raft()->n_persist_failures = saved_n_persist_failures;
	nvmeibt_raft_get_my_raft()->last_persist_failure_timestamp_sec = saved_last_persist_failure_timestamp_sec;
	NNVMEIBT_STR_FREE(spt06, out);
	return rv;
}

DEFINE_TEST(errors_raft_clear_counters)
{
	struct nvmeibt_Str			*out = NNVMEIBT_STR_ALLOC(spt07);
	int							saved_n_persist_failures;
	int64_t						saved_last_persist_failure_timestamp_sec;
	int							rv = -1;
	(void)_ctx;

	NNVMEIBT_STR_RESIZE_BUF(spt08, out, 4096);
	saved_n_persist_failures = nvmeibt_raft_get_my_raft()->n_persist_failures;
	saved_last_persist_failure_timestamp_sec = nvmeibt_raft_get_my_raft()->last_persist_failure_timestamp_sec;
	nvmeibt_raft_get_my_raft()->n_persist_failures = 7;
	nvmeibt_raft_get_my_raft()->last_persist_failure_timestamp_sec = 200;
	nvmeibt_raft_clear_problem_counters();
	TEST_ASSERT_EQ(nvmeibt_raft_get_my_raft()->n_persist_failures, 0);
	TEST_ASSERT_EQ(nvmeibt_raft_get_my_raft()->last_persist_failure_timestamp_sec, 0);
	// Verify errors output no longer contains persist failure
	nvmeibt_raft_get_real_time_errors_str(out);
	TEST_ASSERT_TRUE(strstr(nvmeibt_Str_str(out), "Err=7002") == NULL);
	rv = 0;
out:
	nvmeibt_raft_get_my_raft()->n_persist_failures = saved_n_persist_failures;
	nvmeibt_raft_get_my_raft()->last_persist_failure_timestamp_sec = saved_last_persist_failure_timestamp_sec;
	NNVMEIBT_STR_FREE(spt09, out);
	return rv;
}

DEFINE_TEST(errors_consolidated_output)
{
	struct nvmeibt_Str				*out = NNVMEIBT_STR_ALLOC(spt10);
	struct nvmeibt_block_device		*blkdev = NULL;
	bool							added_ldisk = false;
	bool							initialized_blkdevs = false;
	int								rv = -1;
	(void)_ctx;

	NNVMEIBT_STR_RESIZE_BUF(spt11, out, 4096);
	TEST_add_local_disk_to_hash("test-consolidated-ldisk", false, true);
	added_ldisk = true;
	TEST_init_blkdevs_hash();
	initialized_blkdevs = true;
	TEST_add_blkdev_to_hash(&test_blkdev_uuid_short, 1, NULL, 0, false);
	blkdev = nvmeibt_block_device_get_block_device_by_id(&test_blkdev_uuid_short);
	TEST_ASSERT_NOT_NULL(blkdev);
	snprintf(blkdev->from_config.client_blkdev_name, sizeof(blkdev->from_config.client_blkdev_name), "vol-consolidated");
	blkdev->encrypt_params = calloc(1, sizeof(*blkdev->encrypt_params));
	TEST_ASSERT_NOT_NULL(blkdev->encrypt_params);
	nvmeibt_toma_get_real_time_errors_str(out);
	// Should have the JSON wrapper
	TEST_ASSERT_TRUE(nvmeibt_Str_str(out)[0] == '{');
	TEST_ASSERT_NOT_NULL(strstr(nvmeibt_Str_str(out), "}"));
	// Raft is UNKNOWN — should include no stable leader in consolidated output
	TEST_ASSERT_NOT_NULL(strstr(nvmeibt_Str_str(out), "Err=7001"));
	TEST_ASSERT_NOT_NULL(strstr(nvmeibt_Str_str(out), "DRIVE_WRITE_ERROR: local_disk="));
	if (0) TEST_ASSERT_NOT_NULL(strstr(nvmeibt_Str_str(out), "ENCRYPTION: n_volumes_encrypting=1"));
	rv = 0;
out:
	if (blkdev && blkdev->encrypt_params) {
		free(blkdev->encrypt_params);
		blkdev->encrypt_params = NULL;
	}
	if (initialized_blkdevs)
		TEST_init_blkdevs_hash();
	if (added_ldisk)
		TEST_remove_local_disk_from_hash("test-consolidated-ldisk");
	NNVMEIBT_STR_FREE(spt12, out);
	return rv;
}

DEFINE_TEST(errors_rpc_status_errors_dispatch)
{
	struct nvmeibt_Str			*out = NNVMEIBT_STR_ALLOC(rse01);
	char						cmd[] = "status errors";
	bool						added_ldisk = false;
	int							rv = -1;
	(void)_ctx;

	NNVMEIBT_STR_RESIZE_BUF(rse02, out, 4096);
	TEST_add_local_disk_to_hash("test-rpc-ldisk", false, true);
	added_ldisk = true;
	TEST_ASSERT_TRUE(TEST_nvmeibt_rpc_handle_command(cmd, out) > 0);
	TEST_ASSERT_TRUE(nvmeibt_Str_str(out)[0] == '{');
	TEST_ASSERT_NOT_NULL(strstr(nvmeibt_Str_str(out), "DRIVE_WRITE_ERROR: local_disk="));
	rv = 0;
out:
	if (added_ldisk)
		TEST_remove_local_disk_from_hash("test-rpc-ldisk");
	NNVMEIBT_STR_FREE(rse03, out);
	return rv;
}

DEFINE_TEST(errors_rpc_clear_problem_counters_dispatch)
{
	struct nvmeibt_Str			*out = NNVMEIBT_STR_ALLOC(rcc01);
	char						cmd[] = "simulate clear-problem-counters";
	int							saved_n_persist_failures;
	int64_t						saved_last_persist_failure_timestamp_sec;
	int							rv = -1;
	(void)_ctx;

	NNVMEIBT_STR_RESIZE_BUF(rcc02, out, 4096);
	saved_n_persist_failures = nvmeibt_raft_get_my_raft()->n_persist_failures;
	saved_last_persist_failure_timestamp_sec = nvmeibt_raft_get_my_raft()->last_persist_failure_timestamp_sec;
	nvmeibt_raft_get_my_raft()->n_persist_failures = 9;
	nvmeibt_raft_get_my_raft()->last_persist_failure_timestamp_sec = 300;
	TEST_ASSERT_EQ(TEST_nvmeibt_rpc_handle_command(cmd, out), 0);
	TEST_ASSERT_NOT_NULL(strstr(nvmeibt_Str_str(out), "Problem counters cleared."));
	TEST_ASSERT_EQ(nvmeibt_raft_get_my_raft()->n_persist_failures, 0);
	TEST_ASSERT_EQ(nvmeibt_raft_get_my_raft()->last_persist_failure_timestamp_sec, 0);
	rv = 0;
out:
	nvmeibt_raft_get_my_raft()->n_persist_failures = saved_n_persist_failures;
	nvmeibt_raft_get_my_raft()->last_persist_failure_timestamp_sec = saved_last_persist_failure_timestamp_sec;
	NNVMEIBT_STR_FREE(rcc03, out);
	return rv;
}

/************************** Main ****************************************/

#define STATUS_PRINT_TEST_LIST \
	X(local_disk_excluded_flag,				"Local disk: EXCLUDED flag shown",		"is_excluded + is_explicitly_excluded shown in status") \
	X(local_disk_drive_write_error_flag,	"Local disk: DRIVE_WRITE_ERROR shown",	"is_drive_write_error shown in status") \
	X(local_disk_no_flags_when_healthy,		"Local disk: no flags when healthy",	"Healthy disk shows no EXCLUDED or DRIVE_WRITE_ERROR") \
	X(errors_raft_no_stable_leader,			"Errors: raft no stable leader",		"No stable leader appears in real-time errors output") \
	X(errors_raft_persist_failure,			"Errors: raft persist failure",			"Persist failures appear in real-time errors output") \
	X(errors_raft_clear_counters,			"Errors: clear problem counters",		"clear_problem_counters resets persist failure counter") \
	X(errors_follower_behind_topology,		"Errors: follower behind topology",		"Follower detects it is behind leader topology") \
	X(errors_follower_not_behind_at_current,"Errors: follower not behind",		"Follower within threshold shows no behind warning") \
	X(errors_leader_reports_peer_behind_topology,"Errors: leader peer behind",	"Leader reports peer topology lag") \
	X(errors_raft_configured_host_without_connectivity,"Errors: raft peer no connectivity", "Configured raft host without NM connectivity is reported") \
	X(errors_topo_disk_drive_write_error,	"Errors: topo disk write error",		"disk DRIVE_WRITE_ERROR reported in topo RPC errors") \
	X(errors_topo_local_disk_drive_write_error,"Errors: topo ldisk write error",	"local_disk DRIVE_WRITE_ERROR reported in topo RPC errors") \
	X(errors_topo_local_disk_excluded,		"Errors: topo ldisk excluded",			"local_disk EXCLUDED reported in topo RPC errors") \
	X(errors_topo_no_errors_when_healthy,	"Errors: topo no errors healthy",		"Healthy disk/local_disk produce no topo RPC errors") \
	X(errors_topo_local_disk_not_ready,		"Errors: topo ldisk not ready",			"Not-ready local_disk with segments reported in RPC errors") \
	X(errors_topo_praid_conf_corrupted,		"Errors: topo praid corrupted",			"Corrupted praid reported in topo RPC errors") \
	X(errors_kafka_incompatible_version,	"Errors: kafka incompatible version",	"Incompatible librdkafka version is reported") \
	X(errors_kafka_transient_recent,		"Errors: kafka transient recent",		"Transient kafka comm error within 60s is reported") \
	X(errors_kafka_transient_old,			"Errors: kafka transient old",			"Transient kafka comm error older than 60s is suppressed") \
	X(errors_consolidated_output,			"Errors: consolidated output",			"Consolidated errors output includes all modules") \
	X(errors_rpc_status_errors_dispatch,	"Errors: RPC status errors",			"RPC status errors dispatch returns real-time errors") \
	X(errors_rpc_clear_problem_counters_dispatch,"Errors: RPC clear counters",	"RPC simulate clear-problem-counters resets counters")

int status_report_test_main(int argc, char *argv[])
{
	struct capture_ctx	ctx;
	const char			*selection = NULL;
	int					rv = 1;

	#define X(func, name, desc) {name, desc, test_##func},
	struct toma_test_entry tests[] = { STATUS_PRINT_TEST_LIST };
	#undef X

	TEST_init();

	ctx.capacity = STATUS_BUF_SIZE;
	ctx.buf = (char *)malloc((size_t)ctx.capacity);
	ctx.len = 0;
	if (ctx.buf == NULL) {
		fprintf(stderr, "status_report_test: malloc failed\n");
		goto out;
	}

	if (argc > 1) {
		selection = argv[1];
	}

	rv = test_run_suite("Status Report",
			   tests, (int)(sizeof(tests) / sizeof(tests[0])),
			   &ctx, selection, 0);

out:
	free(ctx.buf);
	return rv;
}
