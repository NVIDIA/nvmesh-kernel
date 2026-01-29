/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "uni_scenario_vol_config.h"
#include "block/datapath_utils_generic/dp_io_stats/nvmeibc_b_dp_iostats.h"
#include "nvmesh_sim.h"
#include "../nvmeibc_block_common.h"
#include "../../datapath_ec/nvmeibc_block_dp_ec_gf.h"
#include "uni_framework/simu_test.h"

/************************* SCSI ioctls ****************************************/
#include <scsi/sg.h>
#include <scsi/scsi.h>
#include "/usr/include/linux/nvme_ioctl.h"

#include "common/compat/kr_incs_compiler_types.h"

static u64 __scsi_get_u64(void* buf, int n) { return ((u64*)buf)[n]; }
// Simulate call: "sg_inq /dev/nvmesh/sm1 " on real devices, assuming you did: sudo yum install sg3_utils
TEST_FUNC void __unitest_issue_ioctls_todisk(struct gendisk *disk, struct nvmeibc_os_api *os) {
	const bool is_detaching = (os->atom.status == nvmeiba_status_detaching);
	#define __IOCTL_RV(rv) (is_detaching ? -ENOTTY : (rv))
	const char *git_ver = (((ulong)COMMIT_ID != 0xdeadbeef) ? __stringify(NVMESH_VERSION) : "v0.0");				// If git exists, verify version.
	u32 scsi_res[9] = {0};							// Daniel: Instead of 36 bytes use 9 ints for simpleand faster  access
	struct nvme_id_ctrl id_ctrl;
	unsigned char scsi_cmd[16] = {INQUIRY, 1 /*EVPD bit*/, 0x00 /*VPD_SUPPORTED_PAGES page code*/, sizeof(scsi_res)/256, sizeof(scsi_res) /* u16 Alloc length */};
	struct sg_io_hdr hdr = {.interface_id = 'S', .cmdp = scsi_cmd, .cmd_len = sizeof(scsi_cmd), .dxferp = scsi_res};
	BUG_ON(do_vfs_ioctl((struct file *)disk, 0, SG_IO, (ulong)&hdr) != __IOCTL_RV(0));
	BUG_ON(!is_detaching && __scsi_get_u64(scsi_res, 0) != 0x800002000000LL);

	scsi_cmd[2] = 0x80; // VPD_SERIAL_NUMBER;
	BUG_ON(do_vfs_ioctl((struct file *)disk, 0, SG_IO, (ulong)&hdr) != __IOCTL_RV(0));
	if (!is_detaching) {
		BUG_ON(scsi_res[0] != 0x14008000);
		BUG_ON(strncmp((char*)&scsi_res[1], os->dev_uuid, 8) != 0);
	}

	scsi_cmd[1] = 0; 	// EVPD bit, wrong page
	BUG_ON(do_vfs_ioctl((struct file *)disk, 0, SG_IO, (ulong)&hdr) != __IOCTL_RV(-EINVAL));

	scsi_cmd[2] = 0; 	// EVPD bit, textual data
	BUG_ON(do_vfs_ioctl((struct file *)disk, 0, SG_IO, (ulong)&hdr) != __IOCTL_RV(0));
	if (!is_detaching) {
		BUG_ON(__scsi_get_u64(scsi_res, 0) != 0x200001f02060000LL);
		BUG_ON(strncmp((char*)&scsi_res[2], "NVMesh  NVMesh 1.1.0-97", 24) != 0);			// 1.1.0-97 is deliberatly a ficticious version number, to be able to verify it via unitest
		BUG_ON(strncmp(&(((char*)scsi_res)[32]), git_ver, 4) != 0);
	}

	BUG_ON(do_vfs_ioctl((struct file *)disk, 0, (unsigned int)-1    , 0) != -ENOTTY);	// Ilelgal
	BUG_ON(do_vfs_ioctl((struct file *)disk, 0, CDROM_GET_CAPABILITY, 0) != -ENOTTY);	// Daniel: I have seen kernels issue this ioctl to us. don't know why
	BUG_ON(do_vfs_ioctl((struct file *)disk, 0, NVME_IOCTL_ID, (ulong)&id_ctrl) != __IOCTL_RV(0));	// Daniel: I have seen kernels issue this ioctl to us. don't know why
	#undef __IOCTL_RV
	return;
}

/************************* CLI ioctls ****************************************/
void verify_nvmeibc_dlba_to_vlba_translation(struct dp_io_topo_iterator *topo_it, const struct nvmeibc_datapath *dp) {
	int slice_number, seg_ind;
	sector_t vol_lba, conv_lba;
	while (dp_io_topo_iterator_next(topo_it, 'r')) {		// loop over lockset's
		for (slice_number=0; slice_number < LOCKSET_SLICES; slice_number++) {
			const u64 start_raid_on_disk = topo_it->res.rlba / topo_it->res.r->slice_size;
			const int blockset_offset = slice_number * topo_it->res.r->slice_size;
			const int ss = get_owner_seg_slice_start(topo_it->res.r, topo_it->res.rlba + blockset_offset);
			for (seg_ind=0; seg_ind < topo_it->res.r->replicas; seg_ind++) {
				int slice_offset = (seg_ind - ss) + ((seg_ind < ss) ? (topo_it->res.r->replicas) : 0);
				vol_lba = topo_it->vlba + blockset_offset + ((slice_offset < topo_it->res.r->slice_size) ? slice_offset : 0);
				conv_lba = nvmeibc_datapath_dlba_to_vlba(dp, topo_it->res.r, seg_ind, start_raid_on_disk + slice_number + topo_it->res.r->segments[seg_ind].first_lba);
				BUG_ON(conv_lba != vol_lba);
			}
		}
	}
}

// Test various external configuration mechanisms through configfs, scsi ioctls() and prints to /proc
#define send_alert_to_mgmt(inst_id, level, _l, _h, _m, i) do { \
	mgmt_simu_expect_alert(&sys->mgmt.mcs[inst_id], level, _h, _m); \
	clientSimulator_send_to_cli(client, "@" _l _h "@" _m ); \
	mgmt_simu_verify_num_alerts(&sys->mgmt.mcs[inst_id], i); \
	expected_num_of_executed_ioctls++; } while (0)

static int __translate_addr_unitests(char cmd[256], const struct nvmeibc_block_device* dev, struct clientSimulator *client) {
	int rv = 0;
	sprintf(cmd, "#%s|translate_addr=0x62\n#|translate_addr=3,2,L", dev->name); clientSimulator_send_to_cli(client, cmd); rv +=2;
	if (1) {			// Translation in illegal topology
		struct nvmeibc_topology *t = nvmeibc_topology_get((void*)&dev->topologies);
		struct nvmeibc_raid1 *pr = t->chunks->raid1s;
		const union nvmeib_lock_id  lid = pr->lid;
		const enum nvmeibc_segment_registration_status registration_status = pr->segments[0].registration_status;
		pr->lid.all = LS_UNLOCKED;
		pr->segments[0].registration_status = SEG_REGSTATUS_EMPTY;
		sprintf(cmd, "#%s|translate_addr=0,1,L,777", dev->name); clientSimulator_send_to_cli(client, cmd); rv +=1;
		pr->lid = lid;
		pr->segments[0].registration_status = registration_status;
		nvmeibc_topology_put(t);
	}
	if (1) {
		const char *fmt = "#%s|translate_dlba%c%c sgmnt=(%u,%u,%u) dlba=%llu";
		sprintf(cmd, fmt, dev->name, ' ', ' ', 0, 1, 1, 5ULL); clientSimulator_send_to_cli(client, cmd); rv +=1;
		sprintf(cmd, fmt, dev->name, '2', 'v', 0, 1, 1, 5ULL); clientSimulator_send_to_cli(client, cmd); rv +=1;
		sprintf(cmd, fmt, dev->name, '2', 'C', 0, 1, 1, 5ULL); clientSimulator_send_to_cli(client, cmd); rv +=1;
		sprintf(cmd, fmt, dev->name, '2', 'r', 0, 1, 1, 5ULL); clientSimulator_send_to_cli(client, cmd); rv +=1;
		sprintf(cmd, fmt, dev->name, '2', 'q', 0, 1, 1, 5ULL); clientSimulator_send_to_cli(client, cmd); // illegal

		sprintf(cmd, "#%s|translate_txbm=0x50u", dev->name); clientSimulator_send_to_cli(client, cmd); rv +=1;
		sprintf(cmd, "#%s|translate_qlcmdv=117", dev->name); clientSimulator_send_to_cli(client, cmd); rv +=1;
	}
	return rv;
}

