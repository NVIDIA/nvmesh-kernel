#!/usr/bin/env python3

"""
A script to test MCS packer
"""

import sys
import os
import json
import struct
import logging 

TEST_DIR = "management_cm/clnt/test/acme"
sys.path.append("management_cm")
from clnt import process_scheme
a = os.getcwd()
os.chdir(a + "/" + TEST_DIR)


def set_owner_unknown(msg_type, msg_data, log):
    """
    a function that tests prepack hook
    """
    if "owner" not in msg_data:
        log.debug("setting owner to be unknown")
        msg_data["owner"] = "Unknown"

    return [msg_data]


MNG_MSG = """
{
    "opcode" : 1,
    "FactoryId" : 355,
    "employees" : [{
            "first_name" : "speedy",
            "last_name" : "gonzales",
            "id" : 2277960,
            "salary" : 9999,
            "siblings" : [{
                "first_name" : "Rapid",
                "last_name" : "Dave",
                "id" : 65543
            }],
            "interests" : [ {
                "ID" : "0001-0002-0003",
                "version" : 15
            }],
            "nationalities" : [{
                "val" : "Israel"
            }]
        },
        {
            "first_name" : "Bugs",
            "last_name" : "Bunny",
            "id" : 6565,
            "salary" : 55,
            "siblings" : [{
                "first_name" : "Hashafan",
                "last_name" : "Hakatan",
                "id" : 65543
            }, {
                "first_name" : "Not",
                "last_name" : "Have name",
                "id" : 434343
            }],
            "interests" : [ {
                "ID" : "0002-0003-0004",
                "version" : 21
            }, {
                "ID" : "0001-0002-0003",
                "version" : 63
            }],
            "nationalities" : [{
                "val" : "Israel"
            }, {
                "val" : "American"
            }]

        }]
}
"""
msg_dat = json.loads(MNG_MSG)
logger = logging.getLogger(__name__)
logging.basicConfig(stream=sys.stdout, level=logging.DEBUG)

packer = process_scheme.Packer("acme.json", None, logger)
packer.set_lib("__main__")
data_to_send = packer.pack2(1, msg_dat)[0]
# l = len(data_to_send)
# clean things and start from the beginning
packer = process_scheme.Packer("acme.json", None, logger)
print(f"data_to_send type is {type(data_to_send)}")
i = 0
while data_to_send:
    version, l1 = struct.unpack("II", data_to_send[:8])
    print(f"version={version} message_len={l1}")
    msg = packer.unpack(data_to_send)
    data_to_send = data_to_send[8 + l1:]
    print(f"unpacked message number {i}: {msg}")
print("Hooray")
