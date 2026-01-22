#!python3

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

import queue
import struct
import socket, errno
import json
import os
import sys
import array
from datetime import datetime
import uuid
from clnt import process_scheme
import io
import fcntl
import random
import marshal
import time
import subprocess
import shutil
import copy
import collections
from typing import List

from Mailbox import Message, MessagePriorities, MessageTypes, LoginMessage, SocketDisconnectionMessage, BinaryMessage, GenericMessage, EmulationModes

def runCommand(command, logger, runInBackground=False):
	try:
		if runInBackground:
			process = subprocess.Popen(command)
			return process, command
		else:
			process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
			output, _ = process.communicate()

		msg = "exit code: {0} output: {1}".format(process.returncode, output)
		logger.debug(msg)

		if process.returncode != 0:
			return False, None
	except OSError as exception:
		logger.exception('Subprocess failed, ex: {}'.format(exception))
		return False, None
	else:
		logger.debug('Subprocess finished')

	return True, output

def readBashFile(filename):
	g = {}
	l = {}

	if os.path.exists(filename):
		exec(compile(open(filename, "rb").read(), filename, 'exec'), g, l)

	return l

def parseKafkaServersStr(serversStr):
	# parse both IPv4 and IPv6 addresses
	def parseSpecificServer(serverPort):
		lastColonIdx = serverPort.rfind(':')
		return [serverPort[:lastColonIdx], serverPort[lastColonIdx + 1:]]

	addresses = list(map(parseSpecificServer, serversStr.split(',')))
	faultyAddresses = [serverPortTuple for serverPortTuple in addresses if len(serverPortTuple) != 2]

	if faultyAddresses:
		raise ValueError(
			'Invalid value of "{}" for KAFKA_SERVERS in /etc/nvmesh/nvmesh.conf - '.format(serversStr)
			+ 'server addresses "{}" should be in the format "ip:port" or "hostname:port"'.format(faultyAddresses))

	return addresses

def getKafkaAddresses(config):
	kafkaAddresses = None

	if not config:
		filepath = '/etc/nvmesh/nvmesh.conf'
		config = readBashFile(filepath)

	if 'KAFKA_SERVERS' in config:
		kafkaAddresses = parseKafkaServersStr(serversStr=config["KAFKA_SERVERS"])

	return kafkaAddresses


