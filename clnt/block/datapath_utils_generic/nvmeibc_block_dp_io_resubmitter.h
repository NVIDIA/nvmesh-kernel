/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_DP_RESUBMITTER_H
#define NVMEIBC_DP_RESUBMITTER_H
/* Class which resubmits and retries IO tasks, when they cannot be executed now
   1. it can retry the entire operation (assuming its previous execution was
      completed).
   2. Retry a specific lock which one IO passes to another (locks elevator)
*/
#include "kr_incs.h"

struct nvmeibc_io_resubmitter {
	struct list_head list_paused_ops;		// Link List of operations which could not be executed since disk was in error state (like disconnected). We will try to resubmit them. The head is the next operation to be resubmitted and if failed again will be inserted to tail and next operation will be retried.
	unsigned int n_reads_ops;				//number of read operations in list_paused_ops
	struct task_struct *thread;				// Assist thread which accesses the above list and executes the operations later (when hopefully disk is OK)
	spinlock_t lock;	  					// Lock to access resubmit related activites (the fields below)
	wait_queue_head_t wait_queue;   		// Wait queue on which the thread sleeps, waken up by interrupt when disk reconnects.
};
int  nvmeibc_io_resubmitter_init(   struct nvmeibc_io_resubmitter*);
int  nvmeibc_io_resubmitter_start(  struct nvmeibc_io_resubmitter*, void* dev);	// Starts resubmition service. Carefully use this function (launch it before first IO arrives).
void nvmeibc_io_resubmitter_destroy(struct nvmeibc_io_resubmitter*);

static inline void nvmeibc_io_resubmitter_wakeup(struct nvmeibc_io_resubmitter* resub){
	wake_up(&(resub->wait_queue));
}

struct operation;
int  nvmeibc_io_resubmitter_submit_op( struct operation *o); // Submit newly allocated operation, whose execution was not started
int  nvmeibc_io_resubmitter_retry_op(  struct operation *o); // Same as above but clean up previous execution state before submitting

#endif  // H beginning

