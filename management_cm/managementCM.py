#!/usr/bin/env python3
import json
import queue
import subprocess
import select
import struct
import socket, errno
import signal
import sys
import os
import logging.handlers
import time
import uuid
from timeit import default_timer as timer
import re
import time
import shutil

from CMSocket import CMSocket, FileSocket, NvmeshUMSocket, process_scheme
from CMSocket import JsonSocket
from confluent_kafka import Producer, Consumer, TopicPartition
from confluent_kafka.admin import AdminClient, NewTopic
from CMSocket import getKafkaAddresses
from daemon import Daemon
from Mailbox import MessageTypes

KAFKA_UNKNOWN_TOPIC = 3
VERBOSE_LOG_LEVEL = 9
MESSAGES_TO_CONSUME = 5
CLIENT_INSTANCES_CHECK_INTERVAL = 5
MCS_MANAGEMENT_TIMEOUT = 300
EMPTY_GUID = '00000000-0000-0000-0000-000000000000'
TLS_CERTS_DIR = '/var/run/nvmesh/tls/nvmeshcm'

schemePath = '/opt/nvmesh/client-repo/management_cm/clnt/'


POLL_PERIOD_BEFORE_FIRST_CONNECTION_SEC = 0.01
SELECT_TIMEOUT_SEC = 2.0
WAIT_FOR_MCS_PROC_TIMEOUT_SECS = 10

