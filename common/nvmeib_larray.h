#ifndef NVMEIB_LARRAY_H_INCLUDED
#define NVMEIB_LARRAY_H_INCLUDED

#define LACONCAT1(x, y) x ## y
#define LACONCAT(x, y) LACONCAT1(x, y)

#define LARRAY_DEFINE(name, type, bits) \
	struct larray_ ## _ ## name { \
		type a[1 << (bits)]; \
		int next; \
	} name

#define LARRAY_DEFINE_SIZE(name, type, size) \
	struct larray_ ## _ ## name { \
		type a[size]; \
		int next; \
	} name

#define LARRAY_INIT(_a) \
	do { \
		int LACONCAT(i, __LINE__); \
		memset((_a).a, 0, sizeof((_a).a)); \
		BUILD_BUG_ON(sizeof((_a).a[0]) < sizeof(int));\
		for (LACONCAT(i, __LINE__) = 0; \
				LACONCAT(i, __LINE__) < (int)ARRAY_SIZE((_a).a) - 1; \
				++LACONCAT(i, __LINE__)) \
			*((int *)&((_a).a[LACONCAT(i, __LINE__)])) = \
				LACONCAT(i, __LINE__) + 1; \
		*((int *)&((_a).a[ARRAY_SIZE((_a).a) - 1])) = -1; \
		(_a).next = 0; \
	} while (false)

#define LARRAY_GET(_a) \
	({\
		int LACONCAT(index, __LINE__) = (_a).next; \
		if (likely((_a).next != -1)) (_a).next = \
			*((int *)&((_a).a[(_a).next])); \
		LACONCAT(index, __LINE__); \
	})

#define LARRAY_PUT(_a, _index) \
	do { \
		*((int *)&((_a).a[_index])) = (_a).next; \
		(_a).next = _index; \
	} while (false);

#endif 
/*
#ifndef LARRAY_H_INCLUDED
#define LARRAY_H_INCLUDED

#define LACONCAT1(x, y) x ## y
#define LACONCAT(x, y) LACONCAT1(x, y)

// fixed size list in an array
#define LARRAY_DEFINE(name, type, bits) \
	struct larray_ ## _ ## name { \
		type a[1 << (bits)]; \
		int next; \
	} name

#define LARRAY_INIT(_a) \
	do { \
		int i; \
		memset(_a.a, 0, sizeof(_a.a)); \
		BUILD_BUG_ON(sizeof(_a.a[0]) < sizeof(int));\
		for (i = 0; i < ARRAY_SIZE(_a.a) - 1; ++i) \
			*((int *)&(_a.a[i])) = i + 1; \
		*((int *)&(_a.a[ARRAY_SIZE(_a.a) - 1])) = -1; \
		_a.next = 0; \
	} while (false)

#define LARRAY_GET(_a) \
	({\
		int index = _a.next; \
		if (likely(_a.next != -1)) _a.next = *((int *)&(_a.a[_a.next])); \
		index; \
	})

#define LARRAY_PUT(_a, _index) \
	do { \
		*((int *)&(_a.a[_index]) = _a.index; \
		_a.index = _index; \
	} while (false);

#endif
*/ 
