import argparse
import json
import os
import subprocess
from pathlib import Path

CONFIG_DIR = Path.home() / '.scaleSimulator'
CLIENT_CONFIG_FILE = 'client-config.json'
TARGET_CONFIG_FILE = 'target-config.json'
UPGRADE_AGENT_CONFIG_FILE = 'upgrade-agent-config.json'
NODE_CONFIG_FILE = 'node-config.json'

COMPONENT_NAMES = {
	'mcs': 'MCS',
	'agent': 'ManagementAgent',
	'client': 'Client',
	'target': 'Target',
	'toma': 'TOMA',
	'statistics': 'Statistics',
	'upgradeAgent': 'UpgradeAgent'
	}


class ScaleSimControllerParser(object):

	@staticmethod
	def addSubParsers(subparsers):
		ScaleSimControllerParser.addClientParser(subparsers)
		ScaleSimControllerParser.addTargetParser(subparsers)
		ScaleSimControllerParser.addComponentParser(subparsers)
		ScaleSimControllerParser.addUpgradeAgentParser(subparsers)
		return subparsers


	@staticmethod
	def addUpgradeAgentParser(subparsers):
		subParser = subparsers.add_parser('upgradeAgent', help='Control the Upgrade Agent Simulator')
		upgradeAgentSubParsers = subParser.add_subparsers(dest="controlType")
		mockCommandParser = upgradeAgentSubParsers.add_parser('mock-command', help='Mock a command')
		mockCommandParser.add_argument('-n', '--command-name', type=str, required=True, help='The command to mock')
		mockCommandParser.add_argument('-o', '--stdout', type=str, help='The stdout to mock')
		mockCommandParser.add_argument('-e', '--stderr', type=str, help='The stderr to mock')
		mockCommandParser.add_argument('-v', '--exit-code', type=int, required=True, help='The exit code to mock')

		def addContainerArg(parser):
			parser.add_argument('-c', '--container', action=ContainerNameResolver, type=str, required=True, help='the containers on which to issue the command. either hostname (scale-3) or index (3) are accepted')

		addContainerArg(mockCommandParser)


	@staticmethod
	def addClientParser(subparsers):
		subParser = subparsers.add_parser('client', help='Control the Client Simulator')
		clientSubParsers = subParser.add_subparsers(dest="controlType")
		attachParser = clientSubParsers.add_parser('attach', help='Send Attach Volume Command')
		detachParser = clientSubParsers.add_parser('detach', help='Send Detach Volume Command')
		enableIoParser = clientSubParsers.add_parser('enable-io', help='Enable IO for Attachments')
		disableIoParser = clientSubParsers.add_parser('disable-io', help='Disable IO for Attachments')
		setHealthParser = clientSubParsers.add_parser('set-health', help='Set Client Health')

		def addArguments(parser):
			parser.add_argument('volumes', type=str, nargs='+', help='The Volumes IDs to attach / detach')
			parser.add_argument('-c', '--containers', action=ContainerNameResolver, nargs='+', type=str, required=True, help='the container on which to issue the command. either hostname (scale-3) or index (3) are accepted')

		addArguments(attachParser)
		addArguments(detachParser)
		addArguments(enableIoParser)
		addArguments(disableIoParser)

		attachParser.add_argument('-m', '--mode', type=str, default='SHARED_READ_WRITE', choices=['SHARED_READ_ONLY', 'SHARED_READ_WRITE', 'EXCLUSIVE_READ_WRITE'], help='the attachment reservation mode')
		attachParser.add_argument('-p', '--preempt', action='store_true')

		setHealthParser.add_argument('-c', '--containers', action=ContainerNameResolver, nargs='+', type=str, required=True, help='the container on which to issue the command. either hostname (scale-3) or index (3) are accepted')
		setHealthParser.add_argument('health', type=str, help='Client Health as integer or string according to Consts.ClientStatus')

	@staticmethod
	def addTargetParser(subparsers):
		subParser = subparsers.add_parser('target', help='Control the Target (TOMA / Leader) Simulator')
		targetSubParser = subParser.add_subparsers(dest="controlType")
		addDiskParser = targetSubParser.add_parser('add-disk', help='Add Disk')
		removeDiskParser = targetSubParser.add_parser('remove-disk', help='Remove Disk')
		addNicParser = targetSubParser.add_parser('add-nic', help='Add NIC')
		removeNicParser = targetSubParser.add_parser('remove-nic', help='Remove NIC')
		stepUp = targetSubParser.add_parser('step-up', help='TOMA step up as Leader')
		stepDown = targetSubParser.add_parser('step-down', help='TOMA step down as Follower')
		rebuildVolume = targetSubParser.add_parser('rebuild-volume', help='Simulate volume rebuild process')
		praidReport = targetSubParser.add_parser('send-praid-report', help='Initiate pRaid Report (regardless if isLeader or not')
		updateSegmentStatus = targetSubParser.add_parser('update-segment-status', help='Send dead status on given segments')
		setVolumeEncryption = targetSubParser.add_parser('set-encryption', help='Set the encryption process result (success/failure)')

		def addValueArg(parser):
			parser.add_argument('-v', '--value', type=int, required=True, help='the value')

		def addContainerArg(parser):
			parser.add_argument('-c', '--container', action=ContainerNameResolver, type=str, required=True, help='the containers on which to issue the command. either hostname (scale-3) or index (3) are accepted')

		def addArgumentsForAddRemove(parser, diskOrNic, addOrRemove):
			parser.add_argument('ids', type=str, nargs='+', help='The {} IDs to {}'.format(diskOrNic, addOrRemove))
			addContainerArg(parser)

		def addArgumentsForRebuildVolume(parser):
			def maxStrOrInt(string):
				string = string.lower()
				if string == 'max':
					value = string
				else:
					try:
						value = int(string)
					except ValueError:
						raise argparse.ArgumentTypeError("This argument should be an int or the string 'max'")
				return value

			addContainerArg(rebuildVolume)
			parser.add_argument('--volume', type=str, help='The Volume ID to rebuild')
			parser.add_argument('--dbs-clean-rate', type=int, default=4100, help='DBs per second cleaning rate')
			parser.add_argument('--amount-of-dbs-to-clean', type=maxStrOrInt, default='max', help='How many DBs to clean, the default is all the segment.')
			parser.add_argument('--side-under-recovery', type=int, default=0, help='Which segment will be under recovery')
			parser.add_argument('--chunk', type=int, default=0)
			parser.add_argument('--praid', type=int, default=0)
			parser.add_argument('--dbs-status-threshold', type=int, default=0, help='The threshold to stop reporting the remaining DBs')

		def addArgumentsForSendPRaidReport(praidReport):
			addContainerArg(praidReport)
			praidReport.add_argument('--inc-minor', action='store_true', help='causes each pRaid minor version to be incremented before sending the report')

		def addArgumentsForDeadSegmentsReport(updateSegmentStatus):
			addContainerArg(updateSegmentStatus)
			updateSegmentStatus.add_argument('--segment-ids', nargs='*', type=str, help='The segment IDs to update')
			updateSegmentStatus.add_argument('--status', type=str, help='The status to update')

		addArgumentsForAddRemove(addDiskParser, 'Disk', 'add')
		addArgumentsForAddRemove(removeDiskParser, 'Disk', 'remove')
		addArgumentsForAddRemove(addNicParser, 'NIC', 'add')
		addArgumentsForAddRemove(removeNicParser, 'NIC', 'remove')
		addContainerArg(stepUp)
		addContainerArg(stepDown)
		addArgumentsForRebuildVolume(rebuildVolume)
		addArgumentsForSendPRaidReport(praidReport)
		addArgumentsForDeadSegmentsReport(updateSegmentStatus)
		addContainerArg(setVolumeEncryption)
		addValueArg(setVolumeEncryption)

	@staticmethod
	def addComponentParser(subparsers):
		subParser = subparsers.add_parser('component', help='Control a Component / Service within a Simulator Container')
		actionSubParsers = subParser.add_subparsers(dest="action")
		startParser = actionSubParsers.add_parser('start', help='Start Components')
		stopParser = actionSubParsers.add_parser('stop', help='Stop Components')
		restartParser = actionSubParsers.add_parser('restart', help='Restart Components')
		killParser = actionSubParsers.add_parser('kill', help='Kill Components')

		def addParserArgs(parser):
			parser.add_argument('-c', '--containers', nargs='+', action=ContainerNameResolver, type=str, required=True, help='the container on which to issue the command. either hostname (scale-3) or index (3) are accepted')
			parser.add_argument('components', type=str, nargs='+', choices=COMPONENT_NAMES.keys() ,help='The Component to start, stop or restart.')

		addParserArgs(startParser)
		addParserArgs(stopParser)
		addParserArgs(restartParser)
		addParserArgs(killParser)

