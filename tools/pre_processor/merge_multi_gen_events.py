#!/usr/bin/python3

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

import sys
import os
import json
import zlib
import shutil


def get_all_jsons(p):
    ret = []
    for root, _, files in os.walk(p):
        for item in files:
            if item.endswith(".trace.json"):
                ret.append(os.path.join(root, item))
    return ret


def main():
    output = sys.argv[1]
    json_path = sys.argv[2]
    compiled_objects = [os.path.basename(o) for o in sys.argv[3:]]
    jsons = get_all_jsons(json_path)
    needed_jsons = []

    for obj in compiled_objects:
        search = obj[:-2]  # remove the .o
        for j in jsons:
            if j.endswith('/' + search + '.trace.json'):
                needed_jsons.append(j)
                break

    # order is important so we order the jsons by names so it will be consistent among machines!
    needed_jsons = sorted(needed_jsons, key=lambda t: os.path.basename(t))
    merged_json = {'traces': []}

    need_dictionary = True
    for j in needed_jsons:
        with open(j, "r") as f:
            o_json = json.load(f)
            map(lambda t: t.pop('forced', None), o_json['traces'])

            merged_json['traces'].extend(o_json['traces'])
            if need_dictionary:
                merged_json['commit_id'] = o_json['commit_id']
                try:
                    tokens = os.environ["TOKENS"]
                    with open(tokens, "r") as tokens_f:
                        merged_json['dictionary'] = json.load(tokens_f)
                except Exception as e:
                    merged_json['dictionary'] = o_json['dictionary']
                need_dictionary = False
        try:
            os.remove(os.path.join(os.path.dirname(j), ".".join(['dict', str(o_json['cksum']), 'json'])))
        except Exception:
            pass

    merged_json['cksum'] = zlib.crc32(str(merged_json).encode()) % (1 << 32)

    with open(output, "w+") as f:
        json.dump(merged_json, f, indent=4)
    shutil.copyfile(output, os.path.join(os.path.dirname(output), "dict.{}.json".format(str(merged_json['cksum']))))


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (IOError, ValueError) as e:
        sys.exit(str(e))


