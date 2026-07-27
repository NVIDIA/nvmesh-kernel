#!/usr/bin/env python3

import os
import sys
import logging
import logging.handlers

DEFAULT_MODULE_NAME = "nvmeibc"

def log(message):
	print(('process_config_files.py: %s' % message))

def readBashFile(filename, l = None):
	if not l:
		l = {}
	g = {}

	if os.path.exists(filename):
		try:
			with open(filename, "rb") as f:
				content = f.read()
			compiled_code = compile(content, filename, 'exec')
			exec(compiled_code, g, l)
		except IOError as e:
			log(f"Error reading file '{filename}': {e}")
			raise
		except SyntaxError as e:
			log(f"Config file '{filename}' is invalid at line {e.lineno}: {e.msg}")
			raise
		except Exception as e:
			log(f"Config file '{filename}' is invalid: {type(e).__name__}: {e}")
			raise

	return l

def write_dict_to_file(dict, filepath):
	with open(filepath, 'w') as config_file:
		for key, value in sorted(dict.items()):
			config_file.write('{k}="{v}"\n'.format(k=key, v=value))
		log('Written {}'.format(filepath))

def processConfigFiles(instance_name, is_client_instance):
	# this function will take the file with parameters from the Configuration profile,
	# And will concatenate the user file (nmvesh.conf) to form the final config_file
	configs_path = '/etc/nvmesh/configs/{instance_name}'.format(instance_name=instance_name)

	if not os.path.exists(configs_path):
		os.makedirs(configs_path, mode=0o755)

	final_config_path = os.path.join(configs_path, '.nvmesh{}.conf'.format('.instance' if is_client_instance else ''))

	mgmt_profile = readBashFile(os.path.join(configs_path, '.mgmt.nvmesh.conf'))

	if is_client_instance:
		user_config_path = '/etc/nvmesh/configs/{module_name}/.nvmesh.conf'.format(module_name=DEFAULT_MODULE_NAME)
	else:
		user_config_path = '/etc/nvmesh/nvmesh.conf'

	user_config = readBashFile(user_config_path)
	instance_config = readBashFile(os.path.join(configs_path, 'nvmesh.instance.conf'))

	for conf_profile_param in ['CONFIG_PROFILE_NAME', 'CONFIG_PROFILE_ID', 'CONFIG_PROFILE_VERSION']:
		user_config.pop(conf_profile_param, None)

	processed_params = {}
	for config in [mgmt_profile, user_config, instance_config]:
		processed_params.update(config)

	# log('Got user_config from /etc/nvmesh/nvmesh.conf - %s' % user_config)
	# log('Got mgmt_profile from /etc/nvmesh/.mgmt.nvmesh.conf - %s' % mgmt_profile)
	# log('Going to write into /etc/nvmesh/.nvmesh.conf - %s' % processed_params)

	write_dict_to_file(processed_params, final_config_path)

	if not is_client_instance:
		write_dict_to_file(processed_params, '/etc/nvmesh/.nvmesh.conf')

if __name__ == '__main__':
	is_client_instance = len(sys.argv) > 1
	instance_name = sys.argv[1] if is_client_instance else DEFAULT_MODULE_NAME
	processConfigFiles(instance_name, is_client_instance)