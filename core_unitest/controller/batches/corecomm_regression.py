#!/usr/bin/env python

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0


import time
import re
import os
import sys
import random
import logging
import argparse
from itertools import chain

sys.path.append(os.path.abspath('..'))

from corecomm_controller_infra import (marry_all_nodes,
                                       unbind_all_nodes,
                                       CorecommTestException,
                                       BINJE_ALLOWED_VALUES,
                                       set_binje,
                                       get_total_drives_list)  # nopep8

log = logging.getLogger('corecomm-regression')

STALE_BIT = (1 << 28)
READLOCK_BIT = (1 << 29)

ERROR_TIMEOUT = -86376423
ERROR_BAIL = -86376424

MAX_EC_SLICE_FOR_TESTS = 2


#@TODO: Revive this test, currently excluded (remove the leading _ to include)
def _test_case_locate_rsc(nodes):
    """ Test case for locate rsc corner cases during discovery
    """
    # Helper function
    def parse_discovery_stats_from_proc(node, disk_name):
        ctx = node.shell(
            'cat /proc/nvmeibc/disks/{0}/diag'.format(disk_name))
        server_locate_zero_rsc = int(
            re.search(r'server_locate_zero_rsc: (\d*)', ctx, re.M).group(1))
        locate_timeout = int(
            re.search(r'locate_timeout: (\d*)', ctx, re.M).group(1))
        locate_timeout_non_zero_rsc = int(
            re.search(r'locate_timeout_non_zero_rsc: (\d*)', ctx, re.M).group(1))
        return server_locate_zero_rsc, locate_timeout, locate_timeout_non_zero_rsc

    if (len(nodes) < 2):
        log.info("Test requires at least 2 separate nodes")
        return True
    # For this test, we require a drive supporting metadata
    srv, clnt = None, None
    while srv == clnt:
        srv, clnt = random.choice(nodes), random.choice(nodes)
    drive = random.choice(srv.drives.items())

    log.info('srv={0}, clnt={1}, drive={2}'.format(
        srv.hostname, clnt.hostname, drive[0]))

    # Must ensure disk is not discovered
    log.info("Cleanup before test")
    tmpd = clnt.discover_raw(drive[0], srv)
    tmpd.disk_remove()

    time.sleep(1)

    log.info("Starting test")

    # Run discovery + release
    srv.set_sym("corecomm_inj_locate_rsc_timeout", 1)
    try:
        with clnt.discover(drive[0], srv) as disk:
            time.sleep(5)  # Wait for all to flush

            post_server_locate_zero_rsc, post_locate_timeout, post_locate_timeout_non_zero_rsc = parse_discovery_stats_from_proc(
                clnt, drive[0])

            assert post_server_locate_zero_rsc == 1, "Unexpected post_server_locate_zero_rsc {0}".format(
                post_server_locate_zero_rsc)
            assert post_locate_timeout == 1, "Unexpected post_locate_timeout {0}".format(
                post_locate_timeout)
            assert post_locate_timeout_non_zero_rsc == 0, "Unexpected post_locate_timeout_non_zero_rsc {0}".format(
                post_locate_timeout_non_zero_rsc)
    finally:
        srv.set_sym("corecomm_inj_locate_rsc_timeout", 0)

    return True


