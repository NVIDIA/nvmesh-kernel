#!/bin/bash

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

DIR=$(dirname $0)
lttng create ephemeral1 --snapshot
lttng create ephemeral2 --snapshot
lttng create nvmesh
insmod $DIR/../zzzlttng/lttng-probe-nvmeibc.ko
insmod $DIR/../zzzlttng/lttng-probe-nvmeibs.ko
lttng enable-channel -s ephemeral1 -k eph --subbuf-size=16k --num-subbuf=4
lttng enable-channel -s ephemeral2 -k eph --subbuf-size=16k --num-subbuf=4
lttng enable-event -s ephemeral1 -k -c eph nvmeibs_EPH\*
lttng enable-event -s ephemeral1 -k -c eph nvmeibc_EPH\*
lttng enable-event -s ephemeral2 -k -c eph nvmeibs_EPH\*
lttng enable-event -s ephemeral2 -k -c eph nvmeibc_EPH\*
lttng enable-channel -s nvmesh -k short --tracefile-size=1M
lttng enable-channel -s nvmesh -k long --tracefile-size=1G
lttng enable-channel -s nvmesh -u toma --tracefile-size=100M
lttng enable-event -s nvmesh -k -c short nvmeibc_SHORT\*
lttng enable-event -s nvmesh -k -c short nvmeibs_SHORT\*
lttng enable-event -s nvmesh -k -c long nvmeibc_LONG\*
lttng enable-event -s nvmesh -k -c long nvmeibs_LONG\*
lttng enable-event -s nvmesh -u -c toma nvmeibt:\*
lttng start nvmesh
lttng start ephemeral1
