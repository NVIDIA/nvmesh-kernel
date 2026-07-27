#!/usr/bin/python3
import json
import subprocess
import sys
import re
import os
import argparse

parser = argparse.ArgumentParser()
error_counter = 0
_checkers_dict = {}
param_fix_list = []
grub_fix_list = []

THIS_DIRECTORY = os.path.dirname(os.path.abspath(__file__))
JSON_CONF_NAME = "configurations.json"
JSON_CONF_PATH = os.path.join(THIS_DIRECTORY, JSON_CONF_NAME)


MODPROBE_CONF_LOCATION = "/opt/nvmesh/common-repo/modprobe.d"
PARAM_LOCATION = "/sys/module/{module_name}/parameters/{param_name}"
MODULE_LOCATION = "/sys/module/{module_name}"
PROC_CMDLINE_LOCATION = "/proc/cmdline"
SYSCONFIG_KDUMP_LOCATION = "/etc/sysconfig/kdump"
IOMMU_LOCATION = "/sys/class/iommu"
SYSCTL_CONF_LOCATION = "/etc/sysctl.conf"
NVMESH_CONF = "/etc/nvmesh/nvmesh.conf"

OCI_KEY = "OCI"
DPU_KEY = "DPU"
LAB_KEY = "LAB"
PARAMETERS_KEY = "parameters"
KDUMP_KEY = "kdump"
GRUB_KEY = "grub"
IOMMU_KEY = "iommu"


def add_func_to_dict(key):
    def _add_dict(func):
        _checkers_dict[key] = func
        return func
    return _add_dict


def print_separator(separator):
    print(separator * 80)


def make_pretty(func):
    def inner(*args, **kwargs):
        print_separator("~")
        value = func(*args, **kwargs)
        print_separator("~")
        return value
    return inner


def print_OK_FAIL(current_error):
    if current_error == error_counter:
        print("**OK!**")
    else:
        print("**FAIL**")


def run_cmd(cmd, splitter=" ", **kwargs):
    cmd_arr = cmd.split(splitter)
    proc = subprocess.Popen(cmd_arr, stdout=subprocess.PIPE, stderr=subprocess.PIPE, **kwargs)
    stdout, stderr = proc.communicate()
    stdout = stdout.decode('ascii').strip()
    stderr = stderr.decode('ascii').strip()
    rv = proc.returncode
    return rv, stdout, stderr


def compare_values(wanted, actual):
    YES = ['Y', 'YES','1']
    NO = ['N','NO','0']
    YES_NO = YES + NO
    if wanted in YES_NO and actual in YES_NO: 
        return (wanted in NO and actual in NO) or (wanted in YES and actual in YES)
    return wanted == actual


def verify_param(module_name, param_name, wanted_value):
    global error_counter
    param_location_with_values = PARAM_LOCATION.format(module_name=module_name, param_name = param_name)
    cmd = f'cat {param_location_with_values}'
    rv, stdout, stderr = run_cmd(cmd)
    if rv != 0:
        print(f'\tError - module param {module_name} not found.\n', file=sys.stderr)
        error_counter += 1
    elif not compare_values(str(wanted_value).upper(), stdout.upper()):
        print(f'\t{module_name} {param_name} is set to {stdout}. Need to be: {wanted_value}.')
        param_fix_list.append(f"echo {wanted_value} > {param_location_with_values}")
        error_counter += 1


def is_tcp():
    global error_counter
    rv, stdout, stderr = run_cmd("cat /sys/module/nvmeibs/parameters/tcp_mode")
    if rv == 0:
        return int(stdout) > 0
    else:
        print(f'\tError - module param nvmeibs not found.\n', file=sys.stderr)
    return False


@make_pretty
def check_module(module_name, params_dict):
    global error_counter
    current_error = error_counter
    module_location_with_name = MODULE_LOCATION.format(module_name=module_name)

    print(f"Checking {module_name} parameters:")
    rv, stdout, stderr = run_cmd(f'ls {module_location_with_name}')
    if rv == 0:
        for param_name in params_dict[module_name]:
            param_value = params_dict[module_name][param_name]
            verify_param(module_name, param_name, param_value)
    else:
        if module_name.upper() == "SIW" and not is_tcp():
            print('\tNot supposed to be loaded')
        else:
            error_counter += 1
            print(f'\tERROR - Module {module_name} not loaded.', file = sys.stderr)
    print_OK_FAIL(current_error)


@add_func_to_dict(PARAMETERS_KEY)
def check_modules(params_dict):
    for module_name in params_dict:
        check_module(module_name, params_dict)


def check_for_args_in_location(arg_list, location):
    global error_counter
    current_error = error_counter
    print(f"Checking {location}:")
    cmd = f"cat {location}"
    rv, stdout, stderr = run_cmd(cmd)
    if rv == 0:
        for arg in arg_list: 
            if arg.replace(' ','') not in stdout.replace(' ',''):
                print(f"\tMissing {arg} in {location}")
                error_counter += 1
    else:
        print(f"\tError: Failed to run: {cmd}.\n{stderr}", file=sys.stderr)
        error_counter += 1
    print_OK_FAIL(current_error)


@add_func_to_dict(GRUB_KEY)
def check_grub(grub_list):
    check_for_args_in_location(grub_list, PROC_CMDLINE_LOCATION)


