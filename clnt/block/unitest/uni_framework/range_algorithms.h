/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#pragma once
#include "kr_incs.h"


#define range_find_if(begin, end, what) 		\
	({											\
		__typeof__(begin) _iterator_ = (begin);	\
		for(; _iterator_ < (end); ++_iterator_){\
			if (!!(what(_iterator_))){			\
				break;							\
			}									\
		}										\
		if (_iterator_ == (end)){				\
			_iterator_ = 0;						\
		}										\
		_iterator_;								\
	})


#define array_find_if(where, what) 				\
	range_find_if( &(where)[0], &(where)[ARRAY_SIZE((where))], what)


//cmp behaves like memcmp cmp(x, y):  -1 <=> (x<y); 0 <=> (x==y); 1 <=> (x>y)
#define range_min(begin, end, cmp)				\
	({											\
		__typeof__(begin) _min_ = (begin);		\
		__typeof__(begin) _iterator_ = (begin);	\
		for(; _iterator_ < (end); ++_iterator_){\
			if ((cmp(_iterator_, _min_)) < 0){	\
				_min_ = _iterator_;				\
			}									\
		}										\
		_min_;									\
	})

#define array_min(where, cmp) \
	range_min( &(where)[0], &(where)[ARRAY_SIZE((where))], cmp)


#define range_count_if(begin, end, what) 											\
	({																				\
		size_t _count_ = 0;															\
		for(__typeof__(begin) _iterator_ = begin; _iterator_ < end ; ++_iterator_){	\
			if (!!(what(_iterator_))){												\
				_count_++;															\
			}																		\
		}																			\
		_count_;																	\
	})


#define array_count_if(where, what) 				\
	range_count_if( &where[0], &where[ARRAY_SIZE(where)], what)


#define range_fill(begin, end, value)                                                   \
    ({																				    \
		for(__typeof__(begin) _iterator_ = (begin); _iterator_ < (end) ; ++_iterator_){	\
            *_iterator_ = (value);                                                      \
		}																			    \
	})


#define array_fill(where, value)													\
	range_fill( &(where)[0], &(where)[ARRAY_SIZE((where))], (value))


#define range_copy(begin, end, output)												\
    ({																				\
		for(__typeof__(begin) _iterator_ = begin; _iterator_ < end ; ++_iterator_){	\
            output[_iterator_-begin] = *_iterator_;									\
		}																			\
	})

#define array_copy(dst, src)								\
	({														\
		BUG_ON(ARRAY_SIZE(dst) != ARRAY_SIZE(src));			\
		range_copy(&src[0], &src[ARRAY_SIZE(src)], dst);	\
	})

#define array_foreach(item, array)      for (typeof(&(array)[0]) (item) = &(array)[0]; (item) < &(array)[ARRAY_SIZE(array)]; ++(item))
#define range_foreach(item, begin, end) for (typeof(begin)       (item) = (begin);     (item) < (end);                       ++(item))

#define range_shuffle(begin, end)						          \
	({                                                            \
		for (u64 _i = (u64)((end) - (begin) - 1); _i > 0; _i--) {  /* Make it fully random be setting in step i a random selection of remaining 0..i-1 elements. */ \
			u64 _j = rand() % (_i+1);                             \
			swap((begin)[_i], (begin)[_j]);                       \
		}                                                         \
    })

#define array_shuffle(array)                                      \
	({                                                            \
		range_shuffle((array), &(array)[0] + ARRAY_SIZE((array)));    \
	})
