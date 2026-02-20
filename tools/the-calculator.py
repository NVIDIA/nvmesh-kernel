#!/usr/bin/env python3
""" This is THE one and the ONLY TRUE calculator for \\X\\LBA translations,
    All other calculators are false and unholy.
    May its glory shine forever.

    DISCLAIMER: I am not an idiot. The year is 2020, and I am using python2
    only because that script should be able to run on old garbage machines.
"""

import re
import os
import argparse
import json
import IPython
import collections


class SmartFormatter(argparse.ArgumentDefaultsHelpFormatter):
    """ Utility: Just used for nicer looking help. Copied from pager.py
    """

    def __init__(self, prog, indent_increment=2, max_help_position=25, width=105):
        super(SmartFormatter, self).__init__(prog, indent_increment, max_help_position, width)

    def smartsplit(self, line, width):
        match = re.match(r'^\s*(?:\d+\.?|\*|\-)?\s*', line)
        if match:
            indent = len(match.group())
            split = super(SmartFormatter, self)._split_lines(
                line, width - indent)
            res = []
            match = re.match(r'^\s+', line)
            if not split:
                return [' ']
            if match:
                res.append(' '*len(match.group()) + split[0])
            else:
                res.append(split[0])
            for line in split[1:]:
                res.append(' '*indent + line)
            return res
        return super(SmartFormatter, self)._split_lines(line, width)

    def _split_lines(self, text, width):
        if text.startswith('R|'):
            res = []
            for line in text[2:].splitlines():
                res.extend(self.smartsplit(line, width))
        else:
            res = super(SmartFormatter, self)._split_lines(text, width)

        return res

    def _fill_text(self, text, width, indent):
        return '\n'.join(indent + line for line in self._split_lines(text, width))


USAGE_EXAMPLE = """
USAGE EXAMPLE:
Starting using configuration of 2 volumes:
    {0} --procdir /proc/nvmeibc/volumes/tv-61128-1 /proc/nvmeibc/volumes/tv-69979-2
    {0} --umvolume ./tv-61128-1
From interractive shell:
    > lba = DLBA('S3HCNX0K600585.1', 233)
    > lba = SLBA(seg('tv-61128-1', 0, 0, 1), 233)
    > lba = RLBA(chunk('tv-61128-1', 0, 0), 233)
    > lba = CLBA(chunk('tv-61128-1', 0), 233)
    > lba = VLBA(volume('tv-61128-1'), 233)
    > print (lba.rlba)
    > print (lba.rlba.blockset)
    > print (lba.clba)
    > print (lba.seg)
    > print (lba.vol)
    > print (lba.raid)
    > print (lba.lock_owner)
    > print (lba.seg.host)
    > print (lba.seg.lmap)
    > print (lba.mirror_slbas)
    > print (lba.mirror_dlbas)
    > print (lba.role)
Example, to get additional help on specific function:
    > public_props(VLBA)
    > DLBA?
    > DLBA.rlba?
Use TAB for autocompletion.

""".format(os.path.basename(__file__))


segs = {}  # Map from (v, c, r, s) to Seg object
raids = {}  # Map from (v, c, r) to Raid object
chunks = {}  # Map from (v, c) to Chunk object
volumes = {}  # Map from v to Volume object

# Below are some quick getters for seg/raid/chunk/volume


def seg(v, c, r, s):
    return segs[(v, c, r, s)]


def seg_by_uuid(uuid):
    return next((s for s in segs.values() if s.uuid.startswith(uuid)), None)


def raid(v, c, r):
    return raids[(v, c, r)]


def chunk(v, c):
    return chunks[(v, c)]


def volume(v):
    return volumes[v]


