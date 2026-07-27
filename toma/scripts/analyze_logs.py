#!/usr/bin/python2
# DanielHsH, 2025/Nov, this script is deprecated. Originally tried to find important stuff in Toma logs. Basically an elaborate grep. Not used in production

import sys, os, subprocess, errno, getopt, re

TOMA_LOG_DIR_NAME = '/var/log/nvmesh/'
log_files_list = ""

lockid_by_id = {}
is_longing_by_id = {}
is_awaiting_answer_by_id = {}
is_registered_by_id = {}
last_msg_by_id = {}
last_reason_by_id = {}
last_msg_by_lockid = {}
err_msgs = []
warn_msgs = []

def del_id_from_all_maps(id):
    lockid = 'blablabla'
    if (lockid_by_id.has_key(id)):
        lockid = lockid_by_id[id]
        del lockid_by_id[id]
    if (is_longing_by_id.has_key(id)):
        del is_longing_by_id[id]
    if (is_awaiting_answer_by_id.has_key(id)):
        del is_awaiting_answer_by_id[id]
    if (is_registered_by_id.has_key(id)):
        del is_registered_by_id[id]
    if (last_msg_by_id.has_key(id)):
        del last_msg_by_id[id]
    if (last_reason_by_id.has_key(id)):
        del last_reason_by_id[id]
    if (last_msg_by_lockid.has_key(lockid)):
        del last_msg_by_lockid[lockid]

