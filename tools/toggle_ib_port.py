#!/usr/bin/env python2

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

import argparse
from infraClient.common.Consts import SwitchHandler
from infraClient.common import LoggerUtils
from infraClient.common.SwitchManager import SwitchManager
from infraClient.common.SystemUtils import SystemUtils
from infraClient.common.Consts import SwitchHandler
import random
import time

def printSwitchPortState():
	print 'switch:{} port:{} Info: {}'.format(args.switchAddress, args.port, switch.getswitchInterfaceIbInfo(args.port))

def sleepRandom(maxTime, port, op):
	randomTimeToSleep = random.randint(0, maxTime)
	print '{} {} sleep({})'.format(port, op, randomTimeToSleep)
	time.sleep(randomTimeToSleep)

if __name__ == '__main__':
	logger = LoggerUtils.getInfraClientLogger(__file__)
	parser = argparse.ArgumentParser(description=__file__)
	parser.add_argument('--switchAddress', type=str, required=True, help='switchAddress')
	parser.add_argument('--port', type=str, required=True, help='switch Port')
	parser.add_argument('--dt', type=int, required=True, help='DownTime sec')
	parser.add_argument('--ut', type=int, required=True, help='UpTime sec')
	args = parser.parse_args()
	switch = SwitchManager(switchAddress=args.switchAddress)
	printSwitchPortState()

	while True:
		print 'setting port {} to {}'.format(args.port, SwitchHandler.INTERFACE_DOWN)
		switch.setSwitchInterfaceState(args.port, SwitchHandler.INTERFACE_DOWN)
		# printSwitchPortState()
		
		sleepRandom(args.dt, args.port, "down")
		
		print 'setting port {} to {}'.format(args.port, SwitchHandler.INTERFACE_UP)
		switch.setSwitchInterfaceState(args.port, SwitchHandler.INTERFACE_UP)
		# printSwitchPortState()
		
		sleepRandom(args.ut, args.port, "up")