def test_case_free_journal_entry(nodes):
    """ Test case for htr's free journal entry
    """
    # For this test, we require a drive supporting metadata
    srv, drive = random.choice([(srv, drive)
                                for srv in nodes for drive in srv.drives.items() if drive[1]["md"] >= 8])
    clnt = random.choice(nodes)
    log.info('srv={0}, clnt={1}, drive={2}'.format(
        srv.hostname, clnt.hostname, drive[0]))

    # Run discovery (if needed)
    with clnt.discover(drive[0], srv, wait_for_serjio_timeout=10) as disk:

        # Now we can pick jmdc + jentry
        jrange = random.choice(srv.get_allocated_jris(drive[0]))
        jentry = random.randint(0, 511)
        # Blkset - select somewhere after the start of data segment in gpt
        # TODO: Make more generic
        blkset_slba = random.randint(1600000, 1700000)

        log.info('srv={0}, clnt={1}, drive={2}, jrange={3} jentry={4} blkset_slba={5}'.format(
            srv.hostname, clnt.hostname, drive[0], jrange, jentry, blkset_slba))

        # Inject some data
        srv.set_jmdc_entry(drive[0], jrange, jentry, blkset_slba, 20, 30)
        srv.set_unknown_entry(drive[0], jrange, jentry)

        sgmt_uuid = str(srv.read_gpt(drive[0])[0])
        boot_id = str(srv.get_serjio_boot_id(drive[0]))

        jmdc = srv.get_jmdc_as_json(drive[0], jrange)[jentry]

        assert jmdc["status"] == "UNKNOWN", "Status injection failed, WTF?"

        disk.pd_free_jrnl_ents(sgmt_uuid, blkset_slba, 1, 0,
                               123, boot_id, jrange, jentry, jmdc["gen_id"], 1)

        jmdc = srv.get_jmdc_as_json(drive[0], jrange)[jentry]

        assert jmdc["status"] == "TAKEN", "Free jetry didn't free anything"

    return True


def test_case_erase_jentry(nodes):
    """ Test case for jam command erase jentry
    """
    # For this test, we require a drive supporting metadata
    srv, drive = random.choice([(srv, drive)
                                for srv in nodes for drive in srv.drives.items() if drive[1]["md"] >= 8])
    clnt = random.choice(nodes)
    log.info('srv={0}, clnt={1}, drive={2}'.format(
        srv.hostname, clnt.hostname, drive[0]))

    # Run discovery (if needed)
    with clnt.discover(drive[0], srv) as disk:

        # Now we can pick jmdc + jentry
        jrange = random.choice(srv.get_allocated_jris(drive[0]))
        jentry = random.randint(0, 511)

        log.info('srv={0}, clnt={1}, drive={2}, jrange={3} jentry={4}'.format(
            srv.hostname, clnt.hostname, drive[0], jrange, jentry))

        gen_id = srv.get_jmdc_as_json(drive[0], jrange)[jentry]['gen_id']

        # Try erase with wrong gen_id. Expect to fail.
        log.info("Expect to fail...")
        try:
            disk.gen_jentry_erase(jentry, gen_id-1)
            assert False, "Did not fail when expected to fail"
        except OSError:
            # Good, we expect OSError here
            log.info("Ok")

        # Inject some meta-data to the entry, expect it to disappear
        srv.set_jmdc_entry(drive[0], jrange, jentry, 10, 20, 30)

        log.info("Expect to succeed...")
        # This call is correct, should not fail
        disk.gen_jentry_erase(jentry, gen_id)
        log.info("Ok")

        # Lets read it
        jmdc = srv.get_jmdc_as_json(drive[0], jrange)[jentry]
        assert jmdc['j2d'] == 4294967295 and jmdc['tx_id'] == 1, "Unexpected jentry after erase ({0},{1})".format(
            jmdc['j2d'], jmdc['tx_id'])

        return True


