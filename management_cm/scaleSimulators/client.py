#!/usr/bin/env python3

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

import socket
import datetime
import json

from component import Component
from consts import Components, MessageOpCode, VolumeAttachmentStatus, PeriodicMessagesIntervals, Consts, ClientStatus, IntervalBetweenMessages, \
	ReservationMode, MessageTypes


class Client(Component):
	def __init__(self):
		self.defaultKeepaliveInterval = PeriodicMessagesIntervals.CLIENT_KEEP_ALIVE
		Component.__init__(self, configFile=Consts.CLIENT_CONFIG_FILE)
		self.managementConfiguration = ManagementClientConfiguration(self)
		'''self.blockDevices = {
			'93aca990-869b-11e9-9dc4-a9a01b883d6a': {
				'uuid': '93aca990-869b-11e9-9dc4-a9a01b883d6a',
				'name': 'r1',
				'version': 1,
				'ioEnabled': True,
				'is_hidden': False
			},
			'430eda80-86aa-11e9-aa45-ab9c3da795aa': {
				'uuid': '430eda80-86aa-11e9-aa45-ab9c3da795aa',
				'name': 'r2',
				'version': 1,
				'ioEnabled': True
			}
		}'''
		self.blockDevices = {}
		self.blockDevicesIOStatus = {}
		self.targetNics = {}
		self.healthStatus = ClientStatus.READY
		self.lastKeepAliveTime = None
		self.clientToken = -1
		self.sentUpdateOnStartup = False
		self.reportID = 1
		self.cachedInitState = {}
		self.messageSequence = 1
		self.configProfile = self.getConfigurationProfileFromPersistencies()
		self.configProfile.pop('userOverride')
		self.isUmClient = False
		self.hasWIPOperations = False
		self.maxAttachmentsVersion = 0
		self.readConfigFromFile(updateInitState=True)

	def getType(self):
		return Components.CLIENT

	def getInterestEvents(self):
		events = []
		return events

	def getEventForClientId(self, eventName):
		return 'clientID_' + self.hostname + '@' + eventName

	def handleDetachVolume(self, msg):
		payload = msg.get('payload')
		attachmentsVersion = payload['attachmentsVersion']
		self.maxAttachmentsVersion = max(self.maxAttachmentsVersion, attachmentsVersion)

		volumes = payload['volumes']
		volumesName = map(lambda volume: volume['name'], volumes)

		self.logger.debug('Got detach volume with attachmentsVersion: {} and volumesName: {}'.format(attachmentsVersion, volumesName))
		self.detachVolumes(volumes)

	def handleUpdateTargetNICs(self, msg):
		targets = msg['payload']['targets']

		for targetUpdate in targets:
			targetID = targetUpdate["node_id"]
			if targetID not in self.targetNics:
				self.targetNics[targetID] = { 'nicsVersion': -1, 'nics': [] }

			cachedTarget = self.targetNics[targetID]

			if targetUpdate['nicsVersion'] > cachedTarget['nicsVersion']:
				self.targetNics[targetID] = targetUpdate
			else:
				self.logger.debug('Got updateTargetNICs, but my cache is newer')

		# try to connect to volumes
		self.tryToConnectToAllIoDisabledVolumes()

	def tryToConnectToAllIoDisabledVolumes(self):
		# find all ioDisabled attachments
		ioDisabledVolumeUUIDS = []
		for bdev in self.blockDevices.values():
			if not bdev['ioEnabled'] or bdev['ioEnabled'] == 0:
				ioDisabledVolumeUUIDS.append(bdev['uuid'])

		for volumeUUID in ioDisabledVolumeUUIDS:
			# get volume configuration and try to connect
			volumeConf = self.managementConfiguration.getVolumeByUUID(volumeUUID)

			if not volumeConf:
				raise ValueError('we have ioDisabled block device but we don\'t have a volume configuration for it volumeName: {}  uuid: {}. volumes: {}'.format(
					bdev['name'], bdev['uuid'],self.managementConfiguration.volumeByUUID))
			# try to connect and if successful -> set ioEnabled
			self.connectToVolumeTargets(volumeConf, {})

	def handleAttachVolumes(self, msg):
		payload = msg.get('payload')

		newAttachmentsVersion = payload['attachmentsVersion']
		self.maxAttachmentsVersion = max(self.maxAttachmentsVersion, newAttachmentsVersion)

		volumesToAttach = []
		self.managementConfiguration.addVolumes(payload)

		for volume in payload.get('volumes', []):
			volumeName = volume.get('name')
			self.logger.debug('handleAttachVolumes processing volume {}'.format(volumeName))

			if volumeName in self.blockDevices:
				bdev = self.blockDevices[volumeName]
				lastAttachmentsVersion = bdev['attachmentsVersion']
				if newAttachmentsVersion == 0 or newAttachmentsVersion > lastAttachmentsVersion:
					bdev['version'] = volume['version']
					self.logger.debug('volume {} attachment updated to attachmentsVersion={}'.format(volumeName, newAttachmentsVersion))
					bdev['attachmentsVersion'] = newAttachmentsVersion
					self.sendUpdateAttachmentStatus(VolumeAttachmentStatus.ATTACHED, [bdev])
				elif newAttachmentsVersion < lastAttachmentsVersion:
					self.logger.debug('volume {} already with newer attachmentsVersion lastAttachmentsVersion={} newAttachmentsVersion={}'.format(
						volumeName, lastAttachmentsVersion, newAttachmentsVersion))
				else:
					# newAttachmentsVersion == lastAttachmentsVersion
					self.logger.debug('volume {} already in the same attachmentsVersion lastAttachmentsVersion={} newAttachmentsVersion={}'.format(
						volumeName, lastAttachmentsVersion, newAttachmentsVersion))
					self.sendUpdateAttachmentStatus(VolumeAttachmentStatus.ATTACHED, [bdev])
			else:
				# volume not in block devices - attach!
				volumesToAttach.append(volume)

			if len(volumesToAttach):
				self.hasWIPOperations = True
				self.readyToAttachVolumes(volumesToAttach, newAttachmentsVersion)

	def setWipToFalse(self, volumeName):
		if volumeName in self.blockDevices:
			self.blockDevices[volumeName]['isWIP'] = False

		# check if any other volume still in WIP
		anyVolumeInWIP = any([bdev['isWIP'] for bdev in self.blockDevices.values()])
		if not anyVolumeInWIP:
			self.hasWIPOperations = False

	def handleUpdateVolumes(self, msg):
		payload = msg.get('payload')

		# update configuration if we know this volume - otherwise ignore update
		self.managementConfiguration.updateExistingVolumes(payload)

	def getKeepAlivePayload(self):
		return {
			'client_status': self.healthStatus,
			'version': self.version,
			'featureCompatibilityVersion': self.getFeatureCompatibilityVersion(),
			'branch': self.branch,
			'commit': self.commit,
			'attachmentsUUIDHash': self.getAttachmentsUUIDHash(),
			'sumOfVolumeVersions': self.getSumOfVolumeVersions(),
			'attachmentsVersion': self.maxAttachmentsVersion,
			'hasWIPOperations': self.hasWIPOperations,
			'configProfile': self.configProfile
		}

	def getSumOfVolumeVersions(self):
		sum = 0
		for uuid in self.blockDevices:
			volConf = self.managementConfiguration.getVolumeByName(uuid)
			if not volConf:
				self.logger.warning("Could not find configuration for volume {}".format(uuid))
				return -1

			sum += volConf['version']

		return sum

	def getMessageSequence(self):
		return self.messageSequence

	def sendKeepAlive(self):
		self.sendMessageToMCS(MessageTypes.CLIENT_KEEPALIVE, self.getKeepAlivePayload())

	def sendMessageToMCS(self, messageType, payload):
		self.messageSequence += 1
		Component.sendMessageToMCS(self, messageType, payload)

	def getComponentSpecificMessageHeaders(self):
		return {
			'clientID': self.hostname,
			'clientToken': self.clientToken,
			'isUmClient': 1 if self.isUmClient else 0
		}

	def sendPeriodicReports(self):
		if not self.lastKeepAliveTime or (self.clientToken and Component.isTimeForNextMsg(self.lastKeepAliveTime, self.getKeepaliveInterval())):
			self.sendKeepAlive()
			self.lastKeepAliveTime = datetime.datetime.now()

		for name in list(self.blockDevicesIOStatus.keys()):
			blockDeviceIOStatus = self.blockDevicesIOStatus[name]
			if blockDeviceIOStatus['ioPermanentlyEnabled'] and Component.isTimeForNextMsg(blockDeviceIOStatus['disabledMsgTime'],
																				IntervalBetweenMessages.IO_DISABLED_TO_IO_ENABLED):
				self.setAttachmentIoEnabled(blockDevice=self.blockDevices[name], ioEnabled=True)
				del self.blockDevicesIOStatus[name]

		self.checkIfAttachmentReadyForIoEnabled()

	def getFeatureCompatibilityVersion(self):
		return 1 # TODO: Dear management team, feel free to override this value
				 # I have no idea how to extract it from our schema JSON file when this code is executed

	def getAttachmentsUUIDHash(self):
		if not self.blockDevices:
			return ''

		sortedBdevNames = sorted(self.blockDevices.keys())
		stringBeforeHash = ''
		for name in sortedBdevNames:
			bdev = self.blockDevices[name]
			value = name
			stringBeforeHash += value + ';'

		return stringBeforeHash

	def getState(self):
		return {
			'client_status': self.healthStatus,
			'block_devices': self.blockDevices.values(),
			'version': self.version,
			'clientID': self.hostname,
			'branch': self.branch,
			'commit': self.commit,
		}

	def handleUpdateKeepaliveToken(self, msg):
		payload = msg.get('payload')
		if 'clientToken' not in payload:
			self.logger.error('Got updateClientKeepaliveToken message without a clientToken. payload: {}'.format(payload))
		elif payload['clientToken'] <= self.clientToken:
			self.logger.warning('Got updateClientKeepaliveToken message with clientToken {}, my clientToken is {}. Ignoring.'.format(payload['clientToken'], self.clientToken))
		else:
			self.reportID = payload['reportID']
			self.updateKeepAliveIntervalIfNeeded(payload.get('keepaliveInterval'))
			self.clientToken = payload['clientToken']
			self.logger.debug('Updating my clientToken to {} and reportID to {}'.format(self.clientToken, self.reportID))

			if self.messageSequence < payload['messageSequence']:
				self.logger.warning('Got updateClientKeepaliveToken message with messageSequence {}, my messageSequence is {}. Updating my messageSequence.'.format(payload['messageSequence'], self.messageSequence))
				self.messageSequence = payload['messageSequence']

			self.sendKeepAlive()

	def handleMessage(self, message):
		opcode = message['opcode']
		self.logger.debug('Got message {} - msg: {}'.format(message['messageType'], message))

		opcodeToHandlingFunction = {
			MessageOpCode.ATTACH_VOLUMES: self.handleAttachVolumes,
			MessageOpCode.UPDATE_VOLUMES: self.handleUpdateVolumes,
			MessageOpCode.DETACH_VOLUMES: self.handleDetachVolume,
			MessageOpCode.UPDATE_TARGET_NICS: self.handleUpdateTargetNICs,
			MessageOpCode.UPDATE_CLIENT_KEEPALIVE_TOKEN: self.handleUpdateKeepaliveToken,
			MessageOpCode.ERROR_RESPONSE: lambda msg: self.logger.warning("Received ERROR RESPONSE from management: %s" % msg)
		}

		handleFunction = opcodeToHandlingFunction.get(opcode, None)

		if handleFunction is None:
			self.logger.warning("Handling of message with opcode %d is Not Implemented" % opcode)
		else:
			handleFunction(message)

	def sendUpdateAttachmentStatus(self, command, attachments):
		self.logger.debug("sendUpdateAttachmentStatus command=%s attachments=%s",command, attachments)

		for volume in attachments:
			self.reportID += 1
			payload = self.createUpdateAttachmentStatusPayload(command, volume)

			if command == VolumeAttachmentStatus.ATTACHED and not payload['attachments'][0]['ioEnabled'] \
				and self.blockDevicesIOStatus[volume['name']]['ioPermanentlyEnabled']:
				self.blockDevicesIOStatus[volume['name']]['disabledMsgTime'] = datetime.datetime.now()

			self.logger.debug('Sending UpdateAttachmentStatus message %s', payload)
			self.sendMessageToMCS(MessageTypes.UPDATE_ATTACHMENT_STATUS, payload)

	def sendUpdateOnAllVolumes(self):
		for bdev in list(self.blockDevices.values()):
			self.sendUpdateAttachmentStatus(VolumeAttachmentStatus.ATTACHED, bdev)

	def createUpdateAttachmentStatusPayload(self, command, volume):
		return {
				'reportID': self.reportID,
				'client_status': self.healthStatus,
				'attachmentsUUIDHash': self.getAttachmentsUUIDHash(),
				'attachments': [
					{
						'uuid': volume['uuid'],
						'vol_status': command,
						'version': volume['version'],
						'ioEnabled': volume.get('ioEnabled'),
						'name': volume['name'],
						'is_hidden': volume.get('is_hidden'),
						'reservation': volume.get('reservation')
					}
				]
			}

	def readyToAttachVolumes(self, volumes, attachmentsVersion):
		self.logger.debug("readyToAttachVolumes %s", volumes)

		# volume is initially ioEnabled=false until recieved targetNICs and connected to targets
		newAttachments = self.addAttachments(volumes, attachmentsVersion)
		self.sendUpdateAttachmentStatus(VolumeAttachmentStatus.ATTACHED, newAttachments)

		askForTargets = {}
		for v in volumes:
			self.connectToVolumeTargets(v, askForTargets)

		if len(askForTargets.keys()) == 0:
			self.logger.debug("No need to send GetTargetNICs, I was abel to ocnnect to all volumes using cached NICs")

		self.sendGetTargetNICs(askForTargets)

	def connectToVolumeTargets(self, volume, askForTargets):
		self.logger.debug('connectToVolumeTargets: volume {}'.format(volume.get('name')))

		allConnected = True
		for chunk in volume['chunks']:
			for praid in chunk['pRaids']:
				for segment in praid['diskSegments']:
					targetID = segment.get('node_id')
					targetUUID = segment.get('nodeUUID')
					connected = self.connectToTarget(targetID, targetUUID, askForTargets)
					if not connected:
						allConnected = False

		self.logger.debug('connectToVolumeTargets: allConnected={}'.format(allConnected))

		if allConnected:
			bd = self.getBlockDevice(name=volume['name'])
			ioEnabledDelay = self.cachedInitState.get('ioEnabledDelayMS',0)
			bd['ioEnableReadyTime'] = datetime.datetime.now() + datetime.timedelta(milliseconds=ioEnabledDelay)

	def checkIfAttachmentReadyForIoEnabled(self):
		now = datetime.datetime.now()
		for blockDevice in self.blockDevices.values():
			if not blockDevice['ioEnabled']:
				ioEnabledTime = blockDevice.get('ioEnableReadyTime')
				if not ioEnabledTime:
					# volume has io disabled - not connected to target yet
					continue
				if ioEnabledTime <= now:
					self.setAttachmentIoEnabled(blockDevice, True)
					self.setWipToFalse(blockDevice['name'])
				else:
					delta = ioEnabledTime - now
					self.logger.debug('volume {} will be ioEnabled in {} seconds'.format(blockDevice['name'], delta.seconds))

	def connectToTarget(self, targetID, targetUUID, askForTargets):
		'''
		simulates trying to connect to the target
		if target not reachable in any of it's nics - adds the target to the askForTargets dict
		'''

		if targetID not in self.targetNics:
			askForTargets[targetID] = {
				"node_id": targetID,
				"nodeUUID": targetUUID,
				"nicsVersion": -1
				}
			return

		target = self.targetNics[targetID]
		nics = target.get('nics', [])
		for nic in nics:
			# we can later add a field to simulate that a NIC is unreachable
			# for now we just assume we can connect to any NIC that we know of
			return True

		# target is known buit we still couldn't connect
		askForTargets[targetID] = { "nicsVersion": target.get('nicsVersion') }

		# we couldn't connect to the target
		if len(nics):
			self.logger.debug('Target {} has no nics. nicsVersion: {}', targetID, target.get('nicsVersion'))
		else:
			self.logger.debug('Could not connect to Target {}. targetNics: {}', targetID, target)

		return False

	def sendGetTargetNICs(self, targetsDict):
		# convert target dict to list
		targets = list(targetsDict.values())
		self.logger.debug('sendGetTargetNICs: targets {}'.format(targets))
		payload = { "targets": targets }
		self.sendMessageToMCS(MessageTypes.GET_TARGET_NICS, payload)

	def addAttachment(self, attachment):
		vol_name = attachment['name']
		self.logger.info('Attaching %s' % vol_name)
		self.blockDevices[vol_name] = attachment
		self.blockDevicesIOStatus[vol_name] = {
			'ioPermanentlyEnabled': False,
			'disabledMsgTime': datetime.datetime.max
		}

	def getNewAttachment(self, volume, attachmentsVersion):
		return {
			'uuid': volume.get('uuid'),
			'name': volume.get('name'),
			'ioEnabled': False,
			'is_hidden': False,
			'reservation': volume['reservation'],
			'version': volume.get('version', 0),
			'attachmentsVersion': self.maxAttachmentsVersion,
			'volumeType': volume.get('type'),
			'isWIP': True
		}

	def addAttachments(self, volumes, attachmentsVersion):
		newlyAttached = []

		for volume in volumes:
			vol_name = volume['name']
			if not vol_name in self.blockDevices:
				attachment = self.getNewAttachment(volume, attachmentsVersion)
				self.addAttachment(attachment)
				newlyAttached.append(attachment)
			# Update the volume version in any case
			newVersion = volume.get('version')
			if newVersion:
				self.blockDevices[vol_name]['version'] = volume['version']
			else:
				self.logger.warning("missing 'version' in attachment %s" % vol_name)

		return newlyAttached

	def detachVolume(self, volume, detachedVolumes):
		volumeName = volume['name']
		blockDevice = self.getBlockDevice(name=volumeName)
		if blockDevice:
			self.logger.info("detaching %s " % volumeName)
			blockDevice['reservation']['mode'] = ReservationMode.NONE
			del self.blockDevices[blockDevice['name']]
			detachedVolumes.append(blockDevice)
		else:
			self.logger.warning("Could not find attachment named %s in blockDevices %s" % (volumeName, self.blockDevices))
			reportDetached = {
				'name': volumeName,
				'uuid': volume['uuid'],
				'version': 0
			}

			detachedVolumes.append(reportDetached)

	def detachVolumes(self, volumes):
		detachedVolumes = []

		for volume in volumes:
			self.detachVolume(volume, detachedVolumes)

		if detachedVolumes:
			self.sendUpdateAttachmentStatus(VolumeAttachmentStatus.DETACHED, detachedVolumes)

	def getBlockDevice(self, uuid=None, name=None):
		if name:
			return self.blockDevices.get(name)
		elif uuid:
			for name in self.blockDevices:
				if self.blockDevices[name]['uuid'] == uuid:
					return self.blockDevices[name]

		return None

	def handleSetIoEnabledControl(self, job):
		bd = self.getBlockDevice(name=job['name'])
		if bd:
			self.setAttachmentIoEnabled(bd, job['ioEnabled'])
		else:
			self.logger.warning("Could not find attachment named %s in blockDevices %s" % (job['name'], self.blockDevices))

	def initState(self, initialState):
		self.logger.debug("initState %s" % json.dumps(initialState, indent=4))
		self.cachedInitState = initialState
		self.isUmClient = initialState.get('isUmClient', False)

	def requestConfigurationForVolumes(self, volumes):
		for volume in volumes:
			payload = { 'volumes': [volume] }
			self.sendMessageToMCS(MessageTypes.GET_CONFIGURATION, payload)

	def setAttachmentIoEnabled(self, blockDevice, ioEnabled):
		self.logger.debug("setAttachmentIoEnabled. blockDevice %s %s" % (blockDevice['name'], blockDevice))
		blockDevice['ioEnabled'] = ioEnabled
		blockDevice['vol_status'] = VolumeAttachmentStatus.ATTACHED
		blockDevice['version'] = blockDevice.get('version', 0) + 1
		self.logger.debug("sending sendUpdateAttachmentStatus")

		self.blockDevicesIOStatus[blockDevice['name']] = {
			'ioPermanentlyEnabled': ioEnabled,
			'disabledMsgTime': datetime.datetime.max
		}

		self.sendUpdateAttachmentStatus(VolumeAttachmentStatus.ATTACHED, [blockDevice])

	def updatePersistentData(self, updateObject):
		self.cachedInitState.update(updateObject)
		self.updateInitState(self.cachedInitState)

