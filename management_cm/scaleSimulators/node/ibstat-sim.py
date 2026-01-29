#!/usr/bin/env python3

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0


import subprocess
from subprocess import Popen

output_template = '''CA '{nic}'
	CA type: MT4117
	Number of ports: 1
	Firmware version: 14.22.1002
	Hardware version: 0
	Node GUID: 0x248a0703002fde26
	System image GUID: 0x248a0703002fde26
	Port 1:
		State: Active
		Physical state: LinkUp
		Rate: 0
		Base lid: 0
		LMC: 0
		SM lid: 0
		Capability mask: 0x04010000
		Port GUID: 0x268a07fffe2fde26
		Link layer: {link_layer}
'''


def parseTransport(valueFromCsv):
	translation_dict = {
		'I': 'InfiniBand',
		'R': 'Ethernet',
		'T': 'Ethernet'
	}

	if valueFromCsv in translation_dict:
		return translation_dict[valueFromCsv]
	else:
		return None

cmd = 'cat /tmp/statistics/nvmeibs/nics.csv'

p = Popen(cmd, shell=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
stdout, stderr = p.communicate()

if not stdout:
	exit(0)

lines = stdout.split('\n')

result_lines = []
# we ignore first line and the last line, since the first is the column names, and the last is an empty line
for index, line in enumerate(lines[1:-1]):
	csv_cols = line.split(',')
	ib_name = csv_cols[0]
	transport = parseTransport(csv_cols[4])
	output = output_template.format(nic=ib_name, link_layer=transport)
	print(output)