def test_case_blkset_recovered_async_cookies(nodes):
    """ Test case for blkset recovered command, specifically async cookies mechanism it uses
    """
    # For this test, we require a drive supporting metadata
    srv, drive = random.choice([(srv, drive)
                                for srv in nodes for drive in srv.drives.items() if drive[1]["md"] >= 8])
    clnt = random.choice(nodes)

    # We give higher chance for corner cases
    delay = random.choice([0, 100, 1000, 3000, 6000, "fully random"])
    if delay == "fully random":
        delay = random.randint(100, 6000)

    log.info('srv={0}, clnt={1}, drive={2}'.format(
        srv.hostname, clnt.hostname, drive[0]))

    # Run discovery (if needed)
    with clnt.discover(drive[0], srv, wait_for_serjio_timeout=10) as disk:

        # Now we can pick jmdc + jentry
        jrange = random.choice(srv.get_allocated_jris(drive[0]))
        jentry = random.randint(0, 511)

        log.info('srv={0}, clnt={1}, drive={2}, jrange={3} jentry={4} delay={5}'.format(
            srv.hostname, clnt.hostname, drive[0], jrange, jentry, delay))

        clnt_uuid = str(srv.get_serjio_client_uuid(drive[0], clnt.hostname))
        sgmt_uuid = str(srv.read_gpt(drive[0])[0])

        local = clnt == srv

        # Do not actually send events to toma
        srv.set_sym("corecomm_inj_toma_do_send_event", 0)
        srv.set_sym("corecomm_inj_serjio_do_send_event", 0)
        srv.set_sym("corecomm_inj_toma_send_event_delay", delay)

        try:
            disk.gen_blkset_recovered(
                clnt_uuid, sgmt_uuid, 8, jrange, jentry, 1)
        except OSError as e:
            assert delay > 2000 and ((e.errno == ERROR_TIMEOUT and local) or (
                e.errno == -5 and not local)), "Unexpected errno: {0}".format(e.errno)
        finally:  # Revert all back
            # Else we may crash toma on the last iteration
            time.sleep(delay / 1000)
            srv.set_sym("corecomm_inj_toma_do_send_event", 1)
            srv.set_sym("corecomm_inj_serjio_do_send_event", 1)
            srv.set_sym("corecomm_inj_toma_send_event_delay", 0)

        with clnt.dup() as clnt_dup:
            with clnt_dup.discover(drive[0], srv) as disk_dup:
                # Now test the same but with disasters
                log.info("Adding some disasters")

                srv.set_sym("corecomm_inj_toma_do_send_event", 0)
                srv.set_sym("corecomm_inj_serjio_do_send_event", 0)
                srv.set_sym("corecomm_inj_toma_send_event_delay", delay)

                try:
                    future = disk.gen_blkset_recovered__async(
                        clnt_uuid, sgmt_uuid, 8, jrange, jentry, 1)
                    disk_dup.disk_remove()  # Synchronously remove the disk
                    # Now lets see how our future completed
                    future.wait()

                    # Case 1 - Managed to perform the op before stop
                    assert (not future.err) or (
                        # Case 2 - Local, op started before shutdown, bailed (-ECANCELED)
                        local and future.err.errno == ERROR_BAIL) or (
                        # Case 3 - Remote, op started before shutdown, bailed (-ECANCELED), but nordda converted it to -5
                        not local and future.err.errno == -5) or (
                        # Case 4 - Op started after shutdown, channel dead
                        future.err.errno == -0xdead), "Unexpected error during disaster: {0}".format(future.err)
                finally:  # Revert all back
                    # Else we may crash toma on the last iteration
                    time.sleep(delay / 1000)
                    srv.set_sym("corecomm_inj_toma_do_send_event", 1)
                    srv.set_sym("corecomm_inj_serjio_do_send_event", 1)
                    srv.set_sym("corecomm_inj_toma_send_event_delay", 0)

    return True


def test_case_lock_ops(nodes):
    """ Test case for lock ops
    """
    # Select test data randomly: server, client, drive and address
    srv = random.choice(nodes)
    clnt = random.choice(nodes)
    drive = random.choice(srv.drives.items())
    addr = random.randint(1000, 100000)

    log.info('srv={0}, clnt={1}, drive={2}, addr={3}'.format(
        srv.hostname, clnt.hostname, drive[0], addr))

    # Run discovery (if needed)
    with clnt.discover(drive[0], srv) as disk:

        # Cmpxchng some address - assert success
        ld = disk.pd_cmpxchg(addr, 0, 69420)
        assert ld.status.value == 4, "Bad lock status {0}".format(ld.status.value)

        # Cmpxchng again, now address is dirty - assert failure
        ld = disk.pd_cmpxchg(addr, 0, 69420)
        assert ld.status.value == 5, "Bad lock status {0}".format(ld.status.value)
        assert ld.value.value == 69420, "Bad lock value {0}".format(ld.value.value)

        # Read lock via pd - assert same value
        ld = disk.pd_read_lock(addr)
        assert ld.value.value == 69420, "Bad lock value {0}".format(ld.value.value)

        # Driect read - assert same value
        ld = srv.funcs.corecomm_direct_read_lock(drive[0], addr)
        assert ld.value.value == 69420, "Bad lock value {0}".format(ld.value.value)

        # Write binfo and test
        disk.pd_write_blkset_info(addr, 42069)

        ld = disk.pd_read_lock(addr)
        assert ld.value.value == (42069 << 32) + \
            69420, "Bad lock value {0}".format(ld.value.value)

        # Revert binfo
        disk.pd_write_blkset_info(addr, 0)

        # Cmpxchng back - assert success
        ld = disk.pd_cmpxchg(addr, 69420, 0)
        assert ld.status.value == 4, "Bad lock status {0}".format(ld.status.value)

    return True

