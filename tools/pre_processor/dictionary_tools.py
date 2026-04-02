#!/usr/bin/python3

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

import argparse
import json
import sys
import traceback
import re

C_TYPE_SIZE_RESOLVER = {
    'string_n': None,
    'string': 'string',
    'symbol': 'symbol',
    'stack_trace': 'stack_trace',
    'printf_arg': 'printf_arg',
    'vprintf_arg': 'vprintf_arg',
    'bitmap': None,
    'hex': None,
    'array_u64': None,
    'array_u64_flex': None,
    'array_u32_flex': None,
    'array_int_flex': None,
    'array_ptr_flex': None,
    'uuid': 128,
    'uuid_le': 128,
    'ipv6': 128,
    'pointer': 64,
    'char': 8,
    'short': 16,
    'int': 32,
    'long': 64,
    'longlong': 64,
    'float': 32,
    'double': 64
}

C_STRING_FMT_REGEX = r'.*\%-?\d*\.?\d*?s.*'

C_TYPE_FORMATS = {
    'string_n': C_STRING_FMT_REGEX,
    'string': C_STRING_FMT_REGEX,
    'symbol': C_STRING_FMT_REGEX,
    'stack_trace': C_STRING_FMT_REGEX,
    'printf_arg': C_STRING_FMT_REGEX,
    'vprintf_arg': C_STRING_FMT_REGEX,
    'ipv6': C_STRING_FMT_REGEX,
    'uuid': C_STRING_FMT_REGEX,
    'uuid_le': C_STRING_FMT_REGEX,
    'bitmap': C_STRING_FMT_REGEX,
    'hex': C_STRING_FMT_REGEX,
    'array_u64': C_STRING_FMT_REGEX,
    'array_u64_flex': C_STRING_FMT_REGEX,
    'array_u32_flex': C_STRING_FMT_REGEX,
    'array_int_flex': C_STRING_FMT_REGEX,
    'array_ptr_flex': C_STRING_FMT_REGEX,
    'pointer': r'.*\%-?(p|ll).*',
    'char': r'.*\%-?\d*\.?\d*(c|d|u|x).*',
    'short': r'.*\%#?-?\d*\.?\d*h?(d|x|u).*',
    'int': r'.*\%#?-?\d*\.?\d*(?:j)?(d|x|u|i|X).*',
    'long': r'.*\%#?-?\d*\.?\d*(?:l|z)(d|x|u).*',
    'longlong': r'.*\%#?-?\d*\.?\d*(?:(?:ll)|j)(d|x|u).*',
    'float': r'.*\%-?\d*\.?\d*f.*',
    'double': r'.*\%-?\d*\.?\d*(?:lf|f|g).*',
}

C_DMESG_TYPE_FORMATS = {
    'ipv6': r'.*\%pI6.*',
    'uuid': r'.*\%pU(?:B|b).*',
    'uuid_le': r'.*\%pU(?:L|l).*',
    'bitmap': r'.*\%[0-9]*pbl.*',
    'hex': r'.*\%[0-9]*ph.*',
    'symbol': r'.*\%(?:pf|pF|ps|pS).*',
    'stack_trace': r'.*\%(?:p).*',
}

C_BASE_TYPE_ALIASES = {
    'char': set(['char', 's8', 'u8']),
    'short': set(['short', 's16', 'u16']),
    'int': set(['int', 's32', 'u32']),
    'longlong': set(['longlong', 's64', 'u64']),
    'long': set(['long']),
    'float': set(['float', 'f32']),
    'double': set(['double', 'f64']),
}

C_SPECIAL_TYPES = set(key for key, val in C_TYPE_FORMATS.items() if val == C_STRING_FMT_REGEX)

C_BASE_TYPE_ALIASES_HELPER = {}
for k in C_BASE_TYPE_ALIASES:
    for v in C_BASE_TYPE_ALIASES[k]:
        C_BASE_TYPE_ALIASES_HELPER[v] = k


def extract_base_type(t):
    if '*' in t:
        return 'pointer'  # It is pointer
    if t in C_SPECIAL_TYPES:
        return t
    if 'auto_errno' == t:
        return 'string'  # It is pointer
    t = t.replace('const', '')  # Strip buzzwords
    t = t.replace('unsigned', '')
    t = t.strip()
    return C_BASE_TYPE_ALIASES_HELPER[t]  # If it is standard C type


