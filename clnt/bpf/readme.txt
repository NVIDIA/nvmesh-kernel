# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0

Work in progress; a taste of what could be done with eBPF:

root@nvme193:~# bpftrace io.bt
Attaching 6 probes...
22:12:41:901540 nvmeiba_bdev_open command=dd pid=1998458 tid=1998458 gendisk=0xffff8f1d3470b800 (major=251,minor=256) mode=0x2 pgid=0x1e7e7a name=nvmesh/vol000 latency=0.029(ms)
22:12:45:650340 nvmeiba_bdev_close command=dd pid=1998458 tid=1998458 gendisk=0xffff8f1d3470b800 mode=0x0 pgid=0x1e7e7a latency=0.021(ms)
22:12:45:651016 nvmeiba_bdev_open command=systemd-udevd pid=800 tid=800 gendisk=0xffff8f1d3470b800 (major=251,minor=256) mode=0x9 pgid=0x320 name=nvmesh/vol000 latency=0.033(ms)
22:12:45:651064 nvmeiba_bdev_open command=systemd-udevd pid=800 tid=800 gendisk=0xffff8f1d3470b800 (major=251,minor=256) mode=0x49 pgid=0x320 name=nvmesh/vol000 latency=0.015(ms)
22:12:45:651579 nvmeiba_bdev_close command=systemd-udevd pid=800 tid=800 gendisk=0xffff8f1d3470b800 mode=0x0 pgid=0x320 latency=0.030(ms)
22:12:45:651694 nvmeiba_bdev_close command=systemd-udevd pid=800 tid=800 gendisk=0xffff8f1d3470b800 mode=0x0 pgid=0x320 latency=0.024(ms)
22:12:45:656511 nvmeiba_bdev_open command=(udev-worker) pid=1998460 tid=1998460 gendisk=0xffff8f1d3470b800 (major=251,minor=256) mode=0x9 pgid=0x1e7e7c name=nvmesh/vol000 latency=0.028(ms)
22:12:46:459846 nvmeiba_bdev_open command=(udev-worker) pid=1998460 tid=1998460 gendisk=0xffff8f1d3470b800 (major=251,minor=256) mode=0x9 pgid=0x1e7e7c name=nvmesh/vol000 latency=0.026(ms)
22:12:46:465396 nvmeiba_bdev_close command=(udev-worker) pid=1998460 tid=1998460 gendisk=0xffff8f1d3470b800 mode=0x0 pgid=0x1e7e7c latency=0.027(ms)
22:12:46:467601 nvmeiba_bdev_open command=probe-bcache pid=1998472 tid=1998472 gendisk=0xffff8f1d3470b800 (major=251,minor=256) mode=0x1 pgid=0x1e7e88 name=nvmesh/vol000 latency=0.020(ms)
22:12:46:468918 nvmeiba_bdev_close command=probe-bcache pid=1998472 tid=1998472 gendisk=0xffff8f1d3470b800 mode=0x0 pgid=0x1e7e88 latency=0.024(ms)
22:12:46:472329 nvmeiba_bdev_close command=(udev-worker) pid=1998460 tid=1998460 gendisk=0xffff8f1d3470b800 mode=0x0 pgid=0x1e7e7c latency=0.022(ms)
