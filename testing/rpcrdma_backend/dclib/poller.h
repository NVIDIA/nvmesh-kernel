/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#ifndef POLLER_H
#define POLLER_H

#include "xkr_incs.h"

int intr_pollers_start(void);
void intr_pollers_stop(void);
void intr_poll_init(struct irq_poll *p, int weight, irq_poll_fn *poll_fn);
void intr_poll_sched(struct irq_poll *p);
void intr_poll_complete(struct irq_poll *p);
bool intr_poll_is_sched(struct irq_poll *p);

#endif
