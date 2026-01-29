/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <corosync/corotypes.h>
#include <corosync/votequorum.h>
#include <stdbool.h>

/************************************* utils *********************************/
static int __get_timestamp_str(char rv[32], const uint32_t unix_epoch_sec) {
	const time_t nowtime = unix_epoch_sec;
	struct tm tmInfo;
	localtime_r(&nowtime, &tmInfo);
	return strftime(rv, 32, "%Y.%m.%d-%H:%M:%S", &tmInfo);
}

static int __get_cur_timestamp_str(char rv[32]) {
	struct timeval tv;
	gettimeofday(&tv, NULL);
	int n_bytes = __get_timestamp_str(rv, tv.tv_sec);
	n_bytes += snprintf(&rv[n_bytes], (32-n_bytes), ".%06lu", tv.tv_usec);
	return n_bytes;
}

#define pr_info(fmt, ...) ({ __get_cur_timestamp_str(arbiter._ts); printf(         "[%s]0x%lx-%s: " fmt, arbiter._ts, ##__VA_ARGS__); })
#define pr_err( fmt, ...) ({ __get_cur_timestamp_str(arbiter._ts); fprintf(stderr, "[%s]0x%lx-%s: " fmt, arbiter._ts, arbiter.handle, arbiter.name, ##__VA_ARGS__); })

static const char *node_state(int state) {
	switch (state) {
		case VOTEQUORUM_NODESTATE_MEMBER: 	return "Member";
		case VOTEQUORUM_NODESTATE_DEAD: 	return "Dead";
		case VOTEQUORUM_NODESTATE_LEAVING:	return "Leaving";
		default:							return "UNKNOWN";
	}
}

/******************************* arbiter logic *******************************/
static struct arbiter_t {
	const char *name;
	votequorum_handle_t handle;
	votequorum_ring_id_t last_received_ring_id;
	votequorum_callbacks_t callbacks;	// Can use quorum_model_v1_data_t callback to track quorum nodes delta's via nodelist_notify_fn;
	char _ts[32];						// Time stamp of last print
	enum init_state_t { AINIT_NONE, AINIT_INIT, AINIT_TRACKED, AINIT_REGISTERED, } init_state;
} arbiter = {"Toma_Raft", 0ULL, {0, 0ULL}, {NULL, NULL, NULL}, {0}, AINIT_NONE};

static bool __is_connected_with_raft_leader(uint64_t debug_gen) {
	#define TOMA_FILE "/var/log/nvmesh/toma_leader_name"
	char leader_host_name[64 + 1]; // +1 for null terminator
	FILE *file = fopen(TOMA_FILE, "r");
	int vote_majority = -EINVAL;			// 1 = Yes, 0 = No, <0 = Error
	if (file) {
		if (fgets(leader_host_name, sizeof(leader_host_name), file) != NULL) {
			const bool leader_host_exists = (leader_host_name[0] != 0);
			pr_info("toma_leader=%s\n", arbiter.handle, arbiter.name, leader_host_exists ? leader_host_name : "???");
			vote_majority = leader_host_exists;
		} else { /* pr_info("Error reading file"); */ }
		fclose(file);
	} else { /* pr_info("Cant open toma_file"); */ }

	if (vote_majority < 0) {
		// pr_info("Cannot access toma=%s\n", arbiter.handle, arbiter.name, TOMA_FILE);
		vote_majority = (debug_gen % 3);		// Just for test
	}
	return vote_majority;
}

static int print_cur_quorum_state(const char* reason) {
	struct votequorum_info info;
	int err = votequorum_getinfo(arbiter.handle, VOTEQUORUM_QDEVICE_NODEID, &info);
	if (err != CS_OK) {
		pr_err("votequorum_getinfo error %d\n", err);
		return -1;
	}
	pr_info("LOG%s, local_node{" CS_PRI_NODE_ID "=%s,votes=%02u/%02u} quorum=%u+, my_votes=%u, ", arbiter.handle, info.qdevice_name, reason, info.node_id, node_state(info.node_state), info.node_votes, info.total_votes, info.quorum, info.qdevice_votes);
	printf("\tcurrent flags=0x%.4x ={", info.flags);
	if (info.flags & VOTEQUORUM_INFO_QDEVICE_REGISTERED) { 	printf("has_arbiter, ");
		if (info.flags & VOTEQUORUM_INFO_QDEVICE_CAST_VOTE) printf("vote-yes, "); else printf("vote-no , ");
	} else													printf("no_arbiter, ");
	if (info.flags & VOTEQUORUM_INFO_QDEVICE_ALIVE) 		printf("alive, "); else printf("dead , ");
	if (info.flags & VOTEQUORUM_INFO_QDEVICE_MASTER_WINS)  printf("master-wins, ");
	if (info.flags & VOTEQUORUM_INFO_QUORATE) printf("has_quorum, "); else printf(" no_quorum, ");
	printf("}\n");
	return 0;
}

static void votequorum_nodelist_notification_fn(votequorum_handle_t vqh, uint64_t context, votequorum_ring_id_t ring_id, uint32_t n_nodes, uint32_t node_list[]) {
	uint32_t i;
	pr_info("\tnode_cb, cur_ringid =(" CS_PRI_RING_ID "), n_nodes=%u {", vqh, arbiter.name, ring_id.nodeid, ring_id.seq, n_nodes);
	for (i=0; i<n_nodes; i++)
		printf("%u, ", node_list[i]);
	printf("}\n");
	memcpy(&arbiter.last_received_ring_id, &ring_id, sizeof(ring_id));
}

