#ifndef __SAFE_CASTING_H__
#define __SAFE_CASTING_H__

#include "common/compat/kr_incs_compiler_types.h"

enum {MAGIC_CAST_VALUE=0xABADBABE};

#define magic_ptr_cast(to_type, ptr) 					\
({														\
	__auto_type __derived = (to_type*)( (ptr) );		\
 	if (ptr) {											\
		BUG_ON(__derived->magic != MAGIC_CAST_VALUE); 	\
	}													\
	__derived;											\
})

/*
 * base/derived hierarchy in C
 *     struct shape{...};
 *     struct circle{
 *         struct shape base;
 *         ...
 *     };
 * derived_cast implements the casting from base to derived
 */

#define assert_base_and_derived_field(base_type, derived_type, field)																	\
({																																	\
	BUILD_BUG_ON_MSG(0 != offsetof(derived_type, field), "'" #field "' should be the first member");									\
	BUILD_BUG_ON_MSG(1 != __builtin_types_compatible_p(typeof(((derived_type*)(NULL))->field), base_type), "casting to a wrong type");	\
 })

#define assert_base_and_derived(base_type, derived_type) \
	assert_base_and_derived_field(base_type, derived_type, base)

#define derived_cast_field(to_type, ptr, field)			\
({														\
	__auto_type __base = (ptr);							\
	__auto_type __derived = (to_type*)( __base );		\
	assert_base_and_derived_field(typeof(*__base), to_type, field);	\
	__derived;											\
 })

#define derived_cast(to_type, ptr) \
	derived_cast_field(to_type, ptr, base)

#define magic_derived_cast(to_type, ptr) 						\
({																\
	__auto_type __safe_derived = derived_cast(to_type, ptr);	\
 	if (ptr) {													\
		BUG_ON(__safe_derived->magic != MAGIC_CAST_VALUE); 		\
	}															\
	__safe_derived;												\
})

#endif//__SAFE_CASTING_H__
