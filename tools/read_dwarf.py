#!/usr/bin/env python3

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

import sys
import json
import ctypes
import inspect

from subprocess import Popen, PIPE
from re import match

class Bunch:
    def __init__(self, **kwds):
        self.as_dict = dict()
        self.as_dict.update(kwds)
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


def read_elf(elf):
    p = Popen(["readelf", "-wi", elf], stdin=PIPE, stdout=PIPE, stderr=PIPE, universal_newlines=True)
    out, err = p.communicate("input data that is passed to subprocess' stdin")
    rc = p.returncode
    if rc:
        raise Exception(
            "Reading {0} returned error {1}, stdout={2} stderr={3}".format(
                elf, rc, out, err
            )
        )
    return out.split("\n")


def parse_header(line):
    if line.endswith(')'):
        m = match(r" <(.*)><(.*)>: Abbrev Number: .* \((.*)\)", line)
        if m:
            return {
                "indent": m.group(1),
                "id": "0x" + m.group(2),
                "type": m.group(3),
                "name": "tp_" + m.group(2),
                "children": [],
                "arr_len": 0
            }
    elif line.endswith('0'):
        m = match(r" <(.*)><(.*)>: Abbrev Number: 0$", line)
        if m:
            return {
                "indent": m.group(1),
                "id": "0x" + m.group(2),
                "type": "CloseBlock",
                "name": None,
                "children": [],
                "arr_len": 0
            }


def parse_name(line):
    # regex is r".* DW_AT_name .*: (.+)$"
    if 'DW_AT_name' in line:
        return line.split(':')[-1].strip()


def parse_datatype(line):
    # r".* DW_AT_type .*: <(.+)>$", line)
    if "DW_AT_type" in line:
        return line.split(":")[-1].strip()[1:-1]


def parse_arr_len(line):
    # r".* DW_AT_upper_bound .*: (\d+)$"
    if "DW_AT_upper_bound" in line:
        try:
            return int(line.split(":")[-1].strip())
        except:
            return None


def parse_const_val(line):
    # r".* DW_AT_const_value .*: (\d+)$"
    if "DW_AT_const_value" in line:
        try:
            return int(line.split(":")[-1].strip())
        except:
            return None


def parse_bit_size(line):
    # r".* DW_AT_bit_size .*: (\d+)$"
    if "DW_AT_bit_size" in line:
        try:
            return int(line.split(":")[-1].strip())
        except:
            return None


def parse_byte_size(line):
    # r".* DW_AT_byte_size .*: (\d+)$"
    if "DW_AT_byte_size" in line:
        try:
            return int(line.split(":")[-1].strip())
        except:
            return None


TYPE_CHILDREN = {
    "DW_TAG_subprogram": {"DW_TAG_formal_parameter"},
    "DW_TAG_enumeration_type": {"DW_TAG_enumerator"},
    "DW_TAG_structure_type": {"DW_TAG_member"},
    "DW_TAG_union_type": {"DW_TAG_member"},
    "DW_TAG_array_type": {"DW_TAG_subrange_type"},
}


def parse_read_elf(lines):
    out = []
    parent = None
    dest = out
    curr = None
    expect = {}
    for line in lines:
        if len(line) > 1 and line[1] == '<':
            curr_ = parse_header(line)
            if curr_:
                curr = curr_
                if curr["type"] not in expect:
                    if curr["type"] in TYPE_CHILDREN:
                        out.append(curr)
                        expect = TYPE_CHILDREN[curr["type"]]
                        parent = curr
                        dest = curr["children"]
                    else:
                        expect = {}
                        dest = out
                        parent = None
                        dest.append(curr)
                else:
                    dest.append(curr)
                continue

        match = parse_name(line)
        if match:
            curr["name"] = match
            continue

        match = parse_datatype(line)
        if match:
            curr["datatype"] = match
            continue

        match = parse_arr_len(line)
        if match:
            parent["arr_len"] = curr["arr_len"] = match
            continue

        match = parse_const_val(line)
        if match:
            curr["const_val"] = match
            continue

        match = parse_bit_size(line)
        if match:
            curr["bit_size"] = match
            continue

        match = parse_byte_size(line)
        if match:
            curr["byte_size"] = match
            continue

    return out


