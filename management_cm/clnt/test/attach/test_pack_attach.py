#!/usr/bin/env python3

"""
A script to test MCS packer
"""

import sys
import os
import logging
import json
import struct
from subprocess import call
TEST_DIR = "management_cm/clnt/test/attach"

if call([
        "management_cm/clnt/process_scheme.py",
        "management_cm/clnt/clnt_scheme.json", TEST_DIR + "/nvmeibc_mcs_stub.h"
]):
    raise RuntimeError("could not create scheme h file")


if call([
        "management_cm/clnt/process_scheme.py", "--header",
        TEST_DIR + "/nvmeib_mcs_header.h"
]):
    raise RuntimeError("could not generate mcs stub header file")


sys.path.append("management_cm")
from clnt import process_scheme
a = os.getcwd()
os.chdir(a + "/" + TEST_DIR)

if call(["gcc", "-o", "test_load", "load.c"]):
    raise RuntimeError("Could not compile load.c")

MNG_MSG = """
{
  "messageTypeVersion": 1,
  "volumes": [
    {
      "_id": "testvol",
      "uuid": "0caecde0-e3d0-11ef-8835-7fb62f6db18a",
      "configuration": {
        "_id": "testvol",
        "action": "none",
        "version": 1,
        "numberOfMirrors": 0,
        "name": "testvol",
        "encryption": {
          "headerSize": 16
        },
        "relativeRebuildPriority": 10,
        "dataBlocks": 1,
        "lockServer": {
          "maxNOwners": 1,
          "type": 4,
          "locksetShift": -1
        },
        "enableCrcCheck": false,
        "RAIDLevel": "Concatenated",
        "blocks": 2685440,
        "stripeSize": 32,
        "blockSize": 4096,
        "isEncrypted": false,
        "status": "online",
        "uuid": "0caecde0-e3d0-11ef-8835-7fb62f6db18a",
        "reservation": {
          "mode": 2,
          "version": 2,
          "reservedBy": "nvme1040",
          "attachedClients": [
            "nvme1040"
          ],
          "lastTransitionDate": "2025-02-05T14:48:56.069Z",
          "preempt": 0
        },
        "chunks": [
          {
            "uuid": "0cb00660-e3d0-11ef-8835-7fb62f6db18a",
            "vlbs": 0,
            "vlbe": 2685439,
            "pRaids": [
              {
                "activated": false,
                "zone": "1",
                "uuid": "0cb00662-e3d0-11ef-8835-7fb62f6db18a",
                "stripeIndex": 0,
                "diskSegments": [
                  {
                    "uuid": "0cb00661-e3d0-11ef-8835-7fb62f6db18a",
                    "diskID": "S3HCNX0K800794.1",
                    "diskUUID": "faff8580-e3cf-11ef-8835-7fb62f6db18a",
                    "nodeUUID": "b4b06810-e3cf-11ef-8835-7fb62f6db18a",
                    "node_id": "nvme1038",
                    "pRaidIndex": 0,
                    "lbs": 1509632,
                    "lbe": 4195071,
                    "type": "data",
                    "pRaidTypeIndex": 0,
                    "status": "normal"
                  }
                ]
              }
            ]
          }
        ]
      },
      "attachment": {
        "version": 3,
        "attachmentsVersionRef": 4,
        "referenceIDs": [
          "testtest",
          "testtest2",
          "testtest3"
        ]
      }
    }
  ],
  "attachmentsVersion": 4
}
"""

#  test1 - pack attach command, send it through file to load.c. the c code will
#  read the data, incarnate it and verify that all data arrives correctly.
msg_dat = json.loads(MNG_MSG)
logger = logging.getLogger(__name__)
logging.basicConfig(filename=None, encoding='utf-8', level=logging.DEBUG)
packer = process_scheme.Packer("../../clnt_scheme.json", "../../..", logger)
sys.path.append("../../../clnt")
packer.set_lib("client_messages")
data_to_send = packer.pack2(90, msg_dat)
logger.debug(f"the packed message size is %s", len(data_to_send))

num_of_volumes = len(msg_dat["volumes"])
logger.debug("the expected number of messages in the binary buffer is %s",
             num_of_volumes)

assert len(data_to_send) == num_of_volumes

with open("vol.dat", "wb") as file:
    file.write(data_to_send[0])


logger.debug(
    "pack stage passed successfully, continue to run unpack compiled program")

if call("./test_load"):
    raise RuntimeError("ERROR: unpack test failed!!!")


# test2 - read load.c reply (vol_status_message) and verify that all data is in place
# and correspod to the attach command

with open("output.bin", 'rb') as file:
    bytes_data = file.read()

res = packer.unpack(bytes_data)

assert res["route"] == "/clients/updateAttachmentStatus"
assert res["messageType"] == "updateAttachmentStatus"
assert res["opcode"] == 2
payload = res["payload"]
attachments = payload["attachments"]
assert len(attachments) == len(msg_dat["volumes"])
msg_dat = json.loads(MNG_MSG)
vols = msg_dat["volumes"]
for v, attch_out in zip(vols, attachments):
    attch_in = v["attachment"]
    vals_in_both = list(set(attch_in["referenceIDs"]) & set(attch_out["referenceIDs"]))
    assert len(vals_in_both) == len(attch_out["referenceIDs"])

#  test3 - pack update command

# remove configuration and attachment section in volume, as this part does not appear in update volumes command
vols = msg_dat["volumes"]
for v in vols:
    v.update(v["configuration"])
    del v["configuration"]
    del v["attachment"]

data_to_send = packer.pack2(91, msg_dat)
assert len(data_to_send) == num_of_volumes

os.unlink("vol.dat")
with open("vol.dat", "wb") as file:
    file.write(data_to_send[0])

assert call("./test_load") == 0

with open("output.bin", 'rb') as file:
    bytes_data = file.read()

res = packer.unpack(bytes_data)

payload = res["payload"]
attachments = payload["attachments"]
assert len(attachments) == len(msg_dat["volumes"])

for attch_out in attachments:
    assert len(attch_out["referenceIDs"]) == 0


for f in [
        "nvmeib_mcs_header.h", "test_load", "nvmeibc_mcs_stub.h", "vol.dat",
        "output.bin"
]:
    os.unlink(f)

logger.debug("Hooray")
