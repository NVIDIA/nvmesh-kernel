#!/usr/bin/env python

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

from xlro.core.util.scanner import get_vol_locks_summary, get_vol_locks_full, get_target_locks_summary
from xlro.core.util.general_utils import host_name, wait_for_property_values
from xlro.core.entities import (Chunk, Client, Drive, Manager, PRaid, Target,
                                Volume)
from xlro.core.util import failed_io as fi
from IPython import embed
import argparse
import json
import importlib
import struct
import string
import os
import subprocess

calc = importlib.import_module('the-calculator')

VLBA = calc.VLBA
CLBA = calc.CLBA
RLBA = calc.RLBA
SLBA = calc.SLBA
DLBA = calc.DLBA

mgmt_node = None
mgmt = None
clients = None
targets = None
volumes = None
drives = None

DEFAULT_DI_PARSER = os.path.expanduser('~/projects/nvmesh/perfTest/io_stress/di_parser/parse_block')


def _to_hex_view(buf, w_size=2, w_per_line=8):
    """ Quick and dirty implementation of buf to hex view
    """
    def __isprint(chh):
        return not (ord(chh) > 127 or ord(chh) < 32 or chh in "\n\r\t| " or not chh in string.printable)

    def __add_ch(ch, res, preview):
        if ch and __isprint(ch):
            preview.append(ch)
        elif ch:
            preview.append('.')
        res.append('{0:02x}'.format(ord(ch)) if ch else '  ')

    def __get_ch(buf, i):
        if (i < len(buf)):
            return buf[i]

    addr = 0
    res = []
    i = 0
    while i < len(buf):
        res.append('{0:08x}: '.format(addr))
        preview = []
        for _ in range(0, w_per_line):
            for _ in range(0, w_size):
                ch = __get_ch(buf, i)
                __add_ch(ch, res, preview)
                i += 1
            res.append(' ')
        res.append('|')
        res.extend(preview)
        res.append('\n')
        addr += w_per_line * w_size
    return ''.join(res)

class Infra:
    """ Encapsulates infra project properties
    """
    def __init__(self, lba):
        self.lba = lba
        self.vol = next(vol for vol in volumes.values if vol.name == lba.vol.v)
        self.vlba = self.vol.LBA(lba.vlba.lba)
        self.dlba = Drive.LBA(int(lba.dlba), lba.vol.block_size)
        self.dlba.block_size = lba.vol.block_size # Workaround for infra bug
        self.dmd = None

    def reread(self):
        self.dmd = next(self.lba.seg.drive.read(self.dlba, 1))

    def reread_if_needed(self):
        if not self.dmd:
            self.reread()

    def read(self):
        self.reread()
        return self.dmd

    def data(self):
        self.reread_if_needed()
        return self.dmd.data

    def md(self):
        self.reread_if_needed()
        return self.dmd.metadata
    
    def md_int(self):
        self.reread_if_needed()
        return (self.dmd.metadata, struct.unpack('<q', self.dmd.metadata))

    def print_data(self):
        print (_to_hex_view(self.data(), 2, 8))

    def print_md(self):
        print (_to_hex_view(self.md(), 2, 8))

    def dump_lba(self, client, logpath='.', dbgdi=False, di_parser=DEFAULT_DI_PARSER):
        self.reread_if_needed()
        if not os.path.exists(logpath):
            os.makedirs(logpath)
        suffix = '_{0}_{1}'.format(self.lba.slba.lba, self.lba.role)
        with open(os.path.join(logpath, 'data' + suffix), 'wb') as f:
            f.write(self.dmd.data)
        with open(os.path.join(logpath, 'metadata' + suffix), 'wb') as f:
            f.write(self.dmd.metadata)
        if dbgdi:
            print([di_parser, os.path.join(logpath, 'data' + suffix), os.path.join(logpath, 'data' + suffix + '.txt')])
            subprocess.call([di_parser, os.path.join(logpath, 'data' + suffix), os.path.join(logpath, 'data' + suffix + '.txt')])

    def dump_slice(self, client, logpath='.', dbgdi=False, di_parser=DEFAULT_DI_PARSER):
        for lba in self.lba.enumerate_slice_slbas():
            lba.infra.dump_lba(client, logpath, dbgdi, di_parser)
    
    def dump_blockset(self, client, logpath='.', dbgdi=False, di_parser=DEFAULT_DI_PARSER):
        for lba in self.lba.enumerate_blockset_slbas():
            lba.infra.dump_lba(client, logpath, dbgdi, di_parser)

    def dump_lba_info(self, client, logpath='./lba_info'):
        if type(client) == str:
            global clients
            client = clients.__dict__[client]
        if not os.path.exists(logpath):
            os.makedirs(logpath)
        return fi.dump_lba_info(dc_volume=self.vol, client=client, dc_vlba=self.vlba, logpath=logpath)

