#!/usr/bin/env python3
"""
process_scheme:
1. create h file for interface.
2. runtime pack and unpack.
"""

import sys
import struct
import json
import re
import os
import time
import hashlib
import platform
from io import StringIO
import copy
import logging
import importlib
import uuid
from typing import BinaryIO, Tuple, Dict, List, Any, Optional

if sys.version_info[0] < 3 or sys.version_info == 3 and sys.version_info[1] < 8:
    raise Exception("MCS requires Python 3.8 or above")

Json = Dict[str, Any]
OpcodeType = int
LenType = int
UuidType = str
TimeStamp = int


# type for opcode instructions
# key is opcode number
# value is tuple for upstream and downstream instructions
OpcodeInst = Dict[int, List[str]]

SPECIAL_CHAR_REP = re.compile(r"[ !@#$%^&-/]+")

PACKER = None


class CodecHolder:
    """
    holds codec and expose codec interface
    about codecs:
    a codec is an object that pack data structures that need special
    scan and format. For example, a gid may arrive from management as string
    in hex format: "0xfe80000000000000e41d2d03001f9251" but we want to send
    two sets of 8 byte integers.
    """
    def __init__(self, codec):
        self.codec = codec

    def bin_repr(self):
        """binary representation of guid in python struct format"""
        return self.codec.bin_repr()

    def encode(self, str_data):
        """encode data from json string to binary"""
        return self.codec.encode(str_data)

    def decode(self, bin_data):
        """decode guid from binary format to string"""
        return self.codec.decode(bin_data)

    def default_val(self):
        """
        return default value to set when the value is missing from upstream
        message
        """
        return self.codec.default_val()

    def get_c_rep(self, var_name):
        """return c representation of a guid in the stub file"""
        return self.codec.get_c_rep(var_name)

# codecs:
# a codec is an object that pack data structures that need special
# scan and format. For example, a gid may arrive from management as string
# in hex format: "0xfe80000000000000e41d2d03001f9251" but we want to send
# two sets of 8 byte integers.
class GuidCodec:
    """encode-decode guids"""
    def __init__(self):
        pass

    def bin_repr(self):
        """
        binary representation of guid in python struct format
        """
        return "8H"

    def encode(self, str_data):
        """encode data from json string to binary"""
        if str_data[:2] != "0x":
            raise RuntimeError("guid format error")
        nacked = list(str(str_data[2:]))
        data_list = [''.join(x) for x in zip(nacked[2::4], nacked[3::4], nacked[0::4], nacked[1::4])]
        return struct.pack("8H", *[int(x, 16) for x in data_list])

    def decode(self, bin_data):
        """decode guid from binary format to string"""
        if len(bin_data) < 16:
            raise RuntimeError("guid binary representation should be at least 16 bytes")
        str_list = ["{0:04x}".format(x) for x in struct.unpack(">8H", bin_data)]
        return "0x{0}".format("".join(str_list))

    def get_c_rep(self, var_name):
        """return c representation of a guid in the stub file"""
        return "unsigned char " + var_name + "[16];"


class SecsSinceEpoch(object):
    def __init__(self):
        pass

    def bin_repr(self):
        """
        binary representation of guid in python struct format
        """
        return "Q"

    def encode(self, str_data):
        """encode data from json string to binary"""
        raise RuntimeError("SecsSinceEpoch only support unpack")

    def decode(self, bin_data):
        """decode guid from binary format to string"""
        val = struct.unpack(self.bin_repr(), bin_data)[0]
        if val == 0:
            val = int(time.time())
        return val

    def get_c_rep(self, var_name):
        """return c representation of a guid in the stub file"""
        return "unsigned long long " + var_name + ";"


class DefaultFromFieldProjection(object):
    """
    If data cannot be taken from a given message during pack, the codec will
    take data from a message (in the message stack).
    The codec uses the field value to select the field value in the selected
    message

     Attributes:
        inner	 inner codec to get default value if the alternative projection \
                data cannot be obtained
        level  the level of the message at messages stack relative to the corrent \
                level
        field field in the selected message from which the projected data is taken
    """

    def __init__(self, inner, field, level):
        """
        constructor
            :param self:
            :param inner: inner codec to get default value if the alternative projection data cannot be obtained
            :param field: field in the selected message from which the projected data is taken
            :param level: the level of the message at messages stack relative to the corrent level
        """
        self.inner = inner
        self.field = field
        self.level = level

    def bin_repr(self):
        """binary representation"""
        return self.inner.bin_repr()

    def encode(self, str_data):
        """encode binary data from json string"""
        return self.inner.encode(str_data)

    def decode(self, bin_data):
        """decode from binary data"""
        return self.inner.decode(bin_data)

    def get_c_rep(self, var_name):
        """return c representation for the stub file"""
        return self.inner.get_c_rep(var_name)

    def default_val(self):
        """
        return default value to set when the value is missing from upstream
        message
        """
        global PACKER
        defval = (PACKER.MsgStack[-1 - self.level]).get(self.field, None)
        if defval is None:
            defval=self.inner.default_val()
        return defval


class TopMessage:
    """
    provide the top message to the inner encode /decode
    """
    def __init__(self, inner):
        self.inner = inner

    def bin_repr(self):
        return self.inner.bin_repr()

    def encode(self, _):
        global PACKER
        PACKER.push_status()
        ret = self.inner.encode(PACKER.MsgStack[0])
        PACKER.pop_status()
        return ret

    def decode(self, bin_data):
        raise RuntimeError("Field value codec does not support decode")

    def get_c_rep(self, var_name):
        return self.inner.get_c_rep(var_name)

    def default_val(self):
        return None


class ParentMessage:
    """
    privide the parent message of the currect encoded message
    """
    def __init__(self, inner, relLevel):
        self.inner = inner
        self.relLevel = relLevel

    def bin_repr(self):
        return self.inner.bin_repr()

    def encode(self, _):
        global PACKER
        PACKER.push_status()
        ret = self.inner.encode(PACKER.MsgStack[-1 - self.relLevel])
        PACKER.pop_status()
        return ret

    def decode(self, bin_data):
        raise RuntimeError("ParentMessage codec does not support decode")

    def get_c_rep(self, var_name):
        return self.inner.get_c_rep(var_name)

    def default_val(self):
        global PACKER
        PACKER.push_status()
        ret = PACKER.MsgStack[-1 - self.relLevel]
        PACKER.pop_status()
        return ret


class Member:
    """
    provide member field from a given message
    """
    def __init__(self, inner, field):
        self.inner = inner
        self.field = field

    def bin_repr(self):
        return self.inner.bin_repr()

    def encode(self, str_data):
        if self.field not in str_data:
            gg = self.inner.default_val()
        else:
            gg = str_data[self.field]
        return self.inner.encode(gg)

    def decode(self, bin_data):
        """
        decode from binary data - not supported yet
        """
        raise RuntimeError("Field value codec does not support decode")

    def get_c_rep(self, var_name):
        return self.inner.get_c_rep(var_name)

    def default_val(self):
        return None


class Length:
    """
    encode a length of  list. for example if we want to get the number of targets
    """
    def __init__(self):
        pass

    def bin_repr(self):
        return 'I'

    def encode(self, str_data):
        return struct.pack("I", len(str_data))

    def decode(self, bin_data):
        """
        decode from binary data - not supported yet
        """
        raise RuntimeError("codec does not support decode")

    def get_c_rep(self, var_name):
        return "unsigned int {0};".format(var_name)

    def default_val(self):
        return None


class StructEncoder:
    """
    encode / decode a struct with a given type
    """
    def __init__(self, structType):
        self.structType = structType

    def bin_repr(self):
        """
        binary representation, will return the underlying struct type binary
        representation
        """
        global PACKER
        return do_get_pack_instr(PACKER.scheme[self.structType], PACKER.scheme, True)

    def encode(self, str_data):
        """
        encode struct, will use the default type packing algo from PACKER
        """
        global PACKER
        PACKER.push_status()
        ret = PACKER.packBody(self.structType, str_data)
        PACKER.pop_status()
        return ret

    def decode(self, bin_data):
        """
        decode from binary data - not supported yet
        """
        raise RuntimeError("Field value codec does not support decode")

    def get_c_rep(self, var_name):
        """
        c representation in the c include file (h file).
        There is no more than placing the struct by value in the
        containing struct
        """
        return "struct {0} {1};\n".format(self.structType, var_name)

    def default_val(self):
        """
        Provide json representation of a default.
        In the struct case, default value can be obtained only if all struct
        members have default value
        """
        global PACKER
        sch = PACKER.scheme[self.structType]
        ret = {}
        for k, obj in sch.items():
            if is_reserved_var_name(k):
                continue
            ret[k] = obj.default_val()
        return ret


