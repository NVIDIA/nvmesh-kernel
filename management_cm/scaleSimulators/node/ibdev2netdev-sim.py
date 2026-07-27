#!/usr/bin/env python3
import subprocess
from subprocess import Popen

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
	nic_name = csv_cols[0]
	result_line = '{device_ib_name} port 1 ==> ib{index} (Up)'.format(device_ib_name=nic_name, index=index)
	result_lines.append(result_line)


print('\n'.join(result_lines))
