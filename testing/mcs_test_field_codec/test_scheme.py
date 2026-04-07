{
	"__globals" : {
		"__version" : 100,
		"__origin_type" : "TOMA",
		"__scheme_name" : "NVMEIBT_MCS_TOMA_SCHEME"
	},
	"clnt_toma_common_scheme.json" : "include",
	"segments_status" : {
		"//" : "toma message sent to server to report status of segments",
		"__type" : "msg",
		"__opcode" : 16,
		"__route" : "/disks/updateDiskSegmentsStatus",
		"__priority" : 2,
		"__resendOnFailover" : false,
		"__dropOnDisconnection" : true,
		"__typename": "nvmeibt_mcs_segments_status",
		"segmentsUpdate" : {
			"__type" : "array",
			"__typename" : "nvmeibt_mcs_segment_status_",
			"__counter" : "n_segments",
			"__offset" : "segments"
		}
	},
	"register_to_events" : {
		"//" : "message that register toma to management events",
		"__type" : "msg",
		"__opcode" : 17,
		"__upstream" : true,
		"__downstream" : false,
		"__route" : "/registerToEvents",
		"__priority" : 1,
		"__resendOnFailover" : true,
		"__dropOnDisconnection" : false,
		"__typename": "nvmeibt_mcs_register_to_events"
	},
	"nvmeibt_mcs_segment_status_" : {
		"//" : "single segment status",
		"__type" : "used",
		"destinationSegmentID" : "36s",
		"sourceSegmentID" : "64s",
		"praidVersion" : "__codec MajorMinorCodec()",
		"remainingDirtyBits" : "q",
		"isLeader" : "q",
		"status" : "16s",
		"vitality" : "16s"
	},
	"reg_to_shutdown" : {
		"__type" : "msg",
		"__opcode" : 15,
		"__route" : "/servers/checkForControlJobs/{hostname}",
		"__typename": "nvmeibt_mcs_reg_to_shutdown",
		"hostname" : "40s",
		"__priority" : 2,
		"__resendOnFailover" : true,
		"__dropOnDisconnection" : false
	},
	"management_log_message": {
		"//" : "toma log message to server to display as alert",
		"__type": "msg",
		"__route" : "/log",
		"__opcode": 28,
		"__upstream": true,
		"__downstream": false,
		"__resendOnFailover" : false,
		"__dropOnDisconnection" : false,
		"__priority" : 1,
		"__typename": "nvmeibt_mcs_log_msg",
		"level": "8s",
		"header": "96s",
		"message": "256s"
	},
	"management_raft_role_message": {
		"//" : "toma tells mgmt whether leader or not",
		"__type": "msg",
		"__route" : "/raft_role",
		"__opcode": 29,
		"__upstream": true,
		"__downstream": false,
		"__resendOnFailover" : false,
		"__dropOnDisconnection" : false,
		"__priority" : 1,
		"__typename": "nvmeibt_mcs_raft_role_msg",
		"raftRole": "20s",
		"segmentID" : "37s",
		"praidVersionMajor" : "I",
		"praidVersionMinor" : "I",
		"raftTerm" : "I"
	},
	"shutdown" : {
		"__type" : "msg",
		"__opcode" : 12,
		"__typename": "nvmeibt_mcs_shutdown",
		"control" : "16s",
		"_id" : "40s"
	},
	"nvmeibt_mcs_disk_" : {
		"//" : "single disk configuration",
		"__type" : "used",
		"uuid": "__codec UuidCodec()",
		"diskID" : "40s",
		"version": "__codec WithDefaultCodec(PodCodec('I'), 1)"
	},
	"nvmeibt_mcs_nic_" : {
		"//" : "nic configuration",
		"__type" : "used",
		"pkey" : "H",
		"uuid" : "__codec UuidCodec()",
		"guid": "__codec GuidCodec()",
		"protocol" : "__codec WithDefaultCodec(MultiValCodec({'Unknown':-1,'Infiniband':0,'RoCE':1,'TCP':2,'__DEFAULT':-1}), 'Unknown')",
		"version": "__codec WithDefaultCodec(PodCodec('I'), 1)"
	},
	"nvmeibt_mcs_segment_" :{
		"__type" : "used",
		"uuid" : "__codec UuidCodec()",
		"type" : "__codec MultiValCodec({'data' : 1, 'raftonly' : 2})",
		"lbs" : "Q",
		"lbe" : "Q",
		"pRaidTypeIndex" : "I",
		"pRaidIndex" : "I",
		"status" :  "__codec WithDefaultCodec(MultiValCodec({'__DEFAULT' : 0, 'markedForRebuild_old' : 1}), 0)",
		"diskUUID" : "__codec UuidCodec()",
		"nodeUUID" : "__codec UuidCodec()",
		"allocationIndex" : "__codec WithDefaultCodec(PodCodec('I'), 0)"
	},
	"nvmeibt_mcs_chunk_" : {
		"//" : "chunk configuration",
		"__type" : "used",
		"uuid" : "__codec UuidCodec()",
		"vlbs" : "Q",
		"vlbe" : "Q",
		"pRaids": {
			"__type": "array",
			"__typename": "nvmeibt_mcs_praid_",
			"__counter": "n_praid",
			"__offset": "pRaids"
		}
	},
	"nvmeibt_mcs_praid_" : {
		"__type" : "used",
		"uuid" : "__codec UuidCodec()",
		"activated": "H",
		"stripeIndex": "I",
		"lock_scheme" : "nvmeibc_locks_scheme_conf",
		"diskSegments" :{
			"__type" : "array",
			"__typename" : "nvmeibt_mcs_segment_",
			"__counter" : "n_segments",
			"__offset" : "segments"
		 }
	},
	"nvmeibt_mcs_volume_" : {
		"//" : "volume configuration",
		"__type" : "used",
		"type" : "__codec MultiValCodec({'normal' : 0, 'recoverer' : 1, 'thin' : 3})",
		"status" : "__codec WithDefaultCodec(MultiValCodec({'__DEFAULT' : 0, 'markedForRebuild_old' : 1, 'markedForDeletion' : 2}), 0)",
		"RAIDLevel" : "__codec MultiValCodec({'Striped RAID-0' : 0, 'Mirrored RAID-1' : 1, 'Striped & Mirrored RAID-10' : 10, 'Concatenated' : 3})",
		"numberOfMirrors" : "__codec IncrementCodec(1, 'H')",
		"blockSize": "H",
		"blocks": "Q",
		"name" : "16s",
		"version" : "H",
		"uuid" : "__codec UuidCodec()",
		"stripeWidth" : "__codec WithDefaultCodec(PodCodec('I'), 1)",
		"stripeSize" : "__codec WithDefaultCodec(PodCodec('I'), 0)",
		"chunks" : {
			"__type" : "array",
			"__typename" : "nvmeibt_mcs_chunk_",
			"__counter" : "n_chunks",
			"__offset" : "chunks"
		}
	},
	"nvmeibt_mcs_target_" : {
		"//" : "toma target description",
		"__type" : "used",
		"node_id" : "40s",
		"uuid" : "__codec UuidCodec()",
		"lock_scheme": "__codec DefaultFromFieldProjection(FieldValue('nvmeibc_locks_scheme_conf'), 'lock_scheme', 1)",
		"disks" : {
			"__type" : "array",
			"__typename" : "nvmeibt_mcs_disk_",
			"__counter" : "n_disks",
			"__offset" : "disks"
		},
		"nics" : {
			"__type" : "array",
			"__typename" : "nvmeibt_mcs_nic_",
			"__counter" : "n_nics",
			"__offset" : "nics"
		}
	},
	"nvmeibt_mcs_toma_config_" : {
		"//" : "toma configuration message used",
		"__type" : "msg",
		"__opcode" : 17,
		"__upstream" : false,
		"__downstream" : true,
		"targets" : {
			"__type" : "array",
			"__typename" : "nvmeibt_mcs_target_",
			"__counter" : "n_targets",
			"__offset" : "targets"
		},
		"volumes" : {
			"__type" : "array",
			"__typename" : "nvmeibt_mcs_volume_",
			"__counter" : "n_volumes",
			"__offset" : "volumes"
		},
		"managementConfiguration": {
			"dbUUID": "__codec UuidCodec()",
			"configurationVersion": "i",
			"protocolVersion": "I"
		},
		"updateType": "__codec MultiValCodec({'full' : 0, 'incremental' : 1})"

	}
}
