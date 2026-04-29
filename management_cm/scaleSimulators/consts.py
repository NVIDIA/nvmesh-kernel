# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

class Consts:
	PID_BASE_DIR = '/var/run/scaleSimulator/'
	SYSLOG_PATH = '/dev/log'
	JSON_UDS = '/var/run/nvmesh/json_uds'
	BLOCK_SIZE = 4096
	CONFIG_DIR = '/var/opt/scaleSimulator/'
	CLIENT_CONFIG_FILE = 'client-config.json'
	AGENT_CONFIG_FILE = 'agent-config.json'
	TARGET_CONFIG_FILE = 'target-config.json'
	UPGRADE_AGENT_CONFIG_FILE = 'upgrade-agent-config.json'
	NODE_CONFIG_FILE = 'node-config.json'
	NODE_PERSISTENCY_FILE = 'node-persistency.json'
	ZONE_BEFORE_ASSIGNEMENT = '-1'
	KEYS_DIR = '/etc/nvmesh/keys/'
	# Sentinel diskUUID used in the reinstate handshake to tell the simulator
	# "treat this segment as non-existent on a real drive and report CONF_CORRUPTED".
	REINSTATE_FAKE_DRIVE_UUID = '11111111-1111-1111-1111-111111111111'

class Components:
	TOMA = 'TOMA'
	LEADER = 'TOMA_LEADER'
	CLIENT = 'CLIENT'
	TARGET = 'TARGET'
	UPGRADE_AGENT = 'UPGRADE_AGENT'
	MANAGEMENT_AGENT = 'MANAGEMENT_AGENT'

class VolumeStatuses:
	INITIALIZING = 'initializing'
	UNAVAILABLE = 'unavailable'

class VolumeActions:
	MARKED_FOR_DELETION = 'markedForDeletion'
	DELETING = 'deleting'

class DiskStatus:
	NOT_INITIALIZED = 'Not_Initialized'
	FORMATTING = 'Formatting'
	INITIALIZING = 'Initializing'
	OK = 'Ok'

class PeriodicMessagesIntervals:
	CLIENT_KEEP_ALIVE = 5
	AGENT_KEEP_ALIVE = 5
	TOMA_KEEP_ALIVE = 5
	UPGRADE_AGENT_KEEP_ALIVE = 5
	DIRTY_BITS_UPDATE = 2
	VOLUME_ZEROING_UPDATE = 2

class IntervalBetweenMessages:
	IO_DISABLED_TO_IO_ENABLED = 1
	DEAD_TO_UNDER_RECOVERY_SEGMENT = 3

class DiskSegmentStatuses:
	NORMAL = 'normal'
	DEPRECATED = 'deprecated'
	DEAD = 'dead'
	UNDER_RECOVERY = 'under_recovery'
	MARKED_FOR_REBUILD_OLD = 'markedForRebuild_old'
	MARKED_FOR_REBUILD = 'markedForRebuild'
	REMAP = 'remap'
	ZEROING = 'zeroing'
	CONF_CORRUPTED = 'conf_corrupted'

class DiskSegmentVitalities:
	UP = 'up'
	DOWN = 'down'

class Messages:
	TARGET_REPORT = '/servers/report'
	TOMA_KEEP_ALIVE = '/toma/keepAlive'
	CLIENT_REPORT = '/clients/report'
	UPDATE_ATTACHMENT_STATUS = '/clients/updateAttachmentStatus'
	CHECK_FOR_CLIENT_CONTROL_JOBS = '/clients/checkForControlJobs'
	UPDATE_DRIVE_ZEROING_PROGRESS = '/updateDriveZeroingProgress'
	CLIENT_KEEP_ALIVE = '/clients/keepAlive'
	CLIENT_CONFIGURATION = '/clients/getConfiguration'
	UPDATE_PRAID_STATUS = '/volumes/updatepRaidStatus'
	UPDATE_DISK_SEGMENTS_DIRTY_BITS = '/volumes/updateDiskSegmentsDirtyBits'
	UPDATE_DISK_SEGMENTS_STATUS = '/volumes/updateDiskSegmentsStatus/'
	UPDATE_SEGMENTS_ZEROING_PROGRESS = '/updateSegmentZeroingProgress'
	RAFT_ROLE = '/raftRole'

class MessageOpCode:
	CHECK_FOR_CLIENT_CONTROL_JOBS = 13
	CONFIGURATION_CHANGE_MESSAGE = 17
	CLIENT_CONFIGURATION = 21
	ERROR_RESPONSE = 25
	VOLUME_REMOVED_EVENT = 27
	FORMAT_DISK_EVENT = 38
#	RESEND_REPORT_EVENT = 39   DEPRECATED
	SEGMENTS_CHANGES_ON_DISK_EVENT = 48
	SEND_REPORT_EVENT = 49
	CLIENT_KEEP_ALIVE = 50
	TOMA_KEEP_ALIVE = 55
	VOLUME_EXTENDED_EVENT = 29
	UPDATE_DISK_SEGMENTS_STATUS_RESPONSE = 16
	UPDATE_DISK_SEGMENTS_DIRTY_BITS = 44
	GET_RESERVATION_VERSION = 5
	SEND_CONFIGURATION_RESPONSE = 66
	UPDATE_CLIENT_KEEPALIVE_TOKEN = 69

	ATTACH_VOLUMES = 90
	UPDATE_VOLUMES = 91
	DETACH_VOLUMES = 92
	UPDATE_TARGET_NICS = 93

class FormatType:
	FORMAT_EC = 'format_ec'
	FORMAT_RAID = 'format_raid'

