#!/usr/bin/env python3

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0


import random
import shutil
import socket
import string
import subprocess
from subprocess import PIPE
import json
import sys
import os
import time
import signal
import traceback

import errno

components = {
	'ManagementAgent': '/cm/scaleSimulators/agent.py',
	'MCS': '/cm/managementCM.py',
	'Client': '/cm/scaleSimulators/client.py',
	'Target': '/cm/scaleSimulators/target.py',
	'TOMA': '/cm/scaleSimulators/toma.py',
	'UPGRADE_AGENT': '/cm/scaleSimulators/upgradeAgent.py',
}

CONFIG_DIR = '/var/opt/scaleSimulator/'
CLIENT_CONFIG_FILE = 'client-config.json'
AGENT_CONFIG_FILE = 'agent-config.json'
TARGET_CONFIG_FILE = 'target-config.json'
UPGRADE_AGENT_CONFIG_FILE = 'upgrade-agent-config.json'
NODE_CONFIG_FILE = 'node-config.json'
NODE_PERSISTENCY_FILE = 'node-persistency.json'

def log(msg):
	print("init_node.py: %s" % msg)
	sys.stdout.flush()

def runCommand(cmd, debug=False):
	if debug:
		log("running: %s" % cmd)
	p = subprocess.Popen(cmd, shell=True, stdout=PIPE, stderr=PIPE)
	stdout, stderr = p.communicate()
	if debug:
		log("exit code: %d" % p.returncode)
		log("stdout: %s" % stdout)
		log("stderr: %s" % stderr)
	return p.returncode, stdout, stderr

def callCommand(cmd, debug=False):
	if debug:
		log("running: %s" % cmd)
	returncode = subprocess.call(cmd, shell=True)
	if debug:
		log("exit code: %d" % returncode)
	return returncode

def loadJsonFromFile(filename):
	log("Loading json from %s" % filename)
	with open(filename, 'r') as fp:
		return json.load(fp)

def saveJsonToFile(obj, filename):
	with open(filename, 'w+') as fp:
		json.dump(obj, fp, sort_keys=True, indent=4, separators=(',', ': '))

def readConfig():
	log("Loading config from env var NODE_CONFIG")
	josnConfigString = os.environ['NODE_CONFIG']
	config = json.loads(josnConfigString)
	return config

def isComponentEnabled(name, config):
	return name in config and config[name]['enabled']

def startClientSim():
	if isComponentEnabled('CLIENT', config):
		startComponent("Client")

def startTargetSim():
	if not isComponentEnabled('TARGET', config):
		return

	startComponent("Target")

def startTomaSim():
	if not isComponentEnabled('TOMA', config):
		return
	return startComponent("TOMA")

def startCM():
	return startComponent("MCS")

def startManagementAgent():
	return startComponent("ManagementAgent")

def startUpgradeAgent():
	return startComponent("UPGRADE_AGENT")

def startComponent(name, componentArgs=''):
	pid = getComponentPid(name)
	if pid:
		log("Component {} is already running PID: {}".format(name, pid))
		return

	log("Starting {} with args: {}".format(name, componentArgs))

	startCmd = 'start'
	fullCommand = 'python3 {command} {start} {args} &'.format(command=components[name], start=startCmd, args=componentArgs)
	return callCommand(fullCommand, debug=True)

def stopComponent(componentName, sig=signal.SIGTERM):
	try:
		pid = getComponentPid(componentName)
		if pid:
			os.kill(pid, sig)
		else:
			log("{} is not running.".format(componentName))
	except OSError as ex:
		log("Error Killing {} with pid {}. ex: {}".format(componentName, pid, ex))

def getComponentPid(componentName):
	componentCmd = components[componentName]

	psCommand = 'ps -ef | grep -v grep | grep %s | head -n 1 | awk \'{ print $2 }\'' % componentCmd
	p = subprocess.Popen(psCommand, stdout=PIPE, stderr=PIPE, shell=True)
	stdout, stderr = p.communicate()
	if not stdout:
		return None
	try:
		pid = int(stdout)
		return pid
	except ValueError as ex:
		log("Error getting PID for {}. ex: {}".format(componentName, ex))
		log("cmd: {} stdout: {} stderr: {}".format(psCommand, stdout, stderr))

	return None

def restartComponent(componentName, args=None):
	stopComponent(componentName)
	time.sleep(2)
	startComponent(componentName, args)

def readNvmeshConfFile():
	log("Reading config File")
	g = {}
	variables = {}

	def include(filename):
		if os.path.exists(filename):
			exec(open(filename).read(),g, variables)

	include('/etc/nvmesh/nvmesh.conf')

	for key, value in variables.items():
		log("{}={}".format(key, value))

def checkComponentsStatus():

	def checkIfProcessIsRunning(name):
		returnCode, stdout, stderr = runCommand('ps -ef | grep -v grep | grep {} | wc -l'.format(name))
		#log('returnCode: %d stdout: %s stderr: %s' % (returnCode, stdout, stderr))
		return int(stdout) >= 1

	for component, filepath in components.items():
		if isinstance(filepath, list):
			# search for any of the options
			filepath = '-E "{}"'.format('|'.join(filepath))

		isRunning = checkIfProcessIsRunning(filepath)
		log('{name:<30} [{status}]'.format(name=component, status='ON' if isRunning else 'OFF'))

