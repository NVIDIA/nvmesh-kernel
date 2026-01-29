# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0

PWD := $(shell pwd)
KDIR ?= ~/development/kernels/linux

default:
	${MAKE} -C ${KDIR} M=${PWD}

clean:
	${MAKE} -C ${KDIR} M=${PWD} clean

