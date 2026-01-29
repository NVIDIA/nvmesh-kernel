#!/usr/bin/env python3

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

import argparse
import sys

from component import Component
from consts import Components, MessageOpCode, Consts, PeriodicMessagesIntervals


class Target(Component):
	def __init__(self):
		self.defaultKeepaliveInterval = PeriodicMessagesIntervals.TOMA_KEEP_ALIVE
		Component.__init__(self, configFile=Consts.TARGET_CONFIG_FILE)

	def getType(self):
		return Components.TARGET

	def getInterestEvents(self):
		return []

	def handleManagementConfiguration(self, configuration):
		pass

	def handleMessage(self, message):
		self.logger.debug("TARGET Received message: {}".format(message))
		if not 'opcode' in message and 'payload' in message:
			message = message['payload']

		opcode = message['opcode']
		payload = message['payload']

		if opcode == MessageOpCode.CONFIGURATION_CHANGE_MESSAGE:
			self.handleManagementConfiguration(payload)
		else:
			self.logger.warning("Handling of message with opcode %d is Not Implemented" % opcode)

if __name__ == "__main__":
	target = Target()
	target.start()