class Volume:
    """ Represents volume
        Volume is defined by its name
    """

    def __init__(self, v, **kwargs):
        self.v = v
        self.__dict__.update(kwargs)

    def long_str(self):
        return "{0}".format(self.key)

    def __str__(self):
        return 'volume({0})'.format(self.v)

    def __repr__(self):
        return str(self)

    def chunks(self):
        return sorted((chunk(self.v, c) for (v, c) in chunks if self.v == v), key=lambda k: k.key)

    @property
    def key(self):
        return self.v


class Chunk:
    """ Represents chunk
        Defined by volume name and chunk index
    """

    def __init__(self, v, c, slice_size, stripe_size, stripe_width, vlba_start, vlba_end):
        self.v = v
        self.c = c

        self.slice_size = slice_size
        self.stripe_size = stripe_size
        self.stripe_width = stripe_width
        self.vlba_start = vlba_start
        self.vlba_end = vlba_end
        self.slices_in_stripe = self.stripe_size // self.slice_size
        self.slices_in_raid = self.stripe_size // self.slice_size // self.stripe_width

    def long_str(self):
        return 'chunk({0}) [{1},{2}) size={3} width={4}'.format(self.c, self.vlba_start, self.vlba_end, self.stripe_size, self.stripe_width)

    def __str__(self):
        return 'chunk{0}'.format(self.key)

    def __repr__(self):
        return str(self)

    def raids(self):
        return sorted((raid(self.v, self.c, r) for (v, c, r) in raids if self.v == v and self.c == c), key=lambda k: k.key)

    @property
    def vol(self):
        return volume(self.v)

    @property
    def key(self):
        return (self.v, self.c)


class Raid:
    """ Represents raid
        Defined by volume name chunk index and raid index inside that chunk
    """

    def __init__(self, v, c, r, ver, lid, slice_size, replicas):
        self.v = v
        self.c = c
        self.r = r

        self.ver = ver
        self.lid = lid
        self.slice_size = slice_size
        self.replicas = replicas

    @property
    def is_ec(self):
        return self.slice_size > 1

    @property 
    def is_jbod(self):
        return self.replicas == 1

    @property
    def is_mirror(self):
        return self.is_ec == False and self.is_jbod == False

    def long_str(self):
        return 'raid ({0}) {1}+{2} lid={3} ver={4}'.format(self.r, self.slice_size, self.replicas - self.slice_size, self.lid, self.ver)

    def __str__(self):
        return 'raid{0}'.format(self.key)

    def __repr__(self):
        return str(self)

    def segs(self):
        return sorted((seg(self.v, self.c, self.r, s) for (v, c, r, s) in segs if self.v == v and self.c == c and self.r == r), key=lambda k: k.key)

    @property
    def key(self):
        return (self.v, self.c, self.r)

    @property
    def chunk(self):
        return chunk(self.v, self.c)

    @property
    def vol(self):
        return volume(self.v)


class Seg:
    """ Represents segment
        You've guessed it
    """

    def __init__(self, v, c, r, s, uuid, dlba_start, dlba_end, disk_name, lmap, **kwargs):
        self.v = v
        self.c = c
        self.r = r
        self.s = s

        self.uuid = uuid
        self.dlba_start = dlba_start
        self.dlba_end = dlba_end
        self.disk_name = disk_name
        self.lmap = lmap

        self.__dict__.update(kwargs)

    def long_str(self):
        return 'seg ({0}) disk {1:20} [{2:9},{3:9}) uuid={4} lm={5}'.format(self.s, self.disk_name, self.dlba_start, self.dlba_end, self.uuid, self.lmap)

    def __str__(self):
        return 'seg{0}'.format(self.key)

    def __repr__(self):
        return str(self)

    @property
    def key(self):
        return (self.v, self.c, self.r, self.s)

    @property
    def raid(self):
        return raid(self.v, self.c, self.r)

    @property
    def chunk(self):
        return chunk(self.v, self.c)

    @property
    def vol(self):
        return volume(self.v)

    @property
    def lock_owner(self):
        lock_owner = int(re.match(r'O(\d+).*', self.lmap).group(1))
        return seg(self.v, self.c, self.r, lock_owner)


