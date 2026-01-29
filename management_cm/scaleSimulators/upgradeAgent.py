# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

import socket
import datetime
import subprocess
import time
import random
import confluent_kafka
import json

from component import Component
from consts import Components, Consts, MessageTypes, PeriodicMessagesIntervals


class UpgradeAgent(Component):
    def __init__(self):
        self.defaultKeepaliveInterval = PeriodicMessagesIntervals.UPGRADE_AGENT_KEEP_ALIVE
        Component.__init__(self, configFile=Consts.UPGRADE_AGENT_CONFIG_FILE)
        self.messageSequence = 0
        self.lastKeepAliveTime = None
        self.hostname = socket.gethostname()
        self.upgraderIndex = self.hostname.split('-')[1]
        self.additionalData = []
        self.upgradeAgentToken = -1
        self.mockCommands = {}
        self.featureCompatibilityVersion = '1'
        self.initialState = {}
        self.readConfigFromFile(updateInitState=True)

    def initState(self, initialState):
        self.logger.debug("initialState: {}".format(json.dumps(initialState, indent=4)))
        self.initialState = initialState

    def handleControls(self, controls):
        self.logger.debug("Got to handleControls. controls: {}".format(controls))
        for control in controls:
            action = control['type']
            if action == 'mock-command':
                command = control['command']
                self.mockCommands[command] = {
                    'stdOut': control.get('stdOut'),
                    'stdErr': control.get('stdErr'),
                    'exitCode': control.get('exitCode')
                }
            else:
                self.logger.error("Unknown control type %s in control %s" % (control['type'], control))
                return

    def getType(self):
        return Components.UPGRADE_AGENT

    def getInterestEvents(self):
        return []

    def getManagementTopicName(self):
        return 'default.management.priority.1.0.0'

    def getInterestConsumersConfig(self):
        topicsConfig = [{'name': '{}.upgradeAgent.commands.1.0.0'.format(self.hostname), 'partition': 0}]
        return topicsConfig

    def afterKafkaInitiated(self):
        self.sendKeepaliveMessage()

    def getMessageSequence(self):
        return self.messageSequence

    def getComponentSpecificMessageHeaders(self):
        return {
            'messageSequence': self.getMessageSequence(),
            'upgradeAgentID': self.hostname,
            'hostname': self.hostname,
            'upgradeAgentToken': self.upgradeAgentToken
        }

    def getLibrdkafkaVersion(self):
        version = confluent_kafka.libversion()
        return version[0] if version else None

    def collectData(self):
        payload = {
            "version": self.getVersion(),
            "librdkafkaVersion": self.getLibrdkafkaVersion(),
            "featureCompatibilityVersion": self.featureCompatibilityVersion,
            "additionalData": {d["key"]: self.runCmd(d["command"], d.get("args", [])) for d in self.additionalData}
        }
        payload.update(self.initialState)
        return payload

    def runMockCommand(self, cmd, args, timeout=None):
        # mock command delay
        delay = random.uniform(0.05, 5.0)
        time.sleep(delay)
        
        mockCommand = self.mockCommands.get(cmd)

        if mockCommand:
            self.logger.debug('Mocking command execution: {} with args: {}'.format(cmd, args))
            result = {
                "exitCode": mockCommand.get('exitCode'),
                "stdOut": mockCommand.get('stdOut'),
                "stdErr": mockCommand.get('stdErr'),
                "isError": mockCommand.get('exitCode') != 0,
            }
            self.logger.debug('Mock command result: {}'.format(result))
            return result

        return self.runCmd(cmd, args, timeout)

    def runShellCmd(self, cmd, timeout=None):
        self.logger.debug('Executing shell command: {}, timeout: {}'.format(cmd, timeout))
        res = subprocess.run(cmd, shell=True, capture_output=True, timeout=timeout)
        return {
            "exitCode": res.returncode,
            "stdOut": res.stdout.decode('utf-8').rstrip(),
            "stdErr": res.stderr.decode('utf-8').rstrip(),
            "isError": res.returncode != 0
        }

    def runCmd(self, cmd, args, timeout=None):
        self.logger.debug('Executing command: {} with args: {}, timeout: {}'.format(cmd, args, timeout))

        try:
            # uncomment to run a real command
            # cmd_str = " ".join([cmd] + args)
            # return self.runShellCmd(cmd_str, timeout)
            result = {
                "exitCode": 0,
                "stdOut": "Some command output",
                "stdErr": "",
            }
            self.logger.debug('Command result: {}'.format(result))

            return result
        except subprocess.TimeoutExpired:
            self.logger.error('Command execution timed out: {cmd}'.format(cmd=cmd))
            return {"isTimeout": True}
        except Exception as e:
            self.logger.error(f'Error executing command {cmd}: {e}')
            return {"isException": True, "exception": str(e)}

    def produceMessageToTopic(self, message, topic):
        Component.produceMessageToTopic(self, message, topic)
        self.messageSequence += 1

    def updateToken(self, token):
        tokenUpdated = False

        if token is not None and token > self.upgradeAgentToken:
            tokenUpdated = True
            self.upgradeAgentToken = token

        return tokenUpdated

    # UPGRADE_AGENT -> MGMT
    def producePeriodicReports(self):
        if not self.lastKeepAliveTime or (self.upgradeAgentToken >= 0 and Component.isTimeForNextMsg(self.lastKeepAliveTime, self.getKeepaliveInterval())):
            self.sendKeepaliveMessage()

    def sendKeepaliveMessage(self):
        try:
            payload = self.collectData()
            payload['health'] = 'healthy'
        except Exception as e:
            self.logger.error("Failed to collect data: {}".format(str(e)))
            payload = {
                'health': 'critical',
                'healthError': str(e)
            }

        message = self.buildGenericMessage(messageType=MessageTypes.UPGRADE_AGENT_KEEPALIVE, payload=payload)

        self.logger.debug('Going to send keepalive message, token: {}, messageSequence: {}'.format(message['upgradeAgentToken'], message['messageSequence']))
        self.produceMessageToTopic(message, self.getManagementTopicName())

        self.lastKeepAliveTime = datetime.datetime.now()

    def sendCommandResultMessage(self, resultData):
        upgradeStepID = resultData.get('upgradeStepID')
        success = resultData.get('success', False)

        payload = {
            'upgradeStepID': upgradeStepID,
            'command': resultData.get('command'),
            'verificationCommand': resultData.get('verificationCommand'),
            'success': success,
            'isTimeout': resultData.get('isTimeout', False),
            'isError': resultData.get('isError', False),
            'isVerification': resultData.get('isVerification', False)
        }
        message = self.buildGenericMessage(messageType=MessageTypes.UPGRADE_AGENT_COMMAND_RESULT, payload=payload)

        self.logger.debug('Going to send command result message for upgradeStepID: {} success: {}'.format(upgradeStepID, success))
        self.produceMessageToTopic(message, self.getManagementTopicName())

    # MGMT -> UPGRADE_AGENT
    def handleMessage(self, message):
        messageNotHandled = False
        messageType = message.get('messageType')
        payload = message.get('payload')
        token = payload.get('upgradeAgentToken')

        if token is not None and token < self.upgradeAgentToken:
            self.logger.warning('Received a message with an older token: {}'.format(token))
            return

        if token is not None and token > self.upgradeAgentToken:
            self.logger.debug('Updating upgrade agent token from {} to {}'.format(self.upgradeAgentToken, token))
            self.upgradeAgentToken = token

        if messageType == MessageTypes.UPDATE_UPGRADE_AGENT_KEEPALIVE_TOKEN:
            messageNotHandled = self.handleUpdateKeepaliveToken(payload)

        elif messageType == MessageTypes.UPGRADE_AGENT_COMMAND:
            messageNotHandled = self.handleUpgradeAgentCommand(payload)
        else:
            self.logger.warning('Unable to handle message messageType {}. Ignoring this message...'.format(messageType))

        return messageNotHandled

    def handleUpdateKeepaliveToken(self, payload):
        keepaliveInterval = payload.get('keepaliveInterval')
        token = payload.get('upgradeAgentToken')
        additionalData = payload.get('additionalData')
        messageSequence = payload.get('messageSequence')

        self.logger.debug(
            'Got update upgrade agent keepalive token message with keepaliveInterval: {} token: {}'
            .format(keepaliveInterval, token)
        )

        if messageSequence is not None and messageSequence + 1 > self.messageSequence:
            newMessageSeq = messageSequence + 1
            self.logger.debug('Updating upgrader messageSequence from {} to {}'.format(self.messageSequence, newMessageSeq))
            self.messageSequence = newMessageSeq

        if additionalData:
            self.additionalData = additionalData

        self.updateKeepAliveIntervalIfNeeded(keepaliveInterval)
        self.sendKeepaliveMessage()

    def handleUpgradeAgentCommand(self, payload):
        commandObj = payload.get('command')
        verificationCommand = commandObj.get('verificationCommand')
        upgradeStepID = payload.get('upgradeStepID')

        if verificationCommand:
            verificationRes = self.runShellCmd(verificationCommand)
            isError = verificationRes.get('isError')
            success = not isError

            self.logger.debug('Verification command output: {}'.format(verificationRes.get('stdOut')))

            if verificationRes.get('exitCode') == 0:
                self.logger.debug('Command already executed successfully, skipping execution')
                self.sendCommandResultMessage({
                    'upgradeStepID': upgradeStepID,
                    'verificationCommand': verificationRes,
                    'success': success,
                    'isError': isError,
                    'isVerification': True
                })
                return

        commandRes = self.runMockCommand(commandObj.get('cmd'), commandObj.get('args'), commandObj.get('timeout'))

        isTimeout = commandRes.get('isTimeout')
        isError = commandRes.get('isError')
        isException = commandRes.get('isException')
        success = not isTimeout and not isError and not isException

        self.sendCommandResultMessage({
            'upgradeStepID': upgradeStepID,
            'command': commandRes,
            'success': success,
            'isTimeout': isTimeout,
            'isError': isError,
            'isException': isException
        })


if __name__ == "__main__":
    upgradeAgent = UpgradeAgent()
    upgradeAgent.start()
