#!/usr/bin/python3

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

import requests
import re
import socket
import argparse
import json

# Disable warning
from requests.packages.urllib3.exceptions import InsecureRequestWarning
requests.packages.urllib3.disable_warnings(InsecureRequestWarning)

NVMESH_CONFIG = '/etc/nvmesh/nvmesh.conf'
MGMT_SERVER_RE = r'^MANAGEMENT_SERVERS=\"(\S+):\S+\"'

parsers = {}
rest_cbs = {}


def parser(key):
    def _add_parser(func):
        parsers[key] = func
        return func
    return _add_parser


def rest_cb(key):
    def _add_cb(func):
        rest_cbs[key] = func
        return func
    return _add_cb

def find_mgmt_server_in_fp(fp):
    for line in fp:
        match = re.match(MGMT_SERVER_RE, line)
        if match:
            return match.group(1)
    raise Exception(f"No mgmt found in {NVMESH_CONFIG}")

def find_mgmt_using_ssh(host):
    try:
        import paramiko
    except ImportError:
        raise Exception("Client is specifid, but not Paramiko installed, please run pip3 install paramiko")
    c = paramiko.SSHClient()
    c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    c.connect(hostname=host)
    stdin, stdout, stderr = c.exec_command(f"cat {NVMESH_CONFIG}")
    try:
        ret = find_mgmt_server_in_fp(stdout)
        return ret
    finally:
        c.close()
        del stdin, stdout, stderr, c

def find_mgmt(client):
    if client:
        return find_mgmt_using_ssh(client)
    with open(NVMESH_CONFIG) as f:
        return find_mgmt_server_in_fp(f)


class Session(requests.Session):
    def __init__(self, prefix_url=None, *args, **kwargs):
        super(Session, self).__init__(*args, **kwargs)
        self.prefix_url = prefix_url

    def request(self, method, url, *args, **kwargs):
        url = f'{self.prefix_url}{url}'
        return super(Session, self).request(method, url, *args, **kwargs)


def get_session(addr):
    url = f'https://{addr}:4000'
    myobj = {'username': 'admin', 'password': 'admin'}

    session = Session(url)
    res = session.post('/login', json=myobj, verify=False)
    res.raise_for_status()
    return session


@parser('attach')
def attach(parser):
    parser.add_argument('volumes', nargs='+', help='volumes to attach')
    parser.add_argument('--client', '-c', help='client to attach, default is local', required=False)
    parser.add_argument('--mode', help='SHARED_READ_WRITE, SHARED_READ_ONLY, EXCLUSIVE_READ_WRITE', required=False)
    parser.add_argument('--version', help='client to attach, default is local', required=False, type=int, default=None)
    parser.add_argument('--preempt', help='client to attach, default is local', required=False, default=None, action='store_true')
    return parser


@rest_cb('attach')
def attach_cb(args, session):
    client = args.client
    if not client:
        client = socket.gethostname()

    reservation_dict = {k: v for k, v in vars(args).items() if k in ['mode', 'version',  'preempt'] and v is not None}

    if reservation_dict:
        volumes = [{"name": v, "reservation": reservation_dict} for v in args.volumes]
    else:
        volumes = [{"name": v} for v in args.volumes]
    return session.post("/clients/attach", json={'client': client, 'volumes': volumes}, verify=False)


@parser('detach')
def detach(parser):
    parser.add_argument('volumes', nargs='*', help='volumes to detach')
    parser.add_argument('--client', '-c', help='client to detach, default is local', required=False)
    parser.add_argument('--all', '-a', help='detach all', required=False, default=None, action='store_true')
    parser.add_argument('--force', '-f', help='force detach', required=False, default=None, action='store_true')
    return parser


@rest_cb('detach')
def detach_cb(args, session):
    client = args.client
    if not client:
        client = socket.gethostname()

    if args.all:
        reply = session.get(f'/clients/{client}', verify=False)
        bdevs = json.loads(reply.content)['block_devices']
        args.volumes = [b['name'] for b in bdevs]

    reservation_dict = {k: v for k, v in vars(args).items() if k in ['force'] and v is not None}
    volumes = [{"name": v, **reservation_dict} for v in args.volumes]
    return session.post("/clients/detach", json={'client': client, 'volumes': volumes}, verify=False)


def validate_rest_reply(reply):
    ret_vals = json.loads(reply.content)
    exception_str = ""

    for ret in ret_vals:
        if not ret['success']:
            exception_str += ret['error'] + "\n"

    if exception_str:
        raise Exception(exception_str)

    return ret_vals


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--mgmt', '-m', help='management address', required=False, default=None)
    subparsers = parser.add_subparsers(help='sub-command help')

    for cmd, parse_func in parsers.items():
        subparser = subparsers.add_parser(cmd)
        subparser.set_defaults(rest_cb=rest_cbs[cmd])
        parse_func(subparser)

    args = parser.parse_args()
    if args.client:
        args.client = socket.gethostbyaddr(args.client)[0]

    validate_rest_reply(args.rest_cb(args, get_session(args.mgmt or find_mgmt(args.client))))


if __name__ == "__main__":
    main()