class UpgradeAgentController(object):
	@staticmethod
	def handleCliCommand(args):
		UpgradeAgentController.createControls('mock-command', args)

	@staticmethod
	def createControls(controlType, args):
		upgradeAgentCmdMgr = UpgradeAgentCommandManager(args.container)

		def createControl(command, exitCode, stdout, stderr):
			return {
				'type': controlType,
				'command': command,
				'exitCode': exitCode,
				'stdOut': stdout,
				'stdErr': stderr
			}

		controls = [createControl(args.command_name, args.exit_code, args.stdout, args.stderr)]
		upgradeAgentCmdMgr.addControls(controls)
		upgradeAgentCmdMgr.sendSigUsr()

class ClientController(object):
	@staticmethod
	def handleCliCommand(args):
		if args.controlType == 'attach':
			ClientController.createVolumeControls('attach', args, additionalKeys={
				'reservationMode': args.mode,
				'preempt': args.preempt })
		elif args.controlType == 'detach':
			ClientController.createVolumeControls('detach', args)
		elif args.controlType == 'enable-io':
			ClientController.createVolumeControls('set-io-enabled', args, additionalKeys={'ioEnabled': True})
		elif args.controlType == 'disable-io':
			ClientController.createVolumeControls('set-io-enabled', args, additionalKeys={'ioEnabled': False})
		elif args.controlType == 'set-health':
			ClientController.createControl('set-health', container=args.containers, additionalKeys={'health': args.health})

	@staticmethod
	def createVolumeControls(controlType, args, additionalKeys=None):

		for container in args.containers:
			clientCmdMgr = ClientCommandManager(container)

			def createControl(volName):
				control = {
					'type': controlType,
					'name': volName
				}

				if additionalKeys:
					for k in additionalKeys:
						control[k] = additionalKeys[k]
				return control

			controls = [createControl(vol) for vol in args.volumes]
			clientCmdMgr.addControls(controls)
			clientCmdMgr.sendSigUsr()

	@staticmethod
	def createControl(controlType, containers, additionalKeys=None):
		for container in containers:
			clientCmdMgr = ClientCommandManager(container)
			control = {
				'type': controlType,
			}

			if additionalKeys:
				for k in additionalKeys:
					control[k] = additionalKeys[k]

			clientCmdMgr.addControls([control])
			clientCmdMgr.sendSigUsr()