TEST_FUNC int unitest_ioctls_fs_proc(struct NVMeshSystem *sys) {
	char cmd[256];
	struct clientSimulator *client = &sys->clients[0];			// Test via the first client
	const struct nvmeibc_block_device* dev = client->devs[0];
	const struct nvmeibc_disk	*disks = client->physDiscs;
	struct nvmeibc_os_api *os			   = dev->os;
	#define _retry_secs_of(dev) ((int)(dev->max_retry_jiffies/HZ))
	int old_val = _retry_secs_of(dev);
	const int ioctl_num = clientSimulator_get_num_executed_ioctls(client);
	int expected_num_of_executed_ioctls = 0;		// Increasing this value each time a valid ioctl is called
	clientSimulator_send_to_cli(client, "#|max_retry_secs 3");	expected_num_of_executed_ioctls++;
	BUG_ON(_retry_secs_of(dev) != 3);
	sprintf(cmd, "#|max_retry_secs %d", old_val);		// Retrun old value.
	clientSimulator_send_to_cli(client, cmd);						expected_num_of_executed_ioctls++;
	BUG_ON(_retry_secs_of(dev) != old_val);

	if (1) {														// Testing mgmt allert messages - set expectors for values and send message
		int number_of_sent_logs = 0;
		// Each send alert to mgmt will increase the local parameter named expected_num_of_executed_ioctls by 1
		send_alert_to_mgmt(0, "DEBUG", "D", "HEADER D", "MESSAGED", ++number_of_sent_logs);
		send_alert_to_mgmt(0, "INFO" , "I", "HEADERI", "MESSAGE I", ++number_of_sent_logs);
		send_alert_to_mgmt(0, "WARNING" , "w", " HEADER W ", " MESSAGEW", ++number_of_sent_logs);
		send_alert_to_mgmt(0, "ERROR", "E", " <HEADERE>", " <MESSAGE E>", ++number_of_sent_logs);
		send_alert_to_mgmt(0, "ERROR", "e", "Only a header thats what I am", "", ++number_of_sent_logs);
		send_alert_to_mgmt(0, "INFO", "i", "", "", ++number_of_sent_logs);
		// Send a max heade0, r 96 (will actually send 95 and compare with 95) and a max message of 128 random generated string
		send_alert_to_mgmt(0, "DEBUG", "d", "LoGnYarAXKUzTpFwCkVKqmZwiHuBrAgaabxQlepEQYKwpeayJoFgdUJvEkkzXVgqCxojmnibTJfVyolGYvauAyGKZxurqSFe",
		                      "ikLWcAtMnpWZlmoaMEHrMFctyEgXZoIcWlRviPqfwgVpLNmTmeChefZwvKelYoTJzOdpBCoZmuQVaoKcSjDuuKsZSYqfGaEftDoLYWmhDTPIYxpryhFhYbSNQquAJFTs", ++number_of_sent_logs);
		send_alert_to_mgmt(0, "INFO", "i", "HEADERISOK", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", ++number_of_sent_logs);
		send_alert_to_mgmt(0, "WARNING", "W", "This header has a | pipe", "Even the message has a | pipe", ++number_of_sent_logs);
		set_cli_error_expector(client, INVALID_CLI_FORMAT);
		clientSimulator_send_to_cli_and_wait(client, "@q");			// 1 messages will not be sent
		mgmt_simu_verify_num_alerts(&sys->mgmt.mcs[client->inst_id], number_of_sent_logs);
	}
	// TODO: verify that we actually did the dump - Daniel should we prioritize it?
	if (1) {														// Testing dump_status for all volumes
		clientSimulator_send_to_cli(client, "@help");						// Real dump status
		expected_num_of_executed_ioctls++;
		clientSimulator_send_to_cli(client, "%dump_vol_status");						// Real dump status
		expected_num_of_executed_ioctls++;
		set_cli_error_expector(client, INVALID_IOCTL_COMMAND);
		clientSimulator_send_to_cli_and_wait(client, "%dump_nothing");						// Should do nothing
		set_cli_error_expector(client, INVALID_CLI_FORMAT);
		clientSimulator_send_to_cli_and_wait(client, "%");									// Should be ignored
	}

	if (1) {											// Test invalid commands
		set_cli_error_expector(client, INVALID_IOCTL_COMMAND);
		clientSimulator_send_to_cli_and_wait(client, "#2pac foreva bitch");	// Test wrong command (missing |)
		clientSimulator_send_to_cli(client, "#2pac|foreva bitch");	// Test ioctl to non existing volume
		clientSimulator_send_to_cli(client, "#|foreva bitch");		// Test unrecognized ioctl
	}

	sprintf(cmd, "#%s|clear_io_stats\n#|clear_io_stats", dev->name); clientSimulator_send_to_cli(client, cmd); expected_num_of_executed_ioctls +=2;
	sprintf(cmd, "#%s|topo_dup\n#|topo_dup"          , dev->name); clientSimulator_send_to_cli(client, cmd); expected_num_of_executed_ioctls +=2;

	sprintf(cmd, "#%s|di_debug_mode=1\n#|di_debug_mode=1", dev->name);	clientSimulator_send_to_cli(client, cmd); BUG_ON(!dev->dp.enable_di_debug_mode); expected_num_of_executed_ioctls +=2;
	sprintf(cmd, "#%s|di_debug_mode=0\n#|di_debug_mode=0", dev->name);	clientSimulator_send_to_cli(client, cmd); BUG_ON( dev->dp.enable_di_debug_mode); expected_num_of_executed_ioctls +=2;

	sprintf(cmd, "#%s|set_read_edic=1", dev->name); clientSimulator_send_to_cli(client, cmd); BUG_ON(!dev->dp.enable_edic_check); expected_num_of_executed_ioctls +=1;
	sprintf(cmd, "#%s|set_read_edic=0", dev->name); clientSimulator_send_to_cli(client, cmd); BUG_ON( dev->dp.enable_edic_check); expected_num_of_executed_ioctls +=1;

	expected_num_of_executed_ioctls += __translate_addr_unitests(cmd, dev, client);
	sprintf(cmd, "#%s|di_bug_on_addr=98\n#%s|max_retry_secs %d\n#%s|volume_suspend=0", dev->name, dev->name, old_val, dev->name); clientSimulator_send_to_cli(client, cmd); expected_num_of_executed_ioctls += 3;
	sprintf(cmd, "#%s|di_bug_on_addr=1000\n#%s|di_bug_on_addr\n", dev->name, dev->name); clientSimulator_send_to_cli(client, cmd); expected_num_of_executed_ioctls += 2;
	sprintf(cmd, "#|di_bug_on_addr=98\n#|max_retry_secs %d\n#|volume_suspend=0", old_val); clientSimulator_send_to_cli(client, cmd); expected_num_of_executed_ioctls += 3;

	sprintf(cmd, "#%s|volume_suspend=1" , dev->name);	clientSimulator_send_to_cli(client, cmd); BUG_ON(!is_suspended(dev->topologies));
	sprintf(cmd, "#%s|volume_suspend=0" , dev->name);	clientSimulator_send_to_cli(client, cmd); BUG_ON( is_suspended(dev->topologies));
	sprintf(cmd, "#|volume_suspend=1");	clientSimulator_send_to_cli(client, cmd); BUG_ON(!is_suspended(dev->topologies));
	sprintf(cmd, "#|volume_suspend=0");	clientSimulator_send_to_cli(client, cmd); BUG_ON( is_suspended(dev->topologies));
	expected_num_of_executed_ioctls += 4;
	{
		int prev_warn = nvmeibcb_dp_io_fail_mgr_set_limit(NULL, 0);
		sprintf(cmd, "#|volume_io_fail=15");	clientSimulator_send_to_cli(client, cmd);
		sprintf(cmd, "#|volume_io_fail=R");	clientSimulator_send_to_cli(client, cmd);
		sprintf(cmd, "#|volume_io_fail=D");	clientSimulator_send_to_cli(client, cmd);
		nvmeibcb_dp_io_fail_mgr_set_limit(NULL, prev_warn);
		expected_num_of_executed_ioctls += 3;
	}

	sprintf(cmd, "#%s|ignore_toma_msg=1" , dev->name);	clientSimulator_send_to_cli(client, cmd); BUG_ON(!dev->ignore_all_toma_msgs);
	sprintf(cmd, "#%s|ignore_toma_msg=0" , dev->name);	clientSimulator_send_to_cli(client, cmd); BUG_ON( dev->ignore_all_toma_msgs);
	sprintf(cmd, "#|ignore_toma_msg=1");				clientSimulator_send_to_cli(client, cmd); BUG_ON(!dev->ignore_all_toma_msgs);
	sprintf(cmd, "#*|ignore_toma_msg=0");				clientSimulator_send_to_cli(client, cmd); BUG_ON( dev->ignore_all_toma_msgs);
	expected_num_of_executed_ioctls += 4;

	if (1) {
		bool *enf = &dev->os->atom.conf.enforce_readonly;
		old_val = *enf;
		sprintf(cmd, "#|enforce_readonly %d", 3);		clientSimulator_send_to_cli(client, cmd);	BUG_ON(          !*enf); expected_num_of_executed_ioctls++;
		sprintf(cmd, "#|enforce_readonly %d", 0);		clientSimulator_send_to_cli(client, cmd);	BUG_ON(           *enf); expected_num_of_executed_ioctls++;
		sprintf(cmd, "#|enforce_readonly %d", old_val);	clientSimulator_send_to_cli(client, cmd);	BUG_ON(old_val != *enf); expected_num_of_executed_ioctls++; // Retrun old value.
	}

	sprintf(cmd, "#%s|os_ptr++", dev->name);		clientSimulator_send_to_cli(client, cmd);	expected_num_of_executed_ioctls++; BUG_ON(!dev->os->unsafe_self_ref.bdev_during_detach);
	sprintf(cmd, "#%s|os_ptr++", dev->name);		clientSimulator_send_to_cli(client, cmd);	expected_num_of_executed_ioctls++; // Illegal, verify will not crash
	sprintf(cmd, "#%s|os_ptr--", dev->name);		clientSimulator_send_to_cli(client, cmd);	expected_num_of_executed_ioctls++; BUG_ON( dev->os->unsafe_self_ref.bdev_during_detach);
	sprintf(cmd, "#%s|os_ptr--", dev->name);		clientSimulator_send_to_cli(client, cmd);	expected_num_of_executed_ioctls++; // Illegal, verify will not crash
	sprintf(cmd, "#|os_ptr++\n#|os_ptr--");			clientSimulator_send_to_cli(client, cmd);	expected_num_of_executed_ioctls+=0;// Illegal, both calls will fail

	sprintf(cmd, "#%s|dump_tcntrs\n#%s|dump_uncompleted\n#%s|dump_transfers %p", dev->name, dev->name, dev->name, disks); clientSimulator_send_to_cli(client, cmd);	expected_num_of_executed_ioctls+=3;
	sprintf(cmd, "#|dump_transfers_raid=0,0\n#%s|dump_transfers_raid=0,0", dev->name); clientSimulator_send_to_cli(client, cmd);	expected_num_of_executed_ioctls+=1;

	sprintf(cmd, "#%s|mgmt_alert_stop\n#|mgmt_alert_stop"             ,dev->name); clientSimulator_send_to_cli(client, cmd); expected_num_of_executed_ioctls+=2;
	sprintf(cmd, "#%s|mgmt_alert_freq 0\n#|mgmt_alert_freq 4294967295",dev->name); clientSimulator_send_to_cli(client, cmd); expected_num_of_executed_ioctls+=1;
	sprintf(cmd, "#%s|mgmt_alert_freq 10\n#|mgmt_alert_freq 10"       ,dev->name); clientSimulator_send_to_cli(client, cmd); expected_num_of_executed_ioctls+=2;
	sprintf(cmd, "#%s|help\n#*|help", dev->name); clientSimulator_send_to_cli(client, cmd); expected_num_of_executed_ioctls+=2;

	if (1) {	// Stale  lock resolver erroneous ioctls
		sprintf(cmd, "#|STLR_zzzzzz\n#|STLR_print");   clientSimulator_send_to_cli(client, cmd);
		sprintf(cmd, "#%s|STLR_print 0,7", dev->name); clientSimulator_send_to_cli(client, cmd);
		sprintf(cmd, "#%s|STLR_clear=7,0", dev->name); clientSimulator_send_to_cli(client, cmd);
		sprintf(cmd, "#%s|STLR_clear 7," , dev->name); clientSimulator_send_to_cli(client, cmd);
	}

	sprintf(cmd, "#|topo_check\n#%s|topo_check", dev->name); clientSimulator_send_to_cli(client, cmd); expected_num_of_executed_ioctls+=2;

	if (1) {	// GF ioctls
		int cur_gf_funcs = __gf_choose_functions(NVMEIBC_GF_DISPLAY_CURRENT), i;
		const char *cur_gf_funcs_cmd = nvmeibc_gf_optimization_to_string(cur_gf_funcs);
		for (i = NVMEIBC_GF_UNOPTIMIZED; i < NVMEIBC_GF_TOTAL; i++) {
			sprintf(cmd, "#|change_gf_func=%s", nvmeibc_gf_optimization_to_string(i)); clientSimulator_send_to_cli(client, cmd);
			if (i == NVMEIBC_GF_SSE2) continue;							// Currently Disabled in the product
			if (i == NVMEIBC_GF_EC_CALC) continue;						// Unitest doesn't support kernel function
#if defined(__aarch64__)
			if (i == NVMEIBC_GF_AVX2) continue;	// ARM doesn't support AVX2
#	if !defined(NVMEIBC_GF_ARM_USER_SPACE_IMPLEMENTATION)
			if (i == NVMEIBC_GF_ARM_INTRINSICS) continue;				// Arm intrinsics may only be used if enabled
#	endif
#else
			if (i == NVMEIBC_GF_ARM_INTRINSICS) continue;				// Arm intrinsics may only be used on arm
#endif
			expected_num_of_executed_ioctls++;
			BUG_ON(__gf_choose_functions(NVMEIBC_GF_DISPLAY_CURRENT) != i);
		}
		sprintf(cmd, "#|change_gf_func=%s", cur_gf_funcs_cmd); clientSimulator_send_to_cli(client, cmd); expected_num_of_executed_ioctls+=1;
	}

	if (!NVMESH_IS_PRODUCTION_COMPILATION) {					// Test /proc/nvmeibc/echo
		extern void __change_memory(char *buf);			// Todo: Test it correctly, not via extern
		u64 x = 0;
		sprintf(cmd, "l0x%llx=0x%llx", (u64)&x, 0x5ULL);
		__change_memory(cmd);
		BUG_ON(x!= 0x5);
		sprintf(cmd, "b0x%llx=0x%llx", ((u64)&x + 2), 0x7ULL);
		__change_memory(cmd);
		BUG_ON(x!= 0x70005);
		sprintf(cmd, "u0x%llx=0x%llx", ((u64)&x + 4), 0xD3ULL);
		__change_memory(cmd);
		BUG_ON(x!= 0xD300070005);
	}

	__unitest_issue_ioctls_todisk(os->atom.disk, os);		// Test all supported ioctl's
	BUG_ON(ioctl_num + expected_num_of_executed_ioctls != clientSimulator_get_num_executed_ioctls(client));

	if (1) { 	// test NORMAL volume conversion of sector between volume & underlying storage (should be made a debug utility)
		struct dp_io_topo_iterator topo_it;
		int vol_ind;
		for (vol_ind = 0; vol_ind < client->nBdevs; vol_ind++) {
			u64 vol_size = client->devs[vol_ind]->size;
			struct nvmeibc_topology *t = ___get_tail_topo_of_device(sys, vol_ind);
			dev = client->devs[vol_ind];
			dp_io_topo_iterator_init(&topo_it, 0, vol_size, t, -1);
			verify_nvmeibc_dlba_to_vlba_translation(&topo_it, &dev->dp);
		}
	}
	return 0;
}

/*************************  Volume configs ************************************/
static void __unmount_volumes(unsigned long param) {
	struct NVMeshSystem *sys = (struct NVMeshSystem *)param;
	struct clientSimulator *client = &sys->clients[0];
	int v;
	for (v=0; v<client->nBdevs; v++)
		osSimulator_unmount(&client->OS, v);
}

// Try Unsafe dettaching vol 0 succeed even though volumes are mounted
static int __unitest_DetachAttachVolume0_unsafe(struct NVMeshSystem *sys){
	int rv, i, volInd = 0;
	struct clientSimulator *client = &sys->clients[0];			// Test via the first client
	const struct nvmeibc_cinst_params_main* c_inst = &client->p->main;
	struct nvmeibc_os_api *os_api;
	struct gendisk 	   	  **disk = &client->OS.disks[volInd];
	struct ramDiskSimulator* lockServer = &sys->servers[0].ramDisk;
	const unsigned int save_orig_per_cpu = max_ios_per_cpu;
	send_command_to_vol(sys, -1, volInd, volCmds_New);
	os_api = client->devs[0]->os;
	// -----------------------------------						// Unsafe detach during mounting and IO
	rv = osSimulator_mount(&client->OS, volInd, FMODE_WRITE); BUG_ON(rv); // Take lock to prevent IO from finishing and issue many IOs
	ramDiskSimulator_lockDo(lockServer, lockServer->committed_addr.block);
	max_ios_per_cpu = 8;
	for (i=0; i<16; i++) {										// Half IO's will go to execution half to per-cpu lists
		BUG_ON((rv = osSimulator_trim(&client->OS, volInd, lockServer->committed_addr.block , 1)) != 0);
	}
	clientSimulator_send_to_cli_va(client, "#%s|max_retry_secs %d", client->devs[volInd]->name, 0); // Change timeout to 1[mSec]
	send_command_to_vol(sys, -1, volInd, volCmds_ForceDetach);
	BUG_ON(!list_empty(nvmeibc_get_volumes(c_inst)));					// Remaining volumes in the list == did not properly detach
	BUG_ON((*disk == NULL) || (block_api_os_is_mounted(os_api) != 1));
	__unitest_issue_ioctls_todisk(*disk, os_api);				// IOctls to unsafely detached volume
	osSimulator_unmount(&client->OS, volInd);
	BUG_ON(nvmeiba_os_apis_get_num('A') != 0);					// All atoms were cleaned
	clientSimulator_wait_for_detach_drain(client);
	BUG_ON((*disk != NULL) || (atomic_read(&client->OS.bds[volInd].n_active_ios) != 0));
	ramDiskSimulator_lockUn(lockServer, lockServer->committed_addr.block);
	max_ios_per_cpu = save_orig_per_cpu;						// Unroll all changes
	return rv;
}

TEST_FUNC int unitest_update_non_existing_volume(struct NVMeshSystem *sys)
{
	int volInd = 0;
        const char *reply;
	struct clientSimulator *client = &sys->clients[0];
	send_command_to_vol(sys, -1, volInd, volCmds_Detach);
	reset_cli_status_verification(client);
        reply = failed_update_non_exis_vol(&client->vols[volInd], CLI_UPDATE_FAILED);

	set_cli_status_verification_expector(client, reply, volInd);

	send_command_to_vol(sys, -1, volInd, volCmds_Update);
	send_command_to_vol(sys, -1, volInd, volCmds_New);

	return 0;
}

TEST_FUNC int unitest_DetachAttachVolume(struct NVMeshSystem *sys){
	int v, i, rv = -1, volInd = 0, expected_num_of_executed_ioctls = 0;
	u64 cur_version;
	struct clientSimulator *client = &sys->clients[0];			// Test via the first client
	const struct nvmeibc_cinst_params_main* c_inst = &client->p->main;
	int n_ranges_backup[NVMESH_N_PHYS_DISKS];
	struct timer_list retry_timer;
	const int ioctl_num = clientSimulator_get_num_executed_ioctls(client);

	// -----------------------------------						// Detach and reattach volume 0
	send_command_to_vol(sys, -1, volInd, volCmds_Detach);
	BUG_ON(!nvmeibc_topo_is_io_ok(&client->devs[1]->topologies));			// Test IO on all other disks
	BUG_ON(!nvmeibc_topo_is_io_ok(&client->devs[2]->topologies));
	send_command_to_vol(sys, -1, volInd, volCmds_New);				// Reattach

	// -----------------------------------						// Detach with all possible flags, and reattach volume 0
	send_command_to_vol(sys, -1, volInd, volCmds_DetachUpgradeHiddenRecoveryForce);
	BUG_ON(!nvmeibc_topo_is_io_ok(&client->devs[1]->topologies));			// Test IO on all other disks
	BUG_ON(!nvmeibc_topo_is_io_ok(&client->devs[2]->topologies));
	send_command_to_vol(sys, -1, volInd, volCmds_New);				// Reattach

	// -----------------------------------						// Try to update configuration backwards (and fail)
	cur_version = ___get_tail_topo_of_device(sys, volInd)->configuration_version;
	__unitest_volume_config_version_dec(&sys->mdb.vols[volInd], &sys->tcf.vols[volInd]);
	reset_cli_status_verification(client); set_cli_status_verification_expector(client, failed_attach_string(&client->vols[volInd], CLI_UPDATE_FAILED), volInd);
	send_command_to_vol(sys, -1, volInd, volCmds_Update);
	BUG_ON(cur_version != ___get_tail_topo_of_device(sys, volInd)->configuration_version);
	__unitest_volume_config_version_inc(&sys->mdb.vols[volInd], &sys->tcf.vols[volInd]);

	// -----------------------------------						// Detach suspended volume, fail attach on wrong configuration and attach again volume 0.
	if (1) {
		char cmd[128];
		const struct nvmeibc_block_device* dev = client->devs[volInd];
		sprintf(cmd, "#%s|volume_suspend=1", dev->name); clientSimulator_send_to_cli(client, cmd); BUG_ON(!is_suspended(dev->topologies)); expected_num_of_executed_ioctls++;
	}
	send_command_to_vol(sys, -1, volInd, volCmds_Detach);			// Detach 0'th volume without force detach, must not have IOs
	for (i = 0; i < client->nPhysDisks; i++) {
		n_ranges_backup[i] = sys->mdb.discs[volInd][i].n_ranges;
		sys->mdb.discs[volInd][i].n_ranges = 0;					// Volume has zero segments, this will fail the attachment
	}
	reset_cli_status_verification(client);
	set_cli_status_verification_expector(client, failed_attach_string(&client->vols[volInd], CLI_ATTACH_FAILED), volInd);
	send_command_to_vol(sys, -1, volInd, volCmds_New);				// Reattach and fail.
	BUG_ON(client->devs[volInd] != NULL);
	for (i=0; i<client->nPhysDisks; i++)
		sys->mdb.discs[volInd][i].n_ranges = n_ranges_backup[i];
	send_command_to_vol(sys, -1, volInd, volCmds_New);				// Reattach
	BUG_ON(!NVMeshSystem_is_stable(sys));

	// -----------------------------------						// Try Dettaching entire system and fail since volumes are mounted
	for (v=0; v<client->nBdevs; v++){
		sys->mdb.vols[v].nextCmd = volCmds_ForceDetach;
		rv = osSimulator_mount(&client->OS, v, FMODE_WRITE); BUG_ON(rv);
	}

	// Test non forced detach on mounted volumes, we should get busy status to cli And we need to see that the volume didn't start to detach
	reset_cli_status_verification(client);
	set_cli_status_verification_expector(client, cli_generic_string(&client->vols[volInd], CLI_BUSY, false), volInd);
	send_command_to_vol_safe_detach_no_wait(sys, -1, volInd);
	BUG_ON(!nvmeibc_topo_is_io_ok(&client->devs[volInd]->topologies));

	for (v=0; v<client->nBdevs; v++)
		sys->mdb.vols[v].nextCmd = volCmds_Illegal;

	sys->mdb.vols[volInd].nextCmd = volCmds_ForceDetach;			// Force detach to overcome the mount

	init_timer(&retry_timer);
	retry_timer.function = __unmount_volumes;
	retry_timer.data 	  = (unsigned long)sys;
	mod_timer(&retry_timer, jiffies + HZ/1000);					// 1[msec]

	NVMeshSystem_send_volumes_config_to_clients(sys, -1);			// All volumes have been logically detached, now we need to release the gendisk and OS
	if (del_timer(&retry_timer)) { 								// If we removed it too early run it directly
		__unmount_volumes((unsigned long)sys);
	}
	NVMeshSystem_serialize(sys);
	BUG_ON(!list_empty(nvmeibc_get_volumes(c_inst)));					// Remaining volumes in the list = did not properly detach
	clientSimulator_wait_for_detach_drain(client);				// Verify detach completed on all volumes
	BUG_ON(nvmeiba_os_apis_get_num('A') != 0);					// All atoms were cleaned

	// ----------------------------------- Try Unsafe dettaching vol 0 succeed even though volumes are mounted
	expected_num_of_executed_ioctls+=1;
	__unitest_DetachAttachVolume0_unsafe(sys);

	// -----------------------------------						// Test IO enabling during attach
	sys->servers[0].simToma.state = tomaState_not_ready;		// Toma 0 will not let vol 0 and 1 to register
	reset_cli_status_verification(client);						// Here we only wait for IO enabled on 2 volumes
	set_cli_status_verification_expector(client, attach_string_no_io(&client->vols[0]), 0);
	set_cli_status_verification_expector(client, attach_string_no_io(&client->vols[1]), 1);
	send_command_to_all(sys, -1, volCmds_New);	 					// Attach without IO
	clientSimulator_wait_for_mainwq(client);		 // Drain main wq - to allow config-fs/MCS message of IO enabling to arrive
	for (v=0; v<client->nBdevs; v++) // Volumes 0,1 use Toma 1, Volumes 2,3 dont use this Toma so they have IO enabled
		BUG_ON((client->devs[v]->status == NCBD_ATTACHING_NO_IO) != (v<2));

	reset_cli_status_verification(client);
	set_cli_status_verification_expector(client, attach_string(&client->vols[0]), 0);
	set_cli_status_verification_expector(client, attach_string(&client->vols[1]), 1);

	tomaSimulator_reconnect(&sys->servers[0].simToma);
	NVMeshSystem_serialize(sys);								// Wait for all attaches to finish
	wait_for_cli_status_verification(client);

	for (v=0; v<client->nBdevs; v++)
		BUG_ON((client->devs[v]->status == NCBD_ATTACHING_NO_IO));	// All volumes have IO enabled

	// -----------------------------------							// Test IO enabling during attach with Toma out of lock-ids (al lock-ids are taken)
	if (1) {
		struct tomaSimulator *toma = &sys->servers[2].simToma;
		for (v=0; v<client->nBdevs; v++)
			client->devs[v]->topologies.dbg_num_enabling_io_toggles = 1;
		send_command_to_vol(sys, -1, 0, volCmds_Detach);                // Detach volumes {0,2} that use Toma 2
		send_command_to_vol(sys, -1, 2, volCmds_Detach);
		toma->state = tomaState_no_free_lock_ids;					// Toma 2 will not let vol 0 and 2 to register, coz it does not have lock_ids. After a few retries it will let the register
		send_command_to_vol(sys, -1, 0, volCmds_New);					// Attach vol '0'
		tomaSimulator_clean_rejected_lids(toma);
		for (v=0; v<client->nBdevs; v++) { 							// Volumes {1,3} are untached and keep having IO enabled
			if (v== 2) continue;
			BUG_ON(!nvmeibc_topo_is_io_ok(&client->devs[v]->topologies));
			BUG_ON(client->devs[v]->topologies.dbg_num_enabling_io_toggles != 1);
		}

		toma->state = tomaState_no_free_lock_ids;					// Toma 2 will not let vol 0 and 2 to register, coz it does not have lock_ids. After a few retries it will let the register
		send_command_to_vol(sys, -1, 2, volCmds_New);					// Attach vol '2', volume '0' is still trying to get register with Toma '2'
		tomaSimulator_clean_rejected_lids(toma);
		for (v=0; v<client->nBdevs; v++) { 							// Volumes {1,3} are untached and keep having IO enabled
			BUG_ON(!nvmeibc_topo_is_io_ok(&client->devs[v]->topologies));
			BUG_ON(client->devs[v]->topologies.dbg_num_enabling_io_toggles != 1);
		}

	}
	// -----------------------------------						// Delete a volume from config simulate volume deletion message
	send_command_to_vol(sys, -1, 0, volCmds_Delete);
	BUG_ON(client->devs[0] != NULL);

	// Before re-attaching the volumes generate complete configuration and ensure only 3 volumes get status update
	reset_cli_status_verification(client);
	for (volInd = 1; volInd < client->nBdevs; volInd++) {
		set_cli_status_verification_expector(client, update_string(&client->vols[volInd]), volInd);
	}
	// Replace with %get_full_conf when that branch is included
	clientSimulator_send_to_cli_and_wait(client, "%get_full_conf");
	expected_num_of_executed_ioctls++;
	BUG_ON(client->devs[0] != NULL);

	// Detach all remaining volumes (also send another delete)
	send_command_to_all(sys, -1, volCmds_Detach);
	for (volInd = 0; volInd < client->nBdevs; volInd++) {
		BUG_ON(client->devs[volInd] != NULL);
	}

	// Before re-attaching all of the volumes generate complete configuration and ensure no update occurs
	reset_cli_status_verification(client);
	// Replace with %get_full_conf
	clientSimulator_send_to_cli_and_wait(client, "%get_full_conf");
	expected_num_of_executed_ioctls++;
	for (volInd = 0; volInd < client->nBdevs; volInd++) {
		BUG_ON(client->devs[volInd] != NULL);
	}

	send_command_to_all(sys, -1, volCmds_New); // Re-attach all of the volumes
	for (volInd = 0; volInd < client->nBdevs; volInd++)
		BUG_ON(client->devs[volInd] == NULL);
	BUG_ON(!NVMeshSystem_is_stable(sys));
	BUG_ON(ioctl_num + expected_num_of_executed_ioctls != clientSimulator_get_num_executed_ioctls(client));
	_NI_dmesg(trace_uni_scenario_vol_config_unitest_DetachAttachVolume, "*************** end");
	return rv;
}

TEST_FUNC int unitest_delayed_volume_reboot(struct NVMeshSystem *sys) {
	const int volInd = 0, rv = 0;
	struct clientSimulator *client = &sys->clients[0];	// Test via the first client
	struct nvmeibc_block_device	*dev = client->devs[volInd];
	struct nvmeibc_recoveries_drainer *drainer;
	struct volumeDescriptor *vol =	  &sys->mdb.vols[volInd];
	struct tTopoOfVolume    *vol_cf = &sys->tcf.vols[volInd];
	struct nvmeibc_topology *t;
	const bool force_reconf_reboot_backup = force_reconf_reboot;	// Restore force_reconf_reboot value after test
	force_reconf_reboot = true;								// force_reconf_reboot must be true for this test

	if (dev == NULL) {	// If not attached attach the volume
		send_command_to_vol(sys, -1, volInd, volCmds_New);
		dev = client->devs[volInd];
	}

	drainer = &dev->dp.running_recovs;

	// Issue 2 reboots. Both will be stuck:
	// first one waiting for recoveries to end,
	// and the second waiting for the first.

	// Set expectors for successful update after releasing the first reboot
	reset_cli_status_verification(client);

	// Purposefully force reboot to wait until we release it, by holding the number of running recoveries
	nvmeibc_recovs_drainer_inc_weak(drainer);

	// Issue 1st reboot.
	__unitest_volume_config_version_inc(vol, vol_cf);
	generate_mcs_attach_message_for_volume(&sys->mgmt.mcs[client->inst_id], vol,
										   MCS_ATTACH_VOLUMES_MESSAGE_MSG, false,
										   MAGIC_CONFIG_UPDATE_TOKEN, NVMEIB_MCS_MSG_WITH_NO_ERROR);

	// Set CLI expector for completion of the 2nd CLI
	// (not the reboot, which still waits).
	set_cli_status_verification_expector(client, update_string(vol), volInd);

	// Issue 2nd reboot.
	__unitest_volume_config_version_inc(vol, vol_cf);
	generate_mcs_attach_message_for_volume(&sys->mgmt.mcs[client->inst_id], vol,
										   MCS_ATTACH_VOLUMES_MESSAGE_MSG, false,
										   MAGIC_CONFIG_UPDATE_TOKEN, NVMEIB_MCS_MSG_WITH_NO_ERROR);

	// Expected version will be verified here
	wait_for_cli_status_verification(client);

	NVMeshSystem_serialize(sys);

	// Verify that second reboot did not complete
	t = nvmeibc_topology_get(&dev->topologies);
	BUG_ON((int)t->configuration_version >= vol_cf->version);
	nvmeibc_topology_put(t);

	// Release the 1st reboot or allow both to complete
	nvmeibc_recovs_drainer_dec_weak(drainer);

	NVMeshSystem_serialize(sys);

	// Verify that second reboot completed
	t = nvmeibc_topology_get(&dev->topologies);
	BUG_ON((int)t->configuration_version != vol_cf->version);
	nvmeibc_topology_put(t);

	BUG_ON(!NVMeshSystem_is_stable(sys));
	// Restore the original value of 'force_reconf_reboot'
	force_reconf_reboot = force_reconf_reboot_backup;
	return rv;
}


static inline enum volumeCommands __get_command_by_mode(enum_reservation_mode mode) {
	if (mode == NVMEIB_C_TO_M_VOLUME_ACTION_REQUEST_RO) {
		return volCmds_AttachReadOnly;
	} else if (mode == NVMEIB_C_TO_M_VOLUME_ACTION_REQUEST_RW) {
		return volCmds_New;
	} else if (mode == NVMEIB_C_TO_M_VOLUME_ACTION_REQUEST_EX) {
		return volCmds_AttachExclusive;
	}
	BUG();
	return volCmds_Illegal;
}


int verify_reservation_mode_io(struct clientSimulator *client, int volInd, int startBlock, int lenBlocks, u8 *mem, u64* magic_pattern, enum_reservation_mode mode){
	int rv;
	if (mode == NVMEIB_C_TO_M_VOLUME_ACTION_REQUEST_RO) { // Verify write/trim fails and read succeeds
		rv = osSimulator_writeArrWait(&client->OS, volInd, startBlock, lenBlocks, mem);		REPORT_ERROR(rv);	// Not executed
		BUG_ON(osSimulator_rv_of_last_io_get(&client->OS, volInd) != -EINVAL);
		rv = osSimulator_trimWait(	  &client->OS, volInd, startBlock, lenBlocks);			REPORT_ERROR(rv);	// Not executed
		BUG_ON(osSimulator_rv_of_last_io_get(&client->OS, volInd) != -EINVAL);
		rv = osSimulator_readArrWait( &client->OS, volInd, startBlock, lenBlocks, mem);		REPORT_ERROR(rv);	// Read the original magic
		BUG_ON(osSimulator_rv_of_last_io_get(&client->OS, volInd));
		__unitest_verify_blocks_pattern(mem, lenBlocks, *magic_pattern, false);					// Verify that read and write matched.
	} else { // Verify all IO types, trim, read 0, write, read
		rv = osSimulator_trimWait(	  &client->OS, volInd, startBlock, lenBlocks);			REPORT_ERROR(rv);	// Not executed
		BUG_ON(osSimulator_rv_of_last_io_get(&client->OS, volInd));
		rv = osSimulator_readArrWait( &client->OS, volInd, startBlock, lenBlocks, mem);		REPORT_ERROR(rv);	// Read the trim data
		BUG_ON(osSimulator_rv_of_last_io_get(&client->OS, volInd));
		__unitest_verify_blocks_pattern(mem, lenBlocks, (u64)0, false);					// Verify that read and write matched.
		*magic_pattern = __unitest_fill_blocks_unique_pattern(mem, lenBlocks);
		rv = osSimulator_writeArrWait(&client->OS, volInd, startBlock, lenBlocks, mem);		REPORT_ERROR(rv);	// Not executed
		BUG_ON(osSimulator_rv_of_last_io_get(&client->OS, volInd));
		rv = osSimulator_readArrWait( &client->OS, volInd, startBlock, lenBlocks, mem);		REPORT_ERROR(rv);	// Read the original magic
		BUG_ON(osSimulator_rv_of_last_io_get(&client->OS, volInd));
		__unitest_verify_blocks_pattern(mem, lenBlocks, *magic_pattern, false);					// Verify that read and write matched.
	}
	return rv;
}

static void __test_hidden_attach_must_always_succeed(struct NVMeshSystem *sys, int volInd) {
	struct clientSimulator *client = &sys->clients[0];			// Test via the first client
	const struct nvmeibc_reservation *reserv;
	send_command_to_vol(sys, -1, volInd, volCmds_RecoveryAttach);
	NVMeshSystem_serialize(sys);
	BUG_ON(!clientSimulator_is_vol_hidden_attached(client, volInd));
	BUG_ON((client->devs[volInd]->topologies.io_perm != NVMEIB_IO_TYPE_PERMIT_NO_IO));
	reserv = &nvmeibc_block_get_res_vat(client->devs[volInd])->res;
	BUG_ON((reserv->version != RESERVATION_MODE_IRRELEVANT) || (reserv->mode != NVMEIB_C_TO_M_VOLUME_ACTION_REQUEST_RC) || (reserv->preempt != NVMEIB_C_TO_M_VOLUME_PREEMPT_UNKNOWN));
	send_command_to_vol(sys, -1, volInd, volCmds_RecoveryDetach);
	NVMeshSystem_serialize(sys);
	BUG_ON(client->devs[volInd] != NULL);
}

TEST_FUNC int unitest_VolumeReservation(struct NVMeshSystem *sys){
	int volInd = 0, rv = 0, conf_ver = 0;
	u64 cur_version = 0;										// Follow the reservation version for the volumes so we can expect it
	struct clientSimulator *client = &sys->clients[0];			// Test via the first client
	enum volumeCommands* nextCmd = &sys->mdb.vols[volInd].nextCmd;		// Experiment with volume 0
	enum_reservation_mode mode, other_mode;
	const int startBlock = 17, lenBlocks = 1;					// 1[Block]
	const int memSize	 = lenBlocks*NVMEIBC_SECTOR_SIZE;
	u8        *mem 		 = malloc(memSize);						// Array to read/write to disk
	u64 magic_pattern = __unitest_fill_blocks_unique_pattern(mem, lenBlocks);	// Set a pattern.
	const struct nvmeibc_reservation *reserv;
	rv = osSimulator_writeArr(&client->OS, volInd, startBlock, lenBlocks, mem);		REPORT_ERROR(rv);
	clientSimulator_wait_for_all_bio_ops(client);

	// Detach volume
	send_command_to_vol(sys, -1, volInd, volCmds_Detach);
	NVMeshSystem_serialize(sys);

	// -----------------------------------
	// Reset mgmt status on volume (set RV=0, RM=None)
	for (mode=NVMEIB_C_TO_M_VOLUME_ACTION_REQUEST_RO;mode<=NVMEIB_C_TO_M_VOLUME_ACTION_REQUEST_EX;mode++) { // Attach volume to client with SHARED mode will not allow other attaches with different modes
		const unsigned int expected_preempt = (mode == NVMEIB_C_TO_M_VOLUME_ACTION_REQUEST_EX) ? NVMEIB_C_TO_M_VOLUME_WEAK_PREEMPT : NVMEIB_C_TO_M_VOLUME_NO_PREEMPT;
		cur_version = 0;	// Follow the reservation version for the volumes so we can expect it
		__unitest_volume_reservation_reset(&sys->mdb.vols[volInd], &sys->tcf.vols[volInd]);
		// Attach with mode
		cur_version++;
		send_command_to_vol(sys, -1, volInd, __get_command_by_mode(mode));
		NVMeshSystem_serialize(sys);
		BUG_ON(!nvmeibc_topo_is_io_ok(&client->devs[volInd]->topologies));
		reserv = &nvmeibc_block_get_res_vat(client->devs[volInd])->res;
		BUG_ON((reserv->version != cur_version) || (reserv->mode != mode) || (reserv->preempt != expected_preempt));

		if (1) {	// Test Illegal mgmt messages related to reservation version
			const bool prev = nvmeibc_warn_on_mgmt_wrong_msg_logic; nvmeibc_warn_on_mgmt_wrong_msg_logic = false;
			// Send Different mode without version increase
			BUG_ON((sys->mdb.vols[volInd].vat.res.mode != mode) || (sys->mdb.vols[volInd].vat.res.version != 1));
			sys->mdb.vols[volInd].vat.res.mode = (((mode+1)%3)+1);	// Simple caluclation to generate different reservation mode, but never 0 {1,2,3} -> {3,1,2}
			BUG_ON(sys->mdb.vols[volInd].vat.res.mode == mode);
			generate_mcs_attach_message_for_volume(&sys->mgmt.mcs[client->inst_id], &sys->mdb.vols[volInd], MCS_ATTACH_VOLUMES_MESSAGE_MSG, false, MAGIC_CONFIG_UPDATE_TOKEN, NVMEIB_MCS_MSG_WITH_NO_ERROR);
			NVMeshSystem_serialize(sys);
			BUG_ON((reserv->version != cur_version) || (reserv->mode != mode) || (reserv->preempt != expected_preempt));
			sys->mdb.vols[volInd].vat.res.mode = mode;

			// Send preempt without version increase
			generate_mcs_attach_message_for_volume(&sys->mgmt.mcs[client->inst_id], &sys->mdb.vols[volInd], MCS_ATTACH_VOLUMES_MESSAGE_MSG, NVMEIB_C_TO_M_VOLUME_PREEMPT, MAGIC_CONFIG_UPDATE_TOKEN, NVMEIB_MCS_MSG_WITH_NO_ERROR);
			NVMeshSystem_serialize(sys);
			BUG_ON((reserv->version != cur_version) || (reserv->mode != mode) || (reserv->preempt != expected_preempt));

			// Send msg with smaller reservation version than what client has
			*((unsigned long long*)&reserv->version) += 1000;
			generate_mcs_attach_message_for_volume(&sys->mgmt.mcs[client->inst_id], &sys->mdb.vols[volInd], MCS_ATTACH_VOLUMES_MESSAGE_MSG, false, MAGIC_CONFIG_UPDATE_TOKEN, NVMEIB_MCS_MSG_WITH_NO_ERROR);
			NVMeshSystem_serialize(sys);
			*((unsigned long long*)&reserv->version) -= 1000;
			BUG_ON((reserv->version != cur_version) || (reserv->mode != mode) || (reserv->preempt != expected_preempt));
			nvmeibc_warn_on_mgmt_wrong_msg_logic = prev;
		}

		// Verify update is not rejected when reservation version is the same
		conf_ver = sys->mdb.vols[volInd].info.version;
		BUG_ON(clientSimulator_get_volume_version(client, volInd) != conf_ver);
		__unitest_volume_config_version_inc(&sys->mdb.vols[volInd], &sys->tcf.vols[volInd]);
		conf_ver++;
		send_command_to_vol_attach_or_update(sys, -1, volInd);
		BUG_ON(clientSimulator_get_volume_version(client, volInd) != conf_ver);

		// Verify update is rejected when reservation version increases without increase in volume version
		__unitest_volume_reservation_inc(&sys->mdb.vols[volInd], &sys->tcf.vols[volInd]);
		reset_cli_status_verification(client); set_cli_status_verification_expector(client, update_string_reject_reserv(&client->vols[volInd]), volInd);
		send_command_to_vol_attach_or_update(sys, -1, volInd);
		BUG_ON(clientSimulator_get_volume_version(client, volInd) != conf_ver);
		reserv = &nvmeibc_block_get_res_vat(client->devs[volInd])->res;
		BUG_ON(clientSimulator_get_volume_version(client, volInd) != conf_ver);
		BUG_ON(reserv->version != (sys->mdb.vols[volInd].vat.res.version -1));

		// When vol version and reservation version increase: Verify, client accepts new conf but stays with previous reservation version.
		__unitest_volume_config_version_inc(&sys->mdb.vols[volInd], &sys->tcf.vols[volInd]);
		__unitest_volume_reservation_inc(&sys->mdb.vols[volInd], &sys->tcf.vols[volInd]);
		BUG_ON(clientSimulator_get_volume_version(client, volInd) != conf_ver);
		generate_mcs_attach_message_for_volume(&sys->mgmt.mcs[client->inst_id], &sys->mdb.vols[volInd], MCS_ATTACH_VOLUMES_MESSAGE_MSG, false, MAGIC_CONFIG_UPDATE_TOKEN, NVMEIB_MCS_MSG_WITH_NO_ERROR);
		NVMeshSystem_serialize(sys);
		BUG_ON(clientSimulator_get_volume_version(client, volInd) != conf_ver + 1);
		reserv = &nvmeibc_block_get_res_vat(client->devs[volInd])->res;
		BUG_ON(reserv->version == sys->mdb.vols[volInd].vat.res.version);
		BUG_ON((reserv->version != cur_version) || (reserv->mode != mode));

		//force_reconf_reboot is the game changer
		//Regardless force_reconf_reboot value, the client overwrites bigger reservation mode version with with what it has.
		//In hot reconfigure, the segments in the good state, so no need to send unregister/register messages. So, client does not update "max seen reserv version".
		//Thus, just decrementing reservation mode version allows to preserve I/O permissions. This is not true in case cold update. The client will get reservation version from toma, will update maximum and will never go back.
		if (force_reconf_reboot == true) {
			//^^^ the code above introduced reservation version mismatch, client will get new configuration from toma and will disallow any I/O
			//__update_reservation_version_max_seen
			BUG_ON(client->devs[volInd]->topologies.io_perm != NVMEIB_IO_TYPE_PERMIT_NO_RM_IO);
			BUG_ON(client->devs[volInd]->topologies.reservation_version_max_seen != 3);
		} else {
			BUG_ON(client->devs[volInd]->topologies.reservation_version_max_seen != 1);
			verify_reservation_mode_io(client, volInd, startBlock, lenBlocks, mem, &magic_pattern, mode);
		}
		// Revert reservation version and update again
		__unitest_volume_reservation_dec(&sys->mdb.vols[volInd], &sys->tcf.vols[volInd]);
		__unitest_volume_reservation_dec(&sys->mdb.vols[volInd], &sys->tcf.vols[volInd]);
		conf_ver++;
		send_command_to_vol_attach_or_update(sys, -1, volInd);
		BUG_ON(clientSimulator_get_volume_version(client, volInd) != conf_ver);
		if (force_reconf_reboot == true){
			BUG_ON(client->devs[volInd]->topologies.io_perm != NVMEIB_IO_TYPE_PERMIT_NO_RM_IO);
			BUG_ON(client->devs[volInd]->topologies.reservation_version_max_seen != 3);
		} else {
			// Test IO
			BUG_ON(client->devs[volInd]->topologies.reservation_version_max_seen != 1);
			verify_reservation_mode_io(client, volInd, startBlock, lenBlocks, mem, &magic_pattern, mode);
		}

		// Detach, verify attach with other mode fails and this mode succeeds (except EX, see code below)
		send_command_to_vol(sys, -1, volInd, volCmds_Detach);
		NVMeshSystem_serialize(sys);
		if (mode != NVMEIB_C_TO_M_VOLUME_ACTION_REQUEST_EX) {
			for (other_mode=NVMEIB_C_TO_M_VOLUME_ACTION_REQUEST_RO;other_mode<=NVMEIB_C_TO_M_VOLUME_ACTION_REQUEST_EX;other_mode++) {
				if (other_mode==mode) {
					continue;
				}
				reset_cli_status_verification(client);
				set_cli_status_verification_expector(client, reservation_mode_denied_string(&client->vols[volInd], true), volInd);
				// Set the next command and attch with uuid
				*nextCmd = __get_command_by_mode(other_mode);
				clientSimulator_get_volumes_config(client, &sys->mgmt, 'u', false, RESERVATION_MODE_IRRELEVANT, false);
				NVMeshSystem_serialize(sys);
				*nextCmd = volCmds_Illegal;
				BUG_ON(client->devs[volInd] != NULL);
			}
		} else
			cur_version++; // Detach increases version

		__test_hidden_attach_must_always_succeed(sys, volInd);

		// Reattach with mode and expect success (For EX we will have to use a real cur_version (the first is 1 and used throught our unitests)
		reset_cli_status_verification(client);
		set_cli_status_verification_expector(client, attach_string_rv(&client->vols[volInd], CLI_ATTACHED, cur_version), volInd);

		send_command_to_vol(sys, -1, volInd, __get_command_by_mode(mode));
		NVMeshSystem_serialize(sys);
		BUG_ON(!nvmeibc_topo_is_io_ok(&client->devs[volInd]->topologies));
		reserv = &nvmeibc_block_get_res_vat(client->devs[volInd])->res;
		BUG_ON((reserv->version != cur_version) || (reserv->mode != mode) || (reserv->preempt != expected_preempt));

		// Test IO
		verify_reservation_mode_io(client, volInd, startBlock, lenBlocks, mem, &magic_pattern, mode);
		// Detach
		send_command_to_vol(sys, -1, volInd, volCmds_Detach);
		NVMeshSystem_serialize(sys);
		cur_version++;

		if (mode != NVMEIB_C_TO_M_VOLUME_ACTION_REQUEST_EX) { // Already tested
			continue;
		}

		// After detaching EX we should be able to attach with all other modes again (once, test --preempt)
		reset_cli_status_verification(client);
		set_cli_status_verification_expector(client, attach_string_rv(&client->vols[volInd], CLI_ATTACHED, cur_version), volInd);
		// Set the next command and attach
		other_mode = NVMEIB_C_TO_M_VOLUME_ACTION_REQUEST_RW;
		*nextCmd = __get_command_by_mode(other_mode);
		clientSimulator_get_volumes_config(client, &sys->mgmt, 'r', false, RESERVATION_MODE_IRRELEVANT, false);
		NVMeshSystem_serialize(sys);
		*nextCmd = volCmds_Illegal;
		BUG_ON(!nvmeibc_topo_is_io_ok(&client->devs[volInd]->topologies));
		reserv = &nvmeibc_block_get_res_vat(client->devs[volInd])->res;
		BUG_ON((reserv->version != cur_version) || (reserv->mode != other_mode) || (reserv->preempt != NVMEIB_C_TO_M_VOLUME_NO_PREEMPT));

		// Test IO
		verify_reservation_mode_io(client, volInd, startBlock, lenBlocks, mem, &magic_pattern, mode);
		send_command_to_vol(sys, -1, volInd, volCmds_Detach);

		// First verify attach is rejected then use --preempt
		reset_cli_status_verification(client);
		set_cli_status_verification_expector(client, reservation_mode_denied_string(&client->vols[volInd], false), volInd);
		// Set the next command and attach with name
		*nextCmd = __get_command_by_mode(NVMEIB_C_TO_M_VOLUME_ACTION_REQUEST_RO);
		clientSimulator_get_volumes_config(client, &sys->mgmt, 'v', false, RESERVATION_MODE_IRRELEVANT, false);
		NVMeshSystem_serialize(sys);
		*nextCmd = volCmds_Illegal;
		BUG_ON(client->devs[volInd] != NULL);

		__test_hidden_attach_must_always_succeed(sys, volInd);

		// Test using preempt succeeds if the given RV is the same as mgmt (0 will fetch from mgmt) and increases the version by 1
		for (other_mode=NVMEIB_C_TO_M_VOLUME_ACTION_REQUEST_RO;other_mode<=NVMEIB_C_TO_M_VOLUME_ACTION_REQUEST_EX;other_mode++) {
			cur_version++;
			reset_cli_status_verification(client);

			set_cli_status_verification_expector(client, attach_string_rv(&client->vols[volInd], CLI_ATTACHED, cur_version), volInd);
			// Set the next command and attach
			*nextCmd = __get_command_by_mode(other_mode);
			clientSimulator_get_volumes_config(client, &sys->mgmt, 'r', true, RESERVATION_MODE_IRRELEVANT, false);
			NVMeshSystem_serialize(sys);
			*nextCmd = volCmds_Illegal;
			BUG_ON(!nvmeibc_topo_is_io_ok(&client->devs[volInd]->topologies));
			reserv = &nvmeibc_block_get_res_vat(client->devs[volInd])->res;
			BUG_ON((reserv->version != cur_version) || (reserv->mode != other_mode) || (reserv->preempt != NVMEIB_C_TO_M_VOLUME_PREEMPT));

			// Verify preempted volumes can update configuration if reservation version doesn't change
			conf_ver = sys->mdb.vols[volInd].info.version;
			BUG_ON(clientSimulator_get_volume_version(client, volInd) != conf_ver);
			__unitest_volume_config_version_inc(&sys->mdb.vols[volInd], &sys->tcf.vols[volInd]);
			conf_ver++;
			reset_cli_status_verification(client);
			set_cli_status_verification_expector(client, attach_string_rv(&client->vols[volInd], CLI_ATTACHED, cur_version), volInd);
			send_command_to_vol_attach_or_update(sys, -1, volInd);
			BUG_ON(clientSimulator_get_volume_version(client, volInd) != conf_ver);

			// When vol version and reservation version increase: Verify, client accepts new conf but stays with previous reservation version.
			__unitest_volume_config_version_inc(&sys->mdb.vols[volInd], &sys->tcf.vols[volInd]);
			__unitest_volume_reservation_inc(&sys->mdb.vols[volInd], &sys->tcf.vols[volInd]);
			BUG_ON(clientSimulator_get_volume_version(client, volInd) != conf_ver);
			generate_mcs_attach_message_for_volume(&sys->mgmt.mcs[client->inst_id], &sys->mdb.vols[volInd], MCS_ATTACH_VOLUMES_MESSAGE_MSG, false, MAGIC_CONFIG_UPDATE_TOKEN, NVMEIB_MCS_MSG_WITH_NO_ERROR);
			NVMeshSystem_serialize(sys);
			BUG_ON(clientSimulator_get_volume_version(client, volInd) != conf_ver + 1);
			reserv = &nvmeibc_block_get_res_vat(client->devs[volInd])->res;
			BUG_ON(reserv->version == sys->mdb.vols[volInd].vat.res.version);
			BUG_ON((reserv->version != cur_version) || (reserv->mode != other_mode));

			// Revert reservation version and update again
			__unitest_volume_reservation_dec(&sys->mdb.vols[volInd], &sys->tcf.vols[volInd]);
			conf_ver++;
			reset_cli_status_verification(client);
			set_cli_status_verification_expector(client, attach_string_rv(&client->vols[volInd], CLI_ATTACHED, cur_version), volInd);
			send_command_to_vol_attach_or_update(sys, -1, volInd);
			BUG_ON(clientSimulator_get_volume_version(client, volInd) != conf_ver);

			if (force_reconf_reboot == true){
				BUG_ON(client->devs[volInd]->topologies.io_perm != NVMEIB_IO_TYPE_PERMIT_NO_RM_IO);
			} else {
				// Test IO
				verify_reservation_mode_io(client, volInd, startBlock, lenBlocks, mem, &magic_pattern, other_mode);
			}

			send_command_to_vol(sys, -1, volInd, volCmds_Detach);
			NVMeshSystem_serialize(sys);
			BUG_ON(client->devs[volInd] != NULL);
		}
		// Last detach from EX
		cur_version++;
	}	// for (mode=

	__test_hidden_attach_must_always_succeed(sys, volInd);
	if (1) {	// Test volume requests attach with lower version
		int preempt;
		for (preempt = false;preempt<=true;preempt++) {
			for (other_mode = NVMEIB_C_TO_M_VOLUME_ACTION_REQUEST_RO; other_mode <= NVMEIB_C_TO_M_VOLUME_ACTION_REQUEST_EX; other_mode++) {
				reset_cli_status_verification(client);
				set_cli_status_verification_expector(client, reservation_denied_string(&client->vols[volInd], false), volInd);
				*nextCmd = __get_command_by_mode(other_mode);
				clientSimulator_get_volumes_config(client, &sys->mgmt, 'v', preempt, 2, false);
				NVMeshSystem_serialize(sys);
				*nextCmd = volCmds_Illegal;
			}
		}
	}

	if (1) {	// Test TOMA Version increase + unregister, IO becomes disabled
		struct tTopoOfVolume *cfv = &sys->tcf.vols[volInd];
		struct tTopoOfPraid* r1 = tTopoOfVolume_getRaid1(cfv, 0);
		const struct nvmeibc_block_device *dev;
		for (other_mode=NVMEIB_C_TO_M_VOLUME_ACTION_REQUEST_RO;other_mode<=NVMEIB_C_TO_M_VOLUME_ACTION_REQUEST_EX;other_mode++) {
			cur_version++;
			reset_cli_status_verification(client);
			set_cli_status_verification_expector(client, attach_string_rv(&client->vols[volInd], CLI_ATTACHED, cur_version), volInd);
			*nextCmd = __get_command_by_mode(other_mode);
			clientSimulator_get_volumes_config(client, &sys->mgmt, 'r', true, RESERVATION_MODE_IRRELEVANT, false);
			NVMeshSystem_serialize(sys);
			*nextCmd = volCmds_Illegal;
			dev = client->devs[volInd];
			BUG_ON(!nvmeibc_topo_is_io_ok(&dev->topologies));
			reserv = &nvmeibc_block_get_res_vat(dev)->res;
			BUG_ON((reserv->version != cur_version) || (reserv->mode != other_mode));
			verify_reservation_mode_io(client, volInd, startBlock, lenBlocks, mem, &magic_pattern, other_mode);

			tomaSimulator_send_unreg_to_raid1_increase_reservation_version(r1->header.uuid);
			NVMeshSystem_serialize(sys);
			BUG_ON((dev->topologies.io_perm != NVMEIB_IO_TYPE_PERMIT_NO_RM_IO) || !nvmeibc_block_status_is_preempted(dev->status));
			BUG_ON((reserv->version != cur_version) || (reserv->mode != other_mode));
			send_command_to_vol(sys, -1, volInd, volCmds_Detach);
			NVMeshSystem_serialize(sys);
			BUG_ON(client->devs[volInd] != NULL);
		}
	}

	free(mem);
	// Done! Detach all volumes and re-attach after resetting the status
	__unitest_volume_reservation_reset(&sys->mdb.vols[volInd], &sys->tcf.vols[volInd]);

	send_command_to_vol(sys, -1, volInd, volCmds_New);
	NVMeshSystem_serialize(sys);
	_NI_dmesg(t_unitestVolumeReservation0, "*************** end");
	return rv;
}

TEST_FUNC int unitest_AttachExistingVolume(struct NVMeshSystem *sys) {
	int volInd = 0, otherVolInd = volInd+1;
	enum volumeCommands* nextCmd = &sys->mdb.vols[volInd].nextCmd;		// Experiment with volume 0
	struct clientSimulator *client = &sys->clients[0];				// Test via the first client
	char new_devname[NVMEIBC_BD_NAME_LEN];
	char old_devname[NVMEIBC_BD_NAME_LEN];

	strcpy(new_devname, sys->mdb.vols[otherVolInd].info.devname);		// Get vol 0 name and vol 1 name
	strcpy(old_devname, sys->mdb.vols[volInd].info.devname);
	__unitest_volume_config_version_inc(&sys->mdb.vols[otherVolInd], &sys->tcf.vols[otherVolInd]);

	send_command_to_vol(sys, -1, volInd, volCmds_Detach);				// Detach 0'th volume
	strcpy(sys->mdb.vols[volInd].info.devname, new_devname);			// Replace volume volInd devname with volume otherVolInd devname
	*nextCmd = volCmds_New;
	reset_cli_status_verification(client);
	set_cli_status_verification_expector(client, failed_attach_string(&sys->mdb.vols[volInd], CLI_ATTACH_FAILED), volInd);
	clientSimulator_get_volumes_config(client, &sys->mgmt, 'u', false, RESERVATION_MODE_IRRELEVANT, false);	// Reattach and fail. Volume with that name already exists
	NVMeshSystem_serialize(sys);
	BUG_ON(client->devs[volInd] != NULL);

	strcpy(sys->mdb.vols[volInd].info.devname, old_devname);			// Revert volume 0 devname back to original name and reattach
	send_command_to_vol(sys, -1, volInd, volCmds_New);
	__unitest_volume_config_version_dec(&sys->mdb.vols[otherVolInd], &sys->tcf.vols[otherVolInd]);

	// Try to rename (update attached) volume to the name of another attached volume
	strcpy(sys->mdb.vols[volInd].info.devname, new_devname);
	*nextCmd = volCmds_Update;
	reset_cli_status_verification(client);
	set_cli_status_verification_expector(client, cli_generic_string(&client->vols[volInd], CLI_UPDATE_FAILED, false), volInd);
	clientSimulator_get_volumes_config(client, &sys->mgmt, 'u', false, RESERVATION_MODE_IRRELEVANT, false);	// Update and fail, must use uuid (renamed)
	NVMeshSystem_serialize(sys);
	*nextCmd = volCmds_Illegal;
	strcpy(sys->mdb.vols[volInd].info.devname, old_devname);

	// Detach volume 1 in order to allow volume rename
	sys->mdb.vols[otherVolInd].nextCmd = volCmds_Detach;
	clientSimulator_get_volumes_config(client, &sys->mgmt, 'u', false, RESERVATION_MODE_IRRELEVANT, false);	// Update and succeed, must use uuid (renamed)
	NVMeshSystem_serialize(sys);
	clientSimulator_wait_for_detach_drain(client);
	BUG_ON(client->devs[otherVolInd] != NULL);

	// Retry to run the update command and verify the name change
	*nextCmd = volCmds_Update;
	strcpy(sys->mdb.vols[volInd].info.devname, new_devname);
	__unitest_volume_config_version_inc(&sys->mdb.vols[volInd], &sys->tcf.vols[volInd]);
	clientSimulator_get_volumes_config(client, &sys->mgmt, 'u', false, RESERVATION_MODE_IRRELEVANT, false);
	NVMeshSystem_serialize(sys);
	BUG_ON(strcmp(client->devs[volInd]->name, new_devname));

	// Rechange the name back and re-attach volume 1
	strcpy(sys->mdb.vols[volInd].info.devname, old_devname);
	__unitest_volume_config_version_inc(&sys->mdb.vols[volInd], &sys->tcf.vols[volInd]);
	clientSimulator_get_volumes_config(client, &sys->mgmt, 'u', false, RESERVATION_MODE_IRRELEVANT, false);
	NVMeshSystem_serialize(sys);
	BUG_ON(strcmp(client->devs[volInd]->name, old_devname));

	__unitest_volume_config_version_dec(&sys->mdb.vols[otherVolInd], &sys->tcf.vols[otherVolInd]);		// Otherwise toma might ignore register
	sys->mdb.vols[otherVolInd].nextCmd = volCmds_New;
	clientSimulator_get_volumes_config(client, &sys->mgmt, 'u', false, RESERVATION_MODE_IRRELEVANT, false);
	NVMeshSystem_serialize(sys);
	BUG_ON(!client->devs[otherVolInd]);
	sys->mdb.vols[otherVolInd].nextCmd = volCmds_Illegal;

	// Revert volume 0 devname back to original for next tests
	strcpy(sys->mdb.vols[volInd].info.devname, old_devname);
	*nextCmd = volCmds_Illegal;
	return 0;
}

TEST_FUNC int unitest_RefIDs_OfExistingVolume(struct NVMeshSystem *sys) {
	int volInd = 0;													// Work with Volume zero Rad10
	struct volumeDescriptor *mdb_vol = &sys->mdb.vols[volInd];
	enum volumeCommands* nextCmd = &mdb_vol->nextCmd;
	struct clientSimulator *client = &sys->clients[0];				// Test via the first client
	char old_devname[NVMEIBC_BD_NAME_LEN];
	strcpy(old_devname, mdb_vol->info.devname);

	mdb_vol->expect_clnt_ref_ids_to_not_match_mongodb = true; // NVMESH-4797:
	send_command_to_vol(sys, -1, volInd, volCmds_Update);						// Just dummy update with prev ref_ids
	// Daniel: tests bellow actually change reference ID's, but cannot verify it
	// Client replies to with attachment.version field. So the reply to
	// volume update request will have latest ref_ids, but there are asyncronous messages
	// as well, periodic updates, io enabled/disabled (etc). So while we update
	// ref_ids, we can concurrently get a periodic status report message of volume with
	// old ref_ids. NVMESH-4845: Properly advance the per volume version and verify correct ref-ids
	volumeDescriptor_ref_ids_generate(mdb_vol);
	send_command_to_vol(sys, -1, volInd, volCmds_Update);						// Few updates with different ref_ids
	volumeDescriptor_ref_ids_generate(mdb_vol);
	send_command_to_vol(sys, -1, volInd, volCmds_Update);
	__unitest_volume_config_version_inc(&sys->mdb.vols[volInd], &sys->tcf.vols[volInd]);
	volumeDescriptor_ref_ids_generate(mdb_vol);									// Update ref_ids with configuration change together
	send_command_to_vol(sys, -1, volInd, volCmds_Update);

	// Update failure: with new ref_ids, Try to rename (update attached) volume to the name of another attached volume
	strcpy(mdb_vol->info.devname, sys->mdb.vols[volInd+1].info.devname);
	*nextCmd = volCmds_Update;
	reset_cli_status_verification(client);
	set_cli_status_verification_expector(client, cli_generic_string(&client->vols[volInd], CLI_UPDATE_FAILED, false), volInd);
	clientSimulator_get_volumes_config(client, &sys->mgmt, 'u', false, RESERVATION_MODE_IRRELEVANT, false);	// Update and fail, must use uuid (renamed)
	NVMeshSystem_serialize(sys);
	*nextCmd = volCmds_Illegal;
	mdb_vol->expect_clnt_ref_ids_to_not_match_mongodb = false;
	strcpy(mdb_vol->info.devname, old_devname);	// Revert volume 0 devname back to original for next tests
	return 0;
}

static void __cli_cmd_repeat_6_times_with_unique_token(struct clientSimulator *client, char* cmd, const bool is_attach) {
	char* last_char = NULL;
	int i;			// attachv <name> <token> --RW 0 -> (len-8 last token digit)
	if (is_attach) last_char = &cmd[strlen(cmd)-8];	// Points to last digit of cli token
					// cancel <token>					(len-1 last token digit)
	else           last_char = &cmd[strlen(cmd)-1];	// Points to last digit of cli token
	for (i = 0; i < 6; i++) {   // Daniel: Just a random number, assuming last digit starts from 1.
		(*last_char)++;		// Generate next digit
		clientSimulator_send_to_cli(client, cmd);
	}
}

TEST_FUNC int unitest_FS_InvalidCfg (struct NVMeshSystem *sys) {
#define INV_CFG_MAX_VOL_SEG             10 // maximum number of segment for a single volume
#define DISK_INDEX_INVALID       -1
	int volInd, rv, i;
	struct clientSimulator      *client	= &sys->clients[0];	// Current client
	struct volumeDescriptor     *vol;  						// Move segment of third volume
	const struct tTopoOfVolume	*cfg_vol;					// Toma Configuration of the current volume
	struct nvmeibc_block_disk   *disks;
	struct disk_range  *last_seg, *extra_seg;
	const char                  *expected_status;
	char token[16];
	char cli_command[NVMEIBC_BD_UUID_LEN + 8 + 17 + 39 + 36 /* Testing long uuids*/]; /* strlen("attachu ") + " token --XX rv --preempt" */
	// backup copy of the original config, before we modify it
	struct volumeDescriptor         backup_vol;// Save the volume so we will be able to restore it, easily.
	struct tTopoOfVolume	        backup_cfg_v;
	struct nvmeibc_block_disk       backup_disks[NVMESH_N_PHYS_DISKS];
	struct disk_range               backup_segs[INV_CFG_MAX_VOL_SEG]; // backup the segments vector we corrupt
	u64                             backup_allocation_end, backup_segment_length;
	const int ioctl_num = clientSimulator_get_num_executed_ioctls(client);	// will be compared with the value at the end of this test
	int expected_num_of_executed_ioctls = 0;
	//int    rv;
	// cause a missing segment mis-config
	enum missing_seg_pos_t {
		missing_seg_pos__first,
		missing_seg_pos__mid,
		missing_seg_pos__last,
	} missing_seg_pos;
	int    missing_seg_idx;
	__unitest_updowngrade_mode   mode;

	for (volInd=0; volInd<sys->clients[0].nBdevs; volInd++)				// Verify IO on all drives
		BUG_ON(!nvmeibc_topo_is_io_ok(&client->devs[volInd]->topologies));

	// 1) volume LBA range isnt fully covered by config - missing segment
	for (volInd = 0; volInd < client->nBdevs; volInd++) {
		vol 		= &sys->mdb.vols[volInd];
		cfg_vol    	= &sys->tcf.vols[volInd];
		disks      	= sys->mdb.discs[volInd];
		// save valid config
		memcpy(backup_disks, disks, sizeof(backup_disks));
		backup_vol = *vol;
		memcpy(&backup_cfg_v, cfg_vol, sizeof(backup_cfg_v));
		BUG_ON(vol->nSegments > INV_CFG_MAX_VOL_SEG);
		memcpy(backup_segs, vol->segs, sizeof(struct disk_range) * vol->nSegments);// backup the original segments vector

		for (missing_seg_pos = missing_seg_pos__first;
			 missing_seg_pos <= missing_seg_pos__last;
			 missing_seg_pos++) {
			// pick the segment to de missing
			switch (missing_seg_pos) {
			case missing_seg_pos__first:
				missing_seg_idx = 0;
				break;
			case missing_seg_pos__mid:
				missing_seg_idx = vol->nSegments / 2; // middle segemnt
				break;
			case missing_seg_pos__last:
				missing_seg_idx = vol->nSegments - 1; // last segemnt
				break;
			default:
				BUG();
			}
			for (mode = UNITEST_UPDOWNGRADE_COLD; mode <= UNITEST_UPDOWNGRADE_HOT; mode++) {
				if (mode == UNITEST_UPDOWNGRADE_COLD)
					send_command_to_vol(sys, -1, volInd, volCmds_Detach);
				mongo_db_simu_reconf_set_new(disks, vol->segs[missing_seg_idx].node_id, DISK_INDEX_INVALID, NULL, 0);
				memmove(&vol->segs[missing_seg_idx],
						&vol->segs[missing_seg_idx + 1],
						sizeof(vol->segs[0]) * (vol->nSegments - missing_seg_idx - 1));
				vol->nSegments--;
				memset(&vol->segs[vol->nSegments], 0, sizeof(vol->segs[0]));// make sure last isnt used by accident
				// try to apply the corrupted config
				__unitest_volume_config_version_inc(&sys->mdb.vols[volInd], &sys->tcf.vols[volInd]);
				reset_cli_status_verification(client);
				expected_status = (mode == UNITEST_UPDOWNGRADE_COLD) ? CLI_ATTACH_FAILED : CLI_UPDATE_FAILED;
				set_cli_status_verification_expector(client, failed_attach_string(vol, expected_status), volInd);
			    send_command_to_vol_attach_or_update(sys, -1, volInd);
#if 0 // extra debugging
				unitest_print("Config post corruption\n");
				nvmeibc_dump_volumes_disk_ids();
				nvmeibc_dump_disks_ids();
#endif
				/*
				 * verify we failed to apply the config:
				 *   1) on non raid1 volumes this should fail due to missing segment
				 *   2) on raid1, missing segment is assumed to be deprecated, hence valid
				 *   3) when first segment is missing, r1 can be converted to JBOD if the index of its pair is 0 instead of 1
				 */
				if (mode == UNITEST_UPDOWNGRADE_COLD) {
					BUG_ON(nvmeiba_os_apis_get_num('A') != MAX_NORMAL_VOLUMES_IN_NVMESH-1);		// on COLD we detached, but attach fails, so 1 volume missing
				} else {
					BUG_ON(nvmeiba_os_apis_get_num('A') != MAX_NORMAL_VOLUMES_IN_NVMESH);		// non COLD so we didnt detach. so failed config change leaves the older config, hence volume remains attached.
				}

				// restore the original config
				if (mode == UNITEST_UPDOWNGRADE_COLD)
					send_command_to_vol(sys, -1, volInd, volCmds_Detach);
				backup_cfg_v.version = (++backup_vol.info.version);
				memcpy(disks, backup_disks, sizeof(backup_disks));
				*vol = backup_vol;
				memcpy((void*)cfg_vol, &backup_cfg_v, sizeof(*cfg_vol));
				memcpy(vol->segs, backup_segs, sizeof(struct disk_range) * vol->nSegments);// backup the original segments vector
				// restore Mesh state with recovered valid config
				_NT(trace_uni_scenario_vol_config_unitest_FS_InvalidCfg, "Restore valid config: @VOL_I, vol->nSegments=@N_SEGMENTS", volInd, vol->nSegments);
				__unitest_volume_config_version_inc(&sys->mdb.vols[volInd], &sys->tcf.vols[volInd]);
				send_command_to_vol_attach_or_update(sys, -1, volInd);
				// validate the cluster is all valid
				BUG_ON(!NVMeshSystem_is_stable(sys));
				BUG_ON(nvmeiba_os_apis_get_num('A') != MAX_NORMAL_VOLUMES_IN_NVMESH);		// all volumes need to be attached
			}
		}
	}

	/*
	 * add an extra segment, beyond what the raid-type requires, to an existing chunk
	 * the stripe_index is out-of-range
	 */
	for (volInd = 0; volInd < client->nBdevs; volInd++) {
		vol 		= &sys->mdb.vols[volInd];
		cfg_vol    	= &sys->tcf.vols[volInd];
		disks      	= sys->mdb.discs[volInd];
		last_seg    = &vol->segs[vol->nSegments - 1];
		extra_seg   = last_seg+1;

		// save valid config
		memcpy(backup_disks, disks, sizeof(backup_disks));
		backup_vol = *vol;
		memcpy(&backup_cfg_v, cfg_vol, sizeof(backup_cfg_v));
		BUG_ON(vol->nSegments > INV_CFG_MAX_VOL_SEG);
		//memcpy(backup_segs, vol->segs, sizeof(struct disk_range) * vol->nSegments);// backup the original segments vector
		for (mode = UNITEST_UPDOWNGRADE_COLD; mode <= UNITEST_UPDOWNGRADE_HOT; mode++) {
			_NT(trace_1_uni_scenario_vol_config_unitest_FS_InvalidCfg, "Test extra Segment: @VOL_I, vol->nSegments=@N_SEGMENTS, mode=@MODE", volInd, vol->nSegments, mode);
			// allocate a range & backup disk allocation state to revert later on
			backup_allocation_end = sys->mdb.srvrs[last_seg->node_id].disk_alloc_end;
			disk_range_init(&sys->mdb, extra_seg, last_seg->bd_start /*bd_start*/,
							last_seg->node_id, last_seg->stripe_index+1/*stripe*/,
							last_seg->chunk_index, last_seg->volume_index,
							last_seg->replicas, last_seg->stripe_width, 1, 1, 1, true);
			BUG_ON(backup_allocation_end == sys->mdb.srvrs[last_seg->node_id].disk_alloc_end);
			_NT(trace_2_uni_scenario_vol_config_unitest_FS_InvalidCfg, "Extra Seg:disk_id=@DISK_ID, bd_start=@BD_START, stripe_index=@STRIPE_INDEX", extra_seg->node_id, extra_seg->bd_start, extra_seg->stripe_index);
			mongo_db_simu_reconf_set_new(disks, DISK_INDEX_INVALID, last_seg->node_id, last_seg, last_seg->dlba_start); // Instead of 1 last_seg set arrays of 2 segs {last_seg,extra_seg}
			vol->nSegments++;

			// try to apply the corrupted config
			if (mode == UNITEST_UPDOWNGRADE_COLD)
				send_command_to_vol(sys, -1, volInd, volCmds_Detach);
			__unitest_volume_config_version_inc(&sys->mdb.vols[volInd], &sys->tcf.vols[volInd]);
			reset_cli_status_verification(client);
			expected_status = (mode == UNITEST_UPDOWNGRADE_COLD) ? CLI_ATTACH_FAILED : CLI_UPDATE_FAILED;
			set_cli_status_verification_expector(client, failed_attach_string(vol, expected_status), volInd);
			send_command_to_vol_attach_or_update(sys, -1, volInd);

			// restore the original config
			backup_cfg_v.version = (++backup_vol.info.version);
			memcpy(disks, backup_disks, sizeof(backup_disks));
			*vol = backup_vol;
			memcpy((void*)cfg_vol, &backup_cfg_v, sizeof(*cfg_vol));
			//memcpy(vol->segs, backup_segs, sizeof(struct disk_range) * vol->nSegments);// restore the original segments vector from backup
			if (mode == UNITEST_UPDOWNGRADE_COLD)
				send_command_to_vol(sys, -1, volInd, volCmds_Detach);
			send_command_to_vol_attach_or_update(sys, -1, volInd);
			disk_range_destroy(extra_seg);
			sys->mdb.srvrs[last_seg->node_id].disk_alloc_end = backup_allocation_end;
			BUG_ON(!NVMeshSystem_is_stable(sys));
		}
	}
	BUG_ON(nvmeiba_os_apis_get_num('A') != MAX_NORMAL_VOLUMES_IN_NVMESH);

	unitest_trace_checkpoint(unitest_FS_InvalidCfg, modify_single_segment_length);

	// ---------------- Update Segment length of vol 0 fails (both single segment and all segments)
	vol	= &sys->mdb.vols[0];
	backup_segment_length = vol->segs[0].length;
	__unitest_volume_config_version_inc(&sys->mdb.vols[0], &sys->tcf.vols[0]);
	reset_cli_status_verification(client);
	set_cli_status_verification_expector(client, failed_attach_string(vol, CLI_UPDATE_FAILED), 0);
	vol->segs[0].length = 2 * backup_segment_length;
	send_command_to_vol(sys, -1, 0, volCmds_Update);

	unitest_trace_checkpoint(unitest_FS_InvalidCfg, modify_all_segments_length);
	if (force_reconf_reboot == false){
		//since we force previous topo destruction, we cannot discover illegal configuration
		//Update all segments to be twich as long and expect failure to update
		reset_cli_status_verification(client);
		set_cli_status_verification_sensetive_expector(client, failed_attach_string(vol, CLI_UPDATE_FAILED), 0);
		for (i = 0; i < vol->nSegments; i++) {
			vol->segs[i].length = 2 * backup_segment_length;
		}
		send_command_to_vol(sys, -1, 0, volCmds_Update);
	}
	for (i = 0; i < vol->nSegments; i++) {		// Reset the segment lengths
		vol->segs[i].length = backup_segment_length;
	}
	vol->nextCmd = volCmds_Illegal;
	__unitest_volume_config_version_dec(&sys->mdb.vols[0], &sys->tcf.vols[0]);

	unitest_trace_checkpoint(unitest_FS_InvalidCfg, modify_all_done);
	// ---------------- MCS testing:  Generate a message with a header version mismatch
	rv = generate_mcs_attach_message_for_volume(&sys->mgmt.mcs[client->inst_id], &client->vols[1], MCS_ATTACH_VOLUMES_MESSAGE_MSG, false, "", NVMEIB_MCS_MSG_WITH_BAD_HEADER); BUG_ON(rv >= 0);
	// Generate a message with a scheme version mismatch
	rv = generate_mcs_attach_message_for_volume(&sys->mgmt.mcs[client->inst_id], &client->vols[0], MCS_ATTACH_VOLUMES_MESSAGE_MSG, false, "", NVMEIB_MCS_MSG_WITH_BAD_SCHEME); BUG_ON(rv >= 0);
	// Attack mcs with buffer overflow
	rv = generate_mcs_attach_message_for_volume(&sys->mgmt.mcs[client->inst_id], &client->vols[0], MCS_ATTACH_VOLUMES_MESSAGE_MSG, false, "", NVMEIB_MCS_MSG_WITH_BUFFER_OVERFLOW); BUG_ON(rv >= 0);
	// Bad opcode, should end with error
	rv = generate_mcs_attach_message_for_volume(&sys->mgmt.mcs[client->inst_id], &client->vols[0], MCS_ATTACH_VOLUMES_MESSAGE_MSG, false, "", NVMEIB_MCS_MSG_WITH_BAD_OPCODE); BUG_ON(rv >= 0);

	// ---------------- MCS testing:  Generate a valid attach and update message for a volume but with an invalid token, expect volume to remain detached
	// Generate detach with token, should fail volume must remain attached
	sys->mdb.vols[0].nextCmd = volCmds_Detach;
	snprintf(&token[0], sizeof(token), "%015llu", (u64)1);
	snprintf(cli_command, sizeof(cli_command), "detachu %s %s", sys->mdb.vols[0].info.uuid, token);
	set_cli_error_expector(client, DETACH_INVALID_FORMAT);
	clientSimulator_send_to_cli_and_wait(client, cli_command);
	NVMeshSystem_serialize(sys);
	clientSimulator_wait_for_detach_drain(client);
	BUG_ON(client->devs[0] == NULL);

	send_command_to_vol(sys, -1, 0, volCmds_Detach); // Detach volume 0
	BUG_ON(client->devs[0] != NULL);

	if (1) {	// Drop a target from the configuration causing an attach failure
		reset_cli_status_verification(client);
		set_cli_status_verification_expector(client, failed_attach_string(vol, CLI_ATTACH_FAILED), 0);
		sys->mdb.vols[0].nextCmd = volCmds_New;
		rv = generate_mcs_attach_message_for_volume(&sys->mgmt.mcs[client->inst_id], &client->vols[0], MCS_ATTACH_VOLUMES_MESSAGE_MSG, false, MAGIC_CONFIG_FORCE__TOKEN, NVMEIB_MCS_MSG_DROP_TARGET);
		wait_for_cli_status_verification(client);
	}
	if (1) {
		// Create a configuration with out a NIC on server 0, attach will succeed but IO is disabled, the disk is paused beforehand
		// Cont after the correct configuration arrives should enable IO
		NVMeshSystem__invoke_pause_on_disk(sys, 0);
		reset_cli_status_verification(client);
		set_cli_status_verification_expector(client, attach_string_no_io(vol), 0);
		rv = generate_mcs_attach_message_for_volume(&sys->mgmt.mcs[client->inst_id], &client->vols[0], MCS_ATTACH_VOLUMES_MESSAGE_MSG, false, MAGIC_CONFIG_FORCE__TOKEN, NVMEIB_MCS_MSG_DROP_NIC);
		wait_for_cli_status_verification(client);
		NVMeshSystem_serialize(sys);								// Volume is attached with IO disabled

		__unitest_volume_config_version_inc(&sys->mdb.vols[0], &sys->tcf.vols[0]);
		reset_cli_status_verification(client);
		set_cli_status_verification_expector(client, attach_string(vol), 0);

		// Generate an update message with a correct configuration
		rv = generate_mcs_attach_message_for_volume(&sys->mgmt.mcs[client->inst_id], &client->vols[0], MCS_ATTACH_VOLUMES_MESSAGE_MSG, false, MAGIC_CONFIG_FORCE__TOKEN, NVMEIB_MCS_MSG_WITH_NO_ERROR);
		NVMeshSystem_serialize(sys);

		// Cont on the disk should enabled IO
		NVMeshSystem__invoke_cont_on_disk(sys, 0, false);
		wait_for_cli_status_verification(client); // Trigger the re-registartion here before waiting for IO enabled
		send_command_to_vol(sys, -1, 0, volCmds_Detach); // Detach the volume for the next scenario
	}
	if (1) {	// Drop a disk from the configuration causing an attach failure
		sys->mdb.vols[0].nextCmd = volCmds_New;
		reset_cli_status_verification(client);
		set_cli_status_verification_expector(client, failed_attach_string(vol, CLI_ATTACH_FAILED), 0);
		rv = generate_mcs_attach_message_for_volume(&sys->mgmt.mcs[client->inst_id], &client->vols[0], MCS_ATTACH_VOLUMES_MESSAGE_MSG, false, MAGIC_CONFIG_FORCE__TOKEN, NVMEIB_MCS_MSG_DROP_DISK);
		wait_for_cli_status_verification(client);
	}

	// Generate messages (attach/update) with a token that has already been used (000000000000001)
	// Expect attached status for volume 0, because we already removed token verification
	reset_cli_status_verification(client);
	set_cli_status_verification_expector(client, attach_string(&client->vols[0]), 0);
	rv = generate_mcs_attach_message_for_volume(&sys->mgmt.mcs[client->inst_id], &client->vols[0], MCS_ATTACH_VOLUMES_MESSAGE_MSG, false, token, NVMEIB_MCS_MSG_WITH_NO_ERROR);
	wait_for_cli_status_verification(client);
	NVMeshSystem_serialize(sys);
	BUG_ON(client->devs[0] == NULL);
	reset_cli_status_verification(client);
	set_cli_status_verification_expector(client, attach_string(&client->vols[0]), 0);
	rv = generate_mcs_attach_message_for_volume(&sys->mgmt.mcs[client->inst_id], &client->vols[0], MCS_ATTACH_VOLUMES_MESSAGE_MSG, false, token, NVMEIB_MCS_MSG_WITH_NO_ERROR);
	wait_for_cli_status_verification(client);
	NVMeshSystem_serialize(sys);
	BUG_ON(client->devs[0] == NULL);

	// Generate "bad" tokens", with invalid chars.
	// Expect attached status for volume 0, because we already removed token verification
	// Previously with token tree, this caused a full configuration triggering one detached message followed by 3 volume updates
	reset_cli_status_verification(client);
	set_cli_status_verification_expector(client,attach_string(&client->vols[0]), 0);
	rv = generate_mcs_attach_message_for_volume(&sys->mgmt.mcs[client->inst_id], &client->vols[0], MCS_ATTACH_VOLUMES_MESSAGE_MSG, false, "100A100A100A10A", NVMEIB_MCS_MSG_WITH_NO_ERROR);
	wait_for_cli_status_verification(client);
	NVMeshSystem_serialize(sys);
	BUG_ON(client->devs[0] == NULL);

	// Generate "bad" tokens", with lesser length
	reset_cli_status_verification(client);
	set_cli_status_verification_expector(client, attach_string(&client->vols[0]), 0);
	rv = generate_mcs_attach_message_for_volume(&sys->mgmt.mcs[client->inst_id], &client->vols[0], MCS_ATTACH_VOLUMES_MESSAGE_MSG, false, "100000", NVMEIB_MCS_MSG_WITH_NO_ERROR);
	wait_for_cli_status_verification(client);
	NVMeshSystem_serialize(sys);
	BUG_ON(client->devs[0] == NULL);

	// Test mcs cancel mechanizm
	mcs_message_queue_activate(&sys->mgmt.mcs[client->inst_id].mq, true);				// Tell MCS simulator to store message
	reset_cli_status_verification(client);
	set_cli_status_verification_expector(client, attach_string(&client->vols[0]), 0);
	// Create a valid token, and send attach, verify that cancel works
	snprintf(cli_command, sizeof(cli_command), "attachu %s %s --RW 0", sys->mdb.vols[0].info.uuid, token);
	clientSimulator_send_to_cli(client, cli_command);
	clientSimulator_send_to_cli(client, cli_command);			// Client will ignore double usage of token
	__cli_cmd_repeat_6_times_with_unique_token(client, cli_command, true);	// Insert the same attach request with many different tokens to make the tree a bit full
	// Send the cancel message before telling the MCS simulator to flush the message
	snprintf(cli_command, sizeof(cli_command), "cancel %s", token);
	clientSimulator_send_to_cli(client, cli_command);
	__cli_cmd_repeat_6_times_with_unique_token(client, cli_command, false);	// Cancel all the repeated attach requests
	// Release the MCS simulator
	clientSimulator_wait_for_mainwq(client);
	rv = mcs_message_queue_flush_n_stop(&sys->mgmt.mcs[client->inst_id].mq);
	BUG_ON(rv == -EIO || rv == -EINVAL); // EIO is failure to send mcs message, EINVAL is mcs queue not initialized
	wait_for_cli_status_verification(client);

	clientSimulator_wait_for_mainwq(client);
	BUG_ON(client->devs[0] == NULL);

	if (1) {	// Test incorrect cli attach commands
		const int len = sizeof(cli_command);
		cli_status_ver_reset(&client->cli_scripts); // Reset the CLI veification (We no longer return INVALID_CLI_FORMAT error for \n
		snprintf(cli_command, len, "\n \n\n \n\t\t\n\t\n  \4\3 \n");					clientSimulator_send_to_cli_and_wait(client, cli_command); // Invisible characters
		set_cli_error_expector(client, INVALID_CLI_COMMAND);
		snprintf(cli_command, len, " kt@ok@%%  \n\x5$#*(^\xf3)\xa1!\17j\xda\n");		clientSimulator_send_to_cli_and_wait(client, cli_command); // Wrong characters in strings. Note: \0xda = (int)'Z'+128
		set_cli_error_expector(client, INVALID_SEND_TOKEN);
		snprintf(cli_command, len, "attachu z1 123   --RW   0  ");						clientSimulator_send_to_cli_and_wait(client, cli_command); // Incorrect token with trailing spaces
		set_cli_error_expector(client, ATTACH_MISSING_TOKEN);
		snprintf(cli_command, len, "attachu z1"); 										clientSimulator_send_to_cli_and_wait(client, cli_command); // Missing token
		set_cli_error_expector(client, ATTACH_MISSING_TOKEN);
		snprintf(cli_command, len, "attachu  012345678901234");							clientSimulator_send_to_cli_and_wait(client, cli_command); // Missing Volume name, Token treated as volume name
		set_cli_error_expector(client, INVALID_CLI_FORMAT);
		snprintf(cli_command, len, "attachu ");											clientSimulator_send_to_cli_and_wait(client, cli_command); // Missing volume name and token
		set_cli_error_expector(client, MISSING_RESERVATION_MODE);
		snprintf(cli_command, len, "attachv z1 012345678901234");						clientSimulator_send_to_cli_and_wait(client, cli_command); // Missing reservation mode
		set_cli_error_expector(client, INVALID_RESERVATION_MODE);
		snprintf(cli_command, len, "attachv z1 012345678901234 --RD");					clientSimulator_send_to_cli_and_wait(client, cli_command); // Invalid reservation mode
		set_cli_error_expector(client, INVALID_RESERVATION_MODE);
		snprintf(cli_command, len, "attachv z1 012345678901234 --RD 0");				clientSimulator_send_to_cli_and_wait(client, cli_command); // Invalid reservation mode
		set_cli_error_expector(client, INVALID_RESERVATION_MODE);
		snprintf(cli_command, len, "attachv z1 012345678901234 --RWW 0");				clientSimulator_send_to_cli_and_wait(client, cli_command); // Invalid reservation mode
		set_cli_error_expector(client, INVALID_RESERVATION_VERSION);
		snprintf(cli_command, len, "attachv z1 012345678901234 --RW --preempt");		clientSimulator_send_to_cli_and_wait(client, cli_command); // INVALID reservation version
		set_cli_error_expector(client, MISSING_RESERVATION_VERSION);
		snprintf(cli_command, len, "attachv z1 012345678901234 --RW");					clientSimulator_send_to_cli_and_wait(client, cli_command); // Missing reservation version
		set_cli_error_expector(client, INVALID_OPTIONAL_FLAGS);
		snprintf(cli_command, len, "attachv z1 012345678901234 --RW 7 --preempttt");	clientSimulator_send_to_cli_and_wait(client, cli_command); // Invalid preempt flag
		set_cli_error_expector(client, INVALID_OPTIONAL_FLAGS);
		snprintf(cli_command, len, "attachv z1 012345678901234 --RW 90 --pretemp");		clientSimulator_send_to_cli_and_wait(client, cli_command); // Invalid preempt flag
		set_cli_error_expector(client, INVALID_OPTIONAL_FLAGS);
		snprintf(cli_command, len, "attachv z1 012345678901234 --RW 5 -5122");				clientSimulator_send_to_cli_and_wait(client, cli_command); // Invalid optional flags
		set_cli_error_expector(client, INVALID_OPTIONAL_FLAGS);
		snprintf(cli_command, len, "attachv z1 012345678901234 --RW 5 --12");			clientSimulator_send_to_cli_and_wait(client, cli_command); // Invalid optional flags
		set_cli_error_expector(client, INVALID_OPTIONAL_FLAGS);
		snprintf(cli_command, len, "attachv z1 012345678901234 --RW 5 --512 --reempt");	clientSimulator_send_to_cli_and_wait(client, cli_command); // Invalid optional flags
		set_cli_error_expector(client, INVALID_OPTIONAL_FLAGS);
		snprintf(cli_command, len, "attachv z1 012345678901234 --RW 5 --51 --preempt");		clientSimulator_send_to_cli_and_wait(client, cli_command); // Invalid optional flags
	}
	if (1) {	// Test attach by volume uuid which is longer than maximum allowed (should be truncated)
		char too_long_uuid[NVMEIBC_BD_UUID_LEN+10];
		const int uuid_len = sizeof(too_long_uuid);
		const int len      = sizeof(cli_command);
		memset(too_long_uuid, 'a', sizeof(too_long_uuid)-1);	too_long_uuid[uuid_len-1] = '\0';	// Generate uuid which is too long
		snprintf(cli_command, len, "attachu %s 012345678901234 --RW 0", too_long_uuid); clientSimulator_send_to_cli(client, cli_command);
	}
	// Re-attach the volume correctly
	NVMeshSystem_send_volumes_config_to_clients(sys, -1);

	// Test attach non-existant volume from CLI
	snprintf(cli_command, NVMEIBC_BD_UUID_LEN + 8 + 17, "attachu no_volume %s --RW 0", token);
	reset_cli_status_verification(client);
	set_cli_status_verification_expector(client, cli_unknown_string("no_volume", true), 0);
	clientSimulator_send_to_cli_and_wait(client, cli_command);

	NVMeshSystem_serialize(sys);
	BUG_ON(!client->devs[0]);

	for (volInd=0; volInd<sys->clients[0].nBdevs; volInd++)				// Verify IO on all drives
		BUG_ON(!nvmeibc_topo_is_io_ok(&client->devs[volInd]->topologies));

	// TODO: Test cancel with invalid values

	// Generate full configuration message
	// Simulate that we expect all 4 volumes to have a new status
	reset_cli_status_verification(client);
	for (volInd = 0; volInd < client->nBdevs; volInd++) {
		set_cli_status_verification_expector(client, update_string(&client->vols[volInd]), volInd);
	}

	// Cause request for full config (can be done in various ways)
	clientSimulator_send_to_cli_and_wait(client, "%get_full_conf");							// Increase number of ioctls + 1
	expected_num_of_executed_ioctls++;

	for (volInd=0; volInd<sys->clients[0].nBdevs; volInd++)				// Verify IO on all drives
		BUG_ON(!nvmeibc_topo_is_io_ok(&client->devs[volInd]->topologies));

	if (1) { // Disk swaping and reappear tests
		// Disk configuration tests, swap disks between servers 1 and 0
		// Simulate that the transport layer re-registered on all toma connections
		// Pause -> swap connections -> Cont -> IO
		NVMeshSystem_DiskReappearEvent(sys, 0, 1, false);
		for (volInd=0; volInd<sys->clients[0].nBdevs; volInd++)				// Verify IO on all drives
			BUG_ON(!nvmeibc_topo_is_io_ok(&client->devs[volInd]->topologies));
		send_command_to_all(sys, -1, volCmds_Detach);	 // Disk configuration tests, verify detach works after swap
		volInd = 0;

		// Just bad path swap disks - IO cannot be enabled
		NVMeshSystem_swap_disk_between_servers(sys, 0, 1);
		reset_cli_status_verification(client);
		set_cli_status_verification_expector(client, attach_string_no_io(&client->vols[volInd]), volInd);
		send_command_to_vol(sys, -1, volInd, volCmds_New);
		send_command_to_vol(sys, -1, volInd, volCmds_Detach); // Detach the volume again to allow swapping back the drives

		// Attach all volumes with swapped drives
		NVMeshSystem_swap_disk_between_servers(sys, 0, 1);
		send_command_to_all(sys, -1, volCmds_New);

		// Swap drives back to original locations and verify with IO
		for (volInd=0;volInd<client->nBdevs;volInd++)
			sys->mdb.vols[volInd].nextCmd = volCmds_Update;
		NVMeshSystem_DiskReappearEvent(sys, 0, 1, true);
		for (volInd=0; volInd<sys->clients[0].nBdevs; volInd++)				// Verify IO on all drives
			BUG_ON(!nvmeibc_topo_is_io_ok(&client->devs[volInd]->topologies));
		send_command_to_all(sys, -1, volCmds_Detach);		// Detach all volumes and reattach them again
		send_command_to_all(sys, -1, volCmds_New);
		for (volInd=0;volInd<client->nBdevs;volInd++) {
			sys->mdb.vols[volInd].nextCmd = volCmds_Illegal;
		}
	}
	BUG_ON(ioctl_num + expected_num_of_executed_ioctls != clientSimulator_get_num_executed_ioctls(client));
	return 0;
}

TEST_FUNC int unitest_recovery_hidden_volume(struct NVMeshSystem *sys)
{
	int v;
	struct clientSimulator *client = &sys->clients[0];	//the single client in the system
	const unsigned orig_self_recovery_detach_time_sec = self_recovery_detach_initial_time_sec;
	self_recovery_detach_initial_time_sec = (1 << 20); // Make auto-detach not happen
	send_command_to_all(sys, -1, volCmds_RecoveryDetach);	for (v=0; v<client->nBdevs; v++) {BUG_ON(client->devs[v] == NULL);}								// Hidden detach on visible volume does not trigger detach
	send_command_to_all(sys, -1, volCmds_Detach);			for (v=0; v<client->nBdevs; v++) {BUG_ON(client->devs[v] != NULL);           }
	send_command_to_all(sys, -1, volCmds_HiddenAttach);		for (v=0; v<client->nBdevs; v++) {BUG_ON(!clientSimulator_is_vol_hidden_attached(client, v));}	// Hidden not recovery
	send_command_to_all(sys, -1, volCmds_HiddenDetach);		for (v=0; v<client->nBdevs; v++) {BUG_ON(client->devs[v] != NULL);} 							// Hidden only detach
	send_command_to_all(sys, -1, volCmds_HiddenAttach);		for (v=0; v<client->nBdevs; v++) {BUG_ON(!clientSimulator_is_vol_hidden_attached(client, v));}	// Hidden not recovery
	send_command_to_all(sys, -1, volCmds_RecoveryAttach);	for (v=0; v<client->nBdevs; v++) {BUG_ON(!clientSimulator_is_vol_hidden_attached(client, v));}	// Hidden->recovery
	send_command_to_all(sys, -1, volCmds_HiddenDetach);		for (v=0; v<client->nBdevs; v++) {BUG_ON(client->devs[v] == NULL);BUG_ON(!clientSimulator_is_vol_hidden_attached(client, v));}// Hidden only detach - will fail
	send_command_to_all(sys, -1, volCmds_RecoveryDetach); 	for (v=0; v<client->nBdevs; v++) {BUG_ON(client->devs[v] != NULL);}
	send_command_to_all(sys, -1, volCmds_ShadowAttach);		for (v=0; v<client->nBdevs; v++) {BUG_ON( clientSimulator_is_vol_hidden_attached(client, v));}	// Hidden->Visible
	send_command_to_all(sys, -1, volCmds_Detach);			for (v=0; v<client->nBdevs; v++) {BUG_ON(client->devs[v] != NULL);           }
	send_command_to_all(sys, -1, volCmds_New);				for (v=0; v<client->nBdevs; v++) {BUG_ON( clientSimulator_is_vol_hidden_attached(client, v));}	// Hidden->Visible
	if (true) { // Test: NVMESH-276 bug due to race condition
		const bool prev = cli_attach_check_if_already_attached; cli_attach_check_if_already_attached = false;
		reset_cli_status_verification(client); set_cli_status_verification_expector(client, failed_attach_string(&client->vols[0], CLI_UPDATE_FAILED), 0);
		send_command_to_vol(sys, -1, 0, volCmds_HiddenAttach);	for (v=0; v<client->nBdevs; v++) {BUG_ON(clientSimulator_is_vol_hidden_attached(client, v));}	// Hidden attach request while volume is attached as visible, remains visible
		cli_attach_check_if_already_attached = prev;
	}
	send_command_to_all(sys, -1, volCmds_Detach);			for (v=0; v<client->nBdevs; v++) {BUG_ON(client->devs[v] != NULL);           }

	if (1) { // Test recovery volume update
		send_command_to_all(sys, -1, volCmds_RecoveryAttach);	for (v=0; v<client->nBdevs; v++) {BUG_ON(!clientSimulator_is_vol_hidden_attached(client, v));}
		__unitest_volume_config_version_inc(&sys->mdb.vols[0], &sys->tcf.vols[0]);		// Increase the version of configuration for vol 0
		send_command_to_all(sys, -1, volCmds_RecoveryUpdate);	for (v=0; v<client->nBdevs; v++) {BUG_ON(!clientSimulator_is_vol_hidden_attached(client, v));}
		send_command_to_all(sys, -1, volCmds_RecoveryUpdate);	for (v=0; v<client->nBdevs; v++) {BUG_ON(!clientSimulator_is_vol_hidden_attached(client, v));}
	}
	if (1) {							// Verify sub volumes is not created on hidden volume
		const int expected_num_of_executed_ioctls = clientSimulator_get_num_executed_ioctls(client)+1;		// One succeed
		char cmd[256];
		sprintf(cmd, "#%s|sub_vol_add name=P_01 start=0 len=256", sys->mdb.vols[0].info.devname);
		reset_cli_status_verification(client);
		set_cli_status_verification_expector(client, cli_generic_string(&sys->mdb.vols[0], CLI_ALIAS_CREATE_FAIL, true), 0);
		clientSimulator_send_to_cli_and_wait(client, cmd);
		BUG_ON(expected_num_of_executed_ioctls != clientSimulator_get_num_executed_ioctls(client));
	}

	reset_cli_status_verification(client);
	for (v = 0; v < sys->mdb.nVols; ++v ) {
		__unitest_volume_config_version_inc(&sys->mdb.vols[v], &sys->tcf.vols[v]);/* Auto increase the version of configuration */
		set_cli_status_verification_expector(client, cli_generic_string(&sys->mdb.vols[v], CLI_ATTACHED, true), v);
	}

	clientSimulator_send_to_cli_and_wait(client, "%get_full_conf");
	for (v=0; v<client->nBdevs; v++) {BUG_ON( !clientSimulator_is_vol_hidden_attached(client, v));}
	//send_command_to_all(sys, -1, volCmds_Update);
	send_command_to_all(sys, -1, volCmds_New); 			for (v=0; v<client->nBdevs; v++) {BUG_ON(clientSimulator_is_vol_hidden_attached(client, v));}
	send_command_to_all(sys, -1, volCmds_Detach);
	send_command_to_all(sys, -1, volCmds_New);
	self_recovery_detach_initial_time_sec = orig_self_recovery_detach_time_sec;
	return 0;
}

/* Disconnect from volume and reconnect to it */
TEST_FUNC int unitest_upgrade_nvmeibc_with_volumes(struct NVMeshSystem *sys){
	int v, rv, nVols = sys->mdb.nVols, io_vol_ind = 0;
	struct clientSimulator *client = &sys->clients[0];	//the single client in the system
	struct nvmeiba_atom_os_api *atoms[4];
	struct osSimulator *OS = &client->OS;
	char cmd[256];
	DECLARE_COMPLETION_ONSTACK(comp0);					// Daniel: Todo, design multicompletion for bio
	DECLARE_COMPLETION_ONSTACK(comp1);
	DECLARE_COMPLETION_ONSTACK(comp2);
	send_command_to_all(sys, -1, volCmds_Detach);
	for (v=0; v<nVols; v++) {
		sys->mdb.vols[v].nextCmd = (v%2) ? volCmds_RecoveryAttach : volCmds_New;				// Some volumes are hidden attach, some regular attach
	}
	send_command_predefined_to_all(sys, -1);
	for (v=0; v<nVols; v++) {
		BUG_ON(clientSimulator_is_vol_hidden_attached(client, v) != (v%2));
		BUG_ON(client->devs[v] == NULL);
		atoms[v] = &client->devs[v]->os->atom;
	}

	if (1) {											// Push IO to resubmition of volume to test that it is properly returned back to ATOM
		struct nvmeibc_topology *t;
		v = io_vol_ind;
		t = nvmeibc_topology_get(&client->devs[v]->topologies);
		t->io_perm = NVMEIB_IO_TYPE_PERMIT_NO_IO;
		nvmeibc_topology_put(t);
		osSimulator_writeArr_async_wait(OS, v, 0, 1, page_address(ZERO_PAGE(0)), &comp0);
		BUG_ON(atoms[v]->pender.n_bios != 0);			// IO is still not in waiting upgrade list
	}

	if (1) {	// ----------------------- Create sub volume to verify upgrade with sub volume
		int expected_num_of_executed_ioctls = clientSimulator_get_num_executed_ioctls(client);
		sprintf(cmd, "#%s|sub_vol_add name=P_04 start=4 len=32", atoms[io_vol_ind]->dev_name);
		reset_cli_status_verification(client);
		set_cli_status_verification_expector(client, cli_generic_string(&client->vols[io_vol_ind], CLI_ALIAS_CREATED, false), io_vol_ind);
		clientSimulator_send_to_cli_and_wait(client, cmd); expected_num_of_executed_ioctls++;
		BUG_ON(expected_num_of_executed_ioctls != clientSimulator_get_num_executed_ioctls(client));
	}

	// Detach all volumes for upgrade
	sprintf(cmd, "#|atom_read_part=0");	clientSimulator_send_to_cli(client, cmd);
	send_command_to_all(sys, -1, volCmds_DetachUpgrade);
	for (v=0; v<nVols; v++) {
		BUG_ON(client->devs[v] != NULL);
		if (v%2) { atoms[v] = NULL; }					// Those atoms were freed
	}

	// 2 Volumes were detached (Vol1, Vol3), 2 were abandoned (Vol0, Vol2) + 1 sub vol abandoned (v0_p04)
	BUG_ON(nvmeiba_os_apis_get_num('A') != 3);
	BUG_ON(nvmeiba_os_apis_get_num('O') != 3);
	BUG_ON(nvmeiba_os_apis_get_num('S') != 1);

	if (1) {	// ----------------------- Test open()/close()/IO on orphan atom
		struct block_device	*bdev;
		v = io_vol_ind;
		BUG_ON(atoms[v]->pender.n_bios != 1);			// IO that existed before upgrade
		rv = osSimulator_diskOpenIdx( OS, v, false, FMODE_READ|FMODE_EXCL, &bdev); BUG_ON(rv);
		osSimulator_writeArr_async_wait(OS, v, 3, 1, page_address(ZERO_PAGE(0)), &comp1);
		BUG_ON(bio_list_size(&atoms[v]->pender.bio_list) != 2);
		BUG_ON(atoms[v]->pender.n_bios != 2);
		rv = osSimulator_diskCloseIdx(OS, v); BUG_ON(rv);
	}
	if (1) {	// ----------------------- Test IO on orphan sub atom
		osSimulator_writeArr_async_wait(OS, 4, 1, 1, page_address(ZERO_PAGE(0)), &comp2);
	}
	if (1) {	// ----------------------- Test mount()/umount() on orphan atom
		v = 2;
		rv = osSimulator_mount(   OS, v, FMODE_WRITE); BUG_ON(rv);
		BUG_ON(atomic_read(&atoms[v]->users.n_opens) != 1);
		rv = osSimulator_unmount( OS, v); BUG_ON(rv);
		BUG_ON(atomic_read(&atoms[v]->users.n_opens) != 0);
	}
	if (1) {	// ----------------------- Test intentional autofail IO during hot upgrade
		int expected_num_of_executed_ioctls = clientSimulator_get_num_executed_ioctls(client) + 1;
		v = io_vol_ind;
		sprintf(cmd, "#|atom_upg_fail_io=1, ptr=0x%llx", (u64)(atoms[v]));		// Autofail 1 (first) bio from the 2 existing
		clientSimulator_send_to_cli(client, cmd);
		BUG_ON(atoms[v]->pender.n_bios != 1);
		BUG_ON(expected_num_of_executed_ioctls != clientSimulator_get_num_executed_ioctls(client));
		wait_for_completion(&comp0);										// IO was autofailed. Verify it completed here
		reup(&comp0);														// Reinit completion for the correct place where IO would finish if we autofailing test is disabled
	}

	if (1) { // --------------------- Simulate hidden attach attempt on volume V0 - should fail
		reset_cli_status_verification(client);
		set_cli_status_verification_expector(client, fail_hidattch_string(&client->vols[io_vol_ind]), io_vol_ind);
		send_command_to_vol(sys, -1, io_vol_ind, volCmds_RecoveryAttach);
		BUG_ON(atoms[v]->pender.n_bios != 1);		// pending IOs remained
		BUG_ON(nvmeiba_os_apis_get_num('O') != 3);	// DEspite failed attach, orphan remained
	}

	// --------------------- Attach first 3 volumes, varify orphans are adopted and all IO completes
	send_command_to_vols(sys, -1, volCmds_New, nVols-1, &sys->mdb.vols[0]);
	BUG_ON(client->devs[nVols-1] != NULL);
	for (v=0; v<nVols-1; v++) {
		if (atoms[v] == NULL) {
			atoms[v] = &client->devs[v]->os->atom;
		} else {
			BUG_ON(atoms[v] != &client->devs[v]->os->atom);				// Attach adopted existing orphan
			BUG_ON(atoms[v]->pender.n_bios != 0);						// All IO's adopted and resubmitted
		}
	}
	BUG_ON(nvmeiba_os_apis_get_num('A') != 4);	// Vol0..3, sub0_p4
	BUG_ON(nvmeiba_os_apis_get_num('O') != 0);
	BUG_ON(nvmeiba_os_apis_get_num('S') != 1);

	if (1) {	// ----------------------- Delete Subvol
		int expected_num_of_executed_ioctls = clientSimulator_get_num_executed_ioctls(client);
		wait_for_completion(&comp2);
		sprintf(cmd, "#%s|sub_vol_del name=P_04 start=4 len=32", atoms[io_vol_ind]->dev_name);
		reset_cli_status_verification(client);
		set_cli_status_verification_expector(client, cli_generic_string(&client->vols[io_vol_ind], CLI_ALIAS_DELETED, false), io_vol_ind);
		clientSimulator_send_to_cli_and_wait(client, cmd); expected_num_of_executed_ioctls++;
		BUG_ON(expected_num_of_executed_ioctls != clientSimulator_get_num_executed_ioctls(client));
		BUG_ON(nvmeiba_os_apis_get_num('S') != 0);
	}

	wait_for_completion(&comp0);
	wait_for_completion(&comp1);
	BUG_ON(osSimulator_allert_pending_ios(OS, 0));

	// --------------------- Test: detach all for upgrade
	prepare_cli_status_verification_for_shutdown(client, true);

	clientSimulator_send_to_cli_and_wait(client, "upgrade-shutdown");
	BUG_ON(nvmeiba_os_apis_get_num('A') != 3);				// 3 Volumes were abandoned (Vol0..Vol2)
	for (v=0; v<nVols-1; v++) {
		osSimulator_mount(OS, v, FMODE_WRITE);			// Mount 3 detached volumes
		BUG_ON(atoms[v]->disk->fops->ioctl != NULL);		// fops were overrwrited by abandoned nvmeiba
	}

	clientSimulator_rmmod(client);
	BUG_ON(!(!client->is_nvmeibc_ko_up && client->is_nvmeiba_ko_up));
	NVMeshSystem_service_nvmeshclient_start(sys, 0);

	sprintf(cmd, "#|atom_read_part=1");	clientSimulator_send_to_cli(client, cmd);
	send_command_to_all(sys, -1, volCmds_New);
	for (v=0; v<nVols-1; v++) { // Sometimes this is a race where we get all statuses (send_command_to_all completes) and yet atom n_opens is still not one (it usually is 2 since we are running re-read paritions
		const int opens = atomic_read(&atoms[v]->users.n_opens);
		BUG_ON(opens != 1 && opens != 2);
		BUG_ON(atoms[v]->disk->fops->ioctl == NULL);		// fops were overrwrited by inherited nvmeibc
		osSimulator_unmount(OS, v);
	}
	atoms[v] = &client->devs[v]->os->atom;
	return 0;
//	__reset_predefined_command(sys);
}

static void __fill_atoms_of_carrier_and_sub(struct clientSimulator *clnt, int vol_ind, struct nvmeiba_atom_os_api **car_atom, struct nvmeiba_atom_os_api **sub_atom) {
	struct nvmeibc_os_api *car_api = clnt->devs[vol_ind]->os;
	struct nvmeiba_part *part;
	(*car_atom) = &car_api->atom;
	part = list_first_entry(&(*car_atom)->sub.part_list, struct nvmeiba_part, part_list);
	(*sub_atom) = container_of(part, struct nvmeiba_atom_os_api, sub);
}

static void __test_ioctls_to_sub_vol(struct NVMeshSystem *sys, int carrier_ind) {
	struct clientSimulator *clnt = &sys->clients[0];
	struct nvmeiba_atom_os_api *car_atom, *sub_atom;
	__fill_atoms_of_carrier_and_sub(clnt, carrier_ind, &car_atom, &sub_atom);
	__unitest_issue_ioctls_todisk(sub_atom->disk, (void*)sub_atom);
}

TEST_FUNC int unitest_sub_vols(struct NVMeshSystem *sys) {
	struct clientSimulator *clnt = &sys->clients[0];
	int vol_ind = 0, sub_ind = 4, rv = 0;
	int sub_ofst = LOCKSET_SLICES-1, sub_len = 3;			// Force sub volume to span accros 2 blocksets even though it is small
	struct nvmeibc_block_device *carrier = clnt->devs[vol_ind];
	char carrier_name[32];
	struct osSimulator *OS = &clnt->OS;
	char cmd[256];
	strlcpy(carrier_name, carrier->os->atom.dev_name, sizeof(carrier_name));

	if (1) {	// Verify sub volumes with very long names that get cut off
		const char *ioctl_add_fmt_long_alias = "#%s|sub_vol_%s name=P_0%d_a123456789012345678901234567890123456789abcdef start=0 len=%d";
		int expected_num_of_executed_ioctls = clientSimulator_get_num_executed_ioctls(clnt);        // None succeed
		int i;
		for (i = 0; i < 2; i++) {
			reset_cli_status_verification(clnt);
			set_cli_status_verification_expector(clnt, cli_generic_string(&clnt->vols[vol_ind], CLI_ALIAS_CREATED, false), vol_ind);
			sprintf(cmd, ioctl_add_fmt_long_alias, carrier_name, "add", sub_ind, i);
			clientSimulator_send_to_cli_and_wait(clnt, cmd);	expected_num_of_executed_ioctls++;
			BUG_ON(nvmeiba_os_apis_get_num('A') != 5);
			sprintf(cmd, ioctl_add_fmt_long_alias, carrier_name, "del", sub_ind, i);
			reset_cli_status_verification(clnt);
			set_cli_status_verification_expector(clnt, cli_generic_string(&clnt->vols[vol_ind], CLI_ALIAS_DELETED, false), vol_ind);
			clientSimulator_send_to_cli_and_wait(clnt, cmd);	expected_num_of_executed_ioctls++;
			BUG_ON(nvmeiba_os_apis_get_num('A') != 4);
			BUG_ON(expected_num_of_executed_ioctls != clientSimulator_get_num_executed_ioctls(clnt));
		}
	}
	if (1) {												// Verify sub volumes are not created in wrong conditions
		const char *ioctl_add_fmt = "#%s|sub_vol_add name=P_0%d start=%d len=%d";
		int expected_num_of_executed_ioctls = clientSimulator_get_num_executed_ioctls(clnt);        // None succeed
		sprintf(cmd, "#%s|sub_vol_del name=P_0%d", carrier_name, sub_ind);
		reset_cli_status_verification(clnt);
		set_cli_status_verification_expector(clnt, cli_generic_string(&clnt->vols[vol_ind], CLI_ALIAS_DELETE_FAIL, false), vol_ind);
		clientSimulator_send_to_cli_and_wait(clnt, cmd);							// Deleting non existing sub volme is a silent error
		sprintf(cmd, ioctl_add_fmt, carrier_name, sub_ind, sub_ofst, sub_len);
		reset_cli_status_verification(clnt);
		set_cli_status_verification_expector(clnt, cli_generic_string(&clnt->vols[vol_ind], CLI_ALIAS_CREATED, false), vol_ind);
		clientSimulator_send_to_cli_and_wait(clnt, cmd);				expected_num_of_executed_ioctls++;
		BUG_ON(nvmeiba_os_apis_get_num('A') != 5);
		__test_ioctls_to_sub_vol(sys, vol_ind);							expected_num_of_executed_ioctls++;
		reset_cli_status_verification(clnt);
		set_cli_status_verification_expector(clnt, cli_generic_string(&clnt->vols[vol_ind], CLI_ALIAS_CREATE_FAIL, false), vol_ind);
		clientSimulator_send_to_cli_and_wait(clnt, cmd);							// Fail to create sub volume with identical name once again
		sprintf(cmd, ioctl_add_fmt, "zz1zz3", sub_ind, sub_ofst, sub_len); expected_num_of_executed_ioctls++;
		reset_cli_status_verification(clnt);
		set_cli_status_verification_expector(clnt, cli_unknown_string("zz1zz3", false), vol_ind);
		clientSimulator_send_to_cli_and_wait(clnt, cmd);							// Fail to create sub vol of non existing carrier volume
		sprintf(cmd, ioctl_add_fmt, carrier_name, sub_ind, sub_ofst, 100040); expected_num_of_executed_ioctls++;
		reset_cli_status_verification(clnt);
		set_cli_status_verification_expector(clnt, cli_generic_string(&clnt->vols[vol_ind], CLI_ALIAS_CREATE_FAIL, false), vol_ind);
		clientSimulator_send_to_cli_and_wait(clnt, cmd);							// Fail to create sub vol outside of carriers lba
		sprintf(cmd, ioctl_add_fmt, "*", sub_ind, 0, 32);				expected_num_of_executed_ioctls++;
		reset_cli_status_verification(clnt);
		set_cli_error_expector(clnt, INVALID_IOCTL_COMMAND);
		clientSimulator_send_to_cli_and_wait(clnt, cmd);							// Fail to create sub vol of multiple carrier volumes
		BUG_ON(expected_num_of_executed_ioctls != clientSimulator_get_num_executed_ioctls(clnt));
		BUG_ON(nvmeiba_os_apis_get_num('A') != 5);
	}
	// Test non forced detach on carrier volume, we should get busy status to cli And we need to see that the volume didn't start to detach
	reset_cli_status_verification(clnt);
	set_cli_status_verification_expector(clnt, cli_generic_string(&clnt->vols[vol_ind], CLI_BUSY, false), vol_ind);
	send_command_to_vol_safe_detach_no_wait(sys, -1, vol_ind);
	BUG_ON(!nvmeibc_topo_is_io_ok(&carrier->topologies));
	BUG_ON(block_api_os_is_mounted(carrier->os) != 1);		// sub volume took 1 ref on carrier

	if (1) { // -------------------------- Verify IO goes to correct offset (Write through sub volume, read through carrier
		u64       magic_pattern;    								// unique 64b signaturre filling the array
		const int memSize = 4*NVMEIBC_SECTOR_SIZE;					// Total array in bytes
		u8        *mem = sim_kmalloc(memSize, GFP_KERNEL);				// Array to read/write to disk
		int startBlock = 1, lenBlocks = 2;
		magic_pattern = __unitest_fill_blocks_unique_pattern(mem, lenBlocks);	// Set a pattern.
		osSimulator_writeArrWait(OS, sub_ind, startBlock,          lenBlocks, mem);
		memset(mem, 0, memSize);								// Clear the array
		osSimulator_readArrWait( OS, vol_ind, startBlock+sub_ofst, lenBlocks, mem);
		__unitest_verify_blocks_pattern(mem, lenBlocks, magic_pattern, false);			// Verify that read and write matched.

		// -------------------------- Verify illegal IO range for sub volume is rejected (though it is legal for carrier
		dp_io_stats_clear_counter(&carrier->dp.io_stats, DP_IO_STATS_CRITICAL_FAIL);
		BUG_ON(osSimulator_writeArr(OS, sub_ind, -1, 100, mem));
		BUG_ON(osSimulator_writeArr(OS, sub_ind, sub_len, 1, mem)); //
		BUG_ON(osSimulator_writeArr(OS, sub_ind, 1, sub_len, mem)); //
		BUG_ON(dp_io_stats_get_counter(&carrier->dp.io_stats, DP_IO_STATS_CRITICAL_FAIL) != 3); // all utests failed as expected
		dp_io_stats_clear_counter(&carrier->dp.io_stats, DP_IO_STATS_CRITICAL_FAIL);
		sim_kfree(mem);
	}
	if (1) {// -------------------------- Verify sub vol is mountable
		struct nvmeibc_os_api *sub = (void*)OS->disks[sub_ind]->private_data;
		BUG_ON(osSimulator_mount(OS, sub_ind, FMODE_READ|FMODE_EXCL));
		BUG_ON(block_api_os_is_mounted(sub) != 1);		// sub volume took 1 ref on carrier
		BUG_ON(sub->atom.sub.parent != &carrier->os->atom);
		osSimulator_unmount(OS, sub_ind);
	}

	if (1) {// -------------------------- Test Force detach on carrier with sub volume
		struct gendisk **disk = &OS->disks[vol_ind];
		struct nvmeiba_atom_os_api *car_atom, *sub_atom;
		int do_mount_car, do_mount_sub;
		__fill_atoms_of_carrier_and_sub(clnt, vol_ind, &car_atom, &sub_atom);
		for (do_mount_car = false; do_mount_car <= 1; do_mount_car++) {					// 2 options, with or without mounting of carrier
		for (do_mount_sub = false; do_mount_sub <= 1; do_mount_sub++) {					// 2 options, with or without mounting of sub vol
			if (do_mount_car) BUG_ON(osSimulator_mount(OS, vol_ind, FMODE_WRITE));
			if (do_mount_sub) BUG_ON(osSimulator_mount(OS, sub_ind, FMODE_WRITE));
			BUG_ON(atomic_read(&car_atom->users.n_opens) != (1+do_mount_car));			// 1 sub volume + optional mounting
			BUG_ON(atomic_read(&sub_atom->users.n_opens) != (  do_mount_sub));
			send_command_to_vol(sys, -1, vol_ind, volCmds_ForceDetach);
			if (do_mount_car) osSimulator_unmount(OS, vol_ind);
			if (do_mount_sub) osSimulator_unmount(OS, sub_ind);
			clientSimulator_wait_for_detach_drain(clnt);
			BUG_ON((disk[vol_ind] != NULL)||(disk[sub_ind] != NULL));									// Here both the carrier and sub volume were detached and cleared
			BUG_ON(nvmeiba_os_apis_get_num('A') != 3);
			BUG_ON(nvmeiba_os_apis_get_num('S') != 0);

			// Reattach back for next iteration
			send_command_to_vol(sys, -1, vol_ind, volCmds_New);
			sprintf(cmd, "#%s|sub_vol_add name=P_0%d start=%d len=%d", carrier_name, sub_ind, sub_ofst, sub_len);
			reset_cli_status_verification(clnt);
			set_cli_status_verification_expector(clnt, cli_generic_string(&clnt->vols[vol_ind], CLI_ALIAS_CREATED, false), vol_ind);
			clientSimulator_send_to_cli_and_wait(clnt, cmd);							//expected_num_of_executed_ioctls++;

			__fill_atoms_of_carrier_and_sub(clnt, vol_ind, &car_atom, &sub_atom);
			BUG_ON((*disk == NULL) || (atomic_read(&car_atom->users.n_opens) != 1));
		}}
		carrier = clnt->devs[vol_ind];
	}

	//unitest_IO(clnt, sub_ind, 1, 2);					// Todo: Insert also tests for TRIM
	sprintf(cmd, "#%s|sub_vol_del name=P_0%d", carrier_name, sub_ind);
	reset_cli_status_verification(clnt);
	set_cli_status_verification_expector(clnt, cli_generic_string(&clnt->vols[vol_ind], CLI_ALIAS_DELETED, false), vol_ind);
	clientSimulator_send_to_cli(clnt, cmd);				// Daniel: no need to wait, mainwq is drained to wait for cli submit and processing is syncronous
	BUG_ON(block_api_os_is_mounted(carrier->os) != 0);	// sub volume took 1 ref on carrier
	return rv;
}

TEST_FUNC int unitest_multi_clnt_instances(struct NVMeshSystem *sys){
	//char cmd[256];
	struct clientSimulator *clnt = &sys->clients[0];			// Test via the first client
	int expected_num_of_executed_ioctls = clientSimulator_get_num_executed_ioctls(clnt);        // None succeed

	if (1) {  // Verify parsing of incorrect args does not fail
		clientSimulator_send_to_cli(clnt, "%clnt++");
		clientSimulator_send_to_cli(clnt, "%clnt++{,xd}");
		clientSimulator_send_to_cli(clnt, "%clnt++{ac,,");
	}
	clientSimulator_send_to_cli(clnt, "%clnt--{mc0001,}"); // Cannot remove non existing instance
	clientSimulator_send_to_cli(clnt, "%clnt--{nvmeibc,nvmesh}"); // Cannot remove first instance
	clientSimulator_send_to_cli(clnt, "%clnt++{mc0001,mc0001}"); expected_num_of_executed_ioctls++;
	clientSimulator_send_to_cli(clnt, "%clnt++{mc0001,mc0001}"); // Cannot add same instance more than once
//	clientSimulator_print_proc_dir(clnt, 1);				// Can see that /proc filess were added under root dir
	clientSimulator_send_to_cli(clnt, "%paramc:ports=mlx4_0:1,mlx_4:2,mlx4_1:1"); expected_num_of_executed_ioctls++;	// Test Changing params of instance
	clientSimulator_send_to_cli(clnt, "%paramm:cluster=n192:4001,n193:4002,n194:4999"); expected_num_of_executed_ioctls++;	// Test Changing params of instance
	clientSimulator_send_to_cli(clnt, "%paramm:protocol=https"); expected_num_of_executed_ioctls++;	// Test Changing params of instance
	clientSimulator_send_to_cli(clnt, "%paramb:unused=unused");	 // Block layer does not support yet params per instance
	clientSimulator_send_to_cli(clnt, "%paramu:unused=unused");	 // Unknown params should just be skipped
	clientSimulator_send_to_cli(clnt, "%paramm:unused=unused");	 // Main layer ignores unsupported params
	BUG_ON(expected_num_of_executed_ioctls != clientSimulator_get_num_executed_ioctls(clnt));

	clientSimulator_send_to_cli(clnt, "%clnt--{mc0001,}"); expected_num_of_executed_ioctls++;
	clientSimulator_send_to_cli(clnt, "%clnt++{mc0002,mc0002}paramc:ports=mlx4_0:1,mlx_4:2,mlx4_1:1|paramc:ports=mlx4_0:1,mlx_4:2,mlx4_1:8|paramb:jstride=5"); expected_num_of_executed_ioctls++;
	clientSimulator_send_to_cli(clnt, "%clnt--{mc0002,}"); expected_num_of_executed_ioctls++;
	clientSimulator_send_to_cli(clnt, "%clnt++{mc0002,mc0002}paramm:cluster=n192:4001,n193:4002,n194:4999|paramm:protocol=http|paramm:auto_gen=1|paramm:db_uuid=kill_me_please5"); expected_num_of_executed_ioctls++;
	clientSimulator_send_to_cli(clnt, "%clnt--{mc0002,}"); expected_num_of_executed_ioctls++;
	clientSimulator_send_to_cli(clnt, "%clnt++{mc0002,mc0002}paramu:invalid=inval");
	BUG_ON(expected_num_of_executed_ioctls != clientSimulator_get_num_executed_ioctls(clnt));
	return 0;
}

TEST_FUNC int unitest_cpu_masks(struct NVMeshSystem *sys)
{
	struct clientSimulator *clnt = &sys->clients[0];
	int vol_ind = 0, vol2_ind = 1;
	const char *mask, *mask2;

	#define MASK_OP(_vol_ind, _op, _mask, _expect_success) do { \
		struct proc_dir_entry *proc = clientSimulator_find_vol_proc_file_by_path(clnt, (_vol_ind), "cpu_masks/cpu_mask_" _op); \
		loff_t pos = 0; \
		int write_rv = proc->fops->write(proc->data, _mask, strlen(_mask), &pos); \
		BUG_ON((_expect_success) != (write_rv >= 0)); \
	} while(0)

	// Single volume, single mask

	mask = "00000000,00000000,00000000,0000000f";
	MASK_OP(vol_ind, "add", mask, true);
	MASK_OP(vol_ind, "del", mask, true);

	// 2 volumes, same mask

	mask = "00000000,00000000,00000000,000000fc";
	MASK_OP(vol_ind, "add", mask, true);
	MASK_OP(vol2_ind, "add", mask, true);
	MASK_OP(vol2_ind, "del", mask, true);
	MASK_OP(vol_ind, "del", mask, true);

	// Single volume, 2 non-overlapping masks

	mask = "00000000,00000000,00000000,00000003";
	mask2 = "00000000,00000000,00000000,0000000c";
	MASK_OP(vol_ind, "add", mask, true);
	MASK_OP(vol_ind, "add", mask2, true);
	MASK_OP(vol_ind, "del", mask2, true);
	MASK_OP(vol_ind, "del", mask, true);

	// Single volume, 2 overlapping masks - expect failure

	mask = "00000000,00000000,00000000,00000003";
	mask2 = "00000000,00000000,00000000,0000000a";
	MASK_OP(vol_ind, "add", mask, true);
	MASK_OP(vol_ind, "add", mask2, false);
	MASK_OP(vol_ind, "del", mask, true);

	// 2 volumes, 2 overlapping masks - expect failure

	mask = "00000000,00000000,00000000,00000003";
	mask2 = "00000000,00000000,00000000,0000000e";
	MASK_OP(vol_ind, "add", mask, true);
	MASK_OP(vol2_ind, "add", mask2, false);
	MASK_OP(vol_ind, "del", mask, true);

	// Empty mask - expect failure

	MASK_OP(vol_ind, "add", "00000000,00000000,00000000,00000000", false);

	// Removal of non-existent mask - expect failure

	MASK_OP(vol_ind, "del", "00000000,00000000,00000c00,00000000", false);

	// Removal of non-existent (overlapping) mask - expect failure

	mask = "00000000,00000000,00000000,00000003";
	MASK_OP(vol_ind, "add", mask, true);
	MASK_OP(vol_ind, "del", "00000000,00000000,00000000,00000002", false);
	MASK_OP(vol_ind, "del", mask, true);

	// Add one mask to a volume detach that volume, then add a partially overlapping one - expect success
	mask = "00000000,00000000,00000000,00000030";
	mask2 = "00000000,00000000,00000000,00000020";
	MASK_OP(vol_ind, "add", mask, true);
	send_command_to_vol(sys, -1, vol_ind, volCmds_Detach);
	send_command_to_vol(sys, -1, vol_ind, volCmds_New);
	MASK_OP(vol_ind, "add", mask2, true);
	MASK_OP(vol_ind, "del", mask2, true);


	#undef MASK_OP

	return 0;
}

