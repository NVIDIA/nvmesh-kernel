#!/usr/bin/env python3

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0

import csv
import logging
import sys
import argparse
import traceback
import json
import operator

PROFILING_CSV = "/proc/nvmeibc/volumes/{0}/profiling.csv"
JSON_STATUS = "/proc/nvmeibc/volumes/{0}/status.json"
PROFILING_INTERVALS_COUNT = 12
PROFILING_RETRY_COUNT = 3


def get_profiler_dict_from_file(csv_file, logger):
	output = dict()
	try:
		with open(csv_file, 'rt') as cf:
			csv_reader = csv.reader(cf, delimiter=',')
			line_count = 0
			header = 0
			n_stages = 0
			profiler = ""
			for row in csv_reader:
				if "profiler_description" in row:
					header = 2
					line_count += 1
					logger.debug("New profiler next")
				elif header == 2: # info regarding the profiler, name and n_stages
					profiler = row[0]
					n_stages = int(row[1])
					output[profiler] = {'stage_count':n_stages}
					line_count += 1
					header = 1
					logger.debug("Profiler: {0}. Has {1} Stages.".format(row[0], int(row[1])))
				elif header == 1: # This line has stage_name, count, average... as discriptions of the next line
					header = 0
					line_count += 1
					logger.debug("Description line {0}.".format(", ".join(row)))
				elif n_stages != 0 and header == 0:
					if output[profiler].get('stages', None) is None:
						output[profiler]['stages'] = []
					output[profiler]['stages'].append(row[0])
					output[profiler][row[0]] = {'count': int(row[1]), 'average': int(row[2]), 'error_count': int(row[3]), 'error_average': int(row[4])}
					output[profiler][row[0]].update({2**i:int(row[i+5]) for i in range(0,PROFILING_INTERVALS_COUNT)})
					output[profiler][row[0]].update({"{0}_retry".format(i+1):int(row[i*2+5+PROFILING_INTERVALS_COUNT]) for i in range(0,PROFILING_RETRY_COUNT)})
					output[profiler][row[0]].update({"{0}_time".format(i+1):int(row[i*2+6+PROFILING_INTERVALS_COUNT]) for i in range(0,PROFILING_RETRY_COUNT)})
					output[profiler][row[0]].update({"total_retry_time":sum([int(row[i*2+6+PROFILING_INTERVALS_COUNT]) for i in range(0,PROFILING_RETRY_COUNT)])})
					n_stages -= 1
					line_count += 1
					logger.debug("Added row {0}, to profiler: {1}.".format(", ".join(row), profiler))
					if n_stages > 0: # The next row describes the next stage, but we already know it
						header = 1
				else:
					logger.error("Error in row {0}.".format(" ".join(row)))
				logger.debug('Processed {0} lines.'.format(line_count))
	except Exception as e:
		if "line contains NULL byte" in str(e):
			pass
		else:
			logger.error(str(e))
			logger.error("Error parsing csv file {0}.".format(csv_file))
			sys.exit(1)
	return output

def get_json_status_from_file(status_file, logger):
	try:
		with open(status_file) as json_file:
			info = json.load(json_file)
	except:
		logger.error("Failure to parse json file {0}. Is the volume attached?".format(status_file))
		sys.exit(1)
	return info

def stage_has_count(stage):
	stage_count = stage.get('count', 0) + stage.get('error_count', 0)
	retry_count = sum([stage.get("{0}_retry".format(i+1), 0) for i in range(0,PROFILING_RETRY_COUNT)])
	return (stage_count + retry_count) > 0

def get_chunk_raid_and_segment_index_from_profiler_name(profiler, logger):
	#  <volume name> Lock OP (0/0/8)
	data = profiler.split("(")[1][:-1]
	# now we have X/Y/Z
	res = [int(index) for index in data.split("/")]
	logger.debug("Parsed profiler {0} into: ci={1}, ri={2}, si={3}.".format(profiler, res[0], res[1], res[2]))
	return res[0], res[1], res[2]

def get_disk_and_host_from_status_json(json_status, ci, ri, si, logger):
	try:
		return " " + json_status['topo']['chunks'][ci]['prs'][ri]['segs'][si]['disk']['host'] + "-" + json_status['topo']['chunks'][ci]['prs'][ri]['segs'][si]['disk']['name']
	except:
		logger.error("Could not get disk and host from json status for ci={0}, ri={1}, si={2}.".format(ci, ri, si))
		sys.exit(1)