class TargetController(object):
	@staticmethod
	def handleCliCommand(args):
		TargetController.createControls(args.controlType, args)

	@staticmethod
	def createControls(controlType, args):
		targetCmdMgr = TargetCommandManager(args.container)

		def createControl(objectID, value):
			control = {
				'type': controlType,
				'id': objectID
			}

			if value is not None:
				control['value'] = value

			return control

		def createRebuildVolumeControl(args):
			return {
				'type': controlType,
				'name': args.volume,
				'dbsCleanRate': args.dbs_clean_rate,
				'amountOfDBsToClean': args.amount_of_dbs_to_clean,
				'chunk': args.chunk,
				'praid': args.praid,
				'sideUnderRecovery': args.side_under_recovery,
				'dbsStatusThreshold': args.dbs_status_threshold
			}

		if hasattr(args, 'ids'):
			controls = [createControl(objID, args.value) for objID in args.ids]
		elif hasattr(args, 'volume'):
			controls = [createRebuildVolumeControl(args)]
		else:
			control = {'type': controlType}
			control.update(vars(args))
			controls = [control]

		targetCmdMgr.addControls(controls)
		targetCmdMgr.sendSigUsr()

class NodeController(object):
	@staticmethod
	def handleCliCommand(args):
		if args.action == 'start':
			NodeController.createControls('start', args)
		elif args.action == 'stop':
			NodeController.createControls('stop', args)
		elif args.action == 'restart':
			NodeController.createControls('restart', args)
		elif args.action == 'kill':
			NodeController.createControls('kill', args)

	@staticmethod
	def createControls(controlType, args):
		for container in args.containers:
			nodeCmdMgr = NodeCommandManager(container)

			def createControl(component):
				return {
					'action': controlType,
					'component': COMPONENT_NAMES[component]
				}

			if hasattr(args, 'components'):
				controls = [ createControl(component) for component in args.components]
			else:
				controls = [{ 'action': controlType }]

			nodeCmdMgr.addControls(controls)
			nodeCmdMgr.sendSigUsr()

