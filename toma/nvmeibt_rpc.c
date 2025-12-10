#include "nvmeibt_debug.h"
#include <stdio.h>
#include <stdlib.h>
#include "nvmeibt_rpc.h"
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include "nvmeibt_toma.h"
#include "nvmeibt_topology.h"
#include "nvmeibt_global.h"
#include "nvmeibt_str.h"
#include "nvmeibt_raft.h"
#include "nvmeibt_seg_active.h"
#include <time.h>
#include "toma/interfaces/log/nvmeibt_binary_tracing.h"
#include "./interfaces/srvr/nvmeibt_srvr_proc.h"

#define TOMA_RPC_PATH TOMA_ROOT_DIR "var/log/nvmesh/toma_rpc"
static int rpc_listener_fd = -1;
static int rpc_child_fd = -1;
static struct nvmeibt_Str *rpc_out_str = NULL;

static int nvmeibt_rpc_command_status(int argc, char *argv[], struct nvmeibt_Str *out)
{
	int i;
	const struct status_type {
		char *name;
		enum nvmeibs_toma_status_type type;
	} status_types[] = {						// DHsH: Unify the strings here with toma_stat_proc_fname[] array
			{ "all", NVMEIBS_TOMA_STATUS_ALL },
			{ "raft", NVMEIBS_TOMA_STATUS_RAFT },
			{ "disk_segments", NVMEIBS_TOMA_STATUS_DSEG },
			{ "block_devices", NVMEIBS_TOMA_STATUS_BDEV },
			{ "disks", NVMEIBS_TOMA_STATUS_DISK },
			{ "ib", NVMEIBS_TOMA_STATUS_IB },
			{ "recovery", NVMEIBS_TOMA_STATUS_RECOVER },
			{ "config", NVMEIBS_TOMA_STATUS_CFG },
			{ "topology", NVMEIBS_TOMA_STATUS_TOPO },
			{ "leader", NVMEIBS_TOMA_STATUS_LEADER },
			{ "local_disks", NVMEIBS_TOMA_STATUS_LOCAL_DISKS },
			{ "memory", NVMEIBS_TOMA_STATUS_MEM_ALLOC },
			{ "all.json", NVMEIBS_TOMA_STATUS_ALL_JSON },
			{ "nm_json", NVMEIBS_TOMA_STATUS_NM_JSON },
			{ "kafka",   NVMEIBS_TOMA_STATUS_KAFKA_INFO },
			{ "zeroing", NVMEIBS_TOMA_STATUS_ZEROING },
	};

	if (argc<2) {
		nvmeibt_Str_sprintf(out, "Specify status type. Available options:\n");
		for (i=0; i<ARRAY_SIZE(status_types); i++) {
			nvmeibt_Str_sprintf(out, "    status %s\n", status_types[i].name);
		}
		return -1;
	}
	for (i=0; i<ARRAY_SIZE(status_types); i++) {
		if (strcmp(argv[1], status_types[i].name)==0) {
			return  nvmeibt_toma_get_status_str(status_types[i].type, out);
		}
	}
	nvmeibt_Str_sprintf(out, "Unknown status type. Use 'status' alone to get a list of available options.\n");
	return -1;
}

static int nvmeibt_rpc_command_trace(int argc, char *argv[], struct nvmeibt_Str *out)
{
	(void)argc; (void)argv;
	nvmeibt_Str_sprintf(out, "Num of bytes written to memory so far: %llu\n", nvmeibt_get_total_bytes());
	nvmeibt_Str_sprintf(out, "Num of buffer written to memory(4k): %llu\n", nvmeibt_get_used_bufs());
	return 0;
}

