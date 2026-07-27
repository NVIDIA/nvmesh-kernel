#ifndef NVMEIB_CPU_MASKS_H
#define NVMEIB_CPU_MASKS_H

#include "common/compat/kr_incs_bit_ops.h"

#define NVMEIB_CPU_MASK_MAX_CPUS 128

struct nvmeib_cpu_mask {
	DECLARE_BITMAP(cpus, NVMEIB_CPU_MASK_MAX_CPUS);
};

#define NVMEIB_CPU_MASK_IS_EMPTY(_mask) bitmap_empty((_mask).cpus, NVMEIB_CPU_MASK_MAX_CPUS)
#define NVMEIB_CPU_MASK_CLEAR(_mask) bitmap_zero((_mask).cpus, NVMEIB_CPU_MASK_MAX_CPUS)
#define NVMEIB_CPU_MASK_EQ(_mask1, _mask2) bitmap_equal((_mask1).cpus, (_mask2).cpus, NVMEIB_CPU_MASK_MAX_CPUS)
#define NVMEIB_CPU_MASK_FOR_EACH_CPU(_cpu, _mask) for_each_set_bit(_cpu, (_mask).cpus, NVMEIB_CPU_MASK_MAX_CPUS)
#define NVMEIB_CPU_MASK_FOR_ALL_CPU(_cpu) for (_cpu = 0; _cpu < NVMEIB_CPU_MASK_MAX_CPUS; _cpu++)
#define NVMEIB_CPU_MASK_COPY(_dstmask, _srcmask) bitmap_copy((_dstmask).cpus, (_srcmask).cpus, NVMEIB_CPU_MASK_MAX_CPUS)
#define NVMEIB_CPU_MASK_BITS(_mask) ((_mask).cpus)
#define NVMEIB_CPU_MASK_WEIGHT(_mask) bitmap_weight((_mask).cpus, NVMEIB_CPU_MASK_MAX_CPUS)

#define NVMEIB_CPU_MASK_OR(_dstmask, _srcmask1, _srcmask2) do {\
	bitmap_or((_dstmask).cpus, (_srcmask1).cpus,\
		(_srcmask2).cpus, NVMEIB_CPU_MASK_MAX_CPUS);\
} while(0)

#define NVMEIB_CPU_MASK_AND_NOT(_dstmask, _srcmask1, _srcmask2) ({\
	bool ret = bitmap_andnot((_dstmask).cpus, (_srcmask1).cpus,\
		(_srcmask2).cpus, NVMEIB_CPU_MASK_MAX_CPUS);\
	ret;\
})

#define NVMEIB_CPU_MASK_SET_CPU(_cpu, _mask) do {\
	BUG_ON((_cpu) < 0 || (_cpu) >= NVMEIB_CPU_MASK_MAX_CPUS);\
	set_bit((_cpu), (_mask).cpus);\
} while(0)

#define NVMEIB_CPU_MASK_CLEAR_CPU(_cpu, _mask) do {\
	BUG_ON((_cpu) < 0 || (_cpu) >= NVMEIB_CPU_MASK_MAX_CPUS);\
	clear_bit((_cpu), (_mask).cpus);\
} while(0)

#define NVMEIB_CPU_MASK_TEST_CPU(_cpu, _mask) ({\
	bool _test;\
	BUG_ON((_cpu) < 0 || (_cpu) >= NVMEIB_CPU_MASK_MAX_CPUS);\
	_test = test_bit((_cpu), (_mask).cpus);\
	_test;\
})

#define NVMEIB_CPU_MASK_TEST_AND_SET_CPU(_cpu, _mask) ({\
	bool _test;\
	BUG_ON((_cpu) < 0 || (_cpu) >= NVMEIB_CPU_MASK_MAX_CPUS);\
	_test = test_and_set_bit((_cpu), (_mask).cpus);\
	_test;\
})

#define NVMEIB_CPU_MASK_TEST_AND_CLEAR_CPU(_cpu, _mask) ({\
	bool _test;\
	BUG_ON((_cpu) < 0 || (_cpu) >= NVMEIB_CPU_MASK_MAX_CPUS);\
	_test = test_and_clear_bit((_cpu), (_mask).cpus);\
	_test;\
})

#define NVMEIB_CPU_MASK_NEXT(_n, _mask) ({\
	unsigned ret;\
	if ((_n) != -1) {\
		BUG_ON((_n) < 0 || (_n) >= NVMEIB_CPU_MASK_MAX_CPUS);\
	}\
	ret = find_next_bit((_mask).cpus, NVMEIB_CPU_MASK_MAX_CPUS, (_n) + 1);\
	ret;\
})

#if !defined(__KERNEL__) || !KS_HAS_FIND_NTH_BIT
inline static unsigned long find_nth_bit(const unsigned long *addr, 
					 unsigned long size, 
					 unsigned long n)
{
	unsigned long bit;
	unsigned long count = 0;

	for_each_set_bit(bit, addr, size) {
		if (count == n)
			return bit;
		count++;
	}

	return size;
}
#endif

#define NVMEIB_CPU_MASK_FIND_NTH_BIT(_n, _mask) find_nth_bit((_mask).cpus, NVMEIB_CPU_MASK_MAX_CPUS, _n)

#define NVMEIB_CPU_MASK_PR_FMT() "%128pbl"
#define NVMEIB_CPU_MASK_PR_ARGS(_mask) NVMEIB_CPU_MASK_BITS(_mask)

struct nvmeib_cpu_mask_info {
	struct nvmeib_cpu_mask mask;
	u64 gen;	// Mask generation number (if mask is not empty, zero otherwise)
};

#endif//NVMEIB_CPU_MASKS_H
