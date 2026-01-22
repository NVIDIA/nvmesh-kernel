#!/usr/bin/env python3

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

from ctypes import CDLL
from re import compile
from subprocess import Popen, PIPE

def _hex_int(x):
    return int(x, 16)


def _auto_int(x):
    """ Parse integer from string, whether it is 0xval format or val (decimal).
    """
    if x.startswith("0x"):
        return int(x[2:], 16)
    return int(x)

# TODO: Add a common base class for this and other implementations


class DumbTextParserDwarf2PyEngine(object):
    def __init__(self, file_name):
        # type: (DumbTextParserDwarf2PyEngine, str) -> None
        """ Initialize parser on a given ELF file
        """
        super(DumbTextParserDwarf2PyEngine, self).__init__()
        self.cache = dict()
        self.file_name = file_name
        self._pre_read_elf_file(file_name)

    def get_dll(self):
        return CDLL(self.file_name)

    # Ex: <1><52e2>: Abbrev Number: 12 (DW_TAG_subroutine_type)
    TAG_RGX = compile(r"^ <[0-9]+><(?P<tid>[\da-f]+)>[^(]*\((?P<tag>[^)]+)\)$")
    # Ex: <16e80a>   DW_AT_decl_file   : 17
    ATTR_RGX = compile(
        r"^[^>]*>\s*(?P<attr>\w+).*:\s(?:<)?(?P<val>0?x?[^<>\n]*)>?$")

    @staticmethod
    def _read_elf_lines(file_name):
        cmd = ["readelf", "-wi", file_name]
        p = Popen(cmd, stdout=PIPE)
        lines = p.stdout.readlines()
        p.stdout.close()
        p.wait()
        return lines

    SIBLING_TAGS = set(
        ["DW_TAG_member",
         "DW_TAG_formal_parameter",
         "DW_TAG_subrange_type",
         "DW_TAG_enumerator"])

    def _pre_read_elf_file(self, file_name):
        lines = None
        siblings = None
        tags = dict()
        for line in self._read_elf_lines(file_name):
            if line and len(line) > 1 and line[1] == "<":
                if line.endswith("Abbrev Number: 0\n"):
                    # Tag close
                    lines = None
                else:
                    # Tag open
                    match = self.TAG_RGX.match(line)
                    lines = list()
                    tid = _hex_int(match.group("tid"))
                    tag = match.group("tag")
                    d = {
                        "tid": tid,
                        "tag": tag,
                        "lines": lines
                    }
                    if tag in self.SIBLING_TAGS:
                        siblings.append(d)
                    else:
                        siblings = list()
                        d["siblings"] = siblings
                        tags[tid] = d

            else:
                if line and lines is not None:
                    lines.append(line)

        self.tags = tags

    TYPE_TAGS = set(["DW_TAG_base_type",
                     "DW_TAG_typedef",
                     "DW_TAG_array_type",
                     "DW_TAG_pointer_type",
                     "DW_TAG_enumeration_type",
                     "DW_TAG_structure_type",
                     "DW_TAG_union_type",
                     "DW_TAG_const_type"])

    INTERESTING_ATTRS_PER_TAG = {
        # Top level
        "DW_TAG_base_type": {
            "DW_AT_byte_size": _auto_int,
            "DW_AT_encoding": str
        },
        "DW_TAG_typedef": {
            "DW_AT_name": str,
            "DW_AT_type": _auto_int
        },
        "DW_TAG_array_type": {
            "DW_AT_type": _auto_int
        },
        "DW_TAG_pointer_type": {
            "DW_AT_type": _auto_int
        },
        "DW_TAG_enumeration_type": {
            "DW_AT_name": str,
            "DW_AT_byte_size": _auto_int
        },
        "DW_TAG_structure_type": {
            "DW_AT_name": str,
            "DW_AT_byte_size": _auto_int,
            "DW_AT_declaration": _auto_int
        },
        "DW_TAG_union_type": {
            "DW_AT_name": str,
            "DW_AT_byte_size": _auto_int
        },
        "DW_TAG_const_type": {
            "DW_AT_type": _auto_int
        },
        "DW_TAG_volatile_type": {
            "DW_AT_type": _auto_int
        },
        "DW_TAG_subprogram": {
            "DW_AT_name": str,
            "DW_AT_type": _auto_int
        },
        # Siblings
        "DW_TAG_member": {
            "DW_AT_name": str,
            "DW_AT_type": _auto_int,
            "DW_AT_byte_size": _auto_int,
            "DW_AT_bit_size": _auto_int,
            "DW_AT_bit_offset": _auto_int,
            "DW_AT_data_member_location": _auto_int
        },
        "DW_TAG_subrange_type": {
            "DW_AT_upper_bound": _auto_int
        },
        "DW_TAG_enumerator": {
            "DW_AT_name": str,
            "DW_AT_const_value": _auto_int
        },
        "DW_TAG_formal_parameter": {
            "DW_AT_name": str,
            "DW_AT_type": _auto_int
        },
    }

    @staticmethod
    def _enrich_dict(d):
        interesting = DumbTextParserDwarf2PyEngine.INTERESTING_ATTRS_PER_TAG.get(
            d["tag"])
        if interesting:
            for line in d["lines"]:
                match = DumbTextParserDwarf2PyEngine.ATTR_RGX.match(line)
                attr = match.group("attr")
                cast = interesting.get(attr)
                if cast:
                    d[attr] = cast(match.group("val"))

    def get_tid_dict(self, tid):
        d = self.cache.get(tid)
        if not d:
            d = self.tags[tid]
            self.cache = d
        self._enrich_dict(d)
        siblings = d.get("siblings")
        if siblings:
            for sibling in siblings:
                self._enrich_dict(sibling)

        return d

    def all_tags(self, tag_types):
        if not tag_types:
            for tid in self.tags:
                yield self.get_tid_dict(tid)
        else:
            for d in self.tags.values():
                if d["tag"] in tag_types:
                    yield self.get_tid_dict(d["tid"])