def indent_string(s, indent="  "):
    return "\n".join([indent + line for line in s.split("\n")])


class DllDataType(object):
    def __init__(self, name="", ctype=None):
        self._name = None
        self.help = None
        self.name = name
        self.ctype = ctype
        self.is_comlex = True
        self.values = {}

    def clone(self, other):
        self._name = other._name
        self.help = other.help
        self.ctype = other.ctype

    @property
    def name(self):
        return self._name

    @name.setter
    def name(self, val):
        self.help = val
        self._name = val.replace(" ", "__") if val else ""

    def __str__(self):
        return self.name


def resolve_type(id, resolved, parsed):
    """ Given Id from DWARF header + previously parsed data
        resolve etry type.
    """
    dt = resolved.get(id)
    if dt:
        return dt
    entry = parsed.get(id, None)
    if not entry:
        # Consider any unresolved type as int. Because.
        dt = DllDataType(name=id, ctype=ctypes.c_int)
        resolved[id] = dt
        return dt

    dt = DllDataType(name=entry.get("name"))

    if entry.get("type") == "DW_TAG_base_type":
        dt.is_comlex = False
        if "long long" in entry.get("name"):
            dt.ctype = ctypes.c_ulonglong if "unsigned" in dt.name else ctypes.c_longlong
        elif "long" in entry.get("name"):
            dt.ctype = ctypes.c_ulong if "unsigned" in dt.name else ctypes.c_long
        elif "short" in entry.get("name"):
            dt.ctype = ctypes.c_ushort if "unsigned" in dt.name else ctypes.c_short
        elif "char" in entry.get("name"):
            dt.ctype = ctypes.c_ubyte if "unsigned" in dt.name else ctypes.c_char
        elif "int" in entry.get("name"):
            dt.ctype = ctypes.c_uint if "unsigned" in dt.name else ctypes.c_int
        elif "_Bool" in entry.get("name"):
            dt.ctype = ctypes.c_bool
        elif entry.get("name") == "sizetype":
            dt.ctype = ctypes.c_ulong
            dt.name = "size_t"
            dt.help = "unsigned long"
        else:
            raise Exception(
                "Unknown base type {0}, entry {1}".format(
                    entry.get("name"), json.dumps(entry)
                )
            )

    if entry.get("type") == "DW_TAG_enumeration_type":
        dt.name = "enum {0}".format(dt.name)
        dt.ctype = type(dt.name, (ctypes.c_int,), {})
        dt.values = Bunch(**{child["name"]: child["const_val"]
                             for child in entry["children"] if child.get("const_val")})

    if (entry.get("type") == "DW_TAG_typedef"):
        if not entry.get("datatype"): # VOID typedef
            dt.ctype = ctypes.c_int
        else:
            resolved_entry = resolve_type(
                entry["datatype"], resolved, parsed)
            real_ctype = resolved_entry.ctype
            if not hasattr(real_ctype, "_length_"):  # If we can subclass typedef do it
                dt.ctype = type(dt.name, (real_ctype,), {"real_ctype": real_ctype})
                setattr(dt.ctype, '__str__',
                        lambda self: str(self.value))
            else:  # For arrays this won't work, typedef = original type
                resolved[entry["id"]] = resolved_entry
                return resolved_entry

    if (entry.get("type") == "DW_TAG_const_type"):
        if not entry.get("datatype"): # VOID typedef
            dt.ctype = ctypes.c_int
        else:
            # For const types - cut it here, simply return the type it points to and update resolved entry.
            dt = resolve_type(entry["datatype"], resolved, parsed)
            resolved[entry["id"]] = dt
            return dt

    if entry.get("type") == "DW_TAG_pointer_type":
        if entry.get("datatype"):
            dt.clone(resolve_type(entry["datatype"], resolved, parsed))
            if dt.ctype == ctypes.c_byte or dt.ctype == ctypes.c_ubyte:
                (dt.ctype, dt.name) = (ctypes.c_char_p, "string")
            else:
                (dt.ctype, dt.name, dt.help) = (ctypes.POINTER(
                    dt.ctype), dt.name+"__ptr", dt.help+"*")
        else:
            (dt.ctype, dt.name, dt.help) = (
                ctypes.c_void_p, "void__ptr", "void*")

    if entry.get("type") == "DW_TAG_array_type":
        arr_len = entry.get("arr_len")
        dt.clone(resolve_type(entry["datatype"], resolved, parsed))
        dt.ctype = dt.ctype * entry.get("arr_len")
        _help = dt.help
        dt.name = dt.name + "_array_{0}".format(entry.get("arr_len"))
        dt.help = _help + "[{0}]".format(entry.get("arr_len"))

    if (entry.get("type") == "DW_TAG_structure_type" or entry.get("type") == "DW_TAG_union_type"):

        # Must first create struct as a prototype to avoid infinite recursion
        dt.name = ('union ' if entry.get("type") ==
                   "DW_TAG_union_type" else 'struct ') + dt.name
        dt.ctype = type(
            dt.name,
            (
                ctypes.Union
                if entry.get("type") == "DW_TAG_union_type"
                else ctypes.Structure,
            ), {"_pack_": 0}
        )
        resolved[entry["id"]] = dt

        # Only now may we check the children
        unnamed = []
        for c in entry["children"]:
            c["real_ctype"] = resolve_type(c["datatype"], resolved, parsed).ctype
            while hasattr(c["real_ctype"], "real_ctype"): # Eliminate typedefs
                c["real_ctype"] = getattr(c["real_ctype"], "real_ctype", c["real_ctype"])
            if c["name"].startswith("tp_"):
                normalize_name = "unnamed_" + str(len(unnamed) + 1)
                c["name"] = normalize_name
                unnamed.append(normalize_name)
        fields = [(member["name"], member["real_ctype"], member["bit_size"]) if member.get('bit_size') else (member["name"], resolve_type(member["datatype"], resolved, parsed).ctype)
                  for member in entry["children"]]

        member_helps = [
            "{0} {1};".format(
                resolve_type(member["datatype"], resolved,
                             parsed).help, member["name"]
            )
            for member in entry["children"]
        ]

        if unnamed:
            dt.ctype._anonymous_ = unnamed
        dt.ctype._fields_ = fields

        # This fixes structure packing - we can know struct ispacked if its size
        # is not as in the dwarf header
        if entry.get("byte_size") and ctypes.sizeof(dt.ctype) != entry.get("byte_size"):
            dt.ctype = type(
                dt.name,
                (
                    ctypes.Union
                    if entry.get("type") == "DW_TAG_union_type"
                    else ctypes.Structure,
                ), {"_pack_": 1}
            )
            if unnamed:
                dt.ctype._anonymous_ = unnamed
            dt.ctype._fields_ = fields
            resolved[entry["id"]] = dt

        setattr(dt.ctype, '__str__', lambda self: dt.name + '{\n' + indent_string('\n'.join(
            '{0}={1}'.format(member['name'], str(getattr(self, member['name']))) for member in entry["children"])) + '\n}')

        dt.help = (
            ("union{\n" if entry.get("type") ==
             "DW_TAG_union_type" else "struct{\n")
            + indent_string("\n".join(member_helps))
            + "\n}"
        )

    # Whatever is left considered VOID*
    if not dt.ctype:
        (dt.ctype, dt.name, dt.help) = (
                ctypes.c_void_p, "void__ptr", "void*")

    if (dt.is_comlex):
        setattr(dt.ctype, "__doc__", dt.help)
    resolved[entry["id"]] = dt
    return dt

