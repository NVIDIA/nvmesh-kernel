#!/usr/bin/python2

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

import json
import re
import sys
import os
import fileinput
import argparse
from collections import deque
import uuid

hostname_re = r'(([a-zA-Z0-9]|[a-zA-Z0-9][a-zA-Z0-9\-]*[a-zA-Z0-9])\.)*([A-Za-z0-9]|[A-Za-z0-9][A-Za-z0-9\-]*[A-Za-z0-9])'
timestamp_re = r'\[(\d{1,2}:\d{2}:\d{2}\.)(\d+)\](\s\(\+?\d\.?\d{9}\))?'
seq_num_cpu_id_re = r'(nvmeib[csb])_((SHORT)|(LONG)|(EPH))_(\S+): { packet_seq_num = \d+, cpu_id = (\d+) }'
variable_list_re = r'{ (([a-zA-Z_$][a-zA-Z_$0-9]*) = (\w*)|("\w*")(, ([a-zA-Z_$][a-zA-Z_$0-9]*) = (\w*)|("\w*"))*) }'


def uuid_list_to_str(long_array):
    pattern = r'\[ \[0\] = (\d+), \[1\] = (\d+) \]'
    match = re.search(pattern, long_array)
    if match is None:
        raise "should match, weird..."
    return str(uuid.UUID(str((int(match.group(2)) << 64) + int(match.group(1)))))

