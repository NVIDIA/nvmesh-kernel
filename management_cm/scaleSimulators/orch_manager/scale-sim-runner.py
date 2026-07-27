#!/usr/bin/env python3
import argparse
import copy
import json
import socket
import threading
import yaml
from subprocess import PIPE, Popen, call
from collections.abc import Mapping
import time
import os
import pymongo
import getpass
from pathlib import Path

THIS_HOSTNAME = socket.gethostname()
TARGET_REPORT_PROJECTION = {
	'_id': 0,
	'branch': 1,
	'commit': 1,
	'configProfile': 1,
	'cpu_load': 1,
	'cpu_temp': 1,
	'disks': 1,
	'nics': 1,
	'node_id': 1,
	'node_status': 1,
	'reportID': 1,
	'tomaToken': 1,
	'version': 1,
	'zone': 1
}
VOLUMES_PROJECTION = {
	'chunks.pRaids.uuid': 1,
	'chunks.pRaids.zone': 1,
	'chunks.pRaids.tomaLeader': 1,
	'chunks.pRaids.version.major': 1,
	'chunks.pRaids.version.minor': 1,
	'chunks.pRaids.diskSegments.uuid': 1
}


class Consts:
	CONFIG_DIR = Path.home() / '.scaleSimulator'
	CLIENT_CONFIG_FILE = 'client-config.json'
	TARGET_CONFIG_FILE = 'target-config.json'
	NODE_PERSISTENCY_FILE = 'node-persistency.json'


def createArgParser():
	parser = argparse.ArgumentParser(description='NVMesh Scale Simulator Manager')
	parser.add_argument('-i', '--image', type=str, default="latest", help='The Docker image ID to spawn the containers from')
	parser.add_argument('--n_start', type=int, required=True, help='Containers Starting Index')
	parser.add_argument('--n_end', type=int, required=True, help='Containers End Index')
	parser.add_argument('--debug', action='store_true', default=False)
	parser.add_argument('--logs', action='store_true', default=False)
	parser.add_argument('-c', '--config', type=str, default=os.path.expanduser('~/.scale-sim/.node-config.json'), help='The path to the node-config.json file')
	parser.add_argument('--mongo-uri', type=str, help='The mongoDB URI to load the nodes configuration from')
	parser.add_argument('--port', action='append', type=str, help='port mapping in the format "5678:5678"')

	return parser


def getContainerHostname(n):
	return "scale-{}".format(n)


def runCommand(cmd, debug=False):
	if debug:
		print(cmd)

	p = Popen(cmd, shell=True, stdout=PIPE, stderr=PIPE)
	stdout, stderr = p.communicate()
	return stdout, stderr, p.returncode


def callCommand(cmd, debug=False):
	if debug:
		print(cmd)

	p = call(cmd, shell=True)
	return p


def getLatestImage(debug):
	cmd = "docker image ls --format \"{{.ID}}\" | head -1"
	out, err, code = runCommand(cmd)
	imageID = out.split('\n')[0]

	if debug:
		print("using latest image with id {}".format(imageID))
	return imageID


def readConfig(filepath):
	try:
		with open(filepath) as f:
			if filepath.endswith('.yaml'):
				config = yaml.safe_load(f)
			else:
				config = json.load(f)
			return config
	except Exception as ex:
		print("Error reading config from file %s. Error: %s" % (filepath, ex))
		raise


def updateDict(d, u):
	# updates a dictionary recursively
	# (unlike python Dict.update which will only update top level key values)
	for k, v in u.items():
		if isinstance(v, Mapping):
			d[k] = updateDict(d.get(k, {}), v)
		else:
			d[k] = v
	return d


def getConfigPerNode(globalConfig, i):
	conf = copy.deepcopy(globalConfig['baseConfig'])

	if 'nodesConfig' in globalConfig:
		nodesConfigs = globalConfig['nodesConfig']

		if len(nodesConfigs) >= i:
			updateObj = nodesConfigs[i - 1]
			updateDict(conf, updateObj)

	return conf


