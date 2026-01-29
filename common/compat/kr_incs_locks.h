/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef KR_INCS_LOCKS_H
#define KR_INCS_LOCKS_H

// TBD: Move spinlock, mutex, rwlock, etc. from simulator kr_incs.h to here.

#if defined(__KERNEL__)

	#include <linux/seqlock.h>

#elif defined(UM_APP)
	
	/* seqlock is not relevant for UM. It is designed to synchronise
	 * counters between CPUs */
	typedef int seqcount_t;
	#define write_seqcount_begin(_s) (void)_s
	#define write_seqcount_end(_s) (void)_s
	#define read_seqcount_begin(_s) ({\
		       void(_s);\
		       1;\
		})
	#define read_seqcount_retry(_s, _seq) ({\
			(void)_s;\
			(void)_seq;\
		       false;\
		})

#elif defined(BLKDEV_SIMULATOR) && BLKDEV_SIMULATOR
	// Seqcount synchronisation mechanism /linux/seqlock.h
	// Implemented in simulator using spinlock
	#define seqcount_t spinlock_t
	#define write_seqcount_begin(_s) spin_lock(&_s)
	#define write_seqcount_end(_s) spin_unlock(&_s)
	#define read_seqcount_begin(_s) ({\
		       spin_lock(&_s);\
		       1;\
		})
	#define read_seqcount_retry(_s, _seq) ({\
		       (void)_seq;\
		       spin_unlock(&_s);\
		       false;\
		})
#else

#endif


#endif//KR_INCS_LOCKS_H