class OpcodeCodec:
    """
    adds the opcode as integer to the body of packed message
    """
    def __init__(self):
        pass

    def bin_repr(self):
        return "I"

    def encode(self, str_data):
        return struct.pack("I", str_data)

    def decode(self, bin_data):
        raise RuntimeError("opcode codec is not meant to be used in upstream")

    def get_c_rep(self, var_name):
        return "int {0};\n".format(var_name)

    def default_val(self):
        global PACKER
        PACKER.push_status()
        ret = PACKER.traceback[0]["opcode"]
        PACKER.pop_status()
        return ret


class WithDefaultCodec:
    """
    implements default value, the inner codec is responible for encoding
    """
    def __init__(self, inner, def_val):
        self.inner = inner
        self.def_val = def_val

    def bin_repr(self):
        """binary representation"""
        return self.inner.bin_repr()

    def encode(self, str_data):
        """encode binary data from json string"""
        if str_data is None or str_data == '':
            return self.inner.encode(self.def_val)
        return self.inner.encode(type(self.def_val)(str_data))

    def decode(self, bin_data):
        """decode from binary data"""
        return self.inner.decode(bin_data)

    def get_c_rep(self, var_name):
        """return c representation for the stub file"""
        return self.inner.get_c_rep(var_name)

    def default_val(self):
        """
        return default value to set when the value is missing from upstream
        message
        """
        return self.def_val

def is_power_of_two(n: int) -> bool:
    """
    Determines if a given number is a power of two.

    Args:
        n (int): The number to check.

    Returns:
        bool: True if n is a power of two, otherwise False.
    """
    if n <= 0:
        return False
    return (n & (n - 1)) == 0

def power_of_two(n: int) -> Optional[int]:
    """
    Determines if a given number is a power of two and calculates the power.

    Args:
        n (int): The number to check.

    Returns:
        int: The power of two if n is a power of two, otherwise None.
    """
    if not is_power_of_two(n):
        return None
    power = 0
    while n > 1:
        n >>= 1
        power += 1
    return power

def validate_block_size(val : int) -> None:
    n = power_of_two(val)
    if not n:
        raise RuntimeError(f"given value is not power of two {val}")
    if n < 9 or n > 12:
        raise RuntimeError(f"block size must be at least 512 bytes {val}")


class VerifyBLockSize:
    """
    Verify that given value is power of 2 and that the power is greater equal 9
    """
    def __init__(self, inner):
        self.inner = inner

    def bin_repr(self):
        """binary representation"""
        return self.inner.bin_repr()

    def encode(self, val: int) -> bytes:
        """
        ensure that the give value is
        """
        validate_block_size(val)
        return self.inner.encode(val)

    def decode(self, bin_data: bytes) -> int:
        """decode from binary data"""
        val: int = self.inner.decode(bin_data)
        validate_block_size(val)
        return val

    def get_c_rep(self, var_name):
        """return c representation for the stub file"""
        return self.inner.get_c_rep(var_name)

    def default_val(self):
        """
        return default value to set when the value is missing from upstream
        message
        """
        return self.inner.default_val()


class PodCodec:
    """
    Plain Old Data codec
    """
    def __init__(self, arg):
        self.arg = arg

    def bin_repr(self):
        """binary representation"""
        return self.arg

    def encode(self, str_data):
        """encode binary data from json string"""
        if isinstance(str_data, str):
            str_data = str_data.encode("utf-8")
        try:
            return struct.pack(self.arg, str_data)
        except Exception as e:
            raise ValueError("Failed to run struct.pack of the data {0} to the argument {1}. Exception: {2}"
                             .format(str_data, self.arg, e))

    def decode(self, bin_data):
        """decode from binary data"""
        str_data = struct.unpack(self.arg, bin_data)[0]
        if isinstance(str_data, bytes):
            nn0 = str_data.find(b'\x00')
            if nn0 >= 0:
                str_data = str_data[:nn0]
            str_data = str_data.decode("utf-8")
        return str_data

    def get_c_rep(self, var_name):
        """return c representation for the stub file"""
        output = StringIO()
        process_h_attr(output, var_name, self.arg)
        contents = output.getvalue()
        output.close()
        return contents[:-1]


class BoolToIntCodec:
    """
    convert client int to textual "true" or "false" that will be sent to management
    """
    def __init__(self, pod: PodCodec):
        self.pod = pod

    def bin_repr(self) -> str:
        """binary representation in python struct format"""
        return self.pod.bin_repr()

    def encode(self, inp: bool) -> bytes:
        """encode binary data from json string"""
        if inp is True:
            return self.pod.encode(1)
        assert inp is False
        return self.pod.encode(0)

    def decode(self, bin_data: bytes) -> bool:
        """decode from binary data"""
        res: int = int(self.pod.decode(bin_data))
        if res != 0:
            return True
        return False

    def get_c_rep(self, var_name: str) -> str:
        """encode binary data from json string"""
        return self.pod.get_c_rep(var_name)


class UpstrmFrmtHexCodec:
    """
    unpack and format to hexa
    """
    def __init__(self, arg, cut):
        self.pod = PodCodec(arg)
        self.cut = cut

    def bin_repr(self):
        """binary representation"""
        return self.pod.bin_repr()

    def encode(self, str_data):
        """encode binary data from json string"""
        return self.pod.encode(str_data)

    def decode(self, bin_data):
        """decode from binary data"""
        if self.cut:
            return hex(self.pod.decode(bin_data))[2:]
        else:
            return hex(self.pod.decode(bin_data))

    def get_c_rep(self, var_name):
        """return c representation for the stub file"""
        return self.pod.get_c_rep(var_name)


class ReplaceCodec:
    """
    replace chars for upsteam message
    """
    def __init__(self, s, r, arg):
        self.pod = PodCodec(arg)
        self.s = s
        self.r = r

    def bin_repr(self):
        """binary representation"""
        return self.pod.bin_repr()

    def encode(self, str_data):
        """encode binary data from json string"""
        return self.pod.encode(str_data)

    def decode(self, bin_data):
        """decode from binary data"""
        ret = self.pod.decode(bin_data).split('\x00')[0]
        ret = ret.replace(self.s, self.r)
        return ret

    def get_c_rep(self, var_name):
        """return c representation for the stub file"""
        return self.pod.get_c_rep(var_name)


class FormatCodec:
    """
    format the object according to the given pyhon format string
    """
    def __init__(self, frmt, inner):
        self.frmt = frmt
        self.inner = inner

    def bin_repr(self):
        """binary representation"""
        return self.inner.bin_repr()

    def encode(self, str_data):
        """encode binary data from json string"""
        raise RuntimeError("FormatCodec is support only upstream messages")

    def decode(self, bin_data):
        """decode from binary data"""
        ret = self.inner.decode(bin_data)
        return self.frmt.format(ret)

    def get_c_rep(self, var_name):
        """return c representation for the stub file"""
        return self.inner.get_c_rep(var_name)


class IncrementCodec:
    """
    Will increment the value from upstream by the given value.
    if the field is missing in upstream set the increment val to be default
    """
    def __init__(self, inc_by, pod_d):
        self.inc_by = inc_by
        self.pod_d = pod_d

    def bin_repr(self):
        """binary representation"""
        return self.pod_d

    def encode(self, str_data):
        """encode binary data from json string"""
        if not isinstance(str_data, int):
            str_data = int(str_data)
        return struct.pack(self.pod_d, str_data + self.inc_by)

    def decode(self, bin_data):
        """decode from binary data"""
        ret = struct.unpack(self.pod_d, bin_data)[0]
        ret -= self.inc_by
        return ret

    def get_c_rep(self, var_name):
        """return c representation for the stub file"""
        output = StringIO()
        process_h_attr(output, var_name, self.pod_d)
        contents = output.getvalue()
        output.close()
        return contents[:-1]

    def default_val(self):
        """
        return default value to set when the value is missing from upstream
        message
        """
        return 0


class MultiValCodec:
    """encode jsong string into enum const"""
    def __init__(self, val_map, rev_val_map=None):
        self.val_map = val_map
        if not rev_val_map:
            self.rev_val_map = {}
            for k, v in val_map.items():
                if v not in self.rev_val_map:
                    self.rev_val_map[v] = k
        else:
            self.rev_val_map = rev_val_map

    def bin_repr(self):
        """binary representation"""
        return "i"

    def encode(self, str_data):
        """encode binary data from json string"""
        str_data = str(str_data)
        if not str_data in self.val_map:
            return struct.pack("i", self.val_map["__DEFAULT"])
        return struct.pack("i", self.val_map[str_data])

    def decode(self, bin_data):
        """decode from binary data"""
        indx = struct.unpack("i", bin_data)[0]
        if indx < 0 or indx >= len(self.rev_val_map):
            raise RuntimeError("MultiValCodec: index out of range {}".format(indx))
        return self.rev_val_map[indx]

    def get_c_rep(self, var_name):
        """return c representation for the stub file"""
        # the following line format nice enum
        ret = ""
        for vv0, kk0 in sorted(self.rev_val_map.items()):
            if not isinstance(kk0, str):
                continue
            if kk0 == "__DEFAULT":
                continue
            kk0 = SPECIAL_CHAR_REP.sub("_", kk0)
            ret += "#define {2}_{0} {1}\n".format(kk0.upper(), vv0, var_name.upper())
        ret += "int "
        ret += var_name
        ret += ";"
        return ret

