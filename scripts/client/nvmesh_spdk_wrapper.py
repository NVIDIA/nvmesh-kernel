#!/usr/bin/python2

import getopt
import sys
import os
import logging.handlers
import subprocess
import json

SYSLOG_PATH = '/dev/log'
NVMESH_CONFIG_PATH = '/etc/nvmesh/nvmesh.conf'
NVMESHUM_BIN_PATH = '/opt/nvmesh/nvmeshum/bin/'
NVMESH_SPDK_RPCPY_PATH = '/usr/bin/nvmeshum_spdk_rpc'
BDEV_EMULATION_SCRIPT_PATH = '{nvmeshum_path}nvmeshum_bdev_emu.py'.format(nvmeshum_path=NVMESHUM_BIN_PATH)
ERROR_CODE = 100
CLUSTER_SIZE = 1048576  # 1MiB
BASE_BDEV = 'uring'

# snapshot vars
BDEV_SUFFIX = '_bdev'
LVS_SUFFIX = '_lvs'
LVOL_SUFFIX = '_lvol'
NAMESPACE = 'nvidia'

action = ''
dev_dir = '/dev/nvmesh'
logger = logging.getLogger('nvmesh_spdk_wrapper')
nvmesh_config = {}
snapshot_data = {
	'volumes': {
		'backing': None,
		'data': None,
		'metadata': None
	},
	'data-uuid': None
}

try:
	handler = logging.handlers.SysLogHandler(address=SYSLOG_PATH)
	handler.setFormatter(logging.Formatter('%(name)s: %(levelname)s: %(message)s'))
	logger.addHandler(handler)
except:
	handler = logging.StreamHandler(sys.stdout)
	handler.setFormatter(logging.Formatter('%(name)s: %(levelname)s: %(message)s'))
	logger.addHandler(handler)

logger.setLevel(logging.DEBUG)

class Action(object):
	STATUS = 'status'
	ATTACH_SNAPSHOT = 'attach-snapshot'
	DETACH_SNAPSHOT = 'detach-snapshot'

class RPCErrors(object):
	NO_SUCH_DEVICE = 'No such device'
	BDEV_INUSE = 'Blocked by rpc filter - Used by an active nvlvstore'

def check_root():
	if not os.geteuid() == 0:
		err = 'Script must be run as root'
		logger.error(err)
		raise SnapshotException(err=err)

def convertBytesToMiB(bytes):
	return bytes / (1024 ** 2)

def parse_args():
	global snapshot_data
	global dev_dir
	global action

	if len(sys.argv) > 1:
		action = sys.argv[1]
		argv = sys.argv[2:]

	try:
		opts, args = getopt.getopt(argv, 'b:d:m:i:s:a', ['backing=', 'data=', 'metadata=', 'data-volume-uuid=', 'data-volume-size='])
	except:
		print('usage: nvmesh_spdk_wrapper.py <attach-snapshot/detach-snapshot/status> -b <backing_volume> -d <data_volume> -m <metadata_volume> -i <data_volume_uuid> -s <data_volume size>')
		print(' -b|--backing-volume      backing volume name')
		print(' -d|--data-volume     	 data volume name')
		print(' -m|--metadata-volume     metadata volume name')
		print(' -i|--data-volume-uuid    the snapshot data volume uuid')
		print(' -s|--data-volume-size    the data volume size in bytes')
		sys.exit(2)

	for opt, arg in opts:
		if opt in ('-b', '--backing-volume'):
			snapshot_data['volumes']['backing'] = arg
		if opt in ('-d', '--data-volume'):
			snapshot_data['volumes']['data'] = arg
		if opt in ('-m', '--metadata-volume'):
			snapshot_data['volumes']['metadata'] = arg
		if opt in ('-i', '--data-volume-uuid'):
			snapshot_data['data-uuid'] = arg
		if opt in ('-s', '--data-volume-size'):
			snapshot_data['lvol-size'] = convertBytesToMiB(int(arg))

	if action not in (Action.STATUS, Action.ATTACH_SNAPSHOT, Action.DETACH_SNAPSHOT):
		logger.error('ERROR: Unsupported or missing option: "{0}"'.format(action))
		sys.exit(1)

	if action in [Action.ATTACH_SNAPSHOT, Action.DETACH_SNAPSHOT]:
		err = None
		if snapshot_data['data-uuid'] is None:
			err = 'ERROR: Missing the data volume uuid for {}, please use --data-volume-uuid.'.format(action)
		elif action == Action.ATTACH_SNAPSHOT and snapshot_data['lvol-size'] is None:
			err = 'ERROR: Missing the data volume size in bytes for {}, please use --data-volume-size.'.format(action)
		else:
			missing_vols = reduce(lambda acc, curr: acc + ([curr] if snapshot_data['volumes'][curr] is None else []), list(snapshot_data['volumes'].keys()), [])
			if len(missing_vols):
				err = 'ERROR: Missing the following volumes for {0}: "{1}"'.format(action, ', '.join(missing_vols))

		if err:
			logger.error(err)
			raise SnapshotException(err)

		snapshot_data['lvs-name'] = '{data_volume_name}{lvs_suffix}'.format(data_volume_name=snapshot_data['volumes']['data'], lvs_suffix=LVS_SUFFIX)
		snapshot_data['lvol-name'] = 'lvol'
		snapshot_data['bdevs'] = {vol: '{vol_name}{bdev_suffix}'.format(vol_name=snapshot_data['volumes'][vol], bdev_suffix=BDEV_SUFFIX)
								  for vol in snapshot_data['volumes'].iterkeys()}