class ManagementCM(Daemon):
	def __init__(self, pidfile, lockfile, logger):
		self.producer = None
		self.consumer = None
		self.consumerId = None
		self.producerId = None
		self.needsReload = False
		self.isReloadingKafkaConnections = False
		self.kafkaOutbox = queue.Queue()
		self.writeList = []
		self.procListener = {}
		self.logger = logger
		self.closing = False
		self.shouldClose = False
		self.concurrentConnections = {}
		self.managementTopic = 'default.management.priority.1.0.0'
		self.hostname = socket.gethostname()
		self.clientTopic = 'client.main'
		self.topicsToSubscribeOn = { '{0}.managementAgent.main.1.0.0'.format(self.hostname): False, '{0}.{1}.1.0.0'.format(self.hostname, self.clientTopic) : False }
		self.consumableTopicsInitiated = False
		self.cacheFolder = '/var/opt/nvmesh/mcs/'
		self.kafkaAdminClient = None
		self.offsetRegistry = {}
		# starting from python 3.5 select.select will not return once signal is received
		# to go around that without waiting the whole timeout, we use a pipe
		self.select_wakeup_p_read, self.select_wakeup_p_write = os.pipe()
		self.readList = [self.select_wakeup_p_read]

		try:
			Daemon.__init__(self, pidfile, lockfile, notifyMainProcessOnStartup=True)
		except Exception as e:
			print(("Unknown exception at Deamon init Exception:{0}".format(e)))
			raise

	def stopSignalHandler(self, signum, frame):
		self.logger.debug("Received close signal, exiting. PID {}".format(os.getpid()))
		self.closing = True
		os.close(self.select_wakeup_p_write) # wakeup select

	def handleSIGHUP(self, signum, frame):
		self.logger.debug(f'Received SIGHUP signal - reloading certificates and Kafka connections')
		if not self.isReloadingKafkaConnections:
			self.needsReload = True
		else:
			self.logger.warning("Reload already in progress, ignoring SIGHUP")

	def reloadConfig(self, signum, frame):
		self.logger.debug("Received reload config signal.")
		readConfigFile()

		self.logger.debug("Setting LogLevel to {}".format(CMConfig.logLevelName))
		self.logger.setLevel(CMConfig.logLevel)
		self.logger.debug("Setting log verbose type to {}".format(list(CMConfig.logVerboseTypes)))

	def handleSocketErrorException(self, exception, cmSocket):
		if exception.args[0] in [errno.EWOULDBLOCK, errno.EAGAIN]:
			self.logger.debug("Non-fatal error, continuing")
			self.logger.debug("Non-fatal error, continuing. Exception: %s", exception)
		else:
			self.logger.debug("Socket closed! %s", exception)
			cmSocket.close()

	def createPath(self, path):
		try:
			os.makedirs(path)
		except OSError as exc:
			if exc.errno == errno.EEXIST and os.path.isdir(path):
				pass
			else:
				raise

	def createFileSocket(self, fd, jsonScheme, ftype):
		return FileSocket(
			filePath=fd,
			jsonScheme=jsonScheme,
			readList=self.readList,
			writeList=self.writeList,
			concurrentConnections=self.concurrentConnections,
			logger=self.logger,
			procListener=self.procListener,
			ftype=ftype,
			kafkaOutbox=self.kafkaOutbox
		)

	def createNvmeshUMSocket(self, cmSocket, jsonScheme, ftype):
		return NvmeshUMSocket(
			cmSocket=cmSocket,
			jsonScheme=jsonScheme,
			readList=self.readList,
			writeList=self.writeList,
			concurrentConnections=self.concurrentConnections,
			logger=self.logger,
			ftype=ftype,
			kafkaOutbox=self.kafkaOutbox
		)

	def removePath(self, path):
		try:
			if os.path.exists(path):
				os.remove(path)
		except Exception as e:
			self.logger.debug('Failed to remove path {0}, ex: {1}'.format(path, e))

	def createNvemshUMListener(self):
		listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
		self.removePath(path=udsPathClient)
		return CMSocket(listener, self.readList, self.writeList, self.logger, self.concurrentConnections, udsPathClient, 5)

	def createJsonListener(self):
		jsonSocketPath = '/var/run/nvmesh/json_uds'
		jsonListener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
		self.removePath(path=jsonSocketPath)
		return CMSocket(jsonListener, self.readList, self.writeList, self.logger, self.concurrentConnections, jsonSocketPath, 5)

	def onProducerError(self, err, producerId):
		self.logger.debug(f"Kafka producer {producerId} error: {err}")

		if self.isSSLRelatedError(err):
			if producerId == self.producerId:
				self.needsReload = True

	def initKafkaProducer(self, conf):
		self.logger.debug('Initiating Kafka producer using the address: {0}'.format(CMConfig.kafkaAddresses))
		producerId = uuid.uuid4().hex[:8]
		self.producerId = producerId

		producer_conf = conf.copy()
		producer_conf['error_cb'] = lambda err: self.onProducerError(err, producerId)
		producer_conf['retries'] = 5
		self.producer = Producer(producer_conf)

	def isSSLRelatedError(self, err):
		errorMessage = str(err)
		return "SSL" in errorMessage or "certificate" in errorMessage

	def onConsumerError(self, err, consumerId):
		self.logger.debug(f"Kafka consumer {consumerId} error: {err}")

		if self.isSSLRelatedError(err):
			if consumerId == self.consumerId:
				self.needsReload = True

	def initKafkaConsumer(self, conf):
		self.logger.debug('Initiating Kafka consumer using the address: {0}'.format(CMConfig.kafkaAddresses))
		consumerId = uuid.uuid4().hex[:8]
		self.consumerId = consumerId

		conf['group.id'] = 'MCS_{0}'.format(socket.gethostname())
		conf['error_cb'] = lambda err: self.onConsumerError(err, consumerId)
		self.consumer = Consumer(conf)

	def validateOutgoingTopicsExists(self, conf):
		self.kafkaAdminClient = AdminClient(conf)
		while not self.closing:
			try:
				topics = self.kafkaAdminClient.list_topics(timeout=5).topics
				if self.managementTopic in topics:
					self.logger.debug('Done validating outgoing topics!')
					break

				self.logger.debug(f'{self.managementTopic} topic doesn\'t exist. Is the management running?')
				time.sleep(5)

			except Exception as e:
				self.logger.debug(f"Failed to validate outgoing topics: {e}")
				self.logger.debug(f"Reloading kafka admin client...")
				self.kafkaAdminClient = None
				time.sleep(5)
				self.kafkaAdminClient = AdminClient(conf)

	# subscribe on newly created incoming topics (when new topic appear we need to subscribe again on all existing topics)
	def subscribeOnIncomingTopicsToConsume(self):
		topicsToSubscribeOn = []
		try:
			currentTopics = self.kafkaAdminClient.list_topics(timeout=5).topics
		except Exception as e:
			self.logger.debug(f"Failed to list topics: {e}")
			return topicsToSubscribeOn

		allTopicsInitiated = True
		shouldSubscribe = False

		for topic in self.topicsToSubscribeOn:
			if topic not in currentTopics:
				allTopicsInitiated = False
			else:
				topicsToSubscribeOn.append(topic)
				if not self.topicsToSubscribeOn[topic]:
					# found new topic to subscribe on
					shouldSubscribe = True
					self.topicsToSubscribeOn[topic] = True

		if shouldSubscribe:
			self.consumer.subscribe(topicsToSubscribeOn)
			if allTopicsInitiated:
				self.consumableTopicsInitiated = True

		return topicsToSubscribeOn

	def copyCertificates(self):
		"""
		Copy TLS certificates to runtime directory.
		"""
		certConfigKeys = ['cert', 'key', 'CA']

		# Verify that the certificate files exist
		for certConfigKey in certConfigKeys:
			certFile = getattr(CMConfig, certConfigKey)
			if not certFile:
				self.logger.error(f'Config key {certConfigKey} is missing')
				sys.exit(1)

			if not os.path.exists(certFile):
				self.logger.error(f'Certificate file {certFile} does not exist')
				sys.exit(1)

		# Copy certificates to runtime directory
		try:
			if not os.path.exists(TLS_CERTS_DIR):
				self.logger.debug(f'Creating TLS certificates directory: {TLS_CERTS_DIR}')
				os.makedirs(TLS_CERTS_DIR, mode=0o700, exist_ok=True)

			srcCertFiles = [CMConfig.cert, CMConfig.key, CMConfig.CA]

			for srcCertFile in srcCertFiles:
				destCertFile = os.path.join(TLS_CERTS_DIR, os.path.basename(srcCertFile))
				shutil.copy2(srcCertFile, destCertFile)
				os.chmod(destCertFile, 0o600)

			self.logger.debug(f'Successfully copied TLS certificates to TLS certificates directory {TLS_CERTS_DIR}')

		except Exception as e:
			self.logger.error(f'Failed to copy certificates: {e}')
			raise

	def getKafkaSSLConfig(self):
		kafkaSslCertFilePath = os.path.join(TLS_CERTS_DIR, os.path.basename(CMConfig.cert))
		kafkaSslKeyFilePath = os.path.join(TLS_CERTS_DIR, os.path.basename(CMConfig.key))
		kafkaSslCaFilePath = os.path.join(TLS_CERTS_DIR, os.path.basename(CMConfig.CA))

		conf = {
			'security.protocol': 'SSL',
			'enable.ssl.certificate.verification': 'true',
			'ssl.certificate.location': kafkaSslCertFilePath,
			'ssl.key.location': kafkaSslKeyFilePath,
			'ssl.ca.location': kafkaSslCaFilePath,
		}

		if CMConfig.keyPass:
			conf['ssl.key.password'] = CMConfig.keyPass

		return conf

	def getKafkaConfig(self):
		mandatoryConfigsForTLS = [CMConfig.CA, CMConfig.cert, CMConfig.key]

		if CMConfig.TLSEnabled and None in mandatoryConfigsForTLS:
			self.logger.error('Failed to get Kafka Configs. Partial mandatory configs found for TLS.')
			sys.exit(1)

		conf = {
			'bootstrap.servers': ','.join([':'.join(serverPortTuple) for serverPortTuple in CMConfig.kafkaAddresses])
		}

		if CMConfig.TLSEnabled:
			sslConf = self.getKafkaSSLConfig()
			conf.update(sslConf)

		return conf

	def initKafka(self):
		conf = self.getKafkaConfig()
		self.logger.info('Initiating Kafka connection')
		done = False
		tries = 0

		while not done and tries < 10:
			try:
				self.validateOutgoingTopicsExists(conf)
				self.initKafkaProducer(conf)

				conf['enable.auto.commit'] = 'false'

				self.initKafkaConsumer(conf)
				done = True
			except Exception as e:
				self.logger.error('Failed to initiate kafka connection. Error: {0}'.format(e))
			finally:
				if not done:
					self.logger.debug('Failed to initiate kafka try #{0}. sleeping and retrying'.format(tries + 1))
					time.sleep(5)
				tries += 1

		if not done:
			self.logger.error('Kafka connection could not be established, exiting')
			sys.exit(1)

	def generateACK(self, messageToken):
		return {
			'messageType': 'confirmedDelivery',
			'messageTypeVersion': 1,
			'opcode': MessageTypes.DELIVERY_DONE,
			'payload': {
				'messageToken': messageToken
			}
		}

	def sendDeliveryDoneMessage(self, message):
		originID = message['originID']

		if originID in self.concurrentConnections:
			connection = self.concurrentConnections[originID]

			ackMessage = json.dumps(self.generateACK(message['messageToken'])).encode('utf-8')
			msgLength = struct.pack("I", len(ackMessage))

			try:
				connection.send(msgLength + ackMessage, self.writeList, True)
			except (socket.error, IOError) as e:
				connection.handleSocketErrorException(e)

	def flushOutbox(self):
		def deliveryDone(err, msg):
			message = json.loads(msg.value())

			if err:
				self.logger.warning('Produce message failed! error: %s message: %s', err, message)
			else:
				self.logger.debug('Produce done for message: %s', message)

				if message['originType'] == 'CLIENT' and 'messageToken' in message and message['messageToken'] != EMPTY_GUID:
					self.logger.debug('deliveryDone for message from client, sending ACK for messageToken: %s', message['messageToken'])
					self.sendDeliveryDoneMessage(message)

		messagesInQueue = self.kafkaOutbox.qsize()

		if messagesInQueue > 0:
			self.logger.debug('Got {} messages to send to Kafka'.format(messagesInQueue))

			while self.kafkaOutbox.qsize() > 0:
				messageToSend = self.kafkaOutbox.get()
				self.logger.debug('Sending the following message to Kafka: {}'.format(messageToSend.payload))
				self.producer.produce(self.managementTopic, value=messageToSend.payload.decode('utf-8'), on_delivery=deliveryDone)

			self.logger.debug('There are {} messages in producer, waiting to be send - Flushing the producer'.format(len(self.producer)))
			self.producer.flush(5)
			self.logger.debug('Done flushing')

	def sendResponseToSender(self, kafkaMessage):
		message = json.loads(kafkaMessage.value())
		origin = message["originID"]
		self.logger.debug(f"Consumed message {message}, origin {origin}")

		commitOnCompletion = lambda: self.registerOffsets(kafkaMessage, True)

		if origin in self.concurrentConnections:
			connection = self.concurrentConnections[origin]
			del message["originID"]

			if "messageID" in message:
				del message["messageID"]

			msg = json.dumps(message).encode('utf-8')
			msgLength = struct.pack("I", len(msg))

			try:
				connection.send(msgLength + msg, self.writeList, True, commitOnCompletion)
			except (socket.error, IOError) as e:
				connection.handleSocketErrorException(e)
		else:
			if not origin == "CLIENT":
				self.logger.debug("Got response for someone, but he disconnected. discarding message.")
				commitOnCompletion()
			else:
				self.logger.debug("Got message to the client but he\'s not connected, rewinding offsets to re-transmit messages")
				self.rewindKafkaOffsets()

	def rewindKafkaOffsets(self):
		assignedPartitions = self.consumer.assignment()
		clientPartitions = [tp for tp in assignedPartitions if self.clientTopic in tp.topic]

		if clientPartitions:
			committedOffsets = self.consumer.committed(clientPartitions)

			for offset in committedOffsets:
				self.consumer.seek(offset)

	def commitOffsets(self, topic, partition):
		offsetToCommit = None
		offsets = list(self.offsetRegistry[topic][partition]['offsets'].keys())
		offsets.sort()

		for offset in offsets:
			if self.offsetRegistry[topic][partition]['offsets'][offset]:
				offsetToCommit = offset
			else:
				break

		if not offsetToCommit:
			return self.logger.debug('I\'ve being called to commit offsets to topic:partition {}:{} but nothing could be committed'.format(topic, partition))

		topicPartition = TopicPartition(topic, partition, offsetToCommit+1)

		try:
			self.logger.debug('Committing offsets to topic:partition {}:{} with offset: {}'.format(topic, partition, offsetToCommit+1))
			self.consumer.commit(offsets=[topicPartition], asynchronous=False)
		except Exception as e:
			self.logger.error('Failed to commit offsets for topic:partition {}:{} with offset: {}. {}'.format(topic, partition, offsetToCommit, e))

	def registerOffsets(self, message, canBeCommitted=False):
		topic = message.topic()
		partition = message.partition()
		offset = message.offset()

		if topic not in self.offsetRegistry:
			self.offsetRegistry[topic] = {}

		if partition not in self.offsetRegistry[topic]:
			self.offsetRegistry[topic][partition] = {
				'offsets': {}
			}

		self.offsetRegistry[topic][partition]['offsets'][offset] = canBeCommitted

		if canBeCommitted:
			self.commitOffsets(topic, partition)

	def consumeTopics(self):
		shouldContinue = True

		if self.kafkaAdminClient is not None and not self.consumableTopicsInitiated:
			subscribedTopics = self.subscribeOnIncomingTopicsToConsume()
			if not len(subscribedTopics):
				return

		while shouldContinue:
			messages = self.consumer.consume(MESSAGES_TO_CONSUME, timeout=0)

			if len(messages):
				for message in messages:
					error = message.error()
					if not error:
						self.registerOffsets(message)
						self.logger.debug('Got a message to send to someone msg: {}'.format(json.loads(message.value())))
						self.sendResponseToSender(message)
					else:
						self.logger.debug('Consumer.consume error occurred. code:{} error:{}'.format(error.code(), error.str()))

			else:
				shouldContinue = False

	def closeKafkaConnection(self):
		if self.producer:
			self.producer.flush(5)

		if self.consumer:
			self.consumer.close()

	def reloadKafkaConnections(self):
		self.logger.debug(f"Reloading TLS Certificates & Kafka connections...")
		self.isReloadingKafkaConnections = True

		self.logger.debug("Closing Kafka connections...")
		self.closeKafkaConnection()

		# Copy certificates on reload if TLS is enabled
		if CMConfig.TLSEnabled:
			self.logger.debug("Reloading TLS certificates...")
			self.copyCertificates()

		# Reload Kafka connections
		self.logger.debug("Starting Kafka connections...")

		conf = self.getKafkaConfig()
		self.kafkaAdminClient = AdminClient(conf)
		self.initKafkaProducer(conf)
		conf['enable.auto.commit'] = 'false'
		self.initKafkaConsumer(conf)

		self.needsReload = False
		self.isReloadingKafkaConnections = False

	def checkAndPerformReload(self):
		if self.needsReload and not self.isReloadingKafkaConnections:
			self.reloadKafkaConnections()

	def startRemoteDebugServer(self, port):
		try:
			import debugpy
			try:
				debugpy.listen(('0.0.0.0', port))
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

	def tryConnectKernelClient(self, clientPath: str, schemePathL: str = schemePath) -> bool:
		"""Try to connect to the client process via its proc file."""
		if CMConfig.nvmeshUMClient:
			return True

		if self.closing:
			self.logger.info("got signal to close before connecting to client proc file, exiting")
			return True

		self.logger.debug("Trying to connect to client proc file %s", clientPath)

		try:
			if os.path.exists(clientPath) and not self.closing:
				self.logger.debug("Creating pseudo socket for client proc fd.")
				self.createFileSocket(
					fd=clientPath, jsonScheme=os.path.join(schemePathL, "clnt_scheme.json"), ftype='c'
				)
				self.logger.debug("connected to client")
				return True
		except IOError:
			self.logger.exception("Failed to create proc listener")

		return False

	def waitForClient(self, clientPath: str, schemePathL: str = schemePath) -> bool:
		"""Wait for the client process to create its proc file."""
		if CMConfig.nvmeshUMClient:
			return True

		self.logger.debug("Waiting for client proc file %s", clientPath)
		start_time = time.time()
		while not os.path.exists(clientPath) and not self.closing:
			if time.time() - start_time > WAIT_FOR_MCS_PROC_TIMEOUT_SECS:
				self.logger.error("Timeout waiting for client proc file %s. will try to connect later.", clientPath)
				return False

			time.sleep(POLL_PERIOD_BEFORE_FIRST_CONNECTION_SEC)

		self.logger.debug("Client proc file %s found=%d closing=%d", clientPath, os.path.exists(clientPath), self.closing)

		return self.tryConnectKernelClient(clientPath, schemePathL)

	def run(self):
		self.logger.debug("Starting MCS")

		signal.signal(signal.SIGINT, self.stopSignalHandler)
		signal.signal(signal.SIGTERM, self.stopSignalHandler)
		signal.signal(signal.SIGABRT, self.stopSignalHandler)
		signal.signal(signal.SIGUSR1, self.reloadConfig)
		signal.signal(signal.SIGHUP, self.handleSIGHUP)
		logger.info("Registered to signals")

		# Copy TLS certificates to runtime directory if TLS is enabled
		if CMConfig.TLSEnabled:
			self.copyCertificates()

		# singaling the main process so it can exit and let systemd know that the service has fully started
		if self.notifyMainProcessOnStartup:
			self.signalMainProcessOnStartup()

		clientCache = os.path.join(self.cacheFolder, 'CLIENT')
		tomaCache = os.path.join(self.cacheFolder, 'TOMA')

		self.createPath(clientCache)
		self.createPath(tomaCache)

		kafkaServersDefined = CMConfig.kafkaAddresses

		if not kafkaServersDefined:
			self.logger.error('nvmesh.conf is missing \'KAFKA_SERVERS\', thus a connection to KAFKA can not be established.')
			sys.exit(1)
		else:
			self.initKafka()

		jsonListener = self.createJsonListener()
		errList = [jsonListener]

		if CMConfig.nvmeshUMClient:
			self.logger.debug('Creating nvmeshUM listener')
			nvmeshUMListener = self.createNvemshUMListener()
			errList += [nvmeshUMListener]
		else:
			self.logger.debug('Communicating with nvmesh client kernel object over proc file')
			clientFD = os.path.join(procPathClient, "mcs")

		Timeout = SELECT_TIMEOUT_SEC

		if CMConfig.remoteDebug:
			self.startRemoteDebugServer(port=5678)
			CMConfig.remoteDebug = False

		clientFD = os.path.join(procPathClient, "mcs")

		kernelClientConnected = self.waitForClient(clientFD)

		while not self.shouldClose:
			try:
				start = timer()

				if self.closing:
					socketsToClose = []

					for socketID in self.concurrentConnections:
						socketsToClose.append(self.concurrentConnections[socketID])

					self.closeKafkaConnection()

					# closing all sockets that needs to be closed
					for sock in socketsToClose:
						sock.close()

					self.shouldClose = True
					continue

				self.checkAndPerformReload()
				self.flushOutbox()
				self.consumeTopics()

				readable, writable, errored = select.select(self.readList, self.writeList, errList, Timeout)

				end = timer()
				timePassed = end - start
				isTimeout = False
				if readable or writable:
					Timeout -= timePassed
					if Timeout <= 0:
						isTimeout = True
						Timeout = SELECT_TIMEOUT_SEC
				else:
					isTimeout = True
					Timeout = SELECT_TIMEOUT_SEC

				for s in readable:
					# Accept JSON path connections
					if s is jsonListener:
						connection, address = s.accept()
						JsonSocket(connection, self.readList, self.writeList, self.concurrentConnections, self.kafkaOutbox, self.logger)
					elif CMConfig.nvmeshUMClient and s is nvmeshUMListener:
						self.logger.debug('accepting mcs NvmeshUMSocket')
						connection, address = s.accept()
						self.createNvmeshUMSocket(cmSocket=connection, jsonScheme=os.path.join(schemePath, "clnt_scheme.json"), ftype='c')
						self.logger.debug('created mcs NvmeshUMSocket')
					elif any([isinstance(s, socketType) for socketType in [JsonSocket, FileSocket, NvmeshUMSocket]]):
						try:
							s.receive()
						except (socket.error, IOError) as e:
							self.handleSocketErrorException(e, s)

				for s in writable:
					try:
						CMSocket.send(s, writeList=self.writeList)
					except (socket.error, IOError) as e:
						self.handleSocketErrorException(e, s)

				# Timeout
				if isTimeout and not kernelClientConnected:
					kernelClientConnected = self.tryConnectKernelClient(clientFD)

			except select.error as e:
				if e.errno == errno.EINTR:
					self.logger.debug("Received EINTR, retrying...")
				else:
					self.logger.exception("Received Exception %s, retrying...", e.__class__.__name__)

		os.close(self.select_wakeup_p_read)
		self.logger.info("Exiting..")

	def getSchemeVersion(self) -> str:
		packer = process_scheme.Packer(os.path.join(schemePath, "clnt_scheme.json"), logger=None)
		print (hex(packer.scheme_version))


