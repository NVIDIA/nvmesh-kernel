/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef KR_INCS_ATOMICS_H
#define KR_INCS_ATOMICS_H
#ifdef __KERNEL__
	// Kernel already has those functions. Define as compatibility for user-space
#else
	// https://gcc.gnu.org/onlinedocs/gcc/_005f_005fatomic-Builtins.html
	typedef struct { long long c; } __attribute__((aligned(sizeof(long long)))) atomic64_t, atomic_long_t; 	// c - counter. Artificial struct to support {0} initialization
	typedef struct { int       c; } __attribute__((aligned(sizeof(int))))       atomic_t;					// c - counter, force alignment to prevent a bug of using atomic fields in packed struct, splitting atomic to cachelines

	// 32[bit]
	#define ATOMIC_INIT(i)		{i}
	#define ATOMIC64_INIT(i)	{i}
	static inline void atomic_set(      atomic_t *v, int i) {        __atomic_store_n(  &v->c, i, __ATOMIC_SEQ_CST); }
	static inline int atomic_read(const atomic_t *v       ) { return __atomic_load_n(   &v->c,    __ATOMIC_SEQ_CST); }
	static inline int atomic_dec_return(atomic_t *v) {        return __atomic_sub_fetch(&v->c, 1, __ATOMIC_SEQ_CST); } // (v->c)--;  return v->c;
	static inline int atomic_inc_return(atomic_t *v) {        return __atomic_add_fetch(&v->c, 1, __ATOMIC_SEQ_CST); } // (v->c)++;  return v->c;
	static inline int atomic_sub_return(  int x, atomic_t *v) { return __atomic_sub_fetch(&v->c, x, __ATOMIC_SEQ_CST); } // (v->c)-=x; return v->c;
	static inline int atomic_add_return(  int x, atomic_t *v) { return __atomic_add_fetch(&v->c, x, __ATOMIC_SEQ_CST); } // (v->c)+=x; return v->c;
	static inline void atomic_sub(        int x, atomic_t *v) { (void)atomic_sub_return(x, v); } // (v->c)-=x;
	static inline void atomic_add(        int x, atomic_t *v) { (void)atomic_add_return(x, v); } // (v->c)+=x;
	static inline void atomic_inc(               atomic_t *v) { (void)atomic_inc_return(v); }
	static inline void atomic_dec(               atomic_t *v) { (void)atomic_dec_return(v); }
	static inline int atomic_dec_and_test(       atomic_t *v) { return atomic_sub_return(1, v) == 0; } // (v->c)--;   return (v->c==0);
	static inline int atomic_sub_and_test(int x, atomic_t *v) { return atomic_sub_return(x, v) == 0; } // (v->c)-=x;  return (v->c==0);

	static inline int atomic_xchg(       atomic_t *v, int n) { return __atomic_exchange_n(&v->c, n, __ATOMIC_SEQ_CST); } // x = v->c; v->c = n; return x
	static inline int atomic_cmpxchg(atomic_t *v, int o, int n) {
		int tmp = o;
		(void)__atomic_compare_exchange_n(&v->c, &tmp, n, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
		return tmp;
	} // const int prev = v->c; if (v->c==o) v->c = n; return prev; }

	// 64[bit]
	static inline void atomic64_set(atomic64_t *v, long long i) {       __atomic_store_n(  &v->c, i, __ATOMIC_SEQ_CST); }
	static inline long long atomic64_read(const atomic64_t *v) { return __atomic_load_n(   &v->c,    __ATOMIC_SEQ_CST); }
	static inline long long atomic64_dec_return(atomic64_t *v) { return __atomic_sub_fetch(&v->c, 1, __ATOMIC_SEQ_CST); } //  v->c--;   return v->c;
	static inline long long atomic64_inc_return(atomic64_t *v) { return __atomic_add_fetch(&v->c, 1, __ATOMIC_SEQ_CST);	} // (v->c)++;  return v->c;
	static inline void atomic64_inc(            atomic64_t *v) { (void)atomic64_inc_return(v); }
	static inline void atomic64_dec(            atomic64_t *v) { (void)atomic64_dec_return(v); }
	static inline void atomic64_add(long long x, atomic64_t *v) { (void)__atomic_add_fetch(&v->c, x, __ATOMIC_SEQ_CST); }
	static inline long long atomic64_sub_return(long long x, atomic64_t *v) { return __atomic_sub_fetch(&v->c, x, __ATOMIC_SEQ_CST); }

	// Atomically adds @a to @v, so long as @v was not already @u.
	static inline int __atomic_add_unless(atomic_t *v, int a, int u){
		int old, c = atomic_read(v);
		while (c != u && ((old = atomic_cmpxchg(v, c, c + a)) != c))
			c = old;
		return c;
	}
	static inline int atomic_add_unless(atomic_t *v, int a, int u){ return __atomic_add_unless(v, a, u) != u; }
	#define atomic_inc_not_zero(v)		atomic_add_unless((v), 1, 0)							// Atomically increments @v by 1, so long as @v is non-zero.

	static inline int atomic_dec_if_positive(atomic_t *v) {
		int c, old, dec;
		c = atomic_read(v);
		for (;;) {
			dec = c - 1;
			if (unlikely(dec < 0))
				break;
			old = atomic_cmpxchg((v), c, dec);
			if (likely(old == c))
				break;
			c = old;
		}
		return dec;
	}
#endif // __KERNEL__
#endif