def parse_printf(c_identefier):
    foo = lambda(x): x
    if c_identefier[3]: # %%
        return '%', foo
    if c_identefier[2]: # %pUB
        return '{}', uuid_list_to_str
    if c_identefier[1] in 'dsius':
        return '{}', foo        
    elif c_identefier[1] in  'xXp':
        return '0x{0:x}', foo
    elif c_identefier[1] in  'o':
        return '0{0:o}', foo
    elif c_identefier[1] in 'c':
        return '{}', unichr
    elif c_identefier[1] == 'nfFeEgGaA':
        sys.exit(-1)
    else: 
        sys.exit(-1)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('-b', '--babeltrace', help='The output of babeltrace, defaults to stdin')
    #parser.add_argument('output', help='This script\'s generated output')
    parser.add_argument('-d', '--dictionary', help='dictionary.json file', default=os.path.join(os.path.dirname(os.path.realpath(__file__)), 'dictionary.json') )
    parser.add_argument('-t', '--nvmeibt_trace', help='paths for nvmeibc[sct].trace.json', default=os.path.join(os.path.dirname(os.path.realpath(__file__)), '..', 'toma', 'nvmeibt.trace.json'))
    parser.add_argument('-s', '--nvmeibs_trace', help='paths for nvmeibc[sct].trace.json', default=os.path.join(os.path.dirname(os.path.realpath(__file__)), '..', 'srv', 'nvmeibs.trace.json'))
    parser.add_argument('-c', '--nvmeibc_trace', help='paths for nvmeibc[sct].trace.json', default=os.path.join(os.path.dirname(os.path.realpath(__file__)), '..', 'clnt', 'nvmeibc.trace.json'))
    args = parser.parse_args()

    dictionary = None
    traces = {}

    with open(args.dictionary) as file:
        dictionary = json.load(file)

    if args.nvmeibt_trace:
        with open(args.nvmeibt_trace) as file:
            traces['nvmeibt'] = json.load(file)
    if args.nvmeibs_trace:
        with open(args.nvmeibs_trace) as file:
            traces['nvmeibs'] = json.load(file)
    if args.nvmeibc_trace:
        with open(args.nvmeibc_trace) as file:
            traces['nvmeibc'] = json.load(file)

    babeltrace_output = open(args.babeltrace) if args.babeltrace else sys.stdin

    #trace_entries = [];
    babeltrace_content = babeltrace_output # .read()
    pattern = r'((\[(\d{1,2}:\d{2}:\d{2}\.)(\d+)\](\s\(\+?[\d\?]+\.?[\d\?]{9}\))?)\s((([a-zA-Z0-9]|[a-zA-Z0-9][a-zA-Z0-9\-]*[a-zA-Z0-9])\.)?(([a-zA-Z0-9]|[a-zA-Z0-9][a-zA-Z0-9\-]*[a-zA-Z0-9])\.)*([A-Za-z0-9]|[A-Za-z0-9][A-Za-z0-9\-]*[A-Za-z0-9]))\s(nvmeib[cst])[_:]((SHORT)|(LONG)|(EPH))_(\S+): { (packet_seq_num = \d+, )?cpu_id = (\d+) }, { (([a-zA-Z_$][a-zA-Z_$0-9]*) = ((\w+)|((\w+)|("(?:[^"\\]|\\.)*)"))(, ([a-zA-Z_$][a-zA-Z_$0-9]*) = ((\w+)|("(?:[^"\\]|\\.)*)"))*)? })'
    pattern = r'((\[(\d{1,2}:\d{2}:\d{2}\.)(\d+)\](\s\(\+?[\d\?]+\.?[\d\?]{9}\))?)\s((([a-zA-Z0-9]|[a-zA-Z0-9][a-zA-Z0-9\-]*[a-zA-Z0-9])\.)?(([a-zA-Z0-9]|[a-zA-Z0-9][a-zA-Z0-9\-]*[a-zA-Z0-9])\.)*([A-Za-z0-9]|[A-Za-z0-9][A-Za-z0-9\-]*[A-Za-z0-9]))\s(nvmeib[cst])[_:]((SHORT)|(LONG)|(EPH))_(\S+): { (packet_seq_num = \d+, )?cpu_id = (\d+) }, { (([a-zA-Z_$][a-zA-Z_$0-9]*) = ((\w+)|((\w+)|("(?:[^"\\]|\\.)*)")|(\[ \[0\] = (\d+), \[1\] = (\d+) \]))(, ([a-zA-Z_$][a-zA-Z_$0-9]*) = ((\w+)|("(?:[^"\\]|\\.)*)")|(\[ \[0\] = (\d+), \[1\] = (\d+) \]))*)? })'
    for line in babeltrace_content:
    	for m in re.findall(pattern, line):
    	    #print m[0]
    	    #continue
    	    variables_list = deque()
    	    timestamp = m[0]
    	    cpu_id = m[18] #int?
    	    variables = m[19]
    	    trace_name = m[16]
    	    module = m[11]
    	    hostname = m[4]
    	    trace_type = m[12] # EPH/LONG/SHORT
    	    m_variables = re.findall(r'([a-zA-Z_$][a-zA-Z_$0-9]*) = ((\w+)|((\w+)|("(?:[^"\\]|\\.)*)"))', variables)
    	    #breakpoint()
    	    for variable in m_variables:
    	        val =  int(variable[1]) if variable[2] else variable[1]
    	        variables_list.append((variable[0], val))
    	    #trace_entries.append([timestamp, hostname, cpu_id, module, variables])

    	    trace = traces[module][trace_name]

    	    timestamp_string = "{}{:,}".format(m[2], int(m[3]))
    	    hostname_string = "{}/{}".format(hostname.split('.')[0], cpu_id)
    	    file_line_str = "{} +{}".format(trace['file'], trace['line'])
    	    mod_line = ' '.join([timestamp_string, hostname_string, file_line_str])

    	    identefier_re = r'@[a-zA-Z_$][a-zA-Z_$0-9]*'
    	    identefiers = re.findall(identefier_re, trace['fmt'])
    	    trace_formatted = trace['fmt']
    	    #breakpoint()
    	    for identefier in identefiers:
    	        dictjson_identefier_fmt = dictionary[identefier]['fmt']
    	        formatted_dictjson_identefier = dictjson_identefier_fmt.replace('{','{{')
    	        formatted_dictjson_identefier = dictjson_identefier_fmt.replace('}','}}')
    	        printf_fmt_re = r'(%(?:(?:[-+0 #]{0,5})(?:\d+|\*)?(?:\.(?:\d+|\*))?(?:h|l|ll|w|I|I32|I64)?([cCdiouxXeEfgGaAnpsSZ]))|(%%))'
    	        printf_fmt_re = r'(%(?:(?:[-+0 #]{0,5})(?:\d+|\*)?(?:\.(?:\d+|\*))?(?:h|l|ll|w|I|I32|I64)?((pUB)|[cCdiouxXeEfgGaAnpsSZ]))|(%%))'
    	        m = re.findall(printf_fmt_re, dictjson_identefier_fmt)
    	        for c_identefier in m:
    	            python_format_identefier, foo = parse_printf(c_identefier)
    	            formatted_dictjson_identefier = formatted_dictjson_identefier.replace('{', '{{').replace('}', '}}').replace(c_identefier[0], python_format_identefier, 1).format(foo(variables_list.popleft()[1]))
    	        trace_formatted = trace_formatted.replace(identefier, formatted_dictjson_identefier, 1)

    	    #assert(trace["type"] == trace_type)
    	    print(mod_line + ": " + trace_formatted)

if __name__ == "__main__":
    main()



#\[(\d{1,2}:\d{2}:\d{2}\.)(\d+)\](\s\(\+?\d\.?\d{9}\))?\s(([a-zA-Z0-9]|[a-zA-Z0-9][a-zA-Z0-9\-]*[a-zA-Z0-9])\.)?(([a-zA-Z0-9]|[a-zA-Z0-9][a-zA-Z0-9\-]*[a-zA-Z0-9])\.)*([A-Za-z0-9]|[A-Za-z0-9][A-Za-z0-9\-]*[A-Za-z0-9])\s(nvmeib[csb])_((SHORT)|(LONG)|(EPH))_(\S+): { packet_seq_num = \d+, cpu_id = (\d+) }, { ([a-zA-Z_$][a-zA-Z_$0-9]*) = (\w*)|("\w*")(, ([a-zA-Z_$][a-zA-Z_$0-9]*) = (\w*)|("\w*"))* }