def verify_single_element(k, v):
    # Type matches size
    try:
        basetype = extract_base_type(v['type'])
        expsize = C_TYPE_SIZE_RESOLVER[basetype]
        expfmtrgx = C_TYPE_FORMATS[basetype]
        exp_gmesg_rgx = C_DMESG_TYPE_FORMATS.get(basetype, None)
    except Exception as e:
        raise type(e)(str(e) + ' while validating token "{}"'.format("example_token")).with_traceback(sys.exc_info()[2])
    try:
        if expsize and v['size'] != expsize and not v.get('is_bitfield'):
            raise ValueError('Token "{}" : Type "{}" does not match size "{}", expected "{}"'.format(
                k, v['type'], v['size'], expsize))
    except Exception as e:
        raise type(e)(str(e) + ' while validating token "{}"'.format(k)).with_traceback(sys.exc_info()[2])
    if not re.match(expfmtrgx, v['fmt']):
        raise ValueError('Bad format "{}" for token "{}" with type "{}" aka "{}"'.format(
            v['fmt'], k, v['type'], basetype))
    if exp_gmesg_rgx and not re.match(exp_gmesg_rgx, v['dmesg_fmt']):
        raise ValueError('Bad dmesg format "{}" for token "{}" with type "{}" aka "{}"'.format(
            v['dmesg_fmt'], k, v['type'], basetype))

def verify_dup_traces(lst):
    tmp = dict()
    msg = ''
    for v in lst:
        prev = tmp.get(v['name'])
        if prev:
            if prev['file'] != v['file'] or abs(prev['line'] - v['line'] > 2 or prev['macro'] != v['macro'] or prev['scope'] != v['scope']):
                msg += 'Duplicate trace definition at {0}:{1}, previous defined at {2}:{3}\n'.format(v['file'], v['line'], prev['file'], prev['line'])
        else:
            tmp[v['name']] = v
    if msg:
        raise ValueError(msg)


def verify_dictionary(dictionary):
    newitems = dict()
    for k in dictionary:
        if dictionary[k].get('composite'):
            verify_dictionary(dictionary[k].get('composite'))
            for sub, subval in dictionary[k].get('composite').items():
                newitems[k + '.' + sub[1:]] = subval
        else:
            verify_single_element(k, dictionary[k])
    dictionary.update(newitems)


def verify_traces(traces):
    verify_dup_traces(traces)


def verify_cksum(cksum):
    return cksum is not None and type(cksum) == int


def verified_decoder(ordered_pairs):
    """ This decoder will raise if it spots duplicate keys in the dictionary
    """
    d = {}
    for k, v in ordered_pairs:
        if k in d:
            raise ValueError('Duplicate key in dictionary: "{}"'.format(k))
        else:
            d[k] = v
    return d


def read_dictionary(filename):
    with open(filename, 'r') as fd:
        return json.load(fd)


def read_dictionary_and_verify(filename):
    with open(filename, 'r') as fd:
        if sys.version_info[0] >= 3 or (sys.version_info[0] >= 2 and sys.version_info[1] >= 7):
            # In python >= 2.7 we have object_pairs_hook
            d = json.load(fd, object_pairs_hook=verified_decoder)
        else:
            # In python <= 2.6 we don't have functionality to validate duplicate keys in json.
            # It is out of scope thouth, since it is not the main build target, even not a
            # common one, duplicate keys will be validated in other targets
            d = json.load(fd)
    verify_dictionary(d.get("dictionary"))
    verify_traces(d.get("traces"))
    verify_cksum(d.get("cksum"))
    return d


def verify_dictionary_action(pargs):
    read_dictionary_and_verify(pargs.dictionary_file)
    print('Dictionary is valid')


def init_parser():
    parser = argparse.ArgumentParser()
    parser.add_argument('-v', '--verify', action='store_const',
                        dest='action', const=verify_dictionary_action, default=verify_dictionary_action)
    parser.add_argument('-d', '--dictionary-file', action='store',
                        dest='dictionary_file', type=str, default='dictionary.json')
    return parser


def main():
    pargs = init_parser().parse_args()
    pargs.action(pargs)

    return 0


if __name__ == '__main__':
    sys.exit(main())