def extend_calc_classes():
    def gety_infra(lba):
        if not hasattr(lba, '_infra') or not lba._infra:
            lba._infra = Infra(lba)
        return lba._infra

    calc.extend_class_list([calc.DLBA, calc.SLBA, calc.RLBA, calc.CLBA, calc.VLBA], "infra", property(gety_infra))


extend_calc_classes()


def refresh_calc():
    calc.volumes = {}
    calc.chunks = {}
    calc.raids = {}
    calc.segs = {}
    for vol in Volume.get_filtered():
        v = vol.name
        calc.volumes[v] = calc.Volume(v, block_size=vol.blockSize)
        for c, chunk in enumerate(vol.chunks):
            calc.chunks[(v, c)] = calc.Chunk(v, c, vol.dataBlocks,
                                             vol.stripeSize*vol.dataBlocks, vol.stripeWidth, chunk.vlbs, chunk.vlbe)
            for raid in chunk.pRaids:
                r = raid.stripeIndex
                calc.raids[(v, c, r)] = calc.Raid(v, c, r, raid.version, None,
                                                  vol.dataBlocks, vol.dataBlocks + vol.parityBlocks)
                for seg in raid.diskSegments:
                    s = seg.pRaidIndex
                    calc.segs[(v, c, r, s)] = calc.Seg(v, c, r, s, seg.uuid,
                                                       seg.lbs, seg.lbe, seg.diskID, None, host=seg.node_id, drive=seg.drive)


def refresh_instances():
    global mgmt
    global clients
    global targets
    global volumes
    global drives
    mgmt = Manager.instance(host=host_name(mgmt_node))
    clients = Bunch(
        **{normalize_name(a.name): a for a in Client.get_filtered()})
    targets = Bunch(
        **{normalize_name(a.name): a for a in Target.get_filtered()})
    volumes = Bunch(
        **{normalize_name(a.name): a for a in Volume.get_filtered()})
    drives = set()
    for t in targets.values:
        drives.update(t.drives)

    refresh_calc()


class Bunch:
    def __init__(self, **kwds):
        self.values = kwds.values()
        self.__dict__.update(kwds)

    def __str__(self):
        return str({k: str(vars(self)[k]) for k in vars(self)})

    def __repr__(self):
        return str(self)


def format_all_drives(wait=True):
    Drive.format_drives(drives)
    if wait and not wait_for_property_values(drives, 'status', ['Ok'], timeout=60*5):
        raise Exception("Not all drives formatted")


def create_mirror(name, capacity, numberOfMirrors=1):
    vol = Volume(name=name, capacity=capacity, RAIDLevel=PRaid.RaidLevels.RAID1, numberOfMirrors=numberOfMirrors)
    Volume.create(vol)
    refresh_instances()


def create_ec(name, capacity, data=4, parity=2, stripeWidth=1, stripeSize=32, protectionLevel='Ignore Separation'):
    vol = Volume(name=name, capacity=capacity, RAIDLevel=PRaid.RaidLevels.ERASURE_CODING, dataBlocks=data, parityBlocks=parity,
                 stripeWidth=stripeWidth, stripeSize=stripeSize, protectionLevel=protectionLevel)
    Volume.create(vol)
    refresh_instances()


def create_jbob(name, capacity):
    vol = Volume(name=name, capacity=capacity, RAIDLevel=PRaid.RaidLevels.JBOD)
    Volume.create(vol)
    refresh_instances()


def create_raid10(name, capacity, numberOfMirrors=1, stripeWidth=2, stripeSize=32, protectionLevel='Ignore Separation'):
    vol = Volume(name=name, capacity=capacity, RAIDLevel=PRaid.RaidLevels.RAID10,
                 stripeWidth=stripeWidth, stripeSize=stripeSize, protectionLevel=protectionLevel, numberOfMirrors=numberOfMirrors)
    Volume.create(vol)
    refresh_instances()


def create_raid0(name, capacity, stripeWidth=2, stripeSize=32):
    vol = Volume(name=name, capacity=capacity, RAIDLevel=PRaid.RaidLevels.RAID0,
                 stripeWidth=stripeWidth, stripeSize=stripeSize)
    Volume.create(vol)
    refresh_instances()


def normalize_name(name):
    name = name.replace('.', '_')
    name = name.replace('-', '_')
    name = name.replace(' ', '_')
    return name


def init_argparser():
    parser = argparse.ArgumentParser("connect_interactive.py")
    parser.add_argument(dest='mgmt', type=str, help='Management host name')
    parser.add_argument('--exec', dest='cmd', type=str, help='Execute commands and exit', nargs='*')

    return parser


def main():
    args = init_argparser().parse_args()
    global mgmt_node
    mgmt_node = args.mgmt
    refresh_instances()
    if args.cmd:
        exec(' '.join(args.cmd))
        return 0
    embed()
    return 0


if __name__ == "__main__":
    import sys
    sys.exit(main())