class CMSocket:
	REPORT_NUMPY_MISSING = True
	CONFIGURATION_FNAME = "CONFIGURATION"

	def __init__(self, cmSocket, readList, writeList, logger, concurrentConnections=None, udsPath=None,
				 maxConcurrentConnections=5, lengthSize=4, addToReadList=True, originType=None):
		self.logger = logger

		def logVerbose(self, msg, *args, **kwargs):
			self.logger.debug(msg)

		# support for logger without verbose function
		if not hasattr(self.logger, 'verbose'):
			setattr(self.logger, 'verbose', logVerbose)

		self.originType = originType
		self.readList = readList

		if addToReadList:
			self.readList.append(self)

		self.writeList = writeList
		self.concurrentConnections = concurrentConnections
		self.id = "CLIENT" if originType == "CLIENT" else str(uuid.uuid1())
		self.cmSocket = cmSocket
		self.isAlive = True

		self.setOptions()

		if udsPath:
			self.cmSocket.bind(udsPath)
			self.cmSocket.listen(maxConcurrentConnections)

		self.expectedLength = lengthSize
		self.messageLength = None
		self.lengthSize = lengthSize
		self.bytesReceived = []
		self.pendingSend = None
		self.outbox = {}
		self.outboxOrder = queue.Queue()

	def setOptions(self):
		self.cmSocket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
		self.cmSocket.setblocking(0)

	def fileno(self):
		return self.cmSocket.fileno()

	def accept(self):
		return self.cmSocket.accept()

	def connect(self, host, port):
		self.cmSocket.settimeout(1)
		connectResult = self.cmSocket.connect((host, port))

		return connectResult

	def addMessageType(self, socketOutbox, message):
		if message.type not in socketOutbox:
			socketOutbox[message.type] = queue.Queue()
		else:
			if not self.isAlive and message.priority == MessagePriorities.MEDIUM:
				# YR: Is this alternative better? socketOutbox[message.type] = Queue.Queue() - will garbage collection handle it?
				while not socketOutbox[message.type].empty():
					socketOutbox[message.type].get()

		if message.priority == MessagePriorities.MEDIUM:
			qlen = socketOutbox[message.type].qsize()
			# YR: Trying not to queue too many of these - not sure it is needed.
			if qlen > 100 and random.random() > 1.0 / (qlen - 99):
				socketOutbox[message.type].get()

		socketOutbox[message.type].put(message)

	def getNumberOfMessagesInOutbox(self):
		sum = 0

		for socket in self.outbox:
			for type in self.outbox[socket]:
				sum += self.outbox[socket][type].qsize()

		return sum

	def clearAllAndCloseSockets(self):
		socketsToClose = []
		sockets = list(self.outbox.keys())

		for socket in sockets:
			for type in self.outbox[socket]:
				while self.outbox[socket][type].qsize():
					message = self.outbox[socket][type].get()

					if message.senderSocket not in socketsToClose:
						socketsToClose.append(message.senderSocket)

			del self.outbox[socket]

		for socket in socketsToClose:
			self.logger.debug("Closing socket: %s", socket.id)

			try:
				socket.close(sendDisconnectMessageOnClose=False)
			except Exception as e:
				self.logger.exception("Failed to close some socket! The socket is %s", socket)

	def addMessageToOutbox(self, message):
		sum = self.getNumberOfMessagesInOutbox()
		if sum > 5000:
			self.logger.warning(
				"Pending messages threshold achieved, clearing outbox and closing all sockets. pending messages: %s", sum)
			self.clearAllAndCloseSockets()
		if not message.senderSocket.id in self.outbox:
			self.outbox[message.senderSocket.id] = {}

		socketOutbox = self.outbox[message.senderSocket.id]

		if self.isAlive or message.priority in (MessagePriorities.HIGH, MessagePriorities.MEDIUM):
			self.outboxOrder.put((message.senderSocket.id, message.type))
			self.addMessageType(socketOutbox, message)

	def getMessageFromOutbox(self):
		message = None
		sum = self.getNumberOfMessagesInOutbox()

		while not message and self.outboxOrder.qsize():
			socketID, messageType = self.outboxOrder.get()

			if socketID in self.outbox and messageType in self.outbox[socketID]:
				message = self.outbox[socketID][messageType].get()

				if self.outbox[socketID][messageType].empty():
					del self.outbox[socketID][messageType]

					if not len(self.outbox[socketID]):
						del self.outbox[socketID]

		return message

	def sendMessages(self, messages, writeList=None):
		for message in messages[1:]:
			self.addMessageToOutbox(message)

		self.sendMessage(messages[0], writeList, True, False)

	def sendMessage(self, message, writeList=None, sendNextMessage=True, isLoginMessage=False):
		if isLoginMessage and not self.isAlive:
			return False

		if self.pendingSend or not self.isAlive:
			self.addMessageToOutbox(message)
			return False
		else:
			return CMSocket.send(self, self.getPayload(message.payload), writeList, sendNextMessage)

	def getPayload(self, message):
		return message['payload']

	def send(self, payload=None, writeList=None, sendNextMessage=True, completion=None):
		if self.pendingSend:
			data = self.pendingSend[0][self.pendingSend[1]:]
		else:
			data = payload
			self.pendingSend = (payload, 0)

		if data and len(data):
			if type(writeList) is list and self not in writeList:
				writeList.append(self)

			bytesSent = self.sendFunction(data)

			if type(writeList) is list and self in writeList:
				writeList.remove(self)
		else:
			raise Exception('Go to send but nothing the payload or pendingSend is empty! pendingSend: {}, payload: {}'
							.format(self.pendingSend, len(payload)))

		if bytesSent < len(data):
			self.pendingSend = (self.pendingSend[0], self.pendingSend[1] + bytesSent)

			if type(writeList) is list and self not in writeList:
				self.logger.debug("Going to add myself to the writeList bytesSent: {}, from: {}".format(self.pendingSend[1], len(self.pendingSend[0])))
				writeList.append(self)

			return False
		else:
			self.pendingSend = None

			if type(writeList) is list and self in writeList:
				writeList.remove(self)

			if sendNextMessage:
				nextMessage = self.getMessageFromOutbox()

				while nextMessage:
					if self.sendMessage(nextMessage, writeList=writeList, sendNextMessage=False):
						nextMessage = self.getMessageFromOutbox()
					else:
						nextMessage = None

			return True

	def clear(self):
		self.bytesReceived = []
		self.messageLength = None

	def receiveFunction(self, size):
		return self.cmSocket.recv(size)

	def sendFunction(self, data):
		return self.cmSocket.send(data)

	def receive(self, messageLength=None):
		if messageLength:
			self.messageLength = messageLength
		if not self.messageLength and len(self.bytesReceived) == 0:
			self.messageLength = self.receiveFunction(self.expectedLength)

			if len(self.messageLength) == 0:
				raise socket.error("socket connection broken")

			try:
				self.messageLength = struct.unpack("I", self.messageLength)[0]
			except Exception as ex:
				self.logger.exception("Failed to unpack first 4 bytes to int 32!")
				self.clear()
				return

			return CMSocket.receive(self)
		else:
			while len(self.bytesReceived) < self.messageLength:
				bytesRead = self.receiveFunction(self.messageLength - len(self.bytesReceived))

				if len(bytesRead) == 0:
					self.isAlive = False
					raise socket.error("socket connection broken")

				self.bytesReceived += bytesRead
			self.messageLength = 0
			r = self.bytesReceived
			self.bytesReceived = b""
			return r

	def close(self):
		if self in self.readList:
			del self.readList[self.readList.index(self)]

		if self in self.writeList:
			del self.writeList[self.writeList.index(self)]

		self.cmSocket.close()

	def handleSocketErrorException(self, exception):
		if exception.args[0] in [errno.EWOULDBLOCK, errno.EAGAIN]:
			self.logger.debug("Non-fatal error, continuing")
		else:
			self.logger.debug("Socket closed! %s", exception)
			self.close()

	def logMessage(self, sender, receiver, msg):
		fromTo = '{0}>{1}'.format(sender, receiver)
		opcode = msg['opcode'] if 'opcode' in msg else None
		self.logger.verbose(msg, verboseType="message", fromTo=fromTo, opcode=opcode)