class MultiFlagCodec:
    """encode json string array into enum const indexed bitmap"""
    def __init__(self, flag_map, rev_flag_map=None):
        self.flag_map = flag_map
        if not rev_flag_map:
            self.rev_flag_map = {}
            for k, v in flag_map.items():
                if v not in self.rev_flag_map:
                    self.rev_flag_map[v] = k
        else:
            self.rev_flag_map = rev_flag_map

    def bin_repr(self):
        """binary representation"""
        return "Q"

    def encode(self, str_data):
        """encode binary data from json string array"""
        #str_data = str(str_data)
        flags = 0
        for flag_str in self.flag_map:
            if flag_str in str_data:
                flags = flags | (1 << self.flag_map[flag_str])
        return struct.pack("Q", flags)

    def decode(self, bin_data):
        """decode from binary data"""
        flags = struct.unpack("Q", bin_data)[0]
        return [self.rev_flag_map[flag] for flag in self.rev_flag_map if flags & (1 << flag)]

    def get_c_rep(self, var_name):
        """return c representation for the stub file"""
        # the following line formats nice enum
        ret = ""
        for vv0, kk0 in sorted(self.rev_flag_map.items()):
            if not isinstance(kk0, str):
                continue
            if kk0 == "__DEFAULT":
                continue
            kk0 = SPECIAL_CHAR_REP.sub("_", kk0)
            ret += "#define {2}_FLAG_{0} {1}\n".format(kk0.upper(), vv0, var_name.upper())
        ret += "unsigned long long "
        ret += var_name
        ret += ";"
        return ret

class MajorMinorCodec:
    """
    convert two integers from the client side to major.
    minor transfered to the management
    """
    def __init__(self):
        pass

    def bin_repr(self):
        """binary representation"""
        return "ii"

    def encode(self, str_data):
        """encode binary data from json string"""
        major, minor = str_data.split('.')
        major = int(major)
        minor = int(minor)
        return struct.pack("ii", major, minor)

    def decode(self, bin_data):
        """decode guid from binary format to string"""
        major, minor = struct.unpack("ii", bin_data)
        return "{0}.{1}".format(major, minor)

    def get_c_rep(self, var_name):
        """return c representation for the stub file"""
        return "int {0}Major; int {0}Minor;".format(var_name)


class UuidCodec:
    """
    ###about:
    converts uuid string to byte array of 16
    managements inpute takes the form 8ae6a0e8-0c42-11e7-b66c-305a3a54073f
    """
    def __init__(self):
        pass

    def bin_repr(self):
        """binary representation"""
        return "16s"

    def encode(self, str_data):
        """encode binary data from json string"""
        return uuid.UUID(str_data).bytes

    def decode(self, bin_data: bytes) -> str:
        """decode guid from binary format to string"""
        return str(uuid.UUID(bytes=bin_data))

    def get_c_rep(self, var_name):
        """return c representation for the stub file"""
        return "unsigned char {0}[16];".format(var_name)


class ConstCodec:
    """
    about:
    sets a given constant into upstream message
    """
    def __init__(self, val):
        self.val = val

    def bin_repr(self):
        """binary representation"""
        return ""

    def encode(self, str_data):
        """
        don't do nothing, the other side don't see this paramter
        """
        return b""

    def decode(self, bin_data):
        """
        plant the host name into the incomming message
        """
        return self.val

    def get_c_rep(self, var_name):
        """return c representation for the stub file"""
        return ""


class ClientAPIVersionCodec(ConstCodec):
    """ Encodes the client API version"""
    def __init__(self):
        super().__init__(PACKER.scheme['enum_client_api_version']['__def']['NVMEIB_C_TO_M_FEATURE_COMPATIBILITY_VERSION'])


class HostNameCodec:
    """
    # about:
    adds the current host name to a message, the other sided don't need
    to set the parametr
    """
    def bin_repr(self):
        """binary representation"""
        return ""

    def encode(self, str_data):
        """
        don't do nothing, the other side don't see this paramter
        """
        return ""

    def decode(self, bin_data):
        """
        plant the host name into the incomming message
        """
        return platform.node()

    def get_c_rep(self, var_name):
        """return c representation for the stub file"""
        return ""


class VersionCodec:
    """
    # about:
    adds the current host name to a message, the other sided don't need
    to set the parametr
    """
    def bin_repr(self):
        """binary representation"""
        return ""

    def encode(self, str_data):
        """
        don't do nothing, the other side don't see this paramter
        """
        return ""

    def decode(self, bin_data):
        """
        plant the host name into the incomming message
        """
        global PACKER
        return PACKER.version_data["version"]

    def get_c_rep(self, var_name):
        """return c representation for the stub file"""
        return ""


class CommitCodec:
    """
    # about:
    adds the current host name to a message, the other sided don't need
    to set the parametr
    """
    def bin_repr(self):
        """binary representation"""
        return ""

    def encode(self, str_data):
        """
        don't do nothing, the other side don't see this paramter
        """
        return ""

    def decode(self, bin_data):
        """
        plant the host name into the incomming message
        """
        global PACKER
        return PACKER.version_data["commit"]

    def get_c_rep(self, var_name):
        """return c representation for the stub file"""
        return ""


class BranchCodec:
    """
    # about:
    adds the current host name to a message, the other sided don't need
    to set the parametr
    """
    def bin_repr(self):
        """binary representation"""
        return ""

    def encode(self, str_data):
        """
        don't do nothing, the other side don't see this paramter
        """
        return ""

    def decode(self, bin_data):
        """
        plant the host name into the incomming message
        """
        global PACKER
        return PACKER.version_data["branch"]

    def get_c_rep(self, var_name):
        """return c representation for the stub file"""
        return ""

CODEC_DEF_RE = re.compile("^__codec +(.*)")

def eval_codec(def_str):
    """return the codec definition out of the json"""
    mm0 = CODEC_DEF_RE.search(def_str)
    if mm0 is None:
        return None
    return eval(mm0.groups()[0])


FORMATTER = re.compile("^([0-9]*)([^0-9]+)")


class Empt: # pylint: disable=too-few-public-methods
    """
    an empty class
    """
    def __init__(self):
        pass

__E__ = Empt()
__OBJ_TYPE = type(__E__)

# array of arrays that includes incarnations offsets for each type
# each element in the arrays include a tuple of the offs and type
INCR_PTRS = []

REPLACER_RE = re.compile(r"\{([^\}]*)\}")

HEADER_MSG = """
{
        "__globals" : {
            "__version" : 100
            },
        "header" : {
            "//" : "Golobal message header with fields commot to all messages",
            "__type" : "__header",
            "msg_len": {
                "msg_len": "I",
                "var_offset": "H"
                },
            "header_version" : "I",
            "scheme_version" : "I",
            "opcode": "I",
            "timestamp": "Q",
            "token": "__codec UuidCodec()",
            "client_generated_uuid": "__codec UuidCodec()"
            }
        }
"""


# load json into ordered dict, so we will not lose the order of the
# attributes in a scheme file
def load_json(f_desc: BinaryIO) -> Tuple[Json, str, str]:
    """
    load schem into the memory
    """
    data = f_desc.read().decode("utf-8").replace("\n", '')
    ret: Json = json.loads(data)
    header = HEADER_MSG.replace("\n", "")
    body: Json = json.loads(header)
    ret["header"] = body["header"]
    return ret, header, data

def calc_check_sum(ret: Json, header: str, data: str) -> Json:
    md5 = hashlib.md5()
    md5.update(header.encode("utf-8"))
    ret["__header_version"] = int(md5.hexdigest()[:8], 16)
    md5.update(data.encode("utf-8"))
    ret["__globals"]["__version"] = int(md5.hexdigest()[:8], 16)
    return ret


def load_header():
    """
    load the common header into memory
    """
    header = HEADER_MSG.replace("\n", "")
    md5 = hashlib.md5()
    md5.update(header.encode("utf-8"))
    ret = json.loads(header)
    ret["__globals"]["__version"] = int(md5.hexdigest()[:8], 16)
    return ret


