#!/usr/bin/env python3

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

import datetime
import errno
import os, time
import logging
import logging.handlers
import json
import signal
import sys
import subprocess
import socket
import select
import struct
import traceback
import fcntl
import multiprocessing

from daemon import Daemon
from CMSocket import CMSocket
from Mailbox import GenericMessage

dir_path = os.path.dirname(os.path.realpath(__file__))

NVMESH_DEV_DIR = 'nvmesh'
NUMBER_OF_CORES = multiprocessing.cpu_count()
is_k8s = 'K8S_ENV' in os.environ
PID_DIR = '/var/run/nvmesh'
KEYS_DIR = '/etc/nvmesh/keys/'
CONFIG_PATH = '/etc/nvmesh/configs/nvmeibc'
USER_NVMESH_CONF_PATH = '/etc/nvmesh/nvmesh.conf'
MGMT_HIDDEN_CONFIG_FILE_PATH = os.path.join(CONFIG_PATH, '.mgmt.nvmesh.conf')


def readBashFile(filename):
	g = {}
	l = {}

	if os.path.exists(filename):
		exec(compile(open(filename, "rb").read(), filename, 'exec'), g, l)

	return l

def logStackTrace(ex, logger):
	exc_type, exc_value, exc_traceback = sys.exc_info()
	exString = traceback.format_exc()
	errorLines = exString.split('\n')

	for line in errorLines:
		logger.error(line)

class MessageTypes(object):
	class FromMgmt(object):
		UPDATE_AGENT_TOKEN = 'updateAgentToken'
		UPDATE_CONFIG_PROFILE = 'updateConfigProfile'
	class ToMgmt(object):
		KEEP_ALIVE = 'keepalive'
		CONFIG_PROFILE_UPDATED = 'configProfileUpdated'
		UPDATE_CONFIG_PROFILE_USER_OVERRIDE = 'updateConfigProfileUserOverride'
		UPDATE_KEYS = 'updateKeys'

g = {}
nvmesh_conf = {}

def include(filename):
	if os.path.exists(filename):
		exec(compile(open(filename, "rb").read(), filename, 'exec'), g, nvmesh_conf)

include('/opt/nvmesh/client-repo/version')

CLIENT_VERSION = nvmesh_conf.get('version', '')
CLIENT_BRANCH = nvmesh_conf.get('branch','')
CLIENT_COMMIT = nvmesh_conf.get('commit','')

RETRY_ERROR_CODE = 3
DEFAULT_KEEP_ALIVE_INTERVAL = 5
CLIENT_MODULE_DIR = '/sys/module/nvmeibc'
SERVER_ADDRESS = '/var/run/nvmesh/json_uds'
SYSLOG_PATH = '/dev/log'
RPM_NAME = "nvmesh-client"
CONFIG_DIR = "/sys/kernel/config/nvmeibc"
CACHED_BLOCK_DEVICES = "/var/opt/nvmesh/toma/cached_block_devices.csv"
HOSTNAME = socket.gethostname()
DEV_DIR = '/dev/nvmesh'
CLUSTER_SIZE = 1048576  # 1MiB


def getNodeID():
	return HOSTNAME


def runCommand(command, runInBackground=False):
	if command is None:
		logger.error("Empty command ignoring")
		return False, None
	logger.debug('Subprocess: "%s"', command)

	try:
		if runInBackground:
			process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
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


def runPipeCommand(commandA, commandB):
	if commandA is None or commandB is None:
		logger.error("Must have 2 commands for piping")
		return False, None

	logger.debug('Subprocess running piped commands: "{0} | {1}"'.format(commandA, commandB))

	try:
		process_a = subprocess.Popen(commandA, stdout=subprocess.PIPE, shell=False)
		process_b = subprocess.Popen(commandB, stdin=process_a.stdout, stdout=subprocess.PIPE, shell=False)
		process_a.stdout.close()

		output, _ = process_b.communicate()
		if process_b.returncode != 0:
			return False, None
	except OSError as exception:
		logger.exception('Subprocess failed for piped commands')
		raise
	else:
		logger.debug('Subprocess finished for piped commands')

	return True, output

def checkIfClientIsOn():
	return os.path.isdir(CLIENT_MODULE_DIR)

def managementAgentSpecialTeamActions():
	if AgentConfig.MANAGEMENT_AGENT_SPECIAL_TEAM_RUN_MLNX_AFFINITY == 1:
		subprocess.call(["mlnx_affinity", "start"])

