#!python3
from logging import Logger
from typing import List, Dict, Any, Optional
import copy
import collections
import json

Json = Dict[str, Any]
SegmentList = List[Json]
ChunksList = List[Json]
TargetList = List[Json]

# DHSH: See _mm_seg_from_json() for full list of toma statuses
seg_status_client_ignores = ["marked_for_deletion"]
seg_status_client_dup_ignores = ["markedforrebuild", "replacement"]
seg_type_client_uses = ["data", "parity", "both"]

#def sendFilteredPayload(data Any: , opcode: int) -> bool:
#    return opcode in (MessageTypes.SEND_CONFIGURATION, MessageTypes.ATTACH_VOLUMES, MessageTypes.UPDATE_VOLUMES, MessageTypes.UPDATE_TARGET_NICS)

def getUnusedSegs(all_segs: SegmentList) -> SegmentList:
    return [
        seg for seg in all_segs
        if str(seg["type"]).lower() not in seg_type_client_uses
    ]

def getIgnoredSegs(all_segs: SegmentList) -> SegmentList:
    return [
        seg for seg in all_segs
        if str(seg["status"]).lower() in seg_status_client_ignores
    ]

def getUsedNonIgnoredSegs(all_segs:  SegmentList) -> SegmentList:
    return [
        seg for seg in all_segs
        if (str(seg["status"]).lower() not in seg_status_client_ignores) and (
            str(seg["type"]).lower() in seg_type_client_uses)
    ]

def getNormalSegs(all_segs: SegmentList) -> SegmentList:
    return [
        seg for seg in getUsedNonIgnoredSegs(all_segs)
        if str(seg["status"]).lower() not in seg_status_client_dup_ignores
    ]

def getDuplicateSegs(all_segs: SegmentList) -> SegmentList:
    normal_segs_idxs: List[int] = [seg["pRaidTypeIndex"] for seg in getNormalSegs(all_segs)]
    return [seg for seg in getUsedNonIgnoredSegs(all_segs) if
            (str(seg["status"]).lower() in seg_status_client_dup_ignores) and (
                seg["pRaidTypeIndex"] in normal_segs_idxs)]

def getRemovedSegs(all_segs: SegmentList) -> SegmentList:
    return getUnusedSegs(all_segs) + getIgnoredSegs(all_segs) + getDuplicateSegs(all_segs)

def getRemainingSegs(all_segs: SegmentList) -> SegmentList:
    normal_segs_idxs = [
        seg["pRaidTypeIndex"] for seg in getNormalSegs(all_segs)
    ]
    sgmnts = [
        seg for seg in getUsedNonIgnoredSegs(all_segs)
        if (str(seg["status"]).lower() not in seg_status_client_dup_ignores) or
        (seg["pRaidTypeIndex"] not in normal_segs_idxs)
    ]
    sgmnts.sort(key=lambda sgmnt: sgmnt["pRaidTypeIndex"])
    return sgmnts

def setNormalStatusOnSegs(segs: SegmentList, logger: Logger) -> SegmentList:
    for seg in segs:
        if (str(seg["status"]).lower() != "normal"):
            logger.warning("Segment %s status is %s changing to normal", seg, seg["status"])
            seg["status"] = "normal"
    return segs

def printEachRemovedSegInfo(all_segs: SegmentList, vol_name: str, logger: Logger) -> None:
    for seg in all_segs:
        if "status" in seg and str(seg["status"]).lower() in [
                "marked_for_rebuild_old", "marked_for_deletion",
                "markedforrebuild_old"
        ]:
            logger.debug(
                "Volume %s segment %s has one of marked_for_rebuild_old/marked"
                "_for_deletion/markedForRebuild_old"
                " (deprecated) statuses %s removing it",
                vol_name, seg, seg["status"])
        if "type" in seg and str(seg["type"]) not in seg_type_client_uses:
            logger.debug("Volume %s has raftonly segment %s removing it", vol_name, seg)

def checkChunkPraidOrder(chunks: ChunksList, logger: Logger) -> None:
    logger.debug("ORDERING: Checking Chunk/Praid ordering")
    for chunk in chunks:
        for i, praid in enumerate(chunk["pRaids"]):
            if i != praid["stripeIndex"]:
                logger.error(
                    "ORDERING: Chunk %s pRaids are not in order, ordinal %s, pRaid[stripeIndex] %s",
                    chunk, i, praid["stripeIndex"])
            for j, segment in enumerate(praid["diskSegments"]):
                if j != segment["pRaidTypeIndex"]:
                    logger.error(
                        "ORDERING: Praid %s segments are not in order, ordinal %s, segment[pRaidTypeIndex] %s",
                        praid, j, segment["pRaidTypeIndex"])

def printEachTargetAndDisk(targets: TargetList, logger: Logger) -> None:
    for target in targets:
        logger.debug(f"Using target {target}")
        for disk in target["disks"]:
            logger.debug(f"Using disk {disk}")


def set_cli_unique_id(mtype: str, data: Json, logger: Logger) -> Json:
    cli_unique_id = "cli_unique_id"

    assert mtype
    if mtype == 'attach_volumes_message':
        data[cli_unique_id] = 'FAAAAAAAAAAAAAA'
    elif mtype == 'update_volumes_message':
        data[cli_unique_id] = 'F0AAAAAAAAAAAAA'
    else:
        data[cli_unique_id] = ""

    return data