def analyze_toma_log_file(log_files_list):

	tmp_file_name = "/tmp/" + sys.argv[0].split('/')[-1].split('.')[0]
	os.system("grep 'msgtype\\|Reject RoCE address on\\|TOMAerr\\|TOMAwarn\\|remove_active_registrant_from_disk_segment_by_idx\\|remove_longing_registrant_on_seg' " +
			  log_files_list + " > " + tmp_file_name)
	f = open(tmp_file_name)
	register_regexp = re.compile(r'msg_type=(?P<msg_type>\S*) reason=(?P<reason>\S*)'
								 ' registrant_node_uuid=(?P<registrant_node_uuid>\S*) segment_id=(?P<segment_id>\S*)'
								 ' praid_version=(?P<praid_version>\S*)\(ctx=(?P<ctx>\S*)\) lock_id=(?P<lock_id>\S*)'
								 ' data_len=(?P<data_len>\S*) handle=(?P<handle>\S*)')
	remove_regexp = re.compile(r'remove_active_registrant_from_disk_segment_by_idx.*\: seg=(?P<seg>\S*)'
							   ' lock_id=(?P<lock_id>\S*) handle=(?P<handle>\S*)')
	remove_longing_regexp = re.compile(r'remove_longing_registrant_on_seg\S* seg=(?P<seg>\S*) handle=0x(?P<handle>\S*)')
	n_lines = 0
	for line in f:
		n_lines += 1
		if (line.find("Reject RoCE address on") != -1):
			err_msgs.append("OOPS! '" + line.rstrip('\r\n') + "' check the netmask (ifconfig)")
			continue
		if (line.find("*TOMAerr*") != -1):
			err_msgs.append(line.rstrip('\r\n'))
			continue
		if (line.find("*TOMAwarn*") != -1):
			warn_msgs.append(line.rstrip('\r\n'))
			continue
		match = register_regexp.search(line)
		if (match is not None):
			msg_type = match.group('msg_type')
			reason = match.group('reason')
			registrant_node_uuid = match.group('registrant_node_uuid')
			segment_id = match.group('segment_id')
			praid_version = match.group('praid_version')
			praid_version_ctx = match.group('ctx')
			lockid = match.group('lock_id')
			data_len = match.group('data_len')
			handle = match.group('handle')
			# print msg_type, registrant_node_uuid, segment_id, praid_version, praid_version_ctx, lockid, data_len, handle
			id = segment_id + '-' + handle

			if (is_registered_by_id.has_key(id) and is_registered_by_id[id] and lockid_by_id.has_key(id) and (lockid_by_id[id] != lockid)):
				if ((msg_type != "TR_REGISTER_DISK_SEGMENT_NACK") and (msg_type != "TR_REGISTRABLE_DISK_SEGMENT")):
					print "lockid mismatch with", lockid_by_id[id], line

			if (not last_msg_by_id.has_key(id)):
				is_longing_by_id[id] = False
				is_awaiting_answer_by_id[id] = False

			if (lockid != "0"):
				lockid_by_id[id] = lockid

			last_msg_by_id[id] = msg_type
			last_reason_by_id[id] = reason
			last_msg_by_lockid[lockid] = msg_type

			if (msg_type == "TR_REGISTRABLE_DISK_SEGMENT"):
				is_longing_by_id[id] = False
			elif (msg_type == "RT_REGISTER_DISK_SEGMENT"):
				is_longing_by_id[id] = False
			elif (msg_type == "TR_REGISTER_DISK_SEGMENT_ACK"):
				is_awaiting_answer_by_id[id] = False
				is_registered_by_id[id] = True
			elif (msg_type == "TR_REGISTER_DISK_SEGMENT_NACK"):
				pass
			elif (msg_type == "TR_UNREGISTER_DISK_SEGMENT"):
				is_longing_by_id[id] = True
				is_awaiting_answer_by_id[id] = True
			elif (msg_type == "RT_UNREGISTER_DISK_SEGMENT"):
				pass
			elif (msg_type == "TR_UNREGISTER_DISK_SEGMENT_ACK"):
				is_registered_by_id[id] = False
			elif (msg_type == "TR_SWITCH_PRAID_TOPOLOGY"):
				is_awaiting_answer_by_id[id] = True
			elif (msg_type == "RT_SWITCH_PRAID_TOPOLOGY_ACK"):
				is_awaiting_answer_by_id[id] = False
			elif (msg_type == "TR_VOLUME_MISMATCH"):
				is_longing_by_id[id] = True
			elif (msg_type == "TR_TOMA_NOT_READY"):
				is_longing_by_id[id] = True
			elif (msg_type == "TR_INVALID_DISK_SEGMENT_ID"):
				is_longing_by_id[id] = True
			else:
				print "Invalid msg_type: ", msg_type
				exit
			continue

		match = remove_regexp.search(line)
		if (match is not None):
			# A client disconnect or whatever succeeded
			segment_id = match.group('seg')
			lockid = match.group('lock_id')
			handle = match.group('handle')
			id = segment_id + '-' + handle
			del_id_from_all_maps(id)

		match = remove_longing_regexp.search(line)
		if (match is not None):
			segment_id = match.group('seg')
			handle = match.group('handle')
			id = segment_id + '-' + handle
			if (not is_registered_by_id.has_key(id)):
				del_id_from_all_maps(id)
	f.close()

	print "--------------------------------  last_msg_by_client  --------------------------------------"
	print '{0:<33s} {1:<30s} {2:<30s} {3:>16s} {4:<17s} {5:<15s}'.format(
		"id", "state", "reason", "lockid", "is_client_longing", "is_toma_waiting")
	for id, state in last_msg_by_id.items():
		if (not is_registered_by_id.has_key(id)):
			is_registered_by_id[id] = False
		if (not lockid_by_id.has_key(id)):
			lockid_by_id[id] = "-1"
		if (not is_longing_by_id.has_key(id)):
			is_longing_by_id[id] = False
		if (not is_awaiting_answer_by_id.has_key(id)):
			is_awaiting_answer_by_id[id] = False
		#if ((not is_longing_by_id[id]) and (not is_awaiting_answer_by_id[id])):
		#	continue
		print '{0:<33s} {1:<30s} {2:<30s} {3:>16s} {4:<17b} {5:<15b}'.format(
			id, state, last_reason_by_id[id], lockid_by_id[id], is_longing_by_id[id], is_awaiting_answer_by_id[id])
	print "--------------------------------  TOMAerr  --------------------------------------"
	for line in err_msgs:
		print line
	print "--------------------------------  TOMAwarn  --------------------------------------"
	for line in warn_msgs:
		print line

	print "==== DONE ===="

def set_log_dir(log_dir):
	global	log_files_list

	os.chdir(log_dir)
	log_files_array = os.popen('ls -1t toma_[01].log').read().split('\n')
	log_files_list = " ".join(log_files_array)


def main(argv):
	global	log_files_list
	global 	TOMA_LOG_DIR_NAME

	help_string = sys.argv[0] + ' -d <toma-log-dir> OR -f <toma_log_file>'
	try:
		opts, args = getopt.getopt(argv,"hd:f:",["help", "toma-log-dir=", "toma-log-file="])
	except getopt.GetoptError:
		print help_string
		sys.exit(2)
	for opt, arg in opts:
		if (opt == '-h'):
			print help_string
			sys.exit()
		elif opt in ("-d", "--toma-log-dir"):
			TOMA_LOG_DIR_NAME = arg
			set_log_dir(TOMA_LOG_DIR_NAME)
		elif opt in ("-f", "--toma-log-file"):
			TOMA_LOG_DIR_NAME = "./"
			log_files_list = arg

	if (log_files_list == ""):
		set_log_dir(TOMA_LOG_DIR_NAME)
	print "============= Analyzing '" + log_files_list + "' in " + TOMA_LOG_DIR_NAME + " ================"

	analyze_toma_log_file(log_files_list)

if __name__ == "__main__":
	main(sys.argv[1:])

