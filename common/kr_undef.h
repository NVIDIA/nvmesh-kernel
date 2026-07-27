#ifndef KR_UNDEF_H
#define KR_UNDEF_H

/* always call the function and possible wrapper functions */
#ifdef schedule_work
#	undef schedule_work
#endif

#ifdef schedule_delayed_work
#	undef schedule_delayed_work
#endif

#include "compat/kr_incs_module.h"

#endif