def process_h_attr(file_desc, varname, frmt_):
    """
    translates python struct defs into c types
    """
    if isinstance(frmt_, CodecHolder):
        file_desc.write(frmt_.get_c_rep(varname))
        file_desc.write("\n")
        return
    elif not isinstance(frmt_, str):
        raise RuntimeError("type of {0} is not c evalueable. scheme contains {1} type {2}".\
                format(varname, frmt_, type(frmt_)))
    elif frmt_.startswith("__codec"):
        # the following line creates codec object and run get_c_rep
        c_rep = eval_codec(frmt_).get_c_rep(varname)
        if len(c_rep):
            file_desc.write(eval_codec(frmt_).get_c_rep(varname))
            file_desc.write("\n")
        return

    mm0 = FORMATTER.search(frmt_)
    groups = mm0.groups()
    array_size = groups[0]
    frmt = groups[1]

    if frmt == "c":
        file_desc.write("char ")
    elif frmt == "b":
        file_desc.write("signed char ")
    elif frmt == "B":
        file_desc.write("unsigned char ")
    elif frmt == "h":
        file_desc.write("short ")
    elif frmt == "H":
        file_desc.write("unsigned short ")
    elif frmt == "i":
        file_desc.write("int ")
    elif frmt == "I":
        file_desc.write("unsigned int ")
    elif frmt == "l":
        file_desc.write("long ")
    elif frmt == "L":
        file_desc.write("unsigned long ")
    elif frmt == "q":
        file_desc.write("long long ")
    elif frmt == "Q":
        file_desc.write("unsigned long long ")
    elif frmt == "f":
        file_desc.write("float ")
    elif frmt == "d":
        file_desc.write("double ")
    elif frmt == "s" or frmt == "p":
        file_desc.write("char ")
    elif frmt == "P":
        file_desc.write("void *")
    else:
        file_desc.write("struct " + frmt + " ")

    file_desc.write(varname)
    if len(array_size):
        file_desc.write("[" + array_size + "]")
    file_desc.write(";\n")

def is_def_type(scheme, name):
    """
    test if a function is a definition type
    """
    if name in scheme.keys() and isinstance(scheme[name], dict)\
            and "__type" in scheme[name].keys() and scheme[name]["__type"] == "enum":
        return True
    return False

def is_array(obj):
    """
    test if a memeber is an array
    """
    if "__type" in obj and obj["__type"] == "array":
        return True
    return False

def is_array_with_coded(obj):
    """
    test if the array unpacking needs to use a helper function to unpack the array
    """
    return "__codec" in obj

def concat(obj):
    """
    test if the objects needs to be concatenated, for example in strings
    """
    if "__join" in obj and obj["__join"] == True:
        return True
    return False

# declared structs so the program can tell if forward definition is needed
DECKARED = {}

def create_forward_def(file_obj, scheme, struct_name):
    """
    create c forward definition line
    """
    obj = scheme[struct_name]
    obj_type = obj["__type"]
    if obj_type == "used":
        file_obj.write("struct {0};\n".format(struct_name))
        DECKARED[struct_name] = 1

def create_h_struct(scheme, file_obj, vartype, varname, obj, inline, opcodes):
    """
    helper fuction that create h struct definition in c file
    """
    ret = False
    if len(vartype) and vartype in DECKARED.keys():
        return True
    if '//' in obj.keys():
        file_obj.write("/* " + obj['//'] + " */\n")
    DECKARED[vartype] = 1
    file_obj.write("struct " + vartype + "{\n")
    for (attr, that) in obj.items():
        if isinstance(that, dict):
            if is_array(that):
                if "__offset" not in that:
                    raise RuntimeError("dyamic array {0} in {1} must have"\
                            " offset attribue that points to the"\
                            " array in var_data".format(attr, varname))
                if "__counter" not in that:
                    raise RuntimeError("dynamic array must hold the amoung of"\
                            " item, therefore __counter attr must be"\
                            " provided. (element {0} in {1}))".\
                            format(attr, varname))
                file_obj.write("int {0};\n".format(that["__counter"]))
                file_obj.write("struct {0} *{1};\n".format(that["__typename"], that["__offset"]))
                continue
            create_h_struct(scheme, file_obj, "", attr, that, True, opcodes)
            continue
        if attr == "//":
            continue
        if attr == "__opcode" and varname != "header":
            ret = True
            opcodes.append(("MCS_" + varname.upper() + "_MSG", that))
        elif is_def_type(scheme, that):
            file_obj.write("struct {0} {1};\n".format(that, attr))
            continue
        if len(attr) < 2 or not attr.startswith("__"):
            process_h_attr(file_obj, attr, that)
    if inline:
        file_obj.write("} __attribute__((packed)) " + varname + ";\n")
    else:
        file_obj.write("} __attribute__((packed));\n\n")
    return ret


def create_enum(header, name, __def):
    """
    write new enum definition to h file
    """
    header.write("typedef enum { \n")
    items = list(__def.items())
    print_item = lambda n, v: header.write(f"{n} = {v}")
    for en_name, en_val in items[:-1]:
        print_item(en_name, en_val)
        header.write(",\n")
    print_item(items[-1][0], items[-1][1])
    header.write("\n}" + "{0};\n\n".format(name))


def create_type(header, name, obj):
    """
    create new type definition for c header file
    """
    if "//" in obj.keys():
        header.write("/* {0} */\n".format(obj['//']))
    if not "__def" in obj.keys():
        raise ValueError("type {0} instruction without definition".format(name))
    if obj["__type"] == "enum":
        create_enum(header, name, obj["__def"])
        return

    raise ValueError("unsupported type definition")

def is_new_struct(obj):
    """
    test if a scheme is a struct
    """
    tt0 = obj["__type"]
    return tt0 == "msg" or tt0 == "payload" or tt0 == "used"

def is_def(obj) -> bool:
    """
    test if object is definition
    """
    return "__def" in obj

def is_msg(obj):
    """
    test if obj is message
    """
    tt0 = obj["__type"]
    return tt0 == "msg"

# returns true if the varname is reserved.
def is_reserved_var_name(varname):
    """
    return true if varname is reserved
    """
    return varname[0:2] == "__" or varname == "//"

def create_incarnation_ptrs(scheme, msg, msgid, offs, vars_to_indx, incr_ptrs_):
    """
    create incarnation offsets table into the h file
    """
    if not msgid in incr_ptrs_.keys():
        incr_ptrs_[msgid] = []
    for (varname, obj) in msg.items():
        if is_reserved_var_name(varname):
            continue

        if isinstance(obj, str) and obj in scheme.keys():
            offs = create_incarnation_ptrs(scheme, scheme[obj], msgid, offs, vars_to_indx, incr_ptrs_)
            continue
        if isinstance(obj, CodecHolder):
            st_rep = obj.bin_repr()
            offs += struct.calcsize(st_rep)
        elif isinstance(obj, str):
            st_rep = eval_codec(obj).bin_repr() if obj.startswith("__codec") else obj
            offs += struct.calcsize(st_rep)
            continue
        if not isinstance(obj, dict):
            continue
        obj_type = obj["__type"] if "__type" in obj else "used"
        if obj_type == "used":
            offs = create_incarnation_ptrs(scheme, obj, msgid, offs, vars_to_indx, incr_ptrs_)
            continue
        if obj_type == "array":
            incr_ptrs_[msgid].append((offs + 4, vars_to_indx[obj["__typename"]]))
            offs += 12  # 4 for counter + 8 for offset
            continue
    return offs

def removekey(msg, k):
    """
    helper function that returns a key from dict and remove the key
    """
    res = msg[k]
    del msg[k]
    return res


def get_inner_used_types(scheme: Json, name: str, msg: Json, declared: List[str]):
    """
    get list of inner used types, data is set into "declared"
    """
    if len(name) and (name[0:2] == "__" or name in declared):
        return
    for (_, item) in msg.items():
        itemisdict = isinstance(item, dict)
        if itemisdict and "__type" in item and item["__type"] == "array":
            array_type = item["__typename"]
            get_inner_used_types(scheme, array_type, scheme[array_type], declared)
            declared.append(array_type)
            continue
        if itemisdict:
            get_inner_used_types(scheme, "", item, declared)
            continue
        if item in scheme:
            get_inner_used_types(scheme, item, scheme[item], declared)

    if len(name):
        declared.append(name)

