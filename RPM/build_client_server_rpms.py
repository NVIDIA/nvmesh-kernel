#!/usr/bin/python2

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

import sys, subprocess, getopt, json, os.path, os, fcntl, time

NVMESH_PREFIX = 'NVMesh-'
SERVER_DIR_NAME = 'NVMesh-target'
CLIENT_DIR_NAME = 'NVMesh-client'

NVMESH_TMP_DIR = '/tmp/nvmesh'
GIT_SPEC_SOURCE_DIR = '{}/RPM/'.format(NVMESH_TMP_DIR)

SERVER_SPEC_FILE_NAME = '{}.spec'.format(SERVER_DIR_NAME)
CLIENT_SPEC_FILE_NAME = '{}.spec'.format(CLIENT_DIR_NAME)
CORE_SPEC_FILE_NAME = 'NVMesh-core.spec'

global image_description_list
image_description_list = {}

global skipped_images
skipped_images= {}

RED_COLOR_LOG = '\033[91m{}\033[0m'
GREEN_COLOR_LOG = '\033[92m{}\033[0m'
YELLOW_COLOR_LOG = '\033[93m{}\033[0m'
BLUE_COLOR_LOG = '\033[94m{}\033[0m'

def create_log_file(file_name):
	global LOG_FILE
	LOG_FILE = open(file_name, 'w+')

def clean_output_dir():
	print 'Cleaning the output dir, avoiding leftovers getting into new RPMs'
	clean_dir = subprocess.Popen('rm -rf {}/{}_{}'.format(OUTPUT_DIR, COMMIT_ID, ('512' if SECTOR_SHIFT else '4k')),
	                             shell=True)
	clean_dir.wait()

def read_file(file_name):
	file_content = []
	with open(file_name, 'r') as my_file:
		file_content.extend(my_file.readlines())
	return file_content

def tail_log_file(file_name, lines=30):
	values_to_ignore = ['OpenSSH', 'ECDSA', 'Connection to']
	file_content = read_file(file_name)
	counter = 0
	tail_content = []
	for line in reversed(file_content):
		if line.rstrip() == '':
			tail_content.insert(0, line)
		elif not any(sub in line for sub in values_to_ignore):
			tail_content.insert(0, line)
			counter += 1
		if counter >= lines:
			break
	return tail_content

def copy_git_repo():
	rmproc = subprocess.Popen('rm -fr {}'.format(NVMESH_TMP_DIR), shell=True)
	rmproc.wait()

	print 'Cloning git repo...'

	clone_cmd = 'cd /tmp; git clone -b "{}" git@gitlab.nvidia.com:{}/nvmesh.git; cd nvmesh'.format(
			BRANCH, PRIVATE_REPOSITORY)
	gitclone_proc = subprocess.Popen(clone_cmd, shell=True)
	proc_res = gitclone_proc.wait()
	print clone_cmd

	if proc_res != 0:
		sys.stderr.write('Error while cloning git repo\n')
		sys.exit(1)

	print 'Changing to given commit...'
	global COMMIT_ID
	commit_id = COMMIT_ID.lower()
	if commit_id == 'head':
		proc = subprocess.Popen('cd {}; git log | head -n1 | cut -d\" \" -f2'.format(NVMESH_TMP_DIR),
		                        stdout=subprocess.PIPE, stderr=None, shell=True)
		COMMIT_ID = proc.communicate()[0]
		COMMIT_ID = COMMIT_ID[:-1]
	print 'Reset to commit: {}'.format(COMMIT_ID)
	resetcommit_proc = subprocess.Popen(
			'cd {}; git reset --hard {}; if [ "$?" != "0" ]; then exit 1; fi'.format(NVMESH_TMP_DIR, COMMIT_ID),
			shell=True)
	proc_res = resetcommit_proc.wait()

	if proc_res != 0:
		sys.stderr.write('Could not reset to specific commit id, probably wrong commit id\n')
		sys.exit(1)

def delete_clone_repo():
	proc = subprocess.Popen('sudo rm -rf {}'.format(NVMESH_TMP_DIR), shell=True)
	proc_res = proc.wait()

	if proc_res != 0:
		sys.stderr.write('Could not delete repo directory\n')

def create_tmp_vm_clone(tmp_clone_name):
	clone_name = '{}_base'.format(tmp_clone_name)
	proc = subprocess.Popen(
			'cd /mnt/ext4/vm-images-new/; sudo time virt-clone --name={1} --original={0} --file /mnt/ext4/vm-images-new/{1}.qcow2  2>&1'.format(clone_name, tmp_clone_name), shell=True)

	proc_res = proc.wait()

	if not proc_res == 0:
		return False
	else:
		return True