class SocketToManagement(CMSocket):
	def __init__(self, cmSocket, readList, writeList, concurrentConnections, kafkaOutbox, logger, type,
				 lengthSize=4, originType="Unknown"):
		CMSocket.__init__(self, cmSocket, readList, writeList, logger, concurrentConnections, lengthSize=lengthSize, originType=originType)

		self.concurrentConnections[self.id] = self
		self.type = type
		self.originType = originType
		self.kafkaOutbox = kafkaOutbox

	def validate(self, jsonObj):
		if "messageType" in jsonObj and "payload" in jsonObj:
			return True
		else:
			raise ValueError("Unexpected schema in jsonObj: %s" % jsonObj)

	def receive(self):
		raise NotImplementedError()

	def close(self, sendDisconnectMessageOnClose=True):
		self.logger.debug("%s died!", self.type)

		if self.id in self.concurrentConnections:
			del self.concurrentConnections[self.id]

		jsonObj = {
			'route': '/socketDisconnect',
			'originType': self.originType
		}

		isMC = hasattr(self, 'ftype') and self.ftype not in ['c', 't']

		CMSocket.close(self)

def writeFileAtomicallySync(filePath, content):
	tempPath = '{}.tmp'.format(filePath)

	with open(tempPath, 'w') as tempFile:
		tempFile.write(content)
		tempFile.flush()
		os.fsync(tempFile.fileno())

	shutil.move(tempPath, filePath)

class VolumeActions:
	ATTACH = 'attach'
	DETACH = 'detach'
	NONE = 'none'