def convert_parsed_to_dict(parsed):
    return { e['id'] : e for e in parsed  }

def resolve_all_types(parsed, fltr=None):
    resolved = {}
    parsed = convert_parsed_to_dict(parsed)
    for entry in parsed.values():
        if fltr and (not entry.get('name') or not match(fltr, entry.get('name'))):
            continue
        resolve_type(entry["id"], resolved, parsed)
    return resolved


def resolve_all_arglists(parsed, resolved_types):
    resolved = {}
    for entry in parsed:
        if entry.get("type") == "DW_TAG_subprogram":
            resolved[entry["name"]] = {
                "arglist": [(resolved_types[arg["datatype"]], arg.get("name", "")) for arg in entry["children"]],
                "restype": resolved_types.get(entry.get("datatype"))
            }
    return resolved

def resolve_dwarf_only(so, fltr=None):
    # Read elf dbug symbols elf
    lines = read_elf(so)

    # Transfor lines into dictionary
    parsed = parse_read_elf(lines)

    # Extract types from dictionary
    resolved = resolve_all_types(parsed, fltr)

    # Turn types into more accessible bunch
    types = Bunch(**{t.name: t.ctype for t in resolved.values()})

    # Turn enums into a bunch
    enums = Bunch()
    for dt in resolved.values():
        if dt.values:
            enums._bunch_update(**{dt.name[6:]: dt.values})

    return types, enums, parsed, resolved