def generate_compile_cmd(tmp_clone_name, image, kernel, ofed_tgz, compile_pager):
	'''
	return the basic command that will be common to all images
	:return: bash command, with open string placement for tmp_clone_name, image, kernel and ofed_tgz (in this specific order)
	'''
	sector_shift = ''
	if SECTOR_SHIFT:
		sector_shift = ' -s'

        compile_pager = 'yes' if compile_pager else 'no'

	return '{} {} {} "{}" "{}" -k "{}" -o "{}" -m {} -i {} {} -p {}'.format(COMPILE_SCRIPT_FILE, tmp_clone_name, image, COMMIT_ID, BRANCH, kernel, ofed_tgz, KVM_IMAGES_DIR, OUTPUT_DIR, sector_shift, compile_pager)

def execute_scripts(image, proc_counter_limit=10, force=False):
	success_status = True
	executing_cmds = []
	compile_list_obj = image_description_list[image][:]
	skipped_images[image] = []
	current_executing_cmds = []

	while True:
		has_changed = False
		if proc_counter_limit > len(executing_cmds) and len(compile_list_obj) > 0:
			current_executing_image = compile_list_obj.pop(0)
			executing_cmds.append(current_executing_image)

			if '{}_base'.format(current_executing_image['image_name']) in existing_clones_array:
				if not create_tmp_vm_clone(current_executing_image['image_name']):
					success_status = False
					sys.stderr.write(RED_COLOR_LOG.format('Failed to clone image {}\n'.format(current_executing_image['image_name'])))
					continue
				else:
					print GREEN_COLOR_LOG.format('Successfully cloned image {}'.format(current_executing_image['image_name']))
			else:
				success_status = False
				sys.stderr.write(RED_COLOR_LOG.format('base image not found for image {}\n'.format(current_executing_image['image_name'])))
				continue

			if success_status or force:
				cmd = generate_compile_cmd(current_executing_image['image_name'], current_executing_image['image'], current_executing_image['kernel'], 
                                        current_executing_image['ofed'], current_executing_image['compile_pager'])

				create_log_file('{}.log'.format(current_executing_image['sys_name']))
				current_executing_image['process'] = subprocess.Popen(cmd, stdout=LOG_FILE, stderr=LOG_FILE, shell=True)
			else:
				print YELLOW_COLOR_LOG.format('skipping execution of image {} because of previous failure'.format(current_executing_image['image_name']))
				skipped_images[image].append(current_executing_image['image_name'])

		current_executing_cmds = []
		for execution in executing_cmds:
			res = execution['process'].poll()
			if res == 0:
				print GREEN_COLOR_LOG.format('Success! Code compiled successfully on {}'.format(execution['sys_name']))
				clean_images(execution['image_name'])
				has_changed = True
			elif res is not None:
				sys.stderr.write(RED_COLOR_LOG.format('Failed to compile on image {}\n'.format(execution['sys_name'])))
				success_status = False
				clean_images(execution['image_name'])
				sys.stderr.write(RED_COLOR_LOG.format('{}\n'.format(tail_log_file('{}.log'.format(execution['sys_name'])))))
				has_changed = True
			else:
				current_executing_cmds.append(execution)

		if has_changed:
			executing_cmds = current_executing_cmds[:]
		
		if len(compile_list_obj) == 0 and len(executing_cmds) == 0:
			break

		time.sleep(2)

	return success_status