def spawnContainers(args, globalConfig):
	threads = []
	for i in range(args.n_start, args.n_end + 1):
		specificNodeConfig = getConfigPerNode(globalConfig, i)
		hostname = specificNodeConfig.get('hostname') or getContainerHostname(i)

		t = threading.Thread(target=thread_function, args=(hostname, specificNodeConfig))
		threads.append(t)
		t.start()

		if i % 100 == 0:
			for t in threads:
				t.join()
			time.sleep(5)


def prepareConfigDir(hostname, changeDirOwner=False):
	hostConfigDir = '{0}/{1}'.format(Consts.CONFIG_DIR, hostname)
	if not os.path.isdir(hostConfigDir):
		# create config directory for this container
		callCommand('sudo mkdir -p {0}'.format(hostConfigDir))

	if changeDirOwner:
		# change owner of the config directory
		callCommand('sudo chown -R {user} {dir}'.format(user=getpass.getuser(), dir=hostConfigDir))


def thread_function(hostname, config):
	prepareConfigDir(hostname, changeDirOwner=True)

	# -d will cause the container to run in 'background' mode otherwise the stdout will be printed to the terminal
	d_flag = '' if args.logs else '-d'

	rm_flag = '' if args.debug else '--rm'

	port_maps = ''
	if args.port:
		for p in args.port:
			port_maps = '-p ' + p

	configString = json.dumps(config, separators=(',', ':'))
	# --net=host lets the container use the host networking
	# -v /dev/log:/dev/log (removed)- mount host /dev/log as /dev/log inside the container - meaning all logs should be written to the host
	# -v /etc/localtime:/etc/localtime:ro will sync the localtime on the container to the hosts localtime, this is important for the statistics sim
	# -v /var/opt/scaleSimulator/{hostname}/:/var/opt/scaleSimulator/ this will mount the dir for the client-config.json file
	# -h $name - set hostname
	# --name $name - set an alias / name for the container
	# --rm - remove container when stopped
	# --env - injecting environment variables
	# --log-driver (removed) in order to configure logging driver --log-driver syslog --log-opt tag=\"scaleSim {{{{.Name}}}}\"

	cmd = "docker run {d_flag} \
		--net=host \
		-v {config_dir}/{hostname}/:/var/opt/scaleSimulator/ \
		-v /etc/localtime:/etc/localtime:ro \
		-h {hostname} \
		--name {hostname} \
		{rm_flag} \
		--env 'NODE_CONFIG={config}' \
		{port_maps} \
		{image}".format(
		config_dir=Consts.CONFIG_DIR,
		hostname=hostname,
		config=configString,
		d_flag=d_flag,
		rm_flag=rm_flag,
		port_maps=port_maps,
		image=args.image
	)

	if args.debug:
		print(cmd)
	else:
		# prevent printing the container id to stdout
		cmd += " > /dev/null"

	code = callCommand(cmd)
	succeeded = code == 0
	msg = "spwaned" if succeeded else "error spawning"
	print("{host}: {action} {container_name}".format(host=THIS_HOSTNAME, action=msg, container_name=hostname))


def findFromMongoDB(collection='', query={}, projection={}, uri=''):
	mongoClient = pymongo.MongoClient(uri)
	db = mongoClient['management']
	col = db[collection]
	return list(col.find(query, projection))


def createContainerPersistencies(hostname='', nicsIDs=[], disksIDs=[]):
	persistencies = {'nics': nicsIDs, 'disks': disksIDs}

	with open(Consts.CONFIG_DIR + '/' + hostname + '/' + Consts.NODE_PERSISTENCY_FILE, 'w') as f:
		f.write(json.dumps(persistencies))


def createContainerTargetConfig(hostname='', targetReport={}, nicsIDs=[], disksIDs=[], pRaids={}):
	print('createContainerTargetConfig')
	control = []
	initialState = {'nics': nicsIDs, 'disks': disksIDs, 'targetReport': targetReport, 'pRaids': pRaids}
	targetConfig = {'control': control, 'initialState': initialState}

	with open(Consts.CONFIG_DIR + '/' + hostname + '/' + Consts.TARGET_CONFIG_FILE, 'w') as f:
		f.write(json.dumps(targetConfig))