static int nvmeibt_rpc_command_simulate(int argc, char *argv[], struct nvmeibt_Str *out)
{
	struct nvmeibt_topology *cur_topo = nvmeibt_global_get_global();
	struct nvmeibt_local_disk *local_disk;
	extern enum ENCRYPT_DELAY_E nvmeibt_encrypt_delay;

	if (argc<2) {
		nvmeibt_Str_sprintf(out, "Error injection and simulation. Available sub-commands:\n"
				"    remove-all-disks\n"
				"    rescan-disks\n"
				"    follower-ser pause/resume\n"
				"    mgmt pause/resume\n"
				"    md-write pause/resume\n"
				"    raft pause-in/pause-out/pause-in-out/resume\n"
				"    topo-discard <num> once/permanent\n"
				"    rebuild endless/normal\n"
				"    smart-cnt fail/normal\n"
				"    zeroing fail/normal\n"
				"    client-disconnect brute/normal\n"
				"    stale-rebuild enable/disable\n"
				"    raft-long-msg <appendix len in Kbytes>\n"
				"    scrubbing enable/disable\n"
				"    encrypt-delay before-exec/after-exec/before-detach/before-commit/none\n"
				"    bm-garbage-collect <Optional: min log size 1..31>\n"
				"    dump-clnt-hash <max_num>\n"
				"    resend-praids-report all/<volume_name>\n"
				"    reelect\n");
		return -1;
	}

	if (strcmp("remove-all-disks", argv[1])==0) {
		XHASHTABLE_FOR_EACH_SAFE(local_disk, &cur_topo->local_disks_hash) {
			nvmeibt_Str_sprintf(out, "Removing disk %s (note: serjio_status was %d)...\n", nvmeibt_local_disk_UUID_str(local_disk), local_disk->serjio_status);
			nvmeibt_local_disk_remove_by_ldisk_id_str(nvmeibt_local_disk_UUID_str(local_disk));
		}
		nvmeibt_Str_sprintf(out, "Done removing all disks.\n");
		return 0;
	} else if (strcmp("rescan-disks", argv[1])==0) {
		struct nvmeibt_csv_file_ctx cfg_file = {DISKS_INFO_FILE, NVMEIBT_CSV_TYPE_LOCAL_DISKS};
		nvmeibt_topology_probe_local_hardware(&cfg_file, 1);
		nvmeibt_Str_sprintf(out, "Done scanning for disks.\n");
		XHASHTABLE_FOR_EACH_SAFE(local_disk, &cur_topo->local_disks_hash) {
			nvmeibt_Str_sprintf(out, "Available disk: %s\n", nvmeibt_local_disk_display(local_disk));
			local_disk->serjio_status = NVMEIBS_SERJIO_STATUS_READY;
			nvmeibt_local_disk_mark_segs_post_update_actions_required(local_disk);
		}
		nvmeibt_Str_sprintf(out, "Done rescanning disks. Note: serjio_status was set to READY for all disks - this might not be correct.\n");
		return 0;
	} else if (strcmp("follower-ser", argv[1])==0 && (argc>2)) {
		if (strcmp("pause", argv[2])==0) {
			nvmeibt_global_set_is_serialize_active_topo_for_leader(false);
			nvmeibt_Str_sprintf(out, "Follower re-serialization paused. Local changes will not be reported to the leader.\n");
			return 0;
		} else if (strcmp("resume", argv[2])==0) {
			nvmeibt_global_set_is_serialize_active_topo_for_leader(true);
			nvmeibt_Str_sprintf(out, "Follower re-serialization resumed.\n");
			return 0;
		}
		nvmeibt_Str_sprintf(out, "Unknown operation, use '%s pause/resume'.\n", argv[1]);
		return -1;
	} else if (strcmp("mgmt", argv[1])==0 && (argc>2)) {
		if (strcmp("pause", argv[2])==0) {
			nvmeibt_topology_set_mgmt_updates_pause_state(1);
			nvmeibt_Str_sprintf(out, "Management updates paused.\n");
			return 0;
		} else if (strcmp("resume", argv[2])==0) {
			nvmeibt_topology_set_mgmt_updates_pause_state(0);
			nvmeibt_Str_sprintf(out, "Management updates resumed.\n");
			return 0;
		}
		nvmeibt_Str_sprintf(out, "Unknown operation, use '%s pause/resume'.\n", argv[1]);
		return -1;
	} else if (strcmp("md-write", argv[1])==0 && argc>2) {
		if (strcmp("pause", argv[2])==0) {
			nvmeibt_ds_metadata_set_error_injection_md_write_pause(1);
			nvmeibt_Str_sprintf(out, "Metadata writes paused.\n");
			return 0;
		} else if (strcmp("resume", argv[2])==0) {
			nvmeibt_ds_metadata_set_error_injection_md_write_pause(0);
			nvmeibt_Str_sprintf(out, "Metadata writes resumed.\n");
			return 0;
		}
		nvmeibt_Str_sprintf(out, "Unknown operation, use '%s pause/resume'.\n", argv[1]);
		return -1;
	} else if (strcmp("raft", argv[1])==0 && argc>2) {
		if (strcmp("pause-in", argv[2])==0) {
			nvmeibt_global_set_raft_pause_mode(RAFT_IN_PAUSED);
			nvmeibt_Str_sprintf(out, "RAFT processing discard incoming messages.\n");
			return 0;
		} else if (strcmp("pause-out", argv[2])==0) {
			nvmeibt_global_set_raft_pause_mode(RAFT_OUT_PAUSED);
			nvmeibt_Str_sprintf(out, "RAFT processing discard outgoing messages.\n");
			return 0;
		} else if (strcmp("pause-in-out", argv[2])==0) {
			nvmeibt_global_set_raft_pause_mode(RAFT_IN_PAUSED | RAFT_OUT_PAUSED);
			nvmeibt_Str_sprintf(out, "RAFT processing discard all messages.\n");
			return 0;
		} else if (strcmp("resume", argv[2])==0) {
			nvmeibt_global_set_raft_pause_mode(RAFT_NOT_PAUSED);
			nvmeibt_Str_sprintf(out, "RAFT processing resumed.\n");
			return 0;
		}
		nvmeibt_Str_sprintf(out, "Unknown operation, use '%s pause-in/pause-out/pause-in-out/resume'.\n", argv[1]);
		return -1;
	} else if (strcmp("topo-discard", argv[1])==0 && argc>3) {
		bool is_permanent;
		int discard_num;
		if (strcmp("once", argv[3])==0) {
			is_permanent = false;
		} else if (strcmp("permanent", argv[3])==0) {
			is_permanent = true;
		} else {
			nvmeibt_Str_sprintf(out, "Unknown operation, use '%s <num> once/permanent'.\n", argv[1]);
			return -1;
		}
		discard_num = atoi(argv[2]);
		if (discard_num < 0) discard_num = 0;
		nvmeibt_Str_sprintf(out, "Discard command accepted. Num=%d\n", discard_num);
		nvmeibt_raft_set_discard_append_entries(discard_num, is_permanent);
		return 0;
	} else if (strcmp("rebuild", argv[1])==0 && argc>2) {
		if (strcmp("endless", argv[2])==0) {
			nvmeibt_recovery_set_endless_rebuild_state(true);
			nvmeibt_Str_sprintf(out, "REBUILD set to endless.\n");
			return 0;
		} else if (strcmp("normal", argv[2])==0) {
			nvmeibt_recovery_set_endless_rebuild_state(false);
			nvmeibt_Str_sprintf(out, "REBUILD set to normal.\n");
			return 0;
		}
		nvmeibt_Str_sprintf(out, "Unknown operation, use '%s endless/normal'.\n", argv[1]);
		return -1;
	} else if (strcmp("smart-cnt", argv[1])==0 && argc>2) {
		if (strcmp("fail", argv[2])==0) {
			nvmeibt_local_disk_set_read_cnt_test(true);
			nvmeibt_Str_sprintf(out, "Start fail read disk's smart counters.\n");
			return 0;
		} else if (strcmp("normal", argv[2])==0) {
			nvmeibt_local_disk_set_read_cnt_test(false);
			nvmeibt_Str_sprintf(out, "Stop fail read disk's smart counters.\n");
			return 0;
		}
		nvmeibt_Str_sprintf(out, "Unknown operation, use '%s fail/normal'.\n", argv[1]);
		return -1;
	} else if (strcmp("zeroing", argv[1])==0 && argc>2) {
		if (strcmp("fail", argv[2])==0) {
			nvmeibt_seg_active_set_zeroing_test(true);
			nvmeibt_Str_sprintf(out, "Start fail zeroing.\n");
			return 0;
		} else if (strcmp("normal", argv[2])==0) {
			nvmeibt_seg_active_set_zeroing_test(false);
			nvmeibt_Str_sprintf(out, "Stop fail zeroing.\n");
			return 0;
		}
		nvmeibt_Str_sprintf(out, "Unknown operation, use '%s fail/normal'.\n", argv[1]);
		return -1;
	} else if (strcmp("client-disconnect", argv[1])==0 && argc>2) {
		if (strcmp("brute", argv[2])==0) {
			nvmeibt_register_set_brute_force_test(true);
			nvmeibt_Str_sprintf(out, "Start brute force test.\n");
			return 0;
		} else if (strcmp("normal", argv[2])==0) {
			nvmeibt_register_set_brute_force_test(false);
			nvmeibt_Str_sprintf(out, "Stop brute force test.\n");
			return 0;
		}
		nvmeibt_Str_sprintf(out, "Unknown operation, use '%s brute/normal'.\n", argv[1]);
		return -1;
	} else if (strcmp("stale-rebuild", argv[1])==0 && argc>2) {
		if (strcmp("disable", argv[2])==0) {
			nvmeibt_recovery_set_is_stale_rebuild_enabled(0);
			nvmeibt_Str_sprintf(out, "Stale rebuilds disabled.\n");
			return 0;
		} else if (strcmp("enable", argv[2])==0) {
			nvmeibt_recovery_set_is_stale_rebuild_enabled(1);
			nvmeibt_Str_sprintf(out, "Stale rebuilds enabled.\n");
			return 0;
		}
		nvmeibt_Str_sprintf(out, "Unknown operation, use '%s enable|disable'.\n", argv[1]);
		return -1;
	} else if (strcmp("scrubbing", argv[1])==0 && argc>2) {
		if (strcmp("disable", argv[2])==0) {
			nvmeibt_recovery_set_is_scrub_enabled(0);
			nvmeibt_Str_sprintf(out, "Scrubbing disabled.\n");
			return 0;
		} else if (strcmp("enable", argv[2])==0) {
			nvmeibt_recovery_set_is_scrub_enabled(1);
			nvmeibt_Str_sprintf(out, "Scrubbing enabled.\n");
			return 0;
		}
		nvmeibt_Str_sprintf(out, "Unknown operation, use '%s enable|disable'.\n", argv[1]);
		return -1;
	} else if (strcmp("raft-long-msg", argv[1])==0 && argc>2) {
		int appendix_len = atoi(argv[2]);
		if (appendix_len < 0) appendix_len = 0;
		if (appendix_len > (RAFT_LONG_MSG_TEST_TOTAL_STR_MAX_LEN / 1024)) {
			nvmeibt_Str_sprintf(out, "Appendix too long. Max len=%d Kbytes\n", (RAFT_LONG_MSG_TEST_TOTAL_STR_MAX_LEN / 1024));
		} else {
			nvmeibt_topology_set_raft_long_msg_test_appendix_len(appendix_len);
			nvmeibt_Str_sprintf(out, "Appendix length configured. Len=%d Kbytes\n", appendix_len);
		}
		return 0;
	} else if (strcmp("encrypt-delay", argv[1])==0 && argc>2) {
		if (strcmp("before-exec", argv[2])==0)
			nvmeibt_encrypt_delay = ENCRYPT_DELAY_BEFORE_EXECUTION;
		else if (strcmp("after-exec", argv[2])==0)
			nvmeibt_encrypt_delay = ENCRYPT_DELAY_AFTER_EXECUTION;
		else if (strcmp("before-detach", argv[2])==0)
			nvmeibt_encrypt_delay = ENCRYPT_DELAY_BEFORE_DETACH;
		else if (strcmp("before-commit", argv[2])==0)
			nvmeibt_encrypt_delay = ENCRYPT_DELAY_BEFORE_COMMIT;
		else if (strcmp("none", argv[2])==0)
			nvmeibt_encrypt_delay = ENCRYPT_DELAY_NONE;
		else {
			nvmeibt_Str_sprintf(out, "Unknown %s option. 'Use before-exec/after-exec/before-detach/before-commit/none'.\n", argv[1]);
			return -1;
		}
		nvmeibt_Str_sprintf(out, "%s %s accepted", argv[1], argv[2]);
		return 0;
	} else if (strcmp("bm-garbage-collect", argv[1])==0) {
		const int gc_level = (argc > 2) ? atoi(argv[2]) : -1;
		nvmeibt_bm_garbage_collect(gc_level);
		malloc_trim(0);
		nvmeibt_Str_sprintf(out, "Garbage collected level[%d+]\n", gc_level);
		return 0;
	} else if (strcmp("dump-clnt-hash", argv[1])==0) {
		struct nvmeibt_client *cl;
		int i = 0;
		int n_clnts_to_print = (argc>2) ? atoi(argv[2]) : 100;		// Default: print first 100 clients
		nvmeibt_Str_sprintf(out, "n_clients=%d\n", XHASHTABLE_N_ELEMENTS(&nvmeibt_global_get_global()->clients_hash));
		XHASHTABLE_FOR_EACH_SAFE(cl, &nvmeibt_global_get_global()->clients_hash) {
			nvmeibt_Str_sprintf(out, "uuid=%s host=%s disk=%s cid=0x%x, n_reg=%d, con=%d\n", cl->client_provided_urn_uuid.str, cl->net.host_name, cl->ldisk_id.str, cl->cid, cl->n_reg_ctx_refs, cl->is_connected);
			if (i >= n_clnts_to_print)
				return 0;
			i++;
		}
		return 0;
	} else if ((strcmp("resend-praids-report", argv[1])==0) && (argc==3)) {
		if (nvmeibt_raft_is_leader()) {
			if ((strcmp("all", argv[2])==0) || (strcmp("*", argv[2])==0)) {
				nvmeibt_topology_leader_resend_all_praids_report_to_mgmt();
				nvmeibt_Str_sprintf(out, "Resent for all volumes\n");
			} else if (nvmeibt_topology_leader_resend_specific_vol_praids_report_to_mgmt(argv[2])) {
				nvmeibt_Str_sprintf(out, "volume=%s not found.\n", argv[2]);
			} else {
				nvmeibt_Str_sprintf(out, "Resent for volume=%s\n", argv[2]);
			}
		} else {
			nvmeibt_Str_sprintf(out, "Rpc rejected. I'm not a leader.\n");
		}
		return 0;
	} else if (strcmp("reelect", argv[1])==0) {
		nvmeibt_Str_sprintf(out, "Reelection initiated.\n");
		nvmeibt_raft_timeout_occurred(1, 1);
		return 0;
	}
#if 0
	else if (strcmp("mallinfo", argv[1])==0) { // YR: On Ubuntu 22, this is mallinfo2. On RH 8, it is still mallinfo. To avoid wasting time now, disabling. Open it when you want to debug.
		struct mallinfo2 m = mallinfo();
	else if (strcmp("mallinfo2", argv[1])==0) { // YR: On Ubuntu 22, this is mallinfo2. On RH 8, it is still mallinfo. To avoid wasting time now, disabling. Open it when you want to debug.
		struct mallinfo2 m = mallinfo2();
		malloc_stats();
		nvmeibt_Str_sprintf(out, "arena    %15d    /* non-mmapped space allocated from system */\n", m.arena);
		nvmeibt_Str_sprintf(out, "ordblks  %15d    /* number of free chunks */\n", m.ordblks);
		nvmeibt_Str_sprintf(out, "smblks   %15d    /* number of fastbin blocks */\n", m.smblks);
		nvmeibt_Str_sprintf(out, "hblks    %15d    /* number of mmapped regions */\n", m.hblks);
		nvmeibt_Str_sprintf(out, "hblkhd   %15d    /* space in mmapped regions */\n", m.hblkhd);
		nvmeibt_Str_sprintf(out, "usmblks  %15d    /* always 0, preserved for backwards compatibility */\n", m.usmblks);
		nvmeibt_Str_sprintf(out, "fsmblks  %15d    /* space available in freed fastbin blocks */\n", m.fsmblks);
		nvmeibt_Str_sprintf(out, "uordblks %15d    /* total allocated space */\n", m.uordblks);
		nvmeibt_Str_sprintf(out, "fordblks %15d    /* total free space */\n", m.fordblks);
		nvmeibt_Str_sprintf(out, "keepcost %15d    /* top-most, releasable (via malloc_trim) space */\n", m.keepcost);
		return 0;
	}
#endif

	nvmeibt_Str_sprintf(out, "Unknown sub-command. Use 'simulate' alone to get a list of available options.\n");
	return -1;
}

static int nvmeibt_rpc_command_locate(int argc, char *argv[], struct nvmeibt_Str *out)
{
	struct nvmeibt_topology	*cur_topo = nvmeibt_global_get_global();
	if (argc<2) {
		nvmeibt_Str_sprintf(out, "Locate a disk using the attention LED, if available.\n"
				"Note that this feature works only when VMD is enabled in the server BIOS setup.\n"
				"\n"
				"Available sub-commands:\n"
				"    list\n"
				"    disk <disk-id> on\n"
				"    disk <disk-id> slow-blink\n"
				"    disk <disk-id> fast-blink\n"
				"    disk <disk-id> off\n"
				);
		return -1;
	}

	if (strcmp(argv[1], "list")==0) {
		struct nvmeibt_local_disk *local_disk;
		XHASHTABLE_FOR_EACH_SAFE(local_disk, &cur_topo->local_disks_hash) {
			if (local_disk->from_config.pcie_slot[0] == 0 && local_disk->from_config.pcie_bdf[0] == 0)
				continue;
			nvmeibt_Str_sprintf(out, "    PCIe slot: %-8s  Address: %-16s  Disk ID: %-65s \n",
					local_disk->from_config.pcie_slot,
					local_disk->from_config.pcie_bdf,
					nvmeibt_local_disk_display(local_disk));
		}
		nvmeibt_Str_sprintf(out, "End of disk list. Note that VMD must be enabled in the server BIOS for this to work.\n");
		return 0;
	} else if (strcmp(argv[1], "disk")==0) {
		struct nvmeibt_local_disk *local_disk;
		int color = -1;
		if (argc<4) {
			nvmeibt_Str_sprintf(out, "Specify disk ID to locate and desired mode, e.g. 'locate disk S3HCNX0K600340.1 on'.\n");
			return -1;
		}
		if 		(strcmp(argv[3], "on")==0) 			color=13;
		else if (strcmp(argv[3], "fast-blink")==0)	color=7;
		else if (strcmp(argv[3], "slow-blink")==0)	color=5;
		else if (strcmp(argv[3], "off")==0)			color=15;
		else {
			nvmeibt_Str_sprintf(out, "Unknown mode, specify on / fast-blink / slow-blink / off.\n");
			return -1;
		}

		XHASHTABLE_FOR_EACH_SAFE(local_disk, &cur_topo->local_disks_hash) {
			if (local_disk->from_config.pcie_slot[0] == 0)
				continue;
			if (strcmp(argv[2], nvmeibt_local_disk_UUID_str(local_disk)) == 0) {
				nvmeibt_local_disk_util_set_attention_LED(local_disk->from_config.pcie_slot, color);
				nvmeibt_Str_sprintf(out, "Success, slot %s set to attention level %d.\n", local_disk->from_config.pcie_slot, color);
				return 0;
			}
		}
		nvmeibt_Str_sprintf(out, "Disk slot not found, use 'locate list' for a list of disk IDs.\n");
		return -1;
	}
	nvmeibt_Str_sprintf(out, "Unknown sub-command. Use 'locate' alone to get a list of available options.\n");
	return -1;
}

static void persist_params_in_file(void)
{
	struct nvmeibt_Str *s = NNVMEIBT_STR_ALLOC(nvmeibt_disk_flow_params_trace_1);
	extern char *config_params_full_path;
	extern bool trace_config_updated_by_toma;
	int fd;

	nvmeibt_debug_config_params_print(s, true, false);
	nvmeibt_disk_flow_params_print(s);

	fd = open(config_params_full_path, O_RDWR | O_TRUNC | O_CREAT, 0644);
	if (fd>=0) {
		_Str_fwrite(s, fd);
		close(fd);
	}
	trace_config_updated_by_toma = true;
	NNVMEIBT_STR_FREE(nvmeibt_disk_flow_params_trace_2, s);
}

static void _print_flow_params(const struct nvmeibt_disk_flow_params_t *params, struct nvmeibt_Str *out)
{
	nvmeibt_Str_sprintf(out, "   -- is_zeroing_using_test_and_write      %s\n", params->is_zeroing_using_test_and_write 	? "on" : "off");
	nvmeibt_Str_sprintf(out, "   -- is_using_nvme_trim_before_zero       %s\n", params->is_using_nvme_trim_before_zero 		? "on" : "off");
	nvmeibt_Str_sprintf(out, "   -- is_secure_erase_after_disk_format    %s\n", params->is_secure_erase_after_disk_format 	? "on" : "off");
	nvmeibt_Str_sprintf(out, "   -- is_zeroing_mandatory                 %s\n", params->is_zeroing_mandatory 				? "on" : "off");
	nvmeibt_Str_sprintf(out, "   -- delete_ns_when_formatting            %s\n", params->delete_ns_when_formatting 			? "on" : "off");
	nvmeibt_Str_sprintf(out, "   -- reset_after_format                   %s\n", params->reset_after_format		 			? "on" : "off");
	nvmeibt_Str_sprintf(out, "   -- skip_reformat                        %s\n", params->skip_reformat						? "on" : "off");
	nvmeibt_Str_sprintf(out, "   -- ignore_metadata                      %s\n", params->ignore_metadata						? "on" : "off");
	nvmeibt_Str_sprintf(out, "   -- force_metadata                       %s\n", params->force_metadata						? "on" : "off");
	nvmeibt_Str_sprintf(out, "   -- force_512b                           %s\n", params->force_512b							? "on" : "off");
}

static u32 *_get_param_ptr(struct nvmeibt_disk_flow_params_t *params, char *pname)
{
	if (strcmp(pname,  "is_zeroing_using_test_and_write") == 0)
		return &params->is_zeroing_using_test_and_write;
	if (strcmp(pname,  "is_using_nvme_trim_before_zero") == 0)
		return &params->is_using_nvme_trim_before_zero;
	if (strcmp(pname,  "is_secure_erase_after_disk_format") == 0)
		return &params->is_secure_erase_after_disk_format;
	if (strcmp(pname,  "is_zeroing_mandatory") == 0)
		return &params->is_zeroing_mandatory;
	if (strcmp(pname,  "delete_ns_when_formatting") == 0)
		return &params->delete_ns_when_formatting;
	if (strcmp(pname,  "reset_after_format") == 0)
		return &params->reset_after_format;
	if (strcmp(pname,  "skip_reformat") == 0)
		return &params->skip_reformat;
	if (strcmp(pname,  "ignore_metadata") == 0)
		return &params->ignore_metadata;
	if (strcmp(pname,  "force_metadata") == 0)
		return &params->force_metadata;
	if (strcmp(pname,  "force_512b") == 0)
		return &params->force_512b;
	return NULL;
}

static int nvmeibt_rpc_command_disk_models(int argc, char *argv[], struct nvmeibt_Str *out)
{
	const struct nvmeibt_disk_flow_params_t *params = NULL;
	struct nvmeibt_disk_flow_params_t new_params;
	u32 *pptr = NULL;

	if (argc<2) {
		nvmeibt_Str_sprintf(out, "Set drive-flow parameters for particular disk models.\n"
				"\n"
				"Available sub-commands:\n"
				"    list\n"
				"    default <param> <on|off>\n"
				"    set <model> <param> <on|off>\n"
				"    remove <model>\n"
				"\n"
				"Available parameters:\n"
				"    -- is_zeroing_using_test_and_write    (method of zeroing: read-and-write or write)\n"
				"    -- is_using_nvme_trim_before_zero     (use DSM Deallocate to trim the volume)\n"
				"    -- is_secure_erase_after_disk_format  (obsolete)\n"
				"    -- is_zeroing_mandatory               (zero by writing zeros)\n"
				"    -- delete_ns_when_formatting          (delete/create namespace, Toshiba)\n"
				"    -- reset_after_format                 (remove/rescan drive, Micron)\n"
				"    -- skip_reformat                      (format only if blocksize is wrong, Optane)\n"
				"    -- ignore_metadata                    (force 4k+0 format, Toshiba)\n"
				"    -- force_metadata                     (force 4k+8 format, Toshiba)\n"
				);
		return -1;
	}

	if (strcmp(argv[1], "list") == 0) {
		params = nvmeibt_disk_flow_params_get(NULL, true);	// defaults
		nvmeibt_Str_sprintf(out, "Defaults:\n");
		_print_flow_params(params, out);

		params = NULL;
		while ((params = nvmeibt_disk_flow_params_get_next_model(params))) {
			nvmeibt_Str_sprintf(out, "Disk model %s:\n", params->model);
			_print_flow_params(params, out);
		}
		return 0;
	} else if (strcmp(argv[1], "default") == 0 && argc>3) {
		params = nvmeibt_disk_flow_params_get(NULL, true);	// defaults
		memcpy(&new_params, params, sizeof(*params));
		pptr = _get_param_ptr(&new_params, argv[2]);
		if (pptr == NULL) {
			nvmeibt_Str_sprintf(out, "Unknown parameter, use 'disk-models' without arguments for help.\n");
			return -1;
		}
		if (strcmp(argv[3], "on") == 0)
			*pptr = 1;
		else if (strcmp(argv[3], "off") == 0)
			*pptr = 0;
		else {
			nvmeibt_Str_sprintf(out, "Unknown value, specify 'on' or 'off'.\n");
			return -1;
		}
		nvmeibt_disk_flow_params_set_model_params(&new_params);
		persist_params_in_file();
		nvmeibt_Str_sprintf(out, "New defaults:\n");
		params = nvmeibt_disk_flow_params_get(NULL, true);	// defaults
		_print_flow_params(params, out);
		return 0;
	} else if (strcmp(argv[1], "set") == 0 && argc>4) {
		char model[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE+1] = "";
		int param_arg = argc-2, rc;
		int model_arg;
		int model_len = 0;

		pptr = _get_param_ptr(&new_params, argv[param_arg]);
		if (pptr == NULL) {
			nvmeibt_Str_sprintf(out, "Unknown parameter, use 'disk-models' without arguments for help.\n");
			return -1;
		}

		for (model_arg = 2; model_arg < param_arg; model_arg++) {
			if (strlen(argv[model_arg] + model_len) >= NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE) {
				nvmeibt_Str_sprintf(out, "Model name too long!\n");
				return -1;
			}
			if (model_arg > 2)
				nvmeibt_strlcat(model, " ", NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE + 1);
			nvmeibt_strlcat(model, argv[model_arg], NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE + 1);
			model_len = strlen(model)+1;
		}

		params = nvmeibt_disk_flow_params_get(model, false);
		if (params == NULL) {
			nvmeibt_Str_sprintf(out, "New disk model added, using defaults for unspecified parameters.\n");
			params = nvmeibt_disk_flow_params_get(NULL, true);
		}
		memcpy(&new_params, params, sizeof(*params));
		nvmeibt_strlcpy(new_params.model, model, sizeof(new_params.model));
		if (strcmp(argv[param_arg+1], "on") == 0)
			*pptr = 1;
		else if (strcmp(argv[param_arg+1], "off") == 0)
			*pptr = 0;
		else {
			nvmeibt_Str_sprintf(out, "Unknown value, specify 'on' or 'off'.\n");
			return -1;
		}

		rc = nvmeibt_disk_flow_params_set_model_params(&new_params);
		if (rc<0) {
			nvmeibt_Str_sprintf(out, "Failed adding new model (maximum 10 models allowed).\n");
			return -1;
		}
		persist_params_in_file();
		params = nvmeibt_disk_flow_params_get(model, true);
		nvmeibt_Str_sprintf(out, "Disk model %s parameters:\n", params->model);
		_print_flow_params(params, out);
		return 0;
	} else if (strcmp(argv[1], "remove") == 0 && argc>2) {
		char model[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE+1] = "";
		int model_arg;
		int model_len = 0;

		for (model_arg = 2; model_arg < argc; model_arg++) {
			if (strlen(argv[model_arg] + model_len) >= NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE) {
				nvmeibt_Str_sprintf(out, "Model name too long!\n");
				return -1;
			}
			if (model_arg > 2)
				nvmeibt_strlcat(model, " ", NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE + 1);
			nvmeibt_strlcat(model, argv[model_arg], NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE + 1);
			model_len = strlen(model)+1;
		}

		params = nvmeibt_disk_flow_params_get(model, false);
		if (params == NULL) {
			nvmeibt_Str_sprintf(out, "Unknown disk model.\n");
			return -1;
		}
		nvmeibt_Str_sprintf(out, "Disk model %s removed.\n", params->model);
		nvmeibt_disk_flow_params_remove_model(params->model);
		persist_params_in_file();
		return 0;
	}
	return -1;
}

int nvmeibt_rpc_command_config(int argc, char *argv[], struct nvmeibt_Str *out)
{
	if (argc<2) {
		nvmeibt_Str_sprintf(out, "Set TOMA operational parameters.\n"
				"\nAvailable parameters:\n");
		nvmeibt_debug_config_params_print(out, false, false);
		nvmeibt_Str_sprintf(out, "\nUse \"config print\" to show the current values.\n");
		return 0;
	}
	if (argc==2 && strcmp(argv[1], "print")==0) {
		nvmeibt_debug_config_params_print(out, true, true);
		return 0;
	}
	if (argc!=3) {
		nvmeibt_Str_sprintf(out, "Specify a parameter and a value, e.g. \"config topology_max_praids_in_a_report 50\".\n");
		nvmeibt_Str_sprintf(out, "To return a parameter to its default value specify \"default\" as the value,\n"
				"e.g. \"config topology_max_praids_in_a_report default\".\n");
		return 0;
	}

	if (nvmeibt_debug_config_params_set(argv[1], argv[2])) {
		nvmeibt_Str_sprintf(out, "Value set successfully. %s=%s\n", argv[1], argv[2]);
		persist_params_in_file();
		return 0;
	}
	nvmeibt_Str_sprintf(out, "Parameter '%s' not found!\n", argv[1]);
	return -1;
}

static int nvmeibt_rpc_command_ignore_raft_member(int argc, char *argv[], struct nvmeibt_Str *out)
{
	if (argc != 2) {
		nvmeibt_Str_sprintf(out, "Ignore raft member. param: node hostname (nvmeXXX.mtv.labs.mlnx)\n");
	} else if (nvmeibt_raft_ignore_member(argv[1])) {
		nvmeibt_Str_sprintf(out, "Command failed\n");
	} else {
		nvmeibt_Str_sprintf(out, "Command accepted\n");
	}
	return 0;
}

static int nvmeibt_rpc_command_shadow_attach(int argc, char *argv[], struct nvmeibt_Str *out)
{
	if ((argc != 3) || ((strcmp(argv[1], "attach") && strcmp(argv[1], "detach")))) {
		nvmeibt_Str_sprintf(out, "Usage: shadow-volume attach/detach vol_name\n");
	} else {
		nvmeibt_attach_detach_shadow_vol(argv[2], strcmp(argv[1], "detach"), out);
	}
	return 0;
}

typedef int (*rpc_command_handler_fn)(int, char **, struct nvmeibt_Str *);
static struct rpc_handler_t {
	char *command;
	rpc_command_handler_fn fn;
} rpc_commands[] = {
		{ "status",             nvmeibt_rpc_command_status },
		{ "trace-status",       nvmeibt_rpc_command_trace },
		{ "simulate",           nvmeibt_rpc_command_simulate },
		{ "locate",             nvmeibt_rpc_command_locate },
		{ "disk-models",        nvmeibt_rpc_command_disk_models },
		{ "config",             nvmeibt_rpc_command_config },
		{ "ignore-raft_member", nvmeibt_rpc_command_ignore_raft_member },
		{ "shadow-volume",      nvmeibt_rpc_command_shadow_attach },
};

static int nvmeibt_rpc_handle_command(char *in, struct nvmeibt_Str *out)
{
	#define MAX_ARGV 12
	int rc = 0;
	int argc = 0, i;
	char *argv[MAX_ARGV] = { NULL };
	char *p;

	NFIN;

	p = in;
	for (i=0; i<MAX_ARGV; i++) {
		argv[i] = p;
		p = strchr(p, ' ');
		if (p) {
			*p=0;
			p++;
			argc++;
		} else {
			argc++;
			break;
		}
	}

	for (i=0; i<ARRAY_SIZE(rpc_commands); i++) {
		if (strcmp(argv[0], rpc_commands[i].command)==0) {
			rc = rpc_commands[i].fn(argc, argv, out);
			break;
		}
	}

	if (i==ARRAY_SIZE(rpc_commands)) {
		if (strcmp(argv[0], "help")==0) {
			nvmeibt_Str_sprintf(out, "Available commands:\n");
			for (i=0; i<ARRAY_SIZE(rpc_commands); i++) {
				nvmeibt_Str_sprintf(out, "    %s\n", rpc_commands[i].command);
			}
		}
	}

	NFOUT;
	return rc;
}

void nvmeibt_rpc_run(void)
{
	struct sockaddr_un sun;
	int rc;
	char inbuf[256];
	int64_t activity_diff;
	struct nvmeibt_topology *cur_topo = nvmeibt_global_get_global();
	static struct timespec rpc_last_active = TIMESPEC_ZERO;
	static int rpc_out_offset = 0;

	NFIN;

	if (!cur_topo)
		goto out;

	if (rpc_listener_fd < 0) {
		int arg;

		unlink(TOMA_RPC_PATH);
		rpc_listener_fd = socket(PF_UNIX, SOCK_STREAM, 0);
		if (rpc_listener_fd < 0) {
			N_Tf(nvmeibt_rpc_trace_1, "Can't create socket, @AUTO_ERRNO");
			goto out;
		}

		arg=1;
		rc = setsockopt(rpc_listener_fd, SOL_SOCKET, SO_REUSEADDR, &arg, sizeof(arg));
		if (rc) {
			N_Tf(nvmeibt_rpc_trace_2, "Can't setsockopt SO_REUSEADDR, @AUTO_ERRNO");
			close(rpc_listener_fd);
			rpc_listener_fd = -1;
			goto out;
		}

		arg = 1;
		ioctl(rpc_listener_fd, FIONBIO, &arg);

		sun.sun_family = PF_UNIX;
		strncpy(sun.sun_path, TOMA_RPC_PATH, sizeof(sun.sun_path)-1);
		rc = bind(rpc_listener_fd, (struct sockaddr *)&sun, sizeof(sun));
		chmod(TOMA_RPC_PATH, 0770);
		if (rc) {
			N_Tf(nvmeibt_rpc_trace_3, "Can't bind RPC socket, @AUTO_ERRNO");
			close(rpc_listener_fd);
			rpc_listener_fd = -1;
			goto out;
		}

		listen(rpc_listener_fd, 1);
	}

	if (rpc_child_fd < 0) {
		int arg;
		socklen_t sunlen = sizeof(sun);

		if (rpc_out_str) {
			NNVMEIBT_STR_FREE(nvmeibt_rpc_trace_3_1, rpc_out_str);
		}

		rpc_child_fd = accept(rpc_listener_fd, (struct sockaddr *)&sun, &sunlen);

		if (rpc_child_fd < 0) {
			// this is normal
			goto out;
		}
		N_Tf(nvmeibt_rpc_trace_4, "Accepted RPC connection, fd @INT", rpc_child_fd);
		rpc_last_active = nvmeibt_global_get_cur_event_start_time();
		arg = 1;
		ioctl(rpc_child_fd, FIONBIO, &arg);
	}

	// at this point we have an active client socket
	activity_diff = timespec_diff_ns(nvmeibt_global_get_cur_event_start_time(), rpc_last_active);
	if (activity_diff > MSEC_TO_NSEC(5000LL)) {
		N_Tf(nvmeibt_rpc_trace_5, "Stopping child RPC connection due to timeout, @LLD", activity_diff);
		close(rpc_child_fd);
		rpc_child_fd = -1;
		goto out;
	}

	inbuf[0] = 0;
	rc = recv(rpc_child_fd, inbuf, sizeof(inbuf)-1, MSG_PEEK);
	if (rc>0) {
		char *line_end;
		inbuf[rc] = 0;
		line_end = strchr(inbuf, '\n');
		if (line_end) {
			rc = recv(rpc_child_fd, inbuf, (line_end-inbuf)+1, 0);		// consume the message
			*line_end = 0;
			N_Tf(nvmeibt_rpc_trace_6, "Got RPC command: @STR", inbuf);
			rpc_last_active = nvmeibt_global_get_cur_event_start_time();

			rpc_out_str = NNVMEIBT_STR_ALLOC(nvmeibt_rpc_trace_6_1);
			NNVMEIBT_STR_RESIZE_BUF(nvmeibt_rpc_trace_6_2, rpc_out_str, 8192);
			rc = nvmeibt_rpc_handle_command(inbuf, rpc_out_str);
			rpc_out_offset = 0;

			if (0 == nvmeibt_Str_strlen(rpc_out_str)) {
				nvmeibt_Str_sprintf(rpc_out_str, "[RPC command not understood. Try 'help']\n");
			}
		}
	}
	else if (rc == 0) {
		// graceful disconnect
		N_Tf(nvmeibt_rpc_trace_7, "Disconnected RPC connection, fd @INT", rpc_child_fd);
		close(rpc_child_fd);
		rpc_child_fd = -1;
		goto out;
	}
	else if (errno != EAGAIN) {
		// error disconnect
		N_Tf(nvmeibt_rpc_trace_8, "RPC receive error, @AUTO_ERRNO");
		close(rpc_child_fd);
		rpc_child_fd = -1;
		goto out;
	}

	if (rpc_out_str) {
		int len = nvmeibt_Str_strlen(rpc_out_str) - rpc_out_offset;
		if (len>0)
		{
			rc = send(rpc_child_fd, nvmeibt_Str_str(rpc_out_str)+rpc_out_offset, len, 0);
			if (rc>0) {
				rpc_out_offset += rc;
			}
			else if (rc==0) {
				rpc_out_offset = nvmeibt_Str_strlen(rpc_out_str);
			}
			else if (rc<0 && errno!=EAGAIN && errno!=EINTR) {
				N_Tf(nvmeibt_rpc_trace_9, "RPC send error, @AUTO_ERRNO");
				rpc_out_offset = nvmeibt_Str_strlen(rpc_out_str);
			}
		}
		if (rpc_out_offset == (int)nvmeibt_Str_strlen(rpc_out_str)) {
			N_Tf(nvmeibt_rpc_trace_10, "RPC complete");
			NNVMEIBT_STR_FREE(nvmeibt_rpc_trace_10_1, rpc_out_str);
			close(rpc_child_fd);
			rpc_child_fd = -1;
			goto out;
		}
	}

out:
	NFOUT;
}

void nvmeibt_rpc_terminate(void)
{
	NFIN;
	if (rpc_child_fd >= 0) {
		close(rpc_child_fd);
		rpc_child_fd = -1;
	}
	if (rpc_listener_fd >= 0) {
		close(rpc_listener_fd);
		rpc_listener_fd = -1;
		unlink(TOMA_RPC_PATH);
	}
	if (rpc_out_str) {
		NNVMEIBT_STR_FREE(nvmeibt_rpc_trace_11_1, rpc_out_str);
	}
	NFOUT;
}