static void votequorum_quorum_notification_fn(votequorum_handle_t vqh, uint64_t context,
	uint32_t quorate, uint32_t n_nodes, votequorum_node_t node_list[]) {
	uint32_t i;
	pr_info("\tquor_cb, has_quorum=%u n_nodes=%u {", vqh, arbiter.name, quorate, n_nodes);
	for (i=0; i<n_nodes; i++)
		printf(CS_PRI_NODE_ID "=%s, " , node_list[i].nodeid, node_state(node_list[i].state));
	printf("}\n");
}

static void votequorum_expectedvotes_notification_fn(votequorum_handle_t vqh, uint64_t context, uint32_t expected_votes) {
	pr_info("change_my_votes=%u\n", vqh, arbiter.name, expected_votes);
}

static void arbiter_t_destroy(struct arbiter_t* a) {
	cs_error_t err;
	if (a->init_state >= AINIT_REGISTERED) {
		print_cur_quorum_state("finish__");
		if ((err = votequorum_qdevice_unregister(a->handle, a->name)) != CS_OK) {
			pr_err("qdevice unregister FAILED: %d\n", err);
		}
	}
	if (a->init_state >= AINIT_TRACKED)
		if ((err = votequorum_trackstop(a->handle)) != CS_OK) {
			pr_err("qdevice unregister FAILED: %d\n", err);
		}
	if (a->init_state >= AINIT_INIT) {
		print_cur_quorum_state("done____");
		if ((err = votequorum_finalize(a->handle)) != CS_OK) {
			pr_err("votequorum_finalize FAILED: %d\n", err);
		}
	}
	a->init_state = AINIT_NONE;
}

static int arbiter_t_init(struct arbiter_t* a) {
	int rv = 0;
	cs_error_t err;
	a->init_state = AINIT_NONE;
	a->callbacks.votequorum_nodelist_notify_fn = votequorum_nodelist_notification_fn;
	a->callbacks.votequorum_expectedvotes_notify_fn = votequorum_expectedvotes_notification_fn;
	a->callbacks.votequorum_quorum_notify_fn = votequorum_quorum_notification_fn;
	if ((err = votequorum_initialize(&a->handle, &a->callbacks)) != CS_OK) {
		pr_err("votequorum_initialize FAILED: %d\n", err); rv = -__LINE__; goto _out;
	}
	a->init_state = AINIT_INIT;
	if ((err = votequorum_context_set(a->handle, (void*)&arbiter)) != CS_OK) {
		pr_err("votequorum_context_set FAILED: %d\n", err); rv = -__LINE__; goto _out;
	}
	if ((err = votequorum_trackstart(a->handle, a->handle, CS_TRACK_CHANGES)) != CS_OK) {
		pr_err("votequorum_trackstart FAILED: %d\n", err); rv = -__LINE__; goto _out;
	}
	a->init_state = AINIT_TRACKED;
	if ((err = votequorum_qdevice_register(a->handle, a->name)) != CS_OK) {
		pr_err("qdevice_register FAILED: %d\n", err); rv = -__LINE__; goto _out;
	}
	a->init_state = AINIT_REGISTERED;
	if ((err = votequorum_qdevice_master_wins(a->handle, a->name, false)) != CS_OK) {
		pr_err("qdevice_master_wins FAILED: %d\n", err); rv = -__LINE__; goto _out;
	}
	print_cur_quorum_state("RaftInit");
_out:
	if (rv)
		arbiter_t_destroy(a);
	return rv;
}

int main(int argc, char *argv[]) {
	int n_poll_count=15;			// Number of times to poll qdevice (default 0=infinte)
	int poll_msec=500;			// Time [msec] to wait between arbiter casting vote according to raft
	int rv = arbiter_t_init(&arbiter);
	if (rv)
		return rv;

	for ( ; --n_poll_count; usleep(poll_msec*1000)) {
		const bool connected_with_raft_leader = __is_connected_with_raft_leader(n_poll_count);
		const char* reason = (connected_with_raft_leader ? "raft_YES" : "raft_NO_");
		cs_error_t err;
		if (votequorum_dispatch(arbiter.handle, CS_DISPATCH_ALL) != CS_OK) {	// Not CS_DISPATCH_BLOCKING
			pr_err("votequorum_dispatch error\n");
			rv = -1; goto _out;
		}
		err = votequorum_qdevice_poll(arbiter.handle, arbiter.name, connected_with_raft_leader, arbiter.last_received_ring_id);
		if (err == CS_ERR_MESSAGE_ERROR) {
			pr_err("qdevice poll passed OLD ring_id\n");
		} else if (err != CS_OK) {
			pr_err("qdevice poll FAILED: %d\n", err);
			rv = -1; goto _out;
		}
		print_cur_quorum_state(reason);
	}
_out:
	votequorum_qdevice_poll(arbiter.handle, arbiter.name, false, arbiter.last_received_ring_id);	// No more quorum
	print_cur_quorum_state("stopping");
	usleep(poll_msec*1000);
	votequorum_dispatch(arbiter.handle, CS_DISPATCH_ALL);				// Receive callback last time which should say that quorum is lost
	arbiter_t_destroy(&arbiter);
	return rv;
}

#ifdef __cplusplus
}
#endif