def test_case_journal_alloc(nodes):
    # For this test, we use X separate disks, possibly on diffrent servers.
    # Each must have metadata.
    total_md_drives_in_system = [ drive for drive in get_total_drives_list(nodes) if drive["info"]["md"] >= 8 ]
    n_disks = random.randint(1, min(len(total_md_drives_in_system), MAX_EC_SLICE_FOR_TESTS))
    drives = random.sample(total_md_drives_in_system, n_disks)
    addrs = [random.randint(drive["info"]["size"]/4096/2, drive["info"]["size"]/4096-1) for drive in drives]
    txid = random.randint(2, 2000)
    clnt = random.choice(nodes)
    drives_srvs = zip([drive["name"] for drive in drives],
        [drive["node"] for drive in drives])

    log.info('n_disks={0} clnt={1}, srvs={2}, drives={3}, addrs={4} txid={5}'.format(
        n_disks,
        clnt.hostname,
        [drive["node"].hostname for drive in drives],
        [drive["name"] for drive in drives],
        addrs,
        txid))

    # The test:
    # 1. Discover on all disks
    # 2. Alloc journals on alldiscs
    # 3. Free journals. Expect no error.
    # 4. Alloc journals with the same arguments. Expect same journals.
    # 5. Change txid. Expect different journals

    with clnt.discover_multiple(drives_srvs) as disks:
        jlbas1, jlbas2, jlbas3 = None, None, None
        
        log.info("Alloc journals 1")
        jlbas1 = clnt.quick_alloc_jrnls(disks, addrs, txid)
        clnt.quick_free_jrnls(disks, jlbas1)

        log.info("Alloc journals 2")
        jlbas2 = clnt.quick_alloc_jrnls(disks, addrs, txid+1)
        clnt.quick_free_jrnls(disks, jlbas2)

        assert jlbas1 != jlbas2, "Expected different jlbas, got the same: {0}".format(jlbas1)

        log.info("Alloc journals 3")
        jlbas3 = clnt.quick_alloc_jrnls(disks, addrs, txid)
        clnt.quick_free_jrnls(disks, jlbas3)

        assert jlbas1 == jlbas3, "Expected same jlbas, got differnt: {0} != {1}".format(jlbas1, jlbas3)

    return True