class ManagementClientConfiguration(object):
	def __init__(self, client):
		self.config = None
		self.volumeByName = {}
		self.volumeByUUID = {}
		self.logger = client.logger
		self.client = client

	def addVolumes(self, msgPayload):
		volumesToKeep = {}
		for name, device in self.client.blockDevices.items():
			volumesToKeep[name] = self.volumeByName.get(name)

		self.config = msgPayload

		self.volumeByName = {}
		self.volumeByUUID = {}

		for v in msgPayload['volumes']:
			self.volumeByName[v['name']] = v
			self.volumeByUUID[v['uuid']] = v

		for name, v in volumesToKeep.items():
			if name not in self.volumeByName:
				self.config['volumes'].append(v)
				self.volumeByName[v['name']] = v
				self.volumeByUUID[v['uuid']] = v

	def updateExistingVolumes(self, msgPayload):
		for v in msgPayload['volumes']:
			uuid = v.get('uuid')
			if not uuid:
				self.logger.error('Failed to process configuration change message. volume has no uuid / name: %s. full message: %s ' % (v, msgPayload))
				return

			volToUpdate = self.getVolumeByUUID(uuid)

			if not volToUpdate:
				self.logger.error('Failed to process configuration change message. Could not find volume %s in configuration volumes (%s)' % (uuid, self.volumeByName.keys()))
				return

			# update all fields
			for key in v:
				volToUpdate[key] = v[key]

	def getVolumeByName(self, name):
		return self.volumeByName.get(name)

	def getVolumeByUUID(self, uuid):
		return self.volumeByUUID.get(uuid)

if __name__ == "__main__":
	client = Client()
	client.start()