def setupPaths():
	varOptNvmeshPath = CONFIG_DIR + 'var/opt/nvmesh'
	etcNvmesh = CONFIG_DIR + '/etc/nvmesh/'

	makedirs_exist_ok(varOptNvmeshPath)

	os.symlink(varOptNvmeshPath, '/var/opt/nvmesh')

	subprocess.check_call('mkdir -p /var/opt/nvmesh/block_devices_configuration', shell=True)

	## create hidden nvmesh conf
	hidden_conf_dir = '/etc/nvmesh/configs/nvmeibc/'
	os.makedirs(hidden_conf_dir)
	shutil.copy('/etc/nvmesh/nvmesh.conf', os.path.join(hidden_conf_dir, '.nvmesh.conf'))

def makedirs_exist_ok(path):
	try:
		os.makedirs(path)
	except OSError as e:
		if e.errno != errno.EEXIST:
			raise

def setNetworkingAndDNS(hostname):
	subprocess.call('echo "127.0.0.1 localhost {}" > /etc/hosts'.format(hostname), shell=True)
	subprocess.call('cat /etc/hosts', shell=True)

def getManagementAddress(config):
	if 'managementServers' in config:
		return config['managementServers']
	else:
		log("No 'managementServers' found in config. using localhost")
		return 'localhost:4001'

def setManagementAddress(config):
	def setServers(serverType, servers):
		log("Configuring nvmesh.conf with {0}_SERVERS=\"{1}\"".format(serverType, servers))
		subprocess.call('echo "\n{0}_SERVERS=\\"{1}\\"" >> /etc/nvmesh/nvmesh.conf'.format(serverType, servers), shell=True)

	mgmtServers = getManagementAddress(config)
	setServers(serverType='MANAGEMENT', servers=mgmtServers)

def getKafkaAddress(config):
	if 'kafkaServers' in config:
		return config['kafkaServers']
	else:
		log("No 'kafkaServers' found in config. using localhost:9092")
		return 'localhost:9092'

def setKafkaAddress(config):
	kafkaServers = getKafkaAddress(config)
	log("Configuring nvmesh.conf with KAFKA_SERVERS=\"{}\"".format(kafkaServers))
	subprocess.call('echo "\nKAFKA_SERVERS=\\"{}\\"" >> /etc/nvmesh/nvmesh.conf'.format(kafkaServers), shell=True)

def generateID(size=12, chars=string.ascii_lowercase + string.digits):
	return ''.join([random.choice(chars) for i in range(size)])

def setNvmeshConfVariables(config):
	if 'nvmeshConf' in config:
		nvmeshConfVars = config['nvmeshConf']
		addToNvmeshConf = ''

		for (key, value) in nvmeshConfVars.items():
			addToNvmeshConf += '\n{}="{}"'.format(key, value)

		print('adding to nvmesh.conf:')
		print(addToNvmeshConf)
		with open('/etc/nvmesh/nvmesh.conf', 'a') as fp:
			fp.write(addToNvmeshConf)

def loadNodePersistency(config):
	nodePersistencyFilePath = '{0}{1}'.format(CONFIG_DIR, NODE_PERSISTENCY_FILE)
	if os.path.isfile(nodePersistencyFilePath):
		# load persistent data
		with open(nodePersistencyFilePath) as fp:
			nodePersistency = json.load(fp)
			log("loaded node persistency: {}".format(nodePersistency))
			if 'nics' in nodePersistency:
				config['nics'] = nodePersistency['nics']
			if 'disks' in nodePersistency:
				config['disks'] = nodePersistency['disks']

			log("updated config: {}".format(config))

	# save persistency file anyway
	nodePersistency = {
		'nics': config['nics'],
		'disks': config['disks']
	}

	with open(nodePersistencyFilePath, 'w') as fp:
		json.dump(nodePersistency, fp)

def createVirtualHardwareIds(config):
	if not 'disks' in config:
		numOfDisks = config['numOfDisks']
		disksUUID = generateID(size=12, chars=string.ascii_uppercase + string.digits)
		diskIds = [ "{}.{}".format(disksUUID, index) for index in range(numOfDisks) ]
		config['disks'] = diskIds

	if not 'nics' in config:
		numOfNics = config['numOfNics']
		nicsUUID = generateID(size=11, chars=string.hexdigits)
		prefix = "fe8"
		nicHexSize = 32
		nicsIds = []
		for i in range(numOfNics):
			numOfZeros = nicHexSize - (len(prefix) + len(nicsUUID) + len(str(i)))
			zeroes = '0' * numOfZeros
			nicId = ''.join(["0x", prefix, zeroes, nicsUUID, str(i)])
			nicsIds.append(nicId)

		config['nics'] = nicsIds

