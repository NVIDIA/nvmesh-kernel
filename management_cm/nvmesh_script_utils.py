#!/usr/bin/python3

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

import errno
import os
import glob
import json
import contextlib
import traceback
import fcntl
import time
import sys
import subprocess
import shutil
import re
from functools import reduce

DEFAULT_MODULE_NAME = "nvmeibc"
DEFAULT_DEV_ROOT = "nvmesh"
LOCK_FILE_PATH="/tmp/.{0}_attach_lockfile"
GLOBAL_INSTANCE_LOCK_FILE_PATH="/tmp/.nvmesh_instance_lockfile"
NVMESH_CONFIG_FILE_PATH="/etc/nvmesh/nvmesh.conf"
CLIENT_VOLUME_FOLDER = "/proc/{0}/volumes"
CLIENT_STATUS_FILE = "/proc/{0}/status"
INSTANCE_CTRL_FILE = "/proc/{0}/instctls".format(DEFAULT_MODULE_NAME)
INSTANCES_STATUS_JSON = "/proc/{0}/inst_list.json".format(DEFAULT_MODULE_NAME)
ATOM_STATUS_JSON = "/proc/nvmeiba/status.json"
MAX_ATOM_STATUS_READ = 128*1024*2 # Max file size if 128k it will break if it requires more chars
PERSISTENCY_ROOT_FOLDER = "/var/opt/nvmesh/clnt_instance_configuration/{0}"
DEFAULT_PERSISTENCY_VOLUMES_PATH = "/var/opt/nvmesh/block_devices_configuration"
PERSISTENCY_VOLUMES_PATH = "/var/opt/nvmesh/clnt_instance_configuration/{0}/block_devices_configuration"
DEFAULT_PERSISTENCY_ALIASES_PATH = "/var/opt/nvmesh/block_devices_sub_vols"
PERSISTENCY_ALIASES_PATH = "/var/opt/nvmesh/clnt_instance_configuration/{0}/block_devices_sub_vols"
NVMESH_SPDK_RPCPY_PATH = "/opt/nvmesh/nvmft/spdk/scripts/rpc.py"
PROC_FILE_PATH = "/proc/{0}/cli/cli"
MCS_CACHE_FOLDER = "/var/opt/nvmesh/mcs/"
CONFIGS_PATH = "/etc/nvmesh/configs"
NVMESH_CLIENT_INSTANCE_DO_PATH = "/usr/bin/nvmesh_client_instance_do"
NVMESH_CLI_PATH = "/usr/bin/nvmesh"
BUFFER_SIZE = 4096
RETRY_ERROR_CODE = 3
MAX_VOLUME_NAME = 31
MAX_BLOCK_DEVICE_PATH = MAX_VOLUME_NAME
PREEMPT_FLAG = "--preempt"
IS_SUB_BLOCK_IO_ALLOWED_FLAG = "--512"
SHARED_READ_WRITE ="SHARED_READ_WRITE"
EXCLUSIVE_READ_WRITE ="EXCLUSIVE_READ_WRITE"
SHARED_READ_ONLY ="SHARED_READ_ONLY"
RO_FLAG = "--RO"
EX_FLAG = "--EX"
RW_FLAG = "--RW"
# Client states
NVMEIBC_MOD_STATE_INITIALIZING = 0
NVMEIBC_MOD_STATE_READY = 1
NVMEIBC_MOD_STATE_PREP_RM = 2
NVMEIBC_MOD_STATE_RM_RDY = 3
NVMEIBC_MOD_STATE_EXITING = 4
CLIENT_VALID_STATES = {NVMEIBC_MOD_STATE_INITIALIZING: "Initializing",
                       NVMEIBC_MOD_STATE_READY:        "Ready",
                       NVMEIBC_MOD_STATE_PREP_RM:      "Prepearing for removal",
                       NVMEIBC_MOD_STATE_RM_RDY:       "Zombie, ready to removal",
                       NVMEIBC_MOD_STATE_EXITING:      "Exiting"}
g={}
nvmeshConfigFileParams={}
def include(filename):
    if os.path.exists(filename):
        exec(compile(open(filename, "rb").read(), filename, 'exec'), g, nvmeshConfigFileParams)

include(NVMESH_CONFIG_FILE_PATH)