class OurHandler(logging.handlers.SysLogHandler):

	def __init__(self, address):
		logging.handlers.SysLogHandler.__init__(self, address=address)
		self.CONTINUATION_STR = "..."
		self.MAX_MSG = 4096 - len(self.CONTINUATION_STR)


	def emit(self, record):
		"""
		Emit a record.
		The record is formatted, and then sent to the syslog server. If
		exception information is present, it is NOT sent to the server.
		"""
		msg = self.format(record) + '\000'
		"""
		We need to convert record level to lowercase, maybe this will
		change in the future.
		"""
		prio = '<%d>' % self.encodePriority(self.facility,
											self.mapPriority(record.levelname))
		# Message is a string. Convert to bytes as required by RFC 5424
		#if type(msg) is str:
		#	msg = msg.encode('utf-8')

		msg_ = msg

		ij = 0
		nParts = len(msg_) / self.MAX_MSG
		for msg in [msg_[i:self.MAX_MSG + i] for i in range(0, len(msg_), self.MAX_MSG)]:
			ij += 1
			msg = prio + msg
			if ij < nParts:
				msg += self.CONTINUATION_STR
			if ij > 1:
				msg = record.name + ': ' + msg
			try:
				if self.unixsocket:
					try:
						# self.socket.send("part {1} will log {0}".format(len(msg), ij))
						self.socket.send(msg.encode('utf-8'))
					except socket.error:
						self.socket.close()  # See issue 17981
						self._connect_unixsocket(self.address)
						# self.socket.send("will log {0}".format(len(msg)))
						self.socket.send(msg.encode('utf-8'))
				elif self.socktype == socket.SOCK_DGRAM:
					self.socket.sendto(msg.encode('utf-8'), self.address)
				else:
					self.socket.sendall(msg.encode('utf-8'))
			except (KeyboardInterrupt, SystemExit):
				raise
			except:
				self.handleError(record)