TEST_FUNC int unitest_volumes_config(struct NVMeshSystem *sys) {
	int rv = 0;
	rv |= SIMU_RUN_TEST(unitest_FS_InvalidCfg,sys);
	rv |= SIMU_RUN_TEST(unitest_ioctls_fs_proc,sys);
	rv |= SIMU_RUN_TEST(unitest_sub_vols,sys);
        rv |= SIMU_RUN_TEST(unitest_update_non_existing_volume, sys);
	rv |= SIMU_RUN_TEST(unitest_DetachAttachVolume,sys);
	rv |= SIMU_RUN_TEST(unitest_AttachExistingVolume,sys);
	rv |= SIMU_RUN_TEST(unitest_recovery_hidden_volume,sys);
	rv |= SIMU_RUN_TEST(unitest_upgrade_nvmeibc_with_volumes,sys);
	rv |= SIMU_RUN_TEST(unitest_delayed_volume_reboot,sys);
	rv |= SIMU_RUN_TEST(unitest_multi_clnt_instances,sys);
	rv |= SIMU_RUN_TEST(unitest_cpu_masks,sys);
	rv |= SIMU_RUN_TEST(unitest_RefIDs_OfExistingVolume,sys);
	BUG_ON(!NVMeshSystem_is_stable(sys));
	return rv;
}
