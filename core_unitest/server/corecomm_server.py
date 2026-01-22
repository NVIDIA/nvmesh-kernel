#!/usr/bin/env python

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

import os
import re
import sys
import json
import rpyc
import uuid
import shlex
import ctypes
import logging
import argparse
import threading
import subprocess

sys.path.append("/opt/nvmesh/tools/dwarf2ctype")
import read_dwarf

from itertools import chain

LIBCORECOMM_PATH = os.path.join(os.path.dirname(__file__), "../libcorecomm.so")

DEVNULL = open(os.devnull, 'w+')

# Logger, initialized
log = logging.getLogger('corecomm-server')


# @TODO: Move subprocess related helpers to a separate module,
# shared between server and controller. Currently code is duplicated.

class LoggerWrapper(threading.Thread):
    """ Based on https://codereview.stackexchange.com/questions/6567/redirecting-subprocesses-output-stdout-and-stderr-to-the-logging-module
    """

    def __init__(self, level, header):
        super(self.__class__, self).__init__()

        # Required for clean program termination in case of deadlock
        self.daemon = True

        self.header = ('(' + header + ')') if header else ''
        self.level = level
        self.fdRead, self.fdWrite = os.pipe()

        self.pipeReader = os.fdopen(self.fdRead)

        self.start()

    def fileno(self):
        """ Return the write file descriptor of the pipe
        """
        return self.fdWrite

    def run(self):
        """ Thread's main funciton
        """
        while True:
            messageFromPipe = self.pipeReader.readline()

            if len(messageFromPipe) == 0:
                self.pipeReader.close()
                os.close(self.fdRead)
                return

            if messageFromPipe[-1] == os.linesep:
                messageToLog = messageFromPipe[:-1]
            else:
                messageToLog = messageFromPipe
            # end if

            lines = messageToLog.split(os.linesep)
            for line in lines:
                self.level(self.header + line)


def vpopen(cmd, stdout=None, stderr=None, **kwargs):
    """ Wrapper for Popen to easily make it silent
    """
    stdout = stdout if stdout else LoggerWrapper(log.debug, cmd[0])
    stderr = stderr if stderr else LoggerWrapper(log.warn, cmd[0])

    return subprocess.Popen(cmd, stdout=stdout, stderr=stderr, **kwargs)


def vcheck_output(cmd, stderr=None, **kwargs):
    """ Wrapper for Popen to easily make it silent
    """
    stderr = stderr if stderr else LoggerWrapper(log.warn, cmd[0])

    return subprocess.check_output(cmd, stderr=stderr, **kwargs)