def addLoggingLevelVerbose():
	logging.addLevelName(VERBOSE_LOG_LEVEL, "VERBOSE")

	def verbose(self, message, *args, **kws):
		if not self.isEnabledFor(VERBOSE_LOG_LEVEL):
			return

		isMessage = False
		#sequences = None
		verboseType = kws.pop('verboseType', '')
		typeWithOpcode = None

		if verboseType == 'message':
			#sequences = (kws.pop('sequences'))
			isMessage = True
			verboseType = kws.pop('fromTo', None)
			opcode = kws.pop('opcode', '-')
			typeWithOpcode = '{0}[{1}]'.format(verboseType, opcode)

		if '*' in CMConfig.logVerboseTypes \
				or verboseType in CMConfig.logVerboseTypes \
				or typeWithOpcode in CMConfig.logVerboseTypes \
				or not verboseType:

			if isMessage:
				json_body = None
				try:
					json_body = json.dumps(message)
				except:
					pass

				verbose_message = 'msg: %s: %s.' % (typeWithOpcode, json_body or message)
				#if sequences:
					#verbose_message += ' connectionSequence: {}, messageSequence: {}'.format(sequences[0], sequences[1])
			else:
				verbose_message = '%s: %s.' % (verboseType, message)

			# Yes, logger takes its '*args' as 'args'.
			self._log(VERBOSE_LOG_LEVEL, verbose_message, args, **kws)

	logging.Logger.verbose = verbose