# A class providing all required utils for the scripts,
class MultiClientUtils(object):
    def __init__(self, logger, multi_client_instance=DEFAULT_MODULE_NAME, dev_root=DEFAULT_DEV_ROOT, json_output=False):
        self.logger = logger
        self.instances_status_json = INSTANCES_STATUS_JSON
        multi_client_instance = multi_client_instance or self.extract_inst_from_dev(dev_root)
        self.instance_name = multi_client_instance
        self.proc_file = PROC_FILE_PATH.format(multi_client_instance)
        self.persistency_root_folder = PERSISTENCY_ROOT_FOLDER
        if multi_client_instance == DEFAULT_MODULE_NAME:
            self.volume_persistency_folder = DEFAULT_PERSISTENCY_VOLUMES_PATH
            self.alias_persistency_folder = DEFAULT_PERSISTENCY_ALIASES_PATH
            self.persistency_root_folder = PERSISTENCY_ROOT_FOLDER.format("")
            if dev_root != DEFAULT_DEV_ROOT:
                logger.debug("Default NVMesh instance should not give a dev_root folder. Using the given root folder {0}.".format(dev_root))
        else:
            self.volume_persistency_folder = PERSISTENCY_VOLUMES_PATH.format(multi_client_instance)
            self.alias_persistency_folder = PERSISTENCY_ALIASES_PATH.format(multi_client_instance)
            self.persistency_root_folder = PERSISTENCY_ROOT_FOLDER.format(multi_client_instance)
            if dev_root is None:
                dev_root = self.get_dev_root_folder()
            if dev_root is None: # No previous lsblk name to use, assume it's the same as multi_client_instance
                dev_root = multi_client_instance
        self.volume_status_folder = CLIENT_VOLUME_FOLDER.format(multi_client_instance)
        self.device_folder = os.path.join("/dev", dev_root)
        self.max_block_device_path = MAX_BLOCK_DEVICE_PATH - len(dev_root) - 1
        self.instance_status_file = CLIENT_STATUS_FILE.format(multi_client_instance)
        self.instance_ctrl_file = INSTANCE_CTRL_FILE
        self.valid_reservation_modes = [SHARED_READ_ONLY, SHARED_READ_WRITE, EXCLUSIVE_READ_WRITE]
        self.lock_file_path = LOCK_FILE_PATH.format(dev_root)
        self.mcs_cache_folder = MCS_CACHE_FOLDER
        self.dev_root = dev_root
        self.json_output = json_output
        self.init_json_response()

    def get_management_db_uuid(self, management_cluster, management_protocol, management_http_port):
        db_uuid = self.run_local_script(script_path=NVMESH_CLI_PATH, args=[
            'get-dbuuid',
            '--mgmt-cluster', management_cluster,
            '--mgmt-protocol', management_protocol,
            '--mgmt-http-port', management_http_port
        ])

        return db_uuid.strip().replace('-', '')

    def create_client_instance(self, instance_configuration):
        if not self.is_instance_active(self.instance_name):
            conf_str = instance_configuration.get_conf_str()
            self.run_local_script(script_path=NVMESH_CLIENT_INSTANCE_DO_PATH, args=['-a', '--already-locked', conf_str])
        elif not self.is_exist_instance_with_mgmt_db_uuid(instance_configuration.management_db_uuid.value, auto_generated=False):
            error_string = 'An instance named {0} is already exist and connected to a different management cluster'.format(self.instance_name)
            self.handle_error_and_exit(error_string=error_string)

    def delete_client_instance(self):
        self.run_local_script(script_path=NVMESH_CLIENT_INSTANCE_DO_PATH, args=['-d', 'INSTANCE_NAME={}'.format(self.instance_name)])

    def run_local_script(self, script_path, args=[], sudo=False):
        if not os.path.exists(script_path):
            self.handle_error_and_exit(error_string='Could not find local script {0}, failed to create client instance.'.format(script_path))
        else:
            cmd = ['sudo'] if sudo else []
            cmd.append(script_path)
            cmd = cmd + args
            return self.execute_local_command(command=cmd)

    def execute_local_command(self, command, shell=False):
        try:
            self.logger.debug('Executing local command: {}'.format(' '.join(command)))
            out = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, shell=shell)
            stdout, stderr = out.communicate()
            self.logger.debug('Returned code {0} after running local command: {1}'.format(out.returncode, ' '.join(command)))
            if stderr or out.returncode != 0:
                self.handle_error_and_exit(error_string=stderr)
            else:
                self.logger.info(stdout)
                return stdout
        except OSError as e:
            self.handle_error_and_exit(error_string='Unable to run command, command: {0}, ex: {1}'.format(command, e))

    def execute_local_pipe_command(self, commandA, commandB):
        if commandA is None or commandB is None:
            self.logger.error("Must have 2 commands for piping")
            return False, None

        self.logger.debug('Subprocess running piped commands: "{0} | {1}"'.format(commandA, commandB))

        try:
            process_a = subprocess.Popen(commandA, stdout=subprocess.PIPE, shell=False)
            process_b = subprocess.Popen(commandB, stdin=process_a.stdout, stdout=subprocess.PIPE, shell=False)
            process_a.stdout.close()

            output, _ = process_b.communicate()
            if process_b.returncode != 0:
                return False, None
        except OSError as exception:
            self.logger.exception('Subprocess failed for piped commands')
            raise
        else:
            self.logger.debug('Subprocess finished for piped commands')

        return True, output

    def verify_ctrl_file_exists(self):
        if os.path.isfile(self.instance_ctrl_file) == False:
            error_string = "Request cannot be executed, as Client does not seem to be running, module is down. ErrorID: 1008; File: {0}".format(self.instance_ctrl_file)
            self.handle_error_and_exit(error_string, error_code=1008)

    def open_ctrl_file(self):
        "Open input file in non-blocking mode"
        return os.open(self.instance_ctrl_file, os.O_RDWR | os.O_NONBLOCK)

    def is_instance_active(self, instance_name):
        try:
            with open(self.instances_status_json, 'r') as status_file:
                status_info = json.load(status_file)
            return any([True for instance in status_info.get("instances") if instance.get("name") == instance_name])
        except:
            return (instance_name == DEFAULT_MODULE_NAME)

    def is_exist_instance_with_mgmt_db_uuid(self, mgmt_db_uuid, auto_generated=True):
        auto_generated_value = "1" if auto_generated else "0"

        try:
            with open(self.instances_status_json, 'r') as status_file:
                status_info = json.load(status_file)
                instances = status_info["instances"]
            return len([instance for instance in instances if instance["management"]["db_uuid"] == mgmt_db_uuid and instance["auto_generated"] == auto_generated_value]) != 0
        except Exception as e:
            self.logger.error('Failed to check if there exist an instance with dbUUID: {0}, ex: {1}'.format(mgmt_db_uuid, e))
            raise

    def is_instance_has_attachments(self):
        has_attachments = True
        try:
            has_attachments = len(self.get_all_volume_names()) != 0
        except Exception as e:
            self.logger.error('Failed to check instance {0} attachments, it will not be deleted even if it has no more volumes attached. Ex: {1}'.format(self.instance_name, e))

        return has_attachments

    @staticmethod
    def get_instances():
        try:
            with open(INSTANCES_STATUS_JSON, 'r') as status_file:
                status_info = json.load(status_file)
                return status_info["instances"]
        except Exception as e:
            raise e

    @staticmethod
    def get_instance_running_index():
        instances = MultiClientUtils.get_instances()
        last_used_index = reduce(lambda last_index, curr_index: last_index if curr_index < last_index else curr_index,
                                 [int(instance["name"].strip("ag")) for instance in [instance for instance in instances if "auto_generated" in instance and instance["auto_generated"] == "1"]]
                                 , 0)
        return last_used_index

    @staticmethod
    def resolve_name_from_db_uuid(management_db_uuid):
        instances = MultiClientUtils.get_instances()
        db_uuid_predicate = lambda instance: "auto_generated" in instance and instance["auto_generated"] == "1" \
                                             and instance and instance["management"]["db_uuid"] == management_db_uuid

        instance_name = [instance["name"] for instance in list(filter(db_uuid_predicate, instances))]
        return instance_name[0] if instance_name else None

    def extract_value_from_ioctl_with_regex(self, regex, instance_name):
        try:
            ioctl = self.read_ioctl(location=PERSISTENCY_ROOT_FOLDER.format(instance_name))
            result = re.search(regex, ioctl)
            return result.group(1)
        except Exception as e:
            self.handle_error_and_exit('Failed to extract value from ioctl. regex: {0}, instance: {1}, ex: {2}'.format(regex, instance_name, e))

    def is_auto_generated(self, instance_name):
        value = self.extract_value_from_ioctl_with_regex(regex='.*auto_gen=([01])|', instance_name=instance_name)
        return value == '1'

    def get_management_db_uuid_from_ioctl(self, instance_name):
        return self.extract_value_from_ioctl_with_regex(regex='.*db_uuid=([0-9a-f]{32})|', instance_name=instance_name)

    def extract_inst_from_dev(self, dev_root):
        try:
            with open(self.instances_status_json, 'r') as status_file:
                status_info = json.load(status_file)
            for instance in status_info.get("instances"):
                if instance.get("dev_dir") == dev_root:
                    return str(instance.get("name"))
        except:
            return None

    @contextlib.contextmanager
    def open_instance_ctrl_file(self):
        self.verify_ctrl_file_exists()
        try:
            f = self.open_ctrl_file()
        except IOError:
            error_string = "Request cannot be executed, as Client does not seem to be running, module is down. ErrorID: 1008; File: {0}".format(self.instance_ctrl_file)
            self.handle_error_and_exit(error_string, error_code=1008)
        try:
            yield f
        finally:
            os.close(f)

    def read_ioctl(self, location):
        try:
            fd = open(location+"/ioctl", 'r')
        except (IOError, OSError) as err:
            if err.errno == 2:
                return None
            else:
                self.logger.debug("Error reading previous persistency ioctl.\n")
                raise
        ioctl = fd.readline()
        fd.close()
        return ioctl

    def read_previous_ioctl(self):
        return self.read_ioctl(self.persistency_root_folder)

    def get_dev_root_folder(self):
        prev_ioctl = self.read_previous_ioctl()
        if prev_ioctl:
            return prev_ioctl.split(',')[1].split('}')[0]
        return None

    def create_persistency(self, ioctl):
        try:
            os.makedirs(self.volume_persistency_folder)
            fd = os.open(self.persistency_root_folder+"/ioctl", os.O_APPEND | os.O_CREAT|os.O_RDWR)
            os.write(fd, ioctl.encode('utf-8'))
            os.close(fd)
            os.makedirs(self.alias_persistency_folder)
        except (IOError, OSError) as err:
            self.logger.error("Error creating persistency folders. Possible corruption to the folder structure. ErrorID: 1044; Client Instance: {0}".format(self.instance_name))
            self.logger.error("Additional info {0}.".format(str(err)))

    def remove_persistency(self):
        try:
            os.rmdir(self.volume_persistency_folder)
            os.rmdir(self.alias_persistency_folder)
            os.remove(self.persistency_root_folder+"/ioctl")
            os.rmdir(self.persistency_root_folder)
        except (IOError, OSError) as err:
            self.logger.error("Error deleting persistency folders. Possible corruption to the folder structure. ErrorID: 1045; Client Instance: {0}".format(self.instance_name))
            self.logger.error("Additional info {0}.".format(str(err)))

    def mark_mcs_cache_to_be_removed(self, instance_name):
        instance_mcs_cache = os.path.join(self.mcs_cache_folder, 'CLIENT_{0}'.format(instance_name))

        try:
            if os.path.exists(instance_mcs_cache):
                os.mknod(os.path.join(instance_mcs_cache, '{0}_clearCache'.format(instance_name)))
        except (IOError, OSError) as err:
            self.logger.error("Error creating a file which indicating to remove the instance's MCS cache. Client Instance: {0}".format(self.instance_name))
            self.logger.error("Additional info {0}.".format(str(err)))

    def remove_configs(self, is_module_conf):
        instance_configs_path = os.path.join(CONFIGS_PATH, DEFAULT_MODULE_NAME if is_module_conf else self.instance_name)
        if is_module_conf:
            instance_configs_path = os.path.join(instance_configs_path, 'nvmesh.instance.conf')

        try:
            if is_module_conf:
                os.remove(instance_configs_path)
            else:
                shutil.rmtree(instance_configs_path)
        except (IOError, OSError) as err:
            self.logger.error("Error deleting config files {0}. Client Instance: {1}".format(instance_configs_path, self.instance_name))
            self.logger.error("Additional info {0}.".format(str(err)))

    def get_all_client_instances(self):
        return [instance for instance in os.listdir(self.persistency_root_folder) if os.path.isdir(os.path.join(self.persistency_root_folder, instance))]

    def read_ioctl_for_instance(self, instance):
        return self.read_ioctl(os.path.join(self.persistency_root_folder, instance))

    def verify_proc_file_exists(self):
        if os.path.isfile(self.proc_file) == False:
            error_string = "Request cannot be executed, as Client does not seem to be running, module is down. ErrorID: 1008; File: {0}".format(self.proc_file)
            self.handle_error_and_exit(error_string, error_code=1008)

    def open_command_file(self, proc_file):
        "Open input file in non-blocking mode"
        #flags = fcntl.fcntl(fd, fcntl.F_GETFL)
        #fcntl.fcntl(fd, fcntl.F_SETFL, flags | os.O_NONBLOCK)
        return os.open(proc_file, os.O_RDWR | os.O_NONBLOCK)

    @contextlib.contextmanager
    def open_proc_file(self):
        self.verify_proc_file_exists()
        try:
            f = self.open_command_file(self.proc_file)
        except (IOError, OSError):
            error_string = "Request cannot be executed, as Client cli cannot be opened. ErrorID: 1008; File: {0}".format(self.proc_file)
            self.handle_error_and_exit(error_string, error_code=1008)
        try:
            yield f
        finally:
            os.close(f)

    @contextlib.contextmanager
    def open_other_instance_proc_file(self, instance_name):
        proc_file = LOCK_FILE_PATH.format(instance_name)
        if os.path.isfile(proc_file) == False:
            error_string = "Request cannot be executed, as Client instance does not seem to be running, module is down. ErrorID: 1008; File: {0}".format(proc_file)
            self.handle_error_and_exit(error_string, error_code=1008)
        if instance_name == self.instance_name:
            raise Exception("Invalid proc file attempt")
        try:
            f = self.open_command_file(proc_file)
        except (IOError, OSError):
            error_string = "Request cannot be executed, as Client instance does not seem to be running, module is down. ErrorID: 1008; File: {0}".format(proc_file)
            self.handle_error_and_exit(error_string, error_code=1008)
        try:
            yield f
        finally:
            f.close()

    def drain_file(self, input_file):
        while True:
            try:
                buffer = os.read(input_file, BUFFER_SIZE)
            except OSError as err:
                if err.errno == errno.EAGAIN or err.errno == errno.EWOULDBLOCK or err.errno == errno.EOVERFLOW:
                    buffer = None
                    break
                else:
                    raise  # something else has happened -- better reraise
            # check for EOF
            if len(buffer) == 0:
                return

    def wait_client_zombie(self):
        try:
            fp = open(self.instance_status_file)
        except OSError as err:
            error_string = "Request cannot be executed, as Client does not seem to be running, module is down. ErrorID: 1008; File: {1}; Errno: {0}".format(err.errno, self.instance_status_file)
            self.handle_error_and_exit(error_string, error_code=1008)
        line = fp.readline()
        if "Zombie" in line:
            return True
        else:
            return False

    def should_check_persistency(self, uuid):
        if "AUTO_ATTACH_VOLUMES" in nvmeshConfigFileParams and nvmeshConfigFileParams["AUTO_ATTACH_VOLUMES"].lower() in ["no", "false"]:
            return
        if len(uuid) == 0:
            return False
        return True

    def is_nvmf_with_spdk_enabled(self):
        if (os.path.isfile(NVMESH_SPDK_RPCPY_PATH) or ("SPDK_RPCPY_PATH" in nvmeshConfigFileParams and nvmeshConfigFileParams["SPDK_RPCPY_PATH"] != "" and os.path.isfile(nvmeshConfigFileParams["SPDK_RPCPY_PATH"])))\
                and self.is_nvmf_or_iscsi_process_up():
            return True
        return False

    def is_nvmf_or_iscsi_process_up(self):
        list_processes_cmd = ['ps', 'aux']
        grep_nvmf_or_iscsi_cmd = ['grep', '-E', '[n]vmf_tgt|[i]scsi_tgt']

        success, res = self.execute_local_pipe_command(list_processes_cmd, grep_nvmf_or_iscsi_cmd)
        if not success:
            return False
        else:
            return True

    def get_cluster_uuid(self):
        try:
            with open(self.instance_status_file, 'r') as f:
                clnt_status = f.readlines()
        except OSError as error:
            self.logger.error(error.message)
            return None

        for _line in clnt_status:
            if _line.startswith('Cluster: '):
                line = _line.strip()
                break
        else:
            self.logger.error('Could not find cluster UUID')
            return None

        cluster_uuid = line[line.find('uuid='):][len('uuid='):].split()[0]
        return cluster_uuid.lower()

    def get_client_instance_state(self, instance_status_file):
        try:
            with open(instance_status_file, 'r') as f:
                clnt_status = f.readlines()
        except OSError as error:
            self.logger.error(error.message)
            return None

        for _line in clnt_status:
            if _line.startswith('module state'):
                line = _line.strip()
                break
        else:
            self.logger.error('Could not find client state')
            return None

        #module state=1 Ready
        value = int(line.split('=')[1].split()[0])
        state = ' '.join(line.split('=')[1].split()[1:])
        if state != CLIENT_VALID_STATES[value]:
            self.logger.error("Client state is not stable: {0}, {1}, expected state = {2}".format(value, state, CLIENT_VALID_STATES[value]))
            return None
        return value

    def get_client_instance_number_read_partition_and_is_on(self, instance_status_file):
        try:
            with open(instance_status_file, 'r') as f:
                clnt_status = f.readlines()
        except OSError as error:
            self.logger.error(error.message)
            return None, None

        for _line in clnt_status:
            if _line.startswith('n_read_part'):
                line = _line.strip()
                break
        else:
            return None, None

        #n_read_part=0, is_on=1
        value = int(line.split('=')[1].split(',')[0])
        is_on = int(line.split('=')[2])
        return value, is_on

    def is_client_state_ready(self):
        if self.instance_name == DEFAULT_MODULE_NAME:
            if self.get_client_instance_state(self.instance_status_file) == NVMEIBC_MOD_STATE_READY:
                return self.are_all_client_instances_ready()
        return self.get_client_instance_state(self.instance_status_file) == NVMEIBC_MOD_STATE_READY

    def is_client_state_already_in_upgrade(self):
        return self.get_client_instance_state(self.instance_status_file) >= NVMEIBC_MOD_STATE_RM_RDY

    def are_all_client_instances_ready(self):
        res = True
        for instance_name in self.get_all_client_instances():
            client_instance_value = self.get_client_instance_state(CLIENT_STATUS_FILE.format(instance_name))
            if client_instance_value != NVMEIBC_MOD_STATE_READY:
                self.logger.error("Client instance {0} is not Ready. Current state is {1}.".format(instance_name, CLIENT_VALID_STATES[int(client_instance_value)]))
                res = False
                break
        return res

    def get_read_partitions(self):
        res = {}
        if self.instance_name == DEFAULT_MODULE_NAME:
            res = self.get_all_read_partitions()
        if res.get(self.instance_name) is None:
            res[self.instance_name] = self.get_client_instance_number_read_partition_and_is_on(self.instance_status_file)
        return res

    def get_all_read_partitions(self):
        res = {}
        for instance_name in self.get_all_client_instances():
            client_instance_value, client_is_on = self.get_client_instance_number_read_partition_and_is_on(CLIENT_STATUS_FILE.format(instance_name))
            res[instance_name] = (client_instance_value, client_is_on)
        return res

    def set_atom_read_part_to_off(self):
        ioctl_cmd = "#|atom_b4upgrade"
        if self.instance_name == DEFAULT_MODULE_NAME:
            for instance_name in self.get_all_client_instances():
                with self.open_other_instance_lock_file(instance_name):
                    with self.open_other_instance_proc_file(instance_name) as command_file:
                        try:
                            os.write(command_file, ioctl_cmd.encode('utf-8'))
                        except OSError as err:
                            error_string = "Write error communicating with Client instance. ErrorID: 1001; File: {0}; Error: ({1}, {2})".format(command_file, str(err), err.errno)
                            self.handle_error_and_exit(error_string, error_code=1001)
        with self.open_proc_file() as command_file:
            try:
                os.write(command_file, ioctl_cmd.encode('utf-8'))
            except OSError as err:
                error_string = "Write error communicating with Client instance. ErrorID: 1001; File: {0}; Error: ({1}, {2})".format(command_file, str(err), err.errno)
                self.handle_error_and_exit(error_string, error_code=1001)

    def update_read_ahead_kb(self, volume_name, read_ahead_kb):
        try:
            read_ahead_kb_path = "/sys/block/{0}!{1}/queue/read_ahead_kb".format(self.dev_root, volume_name)
            with open(read_ahead_kb_path, 'w') as fd:
                fd.write(str(read_ahead_kb))
        except Exception as e:
            self.logger.error("Unexpected exception when trying to update read_ahead_kb for volume {0}: {1}".format(volume_name, str(e)))
            self.logger.error("Traceback: {0}".format(traceback.format_exc()))

    def apply_persistency_flags(self, volume_name, persistency_flags):
        for key, value in persistency_flags.items():
            if key == 'read_ahead_kb':
                self.update_read_ahead_kb(volume_name, value)
            else:
                self.logger.debug("Unknown persistency attribute found for volume {0}, Attribute: {1}, Value: {2}".format(volume_name, key, value))

    def get_volume_persistency(self):
        result = []
        attributes = None
        for volume_uuid in os.listdir(self.volume_persistency_folder):
            try:
                with open(os.path.join(self.volume_persistency_folder, volume_uuid), 'r') as fd:
                    attributes = json.load(fd)
            except ValueError:
                try:
                    with open(os.path.join(self.volume_persistency_folder, volume_uuid), 'r') as fd:
                        attributes = fd.readline()
                except Exception:
                    self.logger.error("Error reading persistency file for volume {0}, volume will attach with default values.".format(volume_uuid))
                    attributes = None
            result.append((volume_uuid, attributes if attributes else None))
        return result

    def get_alias_persistency(self):
        result = []
        attributes = None
        for entry in os.listdir(self.alias_persistency_folder):
            volume_uuid = entry.split('_')[0]
            volume_alias = '_'.join(entry.split('_')[1:])
            try:
                with open(os.path.join(self.alias_persistency_folder, entry), 'r') as fd:
                    attributes = json.load(fd)
            except ValueError:
                try:
                    with open(os.path.join(self.alias_persistency_folder, entry), 'r') as fd:
                        attributes = fd.readline()
                except Exception:
                    self.logger.error("Error reading persistency file for alias {0} of volume {1}, alias will created with default values.".format(volume_alias, volume_uuid))
                    attributes = None
            result.append((volume_uuid, volume_alias, attributes if attributes else None))
        return result

    def check_volume_persistency(self, volume_uuid):
        return os.path.exists(os.path.join(self.volume_persistency_folder, volume_uuid))

    # Create persistency by volume uuid
    # If persistency already exists return False
    def create_volume_persistency(self, volume_uuid, attributes=None):
        if self.check_volume_persistency(volume_uuid):
            return False
        persistency_path = os.path.join(self.volume_persistency_folder, volume_uuid)
        with open(persistency_path, 'w') as fd:
            json.dump(attributes, fd)
        return True

    # Delete persistency by volume uuid
    # Return True if deleted or did not exist and False if any error occured
    def delete_volume_persistency(self, volume_uuid):
        if self.check_volume_persistency(volume_uuid):
            try:
                persistency_path = os.path.join(self.volume_persistency_folder, volume_uuid)
                os.remove(persistency_path)
            except OSError as err:
                if err.errno == errno.ENOENT:
                    pass
                else:
                    return False
        return True

    def check_alias_persistency(self, volume_uuid, alias):
        file_path = os.path.join(self.alias_persistency_folder, volume_uuid + "_" + alias)
        return os.path.exists(file_path)

    # Creates a persistency between a volume uuid and its alias
    def create_alias_persistency(self, volume_uuid, alias, attributes):
        file_path = os.path.join(self.alias_persistency_folder, volume_uuid + "_" + alias)
        with open(file_path, 'w') as fd:
            json.dump(attributes, fd)

    # Deletes the persistency between a volume uuid and its alias
    def delete_alias_persistency(self, volume_uuid, alias):
        try:
            file_path = os.path.join(self.alias_persistency_folder, volume_uuid + "_" + alias)
            os.remove(file_path)
        except OSError as err:
            if err.errno == errno.ENOENT:
                pass

    # Deletes all of a volumes persistent aliases
    def purge_alias_persistency(self, volume_uuid):
        dir_regexp = self.alias_persistency_folder + "/" + volume_uuid + "*"
        aliasesList = glob.glob(dir_regexp)
        self.logger.debug("Removing all aliases of vol uuid={0} from persistency. aliases_list={1}".format(volume_uuid, ', '.join([file.split(volume_uuid)[1][1:] for file in aliasesList])))
        for file_path in aliasesList:
            os.remove(file_path)

    # Future: When using a DB access the one related to multi_client_instance
    #           Replace the api implementation with a DB centric one
    def get_is_hidden_from_volume_status(self, volume_status_json):
        volume_type = None
        try:
            with open(volume_status_json, 'r') as status_file:
                status_info = json.load(status_file)
            volume_type = status_info.get('type')
        except:
            pass
        if volume_type and volume_type != "hidden":
            return False
        elif volume_type:
            return True
        else:
            self.logger.debug("Couldn't extract volume type from status json from file {0}".format(volume_status_json))
        return volume_type

    def is_volume_attachment_hidden(self, volume):
        status_file_path = os.path.join(self.volume_status_folder, volume, "status.json")
        return self.get_is_hidden_from_volume_status(status_file_path)

    def get_name_and_uuid_from_volume_status(self, volume_status_json):
        name = None
        uuid = None
        try:
            with open(volume_status_json, 'r') as status_file:
                status_info = json.load(status_file)
            name = status_info.get('name')
            uuid = status_info.get('uuid')
        except:
            pass
        if not name or not uuid:
            self.logger.debug("Couldn't extract volume name and uuid from status json from file {0}".format(volume_status_json))
        return name, uuid

    def get_reservation_mode_from_volume_status(self, volume_status_json):
        mode = None
        try:
            with open(volume_status_json, 'r') as status_file:
                status_info = json.load(status_file)
            mode = status_info.get('reservation', None)
        except:
            pass
        if mode is None:
            self.logger.debug("Couldn't extract volume access mode from status json from file {0}".format(volume_status_json))
        return mode

    def get_attach_status_from_volume_status(self, volume_status_json):
        status = None
        try:
            with open(volume_status_json, 'r') as status_file:
                status_info = json.load(status_file)
            status = status_info.get('attach_status', None)
        except:
            pass
        if status is None:
            self.logger.debug("Couldn't extract volume attach status from status json from file {0}".format(volume_status_json))
        return status

    def get_reservation_version_from_volume_status(self, volume_status_json):
        version = None
        try:
            with open(volume_status_json, 'r') as status_file:
                status_info = json.load(status_file)
            version = status_info.get('reservation_version', None)
        except:
            pass
        if version is None:
            self.logger.debug("Couldn't extract volume access version from status json from file {0}".format(volume_status_json))
        return version

    def get_attachment_status_from_volume_status(self, volume_status_json):
        status = None
        try:
            with open(volume_status_json, 'r') as status_file:
                status_info = json.load(status_file)
            status = status_info.get('status', None)
        except:
            pass
        if status is None:
            self.logger.debug("Couldn't extract volume attachment status from status json from file {0}".format(volume_status_json))
        return status

    # will be used by Agent as well.
    def get_all_volume_names(self):
        return os.listdir(self.volume_status_folder)

    # Name of volume from UUID or None if not found
    # Cannot use atom (no uuid)
    def uuid_to_name(self, volume_uuid):
        for volume_name in self.get_all_volume_names():
            status_file_path = os.path.join(self.volume_status_folder, volume_name, "status.json")
            volname, voluuid = self.get_name_and_uuid_from_volume_status(status_file_path)
            if voluuid == volume_uuid:
                return volname
        return None

    # Can use atom instead of isdir (more exact, once name is valid can check uuid)
    def name_to_uuid(self, volume):
        volume_dir = os.path.join(self.volume_status_folder, volume)
        if os.path.isdir(volume_dir):
            status_file_path = os.path.join(volume_dir, "status.json")
            volname, voluuid = self.get_name_and_uuid_from_volume_status(status_file_path)
            if volname == volume:
                return voluuid
        return None

    def get_volume_name(self, volume):
        if is_valid_uuid(volume):
            return self.uuid_to_name(volume)
        return volume

    def get_volume_uuid(self, volume):
        if is_valid_uuid(volume):
            return volume
        return self.name_to_uuid(volume)

    def get_volume_mode(self, volume):
        volume_name = self.get_volume_name(volume)
        volume_dir = os.path.join(self.volume_status_folder, volume_name)
        if os.path.isdir(volume_dir):
            status_file_path = os.path.join(volume_dir, "status.json")
        else:
            self.logger.debug("Could not get status json for volume {0}.".format(volume_name))
            return None
        return self.get_reservation_mode_from_volume_status(status_file_path)

    def get_volume_reservation_version(self, volume):
        volume_name = self.get_volume_name(volume)
        volume_dir = os.path.join(self.volume_status_folder, volume_name)
        if os.path.isdir(volume_dir):
            status_file_path = os.path.join(volume_dir, "status.json")
        else:
            self.logger.debug("Could not get status json for volume {0}.".format(volume_name))
            return None
        return self.get_reservation_version_from_volume_status(status_file_path)

    def is_volume_status_preempted(self, volume):
        volume_name = self.get_volume_name(volume)
        volume_dir = os.path.join(self.volume_status_folder, volume_name)
        if os.path.isdir(volume_dir):
            status_file_path = os.path.join(volume_dir, "status.json")
        else:
            self.logger.debug("Could not get status json for volume {0}.".format(volume_name))
            return None
        status = self.get_attach_status_from_volume_status(status_file_path)
        return (status.lower() == "preempted")

    def get_volume_attachment_status(self, volume):
        volume_name = self.get_volume_name(volume)
        volume_dir = os.path.join(self.volume_status_folder, volume_name)
        if os.path.isdir(volume_dir):
            status_file_path = os.path.join(volume_dir, "status.json")
        else:
            self.logger.debug("Could not get status json for volume {0}.".format(volume_name))
            return None
        status = self.get_attachment_status_from_volume_status(status_file_path)
        return status

    def translate_mode_to_reservation(self, mode):
        if mode in self.valid_reservation_modes:
            return mode
        if mode == RW_FLAG:
            return SHARED_READ_WRITE
        if mode == RO_FLAG:
            return SHARED_READ_ONLY
        if mode == EX_FLAG:
            return EXCLUSIVE_READ_WRITE
        self.logger.debug("Could not parse access mode: {0}.".format(mode))
        return None

    def reservation_mode_to_flag(self, mode):
        if mode not in self.valid_reservation_modes:
            self.logger.debug("Could not parse access mode: {0}.".format(mode))
            return None
        if mode == SHARED_READ_ONLY:
            return RO_FLAG
        if mode == SHARED_READ_WRITE:
            return RW_FLAG
        if mode == EXCLUSIVE_READ_WRITE:
            return EX_FLAG

    # Use atom json to find sub volume with alias name
    # Match carrier_gendisk with gendisk and return volume name
    # Only called in error
    def get_carrier_volume_of_alias(self, alias):
        atom_file = os.open(ATOM_STATUS_JSON, os.O_RDONLY)
        try:
            atom_info = json.loads(os.read(atom_file, MAX_ATOM_STATUS_READ))
        except ValueError as err:
            self.logger.debug("Error parsing ATOM info. ATOM STATUS FILE: {0}".format(ATOM_STATUS_JSON))
            return None
        finally:
            os.close(atom_file)
        atom_list = atom_info.get("atoms")

        carrier_disk = next((atom.get("carrier_gendisk") for atom in atom_list if atom.get("name") == alias), None)
        if carrier_disk:
            carrier_name = next((atom.get("name") for atom in atom_list if atom.get("gendisk") in carrier_disk), None)
            if not carrier_name:
                self.logger.debug("We found the carrier_disk={0} but not the volume itself!".format(carrier_disk))
            return carrier_name
        path = os.path.join(self.dev_root, alias[:self.max_block_device_path])
        atom_by_path = next((atom for atom in atom_list if atom.get("path") == path), None)
        if atom_by_path:
            attributes = atom_by_path.get("attr")
            if 'sub' in attributes.get("str_flags"): # A previous alias is using this path, we want to find it's carrier
                carrier_name = self.get_carrier_volume_of_alias(atom_by_path.get("name"))
                if carrier_name:
                    self.logger.debug("Path {0} is in use by alias {1} of volume {2}.".format(path, atom_by_path.get("name"), carrier_name))
                    return carrier_name
                else:
                    self.logger.debug("Path {0} is in use by alias {1}.".format(path, atom_by_path.get("name")))
            return atom_by_path.get("name")
        self.logger.debug("Exhausted search of ATOM info.")
        return carrier_disk

    # Function required to remove any of the following cases:
    # Given uuid/s that are not attached
    # Given both a volume name and it's uuid in the same request
    # returns a list of unknown volume uuids
    #         a dictionary with the remaing volumes to detach by name (required for atom)
    def preliminary_volume_filter(self, volumes):
        unknown_uuids = []
        volume_names = dict()
        for vol in volumes:
            vol_name = self.get_volume_name(vol)
            if not vol_name:
                # Removes all unattached uuids given (very rare)
                unknown_uuids.append(vol)
                self.update_volume_satus_json(vol, "Unknown Volume", error="Unknown Volume ID Given for detach.")
            elif vol_name in volume_names:
                # Both UUID and volume name have been given to the same detach request (impossible - test case)
                uuid = vol if is_valid_uuid(vol) else volume_names[vol_name]
                self.logger.debug("Volume name ({0}) and UUID ({1}) given for the same detach command skipping {2}.".format(vol_name, uuid, vol))
                self.update_volume_satus_json(uuid, "Duplicate Request", error="This request is the same as for volume {0}.".format(vol_name))
            else:
                volume_names[vol_name] = vol
                self.update_volume_satus_json(vol, "Pending")
        return volume_names, unknown_uuids

    # Filter out volumes that have opens in ATOM or are not attached
    def filter_unused_volumes(self, volumes):
        unused_volumes = []
        busy_volumes = []
        unattached_volumes = []
        volume_names, unknown_uuids = self.preliminary_volume_filter(volumes)
        if volume_names:
            atom_file = os.open(ATOM_STATUS_JSON, os.O_RDONLY)
            try:
                atom_info = json.loads(os.read(atom_file, MAX_ATOM_STATUS_READ))
            except ValueError as err:
                return set(volume_names)
            finally:
                os.close(atom_file)
            atom_list = atom_info.get("atoms")
            unused_volumes = [atom.get("name") for atom in atom_list if atom.get("num_opens") == 0 and atom.get("name") in volume_names]
            busy_volumes = [atom.get("name") for atom in atom_list if atom.get("num_opens") > 0 and atom.get("name") in volume_names]
            unattached_volumes = [vol for vol in volume_names if vol not in unused_volumes and vol not in busy_volumes]
        if busy_volumes:
            self.logger.info("Volumes are in use, cannot detach. Volumes: {0}".format(', '.join(busy_volumes)))
            for vol in busy_volumes:
                self.update_volume_satus_json(vol, "Busy")
        if unattached_volumes or unknown_uuids:
            self.logger.info("Volumes are not attached. Volumes: {0}".format(', '.join(unknown_uuids + unattached_volumes)))
            for vol in unattached_volumes:
                self.update_volume_satus_json(vol, "Not Attached")
        return set(unused_volumes)

    def get_device_path_for_volume(self, volume_name):
        return os.path.join(self.device_folder, volume_name[:self.max_block_device_path])

    # Check if name is used in the device directory
    def is_volume_name_in_device_dir(self, volume_name):
        is_blk = False
        full_path = self.get_device_path_for_volume(volume_name)
        if len(volume_name) > self.max_block_device_path:
            self.logger.debug("Full path to block device is longer than allowed, truncated to {0}.".format(full_path))
        is_dir = os.path.exists(full_path)
        if is_dir:
            is_blk = is_block_device(full_path)
        return is_dir, is_blk

    def normalize_volume_names_and_aliases(self, volumes, alias, attach=False):
        normalized_volumes = []
        if alias and len(alias) > MAX_VOLUME_NAME:
            self.logger.error("Alias name is greater than the max length for an alias, truncating name. Requested: {0}; Alias: {1}".format(alias, alias[:MAX_VOLUME_NAME]))
            alias = alias[:MAX_VOLUME_NAME]
        for vol in volumes:
            if not is_valid_uuid(vol):
                if len(vol) > MAX_VOLUME_NAME:
                    self.logger.error("Volume name is greater than the max length for a volume name, truncating name. Requested {0}; Volume name: {1}".format(vol, vol[:MAX_VOLUME_NAME]))
                normalized_volumes.append(vol[:MAX_VOLUME_NAME])
            else:
                normalized_volumes.append(vol)
        if attach:
            if alias and len(alias) > self.max_block_device_path:
                self.logger.error("Alias name is greater than the device directory limit, truncating directory. Alias: {0}; Device path: {1}".format(alias, self.get_device_path_for_volume(alias)))
            for vol in normalized_volumes:
                if len(vol) > self.max_block_device_path:
                    self.logger.error("Volume name is greater than the device directory limit, truncating directory. Volume: {0}; Device path: {1}".format(vol, self.get_device_path_for_volume(vol)))
        return normalized_volumes, alias

    # Json response code, update volumes as statuses arive, or before sending, print at exit or error
    def init_json_response(self):
        if self.json_output:
            self.json_response = {"status": "success", "volumes": {}, "error": "", "error_code": 0}

    def handle_error_and_exit(self, error_string, error_code=0, additional_info=""):
        if self.json_output: # One time error and quit (some volumes might have statuses updated)
            self.json_response["status"] = "failed"
            self.json_response["error_code"] = error_code
            self.json_response["error"] = error_string
            self.print_json_output()
        else:
            self.logger.error(error_string)
            self.logger.error(additional_info)

        self.delete_instance_if_needed()

        sys.exit(1)

    def delete_instance_if_needed(self):
        if self.instance_name != 'nvmeibc' and self.is_instance_active(self.instance_name) and not self.is_instance_has_attachments():
            self.delete_client_instance()

    def print_json_output(self):
        if self.json_output:
            exit_code = 1 if any([data.get("error", False) for vol, data in self.json_response["volumes"].items()]) else None
            self.logger.critical(json.dumps(self.json_response))
            if exit_code:
                sys.exit(1)

    def update_volume_satus_json(self, volume_id, status, error=""):
        if self.json_output:
            volume_dict = self.json_response["volumes"].get(volume_id, None)
            if volume_dict is None:
                self.json_response["volumes"][volume_id] = {"status":status}
            else:
                self.json_response["volumes"][volume_id].update({"status":status})
            if error:
                self.json_response["volumes"][volume_id].update({"error":error})

    def get_volume_satus_error(self, volume_id):
        if self.json_output:
            volume_dict = self.json_response["volumes"].get(volume_id, None)
            if volume_dict is not None:
                volume_error = volume_dict.get("error", None)
                if volume_error is not None:
                    return True
        return False

    @contextlib.contextmanager
    def open_lock_file(self, instance_lock_file=False):
        try:
            created_lock_file = ''
            if instance_lock_file:
                f = open(GLOBAL_INSTANCE_LOCK_FILE_PATH, 'w+')
                created_lock_file = GLOBAL_INSTANCE_LOCK_FILE_PATH
            else:
                f = open(self.lock_file_path, 'w+')
                created_lock_file = self.lock_file_path

            os.chmod(created_lock_file, 0o664)
        except IOError as err:
            if err.errno == errno.EACCES:
                error_string = "Running attach and detach commands should be done as root user. ErrorID: 1046; Access denied for file: {0}".format(self.lock_file_path)
                self.handle_error_and_exit(error_string, error_code=1046)
            else:
                raise
        try:
            while True:
                try:
                    fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    break
                except IOError as err:
                    if err.errno != errno.EAGAIN:
                        raise
                    else:
                        time.sleep(0.01)
            yield f
        except KeyboardInterrupt as err:
            pass
        finally:
            fcntl.flock(f, fcntl.LOCK_UN)
            f.close()

    @contextlib.contextmanager
    def open_other_instance_lock_file(self, instance_name):
        if instance_name == self.instance_name:
            raise Exception("Invalid lock file attempt")
        try:
            f = open(LOCK_FILE_PATH.format(instance_name), 'w+')
        except IOError as err:
            if err.errno == errno.EACCES:
                error_string = "Running attach and detach commands should be done as root user. ErrorID: 1046; Access denied for file: {0}".format(LOCK_FILE_PATH.format(instance_name))
                self.handle_error_and_exit(error_string, error_code=1046)
            else:
                raise
        try:
            while True:
                try:
                    fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    break
                except IOError as err:
                    if err.errno != errno.EAGAIN:
                        raise
                    else:
                        time.sleep(0.01)
            yield f
        except KeyboardInterrupt as err:
            pass
        finally:
            fcntl.flock(f, fcntl.LOCK_UN)
            f.close()