def create_h_file(scheme, out_fn, create_consts_):
    """
    create h file from given scheme.
    data is writen into file "outfn".
    constatants that needs to be added to the h file is added to "create_consts"
    """
    global INCR_PTRS # pylint: disable=global-statement, locally-disabled
    opcodes = []
    msg_types = []
    INCR_PTRS = {}
    replace_data(scheme)
    fnDir = os.path.dirname(out_fn)
    if len(fnDir) and not os.path.exists(fnDir):
        os.makedirs(fnDir)
    with open(out_fn, 'w') as header:
        header.write("/*This header was generated automaticaly from scheme, do change it*/\n\n")
        if create_consts_:
            header.write("#ifndef {0}\n".format(scheme["__globals"]["__scheme_name"]))
            header.write("#define {0}\n\n".format(scheme["__globals"]["__scheme_name"]))
        else:
            header.write("#ifndef NVMEIB_MCS_HEADER_SCHM\n")
            header.write("#define NVMEIB_MCS_HEADER_SCHM\n\n")
        dec_order = []
        version = scheme["__globals"]["__version"]
        if not isinstance(version, int):
            raise RuntimeError("__version field must be integer")
        for varname, obj in scheme.items():
            get_inner_used_types(scheme, varname, obj, dec_order)
        for varname in dec_order:
            obj = scheme[varname]
            if isinstance(obj, dict):
                if varname == "header":
                    continue
                if not "__type" in obj.keys():
                    raise RuntimeError("Must have __type field in obj {0}".format(varname))
                elif is_def(obj):
                    create_type(header, varname, obj)
                    continue
                vartype = obj.get("__typename") or varname
                if create_h_struct(scheme, header, vartype, varname, obj, False, opcodes):
                    if is_msg(obj):
                        msg_types.append((vartype, varname))
        opcodes_to_incr = {}
        items_size_of = []
        i = 0
        vars_to_indx = {}
        max_opcode = -1
        for varname, obj in scheme.items():
            if is_reserved_var_name(varname):
                continue
            header.write("/*{0} id {1}*/\n".format(varname, i))
            vars_to_indx[varname] = i
            i += 1
        i = 0
        for varname, obj in scheme.items():
            if is_reserved_var_name(varname):
                continue
            if isinstance(obj, dict) and\
                    "__type" in obj and obj["__type"] == "msg":
                is_upstream = obj["__upstream"] if "__upstream" in obj else True
                is_downstream = obj["__downstream"] if "__downstream" in obj else True
                opcode = obj["__opcode"]
                if opcode > max_opcode:
                    max_opcode = opcode
                if opcode not in opcodes_to_incr:
                    opcodes_to_incr[obj["__opcode"]] = [-1, -1]
                if is_upstream:
                    opcodes_to_incr[obj["__opcode"]][0] = i
                if is_downstream:
                    opcodes_to_incr[obj["__opcode"]][1] = i

            pack_inst = get_pack_instr(obj, scheme, True)
            items_size_of.append(struct.calcsize(str(pack_inst)))
            create_incarnation_ptrs(scheme, obj, i, 0, vars_to_indx, INCR_PTRS)
            i += 1
        if create_consts_:
            header.write("#define MCS_MAX_OPCODE {0}\n".format(max_opcode))
        if not create_consts_:
            header.write("struct mcs_message {\n")
            obj = scheme["header"]
            create_h_struct(scheme, header, "header", "header", obj, True, opcodes)
            header.write("char msg[0];\n")
            header.write("} __attribute__((packed));\n\n")
            header.write("static const unsigned int MCS_HEADER_VERSION = {0};\n".format(version))
        else:
            op_to_str = ["", "static inline const char *__bcm_comm_base_mcs_op_to_str(int opcode)", "{", "    switch (opcode)", "    {"]
            for opc in opcodes:
                header.write("#define " + opc[0] + " (" + str(opc[1]) + ")\n")
                op_to_str.append("    case {0}: return \"{0}\";".format(opc[0]))
            # calculate the max num of elements in the ptrs. that is the longes list in map values
            op_to_str.append("    }")
            op_to_str.append("    return NULL;")
            op_to_str.append("}")
            op_to_str.append("")
            header.write("\n".join(op_to_str))
            maxptrnum = len(max(INCR_PTRS.values(), key=lambda x: len(x)))
            num_ptrs = len(INCR_PTRS.keys())
            header.write("static const unsigned int MCS_SCHEME_VERSION = 0x{0:x};\n".format(version))
            header.write("#define MCS_SCHEME_VERSION_STR \"0x{0:x}\"\n".format(version))
            header.write("static const int NUM_MSGS = {0};\n".format(num_ptrs))
            header.write("static const int NUM_OFFSETS = {0};\n".format(maxptrnum))
            header.write("static const int incr_offst[{0}][{1}] = {{\n".format(num_ptrs, maxptrnum))
            for i in range(num_ptrs):
                header.write("{")
                ll0 = 0
                if i in INCR_PTRS.keys():
                    ll0 = len(INCR_PTRS[i])
                    header.write(",".join([str(x[0]) for x in INCR_PTRS[i]]))
                if ll0 < maxptrnum:
                    header.write(("," if ll0 else "") + ",".join(["-1"] * (maxptrnum - ll0)))
                header.write("}")
                if i < num_ptrs - 1:
                    header.write(',')
                header.write("\n")
            header.write("};\n\n")
            header.write("static const int incr_types[{0}][{1}] = {{\n".format(num_ptrs, maxptrnum))
            for i in range(num_ptrs):
                header.write("{")
                ll0 = 0
                if i in INCR_PTRS.keys():
                    ll0 = len(INCR_PTRS[i])
                    header.write(",".join([str(x[1]) for x in INCR_PTRS[i]]))
                if ll0 < maxptrnum:
                    header.write(("," if ll0 else "") + ",".join(["-1"] * (maxptrnum - ll0)))
                header.write("}")
                if i < num_ptrs - 1:
                    header.write(',')
                header.write("\n")
            header.write("};\n\n")
            header.write("static const int opcodes_to_incr_id_upstr[] = {")
            maxopcode = max(opcodes_to_incr.keys()) if len(opcodes_to_incr) else 0
            header.write(','.join([str(opcodes_to_incr[i][0])\
                    if i in opcodes_to_incr else "-1" for i in range(maxopcode + 1)]))
            header.write("};\n")
            header.write("static const int opcodes_to_incr_id_downstr[] = {")
            maxopcode = max(opcodes_to_incr.keys()) if len(opcodes_to_incr) else 0
            header.write(','.join([str(opcodes_to_incr[i][1])\
                    if i in opcodes_to_incr else "-1" for i in range(maxopcode + 1)]))
            header.write("};\n")
            header.write("static const size_t sizeof_items[] = {")
            header.write(','.join([str(item_size) for item_size in items_size_of]))
            header.write('};\n')
        header.write("\n#endif\n")

def flat_list(vv0):
    """
    flat a tree (list of lists....) to a flat tree (list)
    """
    flatten = lambda x:\
            [y for l in x for y in flatten(l)] if isinstance(x, list) else [x]
    return flatten(vv0)


def do_get_pack_instr(msg_scheme: Json, global_scheme: Json, ignore_opcode: int) -> str:
    """
    Helper fuction!!!
    recursive fucntion that find pack instruction of a message
    """
    res = ""
    for (var, frmt) in msg_scheme.items():
        is_opcode = var == "__opcode"
        if (ignore_opcode and is_opcode) or (not is_opcode and var[0:2] == '__') or var == "//":
            continue
        if isinstance(frmt, dict):
            if is_array(frmt):
                res += "iii"
            else:
                res += do_get_pack_instr(frmt, global_scheme, ignore_opcode)
        elif isinstance(frmt, CodecHolder):
            res += frmt.bin_repr() + " "
        elif frmt.startswith("__codec"):
            res += eval_codec(frmt).bin_repr() + " "
        elif frmt in global_scheme.keys():
            if is_def_type(global_scheme, frmt):
                res += get_def_type_instr(global_scheme[frmt]) + " "
            else:
                res += do_get_pack_instr(global_scheme[frmt], global_scheme, ignore_opcode)
        else:
            res += frmt + " "
    return res


def get_pack_instr(msg_scheme, global_scheme, ignore_opcode):
    """
    return pack instruction of a message
    """
    return "=" + do_get_pack_instr(msg_scheme, global_scheme, ignore_opcode)


def timestamp() -> TimeStamp:
    """
    calculate current time stamp
    """
    return int(time.time() * 1000000)


def get_def_type_instr(obj):
    """
    return the pack instruction of a give def message
    """
    if obj["__type"] == "enum":
        return "i"

    return "I"


class Msg_Info:
    """
    holds information about a message
    """
    pack_instr = ""

    length = 0

    def __init__(self, _pack_instr):
        self.pack_instr = str(_pack_instr)
        self.length = struct.calcsize(self.pack_instr)