def load_nvmesh_config():
	g = {}
	if os.path.exists(NVMESH_CONFIG_PATH):
		execfile(NVMESH_CONFIG_PATH, g, nvmesh_config)
	else:
		err = 'Missing nvmesh config file: {0}'.format(NVMESH_CONFIG_PATH)
		logger.error(err)
		raise SnapshotException(err)

def check_and_update_rpcpy_path():
	def validate_path(path):
		return path in nvmesh_config and nvmesh_config[path] != '' and os.path.exists(nvmesh_config[path])

	global SPDK_RPCY_TOOL_PATH

	if validate_path('SPDK_RPCPY_PATH'):
		SPDK_RPCY_TOOL_PATH = nvmesh_config['SPDK_RPCPY_PATH']
	elif os.path.isfile(NVMESH_SPDK_RPCPY_PATH):
		SPDK_RPCY_TOOL_PATH = NVMESH_SPDK_RPCPY_PATH
	else:
		err = 'Missing SPDK_RPCPY_PATH in /etc/nvmesh/nvmesh.conf'
		logger.error(err)
		raise SnapshotException(err)

def run_command(command):
	if command is None:
		logger.error('Empty command ignoring')
		return False, None

	logger.debug('Subprocess: "%s"', command)

	try:
		process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, shell=True)
		output, _ = process.communicate()

		msg = "exit code: {0} output: {1}".format(process.returncode, output)
		logger.debug(msg)
		if process.returncode != 0:
			return False, output

	except OSError as exception:
		logger.exception('Subprocess failed: {0}'.format(exception))
		return False, exception.message
	else:
		logger.debug('Subprocess finished')

	return True, output

def run_nvmeshum_command(command):
	success, output = run_command(command)

	if not success:
		try:
			output = json.loads(output)
			output = output['result']['error']
		except ValueError:
			pass

	return success, output

def run_spdk_command(command):
	return run_nvmeshum_command('{rpcpy} {cmd}'.format(rpcpy=SPDK_RPCY_TOOL_PATH, cmd=command))

class SnapshotException(Exception):
	def __init__(self, err):
		self.err = err
		super(SnapshotException, self).__init__(self.err)

def attach_snapshot(force=False):
	assert False, "detach_snapshot() should no longer be called through this path"

def detach_snapshot():
	assert False, "detach_snapshot() should no longer be called through this path"

def handle_response(response):
	print(json.dumps(response))
	sys.exit(0 if response['success'] else ERROR_CODE)

def print_lvols_status():
	assert False, "print_lvols_status() should no longer be called through this path"

if __name__ == "__main__":
	response = {'success': True, 'error': None}

	try:
		check_root()
		parse_args()
		load_nvmesh_config()
		check_and_update_rpcpy_path()

		if action == Action.ATTACH_SNAPSHOT:
			attach_snapshot()
		elif action == Action.DETACH_SNAPSHOT:
			detach_snapshot()
		elif action == Action.STATUS:
			print_lvols_status()
			exit(0)
	except SnapshotException as e:
		response['error'] = e.err
	except Exception as e:
		err = e.message
		response['error'] = err

	if response['error']:
		response['success'] = False

	handle_response(response)