def test_case_journal_io_ops(nodes):
    # For this test, we use X separate disks, possibly on diffrent servers.
    # Each must have metadata.
    total_md_drives_in_system = [ drive for drive in get_total_drives_list(nodes) if drive["info"]["md"] >= 8 ]
    n_disks = random.randint(1, min(len(total_md_drives_in_system), MAX_EC_SLICE_FOR_TESTS))
    drives = random.sample(total_md_drives_in_system, n_disks)
    addrs = [random.randint(drive["info"]["size"]/4096/2, drive["info"]["size"]/4096-1) for drive in drives]
    txid = random.randint(2, 2000)
    clnt = random.choice(nodes)
    binje = 1
    #binje = random.choice(BINJE_ALLOWED_VALUES) #@TODO: Uncomment this when MS fully functional
    iosize = random.randint(1, binje)
    drives_srvs = zip([drive["name"] for drive in drives],
        [drive["node"] for drive in drives])

    log.info('n_disks={0} clnt={1}, srvs={2}, drives={3}, addrs={4} txid={5} binje={6} iosize={7}'.format(
        n_disks,
        clnt.hostname,
        [drive["node"].hostname for drive in drives],
        [drive["name"] for drive in drives],
        addrs,
        txid,
        binje,
        iosize))

    # The test:
    # 1. Discover all disks
    # 2. Allocate journals
    # 3. Perform journal write keeping in mind specific binje
    # 4. Perform NON journal read from jlba
    # 5. Validate results match

    log.info("Discovery...")
    with clnt.discover_multiple(drives_srvs) as disks, set_binje(binje, nodes):
        log.info("Alloc journals..")
        with clnt.alloc_jrnls(disks, addrs, txid) as jlbas:
            for disk, jlba, dlba in zip(disks, jlbas, addrs):
                stamps = [random.randint(1, 10000) for i in range(iosize)]
                mdstamps = [random.randint(1, 10000) for i in range(iosize)]
                mdstamps[0] = clnt.types.union__corecomm_jblock_md.ctype(
                    v0 = clnt.types.struct__corecomm_jblock_md_v0.ctype(
                        j2d_0=dlba, tx_id=txid, version=1, tx_bmp=15)).raw.value

                ld = clnt.quickwrite(disk._handle, jlba, stamps, md=mdstamps, is_jour=True)
                assert ld.io_status.value == 0, "Bad io status {0}".format(ld.io_status.value)

                stamps2, mdstamps2, ld2 = clnt.quickread(
                disk._handle, jlba, iosize)

                assert ld2.io_status.value == 0, "Bad io status {0}".format(
                    ld2.io_status.value)
                assert stamps2 == stamps, "Bad stamps asserted:{0} <=> got:{0}".format(
                    stamps, stamps2)
                assert mdstamps2 == mdstamps, "Bad mdstamps asserted:{0} <=> got:{0}".format(
                    mdstamps, mdstamps2)

                jrange = disk._srv.get_serjio_client_jri(disk._name, disk._clnt.hostname)
                jentry = disk.jam_lba_2_idx(jlba).value
                res = disk.pd_jmdc_read(jrange, 1, 0)
                assert res.md.arr[jentry].j2d.value == dlba and res.md.arr[jentry].tx_id.value == txid, "Error: unexpected jmdc after IO: j2d={0}, tx_id={1}".format(res.md.arr[jentry].j2d.value, res.md.arr[jentry].tx_id.value)

    return True

def test_case_io_ops(nodes):
    """ Test case for io ops
    """
    # Select test data randomly: server, client, drive and address
    srv = random.choice(nodes)
    clnt = random.choice(nodes)
    drive = random.choice(srv.drives.items())
    # Start from lba 1000 in order not to mess up the gpt
    addr = random.randint(1000, 100000)
    iosize = random.randint(1, 32)
    binje = random.choice(BINJE_ALLOWED_VALUES)

    log.info('srv={0}, clnt={1}, drive={2}, addr={3} len={4} binje={5}'.format(
        srv.hostname, clnt.hostname, drive[0], addr, iosize, binje))

    # Run discovery (if needed)
    with clnt.discover(drive[0], srv) as disk, set_binje(binje, nodes):
        # Generate some data
        stamps = [random.randint(1, 10000) for i in range(iosize)]

        # Start by writing some data to lock to test piggiback
        ld = disk.pd_cmpxchg(addr, 0, 69420)
        assert ld.status.value == 4, "Bad lock status {0}".format(ld.status.value)

        # Write something, then read
        ld = clnt.quickwrite(disk._handle, addr, stamps)
        assert ld.io_status.value == 0, "Bad io status {0}".format(ld.io_status.value)

        stamps2, _, ld2 = clnt.quickread(disk._handle, addr, iosize, pb=True)

        assert ld2.io_status.value == 0, "Bad io status {0}".format(ld2.io_status.value)
        assert ld2.value.value == 69420, "Bad lock value {0}".format(ld2.value.value)
        assert stamps2 == stamps, "Bad stamps asserted:{0} <=> got:{0}".format(
            stamps, stamps2)

        # Revert the lock
        ld = disk.pd_cmpxchg(addr, 69420, 0)
        assert ld.status.value == 4, "Bad lock status {0}".format(ld.status.value)

        if (drive[1]['md'] > 0):
            # Drive has metadata - lets test metadata too because why not
            log.info('Drive {0} has metada, testing'.format(drive[0]))

            # Generate some data
            stamps = [random.randint(1, 10000) for i in range(iosize)]
            mdstamps = [random.randint(1, 10000) for i in range(iosize)]

            # Write something, then read
            ld = clnt.quickwrite(disk._handle, addr, stamps, md=mdstamps)
            assert ld.io_status.value == 0, "Bad io status {0}".format(ld.io_status.value)

            stamps2, mdstamps2, ld2 = clnt.quickread(
                disk._handle, addr, iosize)

            assert ld2.io_status.value == 0, "Bad io status {0}".format(
                ld2.io_status.value)
            assert stamps2 == stamps, "Bad stamps asserted:{0} <=> got:{0}".format(
                stamps, stamps2)
            assert mdstamps2 == mdstamps, "Bad mdstamps asserted:{0} <=> got:{0}".format(
                mdstamps, mdstamps2)

    return True