class JsonFileCommandManager(object):
	def __init__(self, processNames, configFile, containerName):
		self.configFilePath = os.path.join(CONFIG_DIR, containerName, configFile)
		self.processNames = processNames
		self.containerName = containerName

	def addControls(self, controls):
		with open(self.configFilePath) as configFile:
			config = json.load(configFile)
			config['controls'] = config['controls'] + controls

		self.writeConfigToFile(config)

	def writeConfigToFile(self, config):
		# writing to swap file, then moving it, to avoid atomicity issues
		swapFile = self.configFilePath + '.swp'
		with open(swapFile, 'w+') as configFile:
			json.dump(config, configFile, sort_keys=True, indent=4, separators=(',', ': '))

		os.rename(swapFile, self.configFilePath)

	def sendSigUsr(self):
		pids = []
		for procName in self.processNames:
			# cmd = 'docker top {} | grep " python3/cm/scaleSimulators/{}" | awk \'{{ print $2 }}\''.format(self.containerName, procName)
			cmd = 'docker exec {} ps -ef | grep "python3 /cm/scaleSimulators/{}" | awk \'{{ print $2 }}\''.format(self.containerName, procName)
			pid = runCommandGetOutput(cmd)
			if pid:
				pids.append(pid)

		cmd = 'docker exec {} kill -USR1 {}'.format(self.containerName, ' '.join(pid.decode() for pid in pids))

		# cmd = 'sudo kill -USR1 {}'.format(' '.join(pids))
		runCommand(cmd)


def runCommandGetOutput(cmd):
	child = subprocess.Popen(cmd, stdout=subprocess.PIPE, shell=True)
	stdout = child.communicate()[0]
	return stdout.rstrip()


def runCommand(cmd, debug=False):
	if debug:
		print(cmd)
	exitCode = subprocess.call(cmd, shell=True)
	return exitCode == 0


class UpgradeAgentCommandManager(JsonFileCommandManager):
	def __init__(self, containerName):
		JsonFileCommandManager.__init__(self, ['upgradeAgent.py'], UPGRADE_AGENT_CONFIG_FILE, containerName)


class ClientCommandManager(JsonFileCommandManager):
	def __init__(self, containerName):
		JsonFileCommandManager.__init__(self, ['client.py'], CLIENT_CONFIG_FILE, containerName)


class TargetCommandManager(JsonFileCommandManager):
	def __init__(self, containerName):
		JsonFileCommandManager.__init__(self, ['toma.py', 'leader.py'], TARGET_CONFIG_FILE, containerName)


class NodeCommandManager(JsonFileCommandManager):
	def __init__(self, containerName):
		JsonFileCommandManager.__init__(self, ['node/init_node.py'], NODE_CONFIG_FILE, containerName)


class ContainerNameResolver(argparse.Action):
	# This class is used to convert container index to container hostname during the argparse parsing stage
	# so that args.container will always be the full hostname
	def __call__(self, parser, namespace, value, option_string=None):
		if isinstance(value, list):
			resolvedList = map(ContainerNameResolver.resolveName, value)
			setattr(namespace, self.dest, resolvedList)
		else:
			value = ContainerNameResolver.resolveName(value)
			setattr(namespace, self.dest, value)

	@staticmethod
	def resolveName(fullNameOrIndex):
		if fullNameOrIndex.startswith('scale-'):
			return fullNameOrIndex
		else:
			return 'scale-{}'.format(fullNameOrIndex)