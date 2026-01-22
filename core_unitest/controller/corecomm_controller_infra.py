#!/usr/bin/env python
# PYTHON_ARGCOMPLETE_OK

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

import os
import sys
import rpyc
import subprocess
import json
import uuid
import time
import atexit
import logging
import threading
import contextlib

# Constants
SERVER_INSTALL_DIR = "/tmp/corecomm_server"
CORECOMM_SRC_ROOT = os.path.abspath(
    os.path.join(os.path.dirname(__file__), ".."))
BINJE_ALLOWED_VALUES = [1, 2, 4, 8, 16]

SSH_CMD = ["ssh", "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null"]

DEVNULL = open(os.devnull, 'rw')


log = logging.getLogger('corecomm-infra')

free_port = 0

# @TODO: Move subprocess related helpers to a separate module,
# shared between server and controller. Currently code is duplicated.

def wait_until(somepredicate, timeout, period=1, *args, **kwargs):
    mustend = time.time() + timeout
    while time.time() < mustend:
        try:
            if somepredicate(*args, **kwargs):
                return True
        except:
            pass
        time.sleep(period)
    return False

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


def vcheck_call(cmd, stdout=None, stderr=None, **kwargs):
    """ Wrapper for Popen to easily make it silent
    """
    stdout = stdout if stdout else LoggerWrapper(log.debug, cmd[0])
    stderr = stderr if stderr else LoggerWrapper(log.warn, cmd[0])

    return subprocess.check_call(cmd, stdout=stdout, stderr=stderr, **kwargs)


def vcheck_output(cmd, stderr=None, **kwargs):
    """ Wrapper for Popen to easily make it silent
    """
    stderr = stderr if stderr else LoggerWrapper(log.warn, cmd[0])

    return subprocess.check_output(cmd, stderr=stderr, **kwargs)


# This is used to make sure no stale connections remain after exit
processes = []


@atexit.register
def process_killer():
    for process in processes:
        try:
            process.kill()
        except Exception as e:
            pass


class Bunch:
    def __init__(self, **kwds):
        self.as_dict = kwds
        self.__dict__.update(kwds)

    def _bunch_update(self, **kwds):
        self.as_dict.update(kwds)
        self.__dict__.update(kwds)

    def _bunch_get(self, val):
        self.__dict__.get(val)

    def __str__(self):
        return str({k: str(vars(self)[k]) for k in vars(self)})

    def __repr__(self):
        return str(self)