def read_config_and_compile_all():
	with open(METRIC_FILE) as data_file:
		data = json.load(data_file)

	success_statuses = {}

	for distro in data:
		print 'Compiling for {} distro'.format(distro)
		for image, combos in data[distro]["distributions_images"].iteritems():
			success_statuses[image] = True
			if distro == 'UB':
				long_distro = 'Ubuntu'
			elif distro == 'EL':
				long_distro = 'RHEL'
			else:
				long_distro = 'Other'

			ofeds = ''
			kernels_lst = ''
			image_description_list[image] = []

                        should_compile_pager = True

			for ofed_tgz, kernels in combos.iteritems():
				inbox_driver = ofed_tgz == 'none'
				ofeds = '{} {}'.format(ofeds, ofed_tgz)
				for kernel in kernels:
					if kernel not in kernels_lst:
						kernels_lst = '{} {}'.format(kernels_lst, kernel)

					single_image_list = {}
					single_image_list['image'] = image
					single_image_list['distro'] = long_distro

					single_image_list['sys_name'] = '{}-{}-{}'.format(image, ofed_tgz, kernel)
					if inbox_driver:
						ofed_ver = 'inbox_driver'
					else:
						ofed_ver = ofed_tgz.split('-')
						ofed_ver = 'o{}-{}'.format(ofed_ver[1], ofed_ver[2])
					if distro == 'EL':
						short_kernel = kernel.replace('.x86_64', '')
					else:
						short_kernel = kernel
					single_image_list['ofed'] = ofed_tgz
					single_image_list['kernel'] = kernel
					single_image_list['image_name'] = '{}-{}-k{}'.format(image, ofed_ver, short_kernel)

                                        single_image_list['compile_pager'] = should_compile_pager
                                        should_compile_pager = False
					
                                        image_description_list[image].append(single_image_list)

			for i in image_description_list[image]:
				i['ofeds'] = ofeds
				i['kernels'] = kernels_lst

	for image in image_description_list.keys():
		success_statuses[image] = execute_scripts(image, force=FORCE)
	return success_statuses

def copy_global_scripts(sources_path):
	print 'Copy global scripts'
	proc = subprocess.Popen('./copyscripts.sh {}'.format(sources_path), shell=True)
	proc.wait()

def create_rpm(kind, spec_file_name, image):  ##kind = server/client
	spec_file_path = os.path.join(GIT_SPEC_SOURCE_DIR, spec_file_name)
	sources_path = '{}/{}_{}/{}/'.format(OUTPUT_DIR, COMMIT_ID, ('512' if SECTOR_SHIFT else '4k'), image)
	target_dir = sources_path
	installers_dir = '.'

	distro = image_description_list[image][0]['distro']
	distro_version = image[1:]
	ofeds = image_description_list[image][0]['ofeds'].replace('none', 'inbox-driver')
	kernels = image_description_list[image][0]['kernels']
	bs = '512B' if SECTOR_SHIFT else '4KB'

	proc = subprocess.Popen(
			'cd {}; export BLOCK_SIZE=\'{}\'; export KERN_VER=\'{}\'; export OFED_VER_STRING=\'{}\'; export DISTRO=\'{}\' ; export DISTRO_VER=\'{}\'; export SIGN_RPM=\'{}\'; ./create_rpm.sh {} {} {} {} {}; cd -'.format(GIT_SPEC_SOURCE_DIR, bs, kernels, ofeds, distro, distro_version, str(SIGN_RPM).lower(), kind, sources_path, spec_file_path, installers_dir, target_dir), shell=True)
	proc.wait()
	print 'Finished creating RPM.'

def image_exists_and_is_running(domain_name):
	cmd = 'sudo virsh list --all | grep {} | grep -v base'.format(domain_name)
	res = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, shell=True)
	stdout, stderr = res.communicate()
	rc = res.wait()
	if rc != 0:
		return False, False
	else:
		if 'running' in stdout:
			return True, True
		return True, False

def stop_image(domain_name):
	stop_image_cmd = 'sudo virsh destroy {}'.format(domain_name)
	stop_res = subprocess.Popen(stop_image_cmd, shell=True)
	rc = stop_res.wait()
	if rc != 0:
		return False
	else:
		return True

def clean_images(domain_name):
	exists, is_running = image_exists_and_is_running(domain_name)

	if is_running:
		stop_image(domain_name)

	if exists:
		print 'Deleting {} VM and storage file'.format(domain_name)
		clean_command = 'sudo virsh undefine {0}; sudo rm -f /mnt/ext4/vm-images-new/{0}.qcow2'.format(domain_name)
		del_proc = subprocess.Popen(clean_command, shell=True)
		proc_res = del_proc.wait()
		if not proc_res == 0:
			sys.stderr.write('Unable to delete {} image\n'.format(domain_name))
		else:
			print '{} was deleted \n'.format(domain_name)
	else:
		sys.stderr.write('{} wasn\'t found\n'.format(domain_name))
		
def create_services_executables():
	print 'Creating services executables'
	proc = subprocess.Popen('sudo ./tmp/nvmesh/RPM/make_executables.sh', shell=True)
	proc.wait()
	