def getLogger():
	addLoggingLevelVerbose()
	logger = logging.getLogger('managementCM')

	if CMConfig.logToStdout:
		handler = logging.StreamHandler(sys.stdout)
	else:
		handler = OurHandler(address=SYSLOG_PATH)

	handler.setFormatter(logging.Formatter('%(name)s: %(levelname)s: %(message)s'))
	logger.addHandler(handler)
	logger.setLevel(CMConfig.logLevel)
	logger.debug("VERBOSE logging enabled for {}".format(list(CMConfig.logVerboseTypes)))
	return logger


def readBashFile(filename):
	g = {}
	l = {}

	if os.path.exists(filename):
		exec(compile(open(filename, "rb").read(), filename, 'exec'), g, l)

	return l

def is_service_enabled(service_name):
	logger = logging.getLogger('managementCM')
	try:
		result = subprocess.check_output(['systemctl', 'is-enabled', service_name], stderr=subprocess.STDOUT)
		output = result.decode().strip()
		return output == 'enabled'
	except subprocess.CalledProcessError as e:
		logger.debug("Error: {}".format(e.output.decode().strip()))
		return False
	except OSError as e:
		logger.debug("OS Error: {} - {}".format(e.errno, e.strerror))
		return False
	except Exception as e:
		logger.debug("Unexpected error: {}".format(str(e)))
		return False