# Original fix for Py2.6: https://github.com/mozilla/mozdownload/issues/73
def totalSeconds(dt):
	# Keep backward compatibility with Python 2.6 which doesn't have this method
	if hasattr(datetime, 'total_seconds'):
		return dt.total_seconds()
	else:
		return (dt.microseconds + (dt.seconds + dt.days * 24 * 3600) * 10 ** 6) / 10 ** 6


def releaseLock(lockfd):
	fcntl.flock(lockfd, fcntl.LOCK_UN)
	lockfd.close()


def tryTakeLock(lockpath):
	try:
		f = open(lockpath, 'w+')
	except IOError as err:
		if err.errno == errno.EACCES:
			logger.error("Couldn't take lock, failed to open file {0}, Permission Denied, should be run as root user".format(
				lockpath))
		return None

	try:
		fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
	except IOError as err:
		logger.debug("Couldn't take lock {0}, the script is probably currently running".format(lockpath))
		return None

	return f

class ClientControlAgent(Daemon):
	def __init__(self, logger):
		self.logger = logger
		self.clientSocket = None
		self.token = -1
		self.messageSequence = 0
		self.keepaliveInterval = DEFAULT_KEEP_ALIVE_INTERVAL
		self.lastMessageTime = None
		self.shouldContinue = True
		self.lunIDCounter = 0
		self.configProfileInfo = {}
		self.shouldSignalMainProcess = None
		self.featureCompatibilityVersion = 1 # must match client's reported version

		pidfile = os.path.join(PID_DIR, 'managementAgent.pid')
		lockfile = os.path.join(PID_DIR, 'managementAgent.lock')

		# registering on a signal that the forked process of the managementAgent has fully started and the main process can exit
		signal.signal(signal.SIGUSR2, self.exitParentGracefully)
		Daemon.__init__(self, pidfile, lockfile, notifyMainProcessOnStartup=True)

	def sendKeepAlive(self, force=False):
		logger.debug('in sendkeepalive lastMessageTime={}'.format(self.lastMessageTime))

		try:
			isTimeForNextKeepAlive = self.lastMessageTime and (datetime.datetime.now() - self.lastMessageTime).seconds < self.keepaliveInterval
			if not self.clientSocket or (not force and isTimeForNextKeepAlive):
				return
		except NameError:
			return

		payload = {
			'configProfileInfo': self.configProfileInfo,
			'featureCompatibilityVersion': self.featureCompatibilityVersion
		}

		self.sendToMCS(MessageTypes.ToMgmt.KEEP_ALIVE, payload, keepaliveInterval=self.keepaliveInterval)

	def handleMessage(self, msg):
		msgObj = msg
		logger.debug("Handling message: %s", msgObj)

		if "messageType" in msgObj:
			msgType = msgObj["messageType"]

			if msgType == MessageTypes.FromMgmt.UPDATE_AGENT_TOKEN:
				self.updateToken(msgObj["payload"])
				self.sendKeepAlive(force=True)
				self.sendMessagesOnNewToken()

			elif msgType == MessageTypes.FromMgmt.UPDATE_CONFIG_PROFILE:
				self.updateConfigurationProfile(msgObj["payload"])
				self.reportConfigUpdateSuccessful(msgObj["payload"])
			else:
				logger.warning("Unknown messageType: %s", msgType)
		else:
			logger.error("No message.messageType")

	def exitParentGracefully(self, signal, flag):
		sys.exit(0)

	def exitGracefully(self, signal, flag):
		self.shouldContinue = False
		sys.exit(0)

	def reloadKeys(self):
		self.logger.debug('Reloading keys and updating the management')
		self.sendUpdateKeysMessage()

	def reloadConfig(self, signum, frame):
		self.logger.debug("Received reload config signal.")
		readConfigFile()

		self.logger.debug("Setting LogLevel to {}".format(AgentConfig.loggingLevel))
		self.logger.setLevel(AgentConfig.loggingLevel)

		self.reloadKeys()

	# Singaling the main process so it can exit and let systemd know that the service has fully started
	def signalMainProcessIfNeeded(self):
		if self.shouldSignalMainProcess:
			self.signalMainProcessOnStartup()
			self.shouldSignalMainProcess = False

	def run(self):
		global handler

		self.readList = []
		self.writeList = []

		self.clientSocket = self.connect(SERVER_ADDRESS)

		signal.signal(signal.SIGINT, self.exitGracefully)
		signal.signal(signal.SIGTERM, self.exitGracefully)
		signal.signal(signal.SIGUSR1, self.reloadConfig)
		logger.info("Registered to signals")

		if os.path.exists(MGMT_HIDDEN_CONFIG_FILE_PATH):
			self.signalMainProcessIfNeeded()

		self.sendKeepAlive(force=True)

		iterations = -1

		while self.shouldContinue:
			if AgentConfig.remoteDebug:
				self.startRemoteDebugServer(port=5679)
				AgentConfig.remoteDebug = False

			iterations += 1
			self.sendKeepAlive()

			if AgentConfig.MANAGEMENT_AGENT_SPECIAL_TEAM_PERIOD > 0 and iterations % AgentConfig.MANAGEMENT_AGENT_SPECIAL_TEAM_PERIOD == 0:
				managementAgentSpecialTeamActions()

			try:
				readable, writeable, expList = select.select(self.readList, self.writeList, [], 1.0)

				if readable:
					for s in readable:
						logger.debug("I have got something to read!")
						data = s.receive()
						msg = None

						try:
							data = bytearray(data).decode()
							msg = json.loads(data)
						except Exception as e:
							logger.debug("Failed to parse message! %s Exception: %s", data, e)
						finally:
							s.clear()

						if msg:
							self.handleMessage(msg)
						else:
							self.reconnect()

				if writeable:
					logger.debug("A socket I've been writing to has been released")
					for s in writeable:
						CMSocket.send(s, writeList=self.writeList)

			except select.error as e:
				if e.errno == errno.EINTR:
					logger.debug("Received EINTR, retrying. err: %s", e)
				else:
					logger.debug(f"Caught socket error {e}")
					self.handleSelectError(e)
			except Exception as e:
				logger.debug(f"Error: something bad happened, nothing I can do, I'm just a little agent, waiting 5 sec and trying again. ex: {e}")
				self.handleSelectError(e)

		logger.debug("Management Agent is shutting down")

	def handleSelectError(self, error):
		if error and logger.level == logging.DEBUG:
			logStackTrace(error, logger)

		self.reconnect()
		time.sleep(5)

	def sendMessagesOnNewToken(self):
		self.sendUpdateKeysMessage()
		self.reportConfigProfilesOverride()

	def startRemoteDebugServer(self, port):
		try:
			import debugpy
			try:
				debugpy.listen(('localhost', port))
				logger.debug('Listening on port {} for debugger'.format(port))
			except socket.error as e:
				if e.errno == errno.EADDRINUSE:
					logger.debug('port {} is in use'.format(port))
				else:
					logger.debug('tried to listen for debug on port {}, ex: {}'.format(port, e))
		except ImportError as e:
			logger.warning('Failed to import debugpy! Install debugpy and try again. ex: {}'.format(e))
		except Exception as e:
			logger.error('Failed to start remote debug server. ex: {}'.format(e))

	def reconnect(self):
		logger.debug("Closing the socket and starting again.")
		try:
			self.clientSocket = self.clientSocket.close()
		except Exception as e:
			logger.exception("Failed to close socket.")
		self.clientSocket = self.connect(SERVER_ADDRESS)
		self.reportConfigProfilesOverride()
		self.sendKeepAlive(force=True)

	def sendUpdateKeysMessage(self):
		keys = self.load_keys()

		# send updateKeys message with the new keys
		logger.debug("keys changed. sending /updateKeys message to management")
		self.sendToMCS(MessageTypes.ToMgmt.UPDATE_KEYS, payload={'keys': keys})

	def load_keys(self):
		self.logger.debug('loading keys from directory %s' % KEYS_DIR)
		keys = []

		def should_ignore_file(filename):
			# this will ignore all "hidden" files including intermediate files when editing with terminal editors (vi/vim)
			return os.path.basename(filename).startswith('.')

		for (path, dirs, fnames) in os.walk(KEYS_DIR):
			for keyFile in fnames:
				if should_ignore_file(keyFile):
					continue
				try:
					with open(os.path.join(path, keyFile), 'r') as keyFD:
						content = keyFD.read()
						key = json.loads(content)
						keys.append(key)
				except Exception as e:
					logging.error('Failed to load key {0}, ex: {1}'.format(keyFile, e))
		return keys

	def updateToken(self, payload):
		self.token = payload['token']
		self.messageSequence = payload['messageSequence']
		if payload['keepaliveInterval'] < 1:
				logger.error('keepaliveInterval cannot be smaller then 1 second, got: {}'.format(payload['keepaliveInterval']))
				return

		self.keepaliveInterval = payload['keepaliveInterval']

	def reportConfigUpdateSuccessful(self, configInfo):
		params = configInfo['parameters']
		self.configProfileInfo.update({
			'id': params['CONFIG_PROFILE_ID'],
			'version': params['CONFIG_PROFILE_VERSION'],
			'name': params['CONFIG_PROFILE_NAME']
		})

		payload = {
			'nodeID': getNodeID(),
			'configProfileInfo': self.configProfileInfo
		}

		self.sendToMCS(MessageTypes.ToMgmt.CONFIG_PROFILE_UPDATED, payload=payload)

	def reportConfigProfilesOverride(self):
		configProfileInfo = self.getConfigProfileData()
		self.configProfileInfo.update(configProfileInfo)

		payload = {
			'nodeID':  getNodeID(),
			'configProfileInfo':  configProfileInfo
		}

		self.sendToMCS(MessageTypes.ToMgmt.UPDATE_CONFIG_PROFILE_USER_OVERRIDE, payload=payload)

	def sendToMCS(self, messageType, payload=None, resendOnFailover=False, priority=1, keepaliveInterval=None):
		logger.debug("Sending to MCS: %s", messageType)

		obj = {
				'messageType': messageType,
				'messageTypeVersion': 1,
				'clientID': getNodeID(),
				'originType': 'MANAGEMENT_AGENT',
				'messageSequence': self.messageSequence,
				'mgmtAgentToken': self.token,
				'payload': payload
			}

		if keepaliveInterval is not None:
			obj['keepaliveInterval'] = keepaliveInterval

		message = GenericMessage(self.clientSocket, obj, logger=logger)
		self.messageSequence += 1

		try:
			self.clientSocket.sendMessage(message, self.writeList)
			self.lastMessageTime = datetime.datetime.now()
		except Exception as e:
			logger.error("Failed to send message to MCS. messageType=%s. Error: %s", messageType, str(e))

	def updateConfigurationProfile(self, configurationProfile):
		if is_k8s:
			logger.info("Skipping Profile update when running in Kubernetes")
			return

		logger.debug("Updating Configuration Profile. Config path: {}".format(CONFIG_PATH))

		tempFilePath = MGMT_HIDDEN_CONFIG_FILE_PATH + '.swp'
		paramsDict = configurationProfile['parameters']
		with open(tempFilePath, 'w+') as nvmeshConfFile:
			for (key, value) in list(paramsDict.items()):
				nvmeshConfFile.write('{}="{}"\n'.format(key, value))

		runCommand(["mv", tempFilePath, MGMT_HIDDEN_CONFIG_FILE_PATH])
		os.chmod(MGMT_HIDDEN_CONFIG_FILE_PATH, 0o664)

		logger.info("Configuration Profile Updated id: {} version: {}".format(paramsDict['CONFIG_PROFILE_ID'], paramsDict['CONFIG_PROFILE_VERSION']))
		self.signalMainProcessIfNeeded()
		return True

	def readFromMCS(self):
		messageLength = self.clientSocket.recv(4)
		if len(messageLength):
			messageLength = struct.unpack("I", messageLength)[0]
			bytesReceived = []

			while len(bytesReceived) < messageLength:
				bytesReceived += self.clientSocket.recv(messageLength - len(bytesReceived))

			msg = "".join(bytesReceived)

			logger.debug("MCS Message %s", msg)
			return msg
		else:
			logger.error("SOCKET Closed")
			return None

	def connect(self, address):
		logger.debug("Connecting to socket...")
		cmSocket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
		cmSocket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

		newSocket = CMSocket(cmSocket, self.readList, self.writeList, self.logger)

		while True:
			try:
				cmSocket.connect(address)
				logger.debug("Connected!")
				break
			except Exception as e:
				time.sleep(1)
				logger.debug("Retrying to connect..." + str(e))

			cmSocket.settimeout(None)

		return newSocket

	def getConfigProfileData(self):
		nodeParams = readBashFile(os.path.join(CONFIG_PATH, '.mgmt.nvmesh.conf'))
		userParams = readBashFile(USER_NVMESH_CONF_PATH)

		nodeParams.update(userParams)

		# The following fields, even if set in the nvmesh.conf will not be considered User override
		userParams.pop('KAFKA_SERVERS', None)
		userParams.pop('MANAGEMENT_SERVERS', None)
		userParams.pop('MANAGEMENT_PROTOCOL', None)
		userParams.pop('CONFIGURED_NICS', None)
		userParams.pop('BLACKLIST_NICS', None)
		userParams.pop('ACCESS_METHOD', None)
		userParams.pop('TOMA_NUM_OF_TRACE_LOGS', None)
		userParams.pop('TOMA_TRACE_LOG_SIZE', None)
		userParams.pop('KAFKA_TLS_ENABLED', None)
		userParams.pop('KAFKA_CA', None)
		userParams.pop('KAFKA_MCS_CERTIFICATE', None)
		userParams.pop('KAFKA_MCS_KEY', None)
		userParams.pop('KAFKA_TOMA_CERTIFICATE', None)
		userParams.pop('KAFKA_TOMA_KEY', None)

		for param in list(userParams.keys()):
			if param.startswith('_'):
				userParams.pop(param)

		hasUserOverride = bool(len(list(userParams.keys())))

		configProfile = {
			'id': nodeParams.get('CONFIG_PROFILE_ID', None),
			'version': nodeParams.get('CONFIG_PROFILE_VERSION', None),
			'name': nodeParams.get('CONFIG_PROFILE_NAME', None),
			'userOverride': hasUserOverride
		}

		return configProfile