def replace_data(scheme: Json) -> None:
    """
        replace all __data with data from the scheme.
        for now we only support replacement on one level of the scheme
        """
    for frmt in scheme.values():
        if not isinstance(frmt, dict):
            continue
        if "__data" in frmt and frmt["__data"] in scheme:
            for v2, frmt2 in scheme[frmt["__data"]].items():
                if not is_reserved_var_name(v2):
                    frmt[v2] = frmt2


def inflate(scheme: Json, data2: str, inc_dir: str) -> Tuple[Json, str]:
    """
    looks for include commands in the scheme and add fields accrodingly
    """
    retdata = data2
    schemes: List[Json] = []
    that = copy.deepcopy(scheme)
    for (field, obj) in scheme.items():
        if isinstance(obj, str) and obj == "include":
            fn = os.path.join(inc_dir, field)
            with open(fn, "r+", encoding="utf-8") as f_desk:
                data = f_desk.read().replace("\n", '')
                schemes.append(json.loads(data))
                retdata += data

    scheme = {}
    i = 0
    for (field, obj) in that.items():
        if isinstance(obj, str) and obj == "include":
            newone = schemes[i]
            i += 1
            for (key, value) in newone.items():
                scheme[key] = value
        else:
            scheme[field] = obj
    return scheme, retdata