class VLBA:
    def __init__(self, vol, vlba):
        if type(vol) == str:
            self.vol = volume(vol)
        else:
            self.vol = vol
        self.lba = vlba
        self.vlba = self

    def __int__(self):
        return int(self.lba)

    def __str__(self):
        return 'VLBA({0}, {1})'.format(str(self.vol), self.lba)

    def __repr__(self):
        return str(self)

    @property
    def chunk(self):
        c = [c for c in chunks.values() if self.vol.v ==
             c.vol.v and self.lba >= c.vlba_start and self.lba < c.vlba_end]
        if not c:
            raise RuntimeError('Could not find any chunk on volume {0} that spans lba {1}'.format(self.vol, self.lba))
        return c[0]

    @property
    def raid(self):
        return self.rlba.raid

    @property
    def seg(self):
        return self.slba.seg

    @property
    def clba(self):
        return CLBA(self.chunk, self.lba - self.chunk.vlba_start)

    @property
    def rlba(self):
        return self.clba.rlba

    @property
    def slba(self):
        return self.rlba.slba

    @property
    def dlba(self):
        return self.slba.dlba

    @property
    def key(self):
        return (self.vol, self.lba)


class CLBA:
    def __init__(self, _chunk, clba):
        self.lba = clba
        self.chunk = chunk(*_chunk) if type(_chunk) == tuple else _chunk
        self.clba = self

    def __int__(self):
        return int(self.lba)

    def __int__(self):
        return int(self.lba)

    def __str__(self):
        return 'CLBA({0}, {1})'.format(str(self.chunk), self.lba)

    def __repr__(self):
        return str(self)

    @property
    def vol(self):
        return self.chunk.vol

    @property
    def raid(self):
        return self.rlba.raid

    @property
    def seg(self):
        return self.slba.seg

    @property
    def vlba(self):
        return VLBA(self.vol, self.lba + self.chunk.vlba_start)

    @property
    def rlba(self):
        number_of_full_stripes = self.lba // (self.chunk.stripe_size * self.chunk.stripe_width)
        lbas_since_full_stripe = self.lba % (self.chunk.stripe_size * self.chunk.stripe_width)

        r = lbas_since_full_stripe // self.chunk.stripe_size
        rlba_offset = lbas_since_full_stripe % self.chunk.stripe_size

        return RLBA(raid(self.chunk.v, self.chunk.c, r), number_of_full_stripes * self.chunk.stripe_size + rlba_offset)

    @property
    def slba(self):
        return self.rlba.slba

    @property
    def dlba(self):
        return self.slba.dlba

    @property
    def key(self):
        return (self.chunk, self.lba)


