/**
 * realloc_upd_test.h - Unit tests for realloc_and_upd two-pass merge
 *
 * Tests realloc_and_upd_follower_persist_and_wire_bufs_with_incoming_data
 * via TEST_realloc_and_upd_follower_persist_and_wire_bufs.
 *
 * Invoked via: ./nvmeibt_toma realloc_upd_test [selection]
 */

#ifndef REALLOC_UPD_TEST_H
#define REALLOC_UPD_TEST_H

#define REALLOC_UPD_TEST_LIST \
	X(first_update_with_raft_log,        "First update with raft log",        "old=NULL, raft_log=true => full memcpy of upd") \
	X(first_update_without_raft_log,     "First update without raft log",     "old=NULL, raft_log=false => only raft_ctx copied") \
	X(equal_bufs_only_raft_ctx_updated,  "Equal bufs updates raft_ctx only",  "All idx match => dst==old, raft_ctx+sw_ver updated") \
	X(no_raft_log_keeps_old,             "No raft log keeps old",             "is_with_raft_log=false => dst==old regardless of idx diff") \
	X(topo_only_same_size_inplace,       "Topo-only same size in-place",      "Only topo idx differs, same size => in-place merge") \
	X(topo_only_diff_size_realloc,       "Topo-only diff size realloc",       "Only topo idx differs, different size => full alloc") \
	X(full_alloc_topo_and_configs,       "Full alloc topo and configs",       "Multiple sections differ => new alloc, old freed") \
	X(error_in_pass1_keeps_old,          "Error in pass1 keeps old buf",      "Bogus TLV type => merge returns -1 => dst==old")

int realloc_upd_test_main(int argc, char *argv[]);

#endif // #ifndef REALLOC_UPD_TEST_H