def compile_all_and_create_rpm():
	copy_git_repo()
	global existing_clones_array
	existing_clones_array = os.popen('sudo virsh list --all --name').read().split('\n')
	success_statuses = read_config_and_compile_all()
	proc = subprocess.Popen('stty sane', shell=True)
	
	create_services_executables()

	for image, success in success_statuses.iteritems():
		if success or FORCE:
			print 'image: {}'.format(image_description_list[image][0]['image'])
			print 'block_size: {}'.format('512B' if SECTOR_SHIFT else '4KB')
			print 'ofeds: {}'.format(image_description_list[image][0]['ofeds'].replace('none', 'inbox-driver'))
			print 'kernels: {}'.format(image_description_list[image][0]['kernels'])
			sources_path = '{}/{}_{}/{}/'.format(OUTPUT_DIR, COMMIT_ID, ('512' if SECTOR_SHIFT else '4k'), image)
			copy_global_scripts(sources_path)
			if os.path.isfile(os.path.join(GIT_SPEC_SOURCE_DIR, CORE_SPEC_FILE_NAME)):
				create_rpm('core', CORE_SPEC_FILE_NAME, image)
			else:
				create_rpm('target', SERVER_SPEC_FILE_NAME, image)
				create_rpm('client', CLIENT_SPEC_FILE_NAME, image)

	delete_clone_repo()

	allSucceeded = all(success_statuses.values())
	if not allSucceeded:
		sys.stderr.write('FAILED!!! One or more images have failed\n')

	print BLUE_COLOR_LOG.format('==== DONE ====')

	if not FORCE and not allSucceeded:
		sys.exit(1)

def main(argv):
	pid_file = 'program.pid'
	fp = open(pid_file, 'w')
	try:
		fcntl.lockf(fp, fcntl.LOCK_EX | fcntl.LOCK_NB)
	except IOError:
		# another instance is running
		print 'Another instance is running, you can only run one instance at a time'
		sys.exit(0)

	try:
		opts, args = getopt.getopt(argv, 'hc:m:b:i:o:d:k:fsp:S',
		                           ['help', 'compile-script-file=', 'dist-ofed-kernel-metric-file=', 'branch=',
		                            'commitid=', 'ofed-tgz-dir=', 'outputdir=', 'kvm-images-dir=', 'force',
		                            'sector-shift', 'private-repository=', 'sign-rpm'])
	except getopt.GetoptError:
		print './build_client_server_rpms.py -c <compile-script-file> -m <dist-ofed-kernel-metric-file> -b <branch> -i <commitid> -o <ofed-tgz-dir> -d <outputdir> -k <kvm-images-dir> -f <force> -s <sector-shift> -S <sign-rpm>'
		sys.exit(2)
	global FORCE
	FORCE = False
	global PRIVATE_REPOSITORY
	PRIVATE_REPOSITORY = 'nvidia'
	global SECTOR_SHIFT
	SECTOR_SHIFT = False
        global SIGN_RPM
        SIGN_RPM = False

	for opt, arg in opts:
		if opt == '-h':
			print './build_client_server_rpms.py -c <compile-script-file> -m <dist-ofed-kernel-metric-file> -b <branch> -i <commitid> -o <ofed-tgz-dir> -d <outputfile> -k <kvm-images-dir> -f <force> -s <sector-shift> -S <sign-rpm>'
			sys.exit()
		elif opt in ('-c', '--compile-script-file'):
			global COMPILE_SCRIPT_FILE
			COMPILE_SCRIPT_FILE = arg
		elif opt in ('-m', '--dist-ofed-kernel-metric-file'):
			global METRIC_FILE
			METRIC_FILE = arg
		elif opt in ('-b', '--branch'):
			global BRANCH
			BRANCH = arg
		elif opt in ('-i', '--commitid'):
			global COMMIT_ID
			COMMIT_ID = arg
		elif opt in ('-o', '--ofed-tgz-dir'):
			global OFED_DIR
			OFED_DIR = arg
		elif opt in ('-d', '--outputdir'):
			global OUTPUT_DIR
			OUTPUT_DIR = arg
		elif opt in ('-k', '--kvm-images-dir'):
			global KVM_IMAGES_DIR
			KVM_IMAGES_DIR = arg
		elif opt in ('-f', '--force'):
			FORCE = True
		elif opt in ('-s', '--sector-shift'):
			SECTOR_SHIFT = True
		elif opt in ('-p', '--private-repository'):
			PRIVATE_REPOSITORY = arg
                elif opt in ('-S', '--sign-rpm'):
                        SIGN_RPM = True
	#    clean_output_dir()
	compile_all_and_create_rpm()

if __name__ == '__main__':
	main(sys.argv[1:])