def createDefaultConfigFiles(config):
	# create default client config file
	clientConfigFile = '{0}{1}'.format(CONFIG_DIR, CLIENT_CONFIG_FILE)

	if os.path.isfile(clientConfigFile):
		clientConfig = loadJsonFromFile(clientConfigFile)
	else:
		clientConfig = {'initialState': {'attachments': {}}, 'controls': []}

	# add all other config variables from the user node-config.baseConfig.CLIENT
	clientConfig['initialState'].update(config['CLIENT'])
	saveJsonToFile(clientConfig, clientConfigFile)

	# create default target config file
	targetConfigFile = '{0}{1}'.format(CONFIG_DIR, TARGET_CONFIG_FILE)
	if os.path.isfile(targetConfigFile):
		targetConfig = loadJsonFromFile(targetConfigFile)
	else:
		targetConfig = {
			'initialState': {
				'disks': config['disks'],
				'nics': config['nics'],
			},
			'controls': []
		}
	# add all other config variables from the user node-config.baseConfig.TOMA
	targetConfig['initialState'].update(config['TOMA'])
	saveJsonToFile(targetConfig, targetConfigFile)

	# create default upgrade agent config file
	upgradeAgentConfigFile = '{0}{1}'.format(CONFIG_DIR, UPGRADE_AGENT_CONFIG_FILE)
	if os.path.isfile(upgradeAgentConfigFile):
		upgradeAgentConfig = loadJsonFromFile(upgradeAgentConfigFile)
	else:
		upgradeAgentConfig = {'initialState': {}, 'controls': []}

	# add all other config variables from the user node-config.baseConfig.UPGRADE_AGENT
	upgradeAgentConfig['initialState'].update(config['UPGRADE_AGENT'])
	saveJsonToFile(upgradeAgentConfig, upgradeAgentConfigFile)

	# create default management agent config file
	agentConfigFile = '{0}{1}'.format(CONFIG_DIR, AGENT_CONFIG_FILE)

	if os.path.isfile(agentConfigFile):
		agentConfig = loadJsonFromFile(agentConfigFile)
	else:
		agentConfig = {'initialState': {}, 'controls': []}

	# add all other config variables from the user node-config.baseConfig.CLIENT
	agentConfig['initialState'].update(config['AGENT'])
	saveJsonToFile(agentConfig, agentConfigFile)

	# create default node config file
	nodeConfigFile = '{0}{1}'.format(CONFIG_DIR, NODE_CONFIG_FILE)
	nodeConfig = { 'configurationProfile': {}, 'controls': [] }

	log("Creating node-config file with: {}".format(nodeConfig))

	saveJsonToFile(nodeConfig, nodeConfigFile)

def shutDownNode(signum, frame):
	log("Simulator Node Shutting Down..")
	# TODO: send SIGTERM to all components, they might have special handling for that in the future.
	exit(0)

def readCommandFromFile(signum, frame):
	nodeConfigFile = '{0}{1}'.format(CONFIG_DIR, NODE_CONFIG_FILE)
	log("Reading Node Command from {}".format(nodeConfigFile))

	try:
		config = None
		with open(nodeConfigFile) as configFile:
			config = json.load(configFile)

		for control in config['controls']:
			handleControl(control)

		# clear commands
		config['controls'] = []

		# save empty commands back to file
		with open(nodeConfigFile, 'w+') as configFile:
			json.dump(config, configFile)

	except Exception as ex:
		log("Error reading command from file")
		traceback.print_exc()

def handleControl(cmd):
	componentName = cmd['component']
	action = cmd['action']
	args = cmd.get('args', '')

	component = components.get(componentName, None)
	if not component:
		log("Unknown Component Name %s" % componentName)
		log("Please Choose from the following Options: %s" % components.keys())
		return

	if action == 'start':
		startComponent(componentName, args)
	elif action == 'stop':
		stopComponent(componentName)
	elif action == 'kill':
		stopComponent(componentName, sig=signal.SIGKILL)
	elif action == 'restart':
		restartComponent(componentName, args)
	else:
		log("Unknown action %s" % action)

if __name__ == '__main__':
	signal.signal(signal.SIGTERM, shutDownNode)
	signal.signal(signal.SIGINT, shutDownNode)
	signal.signal(signal.SIGUSR1, readCommandFromFile)

	hostname = socket.gethostname()
	log("Initializing node {}".format(hostname))
	config = readConfig()

	#setupPaths()
	setNetworkingAndDNS(hostname)
	setManagementAddress(config)
	setKafkaAddress(config)
	setNvmeshConfVariables(config)
	createVirtualHardwareIds(config)
	loadNodePersistency(config)
	createDefaultConfigFiles(config)

	# start Components
	startCM()
	startManagementAgent()
	startClientSim()
	startTargetSim()
	startTomaSim()
	startUpgradeAgent()

	checkComponentsStatus()

	log("Initializing node finished")

	while True:
		try:
			# collect the defunct processes (we are pid 1, it's our responsibility)
			pid, status = os.wait()
		except OSError as ex:
			if ex.errno == errno.ECHILD:
				log("No more child processes !")
			time.sleep(5)