class VolumeAttachmentStatus:
	BUSY = 1
	DETACHED = 2
	DETACH_FAILED = 3
	ATTACHED = 4
	ATTACH_FAILED = 5

class SegmentType:
	DATA = 'data'
	RAFT_ONLY = 'raftonly'
	EXCELERO_METADATA = 'excelero_metadata'

class RaftRole:
	FOLLOWER = 'FOLLOWER'
	LEADER = 'LEADER'

class ClientStatus:
	INITIALIZING = 0
	READY = 1
	PREP_RM = 2
	RM_RDY = 3
	EXITING = 4
	DOWN = 5
	UNAVAILABLE = 6

class Convert:
	BLOCKSET_TO_BYTES = 32 * 4096

class ReservationMode:
	NONE = 0
	SHARED_READ_ONLY = 1
	SHARED_READ_WRITE = 2
	EXCLUSIVE_READ_WRITE = 3

	@staticmethod
	def fromString(string):
		return ReservationMode.__dict__[string]

class ReservationModePreempts:
	NO_PREEMPT = 0
	WEAK_PREEMPT = 1
	PREEMPT = 2
	UNKNOWN = 3

class KafkaAutoOffsetReset:
	LATEST = 'latest'
	EARLIEST = 'earliest'

class MessageTypes:
	# MGMT->TOMA
	HARDWARE_CONFIGURATION = 'hardwareConfiguration'
	ADD_VOLUME = 'addVolume'
	DELETE_VOLUME = 'deleteVolume'
	UPDATE_VOLUME = 'updateVolume'
	DELETE_TARGET = 'deleteTarget'
	ADD_TARGET = 'addTarget'
	FORMAT_DRIVE = 'formatDrive'
	RESEND_REPORT = 'resendReport'
	SEND_PRAID_REPORT = 'sendPRaidReport'
	UPDATE_TOMA_KEEPALIVE_TOKEN = 'updateTomaKeepaliveToken'
	UPDATE_LEADER_KEEPALIVE_TOKEN = 'updateLeaderKeepaliveToken'
	DELETE_VOLUME_COMPLETED = 'deleteVolumeCompleted'
	INIT_ENCRYPTION = 'initEncryption'
	ADD_PASSPHRASE = 'addPassphrase'
	DELETE_PASSPHRASE = 'deletePassphrase'
	ROTATE_PASSPHRASE = 'rotatePassphrase'
	ENCRYPTION_REQUEST_RESPONSE = 'encryptionRequestResponse'
	# TOMA->MGMT
	UPDATE_PRAID_REPORT = 'updatePRaidReport'
	SEND_PRAID_REPORT_RESPONSE = 'sendPRaidReportResponse'
	RAFT_ROLE = 'raftRole'
	TOMA_KEEPALIVE = 'keepalive'
	LEADER_KEEPALIVE = 'leaderKeepalive'
	DRIVE_ZEROING_PROGRESS = 'driveZeroingProgress'
	SEGMENT_ZEROING_PROGRESS = 'segmentZeroingProgress'
	UPDATE_DISK_SEGMENTS_DIRTY_BITS = 'updateDiskSegmentsDirtyBits'
	ENCRYPTION_COMMAND_RESPONSE = 'encryptionCommandResponse'
	REPORT_TARGET = 'reportTarget'
	# MGMT->AGENT
	UPDATE_AGENT_TOKEN = 'updateAgentToken'
	UPDATE_CONFIG_PROFILE = 'updateConfigProfile'
	SPDK_SNAPSHOT_COMMAND = 'spdkSnapshotCommand'
	# AGENT->MGMT
	AGENT_KEEPALIVE = 'keepalive'
	CONFIG_PROFILE_UPDATED = 'configProfileUpdated'
	UPDATE_CONFIG_PROFILE_USER_OVERRIDE = 'updateConfigProfileUserOverride'
	SPDK_SNAPSHOT_COMMAND_RESULT = 'spdkSnapshotCommandResult'
	UPDATE_KEYS = 'updateKeys'
	# CLIENT>MGMT
	CLIENT_KEEPALIVE = 'keepalive'
	# REPORT_CLIENT = 'reportClient'   DEPRECATED
	UPDATE_ATTACHMENT_STATUS = 'updateAttachmentStatus'
	GET_CONFIGURATION = 'getConfiguration'
	LOG = 'log'
	UPDATE_LOG = 'updateLog'
	ACK_LOG = 'ackLog'
	# UPGRADE_AGENT -> MGMT
	UPGRADE_AGENT_KEEPALIVE = 'keepalive'
	UPGRADE_AGENT_COMMAND_RESULT = 'commandResult'
	# MGMT ->UPGRADE_AGENT
	UPDATE_UPGRADE_AGENT_KEEPALIVE_TOKEN = 'updateUpgradeAgentKeepaliveToken'
	UPGRADE_AGENT_COMMAND = 'upgradeAgentCommand'

	# New Client Protocol
	ATTACH_VOLUMES = 'attachVolumes'
	UPDATE_VOLUMES = 'updateVolumes'
	DETACH_VOLUMES = 'detachVolumes'
	GET_TARGET_NICS = 'getTargetNICs'

class Rank:
	PRIORITY = 'priority'
	LOW = 'low'

class MetadataCapabilities(object):
	NOT_SUPPORTED = 0
	INLINE = 1
	SEPARATE = 2
	BOTH = 3

class NvmeshUMStatus:
	DISABLED = 'disabled'
	UP = 'up'
	DOWN = 'down'

class SPDKCommands:
	ATTACH_SNAPSHOT = 'attach_snapshot'
	DETACH_SNAPSHOT = 'detach_snapshot'