def setup_vol_reference_ids(ref_ids: Optional[List[str]]) -> List[Json]:
    """
    convert referenceIDs string array to mcs array with key = "val"
    """
    return [] if ref_ids is None else [{"val": v} for v in ref_ids]


def set_encryption_fields(vol: Json) -> None:
    """
    set volume configuration encryption fields in vol
    """
    mib_shift = 20  # size of one mega byte
    encryption_h: Json = vol.get("encryption", {"headerSize": 16})

    is_encrypted = vol.get("isEncrypted")
    if not is_encrypted:
        encryption_h["headerSize"] = 0

    header_size: int = encryption_h["headerSize"]
    block_size: int = vol["blockSize"]
    if block_size == 0:
        raise RuntimeError("block size is 0")
    n_header_size, reminder = divmod(header_size << mib_shift, block_size)
    if reminder != 0:
        raise RuntimeError(f"Header size is not divisible by the volume block size header in MiB {header_size}.")
    encryption_h["headerSizeBlocks"] = n_header_size
    vol["encryption"] = encryption_h


def fix_and_split_vol_conf(msg_type: str, data: Json, logger: Logger) -> List[Json]:
    TargetInfo = collections.namedtuple('TargetInfo', 'uuid, node_id')
    DiskInfo = collections.namedtuple('DiskInfo', 'diskID, uuid')
    assert "targets" not in data or not data["targets"]
    ret: List[Json] = []
    data = set_cli_unique_id(msg_type, data, logger)

    for vol in data["volumes"]:
        # Copy the entire configuration and filter out
        # redundant targets and disks (also deprecated segments)
        # Also remove the "configuration" field nesting
        new_data = copy.copy(data)
        if "configuration" in vol:
            vol.update(vol["configuration"])
            del vol["configuration"]

        attachment: Optional[Json] = vol.get("attachment")
        if attachment is not None:
            attachment["referenceIDs"] = setup_vol_reference_ids(attachment.get("referenceIDs"))
        new_data["volumes"] = [vol]

        set_encryption_fields(vol)

        targets = collections.defaultdict(set)  # targets 2 disks mapping
        # Collect all node_ids and diskIDs for the volume
        for chunk in vol["chunks"]:
            for praid in chunk["pRaids"]:
                removed_segs = []
                new_list_of_segs = praid["diskSegments"]
                if "status" not in praid["diskSegments"][0] or\
                        "type" not in praid["diskSegments"][0]:
                    logger.error(
                        "Cannot filter segments they do not have status or type fields"
                    )
                else:
                    removed_segs = getRemovedSegs(praid["diskSegments"])
                    logger.debug(
                        "Removing segs %s, they are one of raftonly / marked_for_rebuild_old / marked_for_deletion / markedforrebuild_old",
                        removed_segs)
                    new_list_of_segs = getRemainingSegs(praid["diskSegments"])
                    logger.debug(f"Filtered segs are {new_list_of_segs}")
                    new_list_of_segs = setNormalStatusOnSegs(new_list_of_segs, logger)
                printEachRemovedSegInfo(praid["diskSegments"], str(vol["name"]), logger=logger)
                praid["diskSegments"] = new_list_of_segs
                for segment in praid["diskSegments"]:
                    target = TargetInfo(uuid=segment['nodeUUID'], node_id=segment['node_id'])
                    disk_info = DiskInfo(diskID=segment['diskID'], uuid=segment['diskUUID'])
                    targets[target].add(disk_info)

        new_data["targets"] = []
        for target_info, disks_info in targets.items():
            target_data = {
                    'uuid': target_info.uuid,
                    'node_id': target_info.node_id,
                    'nicsVersion': 0,
                    'disks': [{'diskID': dinfo.diskID, 'uuid': dinfo.uuid} for dinfo in disks_info],
                    'nics': []
            }
            new_data["targets"].append(target_data)

        checkChunkPraidOrder(vol["chunks"], logger=logger)
        printEachTargetAndDisk(new_data["targets"], logger=logger)
        logger.debug("Generated filtered payload for volume %s: %s", vol["name"], json.dumps(new_data))
        new_data["expect_more_cmd_with_identical_av"] = 1

        ret.append(new_data)
    if ret:
        ret[-1]["expect_more_cmd_with_identical_av"] = 0
    return ret


def fix_vol_stat_msg(msg_type: str, data: Json, logger: Logger) -> Json:
    for vol in data["attachments"]:
        vol["referenceIDs"] = [s["val"] for s in vol["referenceIDs"]]
    logger.debug("vol status converted to: %s", str(data))
    return data


def split_update_tgt_nics(msg_type: str, data: Json, logger: Logger) -> List[Json]:
    """
    split "update_target_nics" command with multiple targets to multiple commands each with single target
    """
    ret: List[Json] = []
    targets = data.get("targets")
    if targets is not None and len(targets) > 1:
        for tgt in data["targets"]:
            root = copy.copy(data)
            root["targets"] = [tgt]
            ret.append(root)
    else:
        ret = [data]
    return ret
