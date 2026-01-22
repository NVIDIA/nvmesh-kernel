# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

import argparse
import socket
import datetime
import uuid
import json

from component import Component
from consts import Consts, Components, FormatType, DiskStatus, SegmentType, RaftRole, Convert, MessageTypes, \
	Rank, MetadataCapabilities, DiskSegmentStatuses, DiskSegmentVitalities, \
	PeriodicMessagesIntervals, IntervalBetweenMessages


class TOMA(Component):
	def __init__(self):
		self.defaultKeepaliveInterval = PeriodicMessagesIntervals.TOMA_KEEP_ALIVE
		self.isLeader = False
		Component.__init__(self, configFile=Consts.TARGET_CONFIG_FILE)
		self.pRaids = {}
		self.tomaToken = -1
		self.tomaLeaderToken = 0
		self.messageSequence = 0
		self.lastKeepAliveTime = None
		self.driveZeroBlocksDivider = 10
		self.lastSegmentsStatusReport = None
		self.healthStatus = 1
		self.lastReportTime = None
		self.lastDriveZeroingTime = None
		self.driveZeroingInterval = None
		self.managementConfiguration = {}
		self.configurationVersion = -1
		self.performFullDriveFormat = None
		self.diskBlocks = 195352575
		self.use32BlockSetSize = False  # default 256
		self.holdPeriodicTargetReports = False
		self.autoLeaderElection = False
		self.disksOnFormat = {}
		self.allVolumeSegments = {}
		self.target = None
		self.cachedInitState = None
		self.featureCompatibilityVersion = '0'
		self.tomaSoftwareVersion = '784'
		self.encryptionResult = 1
		# This should be increased whenever leader is being changed.
		self.raftTerm = 1
		self.raftRole = RaftRole.FOLLOWER
		self.selectedLeaderNodeID = None
		self.volumesUnderRebuild = {}
		self.autoRebuild = True
		self.rebuildSettings = self.getDefaultRebuildSettings()
		self.volumeZeroingOn = False
		self.volumeZeroingSettings = self.getDefaultVolumeZeroingSettings()
		self.volumesInZeroing = {}
		self.lastVolumeZeroingIteration = None
		self.readConfigFromFile(updateInitState=True)
		self.init = True
		self.zone = None
		self.targets = {}
		self.volumes = {}

	# kafka utils methods
	def getManagementTopicName(self, priority=True):
		prefix = 'zone{}.'.format(self.zone) if self.zone else 'default.'
		suffix = Rank.PRIORITY if priority else Rank.LOW

		return '{}management.{}.1.0.0'.format(prefix, suffix)

	def getInterestConsumersConfig(self):
		topicsConfig = []

		topicsConfig.append({'name': '{}.TOMA.commands.1.0.0'.format(self.hostname), 'partition': 0})

		if self.zone:
			topicsConfig.append({'name': 'zone{}.TOMA.hardwareConfiguration.1.0.0'.format(self.zone), 'partition': 0})

		if self.isLeader:
			topicsConfig.append({'name': 'zone{}.leader.incrementalUpdates.1.0.0'.format(self.zone), 'partition': 0})
			topicsConfig.append({'name': 'zone{}.leader.incrementalTargetUpdates.1.0.0'.format(self.zone), 'partition': 0})

		return topicsConfig

	def beforeKafkaInitiated(self):
		tempIsLeader = self.isLeader
		self.isLeader = None
		self.setIsLeader(tempIsLeader)

	def afterKafkaInitiated(self):
		self.sendKeepaliveMessage()

	def produceMessageToTopic(self, message, topic, key=None):
		Component.produceMessageToTopic(self, message, topic, key)
		self.messageSequence += 1

	def getMessageSequence(self):
		return self.messageSequence

	### global utils methods
	def getType(self):
		return Components.TOMA

	def getComponentSpecificMessageHeaders(self):
		return {
			'messageSequence': self.getMessageSequence(),
			'hostname': self.hostname,
			'tomaToken': self.tomaToken,
			'leaderToken': self.tomaLeaderToken if self.isLeader else None,
		}

	def doBeforeExit(self):
		self.logger.info('Saving persistencies')
		objToUpdate = self.getPersistentDataToSave()
		self.updatePersistentData(objToUpdate)
		self.logger.info('Exit...')

	def handleMessage(self, message):
		messageNotHandled = None
		messageType = message.get('messageType')
		payload = message.get('payload')
		encryptionMsgTypes = {MessageTypes.INIT_ENCRYPTION,
							  MessageTypes.ENCRYPTION_REQUEST_RESPONSE,
							  MessageTypes.ADD_PASSPHRASE,
							  MessageTypes.ROTATE_PASSPHRASE,
							  MessageTypes.DELETE_PASSPHRASE}

		if messageType == MessageTypes.HARDWARE_CONFIGURATION:
			messageNotHandled = self.handleHardwareConfigurationMessage(payload)
		elif messageType == MessageTypes.ADD_VOLUME:
			messageNotHandled = self.handleAddVolumeMessage(payload)
		elif messageType == MessageTypes.DELETE_VOLUME:
			messageNotHandled = self.handleDeleteVolumeMessage(payload)
		elif messageType == MessageTypes.UPDATE_VOLUME:
			messageNotHandled = self.handleUpdateVolumeMessage(payload)
		elif messageType == MessageTypes.ADD_TARGET:
			messageNotHandled = self.handleAddTargetMessage(payload)
		elif messageType == MessageTypes.DELETE_TARGET:
			messageNotHandled = self.handleDeleteTargetMessage(payload)
		elif messageType == MessageTypes.FORMAT_DRIVE:
			messageNotHandled = self.handleFormatDriveMessage(payload)
		elif messageType == MessageTypes.RESEND_REPORT:
			messageNotHandled = self.handleResendReportMessage(payload)
		elif messageType == MessageTypes.SEND_PRAID_REPORT:
			messageNotHandled = self.handleSendPRaidReportMessage(payload)
		elif messageType == MessageTypes.UPDATE_TOMA_KEEPALIVE_TOKEN:
			messageNotHandled = self.handleUpdateTomaKeepaliveToken(payload)
		elif messageType == MessageTypes.UPDATE_LEADER_KEEPALIVE_TOKEN:
			messageNotHandled = self.handleUpdateLeaderKeepaliveToken(payload)
		elif messageType == MessageTypes.DELETE_VOLUME_COMPLETED:
			messageNotHandled = self.handleDeleteVolumeCompletedMessage(payload)
		elif messageType in encryptionMsgTypes:
			messageNotHandled = self.handleEncryptionMessage(payload, messageType)
		else:
			self.logger.warning('Unable to handle message messageType {}. Ignoring this message...'.format(messageType))

		return messageNotHandled

	def getPersistentDataToSave(self):
		persistentData = {'targetReport': self.target}

		if self.isLeader:
			# adding persistent data that is relevant for a leader
			persistentData.update({
				'configuration': self.managementConfiguration,
				'configurationVersion': self.configurationVersion,
				'pRaids': self.pRaids,
				'leader': self.selectedLeaderNodeID
			})

		return persistentData

	def updatePersistentData(self, updateObject):
		self.cachedInitState.update(updateObject)
		self.updateInitState(self.cachedInitState)

	def producePeriodicReports(self):
		if not self.lastKeepAliveTime or (self.tomaToken >= 0 and Component.isTimeForNextMsg(self.lastKeepAliveTime, self.getKeepaliveInterval())):
			self.sendKeepaliveMessage()

		if self.performFullDriveFormat and self.disksOnFormat and (
				not self.lastDriveZeroingTime or Component.isTimeForNextMsg(self.lastDriveZeroingTime, self.driveZeroingInterval)):
			self.handleFormatDiskProcess(self.disksOnFormat)
			self.lastDriveZeroingTime = datetime.datetime.now()

		if self.isLeader:
			self.handleRebuildingVolumes()
			self.handleVolumesZeroing()

	# TOMA -> MGMT messages
	def sendReportTargetMessage(self):
		if not self.zone:
			self.logger.debug('Not going to send report target as we did not get a zone yet')
			return

		payload = {'node': self.target}
		message = self.buildGenericMessage(messageType=MessageTypes.REPORT_TARGET, payload=payload)

		self.logger.debug('Going to send report target!')
		self.produceMessageToTopic(message, self.getManagementTopicName())

		self.lastReportTime = datetime.datetime.now()

	def sendUpdatePRaidReportMessage(self, pRaidsToUpdate={}, incMinor=False, isResponse=False):
		if not self.isLeader:
			self.logger.debug('Not going to send praid report - not a leader!')
			return

		if not pRaidsToUpdate or not self.pRaids:
			self.logger.debug('Not going to send empty update praid report, aborting...')
			return

		if incMinor:
			for p in pRaidsToUpdate.values():
				p['pRaidMinorVersion'] += 1

		messageType = MessageTypes.SEND_PRAID_REPORT_RESPONSE if isResponse else MessageTypes.UPDATE_PRAID_REPORT
		payload = {'pRaidsUpdate': self.generatePraidReport(pRaidsToUpdate)}
		message = self.buildGenericMessage(messageType=messageType, payload=payload)

		self.logger.debug('Going to send {}!'.format(messageType))
		self.produceMessageToTopic(message, self.getManagementTopicName())

	def sendKeepaliveMessage(self):
		zoneToSend = self.zone if self.zone else Consts.ZONE_BEFORE_ASSIGNEMENT
		payload = {'zone': zoneToSend,
				   'version': self.version,
				   'featureCompatibilityVersion': self.featureCompatibilityVersion,
				   'tomaSoftwareVersion': self.tomaSoftwareVersion,
				   'leaderUUID': self.selectedLeaderNodeID}
		message = self.buildGenericMessage(messageType=MessageTypes.TOMA_KEEPALIVE, payload=payload)

		self.logger.debug('Going to send keepalive message with tomaToken {} zone {}'.format(message['tomaToken'], payload['zone']))
		self.produceMessageToTopic(message, self.getManagementKeepAliveTopicName())

		self.lastKeepAliveTime = datetime.datetime.now()

		if self.isLeader:
			self.sendLeaderKeepaliveMessage()

	def sendLeaderKeepaliveMessage(self):
		if self.zone:
			payload = {'nodeID': self.hostname, 'zone': self.zone, 'raftTerm': self.raftTerm, 'version': self.version,
					   'featureCompatibilityVersion': self.featureCompatibilityVersion}
			message = self.buildGenericMessage(messageType=MessageTypes.LEADER_KEEPALIVE, payload=payload)

			self.logger.debug('Going to send leader keepalive message with leaderToken {} zone {} raftTerm {}'.format(message['leaderToken'], payload['zone'],
																													  payload['raftTerm']))
			self.produceMessageToTopic(message, self.getManagementKeepAliveTopicName())

	def sendDriveZeroingProgressMessage(self, disk, formatDetails):
		zeroPortionSize = disk['blocks'] / self.driveZeroBlocksDivider

		disk['writeCounter'] += zeroPortionSize
		formatDetails['nZeroedBlks'] += zeroPortionSize

		if formatDetails['nZeroedBlks'] >= disk['blocks']:
			formatDetails['nZeroedBlks'] = disk['blocks']

		payload = {
			'nodeID': self.hostname,
			'diskUUID': formatDetails['uuid'],
			'zeroWriteCounter': disk['writeCounter'],
			'nZeroedBlks': formatDetails['nZeroedBlks']
		}
		message = self.buildGenericMessage(messageType=MessageTypes.DRIVE_ZEROING_PROGRESS, payload=payload)

		self.logger.debug('Going to send drive zeroing progress message for disk {}'.format(payload.get('diskUUID')))
		self.produceMessageToTopic(message, self.getManagementTopicName(priority=False))

	def sendSegmentZeroingProgressMessage(self, volume):
		for segment, pRaid, chunk in self.iterateVolumeSegments(volume):
			payload = {
				'segmentUUID': segment.get('uuid'),
				'praidVersion': pRaid.get('version'),
				'pRaidUUID': pRaid.get('uuid'),
				'nZeroedBlks': segment.get('nZeroedBlks')
			}
			message = self.buildGenericMessage(messageType=MessageTypes.SEGMENT_ZEROING_PROGRESS, payload=payload)

			self.logger.debug('Going to send segment zeroing progress message for segment {} in volume {}'.format(segment.get('uuid'), volume.get('name')))
			self.produceMessageToTopic(message, self.getManagementTopicName(priority=False))

	def sendUpdateDiskSegmentsDirtyBitsMessage(self, volumeUnderRebuild):
		volumeUnderRebuild['lastDBsMsgTime'] = datetime.datetime.now()

		payload = {'segmentsDirtyBitsUpdate': [self.generateDirtyBitsReport(volumeUnderRebuild)]}
		message = self.buildGenericMessage(messageType=MessageTypes.UPDATE_DISK_SEGMENTS_DIRTY_BITS, payload=payload)

		self.logger.debug('Going to send update dirty bits report message for volume {}'.format(volumeUnderRebuild['volume'].get('name')))
		self.produceMessageToTopic(message, self.getManagementTopicName(priority=False))

	def sendEncryptionCommandResponseMessage(self, volumeName, uuid, encryptionCommandIndex):
		payload = {'volumeName': volumeName,
				   'volumeUUID': uuid,
				   'encryptionCommandIndex': encryptionCommandIndex,
				   'result': self.encryptionResult,
				   'retryable': True if self.encryptionResult != 1 else False,
				   'error': 'some error' if self.encryptionResult != 1 else None}

		message = self.buildGenericMessage(messageType=MessageTypes.ENCRYPTION_COMMAND_RESPONSE, payload=payload)

		self.logger.debug('Going to send Encryption Command Response!')
		self.produceMessageToTopic(message, self.getManagementTopicName())

	# MGMT -> TOMA messages
	def handleHardwareConfigurationMessage(self, configuration):
		self.hardwareConfiguration = configuration
		newVersion = self.hardwareConfiguration['managementConfiguration']['configurationVersion']

		if newVersion <= self.configurationVersion:
			self.logger.debug('Got older configuration version {}, current version is {}'.format(newVersion, self.configurationVersion))
			return True

		self.configurationVersion = newVersion
		self.logger.debug('Got new hardware configuration with version {}'.format(self.configurationVersion))

		if 'targets' in configuration and len(configuration['targets']):
			leaderServer = configuration['targets'][0]

			if self.autoLeaderElection:
				self.setSelectedLeaderNodeID(leaderServer['_id'])

		if self.isLeader:
			self.init = False
		else:
			self.logger.debug('Got configuration but I\'m not a leader')

		return True

	def handleAddVolumeMessage(self, volume):
		volumeName = volume.get('name')
		self.logger.debug('Got new volume message for volume {}'.format(volumeName))

		if not self.isLeader:
			self.logger.debug('Got add volume message but I\'m not a leader')
			return True

		pRaidsToUpdate = {}
		self.volumes[volumeName] = volume

		for chunk in volume.get('chunks', []):
			for pRaid in chunk.get('pRaids', []):
				pRaidUUID = pRaid.get('uuid')
				self.createPRaidIfNeeded(pRaidUUID)
				pRaidsToUpdate[pRaidUUID] = self.pRaids[pRaidUUID]

				for diskSegment in pRaid.get('diskSegments', []):
					self.updateSegmentStatus(pRaidUUID, diskSegment.get('uuid'), DiskSegmentStatuses.NORMAL)

		self.sendUpdatePRaidReportMessage(pRaidsToUpdate=pRaidsToUpdate)

	def handleDeleteVolumeCompletedMessage(self, volume):
		volumeName = volume.get('name')
		self.logger.debug('Got delete volume completed message for volume {}'.format(volumeName))

		self.deleteVolume(volumeName)

	def handleDeleteVolumeMessage(self, volume):
		volumeName = volume.get('name')
		self.logger.debug('Got delete volume message for volume {}'.format(volumeName))

		if not self.isLeader:
			self.logger.debug('Got delete volume message but I\'m not a leader')
			return True

		cachedVolume = self.volumes.get(volumeName)
		if not cachedVolume:
			self.logger.error('volume to delete not found in cache: {}'.format(volumeName))
			return True

		shouldZeroVolume = self.volumeZeroingOn and not volumeName in self.volumesInZeroing

		if shouldZeroVolume:
			self.startZeroingVolume(cachedVolume)

		else:
			pRaidsToUpdate = {}

			for chunk in cachedVolume.get('chunks', []):
				for pRaid in chunk.get('pRaids', []):
					pRaidUUID = pRaid.get('uuid')
					pRaidsToUpdate[pRaidUUID] = self.pRaids[pRaidUUID]

					for segment in pRaid.get('diskSegments', []):
						self.updateSegmentStatus(pRaidUUID, segment.get('uuid'), DiskSegmentStatuses.DEPRECATED, DiskSegmentVitalities.DOWN)

			self.sendUpdatePRaidReportMessage(pRaidsToUpdate=pRaidsToUpdate)

	def handleUpdateVolumeMessage(self, volume):
		volumeName = volume.get('name')
		self.logger.debug('Got update volume message for volume {}'.format(volumeName))

		if not self.isLeader:
			self.logger.debug('Got update volume message but I\'m not a leader')
			return True

		pRaidsToUpdate = {}
		self.volumes[volumeName] = volume

		for chunk in volume.get('chunks', []):
			for pRaid in chunk.get('pRaids', []):
				pRaidUUID = pRaid.get('uuid')
				hasMarkedForRebuildOld = len([s for s in pRaid['diskSegments'] if s['status'] == DiskSegmentStatuses.MARKED_FOR_REBUILD_OLD]) > 0

				self.createPRaidIfNeeded(pRaidUUID)

				if self.autoRebuild and hasMarkedForRebuildOld:
					autoRebuildSettings = self.rebuildSettings.copy()
					autoRebuildSettings['sideUnderRecovery'] = 'auto'
					self.startRebuildVolume(volume, pRaid, autoRebuildSettings)

				else:
					for diskSegment in pRaid.get('diskSegments', []):
						status = DiskSegmentStatuses.NORMAL

						if diskSegment.get('status') == DiskSegmentStatuses.MARKED_FOR_REBUILD_OLD:
							status = DiskSegmentStatuses.DEPRECATED

						self.updateSegmentStatus(pRaidUUID, diskSegment['uuid'], status)

				pRaidsToUpdate[pRaidUUID] = self.pRaids[pRaidUUID]

		if pRaidsToUpdate:
			self.sendUpdatePRaidReportMessage(pRaidsToUpdate=pRaidsToUpdate)

	def handleAddTargetMessage(self, target):
		nodeID = target.get('nodeID')
		nodeUUID = target.get('uuid')
		targetsInZone = target.get('targetsInZone')
		targetUpdatesSequence = target.get('targetUpdatesSequence')
		self.logger.debug('Got add target message with nodeID: {} uuid: {} targetsInZone: {} targetUpdatesSequence: {}'.format(nodeID, nodeUUID, targetsInZone,
																															   targetUpdatesSequence))

		if not self.isLeader:
			self.logger.debug('Got add target message but I\'m not a leader')
			return True

		if nodeID and nodeUUID:
			self.targets[nodeID] = {'nodeID': nodeID, 'uuid': nodeUUID}

	def handleDeleteTargetMessage(self, target):
		nodeID = target.get('nodeID')
		nodeUUID = target.get('uuid')
		targetsInZone = target.get('targetsInZone')
		targetUpdatesSequence = target.get('targetUpdatesSequence')
		self.logger.debug(
			'Got delete target message with nodeID: {} uuid: {} targetsInZone: {} targetUpdatesSequence: {}'.format(nodeID, nodeUUID, targetsInZone,
																													targetUpdatesSequence))

		if not self.isLeader:
			self.logger.debug('Got delete target message but I\'m not a leader')
			return True

		self.logger.debug('targets status for this node: {}'.format(self.targets))

		if nodeID and nodeID in self.targets:
			del self.targets[nodeID]

	def handleFormatDriveMessage(self, payload):
		diskID = payload.get('diskID')
		formatRequestCounter = payload.get('formatRequestCounter')
		self.logger.debug('Got format drive message with driveID: {}, formatRequestCounter: {}'.format(diskID, formatRequestCounter))

		if diskID in self.disksOnFormat:
			self.logger.warning('Drive {} already in format process, ignoring...'.format(diskID))
			return

		diskToFormat = self.findDisk(diskID)

		if not diskToFormat:
			self.logger.warning('Drive {} could not be found, ignoring...'.format(diskID))
			return

		if formatRequestCounter <= diskToFormat['formatRequestCounter']:
			self.logger.warning('Got format request with wrong formatRequestCounter ({}) while diskToFormat is {}, ignoring...'.format(formatRequestCounter,
																																	   diskToFormat[
																																		   'formatRequestCounter']))
			return

		formatObj = {
			'disk': diskToFormat,
			'formatDetails': payload,
			'activeFormatRequestCounter': formatRequestCounter
		}

		self.holdPeriodicTargetReports = True
		self.disksOnFormat[diskID] = formatObj

		if self.performFullDriveFormat:
			diskToFormat['status'] = DiskStatus.FORMATTING
			self.sendReportTargetMessage()
		else:
			self.handleFormatDiskProcess({diskID: formatObj})

		self.holdPeriodicTargetReports = False

	def handleResendReportMessage(self, payload):
		tomaToken = payload.get('tomaToken')
		drives = payload.get('drives')
		self.logger.debug('Got resend report message with tomaToken: {} and driveIDs: {}'.format(tomaToken, [d.get('diskID') for d in drives]))

		self.updateTomaToken(tomaToken)

		for diskInfo in payload['drives']:
			diskInTarget = self.findDisk(diskInfo['diskID'])

			if diskInTarget:
				diskInTarget['reappearingCounter'] = diskInfo['reappearingCounter']

		self.sendReportTargetMessage()

	def handleSendPRaidReportMessage(self, payload):
		pRaids = payload.get('pRaids')
		self.logger.debug('Got send praid report message with {} pRaids'.format(len(pRaids)))

		if not self.isLeader:
			self.logger.debug('Got send pRaid report message but I\'m not a leader')
			return True

		pRaidsToUpdate = {p.get('uuid'): self.pRaids[p.get('uuid')] for p in pRaids}

		self.sendUpdatePRaidReportMessage(pRaidsToUpdate=pRaidsToUpdate, incMinor=True, isResponse=True)  # not sure about the incMinor

	def handleUpdateTomaKeepaliveToken(self, payload):
		zone = payload.get('zone')
		token = payload.get('token')
		keepaliveInterval = payload.get('keepaliveInterval')
		self.logger.debug(
			'Got update toma keepalive token message with keepaliveInterval: {} tomaToken: {} and zone: {}'.format(keepaliveInterval, token, zone))

		self.updateKeepAliveIntervalIfNeeded(keepaliveInterval)

		zoneUpdated = self.updateZone(zone)
		tokenUpdated = self.updateTomaToken(token)

		self.sendKeepaliveMessage()

		if zoneUpdated or tokenUpdated:
			self.sendReportTargetMessage()

	def handleUpdateLeaderKeepaliveToken(self, payload):
		token = payload.get('token')
		self.logger.debug('Got update leader keepalive token message with tomaLeaderToken: {}'.format(token))

		if not self.isLeader:
			self.logger.debug('Got update leader token message but I\'m not a leader')
			return True

		tokenUpdated = self.updateTomaToken(token, leader=True)

		self.sendLeaderKeepaliveMessage()

		if tokenUpdated:
			self.sendReportTargetMessage()

	def handleEncryptionMessage(self, payload, msgType):
		volumeName = payload.get('volumeName')
		uuid = payload.get('volumeUUID')
		encryptionCommandIndex = payload.get('encryptionCommandIndex')
		self.logger.debug('Got {} message for volume {}'.format(msgType, volumeName))
		self.sendEncryptionCommandResponseMessage(volumeName, uuid, encryptionCommandIndex)

	# TOMA utils methods
	def createPRaidIfNeeded(self, pRaidUUID):
		if not self.pRaids.get(pRaidUUID):
			self.pRaids[pRaidUUID] = {'diskSegments': {}, 'pRaidMajorVersion': 0, 'pRaidMinorVersion': 0, 'uuid': pRaidUUID}

	def handleRebuildingVolumes(self):
		for volume in list(self.volumesUnderRebuild.values()):
			if volume['changedToUnderRecovery']:
				self.handleRecoveryProcess(volume)
			else:
				if Component.isTimeForNextMsg(volume['timeOfDeadSegReport'], IntervalBetweenMessages.DEAD_TO_UNDER_RECOVERY_SEGMENT):
					self.changeRebuildSegmentStatus(volume, status=DiskSegmentStatuses.UNDER_RECOVERY)
					volume['changedToUnderRecovery'] = True

	def handleRecoveryProcess(self, volume):
		if volume['dbsStatusThreshold'] < volume['amountOfDBsToClean']:
			for s in volume['pRaid']['diskSegments']:
				pRaidUUID = volume['pRaid']['uuid']

				if s['status'] == DiskSegmentStatuses.MARKED_FOR_REBUILD_OLD:
					self.updateSegmentStatus(pRaidID=pRaidUUID, segmentID=s['uuid'], status=DiskSegmentStatuses.DEPRECATED)
					s['status'] = DiskSegmentStatuses.DEPRECATED
					self.sendUpdatePRaidReportMessage(pRaidsToUpdate={pRaidUUID: self.pRaids.get(pRaidUUID)})

				if s['status'] == DiskSegmentStatuses.DEPRECATED:
					if s['uuid'] in self.pRaids[pRaidUUID]['diskSegments']:
						del self.pRaids[pRaidUUID]['diskSegments'][s['uuid']]
					del s

			if Component.isTimeForNextMsg(volume['lastDBsMsgTime'], PeriodicMessagesIntervals.DIRTY_BITS_UPDATE):
				self.cleanDirtyBits(volume)
				self.sendUpdateDiskSegmentsDirtyBitsMessage(volume)
		else:
			self.changeRebuildSegmentStatus(volume, status=DiskSegmentStatuses.NORMAL)
			del self.volumesUnderRebuild[volume['volume']['_id']]

	def cleanDirtyBits(self, volume):
		remainingDirtyBits = volume['amountOfDBsToClean'] - (volume['dbsCleanRate'] * PeriodicMessagesIntervals.DIRTY_BITS_UPDATE)
		if remainingDirtyBits > volume['dbsStatusThreshold']:
			volume['amountOfDBsToClean'] = remainingDirtyBits
		else:
			volume['amountOfDBsToClean'] = volume['dbsStatusThreshold']

	def changeRebuildSegmentStatus(self, volume, status, vitality=DiskSegmentVitalities.UP):
		self.logger.debug("Updating segment to '{0}' on: volume: {1}, pRaid: {2}, segment: {3}".format(
			status, volume['volume']['_id'], volume['pRaid']['uuid'], volume['segmentUnderRebuildUUID'])
		)
		self.updateSegmentStatus(
			pRaidID=volume['pRaid']['uuid'],
			segmentID=volume['segmentUnderRebuildUUID'],
			status=status,
			vitality=vitality
		)

		self.sendUpdatePRaidReportMessage(pRaidsToUpdate={volume['pRaid']['uuid']: self.pRaids[volume['pRaid']['uuid']]})

	def getSegmentFromCollection(self, segments, segmentUUID):
		segmentFromCollection = None
		for seg in segments:
			if 'uuid' in seg and seg['uuid'] == segmentUUID or seg['partitionGuid'] == segmentUUID:
				segmentFromCollection = seg

		return segmentFromCollection

	def handleFormatDiskProcess(self, disksOnFormat):
		formatDoneDiskIDs = []

		for diskID, formatObj in disksOnFormat.items():
			disk = formatObj['disk']
			formatDetails = formatObj['formatDetails']
			diskStatus = disk['status']
			formatType = formatDetails['formatType']
			uuid = formatDetails['uuid']
			mgmtDBUUID = formatDetails['dbUUID']
			disk['formatRequestCounter'] = formatDetails['formatRequestCounter']
			disk['activeFormatRequestCounter'] = formatDetails['formatRequestCounter']

			if diskStatus == DiskStatus.INITIALIZING:
				if formatDetails['nZeroedBlks'] >= disk['blocks']:
					disk['status'] = DiskStatus.OK
					formatDoneDiskIDs.append(diskID)
					self.sendReportTargetMessage()
				else:
					self.sendDriveZeroingProgressMessage(disk, formatDetails)
			else:
				if not self.performFullDriveFormat:
					disk['status'] = DiskStatus.OK
					formatDoneDiskIDs.append(diskID)
				elif diskStatus == DiskStatus.FORMATTING:
					disk['status'] = DiskStatus.INITIALIZING
					formatDetails['nZeroedBlks'] = 0

				if formatType and formatType == FormatType.FORMAT_EC:
					disk['metadata_size'] = 8
					disk['block_size'] = 4096
				else:
					self.logger.warning("Scale Simulator might not support this format type at this time")
					disk['metadata_size'] = 0
					disk['block_size'] = 512

				disk['GPT'] = self.generateInitialGPT(uuid, mgmtDBUUID, formatType)

				self.sendReportTargetMessage()

		for diskID in formatDoneDiskIDs:
			del self.disksOnFormat[diskID]

	def findDisk(self, diskID):
		diskInTarget = None
		for disk in self.target['disks']:
			if disk['diskID'] == diskID:
				diskInTarget = disk
				break
		return diskInTarget

	def findNic(self, nicID):
		nicInTarget = None

		for nic in self.target['nics']:
			if nic['nicID'] == nicID:
				nicInTarget = nic
				break
		return nicInTarget

	def startRebuildVolume(self, volumeUnderRebuild, pRaid, rebuildSettings):
		self.logger.info(
			"Starting volume rebuild on: {0}, pRaidUUID: {1}, rebuildSettings: {2}".format(volumeUnderRebuild['name'], pRaid['uuid'], rebuildSettings))
		segmentUnderRebuildUUID = self.getUnderRecoverySegmentUUID(sideUnderRecovery=rebuildSettings['sideUnderRecovery'], pRaid=pRaid)
		self.addVolumeUnderRebuildData(volume=volumeUnderRebuild, pRaid=pRaid, segmentUnderRebuildUUID=segmentUnderRebuildUUID, rebuildSettings=rebuildSettings)
		self.changeRebuildSegmentStatus(
			volume=self.volumesUnderRebuild[volumeUnderRebuild['_id']],
			status=DiskSegmentStatuses.DEAD,
			vitality=DiskSegmentVitalities.DOWN
		)
		self.volumesUnderRebuild[volumeUnderRebuild['_id']]['timeOfDeadSegReport'] = datetime.datetime.now()

	def getPRaidFromVolume(self, volume, chunkNumber, pRaidNumber):
		return volume['chunks'][chunkNumber]['pRaids'][pRaidNumber]

	def getUnderRecoverySegmentUUID(self, sideUnderRecovery, pRaid):
		dataSegmentsOnPRaid = pRaid['diskSegments']
		if sideUnderRecovery == 'auto':
			# find the segment with MARKED_FOR_REBUILD
			markedForRebuildSegments = list(filter(lambda segment: segment['status'] == DiskSegmentStatuses.MARKED_FOR_REBUILD, dataSegmentsOnPRaid))
			markedForRebuildSegment = markedForRebuildSegments[0]
			underRecoverySegmentUUID = markedForRebuildSegment['uuid']
			self.logger.info("Automatic rebuild on segment {} with status {}".format(underRecoverySegmentUUID, markedForRebuildSegment['status']))
		else:
			underRecoverySegmentUUID = dataSegmentsOnPRaid[sideUnderRecovery]['uuid']

		return underRecoverySegmentUUID

	def addVolumeUnderRebuildData(self, volume, pRaid, segmentUnderRebuildUUID, rebuildSettings):
		def convertMaxToDBs():
			return (volume['blocks'] * volume['blockSize']) / Convert.BLOCKSET_TO_BYTES

		self.volumesUnderRebuild[volume['_id']] = {
			'volume': volume,
			'pRaid': pRaid,
			'segmentUnderRebuildUUID': segmentUnderRebuildUUID,
			'changedToUnderRecovery': False,
			'timeOfDeadSegReport': datetime.datetime.max,
			'lastDBsMsgTime': datetime.datetime.max,
			'dbsStatusThreshold': rebuildSettings['dbsStatusThreshold'],
			'dbsCleanRate': rebuildSettings['dbsCleanRate'],
			'amountOfDBsToClean': convertMaxToDBs() if rebuildSettings['amountOfDBsToClean'] == 'max' else rebuildSettings['amountOfDBsToClean']
		}

	def updateSegmentStatus(self, pRaidID, segmentID, status, vitality=DiskSegmentVitalities.UP):
		self.logger.debug("updateSegmentStatus called with segmentID %s and status %s" % (segmentID, status))

		pRaid = self.pRaids.get(pRaidID)

		if not pRaid:
			self.logger.warning("updateSegmentStatus called with pRaid uuid %s that doesn't exist in self.pRaids. ignoring" % pRaidID)
			return

		if segmentID not in pRaid['diskSegments']:
			pRaid['diskSegments'][segmentID] = {'segmentID': segmentID, 'status': '', 'vitality': vitality}

		diskSegment = pRaid['diskSegments'][segmentID]

		if diskSegment['status'] != status:
			diskSegment['status'] = status
			pRaid['pRaidMajorVersion'] += 1
			return True

	def setIsLeader(self, isLeader, resubscribeToTopic=False):
		self.logger.debug("setIsLeader %s" % isLeader)

		if self.isLeader != isLeader:
			self.isLeader = isLeader

			if self.isLeader:
				self.logger.info("I'm now the leader!")
				self.raftRole = RaftRole.LEADER
			else:
				self.logger.info("I'm not the Leader anymore!")
				self.raftRole = RaftRole.FOLLOWER
				self.leaderToken = None
				self.pRaids = {}

			self._updateLeaderNotationOnLogger()

			self.logger.debug('Going to resubscribe to consumers')
			self.initConsumer(resubscribe=resubscribeToTopic)

	def setSelectedLeaderNodeID(self, newLeaderNodeID):
		self.selectedLeaderNodeID = newLeaderNodeID
		self.setIsLeader(self.selectedLeaderNodeID == self.hostname, resubscribeToTopic=True)

	def _updateLeaderNotationOnLogger(self):
		logFormatWithoutLeader = self.getLoggerFormatter()
		logFormatWithLeader = self.getLoggerFormatter('_LEADER')
		newFormatter = logFormatWithLeader if self.isLeader else logFormatWithoutLeader
		for handler in self.logger.handlers:
			handler.setFormatter(newFormatter)

	def getDefaultRebuildSettings(self):
		return {
			'amountOfDBsToClean': 'max',
			'dbsCleanRate': 5000,
			'dbsStatusThreshold': 0,
			'sideUnderRecovery': 0
		}

	def getSegmentBySegmentID(self, segmentID):
		for pRaidUUID, pRaid in self.pRaids.items():
			if segmentID in pRaid['diskSegments']:
				seg = pRaid['diskSegments'][segmentID]
				return seg, pRaid

		return None, None

	def getDefaultVolumeZeroingSettings(self):
		return {
			# approx. 1 second for each 1GB of volume
			'blocksPerSecond': 100000,
		}

	def startZeroingVolume(self, volume):
		self.volumesInZeroing[volume.get('name')] = {
			'volume': volume,
			'lastZeroingProgressReport': None,
			'fullyZeroedSegments': set()
		}

	def handleVolumesZeroing(self):
		TWO_SECONDS = 2
		if not Component.isTimeForNextMsg(self.lastVolumeZeroingIteration, TWO_SECONDS):
			return

		def updateSegmentZeroBlocks(segment, pRaidsToUpdate, fullyZeroedSegments):
			maxBlocks = segment['lbe'] - segment['lbs'] + 1
			rate = self.volumeZeroingSettings.get('blocksPerSecond') * TWO_SECONDS
			zeroedBlocks = segment.get('nZeroedBlks', 0) + rate
			segment['nZeroedBlks'] = min(maxBlocks, zeroedBlocks)
			isFullyZeroed = segment['nZeroedBlks'] == maxBlocks

			if isFullyZeroed:
				self.updateSegmentStatus(pRaidID=pRaid['uuid'],
										 segmentID=segment['uuid'],
										 status=DiskSegmentStatuses.DEPRECATED,
										 vitality=DiskSegmentVitalities.DOWN)

				pRaidUUID = pRaid.get('uuid')
				pRaidsToUpdate[pRaidUUID] = self.pRaids[pRaidUUID]
				fullyZeroedSegments.add(segment['uuid'])
				self.logger.debug("segment fully zeroed segment %s in volume %s" % (segment.get('uuid'), volume.get('name')))

		for zeroingVolume in list(self.volumesInZeroing.values()):
			pRaidsToUpdate = {}
			volume = zeroingVolume.get('volume')
			volumeName = volume.get('name')

			for segment, pRaid, chunk in self.iterateVolumeSegments(volume):
				updateSegmentZeroBlocks(segment, pRaidsToUpdate, zeroingVolume['fullyZeroedSegments'])

			if Component.isTimeForNextMsg(zeroingVolume.get('lastZeroingProgressReport'), PeriodicMessagesIntervals.VOLUME_ZEROING_UPDATE):
				self.sendSegmentZeroingProgressMessage(volume)
				zeroingVolume['lastZeroingProgressReport'] = datetime.datetime.now()

			# If any segments were deprecated > send updatePRaidStatus message for the relevant pRaids
			if len(pRaidsToUpdate.keys()) > 0:
				self.logger.debug("some pRaids had deprecated segments after being fully zeroed in volume %s" % volumeName)
				self.sendUpdatePRaidReportMessage(pRaidsToUpdate=pRaidsToUpdate)

				if self.isVolumeFinishedZeroing(zeroingVolume):
					self.deleteVolume(volumeName)

	def iterateVolumePRaids(self, volume):
		for chunk in volume['chunks']:
			for pRaid in chunk['pRaids']:
				yield pRaid, chunk

	def iterateVolumeSegments(self, volume):
		for pRaid, chunk in self.iterateVolumePRaids(volume):
			for segment in pRaid['diskSegments']:
				yield segment, pRaid, chunk

	def isVolumeFinishedZeroing(self, zeroingVolume):
		volume = zeroingVolume['volume']
		for segment, pRaid, chunk in self.iterateVolumeSegments(volume):
			if segment['uuid'] not in zeroingVolume['fullyZeroedSegments']:
				return False

		self.logger.debug("Volume %s finished zeroing" % volume.get('name'))

		return True

	def updateTomaToken(self, token, leader=False):
		tokenUpdated = False
		tokenName = 'tomaLeaderToken' if leader else 'tomaToken'

		if token and token > getattr(self, tokenName):
			tokenUpdated = True
			setattr(self, tokenName, token)

		return tokenUpdated

	def updateZone(self, zone):
		zoneUpdated = False

		if zone and zone != self.zone:
			zoneUpdated = True
			self.logger.debug('Updating zone from {} to {}'.format(self.zone, zone))
			self.zone = zone
			self.target['zone'] = zone

			self.logger.debug('Going to resubscribe to consumers')
			self.initConsumer(resubscribe=True)

		return zoneUpdated

	def deleteVolume(self, volumeName):
		for pRaid, chunk in self.iterateVolumePRaids(self.volumes.get(volumeName)):
			del self.pRaids[pRaid['uuid']]

		if volumeName in self.volumesInZeroing:
			del self.volumesInZeroing[volumeName]

		if volumeName in self.volumesUnderRebuild:
			del self.volumesUnderRebuild[volumeName]

		del self.volumes[volumeName]

	# Generate components methods
	def generatePraidReport(self, pRaids=None):
		report = []

		reportPRaids = (pRaids or self.pRaids)

		for pRaidUUID, pRaid in reportPRaids.items():
			pRaidReportObj = {
				'segments': [s for s in pRaid['diskSegments'].values()],
				'uuid': pRaid['uuid'],
				'isRaftLeader': 1,
				'pRaidMajorVersion': pRaid['pRaidMajorVersion'],
				'pRaidMinorVersion': pRaid['pRaidMinorVersion'],
				'raftTerm': self.raftTerm
			}

			if pRaid.get('type'):
				pRaidReportObj['type'] = pRaid.get('type')

			report.append(pRaidReportObj)

		return report

	def generateDirtyBitsReport(self, volume):
		def getRebuildingSegment(diskSegments, segmentUnderRebuildUUID):
			isOtherDataSegment = lambda segment: segment['type'] == SegmentType.DATA and segment['uuid'] != segmentUnderRebuildUUID
			rebuildingSegment = list(filter(isOtherDataSegment, diskSegments))[-1]
			return rebuildingSegment

		volPraid = volume['pRaid']
		pRaidUUID = volPraid['uuid']
		rebuildingSegment = getRebuildingSegment(volPraid['diskSegments'], volume['segmentUnderRebuildUUID'])
		report = {
			'pRaidMajorVersion': self.pRaids[pRaidUUID]['pRaidMajorVersion'],
			'pRaidMinorVersion': self.pRaids[pRaidUUID]['pRaidMinorVersion'],
			'pRaidUUID': self.pRaids[pRaidUUID]['uuid'],
			'segmentID': rebuildingSegment['uuid'],
			'remainingDirtyBits': volume['amountOfDBsToClean'],
			'reappearingCounter': 1000
		}

		return report

	def generateDisk(self, diskID):
		return {
			"Completion_Queues": 0,
			"writeCounter": 19045944,
			"Available_Spare": "100_%",
			"isExcluded": False,
			"MSIX_Interrupts": 0,
			"pci_address": "",
			"Percentage_Used": "0_%",
			"block_size": 4096,
			"Controller_Busy_Time": "0x0",
			"Vendor": "0x8086",
			"reappearingCounter": 0,
			"metadata_size": 8,
			"Numa_Node": 0,

			"activeFormatRequestCounter": 0,
			"Number_of_Error_Information_Log_Entries": "0x0",
			"MetadataCapabilities": MetadataCapabilities.BOTH,
			"Serial_Number": diskID,
			"Unsafe_Shutdowns": "0x1",
			"diskID": diskID,
			"status": DiskStatus.NOT_INITIALIZED,
			"blocks": self.diskBlocks,
			"Media_Errors": "0x0",
			"Power_Cycles": "0x1b4",
			"Available_Spare_Threshold": "10_%",
			"formatRequestCounter": 0,
			"Model": "INTEL SSDPE2ME400G4_____________________",
			"Submission_Queues": 31,
			"Power_On_Hours": "0x70f8",
			"Critical_Warning": "0x0",
			"formatOptions": [
				{"metaBS": 0, "dataBS": 512}, {"metaBS": 8, "dataBS": 512}, {"metaBS": 16, "dataBS": 512}, {"metaBS": 0, "dataBS": 4096},
				{"metaBS": 8, "dataBS": 4096}, {"metaBS": 64, "dataBS": 4096}, {"metaBS": 128, "dataBS": 4096}
			]
		}

	def generateInitialGPT(self, diskUUID, mgmtDBUUID, formatType):
		gpt = {
			"diskGuid": diskUUID,
			"isValid": 1,
			"firstUsableLba": 512,
			"lastUsableLba": self.diskBlocks - 512,
			"mgmtDbUuid": mgmtDBUUID,
			"maxNGptEntries": 8192,
			"entries": [
				{
					"partitionGuid": str(uuid.uuid4()),
					"partitionType": "excelero_metadata",
					"partitionName": "excelero_metadata",
					"owner": "nvmesh",
					"start": 512,
					"end": 977151,
					"mgmtDbUuid": "00000000-0000-0000-0000-000000000000",
					"isZeroed": False
				}
			]
		}

		ecMetadataEntries = [
			{
				"partitionGuid": str(uuid.uuid4()),
				"partitionType": "excelero_metadata",
				"partitionName": "excelero_journal_data",
				"owner": "nvmesh",
				"start": 977152,
				"end": 1501439,
				"mgmtDbUuid": "00000000-0000-0000-0000-000000000000",
				"isZeroed": False
			},
			{
				"partitionGuid": str(uuid.uuid4()),
				"partitionType": "excelero_metadata",
				"partitionName": "excelero_serjio_db",
				"owner": "nvmesh",
				"start": 1501440,
				"end": 1509631,
				"mgmtDbUuid": "00000000-0000-0000-0000-000000000000",
				"isZeroed": False
			}
		]

		if formatType == FormatType.FORMAT_EC:
			gpt['entries'] = gpt['entries'] + ecMetadataEntries

		return gpt

	def generateNic(self, id):
		return {
			"status": 1,
			"pkey": "0xffff",
			"deviceType": "mlx4_0",
			"nicID": id,
			"pci_root": 0,
			"protocol": 0,
			"guid": id,
			"mtu": 1024
		}

	def generateTarget(self, nicIds, diskIds):
		return {
			'disks': [self.generateDisk(diskId) for diskId in diskIds],
			'nics': [self.generateNic(uuid) for uuid in nicIds],
			'node_status': self.healthStatus,
			'version': self.version,
			'branch': self.branch,
			'commit': self.commit
		}

	# States and controls
	@staticmethod
	def parseArguments():
		parser = argparse.ArgumentParser(description='NVMesh Scale Simulator Manager')
		subparsers = parser.add_subparsers(dest="command")
		subparsers.add_parser('start', help='Start The Simulator')
		subparsers.add_parser('stop', help='Stop The Simulator')
		subparsers.add_parser('restart', help='Restart The Simulator')

		args = parser.parse_args()
		return args

	def initState(self, initialState):
		self.logger.debug("initialState: {}".format(json.dumps(initialState, indent=4)))
		self.cachedInitState = initialState

		if 'hostname' in initialState:
			self.hostname = initialState['hostname']

		if 'diskBlocks' in initialState:
			self.diskBlocks = initialState['diskBlocks']

		if 'targetReport' in initialState:
			self.logger.debug("Loading persistent self.target Data from file")
			self.target = initialState['targetReport']
		else:
			self.logger.debug("No persistent data found - Generating new Target Hardware")
			nicIds = initialState['nics']
			diskIds = initialState['disks']
			self.target = self.generateTarget(nicIds, diskIds)

		self.target['configProfile'] = self.getConfigurationProfileFromPersistencies()
		self.target['version'] = self.version

		if 'pRaids' in initialState:
			self.pRaids = initialState['pRaids']

		if 'configurationVersion' in initialState:
			self.configurationVersion = initialState['configurationVersion']

		if 'configuration' in initialState:
			self.managementConfiguration = initialState['configuration']

		if 'driveZeroingInterval' in initialState:
			self.logger.debug("Updating driveZeroingInterval to {}".format(initialState['driveZeroingInterval']))
			self.driveZeroingInterval = initialState['driveZeroingInterval']

		if 'fullFormat' in initialState:
			self.performFullDriveFormat = initialState['fullFormat']

		if 'autoLeaderElection' in initialState:
			self.autoLeaderElection = initialState['autoLeaderElection']
			onOrOff = 'on' if self.autoLeaderElection else 'off'
			self.logger.debug("autoLeaderElection is {}".format(onOrOff))

		if 'isLeader' in initialState:
			self.logger.debug("isLeader is set to {}".format(initialState['isLeader']))
			self.isLeader = initialState['isLeader']

		self.autoRebuild = initialState.get('autoRebuild', True)
		self.logger.debug("autoRebuild is set to {}".format(self.autoRebuild))

		if 'rebuildSettings' in initialState:
			self.rebuildSettings.update(initialState['rebuildSettings'])

		self.logger.debug("rebuildSettings is set to {}".format(self.rebuildSettings))

		self.volumeZeroingOn = initialState.get('volumeZeroingOn', False)
		if 'volumeZeroingSettings' in initialState:
			self.volumeZeroingSettings.update(initialState['volumeZeroingSettings'])

	def handleControls(self, controls):
		self.logger.debug("Got to handleControls. controls: {}".format(controls))
		for control in controls:
			action = control['type']
			if action == 'add-disk':
				self.addDisk(control)
			elif action == 'remove-disk':
				self.removeDisk(control)
			elif action == 'add-nic':
				self.addNic(control)
			elif action == 'remove-nic':
				self.removeNic(control)
			elif action == 'step-up':
				self.leaderStepUp()
			elif action == 'step-down':
				self.leaderStepDown()
			elif action == 'rebuild-volume':
				self.performRebuildVolumeAction(control)
			elif action == 'send-praid-report':
				self.logger.debug("send Praid report control type %s in control %s" % (control['type'], control))
				self.sendUpdatePRaidReportMessage(incMinor=control.get('inc_minor'))
			elif action == 'update-segment-status':
				self.handleUpdateSegmentStatusCommand(control)
			elif action == 'set-encryption':
				self.handleSetEncryptionResultCommand(control)
			else:
				self.logger.error("Unknown control type %s in control %s" % (control['type'], control))
				return

			self.sendReportTargetMessage()

	def addDisk(self, control):
		diskID = control['id']
		self.logger.debug("addDisk {}".format(diskID))

		if self.findDisk(diskID):
			self.logger.warning("addDisk failed to add disk, diskID {} already exists".format(diskID))
			return

		disk = self.generateDisk(diskID)
		self.target['disks'].append(disk)

	def removeDisk(self, control):
		diskID = control['id']
		self.logger.debug("removing disk {}".format(diskID))
		self.target['disks'] = [disk for disk in self.target['disks'] if disk['diskID'] != diskID]

	def addNic(self, control):
		nicID = control['id']
		self.logger.debug("addNic {}".format(nicID))

		if self.findNic(nicID):
			self.logger.warning("addNic failed to add NIC, nicID {} already exists".format(nicID))
			return

		nic = self.generateNic(nicID)
		self.target['nics'].append(nic)

	def removeNic(self, control):
		nicID = control['id']
		self.logger.debug("removing NIC {}".format(nicID))
		self.target['nics'] = [nic for nic in self.target['nics'] if nic['nicID'] != nicID]

	def performRebuildVolumeAction(self, control):
		self.logger.info("Starting volume rebuild on: {0}, DBs cleaning rate is: {1} DBs/sec, chunk: {2}, pRaid  {3},"
						 "segment that will go under recovery is: {4}, DBs status treshold is {5}, amount of DBs that will be cleaned: {6}"
						 .format(control['name'], control['dbsCleanRate'], control['chunk'], control['praid'], control['sideUnderRecovery'],
								 control['dbsStatusThreshold'], control['amountOfDBsToClean']))

		volumeUnderRebuild = self.getVolumeFromConfiguration(volumeName=control['name'])

		if not volumeUnderRebuild:
			self.logger.warning("Could not find the volume: '{}' in managementConfiguration, aborting volume rebuild".format(control['name']))
		else:
			pRaid = self.getPRaidFromVolume(volume=volumeUnderRebuild, chunkNumber=control['chunk'], pRaidNumber=control['praid'])
			self.startRebuildVolume(volumeUnderRebuild, pRaid, rebuildSettings=control)

	def leaderStepUp(self):
		self.logger.info("Leader Step Up")
		self.setSelectedLeaderNodeID(self.hostname)

	def leaderStepDown(self):
		self.logger.info("Leader Step Down")
		self.setSelectedLeaderNodeID(None)

	def handleUpdateSegmentStatusCommand(self, control):
		segmentIDs = control['segment_ids']
		status = control['status']
		self.logger.debug("handleReportDeadSegmentsCommand segment IDs: %s " % segmentIDs)

		affectedPRaids = {}
		for segID in segmentIDs:
			segment, pRaid = self.getSegmentBySegmentID(segID)
			if (not pRaid) or (not segment):
				self.logger.debug("handleReportDeadSegmentsCommand Could not find segment ID: %s " % segID)
				continue

			segment['status'] = status
			affectedPRaids[pRaid['uuid']] = pRaid

		self.sendUpdatePRaidReportMessage(pRaidsToUpdate=affectedPRaids, incMinor=True)

	def handleSetEncryptionResultCommand(self, control):
		value = control['value']
		self.logger.debug("handleSetEncryptionResultCommand value: %s " % value)
		self.encryptionResult = value


if __name__ == "__main__":
	args = TOMA.parseArguments()
	toma = TOMA()
	toma.start()