def test_case_get_problems(nodes):
    """ Test case for get blkset problems gen cmd
    """
    # Select test data randomly: server, client, drive and address
    srv = random.choice(nodes)
    clnt = random.choice(nodes)
    drive = random.choice(srv.drives.items())
    addr = random.randint(1000, 100000)
    blkset = int(addr / 32)

    log.info('srv={0}, clnt={1}, drive={2}, addr={3} blkset={4}'.format(
        srv.hostname, clnt.hostname, drive[0], addr, blkset))

    # Run discovery (if needed)
    with clnt.discover(drive[0], srv) as disk:

        disk.pd_write_blkset_info(addr, STALE_BIT)

        res = disk.pd_get_blkset_problems(blkset - 1, 3, 1, 1)

        assert res.problems[0].value == 0, "Bad problems list read {0}".format(
            list(res.problems[:32]))
        assert res.problems[1].value != 0, "Bad problems list read {0}".format(
            list(res.problems[:32]))
        assert res.problems[2].value == 0, "Bad problems list read {0}".format(
            list(res.problems[:32]))

        disk.pd_write_blkset_info(addr, 0)

    return True


def test_case_get_jmdc(nodes):
    """ Test case for get blkset problems gen cmd
    """
    # For this test, we require a drive supporting metadata
    srv, drive = random.choice([(srv, drive)
                                for srv in nodes for drive in srv.drives.items() if drive[1]["md"] >= 8])
    clnt = random.choice(nodes)

    binje = 1
    entries_per_range = 512 / binje

    log.info('srv={0}, clnt={1}, drive={2}'.format(
        srv.hostname, clnt.hostname, drive[0]))

    # Test 3 iterations, in order to validate we cover all the variations of jentries:
    # 1. Beginning of the range; 2. End of the range; 3. Middle of the range;
    for jentry in [0, random.randint(0, entries_per_range-1), entries_per_range-1]:
        log.info("Checking jentry={0}".format(jentry))
        # Run discovery (if needed)
        log.info("Discovery")
        with clnt.discover(drive[0], srv, wait_for_serjio_timeout=10) as disk:
            log.info("Discovery success")
            # Now we can pick jmdc + jentry
            jrange = srv.get_serjio_client_jri(drive[0], clnt.hostname)

            log.info('srv={0}, clnt={1}, drive={2}, jrange={3} jentry={4}'.format(
                srv.hostname, clnt.hostname, drive[0], jrange, jentry))

            clnt_uuid = str(srv.get_serjio_client_uuid(drive[0], clnt.hostname))
            sgmt_uuid = str(srv.read_gpt(drive[0])[0])

            # Dirty the entry
            j2d = random.randint(1, 300)
            tx_id = random.randint(1, 300)
            tx_bmp = random.randint(1, 300)

            log.info("Setting jmdc")

            srv.set_jmdc_entry(drive[0], jrange, jentry, j2d, tx_id, tx_bmp)

            # Call API (Cold recovery )
            log.info("Cold recovery API call")
            res = disk.pd_jmdc_read(jrange, 1, 0)

            # Call another API (Hot recovery)
            log.info("Hot recovery API call")
            res2 = disk.gen_get_uuid_jour(clnt_uuid, sgmt_uuid).ents[jentry]

            assert res.md.arr[jentry].j2d.value == j2d and res.md.arr[jentry].tx_id.value == tx_id and res.md.arr[jentry].tx_bmp.value == tx_bmp, "MD (Cold) mismatch: ({0} {1} {2}) != ({3} {4} {5})".format(
                res.md.arr[jentry].j2d.value, res.md.arr[jentry].tx_id.value, res.md.arr[jentry].tx_bmp.value, j2d, tx_id, tx_bmp)

            assert res2.j2d.value == j2d and res2.tx_id.value == tx_id and res2.tx_bmp.value == tx_bmp, "MD (Hot) mismatch: ({0} {1} {2}) != ({3} {4} {5})".format(
                res2.j2d.value, res2.tx_id.value, res2.tx_bmp.value, j2d, tx_id, tx_bmp)

            # Revert to free
            log.info("Reverting jmdc")
            srv.set_jmdc_entry(drive[0], jrange, jentry, 0xffffffff, 1, 0)

    return True


