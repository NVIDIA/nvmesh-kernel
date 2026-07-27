#!/usr/bin/env python3

import os
import sys
import json
import socket
import requests
import datetime

from component import Component
from consts import Consts, MessageTypes, Components, NvmeshUMStatus, PeriodicMessagesIntervals, SPDKCommands, VolumeAttachmentStatus

# Disable warning
from requests.packages.urllib3.exceptions import InsecureRequestWarning
requests.packages.urllib3.disable_warnings(InsecureRequestWarning)

class Agent(Component):
	def __init__(self):
		self.defaultKeepaliveInterval = PeriodicMessagesIntervals.AGENT_KEEP_ALIVE
		Component.__init__(self, configFile=Consts.AGENT_CONFIG_FILE)
		self.messageSequence = -1
		self.mgmtAgentToken = -1

		self.snapshotsStatus = []
		self.nvmeshUMStatus = NvmeshUMStatus.UP
		self.isStatsSamplingEnabled = False
		self.featureCompatibilityVersion = 1 # must match client's reported version
		self.readConfigFromFile(updateInitState=True)
		self.managementAPIServers = self.getManagementAPIServers()


	# AGENT -> MGMT
	def sendKeepaliveMessage(self):
		payload = {
			'nvmeshumStatus': self.nvmeshUMStatus,
			'snapshotsStatus': self.snapshotsStatus,
			'isStatsSamplingEnabled': self.isStatsSamplingEnabled,
			'configProfileInfo': self.configurationProfile,
			'featureCompatibilityVersion': self.featureCompatibilityVersion
		}

		self.logger.debug('Going to send keepalive message with token {}'.format(self.mgmtAgentToken))
		self.sendMessageToMCS(MessageTypes.AGENT_KEEPALIVE, payload)

		self.lastKeepAliveTime = datetime.datetime.now()

	def sendConfigProfileUpdatedMessage(self):
		payload = { 'configProfileInfo': self.configurationProfile, 'nodeID': self.hostname }

		self.logger.debug('Going to send config profile updated message with configProfileInfo {}'.format(self.configurationProfile))
		self.sendMessageToMCS(MessageTypes.CONFIG_PROFILE_UPDATED, payload)

	def sendUpdateConfigProfileUserOverrideMessage(self):
		configProfileInfo = self.getConfigurationProfileFromPersistencies()
		payload = { 'configProfileInfo': configProfileInfo, 'nodeID': self.hostname }

		self.logger.debug('Going to send update config profile user override message with configProfileInfo {}'.format(configProfileInfo))
		self.sendMessageToMCS(MessageTypes.UPDATE_CONFIG_PROFILE_USER_OVERRIDE, payload)

	def sendSpdkSnapshotCommandResultMessage(self, payload):
		self.logger.debug('Going to send spdk snapshot command result message with snapshotID {} requestUUID {} success {}'
			.format(payload.get('snapshotID'), payload.get('requestUUID'), payload.get('success')))
		self.sendMessageToMCS(MessageTypes.SPDK_SNAPSHOT_COMMAND_RESULT, payload)

	def sendUpdateKeysMessage(self):
		payload = { 'keys': self.getKeys() }

		self.logger.debug('Going to send update keys message with keys {}'.format(map(lambda k: k.get('name'), payload.get('keys'))))
		self.sendMessageToMCS(MessageTypes.UPDATE_KEYS, payload)


	# MGMT -> AGENT
	def handleMessage(self, message):
		messageType = message.get('messageType')
		payload = message.get('payload')

		if messageType == MessageTypes.UPDATE_AGENT_TOKEN:
			self.handleUpdateAgentTokenMessage(payload)
		elif messageType == MessageTypes.UPDATE_CONFIG_PROFILE:
			self.handleUpdateConfigProfileMessage(payload)
		elif messageType == MessageTypes.SPDK_SNAPSHOT_COMMAND:
			self.handleSpdkSnapshotCommandMessage(payload)
		else:
			self.logger.warning('Unable to handle message from with messageType {}. Ignoring this message...'.format(messageType))

	def handleUpdateAgentTokenMessage(self, payload):
		token = payload.get('token')
		messageSequence = payload.get('messageSequence')
		keepaliveInterval = payload.get('keepaliveInterval')
		self.logger.debug('Got new update agent token message with keepaliveInterval: {} token {} and messageSequence {}'.format(keepaliveInterval, token, messageSequence))

		self.updateKeepAliveIntervalIfNeeded(keepaliveInterval)

		isNewToken = self.mgmtAgentToken != token

		self.mgmtAgentToken = token
		self.messageSequence = messageSequence

		self.sendKeepaliveMessage()

		if isNewToken:
			self.logger.debug('Received a new mgmtAgentToken!')
			self.sendUpdateKeysMessage()
			self.sendUpdateConfigProfileUserOverrideMessage()

	def handleUpdateConfigProfileMessage(self, payload):
		configProfileName = payload.get('name')
		configProfileParameters = payload.get('parameters')
		self.logger.debug('Got new update config profile message with name {} and parameters {}'.format(configProfileName, configProfileParameters))

		isUpdated = self.updateConfigurationProfile(configProfileParameters)

		if isUpdated:
			self.sendConfigProfileUpdatedMessage()

	def handleSpdkSnapshotCommandMessage(self, payload):
		result = None
		command = payload.get('command')
		snapshotID = payload.get('snapshotID')
		requestUUID = payload.get('requestUUID')
		self.logger.debug('Got new snapshot command message with command {} snapshotID {} and requestUUID {}'.format(command, snapshotID, requestUUID))

		if command == SPDKCommands.ATTACH_SNAPSHOT:
			result = self.handleSpdkAttachCommand(payload)
		elif command == SPDKCommands.DETACH_SNAPSHOT:
			result = self.handleSpdkDetachCommand(payload)
		else:
			self.logger.error('Unknown SPDK command {}'.format(command))

		if result:
			self.sendSpdkSnapshotCommandResultMessage(result)


	# utils
	def getType(self):
		return Components.MANAGEMENT_AGENT

	def getMessageSequence(self):
		return self.messageSequence

	def getComponentSpecificMessageHeaders(self):
		return {
			'clientID': self.hostname,
			'mgmtAgentToken': self.mgmtAgentToken,
		}

	def sendMessageToMCS(self, messageType, payload):
		Component.sendMessageToMCS(self, messageType, payload)
		self.messageSequence += 1

	def resolveNvmeshUMStatus(self, status):
		if not status in [NvmeshUMStatus.UP, NvmeshUMStatus.DOWN, NvmeshUMStatus.DISABLED]:
			self.logger.error('Unable to resolve nvmeshUMStatus! Using default status: {} status found: {}'.format(NvmeshUMStatus.UP, status))
			status = NvmeshUMStatus.UP

		return status

	def initState(self, initialState):
			self.logger.debug('initState called')
			self.cachedInitState = initialState

			if 'nvmeshUMStatus' in initialState:
				self.nvmeshUMStatus = self.resolveNvmeshUMStatus(initialState['nvmeshUMStatus'])

	def afterSocketInitiated(self):
		self.sendKeepaliveMessage()

	def sendPeriodicReports(self):
		if not self.lastKeepAliveTime or (Component.isTimeForNextMsg(self.lastKeepAliveTime, self.getKeepaliveInterval())):
			self.sendKeepaliveMessage()

	def extractResultInformationsFromSpdkCommandPayload(self, payload):
		command = payload.get('command')
		snapshotID = payload.get('snapshotID')
		snapshotUUID = payload.get('snapshotUUID')
		requestUUID = payload.get('requestUUID')

		return command, snapshotID, snapshotUUID, requestUUID

	def areVolumeIDsAttached(self, volumeIDs=[]):
		cookies = self.loginToManagement()
		getClientResult = self.getClient(projection={ "block_devices": 1 }, cookies=cookies)

		if getClientResult:
			for blockDevice in getClientResult.get('block_devices', []):
				if blockDevice.get('name') in volumeIDs:
					if blockDevice.get('vol_status') == VolumeAttachmentStatus.ATTACHED:
						volumeIDs.remove(blockDevice.get('name'))

		return len(volumeIDs) == 0

	def generateSpdkSnapshotCommandResult(self, payload):
		command, snapshotID, snapshotUUID, requestUUID = self.extractResultInformationsFromSpdkCommandPayload(payload)

		return {
			'success': True,
			'error': '',
			'command': command,
			'snapshotID': snapshotID,
			'snapshotUUID': snapshotUUID,
			'requestUUID': requestUUID
		}

	def handleSpdkAttachCommand(self, payload):
		result = self.generateSpdkSnapshotCommandResult(payload)

		areVolumesAttached = self.areVolumeIDsAttached([payload.get('sourceID'), payload.get('metadataVolumeID')])

		if not areVolumesAttached:
			result['success'] = False
			result['error'] = 'not all volumes are attached'

		return result

	def handleSpdkDetachCommand(self, payload):
		result = self.generateSpdkSnapshotCommandResult(payload)

		# TODO: add test to validate if result should be success or not

		return result

	def getKeys(self):
		keys = []

		for (path, dirs, fnames) in os.walk(Consts.KEYS_DIR):
			for file in fnames:
				if not os.path.basename(file).startswith('.'):
					try:
						filePath = os.path.join(path, file)
						with open(filePath, 'r') as fileFD:
							key = json.loads(fileFD.read())
							keys.append(key)
					except Exception as e:
						self.logger.error('Failed to load key {}, ex: {}'.format(file, e))

		return keys

	def getManagementAPIServers(self):
		apiServers = None

		filepath = '/etc/nvmesh/nvmesh.conf'
		config = self.readBashFile(filepath)

		if not 'MANAGEMENT_SERVERS' in config:
			self.logger.error('Failed to find MANAGEMENT_SERVERS in nvmesh.conf, exiting')
			sys.exit(1)

		try:
			apiServers = map(lambda x: x.split(':')[0], config['MANAGEMENT_SERVERS'].split(','))
		except Exception as e:
			self.logger.error('Failed to parse MANAGEMENT_SERVERS in nvmesh.conf, found: {}, ex: {} , exiting'.format(config['MANAGEMENT_SERVERS'], e))
			sys.exit(1)

		self.logger.debug('Management API servers: {}'.format(apiServers))
		return apiServers

	def loginToManagement(self):
		apiServer = self.managementAPIServers[0]
		result = requests.post('https://{}:4000/login'.format(apiServer), json={'username': 'admin', 'password': 'admin'}, verify=False)

		if result.status_code != 200:
			self.logger.error('Failed to login to {}'.format(apiServer))

		else:
			self.managementCookies = result.cookies

		return self.managementCookies

	def getClient(self, projection={}, cookies={}):
		client = None
		apiServer = self.managementAPIServers[0]
		mongoFilter = { "_id": "{}".format(self.hostname) }
		url = 'https://{}:4000/clients/all/0/0?filter={}&projection={}'.format(apiServer, json.dumps(mongoFilter), json.dumps(projection))
		result = requests.get(url, cookies=cookies, verify=False)

		if result.status_code != 200:
			self.logger.error('Failed to get client with request {}'.format(url))

		else:
			try:
				client = json.loads(result.content)[0]
			except Exception as e:
				self.logger.error('Failed to parse get client result with request {}, error {}'.format(url, e))

		return client

if __name__ == "__main__":
	agent = Agent()
	agent.start()