class ClientSocket(SocketToManagement):
	def __init__(self, socket, readList, writeList, concurrentConnections, kafkaOutbox, logger, type, originType="Unknown"):
		SocketToManagement.__init__(self, socket, readList, writeList, concurrentConnections, kafkaOutbox, logger, type, originType=originType)

		self.cache = {
			'isDirty': False,
			'attachments': {},
			'targets': []
		}

		self.CACHE_LOCATION = '/var/opt/nvmesh/mcs/client_persistence'
		self.isUpdateTokenReceived = False

		self.loadCacheFromDrive()

	def generateKeepAliveMessage(self):
		keepAlive = self.cache['keepAlive']

		return {
			'messageType': 'updateClientKeepaliveToken',
			'opcode': MessageTypes.UPDATE_CLIENT_KEEPALIVE_TOKEN,
			'messageTypeVersion': keepAlive['messageTypeVersion'],
			'payload': {
				'messageSequence': keepAlive['messageSequence'],
				'clientToken': keepAlive['clientToken'],
				'reportID': keepAlive['reportID']
			}
		}

	def generateUpdateTargetNICsMessage(self):
		targets = self.cache['targets']
		messagesByVersion = {}

		for target in targets:
			if target['messageTypeVersion'] not in messagesByVersion:
				messageTypeVersion = target['messageTypeVersion']
				messagesByVersion[messageTypeVersion] = {
					'messageType': 'updateTargetNICs',
					'opcode': MessageTypes.UPDATE_TARGET_NICS,
					'messageTypeVersion': messageTypeVersion,
					'payload': {
						'targets': []
					}
				}

			messagesByVersion[messageTypeVersion]['payload']['targets'].append(target)

		messages = []

		for version in messagesByVersion:
			messages.append(messagesByVersion[version])

		return messages

	def generateAttachByUUID(self, uuid):
		message = None

		if 'attachments' in self.cache and uuid in self.cache['attachments']:
			volume = self.cache['attachments'][uuid]

			if volume['action'] == VolumeActions.ATTACH:
				message = {
					'messageType': 'attachVolumes',
					'opcode': MessageTypes.ATTACH_VOLUMES,
					'messageTypeVersion': volume['messageTypeVersion'],
					'payload': {
						'volumes': [{
							'configuration': volume['configuration'],
							'attachment': volume['attachment']
								if 'attachmentsVersionRef' in volume['attachment'] and
									volume['attachmentsVersion'] == volume['attachment']['attachmentsVersionRef']
								else {}
						}],
						'attachmentsVersion': volume['attachmentsVersion']
					}
				}

		return message

	def generateAttachesFromCache(self):
		messages = []

		for uuid in self.cache['attachments']:
			message = self.generateAttachByUUID(uuid)

			if message:
				messages.append(message)

		return messages

	def generateEndOfCacheMessage(self):
		return [{
			'messageType': 'endOfCache',
			'opcode': MessageTypes.END_OF_CACHE,
			'messageTypeVersion': 1,
			'payload': {}
			}]

	def resendMessagesFromCache(self, writeList):
		messages = []

		#Generate attaches
		messages += self.generateAttachesFromCache()
		messages += self.generateUpdateTargetNICsMessage()
		messages += self.generateEndOfCacheMessage()

		preparedMessages = []

		for message in messages:
			preparedMessages += self.prepareMessages(message)

		self.logger.debug('resendMessagesFromCache sending {} messages: {}'.format(len(preparedMessages), preparedMessages))

		if len(preparedMessages):
			self.sendMessages(preparedMessages, writeList)

	def loadObjectFromPersistence(self, path):
		with open(path) as fd:
			content = json.loads(fd.read())

		return content

	def loadCacheFromDrive(self):
		if not os.path.exists(self.CACHE_LOCATION):
			return os.makedirs(self.CACHE_LOCATION)

		cachedFiles = [(os.path.join(self.CACHE_LOCATION, filename), filename) for filename in os.listdir(self.CACHE_LOCATION)]

		for absolutePath, filename in cachedFiles:
			if filename.endswith('.tmp'):
				self.logger.debug('Found {} cached file upon startup, we\'ve probably crashed before we finished to save it to disk, removing it'.format(filename))
				os.remove(absolutePath)
			elif filename == 'CLIENT_KEEPALIVE':
				self.cache['keepAlive'] = self.loadObjectFromPersistence(absolutePath)
			elif filename == 'TARGET_NICS':
				self.cache['targets'] = self.loadObjectFromPersistence(absolutePath)
			else:
				self.cache['attachments'][filename] = self.loadObjectFromPersistence(absolutePath)

	def purgeCache(self):
		self.logger.debug('Purging the cache')
		try:
			for file in [os.path.join(self.CACHE_LOCATION, filename) for filename in os.listdir(self.CACHE_LOCATION)]:
				self.logger.debug('Removing the file: {}'.format(file))
				os.unlink(file)
		except Exception as e:
			self.logger.error('Failed to purge cache. Error: {}'.format(e))
		finally:
			self.cache = {
				'isDirty': False,
				'attachments': {},
				'targets': []
			}

	def handleUpdateClientTokenMessage(self, msg):
		payload = msg['payload']

		if 'keepAlive' in self.cache and self.cache['keepAlive']['clientUUID'] != msg['payload']['clientUUID']:
			self.logger.debug('UUID has changed, probalby the client was removed from the management oldUUID: {}, messageUUID: {}'
				.format(self.cache['keepAlive']['clientUUID'], msg['payload']['clientUUID']))

			self.purgeCache()

		if 'keepAlive' not in self.cache or self.cache['keepAlive']['clientToken'] < payload['clientToken']:
			self.cache['keepAlive'] = {
				'messageTypeVersion': msg['messageTypeVersion'],
				'messageSequence': payload['messageSequence'],
				'clientToken': payload['clientToken'],
				'clientUUID': payload['clientUUID'],
				'reportID': payload['reportID'] if 'reportID' in payload else -1
			}

			self.cache['isDirty'] = True
			self.cache['shouldForwardMessage'] = True
		elif 'keepAlive' in self.cache and self.cache['keepAlive']['clientToken'] == payload['clientToken']:
			self.cache['shouldForwardMessage'] = True

	def setConfigurationToCache(self, configuration):
		cachedVolume = self.cache['attachments'][configuration['uuid']]

		if 'configuration' not in cachedVolume or cachedVolume['configuration']['version'] < configuration['version']:
			cachedVolume['configuration'] = configuration

	def setAttachmentObjToCache(self, uuid, attachment):
		cachedVolume = self.cache['attachments'][uuid]

		if 'attachment' not in cachedVolume or cachedVolume['attachment']['version'] < attachment['version'] or cachedVolume['attachment']['attachmentsVersionRef'] < attachment['attachmentsVersionRef']:
			cachedVolume['attachment'] = attachment

	def handleAttachMessage(self, msg):
		payload = msg['payload']
		attachmentsVersion = payload['attachmentsVersion']
		cachedVolume = None

		for volume in payload['volumes']:
			if volume['uuid'] in self.cache['attachments']:
				cachedVolume = self.cache['attachments'][volume['uuid']]

			if not cachedVolume or attachmentsVersion > cachedVolume['attachmentsVersion']:
				self.cache['attachments'][volume['uuid']] = {
					'messageTypeVersion': msg['messageTypeVersion'],
					'action': VolumeActions.ATTACH,
					'attachmentsVersion': attachmentsVersion
				}

				#Takes the configuration & attachments only if the version in the message is higher
				#It may be lower if the emulate \ update messages arrived before the attach command.
				self.setConfigurationToCache(volume['configuration'])
				self.setAttachmentObjToCache(volume['uuid'], volume['attachment'])

				self.cache['isDirty'] = True

			#Check if we received update before attach, and set to attach although the attachmentsVersion might be lower
			elif cachedVolume['action'] == VolumeActions.NONE:
				cachedVolume['action'] = VolumeActions.ATTACH
				cachedVolume['attachmentsVersion'] = attachmentsVersion
				self.cache['isDirty'] = True

				self.setConfigurationToCache(volume['configuration'])
				self.setAttachmentObjToCache(volume['uuid'], volume['attachment'])

		self.cache['shouldForwardMessage'] = self.cache['isDirty']

	def handleDetachMessage(self, msg):
		payload = msg['payload']
		attachmentsVersion = payload['attachmentsVersion']
		cachedVolume = None

		for volume in payload['volumes']:
			if volume['uuid'] in self.cache['attachments']:
				cachedVolume = self.cache['attachments'][volume['uuid']]

			if not cachedVolume or attachmentsVersion > cachedVolume['attachmentsVersion']:
				self.cache['attachments'][volume['uuid']] = {
					'messageTypeVersion': msg['messageTypeVersion'],
					'action': VolumeActions.DETACH,
					'attachmentsVersion': attachmentsVersion
				}

				self.cache['isDirty'] = True
				self.cache['shouldForwardMessage'] = True
			elif cachedVolume['action'] == VolumeActions.NONE:
				cachedVolume['action'] = VolumeActions.DETACH
				cachedVolume['attachmentsVersion'] = attachmentsVersion
				self.cache['isDirty'] = True

	def handleUpdateVolumeMessage(self, msg):
		payload = msg['payload']
		cachedVolume = None

		for volume in payload['volumes']:
			if volume['uuid'] in self.cache['attachments']:
				cachedVolume = self.cache['attachments'][volume['uuid']]

			if not cachedVolume:
				self.cache['attachments'][volume['uuid']] = {
					'messageTypeVersion': msg['messageTypeVersion'],
					'action': VolumeActions.NONE,
					'attachmentsVersion': -1, #Setting to -1 as no action is required if this is the first message on the volume
					'configuration': volume
				}

				self.cache['isDirty'] = True
			#we won't have config field in cachedVolume if the last messages was detach
			elif 'configuration' in cachedVolume and volume['version'] > cachedVolume['configuration']['version']:
				cachedVolume['configuration'] = volume

				if cachedVolume['action'] == VolumeActions.ATTACH:
					self.cache['shouldForwardMessage'] = True

				self.cache['isDirty'] = True

	def handleUpdateTargetNicsMessage(self, msg):
		self.logger.debug("update targets nics: %s", json.dumps(msg))
		payload = msg['payload']

		for target in payload['targets']:
			targetID = target['node_id']

			cachedTarget = [t for t in self.cache['targets'] if t['node_id'] == targetID]

			if not len(cachedTarget) or cachedTarget[0]['nicsVersion'] < target['nicsVersion'] or cachedTarget[0]['targetUpdatesSequence'] < target['targetUpdatesSequence']:
				target['messageTypeVersion'] = msg['messageTypeVersion']

				if len(cachedTarget):
					cachedTarget[0].update(target)
				else:
					self.cache['targets'].append(target)

				self.cache['shouldForwardMessage'] = True
				self.cache['isDirty'] = True

	def handleUpdateVolumeAttachmentByMessage(self, msg):
		debugMsg  = 'Update volume '

		if msg['opcode'] == MessageTypes.UPDATE_VOLUME_EMULATION:
			debugMsg += 'emulation'
		elif msg['opcode'] == MessageTypes.UPDATE_VOLUME_REFERENCE:
			debugMsg += 'reference'

		debugMsg += ': {}'.format(json.dumps(msg))

		self.logger.debug(debugMsg)

		payload = msg['payload']
		attachment = payload['attachment']
		volumeUUID = payload['volumeUUID']
		cachedVolume = None

		if volumeUUID in self.cache['attachments']:
			cachedVolume = self.cache['attachments'][volumeUUID]

		if not cachedVolume:
			self.cache['attachments'][volumeUUID] = {
				'messageTypeVersion': msg['messageTypeVersion'],
				'action': VolumeActions.NONE,
				'attachmentsVersion': -1,
				'attachment': attachment
			}

			self.cache['isDirty'] = True
		elif 'attachment' not in cachedVolume or attachment['version'] > cachedVolume['attachment']['version'] or attachment['attachmentsVersionRef'] > cachedVolume['attachment']['attachmentsVersionRef']:
			if 'attachmentsVersion' in payload and payload['attachmentsVersion'] > cachedVolume['attachmentsVersion']:
				cachedVolume['attachmentsVersion'] = payload['attachmentsVersion']

			cachedVolume['attachment'] = attachment

			self.cache['isDirty'] = True

			if attachment['attachmentsVersionRef'] == cachedVolume['attachmentsVersion']:
				self.cache['shouldForwardMessage'] = True

	def writeVolumeMessageToPersistence(self, msg):
		payload = msg['payload']

		for volume in payload['volumes']:
			uuid = volume['uuid']
			writeFileAtomicallySync(os.path.join(self.CACHE_LOCATION, uuid), json.dumps(self.cache['attachments'][uuid]))

	def updateClientPersistence(self, msg):
		try:
			msgOpcode = msg['opcode']

			if msgOpcode == MessageTypes.UPDATE_CLIENT_KEEPALIVE_TOKEN:
				writeFileAtomicallySync(os.path.join(self.CACHE_LOCATION, 'CLIENT_KEEPALIVE'), json.dumps(self.cache['keepAlive']))
			elif msgOpcode == MessageTypes.UPDATE_TARGET_NICS:
				writeFileAtomicallySync(os.path.join(self.CACHE_LOCATION, 'TARGET_NICS'), json.dumps(self.cache['targets']))
			elif msgOpcode in [MessageTypes.DETACH_VOLUMES, MessageTypes.UPDATE_VOLUMES, MessageTypes.ATTACH_VOLUMES]:
				self.writeVolumeMessageToPersistence(msg)

			return True
		except Exception as e:
			self.logger.error('Received an error while trying to write client persistence, this message cannot be committed. Exception: {}'.format(e))
			return False

	def cacheMessage(self, msg, completed):
		msgOpcode = msg['opcode']

		if msgOpcode == MessageTypes.UPDATE_CLIENT_KEEPALIVE_TOKEN:
			self.handleUpdateClientTokenMessage(msg)
		elif msgOpcode == MessageTypes.ATTACH_VOLUMES:
			self.handleAttachMessage(msg)
		elif msgOpcode == MessageTypes.DETACH_VOLUMES:
			self.handleDetachMessage(msg)
		elif msgOpcode == MessageTypes.UPDATE_VOLUMES:
			self.handleUpdateVolumeMessage(msg)
		elif msgOpcode == MessageTypes.UPDATE_TARGET_NICS:
			self.handleUpdateTargetNicsMessage(msg)
		elif msgOpcode == MessageTypes.UPDATE_VOLUME_EMULATION or msgOpcode == MessageTypes.UPDATE_VOLUME_REFERENCE:
			self.handleUpdateVolumeAttachmentByMessage(msg)
		elif msgOpcode == MessageTypes.UPDATE_VOLUME_REFERENCE:
			self.handleUpdateVolumeAttachmentByMessage(msg)
		else:
			return True

		if self.cache['isDirty']:
			if self.updateClientPersistence(msg) and completed:
				completed()
		else:
			completed()

		return 'shouldForwardMessage' in self.cache and self.cache['shouldForwardMessage']

	def shouldConvertMsgToAttach(self, msg):
		return msg['opcode'] == MessageTypes.ATTACH_VOLUMES or msg['opcode'] == MessageTypes.UPDATE_VOLUME_EMULATION or msg['opcode'] == MessageTypes.UPDATE_VOLUME_REFERENCE

	#This function will receive an attach message from the management and will send an updated attachment message for every volume in the original attach message
	def sendAttachesFromCacheByMessage(self, msg, writeList):
		preparedMessages = []
		if msg['opcode'] == MessageTypes.ATTACH_VOLUMES:
			for volume in msg['payload']['volumes']:
				attachMessage = self.generateAttachByUUID(volume['uuid'])
				if attachMessage:
					preparedMessages += self.prepareMessages(attachMessage)
		elif msg['opcode'] in [MessageTypes.UPDATE_VOLUME_EMULATION, MessageTypes.UPDATE_VOLUME_REFERENCE]:
			attachMessage = self.generateAttachByUUID(msg['payload']['volumeUUID'])
			if attachMessage:
				preparedMessages = self.prepareMessages(attachMessage)

		if preparedMessages:
			self.sendMessages(preparedMessages, writeList)

	def send(self, payload=None, writeList=None, sendNextMessage=True, completed=None):
		if not self.originType == 'CLIENT':
			self.sendToClient(payload, writeList, sendNextMessage)

			if completed:
				return completed()

			return True

		msg = json.loads(payload[4:])

		if self.cacheMessage(msg, completed):
			#In case this is an attachVolumes message, we should send it from cache instead of forwarding the message, same of update emulation message.
			if self.shouldConvertMsgToAttach(msg):
				self.sendAttachesFromCacheByMessage(msg, writeList)
			else:
				self.sendToClient(payload, writeList, sendNextMessage)

			if not self.isUpdateTokenReceived and msg['opcode'] == MessageTypes.UPDATE_CLIENT_KEEPALIVE_TOKEN:
				self.isUpdateTokenReceived = True
				self.replayCacheUponReceivingUpdateToken(writeList)

	def replayCacheUponReceivingUpdateToken(self, writeList):
		pass

	def prepareMessage(self, opcode, messageTypeVersion, payload):
		ret: List[BinaryMessage] = []
		payload.update({"messageTypeVersion": messageTypeVersion})

		try:
			data = self.packer.pack2(opcode, payload)
		except Exception as e:
			self.logger.error("error while trying to pack message: %s", str(e))
			raise

		self.logger.debug("sendData::FileSocket - After Pack. opcode: %s num messages=%d", opcode, len(data))

		for msg in data:
			ret.append(BinaryMessage(self, opcode, msg, logger=self.logger))

		return ret

	def prepareMessages(self, data):
		opcode = data["opcode"]
		assert 'messageTypeVersion' in data, "data didn't have the messageTypeVersion key"
		messageTypeVersion = data["messageTypeVersion"]
		return self.prepareMessage(opcode, messageTypeVersion, data["payload"] or {})

