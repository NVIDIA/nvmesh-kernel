#ifndef DP_TOPOLOGY_TRAITS_H
#define DP_TOPOLOGY_TRAITS_H

#include "common/compat/kr_incs_types.h"
typedef u16 __bitwise sgmnts_bmp_t;
struct dp_topology_traits {
	u8 n_parities;
	u8 n_degraded;

	sgmnts_bmp_t dbits_on;   // write turns ON dirty bits for these (== dead)
	sgmnts_bmp_t dbits_off;  // full-blockset write turns OFF dirty bits (== W| W-)
	sgmnts_bmp_t wm;         // W- only (W_IS_DIRTY — convict candidates)
};

#endif // DP_TOPOLOGY_TRAITS_H