class CorecommServerConnection(object):
    def __init__(self, node, server_install_dir=SERVER_INSTALL_DIR, host_mask="0.0.0.0", port=18861, dbg='INFO'):

        self.node = node
        self.host_mask = host_mask
        self.port = port
        self.dbg = dbg
        self.server_install_dir = server_install_dir

        global free_port
        if free_port <= port:
            free_port = port + 1

        try:  # Start the server process on remote
            cmd = SSH_CMD + ["-t", "-t",
                   node,
                   "bash",
                   "-c",
                   "'cd {0} && ./start-server.sh --host {1} --port {2} --dbg {3}'".format(os.path.join(server_install_dir, "server"), host_mask, port, dbg)]
            log.debug('Server start command: ' + str(cmd))
            self.server_process = vpopen(cmd, stdin=DEVNULL)
            processes.append(self.server_process)
        except subprocess.CalledProcessError as e:
            raise RuntimeError(
                "Error during server start: {0} returned {1}".format(e.cmd, e.returncode))

        self.conn = None
        retry = 10
        while retry > 0:  # Wait for connection - we are installed so should be fast
            try:
                self.conn = rpyc.connect(node, port=port)
                break
            except:
                retry -= 1
                time.sleep(0.5)

        if not self.conn:
            raise Exception(
                "Could not connect to RPC server on node {0}".format(node))

        # Basically here we are done.
        # Next jsut setting up some shortcut functions, only for comfort, not a must
        self.ctypes = self.conn.root.ctypes
        self.pointer = self.conn.root.ctypes.pointer
        self.memmove = self.conn.root.ctypes.memmove
        self.POINTER = self.conn.root.ctypes.POINTER
        self.cast = self.conn.root.ctypes.cast
        self.shell = self.conn.root.shell
        self.async_shell = rpyc.async_(self.conn.root.shell)

        self.dll = self.conn.root.dll
        self.types = self.conn.root.types
        self.funcs = self.decorate_funcs(self.conn.root.funcs)
        self.enums = self.conn.root.enums

        # Craete connection
        self.corecomm_handle = self.funcs.corecomm_create().value
        if self.corecomm_handle == 0:
            raise OSError(self.errno, "Error {0} trying to connect to {1}".format(
                self.errno, self.node))

        self.refresh_fields()

    def refresh_fields(self):
        self.hostname = self.get_hostname()
        self.drives = self.get_drives()
        self.nics = self.get_nics()

    def __del__(self):
        try:
            log.debug("Connection to node {0} destructor".format(self.node))
            if (self.server_process):
                self.server_process.kill()
                processes.remove(self.server_process)
        except Exception as e:
            pass

    @contextlib.contextmanager
    def dup(self):
        """ Clone self, resulting another connection object to be used cocurrently
        """
        cp = CorecommServerConnection(
            self.node, self.server_install_dir, self.host_mask, free_port, self.dbg)
        try:
            yield cp
        finally:
            del cp

    def set_sym(self, sym, value, size_bytes=None, deref=False):
        if type(value) == str:
            size_bytes = size_bytes if size_bytes else len(value) + 1
            tmp = self.types.struct__page_container.ctype()
            self.ctypes.memmove(self.pointer(tmp), value, size_bytes)
            self.funcs.corecomm_set_symbol(
                sym, size_bytes, self.pointer(tmp), deref)
        if type(value) == int:
            size_bytes = size_bytes if size_bytes else 4
            value = self.ctypes.c_ulong(value)
            tmp = self.types.struct__page_container.ctype()
            self.ctypes.memmove(self.pointer(
                tmp), self.pointer(value), size_bytes)
            self.funcs.corecomm_set_symbol(
                sym, size_bytes, self.pointer(tmp), deref)

    def discover_raw(self, disk_name, srv=None, wait_for_serjio_timeout=0):
        """ Run corecomm_discover, return python disk object,
            aggregating all available ops on disk
        """

        class invoke_on_disk_decorator():
            """ Decorate native function -
                invoke function with disk handle as first argument
            """
            def __init__(self, disk, func):
                # type func : PyCFunc on remote
                self.disk = disk
                self.func = func

                self.restypestr = func.arglist[0]["type"].name
                self.restype = func.restype
                self.name = func.name
                self.arglist = func.arglist[1:]
                self.__doc__ = func.gen_doc(func.restypestr, func.restype, func.name, func.arglist)
                self.gen_doc = func.gen_doc

            def __call__(self, *args):
                return self.func(self.disk._handle, *args)

        if not srv:
            srv = self

        handle = self.funcs.corecomm_discover(disk_name, srv.hostname)
        if handle.value <= 0:
            raise OSError(
                self.errno, "Error {0} during discovery of {1}:{2}".format(self.errno, disk_name, srv.hostname))

        if wait_for_serjio_timeout:
            log.debug("Discovery successful, waiting for serjio ack")
            if not wait_until(lambda: srv.get_serjio_client_uuid(disk_name, self.hostname), wait_for_serjio_timeout):
                self.funcs.corecomm_disk_remove(handle)
                raise RuntimeError("Timeout while waiting for serjio on {0} to ack client {1} after discovery",
                                srv.hostname, self.hostname)

        disk = Bunch(_handle=handle, _clnt=self, _srv=srv, _name=disk_name)
        for func_name in self.funcs.as_dict:
            func = self.funcs.as_dict[func_name]
            if len(func.arglist) and func.arglist[0]["type"].name == 'cdisk_handle':
                disk._bunch_update(
                    **{func_name.replace('corecomm_', ''): invoke_on_disk_decorator(disk, func)})

        return disk

    def discover_multiple_raw(self, disks, wait_for_serjio_timeout=0):
        """ Discover multiple disks. Transactional, either all success or none.
            Expects @disks as a collection of pairs (disk_name, srv)
            srv can be None, in this case it is self
        """
        result = []
        for entry in disks:
            disk_name, srv = None, None
            if isinstance(entry, str):
                disk_name = entry
            else:
                disk_name, srv = entry

            try:
                result.append(self.discover_raw(disk_name, srv, wait_for_serjio_timeout))
            except:
                # Error - undo all
                for disk in result:
                    disk.disk_remove()
                raise
        return result

    @contextlib.contextmanager
    def discover(self, disk_name, srv, wait_for_serjio_timeout=0):
        disk = self.discover_raw(disk_name, srv, wait_for_serjio_timeout)
        try:
            yield disk
        finally:
            disk.disk_remove()
    
    @contextlib.contextmanager
    def discover_multiple(self, disks, wait_for_serjio_timeout=0):
        disks = self.discover_multiple_raw(disks, wait_for_serjio_timeout)
        try:
            yield disks
        finally:
            for disk in disks:
                disk.disk_remove()

    def decorate_funcs(self, funcs):
        """ Run some decorations on functions to be more useful within python code
        """
        # Decorators

        class return_status_decorator:
            """ Decorate native function - raise exception instead of setting errno
            """
            def __init__(self, node, func):
                self.func = func
                self.node = node

                self.restypestr = "(Success or Exception)"
                self.restype = func.restype
                self.name = func.name
                self.arglist = func.arglist
                self.__doc__ = func.gen_doc(func.restypestr, func.restype, func.name, func.arglist)
                self.gen_doc = func.gen_doc

            def __call__(self, *args):
                rv = self.func(*args)
                if rv.value != 0:
                    raise OSError(self.node.errno, "Error {0} while running {1}".format(
                        self.node.errno, self.name))

        class output_struct_decorator:
            """ Decorate native function - if the last argument is by reference structure for
                output, make sure to allocate it prior the call, then make it return value.
            """
            def __init__(self, node, func):
                self.node = node
                self.func = func

                self.value_type = func.arglist[-1]["type"].orig.ctype # Assume it is a pointer

                self.output = self.value_type()
                self.outptr = self.node.pointer(self.output)

                # -5 to remove the __ptr at the end
                self.restypestr = func.arglist[-1]["type"].name[:-5]
                self.restype = func.restype
                self.name = func.name
                self.arglist = func.arglist[:-1]
                self.__doc__ = func.gen_doc(func.restypestr, func.restype, func.name, func.arglist)
                self.gen_doc = func.gen_doc

            def __call__(self, *args):
                args = list(args)  # Because originally it may be tuple
                args.append(self.outptr)
                rv = self.func(*args)
                if rv:
                    return self.output, rv
                else:
                    return self.output

        class auto_handle_decorator:
            """ Decorate native function - if the first argument is connection handle -
                hide it and pass automatically.
            """
            def __init__(self, node, func):
                self.func = func
                self.node = node

                self.restypestr = func.restypestr
                self.restype = func.restype
                self.name = func.name
                self.arglist = func.arglist[1:]
                self.__doc__ = func.gen_doc(func.restypestr, func.restype, func.name, func.arglist)
                self.gen_doc = func.gen_doc

            def __call__(self, *args):
                # Remove the __ptr at the end
                args = list(args)  # Because originally it may be tuple
                args.insert(0, self.node.corecomm_handle)
                return self.func(*args)

        class async_call_decorator:
            """ Decorate native function - run it asynchronously.
            """
            def __init__(self, func, async_name):
                self.func = func

                self.res = Bunch(res=None, err=None, thread=None, wait=None)

                self.restypestr = func.restypestr
                self.restype = func.restype
                self.name = async_name
                self.arglist = func.arglist
                self.__doc__ = func.gen_doc(func.restypestr, func.restype, func.name, func.arglist)
                self.gen_doc = func.gen_doc

            def worker_func(self, *args):
                try:
                    self.res.res = self.func(*args)
                except Exception as e:
                    self.res.err = e

            def __call__(self, *args):
                args = tuple(args)
                self.res.thread = threading.Thread(target=self.worker_func, args=args)
                self.res.wait = self.res.thread.join
                self.res.thread.daemon = True
                self.res.thread.start()
                return self.res

        # Body
        new_funcs = Bunch()
        for func_name in funcs.as_dict:
            func = funcs.as_dict[func_name]
            if func.restype and func.restype.name == "value_or_minus_1":
                func = return_status_decorator(self, func)
            if len(func.arglist) and func.arglist[-1]["name"] == "output" and func.arglist[-1]["type"].name.endswith('__ptr'):
                func = output_struct_decorator(self, func)
            if len(func.arglist) and func.arglist[0]["type"].name == "corecomm_handle":
                func = auto_handle_decorator(self, func)

            new_funcs._bunch_update(**{func_name: func})

            async_func_name = func_name + '__async'
            async_func = async_call_decorator(func, async_func_name)

            new_funcs._bunch_update(**{async_func_name: async_func})

        return new_funcs

    @property
    def errno(self):
        return self.funcs.corecomm_errno().value

    def dmesg(self, args=["-T"]):
        cmd = ["dmesg"]
        cmd.extend(args)
        return self.conn.root.shell(cmd)

    def get_tcp_mode(self):
        return str(self.shell("cat /sys/module/nvmeibc/parameters/tcp_mode")).rstrip('\n') == 'Y'

    def get_nics(self):
        def get_nic_type(line):
            if line.startswith("siw"):
                return self.enums.CORECOMM_RDMA_IWARP
            if line.split(',')[9] == "true":
                return self.enums.CORECOMM_RDMA_ROCE
            return self.enums.CORECOMM_RDMA_IB
        cmd = ["cat", "/proc/nvmeibs/nics.csv"]
        try:
            lines = filter(bool, self.conn.root.shell(cmd).split('\n')[1:])
        except:
            return []
        result = []
        for line in lines:
            split = line.split(',')
            nic = {"gid": str(split[1]), "pkey": int(split[2]), "type": get_nic_type(line)}
            result.append(nic)
            log.debug(nic)
        return result

    def register_all_nics_from(self, other):
        tcp_mode = self.get_tcp_mode()

        for nic in other.get_nics():
            log.info("Register nic {0} of {1} from {2}".format(
                nic["gid"], self.node, other.hostname))
            if tcp_mode and nic["type"] != self.enums.CORECOMM_RDMA_IWARP:
                log.info("Skipping, not iWarp and env is TCP")
                continue
            self.funcs.corecomm_register_arnic(
                other.hostname, nic["gid"], nic["pkey"], nic["type"])

    def write_gpt(self, disk_id, data_partitions=None):
        disk = self.get_drives()[disk_id]
        self.funcs.corecomm_freeze(disk_id)
        try:
            self.conn.root.write_gpt_disk(disk['dev'], str(
                int((0.005 * disk['size']) / 1024 / 1024)) + "M", data_partitions)
            time.sleep(1)
        finally:
            self.funcs.corecomm_unfreeze(disk_id)

    def read_gpt(self, disk_id):
        disk = self.get_drives()[disk_id]
        return [uuid.UUID(uuid_str) for uuid_str in self.conn.root.read_partitions_info_filtered(disk['dev'])]

    def zero_partition(self, disk_id, uuid):
        disk = self.get_drives()[disk_id]
        self.funcs.corecomm_freeze(disk_id)
        try:
            part = next(p for p in self.conn.root.read_partitions_info(disk['dev']) if p["guid_code"] == uuid)
            self.shell("sudo dd if=/dev/zero of={0} bs={1} count={2} seek={3}".format(
                disk["dev"],
                part["sector_size"],
                part["last_sector"] - part["first_sector"] + 1,
                part["first_sector"] - 1
            ))
        finally:
            self.funcs.corecomm_unfreeze(disk_id)

    def get_hostname(self):
        return str(self.conn.root.shell("hostname")).strip('\n').rstrip('\n')

    def get_serjio_boot_id(self, disk_id):
        return self.shell("cat /proc/nvmeibs/serjio/{0}/boot_id".format(disk_id)).rstrip("\n")

    def get_allocated_jris(self, disk_id):
        lines = self.shell(
            "cat /proc/nvmeibs/serjio/{0}/ranges.csv | cut -f1 -d','".format(disk_id)).split('\n')[1:]
        return [int(l) for l in lines if l]

    def get_jmdc_as_json(self, disk_id, rng_id):
        lines = self.shell(
            "cat /proc/nvmeibs/serjio/{0}/jentries/rng{1}.csv".format(disk_id, rng_id)).split('\n')[1:]
        # Could do one liner, looks ugly in this case
        res = []
        for line in lines:
            if not line:
                continue
            sp = line.split(',')
            res.append({
                'jentry':   int(sp[0]),
                'status':   str(sp[1]),
                'gen_id':   int(sp[2]),
                'j2d':      int(sp[3]),
                'tx_id':    int(sp[4], 16),
                'tx_bmp':   str(sp[5]),  # TODO: parse as bitmap
                'seg_uuid': str(sp[6])
            })
        return res

    def get_serjio_client_uuid_list(self, disk_id):
        lines = self.shell(
            "cat /proc/nvmeibs/serjio/{0}/clients.csv | cut -f2,3,4 -d','".format(disk_id)).split('\n')[1:]
        return [(uuid.UUID(l.split(',')[0]), l.split(',')[1], int(l.split(',')[2])) for l in lines if l]

    def get_serjio_client_uuid(self, disk_id, hostname):
        return next(clnt[0] for clnt in self.get_serjio_client_uuid_list(disk_id) if clnt[1].startswith(hostname))

    def get_serjio_client_jri(self, disk_id, hostname):
        return next(clnt[2] for clnt in self.get_serjio_client_uuid_list(disk_id) if clnt[1].startswith(hostname))

    def set_jmdc_entry(self, disk_id, range, entry, j2d, tx_id, tx_bmp, ver=0):
        """ Set jmdc entry via proc exposed by serjio
        """
        self.shell(["bash", "-c", "echo {0}:{1}:{2}:{3}:{4}:{5} > /proc/nvmeibs/serjio/{6}/jmdc_set".format(
            range, entry, j2d, tx_id, hex(tx_bmp), hex(ver), disk_id)])

    def set_unknown_entry(self, disk_id, jrange, jentry):
        try:
            self.shell(
                "echo -n {0}:{1} > /proc/nvmeibs/serjio/{2}/unknown_entry".format(jrange, jentry, disk_id))
        except Exception:
            pass

    def set_module_param(self, module, param, value):
        self.conn.root.set_module_param(module, param, value)
    
    def get_module_param(self, module, param):
        return self.shell(["cat", "/sys/module/{0}/parameters/{1}".format(module, param)]).rstrip("\n")

    def get_drives(self):
        VENDORS = {
            "HGST": 0x1c58,
            "Samsung": 0x144d,
            "Intel": 0x8086,
            "Micron": 0x1344,
            "Memblaze": 0x1c5f,
            "Toshiba": 0x1179,
            "SanDisk": 0x15b7,
            "WesternDigital": 0x1b96,
            "Kingston": 0x2646,
        }
        drives = {}
        cmd = ["nvme", "list", "-o", "json"]
        # This line is needed because for some reason nvme cli refuses to show metadata size in json format
        list_text = self.shell("nvme list -o normal").split('\n')[2:]

        # First get data from nvme CLI
        for drive, text in zip(json.loads(self.shell(cmd))["Devices"], list_text):
            try:
                self.shell('df | grep {0}'.format(drive["DevicePath"]))
                # Grep will fail if device path is not mounted
                log.debug("Device {0} is mounted, skipping".format(
                    drive["DevicePath"]))
                continue
            except:
                pass

            try:
                self.shell('pvs | grep {0}'.format(drive["DevicePath"]))
                # Grep will fail if device path is not mounted
                log.debug("Device {0} is used by LVM, skipping".format(
                    drive["DevicePath"]))
                continue
            except:
                pass

            model = drive["ModelNumber"];
            vendor = next(v for v in VENDORS if model.lower().startswith(v.lower()))
            drives[drive["SerialNumber"] + "." + str(drive.get("NameSpace", 1))] = {
                "size": int(drive["PhysicalSize"]),
                "sector": int(drive["SectorSize"]),
                "dev": str(drive["DevicePath"]),
                "md": int(filter(bool, text.split(' '))[-3]),
                "model": model,
                "vendor": vendor,
                "vendor_id": VENDORS[vendor],
            }

        # Next, also add data from disks.csv
        diskscsv = self.shell("cat /proc/nvmeibs/disks.csv").split('\n')[1:]
        for line in diskscsv:
            if not line:
                continue
            sp = line.split(",")
            if not sp[0] in drives.keys():
                drives[sp[0]] = {
                    "size": int(sp[1]) * int(sp[2]),
                    "sector": int(sp[2]),
                    "dev": str(sp[6]),
                    "md": int(sp[7]),
                    "model": "WTF",
                    "vendor": next(v for v in VENDORS if VENDORS[v] == int(sp[9])),
                    "vendor_id": int(sp[9]),
                }

        return drives

    def quickfmt(self, disk_id, lbaf=3, flag_nvme_format=1, flag_delete_create_ns=0, flag_reset_ctrlr=1, is_inline=0):
        """ Shortcut for disk format
        """
        disk_id = str(disk_id) # For case it is byte array
        disk = self.get_drives()[disk_id]
        
        fd = self.types.struct__corecomm_format_disk.ctype(
            # disk_id = disk_id,
            vendor_id = disk["vendor_id"],
            flags = self.types.union__corecomm_format_disk_flags.ctype(
                bf = self.types.struct__corecomm_format_disk_flags_bitfields.ctype(
                    flag_reset_ctrlr = int(flag_reset_ctrlr),
                    flag_nvme_format = int(flag_nvme_format),
                    flag_delete_create_ns = int(flag_delete_create_ns),
                )
            ),
            format_id = self.types.union__corecomm_format_disk_format_id.ctype(
                bf = self.types.struct__corecomm_format_disk_format_id_bitfields.ctype(
                    id = lbaf,
                    is_inline = int(is_inline)
                )
            )
        )

        self.memmove(fd.disk_id, disk_id, min(len(disk_id), self.types.name_t.orig.size))

        return self.funcs.corecomm_format_local_disk(fd)


    def quickwrite(self, disk, addr, data_stamps, sub_block=-1, md=None, ndb=None, is_jour=False):
        """ Shortcut function to quickly write as set of stamps to disk
        """
        stamp_arr_ = (len(data_stamps) * self.ctypes.c_ulong)(*data_stamps)
        stamp_arr = self.cast(stamp_arr_, self.POINTER(self.types.uint64.ctype))
        _ndb = ndb
        if not _ndb:
            _ndb = self.funcs.corecomm_alloc_ndb(len(data_stamps))
            if (_ndb.value < 0):
                raise OSError(self.errno, "While allocating NDB")

        iofunc = self.funcs.corecomm_pd_execute_io_blocks if not is_jour else self.funcs.corecomm_pd_execute_io_jour_blocks

        ld = None

        offset_bytes = 0 if sub_block == -1 else sub_block*512

        try:
            self.funcs.corecomm_stamp_ndb(
                _ndb, stamp_arr, len(data_stamps), 0, offset_bytes, 0)
            if md:
                stamp_arr_md_ = (len(md) * self.ctypes.c_ulong)(*md)
                stamp_arr_md = self.cast(stamp_arr_md_, self.POINTER(self.types.uint64.ctype))
                self.funcs.corecomm_stamp_ndb(
                    _ndb, stamp_arr_md, len(md), 0, offset_bytes, 1)
            ld = iofunc(
                disk, _ndb, addr, len(data_stamps), 2, 0, 0, sub_block)
        finally:
            if not ndb:  # Cleanup if we allocated ndb
                self.funcs.corecomm_free_ndb(_ndb)

        return ld

    def quickread(self, disk, addr, datalen=1, sub_block=-1, ndb=None, pb=False, is_jour=False):
        """ Shortcut function to quickly read as set of stamps from disk
        """
        stamp_arr_ = (datalen * self.ctypes.c_ulong)()
        stamp_arr_md_ = (datalen * self.ctypes.c_ulong)()
        stamp_arr = self.cast(stamp_arr_, self.POINTER(self.types.uint64.ctype))
        stamp_arr_md = self.cast(stamp_arr_md_, self.POINTER(self.types.uint64.ctype))
        _ndb = ndb
        if not _ndb:
            _ndb = self.funcs.corecomm_alloc_ndb(datalen)
            if (_ndb.value < 0):
                raise OSError(self.errno, "While allocating NDB")

        iofunc = self.funcs.corecomm_pd_execute_io_blocks if not is_jour else self.funcs.corecomm_pd_execute_io_jour_blocks

        offset_bytes = 0 if sub_block == -1 else sub_block*512

        ld = None
        try:
            ld = iofunc(
                disk, _ndb, addr, datalen, 1, pb, addr, sub_block)
            self.funcs.corecomm_read_stamps_ndb(_ndb, stamp_arr, datalen, 0, offset_bytes, 0)
            self.funcs.corecomm_read_stamps_ndb(
                _ndb, stamp_arr_md, datalen, 0, offset_bytes, 1)
        finally:
            if not ndb:  # Cleanup if we allocated ndb
                self.funcs.corecomm_free_ndb(_ndb)

        return [stamp_arr_[i] for i in range(datalen)], [stamp_arr_md_[i] for i in range(datalen)], ld

    def quick_alloc_jrnls(self, disks, dlbas, txid):
        """ Shortcut function for alloc journals
            returns jlbas
        """
        # First generate the C data structures
        # 0. n_disks
        n_disks = len(disks)
        # 1. disks
        handles = [d._handle.value for d in disks]
        handles_arr = self.types.corecomm_disks_arr.ctype (*handles)
        disks_ = self.types.struct__corecomm_disks_set.ctype(handles_arr)
        disks = self.pointer(disks_)
        # 2. dlbas
        dlbas_ = self.types.struct__corecomm_lbas_set.ctype(self.types.corecomm_lbas_arr.ctype(*dlbas))
        dlbas = self.pointer(dlbas_)
        # 3. txid - nothing to do
        jlbas = self.funcs.corecomm_alloc_jrnls(n_disks, disks, txid, dlbas)
        return [jlbas.lbas[i].value for i in range(n_disks)]

    def quick_free_jrnls(self, disks, jlbas, wr_sts_bm=0):
        """ Shortcut function for free journals
        """
        # First generate the C data structures
        # 0. n_disks
        n_disks = len(disks)
        # 1. disks
        handles = [d._handle.value for d in disks]
        handles_arr = self.types.corecomm_disks_arr.ctype (*handles)
        disks_ = self.types.struct__corecomm_disks_set.ctype(handles_arr)
        disks = self.pointer(disks_)
        # 2. jlbas
        jlbas_ = self.types.struct__corecomm_lbas_set.ctype(self.types.corecomm_lbas_arr.ctype(*jlbas))
        jlbas = self.pointer(jlbas_)
        # 3. wr_sts_bm - nothing to do
        return self.funcs.corecomm_free_jrnls(n_disks, disks, jlbas, wr_sts_bm)

    @contextlib.contextmanager
    def alloc_jrnls(self, disks, dlbas, txid, wr_sts_bm=0):
        """ Context managed alloc journals: ensure journals are freed
        """
        jlbas = self.quick_alloc_jrnls(disks, dlbas, txid)
        try:
            yield jlbas
        finally:
            self.quick_free_jrnls(disks, jlbas, wr_sts_bm)