# Very useful functions below

import uuid
import stat

def is_block_device(path):
    try:
        return stat.S_ISBLK(os.stat(path).st_mode)
    except:
        return False

def is_valid_uuid(volume_name):
    try:
        uuid_obj = uuid.UUID(volume_name)
    except:
        return False

    return str(uuid_obj) == volume_name

def print_given_params(*args, **kwargs):
    res = []
    for k,v in kwargs.items():
        if v is not None:
            res += ["({0}: {1})".format(k, v)]
    return res

class InstanceConfiguration(object):
    class Param(object):
        def __init__(self, conf_key, value, conf_file_key=None, conf_file_value=None):
            self.conf_key = conf_key
            self.value = value
            self.conf_file_key = conf_file_key
            self.conf_file_value = conf_file_value

    @staticmethod
    def is_true_param(p):
        if isinstance(p, str):
                return p.lower() == "yes"
        return p is True

    def __init__(self, name, management_cluster=None, management_protocol=None, management_db_uuid=None,
                 auto_generated=None, configured_nics=None, enable_rdda=None, disable_rdda=None,
                 tcp_enabled=None, tcp_only=None):
        self.name = self.Param(conf_key='INSTANCE_NAME', value=name)
        self.management_cluster = self.Param(conf_key='MANAGEMENT_SERVERS', value=management_cluster)
        self.management_protocol = self.Param(conf_key='MANAGEMENT_PROTOCOL', value=management_protocol)
        self.management_db_uuid = self.Param(conf_key='MANAGEMENT_DB_UUID', value=management_db_uuid)
        if auto_generated is not None:
            self.auto_generated = self.Param(conf_key='AUTO_GENERATED', value="1" if auto_generated else "0")
        self.configured_nics = self.Param(conf_key='CONFIGURED_NICS', value=configured_nics)
        if enable_rdda:
            self.enable_rdda = self.Param(conf_key='RDDA', value=None, conf_file_key='MLX5_RDDA_ENABLED', conf_file_value="Yes")
        elif disable_rdda is False:
            self.disable_rdda = self.Param(conf_key='NO_RDDA', value=None, conf_file_key='MLX5_RDDA_ENABLED', conf_file_value="No")
        self.tcp_enabled = self.Param(conf_key='TCP_ENABLED', value="Yes" if self.is_true_param(tcp_enabled) or self.is_true_param(tcp_only) else "No")
        self.tcp_only = self.Param(conf_key='TCP_ONLY', value="Yes" if self.is_true_param(tcp_only) else "No")

    def filter_params(self):
        return [param for param in list(self.__dict__.values()) if not (param.value is None and param.conf_file_value is None)]

    def get_conf_str(self):
        return '|'.join(
                ['{conf_key}{v}'.format(conf_key=param.conf_key, v='={value}'.format(value=param.value) if param.value else '') for param in self.filter_params()]
            )

    def get_conf_file_content(self):
        return '\n'.join(
            ['{conf_key}=\"{value}\"'.format(
                    conf_key=param.conf_file_key if param.conf_file_key else param.conf_key,
                    value=param.conf_file_value if param.conf_file_value else param.value
                ) for param in self.filter_params()]
        )