class JsonSocket(ClientSocket):
	def __init__(self, cmSocket, readList, writeList, concurrentConnections, kafkaOutbox, logger):
		ClientSocket.__init__(self, cmSocket, readList, writeList, concurrentConnections, kafkaOutbox, logger, "JsonSocket")

	def receive(self):
		msg = CMSocket.receive(self)
		msg = bytearray(msg).decode()
		self.logger.debug('JsonSocket received new message.')
		try:
			jsonObj = json.loads("".join(msg))

			if (not self.originType or self.originType == 'Unknown') and 'originType' in jsonObj:
				self.originType = jsonObj['originType']

			self.validate(jsonObj)
		except Exception as e:
			self.logger.exception("Failed to parse received json!")
			return
		finally:
			CMSocket.clear(self)

		message = Message(self, jsonObj, logger=self.logger)

		self.logMessage(self.originType, 'MGMT', jsonObj)
		self.kafkaOutbox.put(message)

		return jsonObj

	def sendToClient(self, payload=None, writeList=None, sendNextMessage=True):
		try:
			if not payload:
				self.logger.error("send::JsonSocket called without payload")
				return

			self.logger.verbose("send::JsonSocket before json.loads: {}".format(payload), verboseType="JsonSocket")
			data = json.loads(payload[4:])
			self.logger.verbose("send::JsonSocket after json.loads, data: {}".format(data), verboseType="JsonSocket")

			self.sendData(data, writeList, sendNextMessage)

		except (socket.error, IOError) as e:
			self.handleSocketErrorException(e)
		except Exception as ex:
			self.logger.exception("Failed while send::JsonSocket, payload: {}, ex: {}".format(payload, ex))

	def sendData(self, data, writeList=None, sendNextMessage=True):
		self.logger.verbose("sendData::JsonSocket data: {}".format(data), verboseType="JsonSocket")

		if not ('payload' in data and data['payload'] is not None):
			self.logger.error("sendData::JsonSocket received data without key: {}".format('payload'), verboseType="JsonSocket")
			return

		message = GenericMessage(self, data, logger=self.logger)
		self.logger.verbose("sendData::JsonSocket going to send message: {}".format(message), verboseType="JsonSocket")
		CMSocket.sendMessage(self, message, writeList, sendNextMessage)

	def prepareMessages(self, messages):
		message = GenericMessage(self, messages, logger=self.logger)
		return [message]