# holds the entire schem
class Packer:
    """
    ###packer class
    read scheme, packs messages and... unpack them
    """
    global __OBJ_TYPE

    def __init__(self, file_name: str, inst_path: Optional[str] = None, logger: logging.Logger = None, reverse=False) -> None:
        self.payload = ""
        # self.types = {}
        self.logger = logger
        self.scheme: Json = {}
        self.var_offset: int = 0
        self.bin_msg: bytes = b""
        global PACKER
        PACKER = self
        # traceback helps to debug and get more detailed information about errors
        self.traceback: List[Dict[str, Any]] = []
        self.StatusStack = []
        # message stack contains messages at current level and up
        self.MsgStack = []
        self.size_arr_desc = struct.calcsize("=iQ")
        if inst_path is None:
            inst_path = "/opt/nvmesh/client-repo/management_cm"
        with open(file_name, "r+b") as f_desc:
            self.scheme, self.loaded_header, data = load_json(f_desc)

        if (reverse):
            for key, val in self.scheme.items():
                if isinstance(val, dict) and "__downstream" in val:
                    self.scheme[key]['__downstream'] = not self.scheme[key]['__downstream']
                    self.scheme[key]['__upstream'] = not self.scheme[key]['__upstream']
                    self.scheme[key]["__route"] = "/dummy"
                    self.scheme[key]["__messageType"] = "attachVolume"

        self.scheme, self.data = inflate(self.scheme, data, inst_path)
        calc_check_sum(self.scheme, self.loaded_header, self.data)
        for i, obj in enumerate(self.scheme.values()):
            if isinstance(obj, dict):
                obj["__typeid"] = i
        replace_data(self.scheme)
        self.scheme_version = self.scheme["__globals"]["__version"]
        modname = globals()['__name__']
        module = sys.modules[modname]
        self.lib = self
        self.header_version = self.scheme["__header_version"]
        self.header = self.scheme["header"]
        self.header_pack_instr = get_pack_instr(self.header, self.scheme, False)
        self.header_size = struct.calcsize(str(self.header_pack_instr))
        self.gen_packs_instrcs()
        uuid_br = eval_codec(str(self.header["client_generated_uuid"])).bin_repr()
        self.uuid_size = struct.calcsize(uuid_br)
        self.create_codecs()
        self.version_data = {"version":"", "commit":"", "branch":""}
        fn = "/opt/nvmesh/client-repo/version"
        if os.path.exists(fn):
            with open(fn, "r") as f:
                for line in f.readlines():
                    [key, val] = line.split("=")
                    val = val.replace("\"", "")
                    self.version_data[key] = val[:-1]

    def set_lib(self, lib: str) -> None:
        self.lib = importlib.import_module(lib)

    def pack_msg_no_payload(self, msg_scheme: Json, msg_dat: Json, res: bytes) -> bytes:
        """
        ###pack message.
        the message does not traverse to the inner dynamic arrays
        """
        __OBJ_TPYE = type(__E__)
        # go over all vars and skip arrays
        for (var, frmt) in zip(msg_scheme.keys(), msg_scheme.values()):
            if is_reserved_var_name(var):
                continue

            self.traceback.append({"var": var})
            if var not in msg_dat:
                if isinstance(frmt, CodecHolder):
                    try:
                        val = frmt.default_val()
                    except Exception as e:
                        raise RuntimeError(f"cannot supply default value to member {var} with the codec {frmt}. Exception: {str(e)}")
                elif is_array(frmt):  # if we got missing array, try to handle it as an empty array
                    val = []
                else:
                    val = {}
            else:
                val = msg_dat[var]

            if isinstance(val, list):
                res += struct.pack("=iii", len(val), 0, 0)
                self.traceback.pop()
                continue
            if isinstance(frmt, CodecHolder):
                try:
                    res += frmt.encode(val)
                except RuntimeError as e:
                    raise RuntimeError(f"tried to pack {var} and with value{val} and got error: {str(e)}")
                self.traceback.pop()
                continue
            if type(val) == __OBJ_TPYE or isinstance(val, dict):
                try:
                    self.MsgStack.append(val)
                    if isinstance(frmt, dict):
                        res = self.pack_msg_no_payload(frmt, val, res)
                    else:
                        res = self.pack_msg_no_payload(self.scheme[frmt], val, res)
                    self.MsgStack.pop()
                    self.traceback.pop()
                    continue
                except RuntimeError as e:
                    raise RuntimeError(str(e) + "-> tried to pack {0}:\n{1}".format(var, str(e)))

            elif isinstance(val, str):
                val = val.encode("utf-8")
            if is_def_type(self.scheme, frmt):
                frmt = get_def_type_instr(self.scheme[frmt])
            flaten = tuple(flat_list(['=' + str(frmt), val]))
            try:
                res += struct.pack(*flaten)
            except Exception as e:
                raise RuntimeError("(var {4}) error packing {0} type {3} with {1}:{2}\n".format(val, frmt, e, type(val), var))
            self.traceback.pop()
        return res

    def fix_offsets(self, msg_scheme: Json, msg_dat, res, base_offs, array_size, fix_from, msg_stack):
        """
        fix offsets
        """
        __OBJ_TYPE = type(__E__)
        # go over all vars and skip arrays
        itr = fix_from
        payload = b""
        next_messages_to_fix = []
        # self.MsgStack.append(msg_dat)
        self.MsgStack = msg_stack

        # unless the message is dynamic array_size will be 1
        for ir0 in range(array_size):
            for (var, frmt) in zip(msg_scheme.keys(), msg_scheme.values()):
                val: Any
                if is_reserved_var_name(var):
                    continue
                self.traceback.append(var)
                if var not in msg_dat:
                    if isinstance(frmt, CodecHolder):
                        try:
                            val = ""
                            # self.MsgStack.append(val)
                            # val = frmt.default_val()
                            # self.MsgStack.pop()
                        except Exception as e:
                            raise RuntimeError(f"cannot supply default value to member {var} with the codec {frmt}. Exception: {str(e)}")
                    elif isinstance(frmt, dict) and "__type" in frmt and frmt["__type"] == "array":
                        val = []
                    else:
                        val = {}
                else:
                    val = msg_dat[var]
                if isinstance(val, list):
                    itr += 4
                    res = res[0:itr] + struct.pack("=Q", base_offs) + res[itr + 8:]
                    itr += 8
                    indx = 0
                    for obj in val:
                        next_messages_to_fix.append((base_offs, frmt, obj, copy.deepcopy(self.traceback), self.MsgStack + [obj]))
                        old_len = len(payload)
                        try:
                            self.traceback.append("[{0}]".format(indx))
                            self.MsgStack.append(obj)
                            payload = self.pack_msg_no_payload(self.scheme[frmt["__typename"]], obj, payload)
                            self.MsgStack.pop()
                            self.traceback.pop()
                        except RuntimeError as e:
                            raise RuntimeError(str(e) + "-> tried to pack {0}".format(var))
                        base_offs += (len(payload) - old_len)
                        indx += 1
                    self.traceback.pop()
                    continue
                elif type(val) == __OBJ_TYPE or isinstance(val, dict):
                    if isinstance(frmt, dict):
                        res, pp0, msg_to_fix, itr = self.fix_offsets(frmt, val, res, base_offs, 1, itr, self.MsgStack + [val])
                    else:
                        res, pp0, msg_to_fix, itr = self.fix_offsets(self.scheme[frmt], val, res, base_offs, 1, itr, self.MsgStack + [val])
                    next_messages_to_fix += msg_to_fix
                    payload += pp0
                    base_offs += len(pp0)
                    self.traceback.pop()
                    continue
                elif isinstance(frmt, CodecHolder):
                    cmprs = frmt.bin_repr()
                    itr += struct.calcsize(cmprs)
                    self.traceback.pop()
                    continue
                elif isinstance(val, str):
                    val = str(val)
                if is_def_type(self.scheme, frmt):
                    frmt = get_def_type_instr(self.scheme[frmt])
                cmprs = ('=' + str(frmt))
                itr += struct.calcsize(cmprs)
                self.traceback.pop()
        self.MsgStack.pop()
        return res, payload, next_messages_to_fix, itr

    def create_item_codec(self, element):
        """replace codecs instruction with objects in a single scheme item"""
        for var, frmt in element.items():
            if is_reserved_var_name(var):
                continue
            if isinstance(frmt, dict):
                self.create_item_codec(frmt)
                continue
            if frmt.startswith("__codec"):
                codec = eval_codec(frmt)
                element[var] = CodecHolder(codec)

    def create_codecs(self):
        """replace all codec instruction strings with codecs objects"""
        for (var, frmt) in self.scheme.items():
            if var == "header":
                continue
            if not isinstance(frmt, dict):
                continue
            self.create_item_codec(frmt)

    def gen_packs_instrcs(self) -> None:
        """
        create instruction for messages in the scheme
        """
        self.pack_instr = {}
        self.opcode_to_frmt: OpcodeInst = {}
        for (var, frmt) in zip(self.scheme.keys(), self.scheme.values()):
            if var == "header":
                continue
            if not isinstance(frmt, dict):
                continue
            if frmt.get("__type") != "msg":
                continue
            is_upstream = frmt["__upstream"] if "__upstream" in frmt else True
            is_downstream = frmt["__downstream"] if "__downstream" in frmt else True
            opcode = frmt["__opcode"]
            strvar = str(var)
            if opcode not in self.opcode_to_frmt:
                self.opcode_to_frmt[opcode] = ["", ""]
            if is_upstream:
                self.opcode_to_frmt[opcode][0] = strvar
            if is_downstream:
                self.opcode_to_frmt[opcode][1] = strvar
            instr = get_pack_instr(frmt, self.scheme, True)
            self.pack_instr[var] = Msg_Info(instr)

    def get_msg_pack_inst(self, opcode, is_downstream):
        """
        get pack instruction of a given opcode
        """
        if opcode in self.opcode_to_frmt:
            if is_downstream:
                msg_type = self.opcode_to_frmt[opcode][1]
            else:
                msg_type = self.opcode_to_frmt[opcode][0]
        if msg_type in self.pack_instr:
            return (self.scheme[msg_type], self.pack_instr[msg_type], msg_type)
        raise RuntimeError("cannot find opcode {0} in scheme".format(opcode))

    def doJoin(self, msg_scheme, mp):
        """
        join message
        """
        res = ["{"]
        is_empty = True
        for (var, frmt) in msg_scheme.items():
            if is_reserved_var_name(var):
                continue
            is_codec = isinstance(frmt, CodecHolder)
            if not is_codec and is_array(frmt):
                data = mp[var]
                if not len(data):
                    continue
                if not is_empty:
                    res.append(',')
                else:
                    is_empty = False
                res.append(data)
                continue
            if not is_empty:
                res.append(',')
            else:
                is_empty = False
            res.append('"{0}": "{1}"'.format(var, str(mp[var])))
        res.append("}")
        return eval(''.join(res))

    def calcHash(self, elements: List[Dict[str, str]]) -> str:
        if not elements:
            return ""

        string_ar = []

        md5 = hashlib.md5()
        for elem in elements:
            string_ar.append(elem["ID"].encode("utf-8") + b":" + str(elem["version"]).encode("utf-8"))
        str_for_hash = b";".join(sorted(string_ar))
        md5.update(str_for_hash)
        ret = md5.hexdigest()
        return ret

    def hashSimpleStringArray(self, elements: List[Dict[str, str]]) -> str:
        if not elements:
            return ""

        string_ar: List[bytes] = []
        md5 = hashlib.md5()
        for elem in elements:
            string_ar.append(elem["val"].encode("utf-8"))
        str_for_hash = b";".join(sorted(string_ar))
        md5.update(str_for_hash)
        ret = md5.hexdigest()
        return ret

    def unpack_msg_private(self, msg_scheme, data, payload):
        """
        helper recursive message to uppack
        """
        res = {}
        i = 0
        is_def = False
        needJoin = concat(msg_scheme)
        for (var, frmt) in msg_scheme.items():
            if is_reserved_var_name(var):
                continue
            self.traceback.append(var)
            is_codec = isinstance(frmt, CodecHolder)
            if not is_codec and is_array(frmt):
                bindat = data[i: i + self.size_arr_desc]
                (num_elements, offset) = struct.unpack("=iQ", bindat)
                res[var] = []
                var_data_indx = 0
                if not needJoin:
                    for _ in range(num_elements):
                        (item, var_data_indx) =\
                                self.unpack_msg_private(self.scheme[frmt["__typename"]], payload[offset:], payload)
                        offset += var_data_indx
                        res[var].append(item)
                else:
                    res[var] = payload[offset: offset + num_elements]
                    offset += num_elements
                i += self.size_arr_desc
                if frmt.get("__codec") == "stringsArrayToHash":
                    res[var] = self.calcHash(res[var])
                elif frmt.get("__codec") == "SimpleStringArrayToHash":
                    res[var] = self.hashSimpleStringArray(res[var])
                self.traceback.pop()
                continue
            is_def = is_def_type(self.scheme, frmt)
            if not is_codec and ( isinstance(frmt, str) or isinstance(frmt, str)) and frmt in self.scheme and not is_def:
                (res[var], ii) = self.unpack_msg_private(self.scheme[frmt], data[i:], payload)
                i += ii
                self.traceback.pop()
                continue
            elif is_def:
                frmt = get_def_type_instr(self.scheme[frmt])
            if not is_codec and isinstance(frmt, dict):
                (res[var], i) = self.unpack_msg_private(frmt, data[i:], payload)
                self.traceback.pop()
                continue
            si0 = 0
            if is_codec:
                codec = frmt
                si0 = struct.calcsize(codec.bin_repr())
                tt0 = codec.decode(data[i:i+si0])
                res[var] = tt0
                i += si0
                self.traceback.pop()
                continue
            else:
                si0 = struct.calcsize(str(frmt))
                try:
                    tt0 = struct.unpack(frmt, data[i:si0 + i])
                except struct.error as ee0:
                    raise
            msg = None
            # handle null terminated string
            if frmt[-1] == 's' and (len(frmt) == 1 or frmt[:-1].isdigit()):
                nn0 = tt0[0].find(b'\x00')
                if nn0 < 0:
                    msg = tt0[0]
                if nn0 >= 0:
                    msg = tt0[0][:nn0]
                res[var] = msg.decode("utf-8")
            else:
                res[var] = tt0[0]
            i += si0
            self.traceback.pop()
        if needJoin:
            res = self.doJoin(msg_scheme, res)
        return (res, i)

    def pack2(self, opcode: int, msg_json: Json) -> List[bytes]:
        """
        pack json message with scheme defined by the given opcode
        """
        if opcode in self.opcode_to_frmt:
            msg_type = self.opcode_to_frmt[opcode][1]
        else:
            return []
        msg_json_copy = copy.deepcopy(msg_json)
        data = self.pack(msg_type, msg_json_copy)
        # print ("packed data=", ":".join("{:02x}".format(ord(c)) for c in data))
        msg_scheme = self.scheme[msg_type]
        if "__filter" in msg_scheme and msg_scheme["__filter"]:
            self.logger.debug("pack message filtered")
            data = []
        self.logger.debug("pack2 returning %d messages opcode=%d", len(data), opcode)
        return data

    def get_traceback(self):
        """
        get the traceback as a nice formated string
        """
        return "->".join(str(self.traceback))

    def push_status(self) -> None:
        self.StatusStack.append((self.traceback, self.MsgStack))

    def pop_status(self):
        self.traceback, self.MsgStack = self.StatusStack[-1]
        self.StatusStack.pop()

    def packBody(self, msg_type, msg_dat):
        """
        pack a message by using the key msg_type
        """
        self.traceback += [msg_type]
        self.payload = ""
        if msg_type in self.scheme:
            msg_scheme = self.scheme[msg_type]
        else:
            return ""
        if msg_dat == None:
            msg_dat = {}
        msg_packed = b""
        self.MsgStack = [msg_dat]
        stack = self.MsgStack
        msg_packed = self.pack_msg_no_payload(msg_scheme, msg_dat, msg_packed)
        msg_packed, payload, next_messages_to_fix, _ = self.fix_offsets(
            msg_scheme, msg_dat, msg_packed, 0, 1, 0, stack)
        while len(next_messages_to_fix) > 0:
            msg_to_process = []
            for msg_info in next_messages_to_fix:
                msg_offs_in_payload, frmt, obj, self.traceback, stack = msg_info
                base_offs = len(payload)
                payload, next_payload, more_msgs, itr = self.fix_offsets(
                    self.scheme[frmt["__typename"]], obj, payload, base_offs,
                    1, msg_offs_in_payload, stack)
                msg_to_process += more_msgs
                payload += next_payload
            next_messages_to_fix = msg_to_process
        len_msg = len(msg_packed)
        msg_len = self.header_size + len_msg + len(payload)
        var_offset = self.header_size + len_msg
        self.traceback.pop()
        return msg_packed + payload

    def prepack(self, msg_type: str, msg_scheme: Json, msg_dat: Json) -> List[Json]:
        ret: List[Json] = []
        prepack = msg_scheme.get("__prepack")
        if prepack:
            self.logger.debug('running prepack %s msg_type=%s on message %s', prepack, msg_type, msg_dat)
            f = getattr(self.lib, prepack)
            # a prepack func receives a list of messages and return list of messages.
            # usually, the input will be list of one message and the prepack may choose to return multiple messages
            ret = f(msg_type, msg_dat, self.logger)
            if not isinstance(ret, list):
                ret = [ret]
        else:
            ret = [msg_dat]

        for msg in ret:
            self.logger.debug(f"packer: message for packing: {msg}")

        return ret

    def pack(self, msg_type: str, msg_dat: Optional[Json]) -> List[bytes]:
        """
        pack a message by using the key msg_type
        """
        mdt: Json
        global PACKER
        prev = PACKER
        PACKER = self
        self.payload = ""
        self.logger.debug(f"pack called {msg_dat}")
        if msg_type in self.scheme:
            msg_scheme: Json = self.scheme[msg_type]
        else:
            PACKER = prev
            self.logger.debug("pack return nothing")
            return []

        if msg_dat is None:
            msg_dat = {}

        opcode = msg_scheme["__opcode"]
        self.traceback = [{"type": msg_type, "opcode": opcode}]
        msg_packed: List[bytes] = []
        messages: List[Json] = self.prepack(msg_type=msg_type,
                                            msg_scheme=msg_scheme,
                                            msg_dat=msg_dat)
        for mdt in messages:
            this_packed = b""
            self.MsgStack = [mdt]
            stack = self.MsgStack
            this_packed = self.pack_msg_no_payload(msg_scheme, mdt, this_packed)

            this_packed, payload, next_messages_to_fix, _ = self.fix_offsets(
                msg_scheme, mdt, this_packed, 0, 1, 0, stack)

            while len(next_messages_to_fix) > 0:
                msg_to_process = []
                for msg_info in next_messages_to_fix:
                    msg_offs_in_payload, frmt, obj, self.traceback, stack = msg_info
                    base_offs = len(payload)
                    payload, next_payload, more_msgs, _ = self.fix_offsets(
                        self.scheme[frmt["__typename"]], obj, payload,
                        base_offs, 1, msg_offs_in_payload, stack)
                    msg_to_process += more_msgs
                    payload += next_payload
                next_messages_to_fix = msg_to_process
            len_msg = len(this_packed)
            msg_len = self.header_size + len_msg + len(payload)
            var_offset = self.header_size + len_msg
            tt0 = timestamp()
            self.traceback = [{"type": "HEADER"}]
            zero_uuid = b'\x00' * self.uuid_size
            rv0 = struct.pack(self.header_pack_instr, msg_len, var_offset,
                              self.header_version, self.scheme_version, opcode,
                              tt0, zero_uuid, zero_uuid)
            msg_packed.append(rv0 + this_packed + payload)

        self.traceback.pop()
        PACKER = prev
        return msg_packed

    def unpack_header(self, data: bytes) -> Tuple[LenType, OpcodeType, UuidType, TimeStamp]:
        """
        unpack message header
        """
        self.traceback.append({"type": "HEADER"})
        token: bytes
        rr0 = struct.unpack(self.header_pack_instr, data[0:self.header_size])
        (total_len, var_offset, header_version, scheme_version, opcode, ts, token) = rr0[:7]
        token_str = str(uuid.UUID(bytes=token))

        if header_version != self.header_version:
            raise RuntimeError(
                f"header version from other side ({header_version}) does not match the local header version ({self.header_version})")
        if scheme_version != self.scheme_version:
            raise RuntimeError(f"scheme version from other side ({scheme_version})" +
                               f" does not match the local expected scheme version ({self.scheme_version})")
        self.var_offset = var_offset
        self.bin_msg = data
        msg_len = total_len - self.header_size
        self.traceback.pop()
        return (msg_len, opcode, token_str, ts)

    def unpack_msg(self, opcode: int, data: bytes) -> Tuple[Json, Json]:
        """
        unpack message
        """
        (msg_scheme, msg_instr_obj, msg_type) = self.get_msg_pack_inst(opcode, False)
        (msg_instr, msg_h_size) = (msg_instr_obj.pack_instr, msg_instr_obj.length)


        # TODO remove the unecessar data copy in the following line
        (msg_res, _) = self.unpack_msg_private(msg_scheme, data[:msg_h_size], data[msg_h_size:])

        postunpack: str = msg_scheme.get("__postunpack")
        if postunpack:
            self.logger.debug('running postunpack %s opcode=%d', postunpack, opcode)
            f = getattr(self.lib, postunpack)
            msg_res = f(msg_type, msg_res, self.logger)

        return msg_res, msg_scheme

    def fill_special(self, msg, val):
        """
        replaces values in specil field from data fetched from message.
        """
        # the following line will search for all {..} and replace it with values from msg
        # after each time a value is found, I delete the key, value from the msg
        return REPLACER_RE.sub(lambda x: str(removekey(msg, x.group(1))), val)

    def unpack(self, data: bytes) -> Json:
        """
        unpack whole message
        """
        global PACKER
        prev = PACKER
        PACKER = self
        self.traceback = []
        (msg_len, opcode, token, _) = self.unpack_header(data)
        # msg_len includes the message and the payload
        msg, msg_scheme = self.unpack_msg(opcode, data[-msg_len:])
        self.traceback.append({"opcode": opcode})
        route_msg = self.fill_special(msg, msg_scheme["__route"])

        assert "__messageType" in msg_scheme, f"msg_scheme didn't contain __messageType. msg_scheme: {msg_scheme}"

        upstream_header = msg.pop("upstream_header", None)
        assert upstream_header is not None, "upstream_header was not part of the upstream message"
        assert upstream_header.get('messageTypeVersion'), "upstream_header didn't have the messageTypeVersion key"
        assert upstream_header.get('messageSequence'), "upstream_header didn't have the messageSequence key"
        assert upstream_header.get('clientToken'), "upstream_header didn't have the clientToken key"
        assert upstream_header.get('clientID'), "upstream_header didn't have the clientID key"

        res = {"route": route_msg,
               "messageType": msg_scheme["__messageType"],
               "resendOnFailover": msg_scheme["__resendOnFailover"],
               "dropOnDisconnection": msg_scheme["__dropOnDisconnection"],
               "priority": msg_scheme["__priority"],
               "opcode": opcode,
               "originType": self.scheme["__globals"]["__origin_type"],
               "enableCache": msg_scheme.get("__enableCache", False),
               "isCacheAllowed": msg_scheme.get("__enableCache", False),
               "messageToken": token,
               "payload": msg
               }

        res.update(upstream_header)
        self.logger.debug(f"unpacking result: {json.dumps(res)}")

        self.traceback.pop()
        PACKER = prev
        return res


def main():
    """
    main function that create stub file
    """
    if len(sys.argv) <= 1:
        raise RuntimeError("usage: {} <scheme> <header.h>".format(sys.argv[0]))
    outfn = sys.argv[2]
    do_create_consts = True
    # header optiong generate header h file common to all shcemes
    if sys.argv[1] == "--header":
        scheme_dat = load_header()
        do_create_consts = False
    else:
        # this option will generate scheme related h file
        file_name = sys.argv[1]
        path = None
        for path_ in ["management_cm", "../management_cm"]:
            if os.path.exists(path_):
                path = path_
                break;
        if path is None:
            raise RuntimeError("cannot find scheme")
        packer = Packer(file_name, path)
        scheme_dat = packer.scheme
        data = packer.data
        header = packer.loaded_header
        calc_check_sum(scheme_dat, header, data)
    create_h_file(scheme_dat, outfn, do_create_consts)

if __name__ == "__main__":
    main()