def addSysLogHandlerToLogger():
	try:
		from managementCM import OurHandler
		handler = OurHandler(address=SYSLOG_PATH)
		handler.setFormatter(logging.Formatter('%(name)s: %(levelname)s: %(message)s'))
		logger.addHandler(handler)
	except:
		# if couldn't get syslog handler - log to stdout
		addStdoutHandlerToLogger()


def addStdoutHandlerToLogger():
	handler = logging.StreamHandler(sys.stdout)
	handler.setFormatter(logging.Formatter('%(name)s: %(levelname)s: %(message)s'))
	logger.addHandler(handler)


class AgentConfig(object):
	MANAGEMENT_AGENT_SPECIAL_TEAM_RUN_MLNX_AFFINITY = 0
	MANAGEMENT_AGENT_SPECIAL_TEAM_PERIOD = 0
	LOG_TO_STDOUT = False
	loggingLevel = None
	remoteDebug = False


def readConfigFile():
	subprocess.call('/opt/nvmesh/bin/process_config_files')

	CONFIG_FILE = os.path.join(CONFIG_PATH, '.nvmesh.conf')
	include(CONFIG_FILE)

	AgentConfig.MANAGEMENT_AGENT_SPECIAL_TEAM_RUN_MLNX_AFFINITY = nvmesh_conf.get('MANAGEMENT_AGENT_SPECIAL_TEAM_RUN_MLNX_AFFINITY', 0)
	AgentConfig.MANAGEMENT_AGENT_SPECIAL_TEAM_PERIOD = nvmesh_conf.get('MANAGEMENT_AGENT_SPECIAL_TEAM_PERIOD', 0)

	if 'AGENT_LOG_TO_STDOUT' in nvmesh_conf:
		LOG_TO_STDOUT = nvmesh_conf['AGENT_LOG_TO_STDOUT']
		if isinstance(LOG_TO_STDOUT, str) and LOG_TO_STDOUT == 'True':
			addStdoutHandlerToLogger()
		else:
			addSysLogHandlerToLogger()
	else:
		addSysLogHandlerToLogger()

	if 'AGENT_LOGGING_LEVEL' in nvmesh_conf:
		AgentConfig.loggingLevel = nvmesh_conf['AGENT_LOGGING_LEVEL']

		if AgentConfig.loggingLevel == 'DEBUG':
			logger.setLevel(logging.DEBUG)
		else:
			logger.setLevel(logging.WARNING)

	AgentConfig.remoteDebug = 'REMOTE_DEBUG_AGENT' in nvmesh_conf and nvmesh_conf['REMOTE_DEBUG_AGENT'] in ['Yes', 'yes', 'True', 'true']


def validateConfigRequiredParams():
	requiredParams = ['KAFKA_SERVERS']

	for param in requiredParams:
		if param not in nvmesh_conf or nvmesh_conf[param] == '':
			logger.error('{} is not configured in nvmesh.conf, exiting.'.format(param))
			sys.exit(3)


if __name__ == "__main__":
	logger = logging.getLogger('managementAgent')
	readConfigFile()
	clientAgent = ClientControlAgent(logger=logger)
	validateConfigRequiredParams()

	if len(sys.argv) > 1:
		pid = 1
		if 'start' == sys.argv[1]:
			clientAgent.shouldSignalMainProcess = True
			pid = clientAgent.start()
		elif 'stop' == sys.argv[1]:
			clientAgent.stop()
		elif 'restart' == sys.argv[1]:
			pid = clientAgent.restart()
		elif 'run' == sys.argv[1]:
			pid = clientAgent.run()
		else:
			logger.warning("Unknown command")
			sys.exit(2)
		if pid == 0:
			sys.exit(0)
	else:
		logger.debug("usage: %s start|stop|restart", sys.argv[0])
		sys.exit(2)