class FileSocket(ClientSocket):
	CLIENT_VERSION_FILE = '/proc/nvmeibc/version'
	IS_SETTING_CLIENT_MODULE_VERSION = False
	CLIENT_MODULE_VERSION = None

	def __init__(self, filePath, jsonScheme, readList, writeList, concurrentConnections, logger, procListener, ftype, kafkaOutbox):
		self.procListener = procListener
		self.fd = io.FileIO(filePath, 'r+b', os.O_NONBLOCK)
		self.packer = process_scheme.Packer(jsonScheme, logger=logger)
		self.packer.set_lib("clnt.client_messages")

		ftypeToOriginType = {
			't': 'TARGET',
			'c': 'CLIENT'
		}
		originType = ftypeToOriginType.get(ftype, 'CLIENT')

		ClientSocket.__init__(self, self.fd, readList, writeList, concurrentConnections, kafkaOutbox, logger, "FileSocket", originType=originType)

		self.procListener[ftype] = self
		self.ftype = ftype
		self.ReceiveLength = 0
		self.lenIoctl = 0x8008BB01
		self.typeOfSize = 'Q'
		self.buf = array.array('b', b'\x00' * struct.calcsize(self.typeOfSize))

		if self.originType == 'CLIENT' and not (FileSocket.CLIENT_MODULE_VERSION or FileSocket.IS_SETTING_CLIENT_MODULE_VERSION):
			CMSocket.IS_SETTING_CLIENT_MODULE_VERSION = True
			try:
				while not FileSocket.CLIENT_MODULE_VERSION:
					if os.path.exists(FileSocket.CLIENT_VERSION_FILE):
						self.setClientModuleVersion()
					else:
						time.sleep(0.5)
			except Exception as e:
				self.logger.error('Failed to check if {} exists, ex: {}'.format(FileSocket.CLIENT_VERSION_FILE, e))

		self.resendMessagesFromCache(writeList)

	def setClientModuleVersion(self):
		success, out = runCommand(command=['cat', FileSocket.CLIENT_VERSION_FILE], logger=self.logger)
		if success:
			out = json.loads(out)
			FileSocket.CLIENT_MODULE_VERSION = out['version']
		else:
			self.logger.error('Failed to get the client module version, exiting.')
			exit(1)

	def setOptions(self):
		pass

	def receiveFunction(self, size):
		return self.fd.read(size)

	def sendFunction(self, data):
		return self.fd.write(data)

	def receive(self):
		try:
			fcntl.ioctl(self.fd.fileno(), self.lenIoctl, self.buf)
		except Exception as f:
			self.logger.debug("excepted as f type %s message: %s args %s", type(f), str(f), f.args)
			if isinstance(f, IOError) and f.args[0] == errno.ENOTTY:
				self.logger.debug("connection reset by peer")
				raise
			self.logger.debug("an exception is thrown ewouldblock is assumed")
			e = IOError()
			e.args = (errno.EWOULDBLOCK,)
			raise e
		sizeOfMsg = struct.unpack(self.typeOfSize, self.buf)[0]
		data = CMSocket.receive(self, sizeOfMsg)
		res = self.packer.unpack(bytes(data[4:]))

		if self.originType == 'CLIENT':
			res["clientVersion"] = FileSocket.CLIENT_MODULE_VERSION

		message = Message(self, res, priority=res["priority"], type=res["opcode"], logger=self.logger)
		self.logMessage(self.originType, 'MGMT', res)

		self.kafkaOutbox.put(message)

	@staticmethod
	def filterResponseForRequestedVolumes(response, requestedVolumes):
		requestedVolumesNames = [vol["name"] for vol in requestedVolumes]
		requestedVolumesUUIDs = [vol["uuid"] for vol in requestedVolumes]

		if "volumes" in response["payload"]:
			response = copy.deepcopy(response)
			response["payload"]["volumes"] = [cacheVol for cacheVol in response["payload"]["volumes"] if cacheVol["name"] in requestedVolumesNames or cacheVol["uuid"] in requestedVolumesUUIDs]

		return response

	def sendToClient(self, payload=None, writeList=None, sendNextMessage=True):
		self.logger.debug("NvmeshUMSocket send called")
		try:
			data = json.loads(payload[4:])

			binaryMessages = self.prepareMessages(data)

			CMSocket.sendMessages(self, binaryMessages, writeList)
		except RuntimeError as e:
			self.logger.exception("runt time error\nerror:%s\n when trying to pack message:%s\ntraceback:%s", e.args, e.message, self.packer.get_traceback())
			return
		except (socket.error, IOError) as e:
			self.handleSocketErrorException(e)
		except Exception as e2:
			self.logger.exception("Can't respond to file socket\nerror %s\n - couldn't decode JSON! %s:%s", payload[4:], e2.args, e2.message)

	def close(self, sendDisconnectMessageOnClose=True):
		if self.ftype in self.procListener:
			del self.procListener[self.ftype]

		SocketToManagement.close(self, sendDisconnectMessageOnClose)