if __name__ == "__main__":
	arg_parser = argparse.ArgumentParser("parse_csv")
	arg_parser.add_argument("-d", "--debug", action='store_true', help="print parse debug information")
	arg_parser.add_argument("-s", "--status-file", help="Volume status json file output for offline analysis")
	arg_parser.add_argument("-c", "--csv-file", help="Volume profiling csv file output for offline analysis")
	arg_parser.add_argument("-b", "--bucket", action='store_true', help="Display bucket information")
	arg_parser.add_argument("-o", "--output", help="Output file for plotting")
	arg_parser.add_argument("-v", "--volumes", nargs='*', help="volumes to be parsed")
	args = arg_parser.parse_args(sys.argv[1:])

	logger = logging.getLogger('parse_csv')
	stdout_handler = logging.StreamHandler(sys.stdout)
	stdout_handler.setFormatter(logging.Formatter('%(message)s'))
	if args.debug:
		logger.setLevel(logging.DEBUG)
	else:
		logger.setLevel(logging.INFO)
	logger.addHandler(stdout_handler)

	if args.status_file and not args.csv_file:
		logger.error("Volume status file given without a profiling csv file.")
		sys.exit(1)
	if args.csv_file and not args.status_file:
		logger.error("Volume profiling csv file given without a status file.")
		sys.exit(1)

	if args.volumes and (args.status_file or args.csv_file):
		logger.error("Must supply only volume names or both a status file and profiling csv file, cannot comply with both.")
		sys.exit(1)

	if args.status_file:
		try:
			json_status = get_json_status_from_file(args.status_file, logger)
			profilers_dict = get_profiler_dict_from_file(args.csv_file, logger)
			profilers_sorted = list(profilers_dict.keys());
			profilers_sorted.sort()
			for profi in profilers_sorted:
				for stage in profilers_dict[profi]['stages']:
					if stage_has_count(profilers_dict[profi][stage]):
						disk_and_host = ""
						if "OP" in profi:
							ci, ri, si = get_chunk_raid_and_segment_index_from_profiler_name(profi, logger)
							disk_and_host = get_disk_and_host_from_status_json(json_status, ci, ri, si, logger)
						keys_sorted = [key for key, value in profilers_dict[profi][stage].items() if not isinstance(key, int) and (value > 0 or key == "average")]
						keys_sorted.sort()
						measurements = ' '.join(["{0}: {1:>6}".format(key, profilers_dict[profi][stage][key], " [usec]" if key == "average" else '') for key in keys_sorted])
						logger.info("Profiler: {0:<32}|{1:<40}| stage {2:<16}: {3}.".format(profi, disk_and_host, stage, measurements))
						count = 0.0
						if args.bucket:
							line = ""
							result = dict()
							for i in range(PROFILING_INTERVALS_COUNT):
								count += profilers_dict[profi][stage][2**i]
								if profilers_dict[profi][stage][2**i] != 0:
									result[2**i] = profilers_dict[profi][stage][2**i]
							if count != profilers_dict[profi][stage].get('count', 0) + profilers_dict[profi][stage].get('error_count', 0):
								logger.debug("Bucket count {0} and profiler differ {1}.".format(count, profilers_dict[profi][stage].get('count', 0) + profilers_dict[profi][stage].get('error_count', 0)))
							dd = sorted(result.items(), key=operator.itemgetter(1), reverse=True)
							for k,v in dd.items():
								line += "{0}[usecs] [{1:4.2f}%] ".format(k, 100*(v/count))
							logger.info(line)
		except Exception as err:
				logger.error("Exception: {0}.".format(str(err)))
				logger.error("Traceback: {0}".format(traceback.format_exc()))

	elif args.volumes:
		for volume in args.volumes:
			try:
				json_status = get_json_status_from_file(JSON_STATUS.format(volume), logger)
				output = get_profiler_dict_from_file(PROFILING_CSV.format(volume), logger)
				logger.info("Volume {0} Profilers:".format(volume))
				profilers_sorted = list(output.keys());
				profilers_sorted.sort()
				for profi in profilers_sorted:
					for stage in output[profi]['stages']:
						if output[profi][stage].get('count', 0) != 0 or output[profi][stage].get('error_count', 0) != 0:
							disk_and_host = ""
							if "OP" in profi:
								ci, ri, si = get_chunk_raid_and_segment_index_from_profiler_name(profi, logger)
								disk_and_host = get_disk_and_host_from_status_json(json_status, ci, ri, si, logger)
							keys_sorted = [key for key, value in output[profi][stage].items() if not isinstance(key, int) and (value > 0 or key == "average")]
							keys_sorted.sort()
							measurements = ' '.join(["{0}: {1:>6}{2}".format(key, output[profi][stage][key], " [usec]" if key == "average" else '') for key in keys_sorted])
							logger.info("Profiler: {0:<32}|{1:<40}| stage {2:<16}: {3}.".format(profi, disk_and_host, stage, measurements))
							count = 0.0
							if args.bucket:
								line = ""
								result = dict()
								for i in range(PROFILING_INTERVALS_COUNT):
									count += output[profi][stage][2**i]
									if output[profi][stage][2**i] != 0:
										result[2**i] = output[profi][stage][2**i]
								if count != output[profi][stage].get('count', 0) + output[profi][stage].get('error_count', 0):
									logger.debug("Bucket count {0} and profiler differ {1}.".format(count, output[profi][stage].get('count', 0)+ output[profi][stage].get('error_count', 0)))
								dd = sorted(result.items(), key=operator.itemgetter(1), reverse=True)
								for (i,(k,v)) in enumerate(dd):
									line += "{0}[usecs] [{1:4.2f}%] ".format(k, 100*(v/count))
								if line:
									logger.info(line)

			except Exception as err:
				logger.error("Exception: {0}.".format(str(err)))
				logger.error("Traceback: {0}".format(traceback.format_exc()))