class RLBA:
    def __init__(self, _raid, rlba):
        self.lba = rlba
        self.raid = raid(*_raid) if type(_raid) == tuple else _raid
        self.rlba = self

    def __str__(self):
        return 'RLBA({0}, {1})'.format(str(self.raid), self.lba)

    def __int__(self):
        return int(self.lba)

    def __repr__(self):
        return str(self)

    @property
    def rlba_slice(self):
        return (self.lba // self.raid.slice_size)

    @property
    def index_in_slice(self):
        return (self.lba % self.raid.slice_size)

    @property
    def slice_owner(self):
        return seg(self.raid.v, self.raid.c, self.raid.r, (self.rlba_slice // 64) % self.raid.replicas)

    @property
    def mirror_slbas(self):
        if (self.raid.slice_size != 1):
            raise RuntimeError('Raid is EC')
        return [SLBA(seg(self.raid.v, self.raid.c, self.raid.r, s), self.slba.lba) for s in range(0, self.raid.replicas)]

    @property
    def p_segment(self):
        return seg(self.raid.v, self.raid.c, self.raid.r, (self.slice_owner.s + self.raid.slice_size) % self.raid.replicas)

    @property
    def q_segment(self):
        return seg(self.raid.v, self.raid.c, self.raid.r, (self.slice_owner.s + self.raid.slice_size + 1) % self.raid.replicas)

    @property
    def p_slba(self):
        return SLBA(self.p_segment, self.rlba_slice)

    @property
    def q_slba(self):
        return SLBA(self.q_segment, self.rlba_slice)

    @property
    def mirror_dlbas(self):
        return [slba.dlba for slba in self.mirror_slbas]

    @property
    def seg(self):
        s = (self.slice_owner.s + self.index_in_slice) % self.raid.replicas
        return seg(self.raid.v, self.raid.c, self.raid.r, s)

    @property
    def chunk(self):
        return self.raid.chunk

    @property
    def vol(self):
        return self.raid.vol

    @property
    def slba(self):
        return SLBA(self.seg, self.rlba_slice)

    @property
    def dlba(self):
        return self.slba.dlba

    @property
    def clba(self):
        raid_index_in_stripe = self.raid.r
        raid_offset_within_stripe_blocks = raid_index_in_stripe * self.chunk.stripe_size
        slice_offset_within_stripe_blocks = (self.rlba_slice % self.chunk.slices_in_stripe) * self.raid.slice_size
        number_of_full_stripes = self.rlba_slice // self.chunk.slices_in_stripe
        clba_stripe_blocks = number_of_full_stripes * self.chunk.stripe_size * self.chunk.stripe_width
        slice_start_clba = clba_stripe_blocks + raid_offset_within_stripe_blocks + slice_offset_within_stripe_blocks
        return CLBA(self.chunk, slice_start_clba + self.index_in_slice)

    @property
    def vlba(self):
        return self.clba.vlba

    @property
    def blockset(self):
        return (self.lba // self.raid.slice_size // 32)

    @property
    def blockset_start(self):
        return self.blockset * self.raid.slice_size * 32

    def enumerate_slice_slbas(self):
        start = self.slba.srlba
        for i in range(self.raid.slice_size):
            yield (start + i).slba
        if self.raid.is_mirror:
            yield self.p_slba
        elif self.raid.is_ec:
            yield self.p_slba
            yield self.q_slba

    def enumerate_blockset_slbas(self):
        start = RLBA(self.raid, self.blockset_start)
        for i in range(32):
            for slba in (start + i*self.raid.slice_size).enumerate_slice_slbas():
                yield slba

    @property
    def key(self):
        return (self.raid, self.lba)


class SLBA:
    def __init__(self, _seg, slba):
        self.lba = slba
        self.seg = seg(*_seg) if type(_seg) == tuple else (seg_by_uuid(_seg) if type(_seg) == str else _seg)
        self.slba = self

    def __int__(self):
        return int(self.lba)

    def __int__(self):
        return int(self.lba)

    def __str__(self):
        return 'SLBA({0}, {1})'.format(str(self.seg), self.lba)

    def __repr__(self):
        return str(self)

    @property
    def raid(self):
        return self.seg.raid

    @property
    def chunk(self):
        return self.seg.chunk

    @property
    def vol(self):
        return self.seg.vol

    @property
    def dlba(self):
        return DLBA(self.seg.disk_name, self.seg.dlba_start + self.lba)

    @property
    def srlba(self):
        return RLBA(self.raid, self.lba * self.raid.slice_size)

    @property
    def rlba(self):
        block_in_slice = self.role
        if block_in_slice >= self.raid.slice_size:  # Parities are considered 0, same as in C code
            block_in_slice = 0
        return RLBA(self.raid, self.srlba.lba + block_in_slice)

    @property
    def clba(self):
        return self.rlba.clba

    @property
    def vlba(self):
        return self.clba.vlba

    @property
    def role(self):
        if self.seg.s - self.srlba.slice_owner.s < 0:
            return self.seg.s - self.srlba.slice_owner.s + self.raid.replicas
        else:
            return self.seg.s - self.srlba.slice_owner.s

    @property
    def key(self):
        return (self.seg, self.lba)


class DLBA:
    def __init__(self, id, dlba):
        self.lba = dlba
        seg = seg_by_uuid(id)
        if seg:
            self.disk_name = seg.disk_name
        else:
            self.disk_name = id
        self.dlba = self

    def __int__(self):
        return int(self.lba)

    def __str__(self):
        return 'DLBA("{0}", {1})'.format(self.disk_name, self.lba)

    def __repr__(self):
        return str(self)

    @property
    def seg(self):
        s = [s for s in segs.values() if s.disk_name ==
             self.disk_name and self.lba >= s.dlba_start and self.lba < s.dlba_end]
        if not s:
            raise RuntimeError('Could not find any segment on disk {0} that spans lba {1}'.format(
                self.disk_name, self.lba))
        return s[0]

    @property
    def raid(self):
        return self.seg.raid

    @property
    def chunk(self):
        return self.raid.chunk

    @property
    def vol(self):
        return self.chunk.vol

    @property
    def slba(self):
        return SLBA(self.seg, self.lba - self.seg.dlba_start)

    @property
    def rlba(self):
        return self.slba.rlba

    @property
    def clba(self):
        return self.rlba.clba

    @property
    def vlba(self):
        return self.clba.vlba

    @property
    def key(self):
        return (self.disk_name, self.lba)


def extend_class_list(cls_list, prop, value):
    """ Add attribute @prop with @value to each elemnt in cls_list
    """
    for cls in cls_list:
        assert not hasattr(cls, prop)
        setattr(cls, prop, value)


def extend_classes():
    """ For most classes here, many properties are as simple as return self.some_class.prop.
        Writing all those manually is pointless. Do it dynamically here.
    """
    extend_class_list([DLBA, RLBA, CLBA, VLBA], "role", property(lambda self: self.slba.role))
    extend_class_list([DLBA, SLBA, CLBA, VLBA], "mirror_slbas", property(lambda self: self.rlba.mirror_slbas))
    extend_class_list([DLBA, SLBA, CLBA, VLBA], "mirror_dlbas", property(lambda self: self.rlba.mirror_dlbas))
    extend_class_list([DLBA, SLBA, CLBA, VLBA], "blockset", property(lambda self: self.rlba.blockset))
    extend_class_list([DLBA, SLBA, CLBA, VLBA], "blockset_start", property(lambda self: self.rlba.blockset_start))
    extend_class_list([DLBA, SLBA, CLBA, VLBA], "p_segment", property(lambda self: self.rlba.p_segment))
    extend_class_list([DLBA, SLBA, CLBA, VLBA], "p_slba", property(lambda self: self.rlba.p_slba))
    extend_class_list([DLBA, SLBA, CLBA, VLBA], "p_dlba", property(lambda self: self.rlba.p_slba.dlba))
    extend_class_list([DLBA, SLBA, CLBA, VLBA], "q_segment", property(lambda self: self.rlba.q_segment))
    extend_class_list([DLBA, SLBA, CLBA, VLBA], "q_slba", property(lambda self: self.rlba.q_slba))
    extend_class_list([DLBA, SLBA, CLBA, VLBA], "q_dlba", property(lambda self: self.rlba.q_slba.dlba))
    extend_class_list([DLBA, RLBA, CLBA, VLBA], "srlba", property(lambda self: self.slba.srlba))
    extend_class_list([DLBA, SLBA, CLBA, VLBA], "slice_owner", property(lambda self: self.rlba.slice_owner))
    extend_class_list([DLBA, SLBA, CLBA, VLBA], "enumerate_slice_slbas", lambda self: self.rlba.enumerate_slice_slbas())
    extend_class_list([DLBA, SLBA, CLBA, VLBA], "enumerate_blockset_slbas", lambda self: self.rlba.enumerate_blockset_slbas())
    extend_class_list([DLBA, SLBA, RLBA, CLBA, VLBA], "lock_owner", property(lambda self: self.slice_owner.lock_owner))
    extend_class_list([DLBA, SLBA, RLBA, CLBA, VLBA], "__add__", lambda self, add: self.__class__(self.key[0], self.key[1] + add))


extend_classes()  # Call extend classes here, not in main


def public_props(o):
    """ List only public attributes of an object
    """
    return list(name for name in dir(o) if not name.startswith('_'))


def init_argparser():
    """ Initialize argument parser
    """
    parser = argparse.ArgumentParser('The Calculator', formatter_class=SmartFormatter, epilog='R|' + USAGE_EXAMPLE)
    parser.add_argument('--procdir', dest='procdir', action='store', type=str, nargs='+', help='List of volume proc dirs')
    parser.add_argument('-r', dest='recurse', action='store_true', help='Supplied procdirs represent proc volumes root dir')
    parser.add_argument('--umvolume', dest='umvolume', action='store', type=str, help='UM volume configuration extracted from UM traces via nvmeshum_query_volume_conf.py')
    return parser


def load_volume_procs(procdir, rec = False):
    """ Load the config of a single volume from proc
    """
    if (rec):
        for d in os.listdir(procdir):
            if os.path.isdir(os.path.join(procdir, d)):
                load_volume_procs(os.path.join(procdir, d))
        return

    with open(os.path.join(procdir, 'status')) as f:
        lines = f.readlines()

    with open(os.path.join(procdir, 'status.json')) as f:
        stat = json.load(f)

    v = stat['name'].encode('utf-8')
    volumes[v] = Volume(v)

    for chunk in stat['topo']['chunks']:
        for line in lines:
            c = chunk['ci']
            match = re.match(r'Chunk #' + str(c) + r': Stripe{Size=(\d+), Width=(\d+)} Slice{(\d+)\+(\d+)}', line)
            if match:
                stripe_size = int(match.group(1))
                stripe_width = int(match.group(2))
                ndata = int(match.group(3))
                nparity = int(match.group(4))
                replicas = ndata + nparity
            else:
                match = re.match(r'Chunk #' + str(c) + r': Stripe{Size=(\d+), Width=(\d+)} Replicas=(\d+)', line)
                if match:
                    stripe_size = int(match.group(1))
                    stripe_width = int(match.group(2))
                    replicas = int(match.group(3))
                    ndata = 1
        chunks[(v, c)] = Chunk(v, c, ndata, stripe_size, stripe_width, chunk['vlba_start'], chunk['vlba_end'])
        for raid in chunk['prs']:
            r = raid['ri']
            raids[(v, c, r)] = Raid(v, c, r, raid['version'], raid['lock'], ndata, replicas)
            for seg in raid['segs']:
                s = seg['si']
                segs[(v, c, r, s)] = Seg(v, c, r, s, seg['uuid'], seg['dlba_start'],
                                         seg['dlba_end'], seg['disk']['name'], seg['lmap'], host=seg["disk"]['host'])


UMVolumeRecords = collections.namedtuple('UMVolumeRecords', 'volume, chunks, praids, segments')
def parse_um_volume_file(um_volume_fpath):
    with open(um_volume_fpath) as f:
        lines = f.readlines()

    volume = None
    chunks = []
    praids_records = {}
    praids = collections.defaultdict(list) # ci : [record]

    segments_records = {}
    segments = collections.defaultdict(list) # (ci, ri) : [record]

    for line in lines:
        record = json.loads(line.encode('utf-8'))
        if 'VOLUME_PTR' in record:
            volume = record 
        elif 'CHUNK_PTR' in record:
            chunks.append(record)
        elif 'PRAID_PTR' in record:
            ci = int(record['CI'])
            ri = int(record['RI'])
            if (ci, ri) not in praids_records:
                praids[int(record['CI'])].append(record)
                praids_records[(ci,ri)] = record
            else:
                if 'LID' in record:
                    praids_records[(ci,ri)]['LID'] = record['LID']
                elif 'SI' in record and 'LI' in record and 0 == int(record['LI']) and 'SI_OF_LI' in record:
                    si = int(record['SI'])
                    segments_records[(ci,ri,si)]['lmap'] = 'O' + record['SI_OF_LI']
        elif 'SEGMENT_PTR' in record:
            ci = int(record['CI'])
            ri = int(record['RI'])
            si = int(record['SI'])
            if (ci, ri, si) not in segments_records:
                segments[(int(record['CI']), int(record['RI']))].append(record)
                segments_records[(ci,ri,si)] = record

    return UMVolumeRecords(volume=volume, chunks=chunks, praids=praids, segments=segments);

def load_um_volume(um_volume_fpath):
    """ Load the config of a single volume from UM generated traces
    """
    rcrds = parse_um_volume_file(um_volume_fpath)

    v = rcrds.volume['VOL'].encode('utf-8')
    volumes[v] = Volume(v)

    for chunk in rcrds.chunks:
        c = int(chunk['CI'])
        stripe_size = int(chunk['CHUNK_CONFIG_INFO.STRIPE_SIZE'])
        stripe_width = int(chunk['CHUNK_CONFIG_INFO.STRIPE_WIDTH'])
        ndata = int(rcrds.volume['VOLUME_CONFIG_INFO.N_DATA'])
        nparity = int(rcrds.volume['VOLUME_CONFIG_INFO.N_PARITY'])
        nmirrors = int(rcrds.volume['VOLUME_CONFIG_INFO.N_MIRRORS'])
        replicas = ndata + nparity + nmirrors
        vlba_start = int(chunk['CHUNK_CONFIG_INFO.VLBA_START'], base=16)
        vlba_end = int(chunk['CHUNK_CONFIG_INFO.VLBA_END'], base=16) + 1

        chunks[(v, c)] = Chunk(v, c, ndata, stripe_size, stripe_width, vlba_start, vlba_end)
        for raid in rcrds.praids[c]:
            #import pudb; pudb.set_trace()
            r = int(raid['RI'])
            version = int(raid['CONFIG_VERSION'], 16)
            raids[(v, c, r)] = Raid(v, c, r, version, lid=raid['LID'], slice_size=ndata, replicas=replicas)
            for seg in rcrds.segments[(c,r)]:
                s = int(seg['SI'])
                dlba_start = int(seg["SEGMENT_CONFIG_INFO.DLBA_START"], base=16)
                dlba_end = int(seg["SEGMENT_CONFIG_INFO.DLBA_END"], base=16) + 1
                segs[(v, c, r, s)] = Seg(v, c, r, s, seg['UUID_8'], dlba_start,
                                         dlba_end, seg['SEGMENT_CONFIG_INFO.DISK_NAME'], seg['lmap'], host=seg["hostname"])

def pretty_print_volumes():
    """ Helper function, prints the config of currently known volumes in a human readable way
    """
    for vol in volumes.values():
        print('{0}'.format(vol.long_str()))
        for chunk in vol.chunks():
            print('  {0}'.format(chunk.long_str()))
            for raid in chunk.raids():
                print('    {0}'.format(raid.long_str()))
                for seg in raid.segs():
                    print('       {0}'.format(seg.long_str()))


def main():
    args = init_argparser().parse_args()
    if args.procdir:
        for p in args.procdir:
            load_volume_procs(p, args.recurse)
    if args.umvolume:
        load_um_volume(args.umvolume)
    print(USAGE_EXAMPLE)
    IPython.embed()
    return 0


if __name__ == '__main__':
    import sys
    sys.exit(main())