class NvmeshUMSocket(ClientSocket):
	#AK: TODO - there's currently no client version file in nvmeshum
	#	implement when this is relevant
	#CLIENT_VERSION_FILE = '/proc/nvmeibc/version'
	#IS_SETTING_CLIENT_MODULE_VERSION = False
	#CLIENT_MODULE_VERSION = None

	def __init__(self, cmSocket, jsonScheme, readList, writeList, concurrentConnections, logger, ftype, kafkaOutbox):
		self.packer = process_scheme.Packer(jsonScheme, logger=logger)

		#enable packer to run functions from this module
		self.packer.set_lib("clnt.client_messages")

		ftypeToOriginType = {
			't': 'TARGET',
			'c': 'CLIENT'
		}
		originType = ftypeToOriginType.get(ftype, 'CLIENT')

		ClientSocket.__init__(self, cmSocket, readList, writeList,
			      concurrentConnections, kafkaOutbox, logger, "NvmeshUMSocket", originType=originType)

		self.ftype = ftype
		self.resendMessagesFromCache(writeList)

	def receive(self):
		self.logger.debug("NvmeshUMSocket receive called")
		data = CMSocket.receive(self)
		res = self.packer.unpack(bytes(data))

		if self.originType == 'CLIENT':
			res["clientVersion"] = "nvmeshum_dummy_version"

		message = Message(self, res, priority=res["priority"], type=res["opcode"], logger=self.logger)
		self.logMessage(self.originType, 'MGMT', res)

		self.kafkaOutbox.put(message)

	def replayCacheUponReceivingUpdateToken(self, writeList):
		self.logger.debug('This is the first updateToken, replaying messages from cache')
		self.resendMessagesFromCache(writeList)

	@staticmethod
	def filterResponseForRequestedVolumes(response, requestedVolumes):
		requestedVolumesNames = [vol["name"] for vol in requestedVolumes]
		requestedVolumesUUIDs = [vol["uuid"] for vol in requestedVolumes]

		if "volumes" in response["payload"]:
			response = copy.deepcopy(response)
			response["payload"]["volumes"] = [cacheVol for cacheVol in response["payload"]["volumes"] if cacheVol["name"] in requestedVolumesNames or cacheVol["uuid"] in requestedVolumesUUIDs]

		return response

	def sendToClient(self, payload=None, writeList=None, sendNextMessage=True):
		self.logger.debug("NvmeshUMSocket send called")
		try:
			data = json.loads(payload[4:])

			binaryMessages = self.prepareMessages(data)

			CMSocket.sendMessages(self, binaryMessages, writeList)
		except RuntimeError as e:
			self.logger.exception("runt time error\nerror:%s\n when trying to pack message:%s\ntraceback:%s",
								  e.args, e.message, self.packer.get_traceback())
			return
		except (socket.error, IOError) as e:
			self.handleSocketErrorException(e)
		except Exception as e2:
			self.logger.exception("Can't respond to file socket\nerror %s\n - couldn't decode JSON! %s"
								  , payload[4:], e2)

	def close(self, sendDisconnectMessageOnClose=True):
		self.logger.debug("NvmeshUMSocket socket close called")

		SocketToManagement.close(self, sendDisconnectMessageOnClose)