test_cases = [(key, value) for key, value in globals().items()
              if key.startswith('test_case_')]


def str2bool(v):
    if isinstance(v, bool):
        return v
    if v.lower() in ('yes', 'true', 't', 'y', '1'):
        return True
    elif v.lower() in ('no', 'false', 'f', 'n', '0'):
        return False
    else:
        raise argparse.ArgumentTypeError('Boolean value expected.')


def unsigned_int(value):
    ivalue = int(value)
    if ivalue <= 0:
        raise argparse.ArgumentTypeError(
            "{0} is an invalid positive int value".format(value))
    return ivalue


def create_argparser():
    """ Prepare argparser
    """
    parser = argparse.ArgumentParser()
    parser.add_argument("--filter", dest="filter", type=str, default='.*')
    parser.add_argument("--unbind", dest="unbind", type=str2bool, default=True)
    parser.add_argument("--fmt", dest="fmt", type=str2bool, default=True)
    parser.add_argument("--bind", dest="bind", type=str2bool, default=True)
    parser.add_argument("--gpt", dest="gpt", type=str2bool, default=True)
    parser.add_argument("--arnics", dest="arnics", type=str2bool, default=True)
    parser.add_argument("--wipe-serjio-db", dest="wipe_serjio_db", type=str2bool, default=True)
    parser.add_argument("--iterations", dest="iterations",
                        type=unsigned_int, default=1)

    return parser


def corecomm_main(nodes, batch_args=[]):
    log.info("Starting regression test, input test set: " +
             str([node.hostname for node in nodes]))

    args = create_argparser().parse_args(batch_args)

    log.info("Setup")
    if (args.unbind):
        unbind_all_nodes(nodes)
    marry_all_nodes(nodes, gpt=args.gpt, fmt=args.fmt,
                    bind=args.bind, arnics=args.arnics,
                    wipe_serjio_db=args.wipe_serjio_db)
    log.info("Setup done")

    matching_tests = [(name, func) for (name, func) in test_cases if re.match(args.filter, name)]

    for i in range(1, args.iterations+1):
        log.info(">>>> Iteration No-{0:03} START".format(i))
        for j, (name, func) in enumerate(matching_tests):
            if re.match(args.filter, name):
                log.info('>>>> Test case {0} START'.format(name))
                assert func(nodes), "Test case {0} FAILED".format(name)
                log.info('>>>> Test case {0} PASSED'.format(name))
                log.info('>>>>>>>>>>> PROGRESS {0}% <<<<<<<<<<<<'.format(
                    int(100 * ((i-1) * len(matching_tests) + j + 1) / (args.iterations * len(matching_tests)))
                    ))
                time.sleep(5)
        log.info('>>>> Iteration No-{0:03} PASSED'.format(i))
    log.info('>>>> All test cases passed')
