#!/usr/bin/env python3

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0



"""
A script to test MCS packer
"""

import sys
import os
import logging
sys.path.append(os.path.join("management_cm", "client", "test"))
from test_data import data
from time import perf_counter as pc
TEST_DIR = "management_cm/client/test/filter"
a = os.getcwd()
client_dir = os.path.join(a, "management_cm", "client")
sys.path.append(client_dir)
mgmt_dir = os.path.join(a, "management_cm")
sys.path.append(mgmt_dir)

os.chdir(mgmt_dir)
from client import process_scheme
logger = logging.getLogger(__name__)
logging.basicConfig(filename=None, encoding='utf-8', level=logging.WARNING)
scheme_file = os.path.join(a, "management_cm", "client", "clnt_scheme.json")
packer = process_scheme.Packer(scheme_file, mgmt_dir, logger)

import client_messages
sys.path.append("../../client")
os.chdir(a + "/" + TEST_DIR)


msg_dat = data.MIDDLEMAN_ATTACH_MESSAGE
msg_dat['payload']['messageTypeVersion'] = msg_dat['messageTypeVersion']

packer.set_lib("client_messages")

t0 = pc()
data_to_send = packer.pack2(90, msg_dat['payload'])
t1 = pc()
print(f"the entire pack process took {t1 - t0}")

t0 = pc()
import cProfile
#cProfile.run("client_messages.fix_and_split_vol_conf(msg_type=\"attach_volumes_message\", data=msg_dat['payload'], logger=logger)")
client_messages.fix_and_split_vol_conf(msg_type="attach_volumes_message", data=msg_dat['payload'], logger=logger)
t1 = pc()
#for r in res:
#    print(f"r={r['targets']}")

print(f"calling prepack hook took {t1 - t0}")

print("Hooray")