def readConfigFile():
	subprocess.call('/opt/nvmesh/bin/process_config_files')

	filepath = '/etc/nvmesh/configs/nvmeibc/.nvmesh.conf'
	configFile = readBashFile(filepath)

	loggingLevelToNumber = {
		"VERBOSE": VERBOSE_LOG_LEVEL,
		"DEBUG": logging.DEBUG,
		"INFO": logging.INFO,
		"WARNING": logging.WARNING,
		"ERROR": logging.ERROR,
	}

	CMConfig.remoteDebug = 'REMOTE_DEBUG_MCS' in configFile and configFile['REMOTE_DEBUG_MCS'] in ['Yes', 'yes', 'True', 'true']

	if 'MCS_LOG_TO_STDOUT' in configFile:
		logToStdout = configFile["MCS_LOG_TO_STDOUT"]
		CMConfig.logToStdout = bool(logToStdout)

	if 'MCS_LOGGING_LEVEL' in configFile:
		logLevelString = configFile["MCS_LOGGING_LEVEL"]
		if logLevelString in list(loggingLevelToNumber.keys()):
			CMConfig.logLevel = loggingLevelToNumber[logLevelString]
			CMConfig.logLevelName = logLevelString

	if 'MCS_LOGGING_VERBOSE_TYPES' in configFile:
		verboseTypesSet = parseLoggingVerboseTypes(configFile["MCS_LOGGING_VERBOSE_TYPES"])
		CMConfig.logVerboseTypes = verboseTypesSet

	if 'MCS_MANAGEMENT_TIMEOUT' in configFile:
		global MCS_MANAGEMENT_TIMEOUT
		MCS_MANAGEMENT_TIMEOUT = int(configFile["MCS_MANAGEMENT_TIMEOUT"])

	# get 'KAFKA_ADDRESSES'
	CMConfig.kafkaAddresses = getKafkaAddresses(configFile)

	# get TLS configs
	CMConfig.TLSEnabled = 'KAFKA_TLS_ENABLED' in configFile and configFile['KAFKA_TLS_ENABLED'] in ['Yes', 'yes', 'True', 'true']
	CMConfig.CA = configFile.get('KAFKA_CA')
	CMConfig.cert = configFile.get('KAFKA_MCS_CERTIFICATE')
	CMConfig.key = configFile.get('KAFKA_MCS_KEY')
	CMConfig.keyPass = configFile.get('KAFKA_MCS_KEY_PASSPHRASE')

	if 'MCS_LOG_TO_STDOUT' in configFile:
		logToStdout = configFile["MCS_LOG_TO_STDOUT"]
		CMConfig.logToStdout = bool(logToStdout)

	if is_service_enabled('nvmeshum.service'):
		CMConfig.nvmeshUMClient = True