def get_total_drives_list(nodes):
    drives = []
    for node in nodes:
        for drive in node.drives.items():
            drives.append({"name":drive[0], "info":drive[1], "node":node})
    return drives

def install_server(node, server_install_dir=SERVER_INSTALL_DIR, wipe=False):
    try:
        if wipe:
            vcheck_call(
                SSH_CMD + [node,
                "rm",
                "-rf",
                server_install_dir])

        vcheck_call(
            ["rsync",
             "-e", " ".join(SSH_CMD), # Custom ssh command
             "-a",
             "-r",
             "--rsync-path",
             "mkdir -p {0} && rsync".format(server_install_dir),
             CORECOMM_SRC_ROOT + "/",
             "{0}:{1}".format(node, server_install_dir)])

        vcheck_call(
            SSH_CMD + [node,
             "make",
             "-C",
             server_install_dir,
             "clean",
             "so"])

        vcheck_call(
            SSH_CMD + [node,
             "bash",
             "-c",
             "'cd {0} && ./install-server.sh'".format(os.path.join(server_install_dir, "server"))])
    except subprocess.CalledProcessError as e:
        raise RuntimeError(
            "Error during installtion: {0} returned {1}".format(e.cmd, e.returncode))


def node_to_var_name(node):
    """ Given remove node name, translates it into var name acceptable in python
    """
    return node.replace('-', '_').replace('.', "_").replace(' ', '_')