def resolve(so):

    types, enums, parsed, resolved = resolve_dwarf_only(so)

    # Extract functions from dictionary
    arglists = resolve_all_arglists(parsed, resolved)

    # First, load DLL
    dll = ctypes.CDLL(so)
    if not dll:
        raise Exception("Error reading dyhnamic library {0}".format(so))

    # Turn functions into more accessible bunch
    funcs = Bunch()
    for func, args in arglists.items():
        try:
            dll_func = getattr(dll, func)
            dll_func.restype = args["restype"].ctype if args["restype"] else None
            dll_func.argtypes = [t[0].ctype for t in args["arglist"]]

            # Info for help and decorations
            dll_func.argtypenames = [t[0].name for t in args["arglist"]]
            dll_func.restypename = args["restype"].name if args["restype"] else "void"
            dll_func.argnames = [t[1] for t in args["arglist"]]

            dll_func.__doc__ = dll_func.restypename + " " + func + \
                "(" + ", ".join([t[0].name + " " + t[1]
                                 for t in args["arglist"]]) + ")"
            funcs._bunch_update(**{func: dll_func})
        except AttributeError as e:
            # To avoid spamming with error messages, assume static functions end with _ or begin with __
            if not func.endswith('_') and not func.startswith('__'):
                print >> sys.stderr, "Error importing {0}. Static? Probably can ignore this message. {1}".format(
                    func, str(e))

    return dll, types, funcs, enums


def main():
    import IPython
    import cProfile
    # Main for tests only
    NEEDED_C_STRUCTS = ["nvmeib_get_disk_names_reply", "nlmsghdr", "nvmeib_nl_uk_comm_msg",
                    "nvmeib_nl_uk_comm_msg", "nvmeib_nl_uk_comm_msg", "nvmeib_nl_uk_comm_rep", "nvmeib_io_to_disk",
                    "nvmeib_io_to_disk_reply"]
    NEEDED_C_ENUMS = ["uk_comm_opcode"]
    NEEDED_C_STRUCTURES = NEEDED_C_STRUCTS + NEEDED_C_ENUMS
    to_resolve =  "|".join(NEEDED_C_STRUCTURES)
    cProfile.run(f"types, enums, parsed, resolved = resolve_dwarf_only(\"/opt/nvmesh/common-repo/tools/infra_shared.so\", '{to_resolve}')")
    IPython.embed()
    return 0


if __name__ == "__main__":
    sys.exit(main())