# CMConfig.MULTI_INSTANCE_ENABLED = 'MULTI_INSTANCE_ENABLED' in configFile and configFile['MULTI_INSTANCE_ENABLED'] == 'Yes'

def parseLoggingVerboseTypes(verboseTypesString):
	allClientTypes = ['CLIENT', 'TARGET', 'TOMA', 'MANAGEMENT_AGENT']
	allClientsToMgmt = set(['{0}>MGMT'.format(client) for client in allClientTypes])
	mgmtToAllClients = set(['MGMT>{0}'.format(client) for client in allClientTypes])
	verboseTypesSet = set([eventType.strip() for eventType in verboseTypesString.split(',')])

	if '*' in verboseTypesSet:
		verboseTypesSet = verboseTypesSet.union(allClientsToMgmt).union(mgmtToAllClients)
	if '*>MGMT' in verboseTypesSet:
		verboseTypesSet = verboseTypesSet.union(allClientsToMgmt)
	if 'MGMT>*' in verboseTypesSet:
		verboseTypesSet = verboseTypesSet.union(mgmtToAllClients)

	itemsToRemove = set()
	itemsToAdd = set()
	pattern = re.compile(r"^(.*)\[(\d+(;\d+)*)\]$")

	for item in verboseTypesSet:
		if '<>' in item:
			itemsToRemove.add(item)
			match = pattern.match(item)
			opcodesPart = ''
			if match:
				item = match.groups()[0]
				opcodesPart = '[{}]'.format(match.groups()[1])
			components = item.split('<>')
			itemsToAdd.add('{0}>{1}{2}'.format(components[0], components[1], opcodesPart))
			itemsToAdd.add('{0}>{1}{2}'.format(components[1], components[0], opcodesPart))
			itemsToRemove.add(item)

	verboseTypesSet = verboseTypesSet.union(itemsToAdd)
	verboseTypesSet = verboseTypesSet.difference(itemsToRemove)
	itemsToAdd = set()

	for item in verboseTypesSet:
		match = pattern.match(item)
		if match:
			components = match.groups()[0]
			opcodes = match.groups()[1].split(';')
			for opcode in opcodes:
				itemsToAdd.add('{0}[{1}]'.format(components, opcode))
			itemsToRemove.add(item)
	verboseTypesSet = verboseTypesSet.union(itemsToAdd)
	verboseTypesSet = verboseTypesSet.difference(itemsToRemove)
	return verboseTypesSet