class CorecommService(rpyc.Service):
    def __init__(self):
        super(CorecommService, self).__init__()
        (
            self.funcs,
            self.types,
            self.enums,
            self.dll
        ) = read_dwarf.extract_funcs_first_full_interface_as_bunch(
            read_dwarf.DumbTextParserDwarf2PyEngine(LIBCORECOMM_PATH),
            filter="corecomm.*")
        self.ctypes = ctypes  # Need to fully expose ctypes
        self.DEVNULL = DEVNULL

    def shell(self, cmd, stderr=None, stdin=DEVNULL):
        if (type(cmd) == str):
            cmd = ["bash", "-c", cmd]
        log.debug("Executing " + str(cmd))
        return vcheck_output(cmd, stderr=stderr, stdin=stdin)

    def get_bdf(self, disk_name):
        match = re.match(r'(.+)\.[0-9]', disk_name)
        if match:
            disk_name = match.group(1)

        devices = json.loads(self.shell(
            ['nvme', 'list', '-o', 'json'], stderr=DEVNULL))["Devices"]

        device = next((device for device in devices if device.get(
            "SerialNumber") == disk_name))

        device_name = device["DevicePath"][5:]

        bdf = os.readlink(os.path.join("/sys/block", device_name))

        match = re.match(r'.*/([^\/]+)/(nvme|block)/.*', bdf)
        if match:
            bdf = match.group(1)
        else:
            raise NameError("Malformed bdf {0}".format(bdf))

        return bdf

    def bind_disk(self, disk_name):
        if self.is_disk_bound(disk_name):
            log.info("Disk {0} is bound already".format(disk_name))
            return
        bdf = self.get_bdf(disk_name)
        with open("/sys/bus/pci/drivers/nvme/unbind", "w") as f:
            f.write(bdf)
        with open("/sys/bus/pci/drivers/nvmeibs/bind", "w") as f:
            f.write(bdf)

    def unbind_disk(self, disk_name):
        if self.is_disk_unbound(disk_name):
            log.info("Disk {0} is not bound already".format(disk_name))
            return
        bdf = self.get_bdf(disk_name)
        with open("/sys/bus/pci/drivers/nvmeibs/unbind", "w") as f:
            f.write(bdf)
        with open("/sys/bus/pci/drivers/nvme/bind", "w") as f:
            f.write(bdf)

    def write_gpt_disk(self, dev_name, excelero_md_part_size, data_partitions=None):
        """ This one emulates toma behaviour for writing GPT. In a less idiotic way.
        """
        if data_partitions is None:
            data_partitions = [("+0", "-0", str(uuid.uuid4()).lower())]

        excelero_md_part_size = "+" + str(excelero_md_part_size)

        # Building the command to send to gdisk
        gdisk_cmd = [dev_name, "", "", "",  # Skip some lines
                     "o", "Y", "", "x", "n", "m", "p",  # Erase old gpt
                     # excelero_metadata
                     "n", "", "+0", excelero_md_part_size, "3CA24AA3-D8FA-1C6B-9D0B-07ABC42764A6",
                     "n", "", "+0", "+2G", "426A4B11-8886-7A0F-3BCE-D84114FDFE90",  # excelero_journal_data
                     "n", "", "+0", "+32M", "44E54508-3B09-FC55-94D4-FF6351D079AE",  # excelero_serjio_db
                     "c", "1", "excelero_metadata", "c", "2", "excelero_journal_data", "c", "3", "excelero_serjio_db",  # Rename
                     ]

        # Add data partitions
        for i, dp in enumerate(data_partitions, start=4):
            gdisk_cmd.extend(
                ["n", "", dp[0], dp[1], "E4084CC9-6A17-FB2D-E554-6E50C747A395"])  # Create
            gdisk_cmd.extend(["x", "c", str(i), dp[2], "m"])  # Set GUID
            gdisk_cmd.extend(["c", str(i), dp[2]])  # Set name

        gdisk_cmd.extend(["p", "w", "Y", "", ""])  # Write to disk

        p = vpopen(['gdisk'], stdin=subprocess.PIPE)
        p.stdin.write("\n".join(gdisk_cmd))  # Execute the command
        p.stdin.flush()
        p.stdin.close()

        return p.wait()

    def read_partitions_info(self, dev_name):
        """ Read data partition information, done in a very straightforward way,
            could be done optimized but this is really not crucial.
        """
        p = vpopen(['gdisk', dev_name], stdin=subprocess.PIPE,
                   stdout=subprocess.PIPE)
        for i in range(1, 20):
            p.stdin.write("\n".join(['i', str(i)]))
            p.stdin.write("\n")
        p.stdin.flush()
        p.stdin.close()

        parts = []
        part = {}
        for line in p.stdout.readlines():
            match = re.match(r".*Partition GUID code: (.*) \(", line)
            if match:
                if part:
                    parts.append(part)
                part = {
                    "guid_code": match.group(1),
                    "sector_size": 4096 #@TODO: make this dynamic
                    }
                continue
            match = re.match(r"Partition unique GUID: (.*)", line)
            if match:
                part["uuid"] = match.group(1)
                continue
            match = re.match(r"First sector: (\d*)", line)
            if match:
                part["first_sector"] = int(match.group(1))
                continue
            
            match = re.match(r"Last sector: (\d*)", line)
            if match:
                part["last_sector"] = int(match.group(1))
                continue

        
        if part:
            parts.append(part)

        p.stdout.close()
        p.wait()

        return parts

    def read_partitions_info_filtered(self, dev_name):
        """ Read data partition information, extract only data partitions
        """
        p = vpopen(['gdisk', dev_name], stdin=subprocess.PIPE,
                   stdout=subprocess.PIPE)
        for i in range(4, 20):
            p.stdin.write("\n".join(['i', str(i)]))
            p.stdin.write("\n")
        p.stdin.flush()
        p.stdin.close()

        parts = []
        for line in p.stdout.readlines():
            match = re.match(r"Partition unique GUID: (.*)", line)
            if match:
                parts.append(match.group(1))

        p.stdout.close()
        p.wait()

        return parts

    def is_disk_unbound(self, disk_name):
        """ Actually not the same as not is_disk_bound, pay attention
        """
        bdf = self.get_bdf(disk_name)
        return any(path for path in chain.from_iterable(os.walk("/sys/bus/pci/drivers/nvme")) if bdf in path)

    def is_disk_bound(self, disk_name):
        bdf = self.get_bdf(disk_name)
        return any(path for path in chain.from_iterable(os.walk("/sys/bus/pci/drivers/nvmeibs")) if bdf in path)

    def set_module_param(self, module, param, value):
        with open("/sys/module/{0}/parameters/{1}".format(module, param), "w") as f:
            f.write(str(value))


def init_argparser():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", dest="hostname", default="0.0.0.0", type=str)
    parser.add_argument("--port", dest="port", default=18861, type=int)
    parser.add_argument("--dbg", dest="dbg", type=str, default='INFO', choices=['CRITICAL',
                                                                                'ERROR',
                                                                                'WARNING',
                                                                                'INFO',
                                                                                'DEBUG',
                                                                                'NOTSET'])
    return parser


def main():
    args = init_argparser().parse_args()
    logging.basicConfig(stream=sys.stdout,
                        level=args.dbg,
                        format="%(levelname)7s: %(name)s: %(message)s")

    log.info("Going to start server on port={0} hostname={1}".format(
        args.port, args.hostname))

    rpyc.ThreadedServer(CorecommService,
                        port=args.port,
                        hostname=args.hostname,
                        protocol_config={
                            'allow_public_attrs': True
                        }).start()
    return 0


if __name__ == "__main__":
    exit(main())