@add_func_to_dict(KDUMP_KEY)
def check_sysconfig_kdump(arg_list):
    check_for_args_in_location(arg_list, SYSCONFIG_KDUMP_LOCATION)


def chech_crash_from_console(arg_list):
    check_for_args_in_location(arg_list, SYSCTL_CONF_LOCATION)


@add_func_to_dict(KDUMP_KEY)
@make_pretty
def check_kdump(kdump_dict):
    print(f"Checking kdump configurations:")
    for key in kdump_dict:
        if key == "sysconfig-kdump":
            check_sysconfig_kdump(kdump_dict[key])
        if key == "crash-from-console":
            chech_crash_from_console(kdump_dict[key])


EMPTY = "-1"
NOT_EMPTY = "0"


@add_func_to_dict(IOMMU_KEY)
@make_pretty
def check_iommu(wanted_value):
    global error_counter
    current_error = error_counter
    print("Checking IOMMU:")
    cmd = f'[ "$(ls -A {IOMMU_LOCATION})" ] && echo {NOT_EMPTY} || echo {EMPTY}'
    rv, stdout, stderr = run_cmd(cmd, splitter="\n", shell=True)
    if rv == 0:
        if stdout == EMPTY and wanted_value == False:
            print("\tIOMMU is OFF as requested")
        elif stdout == NOT_EMPTY and wanted_value == True:
            print("\tIOMMU is ON as requested")
        else:
            print(f"\tIOMMU is wrong! please { 'enable' if wanted_value  else 'disable'} it")
            error_counter += 1
    else:
        print(f"\tError: Failed to run: {cmd}.\n{stderr}", file=sys.stderr)
        error_counter += 1

    print_OK_FAIL(current_error)


def get_modprobe_file_name():
    modprobe_key = ""
    with open(NVMESH_CONF, 'r') as f:
        lines = f.readlines()
    for line in lines:
        if line.startswith("NVMESH_MODE"):
            _, modprobe_key = line.split('=')
            modprobe_key = modprobe_key.strip()
            return modprobe_key
    print(f"ERROR - There's no NVMESH_MODE configured in {NVMESH_CONF}. Not checking for default values.", file=sys.stderr)
    return None


def parse_modprobe_conf_file():
    modules_dict = {}
    modprobe_file = get_modprobe_file_name()
    if modprobe_file is None:
        return modules_dict
    path_to_modprobe = f"{MODPROBE_CONF_LOCATION}/{modprobe_file[1:-1]}.conf"
    print (f"Checking {path_to_modprobe}")
    with open(path_to_modprobe, 'r') as f:
        lines = f.readlines()
    
    for line in lines:
        if line.startswith('options'):
            match = re.match(r'options (\w+) (.*)', line)
            module_name = match.group(1)
            mod_params = match.group(2)
            params_dict = {}
            for mod_param in mod_params.split():
                key, value = mod_param.split('=')
                params_dict[key] = value
            modules_dict[module_name] = params_dict
    return modules_dict


def check_specific_configuration(conf_dict):
    for key in conf_dict:
        if key in _checkers_dict:
            _checkers_dict[key](conf_dict[key])
        else:
            print(f"{key} - undefined arguement. Skipping\n")


@make_pretty
def check_default_modprobe_conf():
    print("Checking default modprobe.conf:")
    print_separator("#")
    default_modprobe_dict = parse_modprobe_conf_file()
    if default_modprobe_dict:
        check_modules(default_modprobe_dict)
    print_separator("#")


def print_fix_list():
    if param_fix_list:
        print(f"To fix module parameters, run    as sudo the following:\n")
        for fix in param_fix_list:
            print(fix)
        return -1


def finish_result():
    global error_counter
    print_separator("*")
    print("\n")
    if error_counter == 0:
        print(f"All configurations are OK! Hurray! error_counter = {error_counter}\n")
        return 0

    print(f"Node needs to be configured before starting test. Please see the logs above. error_counter = {error_counter}\n")
    print_fix_list()


def handle_arguments():
    global parser
    default_only = False
    modprobe_file = get_modprobe_file_name()
    help_description = f"Tool that verifies that the node is using the wanted configurations.\n\
                        Usage: node_addmission.py [configuration_arg]\n\
                        Where configuration_arg should be a label that is present in configurations.json.\n\
                        If none were given, it will execute only the default configurations checks that\
                        are in {MODPROBE_CONF_LOCATION}/{modprobe_file[1:-1]}.conf"

    parser = argparse.ArgumentParser(description=help_description)
    parser.add_argument('-k', '--conf-key', metavar='conf_key', required=False, type=str, help=f'Verifying the configurations specified in the given key in configurations.json.')
    parser.add_argument('-d', '--default-only', action='store_true' ,help=f'Verifying only {MODPROBE_CONF_LOCATION}/{modprobe_file[1:-1]}.conf')
    args = parser.parse_args()
    return args


def main():
    
    args = handle_arguments()
    default_only = args.default_only
    conf_key = args.conf_key

    check_default_modprobe_conf()
    with open(JSON_CONF_PATH, "r") as read_file:
        conf_dict = json.load(read_file)

    if conf_key not in conf_dict:
        default_only = True

    print("\n")
    if not default_only:
        print(f"Checking specified {conf_key} configurations:")    
        print_separator("#")
        check_specific_configuration(conf_dict[conf_key])
        print_separator("#")

    return finish_result()


if __name__ == "__main__":
    sys.exit(main())
