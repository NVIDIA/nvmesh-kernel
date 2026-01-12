#ifndef KR_INCS_COMPILER_TYPES_H
#define KR_INCS_COMPILER_TYPES_H
#ifdef __KERNEL__
	// Kernel already has those functions. Define as compatibility for user-space

	#if !defined(__attr_no_alignment_sanity)
		#define __attr_no_alignment_sanity
	#endif

	#define __concurrent_access
	
#else
	#define BUILD_BUG_ON(condition) ((void)sizeof(char[1 - 2*!!(condition)]))
	#define BUILD_BUG_ON_MSG(condition, msg) BUILD_BUG_ON(condition)
	#define BUILD_BUG_IF_ZERO(P) sizeof(char[-(int)!(P)])				// Alternative  (sizeof(struct { int : (-!!(e)); }))

	//linux/compiler_types.h
	#ifndef typeof		// Might be defined by SPDK or other frameworks
		#define typeof(XYZ) __typeof__(XYZ)
	#endif
	#ifndef __same_type
		#define __same_type(a, b) __builtin_types_compatible_p(typeof(a), typeof(b))
	#endif
	#ifndef __must_be_array
		#define __must_be_array(a)	BUILD_BUG_IF_ZERO(!__same_type((a), &(a)[0]))
	#endif
	#if !defined(ARRAY_SIZE)
		#ifdef TOMA
			#define ARRAY_SIZE(arr) ((int)sizeof(arr) / (int)sizeof((arr)[0]) + (int)__must_be_array(arr))
		#else
			#define ARRAY_SIZE(arr) (     sizeof(arr) /      sizeof((arr)[0]) + __must_be_array(arr))
		#endif
	#endif

	// container_of - cast a member of a structure out to the containing structure. @ptr: the pointer to the member. @type: the type of the container struct this is embedded in. @member:	the name of the member within the struct.
	#if !defined(container_of)
		#define container_of(ptr, type, member) ({const typeof( ((type *)0)->member ) *__mptr = (ptr); (type *)( (char *)__mptr - offsetof(type,member) );})
	#endif
	#ifndef offsetof
		#define offsetof(type, member)  __builtin_offsetof (type, member)
	#endif

	// include/asm-generic/bug.h
	#ifndef __FUNCTION__
		#define __FUNCTION__ (__func__)				/* Trap pasters of __FUNCTION__ at compile-time */
	#endif

	#if !defined(__attr_no_alignment_sanity)
		#define __attr_no_alignment_sanity __attribute__((no_sanitize("alignment")))
	#endif

	#if !defined(__STDC_NO_ATOMICS__) && defined(__SANITIZE_THREAD__)
		#define __concurrent_access _Atomic
		#define __concurrent_store(p, v) atomic_store(&(p), v)
		#define __concurrent_load(p)    atomic_load(&(p))
	#else 
		#define __concurrent_access
		#define __concurrent_store(p, v) ((p) = (v))
		#define __concurrent_load(p)    (p)
	#endif

#endif // __KERNEL__

#define NVMESH_USED           __attribute__((__used__))
#define NVMESH_SECTION(name)  __attribute__((__section__(name)))
#define NVMESH_ALIGNED(x)     __attribute__((__aligned__(x)))


#define const_cast_ptr(type, ptr) 																								\
({																																\
	BUILD_BUG_ON_MSG(!__same_type(const type, typeof(ptr)), "the const_cast_pre should be use on the same unqualified type");	\
	(type)(ptr);																												\
})
#endif