class CMConfig(object):
	kafkaAddresses = None
	logLevelName = "WARNING"
	logLevel = logging.WARNING
	logVerboseTypes = set()
	logToStdout = False
	nvmeshUMClient = False
	MULTI_INSTANCE_ENABLED = False
	remoteDebug = False

 	# TLS configs for Kafka
	TLSEnabled = False
	CA = None
	cert = None
	key = None
	keyPass = None

if __name__ == "__main__":
	SYSLOG_PATH = '/dev/log'
	PID_DIR = '/var/run/nvmesh'
	PID_FILE = os.path.join(PID_DIR, 'managementCM.pid')
	LOCK_FILE = os.path.join(PID_DIR, 'managementCM.lock')
	udsPathClient = '/tmp/mcs'
	procPathClient = '/proc/nvmeibc/mcs'
	procPathServer = '/proc/nvmeibs/mcs'
	# procPathMC = '/proc/{clientInstName}/mcs/mcs'

	if not (len(sys.argv) == 2 and sys.argv[1] in ['--version', '-v']): #readConfigFile not needed in this case, we only want print the version
		readConfigFile()

	logger = getLogger()

	if not os.path.exists(PID_DIR):
		try:
			os.mkdir(PID_DIR)
		except OSError as e:
			print(("Creation of the PID directory {0} failed with err: {1}".format(PID_DIR, e)))
			sys.exit(2)

	if len(sys.argv) >= 3:
		schemePath = sys.argv[2]
	else:
		schemePath = '/opt/nvmesh/client-repo/management_cm/clnt/'

	managementCM = ManagementCM(PID_FILE, LOCK_FILE, logger)


	if len(sys.argv) >= 2:
		pid = 1
		if 'start' == sys.argv[1]:
			pid = managementCM.start()
		elif 'stop' == sys.argv[1]:
			managementCM.stop()
		elif 'restart' == sys.argv[1]:
			pid = managementCM.restart()
		elif 'run' == sys.argv[1]:
			pid = managementCM.run()
		elif sys.argv[1] in ['--version', '-v']:
			managementCM.getSchemeVersion()
		else:
			logger.warning("Unknown command")
			sys.exit(2)
		if pid == 0:
			sys.exit(0)
	else:
		logger.debug("usage: %s start|stop|restart", sys.argv[0])
		sys.exit(2)
	sys.exit(0)
