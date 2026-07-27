import abc
import time
import os
import sys
import signal
import errno
import logging
import logging.handlers
import socket
import select
import json
import traceback
import datetime

sys.path.append('../')
from CMSocket import CMSocket
from Mailbox import GenericMessage
from consts import Consts, KafkaAutoOffsetReset, Components, Messages
from confluent_kafka import Producer, Consumer, KafkaError, KafkaException, TopicPartition


class Component(object):
	def __init__(self, configFile):
		self.hostname = socket.gethostname()
		self.configFilePath = os.path.join(Consts.CONFIG_DIR, configFile)
		self.shouldContinue = True
		self.readList = []
		self.writeList = []

		self.isKafka = self.getType() == Components.TOMA or self.getType() == Components.UPGRADE_AGENT

		signal.signal(signal.SIGINT, self.exitGracefully)
		signal.signal(signal.SIGTERM, self.exitGracefully)

		handler = logging.StreamHandler(sys.stdout)
		handler.setFormatter(self.getLoggerFormatter())
		self.logger = logging.getLogger(str(self.getLoggerName()))

		self.logger.addHandler(handler)
		self.logger.setLevel(logging.DEBUG)

		if self.isKafka:
			self.bootstrapServers = self.getBoostrapServers()
			self.consumer = None
			self.producer = None

		self.version = self.getVersion().strip()
		self.branch = self.getVersionParam('branch-name').strip()
		self.commit = self.getVersionParam('commit-id').strip()

		self.configurationProfile = {}
		self.keepaliveInterval = self.getKeepaliveInterval()

	def getType(self):
		pass

	def getLoggerFormatter(self, specialAnotiation=''):
		return logging.Formatter('%(asctime)s %(name)s{}: %(levelname)s: %(message)s'.format(specialAnotiation))

	def connect(self):
		self.logger.debug("Connecting to MCS json_uds...")
		cmSocket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
		cmSocket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

		newSocket = CMSocket(cmSocket, self.readList, self.writeList, self.logger)

		while True:
			try:
				cmSocket.connect(Consts.JSON_UDS)
				self.logger.debug("Connected!")
				break
			except Exception as e:
				time.sleep(5)
				self.logger.debug("Failed to connect to json_uds, retrying. Exception: {0}".format(e))

			cmSocket.settimeout(None)

		return newSocket

	def destroyAndReinitializeSocket(self, socket, epoll, exception):
		# destroy
		traceback.print_exc()
		self.logger.debug('destroyAndReinitializeSocket - Socket closed - ex: {}'.format(exception))
		epoll.unregister(socket)
		epoll.close()
		socket.close()

		# reinitialize
		socket, epoll = self.initSocket()
		self.afterSocketInitiated()

		return socket, epoll

	def exitGracefully(self, signal=None, flag=None):
		self.shouldContinue = False

	def sendMessageToMCS(self, messageType, payload):
		message = self.buildGenericMessage(messageType, payload)

		try:
			self.socket.sendMessage(message, self.writeList)
		except Exception as e:
			self.logger.error("Failed to send a message to MCS. Exception: {}".format(e))

	def getInterestEvents(self):
		pass

	def handleMessage(self, message):
		pass

	def sendPeriodicReports(self):
		pass

	def getComponentSpecificMessageHeaders(self):
		pass

	def getMessageSequence(self):
		pass

	def getKeepaliveInterval(self):
		return self.keepaliveInterval if hasattr(self, 'keepaliveInterval') and self.keepaliveInterval else self.defaultKeepaliveInterval

	def updateKeepAliveIntervalIfNeeded(self, keepaliveInterval):
		if keepaliveInterval and keepaliveInterval != self.keepaliveInterval:
			self.logger.debug('Updating keepalive interval from {} to {}'.format(self.keepaliveInterval, keepaliveInterval))
			self.keepaliveInterval = keepaliveInterval

	def buildGenericMessage(self, messageType, payload, messageTypeVersion = 1):
		message = {
			'messageType': messageType,
			'messageTypeVersion': messageTypeVersion,
			'messageSequence': self.getMessageSequence(),
			'originType': self.getType(),
			'keepaliveInterval': self.getKeepaliveInterval(),
			'payload': payload
		}

		message.update(self.getComponentSpecificMessageHeaders())

		return message if self.isKafka else GenericMessage(self.socket, message, self.logger)

	def initSocket(self):
		jsonUDS = self.connect()
		self.socket = jsonUDS
		epoll = select.epoll()
		epoll.register(jsonUDS, select.EPOLLIN)

		return jsonUDS, epoll

	def afterSocketInitiated(self):
		# Override this method in Specific Component
		# to run any initializations that require the socket
		pass

	def doBeforeExit(self):
		pass

	def initKafka(self):
		self.initConsumer()
		self.initProducer()

	def beforeKafkaInitiated(self):
		pass

	def afterKafkaInitiated(self):
		pass

	def getInterestConsumersConfig(self):
		# Override should be with format: [{ topics: ['topic1', 'topic2'], autoOffsetReset: 'earliest'}, ...]
		pass

	def initConsumer(self, resubscribe=False):
		topicsConfig = self.getInterestConsumersConfig()

		conf = {'bootstrap.servers': ','.join(self.bootstrapServers),
				'auto.offset.reset': 'earliest',
				'enable.auto.commit': False,
				'group.id': self.getType() + '_' + self.hostname}

		if resubscribe:
			self.consumer = None
		consumer = Consumer(conf)

		topicsPartitions = map(lambda topic: TopicPartition(topic.get('name'), topic.get('partition')), topicsConfig)

		consumer.assign(list(topicsPartitions))
		self.consumer = consumer

	def initProducer(self):
		conf = {'bootstrap.servers': ','.join(self.bootstrapServers),
				'client.id': 'scaleSimulator'}
		self.producer = Producer(conf)

	def consume(self):
		message = self.consumer.poll(timeout=1.0)
		if message is None:
			return

		try:
			message_value = json.loads(message.value().decode('utf-8'))
			self.logger.debug('got a message: {}'.format(message_value))
		except Exception as e:
			self.logger.error('Failed to decode message! message value: {}, exception: {}'.format(message.value(), e))
			return

		message_type = message_value.get('messageType')

		self.logger.debug('Received a message! topic: {}, type: {}, offset: {}'.format(message.topic(), message_type, message.offset()))

		try:
			messageNotHandled = self.handleMessage(message_value)

			if messageNotHandled:
				self.logger.debug('message not handled! topic: {}, type: {}, offset: {}'.format(message.topic(), message_type, message.offset()))
			else:
				self.consumer.commit(message)

		except Exception as e:
			self.logger.error('Failed to handle message! topic: {}, type: {}, offset: {}, exception: {}, traceback: {}'.format(message.topic(), message_type, message.offset(), e, traceback.format_exc()))
			self.consumer.close()
			self.exitGracefully()

	def produceMessageToTopic(self, message, topic):
		try:
			self.producer.produce(topic, value=json.dumps(message))
			self.producer.flush()  # flush is not raising an exception if the topic does not exist

		except Exception as e:
			self.logger.error('Failed to produce message to topic: {}, message: {}, ex: {}'.format(topic, message, e))
			self.exitGracefully()

	def producePeriodicReports(self):
		pass

	def start(self):
		signal.signal(signal.SIGUSR1, self.handleReadConfigSignal)

		if self.isKafka:
			self.beforeKafkaInitiated()
			self.initKafka()
			self.afterKafkaInitiated()

			while self.shouldContinue:
				self.producePeriodicReports()
				self.consume()

			self.doBeforeExit()

		else:
			try:
				jsonUDS, epoll = self.initSocket()

				self.afterSocketInitiated()

				while self.shouldContinue:
					self.sendAllOutbox()

					try:
						if self.writeList:
							try:
								epoll.modify(jsonUDS, select.EPOLLOUT)
							except IOError as e:
								if e.errno == errno.EINTR:
									self.logger.debug('Received {} while modifying the epoll object, retrying. err: {}'.format(e.errno, e))
									self.destroyAndReinitializeSocket(jsonUDS, epoll, e)
								elif e.errno != errno.EEXIST:
									raise

						events = epoll.poll(1)

						for fileno, event in events:
							if event & select.EPOLLIN:
								received = jsonUDS.receive()
								message = json.loads(''.join(bytearray(received).decode("utf-8")))
								self.logger.debug("{0}: Got {1} message: {2}".format(self.getType(), message.get('messageType'), json.dumps(message)))
								self.handleMessage(message)
							elif event & select.EPOLLOUT:
								CMSocket.send(jsonUDS, writeList=self.writeList)

								if not self.writeList:
									epoll.modify(jsonUDS, select.EPOLLIN)

						self.sendAllOutbox()
						self.sendPeriodicReports()
					except IOError as e:
						if e.errno in [errno.EINTR, errno.EAGAIN]:
							errorName = errno.errorcode[e.errno]
							self.logger.debug("Received {0}, retrying. err: {1}".format(errorName, e))
						else:
							self.logger.debug("Received IOError retrying. err: {0}".format(e))
							jsonUDS, epoll = self.destroyAndReinitializeSocket(jsonUDS, epoll, e)
			# 		except Exception as e:
			# 			self.logger.error('Something bad happened %s', e)
			# 			raise e
			# except Exception as e:
			# 	self.logger.error('Something bad happened %s', e)
			# 	raise e
			finally:
				pass

	def sendAllOutbox(self):
		message = self.socket.getMessageFromOutbox()

		while message:
			if self.socket.sendMessage(message, writeList=self.writeList, sendNextMessage=False):
				message = self.socket.getMessageFromOutbox()
			else:
				message = None

	def stop(self):
		self.shouldContinue = False

	def readConfigFromFile(self, updateInitState=False, configFilePath=None):
		configFilePath = configFilePath or self.configFilePath
		self.logger.info("Reading config from {}".format(configFilePath))

		with open(configFilePath) as configFile:
			config = json.load(configFile)
			if updateInitState:
				self.initState(config['initialState'])
			else:
				# not handling controls on component init.
				self.handleControls(config['controls'])

			if 'initialState' in config and 'logLevel' in config['initialState']:
				level = config['initialState']['logLevel']
				self.logger.info('Setting logLevel to {}'.format(level))
				self.logger.setLevel(level)

			self.updateControls([])

		return config

	def handleReadConfigSignal(self, signum, frame):
		self.logger.info("Received SIGUSR1 reading config from file {}".format(self.configFilePath))
		self.readConfigFromFile()

	@abc.abstractmethod
	def handleControls(self, controls):
		pass

	@abc.abstractmethod
	def initState(self, initialState):
		pass

	def updateInitState(self, initialState):
		with open(self.configFilePath) as configFile:
			config = json.load(configFile)
			config['initialState'] = initialState

		self.writeConfigToFile(config)

	def updateControls(self, controls):
		self.logger.info("Got to updateControls controls:{}".format(controls))

		with open(self.configFilePath) as configFile:
			config = json.load(configFile)
			config['controls'] = controls

		self.writeConfigToFile(config)

	def writeConfigToFile(self, config, configFilePath=None):
		configFilePath = configFilePath or self.configFilePath

		# writing to swap file, then moving it, to avoid atomicity issues
		swapFile = configFilePath + '.swp'
		with open(swapFile, 'w+') as configFile:
			json.dump(config, configFile, sort_keys=True, indent=4, separators=(',', ': '))

		os.rename(swapFile, configFilePath)

	def getLoggerName(self):
		return self.getType()

	def getVersionParam(self, param_name):
		file_path = '/version/%s' % param_name
		try:
			with open(file_path, 'r') as f:
				value = f.readline()
			return value
		except Exception as err:
			return None

	def getVersion(self):
		git_describe = self.getVersionParam('git-describe')
		if git_describe:
			version_parts = git_describe.split('-')[:2]
			return '-'.join(version_parts) + '-SIM'
		else:
			return "no version - sim"

	@staticmethod
	def isTimeForNextMsg(lastMsgTime, interval):
		if not lastMsgTime:
			return datetime.datetime.now()

		return (datetime.datetime.now() - lastMsgTime).seconds > interval

	def readBashFile(self, filename):
		g = {}
		l = {}

		if os.path.exists(filename):
			exec(open(filename).read(),g, l)

		return l

	def getBoostrapServers(self):
		bootstrapServers = None

		filepath = '/etc/nvmesh/nvmesh.conf'
		config = self.readBashFile(filepath)

		if not 'KAFKA_SERVERS' in config:
			self.logger.error('Failed to find KAFKA_SERVERS in nvmesh.conf, exiting')
			sys.exit(1)

		try:
			bootstrapServers = config['KAFKA_SERVERS'].split(',')
		except Exception as e:
			self.logger.error('Failed to parse KAFKA_SERVERS in nvmesh.conf, found: {}, ex: {} , exiting'.format(config['KAFKA_SERVERS'], e))
			sys.exit(1)

		self.logger.debug('Kafka bootstrap servers: {}'.format(bootstrapServers))
		return bootstrapServers

	'''
	Configuration profile implementation (scaleSimulator)

	- self.configurationProfile exists on the component level and is defaulted to {}
	- managementAgent will write to the NODE_CONFIG_FILE file every update to the configuration profile
	- managementAgent will report configuration profile information from the cache
	- client/toma/agent will report configuration profile from the NODE_CONFIG_FILE - file will be read only on component start!
		it should be done using self.getConfigurationProfileFromPersistencies()
	- user override can be simulated via /etc/nvmesh/nvmesh.override.conf - restart component is required
	'''
	def updateConfigurationProfile(self, configurationProfileParameters):
		isUpdated = True
		id, name, version = self.extractConfigurationProfileInformation({}, configurationProfileParameters)

		if id and name and version:
			configurationProfile = self.getConfigurationProfileFromPersistencies()
			configurationProfile.update({ 'id': id, 'name': name, 'version': version })
			self.configurationProfile = configurationProfile

			configFilePath = os.path.join(Consts.CONFIG_DIR, Consts.NODE_CONFIG_FILE)
			config = self.readConfigFromFile(configFilePath=configFilePath)
			config.update({ 'configurationProfile': self.configurationProfile })
			self.writeConfigToFile(config, configFilePath)
		else:
			self.logger.error('Not updating configuration profile! not all of the required params exists {}'.format(configurationProfileParameters))
			isUpdated = False

		return isUpdated

	def getConfigurationProfileFromConfigFile(self):
		config = self.readConfigFromFile(configFilePath=os.path.join(Consts.CONFIG_DIR, Consts.NODE_CONFIG_FILE))
		return config.get('configurationProfile')

	def getConfigurationProfileFromUserOverride(self):
		return self.readBashFile('/etc/nvmesh/nvmesh.override.conf')

	def getConfigurationProfileFromPersistencies(self):
		configurationProfile = {}
		config = self.getConfigurationProfileFromConfigFile()
		override = self.getConfigurationProfileFromUserOverride()
		id, name, version = self.extractConfigurationProfileInformation(config, override)

		configurationProfile = { 'id': id, 'name': name, 'version': version, 'userOverride': bool(override)}

		self.configurationProfile = configurationProfile
		return configurationProfile

	def extractConfigurationProfileInformation(self, config, override):
		id = override.get('CONFIG_PROFILE_ID') or config.get('id', 'no profile')
		name = override.get('CONFIG_PROFILE_NAME') or config.get('name', 'no profile')
		version = override.get('CONFIG_PROFILE_VERSION') or config.get('version', 0)

		return id, name, version
