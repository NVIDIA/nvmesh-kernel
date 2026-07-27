#ifndef KR_INCS_ASSERT_H
#define KR_INCS_ASSERT_H
#ifdef __KERNEL__
	// Kernel already has those functions. Define as compatibility for user-space
#else
	#include <assert.h>
	#if !defined(WARN)
		#error "Each platform must define a macros: WARN(condition, fmt, ...) and optionally BUG_ON(condition), prior to including this file"
	#endif

	#include <stdlib.h>
	#include <stdio.h>
	#define oops do { fprintf(stderr, "Not a run time code\n"); abort(); } while (0)

	// Compatibility for other WARN/BUG macros which may be undefined by caller, and are based on the above
	#if !defined(BUG_ON)
		#define BUG_ON(condition) 		WARN(condition, "************************** BUG!!!! in %s() line %d, condition=%s\n", __FUNCTION__, __LINE__, #condition)
	#endif
	#if !defined(WARN_ON)
		#define WARN_ON(condition)		BUG_ON(condition)
	#endif
	#if !defined(WARN_ON_ONCE)
		#define WARN_ON_ONCE(condition)		BUG_ON(condition)
	#endif
	#if !defined(BUG)
		#define BUG()						BUG_ON(true)
	#endif
	#if !defined(WARN_ONCE)
		#define WARN_ONCE(   condition, format, ...)	WARN(condition, format, ##__VA_ARGS__)
	#endif
	// Todo move #define NTOMA_ASSERT  to here
	#include "kr_incs_compiler_types.h"
#endif // __KERNEL__
#endif