input_nodes = []


def marry_all_nodes(input_nodes, fmt=True, gpt=True, wipe_serjio_db=True, bind=True, arnics=True):

    # binding
    if bind:
        for node in input_nodes:
            for drive in node.get_drives():
                log.info("Binding {0} to {1}".format(drive, node.node))
                try:
                    node.conn.root.bind_disk(drive)
                except Exception as e:
                    log.debug("Something wrong, skipping {0}".format(str(e)))
        log.info("Sleep 10s after bind")
        time.sleep(10)

    # format
    if fmt:
        for node in input_nodes:
            for drive, drive_props in node.get_drives().items():
                log.info("Formatting {0}:{1}".format(drive, node.node))
                try:
                    node.quickfmt(drive)
                except Exception as e:
                    log.debug("Something wrong, skipping {0}".format(str(e)))

    # write gpt
    if gpt:
        for node in input_nodes:
            for drive in node.get_drives():
                log.info("Writing gpt for {0}:{1}".format(drive, node.node))
                node.write_gpt(drive)

    if wipe_serjio_db:
        for node in input_nodes:
            for drive in node.get_drives():
                log.info("Zeroing serjio db for {0}:{1}".format(drive, node.node))
                node.zero_partition(drive, "44E54508-3B09-FC55-94D4-FF6351D079AE")

    # arnics
    if arnics:
        for node1 in input_nodes:
            for node2 in input_nodes:
                node1.register_all_nics_from(node2)

    # refresh_fields - I don't see why not do it
    if arnics or gpt or fmt or bind:
        for node in input_nodes:
            node.refresh_fields()


def unbind_all_nodes(input_nodes):
    for node in input_nodes:
        for drive in node.get_drives():
            log.info("Unbinding {0} from {1}".format(drive, node.node))
            try:
                node.conn.root.unbind_disk(drive)
            except Exception as e:
                log.debug("Something wrong, skipping {0}".format(str(e)))


class CorecommTestException(Exception):
    pass

@contextlib.contextmanager
def set_binje(binje, input_nodes):
    if not binje in BINJE_ALLOWED_VALUES:
        raise RuntimeError("Binge = {0} is not allowed".format(binje))
    bkup = []
    for node in input_nodes:
        bkup.append(node.get_module_param("nvmeibc", "nvmeibc_jentry_num_blocks"))
        node.set_module_param("nvmeibc", "nvmeibc_jentry_num_blocks", binje)
    try:
        yield
    finally:
        for node, bk in zip(input_nodes, bkup):
            node.set_module_param("nvmeibc", "nvmeibc_jentry_num_blocks", bk)
        
