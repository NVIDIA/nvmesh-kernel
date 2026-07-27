#!/usr/bin/env python3
import os, sys, json
from json import JSONDecoder

__author__ = 'DanielHsH'
__version__ = '0.01'
__date__ = '05/12/2016'

debug = 0;

def usage():
	print('Usage: ./parse_mult_fio_json.py <dir_of_fio_json_output_files> ');

def loads_json_obj_list(s):
	decoder = JSONDecoder()
	s_len = len(s)
	objs = []
	end = 0
	while end < s_len:
		while (s[end]=="\n"):
			end = end+1
		obj, end = decoder.raw_decode(s, idx=end)
		objs.append(obj)
		while (end < s_len) and (s[end]=="\n"):
			end = end+1
	if debug:
		print("Parsed:", len(objs), "fio runs\n-----------------------");
	return objs

def parse_file(file_path, attributes):
	content = open(file_path).read();
	all_runs = loads_json_obj_list(content); ##cur_run = json.loads(content)
	g_stats = [];
	for cur_run in all_runs:
		cur_run_name = cur_run["jobs"][0]["jobname"];
		attributes=cur_run_name.split("-");
		run_stats = {'engine': str(attributes[1]), 'block_size': int(attributes[2]), 'n_threads':len(cur_run["jobs"]), 'scenario':str(attributes[3])}
		engine = attributes[1]
		block_size = attributes[2]
		n_threads = len(cur_run["jobs"]);  ## number of jobs
		run_stats['read'] = {};
		run_stats['write'] = {};
		run_stats['trim'] = {};
		if debug:
			print('***> engine={}, block={}[bytes], threads={}, scenario={}'.format(engine,block_size,n_threads, attributes[3]));
		for rw in ("read", "write", "trim"):
			mean_lat_arr = []
			iops_arr = []
			bw_arr = []
			for cur_job in cur_run["jobs"]:
				ib_verb_stats = cur_job[rw]
				if ib_verb_stats["io_bytes"] == 0:
					continue;
				iops_arr.append(float(ib_verb_stats["iops"]))
				bw_arr.append(float(ib_verb_stats["bw"]))
				clat = ib_verb_stats["clat"]
				mean_lat_arr.append(float(clat["mean"]))
			if (len(mean_lat_arr) == 0):
				continue;
			mean_lat = sum(mean_lat_arr)/float(len(mean_lat_arr))
			mean_iops = sum(iops_arr)/float(len(iops_arr))
			mean_bw = sum(bw_arr)/float(len(bw_arr))
			run_stats[rw] = {'mean_lat': mean_lat, 'mean_iops': mean_iops, 'mean_bw':mean_bw}
			#print '\t {}, mean_lat={}[usec], iops={}, bw={}'.format(rw, mean_lat, mean_iops, mean_bw);
			#stdev = sqrt(s1^2 + s2^2 + ... + s12^2)
		g_stats.append(run_stats);
		if debug:
			print(run_stats);
	return g_stats

def parse_folder(folder, attributes):
	g_stats = {};
	os.system('find ' + folder + ' -type f > files_paths')
	file_paths=open("files_paths").read().split("\n")
	file_paths.remove("")
	os.system('rm files_paths')
	for file_path in file_paths:
		g_stats = parse_file(file_path, attributes)
	return g_stats

def calc_unique_params_for_graphs(g_stats):
	engines_list = [];
	blocks_sz_list = [];
	ib_verbs_list = ["read", "write", "trim"];
	scenarios_list = [];
	for run in g_stats:
		if run['engine'] not in engines_list:
			engines_list.append(run['engine']);
		if run['block_size'] not in blocks_sz_list:
			blocks_sz_list.append(run['block_size']);
		if run['scenario'] not in scenarios_list:
			scenarios_list.append(run['scenario']);
	if debug:
		print(engines_list, blocks_sz_list, scenarios_list);
	lists = {}
	lists['engines']   = engines_list;
	lists['blocks_sz'] = blocks_sz_list;
	lists['ibverbs']   = ib_verbs_list;
	lists['scenarios'] = scenarios_list;
	return lists

def print_single_report(g_stats, lists, cur_engine, ib_verb, y_column, y_units, y_scale, n_threads):
	blocks_sz_list = lists['blocks_sz'];
	graph   = dict((el,0) for el in blocks_sz_list);
	amounts = dict((el,0) for el in blocks_sz_list);
	for run in g_stats:
		if run['engine'] != cur_engine:
			continue;
		if run['n_threads'] != n_threads:
			continue;
		if not run[ib_verb]:
			continue;
		if debug:
			print(run['block_size'], run[ib_verb], run['scenario']);
		graph[  run['block_size']] += run[ib_verb][y_column]
		amounts[run['block_size']] += 1
	## R/W Random
	if debug:
		print(graph, amounts);
	n_measurements = 0;
	for s in blocks_sz_list:
		n_measurements += amounts[s];
		if amounts[s]>0:
			graph[s] /= amounts[s];
		else:
			graph[s] = 0;
	if n_measurements == 0:
		return;
	if n_threads==1:
		header = 'Single thread'
	else:
		header = 'rand-RW, {} thread'.format(n_threads);
	print('\t {}, {}, {} vs block_size:'.format(header, ib_verb,y_column));
	for s in blocks_sz_list:
		print('\t\t{:>5} : {:8.2f}{}'.format(s, graph[s]/y_scale, y_units));

def print_reports(g_stats, lists):
	blocks_sz_list = lists['blocks_sz'];
	for e in lists['engines']:
		print('Engine: ----------- {} -----------------------'.format(e));
		for n_threads in (1,8):
			for ib_verb in lists['ibverbs']:
				y_column='mean_bw'
				y_scale=1000
				y_units='[KB/s]'
				print_single_report(g_stats,lists,e,ib_verb,y_column, y_units, y_scale, n_threads);
				y_column='mean_lat'
				y_scale=1
				y_units='[u/Sec]'
				y_column='mean_iops'
				y_scale=1000
				y_units='[Kiops]'
				print_single_report(g_stats,lists,e,ib_verb,y_column, y_units, y_scale, n_threads);

def main(argv):
	if len(argv) < 1:
		usage()
		sys.exit(1)
	folder=argv[0]
	attributes=argv;
	del attributes[0]
	g_stats = parse_folder(folder, attributes)
	if debug:
		print(len(g_stats), g_stats);
	lists = calc_unique_params_for_graphs(g_stats);
	print_reports(g_stats, lists);
	sys.exit(0)

if __name__ == "__main__":
	main(sys.argv[1:])