def removeFileIfExisting(path):
	try:
		if os.path.exists(path):
			os.remove(path)
	except Exception as e:
		print('ERROR while removing file if existing, path: {}, ex: {}'.format(path, e))


def createContainerConfigs(hostname='', target={}, volumes=[], config={}):
	prepareConfigDir(hostname, changeDirOwner=True)

	pRaids = convertVolumesToPRaidsFormat(volumes=volumes, target=target, hostname=hostname, config=config)
	nicsIDs = list(map(lambda x: x['nicID'], target['nics']))
	disksIDs = list(map(lambda x: x['diskID'], target['disks']))

	createContainerPersistencies(hostname=hostname, nicsIDs=nicsIDs, disksIDs=disksIDs)
	createContainerTargetConfig(hostname=hostname, targetReport=target, nicsIDs=nicsIDs, disksIDs=disksIDs, pRaids=pRaids)

	removeFileIfExisting(Consts.CONFIG_DIR + '/' + Consts.CLIENT_CONFIG_FILE)


def createContainerConfigsFromMongo(args=None, config={}):
	try:
		volumes = findFromMongoDB(collection='volume', projection=VOLUMES_PROJECTION, uri=args.mongo_uri)
	except Exception as e:
		print('ERROR while looking for volumes in DB, ex: {}'.format(e))

	for i in range(args.n_start, args.n_end + 1):
		hostname = getContainerHostname(i)

		try:
			targetDocument = findFromMongoDB(collection='server', query={'node_id': hostname}, projection=TARGET_REPORT_PROJECTION, uri=args.mongo_uri)[0]
		except Exception as e:
			print('ERROR while looking for {} in DB, ex: {}'.format(hostname, e))
			continue

		createContainerConfigs(hostname=hostname, target=targetDocument, volumes=volumes, config=config)


def isLeader(hostname, config={}):
	hostnameNumber = int(len(hostname.split('.')[0].split('-')) == 2 and hostname.split('.')[0].split('-')[1])

	if not hostnameNumber:
		return False

	isLeaderChosenManually = 'nodesConfig' in config and len(config['nodesConfig']) >= hostnameNumber and 'TOMA' in config['nodesConfig'][hostnameNumber-1] and 'isLeader' in config['nodesConfig'][hostnameNumber-1]['TOMA'] and config['nodesConfig'][hostnameNumber-1]['TOMA']['isLeader']
	isLeaderFromAutoLeaderElection = hostnameNumber == 1 and 'baseConfig' in config and 'TOMA' in config['baseConfig'] and 'autoLeaderElection' in config['baseConfig']['TOMA'] and config['baseConfig']['TOMA']['autoLeaderElection']

	return isLeaderChosenManually or isLeaderFromAutoLeaderElection


def convertVolumesToPRaidsFormat(volumes={}, target={}, hostname='', config={}):
	pRaids = {}
	zone = target.get('zone')

	if not isLeader(hostname, config=config):
		return pRaids

	try:
		for volume in volumes:
			for chunk in volume['chunks']:
				for pRaid in chunk['pRaids']:
					if pRaid['zone'] == zone:
						diskSegments = {}

						for diskSegment in pRaid['diskSegments']:
							diskSegments[diskSegment['uuid']] = {
								'segmentID': diskSegment['uuid'],
								'status': 'normal',
								'vitality': 'up'
							}

						pRaids[pRaid['uuid']] = {
							'uuid': pRaid['uuid'],
							'diskSegments': diskSegments,
							'pRaidMinorVersion': pRaid['version']['minor'],
							'pRaidMajorVersion': pRaid['version']['major']
						}

	except Exception as e:
		print('ERROR- while convertVolumesToPRaidsFormat: {}'.format(e))
		print('ERROR - using empty pRaids')
		pRaids = {}

	finally:
		return pRaids


if __name__ == "__main__":
	parser = createArgParser()
	args = parser.parse_args()

	config = readConfig(args.config)

	if args.mongo_uri:
		createContainerConfigsFromMongo(args=args, config=config)

	if args.image == "latest":
		args.image = getLatestImage(debug=args.debug)

	spawnContainers(args, config)
