#!/usr/bin/env python
# PYTHON_ARGCOMPLETE_OK

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

import argparse
import atexit
import json
import logging
import os
import subprocess
import sys
import time
import uuid
import shlex

import IPython
import rpyc

from corecomm_controller_infra import CorecommServerConnection, install_server, SERVER_INSTALL_DIR
from corecomm_controller_infra import marry_all_nodes as marry_all_nodes_, set_binje as set_binje_
from corecomm_controller_infra import unbind_all_nodes as unbind_all_nodes_
from corecomm_controller_infra import node_to_var_name, get_total_drives_list

log = logging.getLogger('corecomm-controller')


def argcomplete_if_possible(parser):
    """ Try initialize argcomplete if available
    """
    from importlib import import_module

    try:
        argcomplete = import_module("argcomplete")
    except:
        pass  # Too bad, but we can live with that
    else:
        argcomplete.autocomplete(parser)

    return parser


def shell_args(inp):
    if not inp or inp[0] != '[' or inp[-1] != ']':
        raise argparse.ArgumentTypeError(
            'Malformed filter args: {0}'.format(inp))
    return shlex.split(inp[1:-1])


def create_argparser():
    """ Prepare argparser
    """
    parser = argparse.ArgumentParser()

    parser.add_argument(
        "nodes", action="store", nargs="+", type=str, help="List of nodes to connect to"
    )
    parser.add_argument("--batches", dest="batches", action="store", nargs="*", type=str,
                        help="List of batch files to execute. If not specified will spawn into the interractive mode.")

    parser.add_argument("--verbose", dest="verbose", action="store_true")

    parser.add_argument("--wipe", dest="wipe", action="store_true")
    
    parser.add_argument("--server-install-dir", dest="server_install_dir", action="store", default=SERVER_INSTALL_DIR)

    parser.add_argument("--batch-args", dest="batch_args",
                        type=shell_args, default=shell_args('[]'))

    parser.add_argument("--dbg", dest="dbg", type=str, default='INFO', choices=['CRITICAL',
                                                                                'ERROR',
                                                                                'WARNING',
                                                                                'INFO',
                                                                                'DEBUG',
                                                                                'NOTSET'])

    parser.add_argument("--server-dbg", dest="server_dbg", type=str, default='INFO', choices=['CRITICAL',
                                                                                              'ERROR',
                                                                                              'WARNING',
                                                                                              'INFO',
                                                                                              'DEBUG',
                                                                                              'NOTSET'])

    return argcomplete_if_possible(parser)


input_nodes = []


def marry_all_nodes(*args, **kwargs):
    marry_all_nodes_(input_nodes, *args, **kwargs)

def set_binje(binje):
    return set_binje_(binje, input_nodes)


def unbind_all_nodes(*args, **kwargs):
    unbind_all_nodes_(input_nodes, *args, **kwargs)


def run_batch(batch, nodes, args):
    import imp

    try:
        batch_module = imp.load_source('__batch__', batch)
        batch_module.corecomm_main(nodes, args)
    except Exception as e:
        raise  # TODO: Do somethig with `e`


def main():

    parser = create_argparser()
    args = parser.parse_args()
    logging.basicConfig(stream=sys.stdout,
                        level=args.dbg,
                        format="%(levelname)7s: %(name)s: %(message)s")

    for node in args.nodes:
        log.debug("Installing node {0}".format(node))
        install_server(node, server_install_dir=args.server_install_dir, wipe=args.wipe)

    global input_nodes
    servers = []
    for node in args.nodes:
        log.debug("Starting server {0}".format(node))
        exec('{0} = CorecommServerConnection(node, server_install_dir="{1}", dbg=args.server_dbg); input_nodes.append({0})'.format(
            node_to_var_name(node),
            args.server_install_dir))

    if args.batches:
        for batch in args.batches:
            run_batch(batch, input_nodes, args.batch_args)
    else:
        IPython.embed()  # Go to interractive shell
    return 0


if __name__ == "__main__":
    exit(main())
