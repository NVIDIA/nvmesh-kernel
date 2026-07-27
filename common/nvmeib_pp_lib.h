#ifndef NVMEIB_PP_LIB
#define NVMEIB_PP_LIB

/* Undef, not to run the test suite */
#define NV_PP_TEST 1

/* NVMESH Preprocessing Library */

/* Maximum Value that most operations work up until */
#define NV_PP_MAX    20

#define NV_PP_MAX_SEQ  0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19
#define NV_PP_MAX_RSEQ 19,18,17,16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0

/* Generate sequence 0,1,...(x-1) */
#define NV_PP_SEQ(x) NV_PP_DEFER(_NV_PP_SEQ(x))
#define _NV_PP_SEQ(x) NV_PP_TAKE(x,NV_PP_MAX_SEQ)

/* Generate reverse sequence x-1,x-2,...,1,0 */
#define NV_PP_RSEQ(x) NV_PP_DEFER(_NV_PP_RSEQ(x))
#define _NV_PP_RSEQ(x) NV_PP_EAT(NV_PP_SUB(NV_PP_MAX,x),NV_PP_MAX_RSEQ)

/* Used to force an additional pass */
#define NV_PP_EMPTY(...)
#define NV_PP_DEFER(x) x NV_PP_EMPTY()

/* Used for stripping parentheses to pass comma-seperated list without messing up __VA_ARGS__:
 * To strip parentheses from x
 * NV_PP_DEFER(NV_PP_VARGS x)
 */


#define NV_PP_VARGS(...) __VA_ARGS__


#define NV_PP_EVAL(...) __VA_ARGS__

/* Stringify x */
#define NV_PP_STR(...) NV_PP_DEFER(_NV_PP_STR(__VA_ARGS__))
#define _NV_PP_STR(...) #__VA_ARGS__

#define NV_PP_CAT(x, ...)	NV_PP_PRIM_CAT(x, __VA_ARGS__)
#define NV_PP_PRIM_CAT(x, ...)	x ## __VA_ARGS__

/* Concatenate n arguments together */
#define NV_PP_N_CAT(...) NV_PP_DEFER(_NV_PP_N_CAT(__VA_ARGS__))
#define _NV_PP_N_CAT(...) __NV_PP_N_CAT(NV_PP_LIST_SZ(__VA_ARGS__), __VA_ARGS__)
#define __NV_PP_N_CAT(n, ...) NV_PP_N_CAT_##n(__VA_ARGS__)
#define NV_PP_N_CAT_0(...) 		__VA_ARGS__
#define NV_PP_N_CAT_1(x, ...) x ## __VA_ARGS__
#define NV_PP_N_CAT_2(x, ...) x ## NV_PP_N_CAT_1(__VA_ARGS__)
#define NV_PP_N_CAT_3(x, ...) x ## NV_PP_N_CAT_2(__VA_ARGS__)
#define NV_PP_N_CAT_4(x, ...) x ## NV_PP_N_CAT_3(__VA_ARGS__)
#define NV_PP_N_CAT_5(x, ...) x ## NV_PP_N_CAT_4(__VA_ARGS__)
#define NV_PP_N_CAT_6(x, ...) x ## NV_PP_N_CAT_5(__VA_ARGS__)
#define NV_PP_N_CAT_7(x, ...) x ## NV_PP_N_CAT_6(__VA_ARGS__)
#define NV_PP_N_CAT_8(x, ...) x ## NV_PP_N_CAT_7(__VA_ARGS__)
#define NV_PP_N_CAT_9(x, ...) x ## NV_PP_N_CAT_8(__VA_ARGS__)
#define NV_PP_N_CAT_10(x, ...) x ## NV_PP_N_CAT_9(__VA_ARGS__)
#define NV_PP_N_CAT_11(x, ...) x ## NV_PP_N_CAT_10(__VA_ARGS__)
#define NV_PP_N_CAT_12(x, ...) x ## NV_PP_N_CAT_11(__VA_ARGS__)
#define NV_PP_N_CAT_13(x, ...) x ## NV_PP_N_CAT_12(__VA_ARGS__)
#define NV_PP_N_CAT_14(x, ...) x ## NV_PP_N_CAT_13(__VA_ARGS__)
#define NV_PP_N_CAT_15(x, ...) x ## NV_PP_N_CAT_14(__VA_ARGS__)
#define NV_PP_N_CAT_16(x, ...) x ## NV_PP_N_CAT_15(__VA_ARGS__)
#define NV_PP_N_CAT_17(x, ...) x ## NV_PP_N_CAT_16(__VA_ARGS__)
#define NV_PP_N_CAT_18(x, ...) x ## NV_PP_N_CAT_17(__VA_ARGS__)
#define NV_PP_N_CAT_19(x, ...) x ## NV_PP_N_CAT_18(__VA_ARGS__)

/* Generate a list of x with n_x elements */
#define NV_PP_X_LIST(n_x,x) NV_PP_DEFER(_NV_PP_X_LIST(n_x,x))
#define _NV_PP_X_LIST(n_x,x) NV_PP_X_LIST_##n_x(x)
#define NV_PP_X_LIST_1(x)    x
#define NV_PP_X_LIST_2(x)    x,NV_PP_X_LIST_1(x)
#define NV_PP_X_LIST_3(x)    x,NV_PP_X_LIST_2(x)
#define NV_PP_X_LIST_4(x)    x,NV_PP_X_LIST_3(x)
#define NV_PP_X_LIST_5(x)    x,NV_PP_X_LIST_4(x)
#define NV_PP_X_LIST_6(x)    x,NV_PP_X_LIST_5(x)
#define NV_PP_X_LIST_7(x)    x,NV_PP_X_LIST_6(x)
#define NV_PP_X_LIST_8(x)    x,NV_PP_X_LIST_7(x)
#define NV_PP_X_LIST_9(x)    x,NV_PP_X_LIST_8(x)
#define NV_PP_X_LIST_10(x)   x,NV_PP_X_LIST_9(x)
#define NV_PP_X_LIST_11(x)   x,NV_PP_X_LIST_10(x)
#define NV_PP_X_LIST_12(x)   x,NV_PP_X_LIST_11(x)
#define NV_PP_X_LIST_13(x)   x,NV_PP_X_LIST_12(x)
#define NV_PP_X_LIST_14(x)   x,NV_PP_X_LIST_13(x)
#define NV_PP_X_LIST_15(x)   x,NV_PP_X_LIST_14(x)
#define NV_PP_X_LIST_16(x)   x,NV_PP_X_LIST_15(x)
#define NV_PP_X_LIST_17(x)   x,NV_PP_X_LIST_16(x)
#define NV_PP_X_LIST_18(x)   x,NV_PP_X_LIST_17(x)
#define NV_PP_X_LIST_19(x)   x,NV_PP_X_LIST_18(x)
#define NV_PP_X_LIST_20(x)   x,NV_PP_X_LIST_19(x)

/* Substitutes x with x + 1 (Used by NV_PP_ADD) */
#define NV_PP_INC(x) NV_PP_DEFER(_NV_PP_INC(x))
#define _NV_PP_INC(x) _NV_PP_INC_##x
#define _NV_PP_INC_0  1
#define _NV_PP_INC_1  2
#define _NV_PP_INC_2  3
#define _NV_PP_INC_3  4
#define _NV_PP_INC_4  5
#define _NV_PP_INC_5  6
#define _NV_PP_INC_6  7
#define _NV_PP_INC_7  8
#define _NV_PP_INC_8  9
#define _NV_PP_INC_9  10
#define _NV_PP_INC_10 11
#define _NV_PP_INC_11 12
#define _NV_PP_INC_12 13
#define _NV_PP_INC_13 14
#define _NV_PP_INC_14 15
#define _NV_PP_INC_15 16
#define _NV_PP_INC_16 17
#define _NV_PP_INC_17 18
#define _NV_PP_INC_18 19
#define _NV_PP_INC_19 20

/* Substitutes x with (x-1) - Used by NV_PP_SUB */
#define NV_PP_DEC(x) NV_PP_DEFER(_NV_PP_DEC(x))
#define _NV_PP_DEC(x) _NV_PP_DEC_##x
#define _NV_PP_DEC_1  0
#define _NV_PP_DEC_2  1
#define _NV_PP_DEC_3  2
#define _NV_PP_DEC_4  3
#define _NV_PP_DEC_5  4
#define _NV_PP_DEC_6  5
#define _NV_PP_DEC_7  6
#define _NV_PP_DEC_8  7
#define _NV_PP_DEC_9  8
#define _NV_PP_DEC_10 9
#define _NV_PP_DEC_11 10
#define _NV_PP_DEC_12 11
#define _NV_PP_DEC_13 12
#define _NV_PP_DEC_14 13
#define _NV_PP_DEC_15 14
#define _NV_PP_DEC_16 15
#define _NV_PP_DEC_17 16
#define _NV_PP_DEC_18 17
#define _NV_PP_DEC_19 18
#define _NV_PP_DEC_20 19

/* Substitutes with the value (x - y) */
#define NV_PP_SUB(x, y) NV_PP_DEFER(_NV_PP_SUB(x,y))
#define _NV_PP_SUB(x,y) _NV_PP_SUB_##y(x)
#define _NV_PP_SUB_0(x)  x
#define _NV_PP_SUB_1(x)  NV_PP_DEC(x)
#define _NV_PP_SUB_2(x)  NV_PP_DEC(_NV_PP_SUB_1(x))
#define _NV_PP_SUB_3(x)  NV_PP_DEC(_NV_PP_SUB_2(x))
#define _NV_PP_SUB_4(x)  NV_PP_DEC(_NV_PP_SUB_3(x))
#define _NV_PP_SUB_5(x)  NV_PP_DEC(_NV_PP_SUB_4(x))
#define _NV_PP_SUB_6(x)  NV_PP_DEC(_NV_PP_SUB_5(x))
#define _NV_PP_SUB_7(x)  NV_PP_DEC(_NV_PP_SUB_6(x))
#define _NV_PP_SUB_8(x)  NV_PP_DEC(_NV_PP_SUB_7(x))
#define _NV_PP_SUB_9(x)  NV_PP_DEC(_NV_PP_SUB_8(x))
#define _NV_PP_SUB_10(x)  NV_PP_DEC(_NV_PP_SUB_9(x))
#define _NV_PP_SUB_11(x)  NV_PP_DEC(_NV_PP_SUB_10(x))
#define _NV_PP_SUB_12(x)  NV_PP_DEC(_NV_PP_SUB_11(x))
#define _NV_PP_SUB_13(x)  NV_PP_DEC(_NV_PP_SUB_12(x))
#define _NV_PP_SUB_14(x)  NV_PP_DEC(_NV_PP_SUB_13(x))
#define _NV_PP_SUB_15(x)  NV_PP_DEC(_NV_PP_SUB_14(x))
#define _NV_PP_SUB_16(x)  NV_PP_DEC(_NV_PP_SUB_15(x))
#define _NV_PP_SUB_17(x)  NV_PP_DEC(_NV_PP_SUB_16(x))
#define _NV_PP_SUB_18(x)  NV_PP_DEC(_NV_PP_SUB_17(x))
#define _NV_PP_SUB_19(x)  NV_PP_DEC(_NV_PP_SUB_18(x))
#define _NV_PP_SUB_20(x)  NV_PP_DEC(_NV_PP_SUB_19(x))

/* Substitutes with the value (x + y) */
#define NV_PP_ADD(x, y) NV_PP_DEFER(_NV_PP_ADD(x,y))
#define _NV_PP_ADD(x,y) _NV_PP_ADD_##y(x)
#define _NV_PP_ADD_0(x)  x
#define _NV_PP_ADD_1(x)  NV_PP_INC(x)
#define _NV_PP_ADD_2(x)  NV_PP_INC(_NV_PP_ADD_1(x))
#define _NV_PP_ADD_3(x)  NV_PP_INC(_NV_PP_ADD_2(x))
#define _NV_PP_ADD_4(x)  NV_PP_INC(_NV_PP_ADD_3(x))
#define _NV_PP_ADD_5(x)  NV_PP_INC(_NV_PP_ADD_4(x))
#define _NV_PP_ADD_6(x)  NV_PP_INC(_NV_PP_ADD_5(x))
#define _NV_PP_ADD_7(x)  NV_PP_INC(_NV_PP_ADD_6(x))
#define _NV_PP_ADD_8(x)  NV_PP_INC(_NV_PP_ADD_7(x))
#define _NV_PP_ADD_9(x)  NV_PP_INC(_NV_PP_ADD_8(x))
#define _NV_PP_ADD_10(x)  NV_PP_INC(_NV_PP_ADD_9(x))
#define _NV_PP_ADD_11(x)  NV_PP_INC(_NV_PP_ADD_10(x))
#define _NV_PP_ADD_12(x)  NV_PP_INC(_NV_PP_ADD_11(x))
#define _NV_PP_ADD_13(x)  NV_PP_INC(_NV_PP_ADD_12(x))
#define _NV_PP_ADD_14(x)  NV_PP_INC(_NV_PP_ADD_13(x))
#define _NV_PP_ADD_15(x)  NV_PP_INC(_NV_PP_ADD_14(x))
#define _NV_PP_ADD_16(x)  NV_PP_INC(_NV_PP_ADD_15(x))
#define _NV_PP_ADD_17(x)  NV_PP_INC(_NV_PP_ADD_16(x))
#define _NV_PP_ADD_18(x)  NV_PP_INC(_NV_PP_ADD_17(x))
#define _NV_PP_ADD_19(x)  NV_PP_INC(_NV_PP_ADD_18(x))
#define _NV_PP_ADD_20(x)  NV_PP_INC(_NV_PP_ADD_19(x))

/* Remove the first N arguments */
#define NV_PP_EAT_CHECK(nparams, ...) NV_PP_IF_ELSE(NV_PP_LIST_SZ_LT(nparams, __VA_ARGS__), NV_PP_EAT_ERR_MSG, NV_PP_EAT)(nparams, __VA_ARGS__)
#define NV_PP_EAT_ERR_MSG(nparams, ...) NV_PP_ERR("Invalid num args for NV_PP_EAT " NV_PP_STR(nparams) "- " NV_PP_STR(__VA_ARGS__))
#define NV_PP_EAT(nparams, ...) NV_PP_DEFER(_NV_PP_EAT(nparams, __VA_ARGS__))
#define _NV_PP_EAT(nparams, ...) NV_PP_EAT_##nparams(__VA_ARGS__)
#define NV_PP_EAT_0(...) __VA_ARGS__
#define NV_PP_EAT_1(eat, ...) NV_PP_EAT_0(__VA_ARGS__)
#define NV_PP_EAT_2(eat, ...) NV_PP_EAT_1(__VA_ARGS__)
#define NV_PP_EAT_3(eat, ...) NV_PP_EAT_2(__VA_ARGS__)
#define NV_PP_EAT_4(eat, ...) NV_PP_EAT_3(__VA_ARGS__)
#define NV_PP_EAT_5(eat, ...) NV_PP_EAT_4(__VA_ARGS__)
#define NV_PP_EAT_6(eat, ...) NV_PP_EAT_5(__VA_ARGS__)
#define NV_PP_EAT_7(eat, ...) NV_PP_EAT_6(__VA_ARGS__)
#define NV_PP_EAT_8(eat, ...) NV_PP_EAT_7(__VA_ARGS__)
#define NV_PP_EAT_9(eat, ...) NV_PP_EAT_8(__VA_ARGS__)
#define NV_PP_EAT_10(eat, ...) NV_PP_EAT_9(__VA_ARGS__)
#define NV_PP_EAT_11(eat, ...) NV_PP_EAT_10(__VA_ARGS__)
#define NV_PP_EAT_12(eat, ...) NV_PP_EAT_11(__VA_ARGS__)
#define NV_PP_EAT_13(eat, ...) NV_PP_EAT_12(__VA_ARGS__)
#define NV_PP_EAT_14(eat, ...) NV_PP_EAT_13(__VA_ARGS__)
#define NV_PP_EAT_15(eat, ...) NV_PP_EAT_14(__VA_ARGS__)
#define NV_PP_EAT_16(eat, ...) NV_PP_EAT_15(__VA_ARGS__)
#define NV_PP_EAT_17(eat, ...) NV_PP_EAT_16(__VA_ARGS__)
#define NV_PP_EAT_18(eat, ...) NV_PP_EAT_17(__VA_ARGS__)
#define NV_PP_EAT_19(eat, ...) NV_PP_EAT_18(__VA_ARGS__)
#define NV_PP_EAT_20(eat, ...) NV_PP_EAT_19(__VA_ARGS__)

/* Take the first n arguments */
#define NV_PP_TAKE_CHECK(nparams, ...) NV_PP_IF_ELSE(NV_PP_LIST_SZ_LT(nparams, __VA_ARGS__), NV_PP_TAKE_ERR_MSG, NV_PP_TAKE)(nparams, __VA_ARGS__)
#define NV_PP_TAKE_ERR_MSG(nparams, ...) NV_PP_ERR("Invalid num args for NV_PP_TAKE " NV_PP_STR(nparams) "- " NV_PP_STR(__VA_ARGS__))
#define NV_PP_TAKE(nparams, ...) NV_PP_DEFER(_NV_PP_TAKE(nparams, __VA_ARGS__))
#define _NV_PP_TAKE(nparams, ...) NV_PP_TAKE_##nparams(__VA_ARGS__)
#define NV_PP_TAKE_0(...)
#define NV_PP_TAKE_1(p0,...) p0
#define NV_PP_TAKE_2(p0,p1,...) p0,p1
#define NV_PP_TAKE_3(p0,p1,p2,...) p0,p1,p2
#define NV_PP_TAKE_4(p0,p1,p2,p3,...) p0,p1,p2,p3
#define NV_PP_TAKE_5(p0,p1,p2,p3,p4,p5,...) p0,p1,p2,p3,p4,p5
#define NV_PP_TAKE_6(p0,p1,p2,p3,p4,p5,p6,...) p0,p1,p2,p3,p4,p5,p6
#define NV_PP_TAKE_7(p0,p1,p2,p3,p4,p5,p6,p7,...) p0,p1,p2,p3,p4,p5,p6,p7
#define NV_PP_TAKE_8(p0,p1,p2,p3,p4,p5,p6,p7,p8,...) p0,p1,p2,p3,p4,p5,p6,p7,p8
#define NV_PP_TAKE_9(p0,p1,p2,p3,p4,p5,p6,p7,p8,p9,...) p0,p1,p2,p3,p4,p5,p6,p7,p8,p9
#define NV_PP_TAKE_10(p0,p1,p2,p3,p4,p5,p6,p7,p8,p9,p10,...) p0,p1,p2,p3,p4,p5,p6,p7,p8,p9,p10
#define NV_PP_TAKE_11(p0,p1,p2,p3,p4,p5,p6,p7,p8,p9,p10,p11,...) p0,p1,p2,p3,p4,p5,p6,p7,p8,p9,p10,p11
#define NV_PP_TAKE_12(p0,p1,p2,p3,p4,p5,p6,p7,p8,p9,p10,p11,p12,...) p0,p1,p2,p3,p4,p5,p6,p7,p8,p9,p10,p11,p12
#define NV_PP_TAKE_13(p0,p1,p2,p3,p4,p5,p6,p7,p8,p9,p10,p11,p12,p13,...) p0,p1,p2,p3,p4,p5,p6,p7,p8,p9,p10,p11,p12,p13
#define NV_PP_TAKE_14(p0,p1,p2,p3,p4,p5,p6,p7,p8,p9,p10,p11,p12,p13,p14,...) p0,p1,p2,p3,p4,p5,p6,p7,p8,p9,p10,p11,p12,p13,p14
#define NV_PP_TAKE_15(p0,p1,p2,p3,p4,p5,p6,p7,p8,p9,p10,p11,p12,p13,p14,p15,...) p0,p1,p2,p3,p4,p5,p6,p7,p8,p9,p10,p11,p12,p13,p14,p15
#define NV_PP_TAKE_16(p0,p1,p2,p3,p4,p5,p6,p7,p8,p9,p10,p11,p12,p13,p14,p15,p16,...) p0,p1,p2,p3,p4,p5,p6,p7,p8,p9,p10,p11,p12,p13,p14,p15,p16
#define NV_PP_TAKE_17(p0,p1,p2,p3,p4,p5,p6,p7,p8,p9,p10,p11,p12,p13,p14,p15,p16,p17,...) p0,p1,p2,p3,p4,p5,p6,p7,p8,p9,p10,p11,p12,p13,p14,p15,p16,p17
#define NV_PP_TAKE_18(p0,p1,p2,p3,p4,p5,p6,p7,p8,p9,p10,p11,p12,p13,p14,p15,p16,p17,p18,...) p0,p1,p2,p3,p4,p5,p6,p7,p8,p9,p10,p11,p12,p13,p14,p15,p16,p17,p18
#define NV_PP_TAKE_19(p0,p1,p2,p3,p4,p5,p6,p7,p8,p9,p10,p11,p12,p13,p14,p15,p16,p17,p18,p19,...) p0,p1,p2,p3,p4,p5,p6,p7,p8,p9,p10,p11,p12,p13,p14,p15,p16,p17,p18,p19

/* Substitutes with the size of the argument list */
#define NV_PP_LIST_SZ(...) _NV_PP_LIST_SZ(pad,__VA_ARGS__,NV_PP_MAX_RSEQ)
#define _NV_PP_LIST_SZ(...) NV_PP_TAKE(1,NV_PP_EAT(NV_PP_MAX,__VA_ARGS__))

/* If the argument list size is n, substitutes with 1 otherwise 0 */
#define NV_PP_LIST_SZ_EQ(n, ...) NV_PP_DEFER(_NV_PP_LIST_SZ_EQ(n, __VA_ARGS__))
#define _NV_PP_LIST_SZ_EQ(n, ...) __NV_PP_LIST_SZ_EQ(0, ## __VA_ARGS__,NV_PP_X_LIST(NV_PP_SUB(NV_PP_MAX,n),0),1,NV_PP_X_LIST(n,0))
#define __NV_PP_LIST_SZ_EQ(...) NV_PP_TAKE(1,NV_PP_EAT(1,NV_PP_EAT(NV_PP_MAX,__VA_ARGS__)))

/* If the argument list size is n, substitutes with 0 otherwise 1 */
#define NV_PP_LIST_SZ_NE(n, ...) NV_PP_DEFER(_NV_PP_LIST_SZ_NE(n, __VA_ARGS__))
#define _NV_PP_LIST_SZ_NE(n, ...) __NV_PP_LIST_SZ_NE(1, ## __VA_ARGS__,NV_PP_X_LIST(NV_PP_SUB(NV_PP_MAX,n),1),0,NV_PP_X_LIST(n,1))
#define __NV_PP_LIST_SZ_NE(...) NV_PP_TAKE(1,NV_PP_EAT(1,NV_PP_EAT(NV_PP_MAX,__VA_ARGS__)))

/* If the argument list size is < n, substitutes with 1, otherwise 0 */
#define NV_PP_LIST_SZ_LT(n, ...) NV_PP_DEFER(_NV_PP_LIST_SZ_LT(n, __VA_ARGS__))
#define _NV_PP_LIST_SZ_LT(n, ...) __NV_PP_LIST_SZ_EQ(0, ## __VA_ARGS__,NV_PP_X_LIST(NV_PP_SUB(NV_PP_MAX,n),0),0,NV_PP_X_LIST(n,1))

/* If the argument list size is > n, substitutes with 1, otherwise 0 */
#define NV_PP_LIST_SZ_GT(n, ...) NV_PP_DEFER(_NV_PP_LIST_SZ_GT(n, __VA_ARGS__))
#define _NV_PP_LIST_SZ_GT(n, ...) __NV_PP_LIST_SZ_EQ(0, ## __VA_ARGS__,NV_PP_X_LIST(NV_PP_SUB(NV_PP_MAX,n),1),0,NV_PP_X_LIST(n,0))

/* Substitutes with the Boolean complement of x */
#define NV_PP_NOT(x) NV_PP_DEFER(_NV_PP_NOT(x))
#define _NV_PP_NOT(x) _NV_PP_NOT_##x
#define _NV_PP_NOT_0 1
#define _NV_PP_NOT_1 0

/* Substitutes with the Boolean AND of x and y */
#define NV_PP_AND(x,y) NV_PP_DEFER(_NV_PP_AND(x,y))
#define _NV_PP_AND(x,y) _NV_PP_AND_##x_##y
#define _NV_PP_AND_0_0 0
#define _NV_PP_AND_0_1 0
#define _NV_PP_AND_1_0 0
#define _NV_PP_AND_1_1 1

/* Substitutes with the Boolean OR of x and y */
#define NV_PP_OR(x,y) NV_PP_DEFER(_NV_PP_OR(x,y))
#define _NV_PP_OR(x,y) _NV_PP_OR_##x_##y
#define _NV_PP_OR_0_0 0
#define _NV_PP_OR_0_1 1
#define _NV_PP_OR_1_0 1
#define _NV_PP_OR_1_1 1


#define NV_PP_MSG(x) _Pragma(_NV_PP_MSG(x))
#define _NV_PP_MSG(x) __NV_PP_MSG(message #x)
#define __NV_PP_MSG(x)	#x

#define NV_PP_ERR(x) _Pragma(_NV_PP_ERR(x))
#define _NV_PP_ERR(x) __NV_PP_ERR(GCC error #x)
#define __NV_PP_ERR(x)	#x

/* Do pragma message if condition is 1 */
#define NV_PP_MSG_IF(cond,...) NV_PP_DEFER(_NV_PP_MSG_IF(cond,__VA_ARGS__))
#define _NV_PP_MSG_IF(cond,...) _NV_PP_MSG_IF_##cond(__VA_ARGS__)
#define _NV_PP_MSG_IF_0(...)
#define _NV_PP_MSG_IF_1(...) NV_PP_MSG(__VA_ARGS__)

/* Do pragma error if condition is 1 */
#define NV_PP_ERR_IF(cond,msg) NV_PP_DEFER(_NV_PP_ERR_IF(cond,msg))
#define _NV_PP_ERR_IF(cond,msg) _NV_PP_ERR_IF_##cond(msg)
#define _NV_PP_ERR_IF_0(msg)
#define _NV_PP_ERR_IF_1(msg) NV_PP_ERR(msg)

/* If x == y, substitue 1 otherwise 0 */
#define NV_PP_EQ(x,y) NV_PP_DEFER(_NV_PP_EQ(x,y))
#define _NV_PP_EQ(x,y) NV_PP_LIST_SZ_EQ(x, NV_PP_X_LIST(y,tmp))

/* If x < y, substitue 1 otherwise 0 */
#define NV_PP_LT(x,y) NV_PP_DEFER(_NV_PP_LT(x,y))
#define _NV_PP_LT(x,y) NV_PP_LIST_SZ_LT(y, NV_PP_X_LIST(x,tmp))

/* If x > y, substitue 1 otherwise 0 */
#define NV_PP_GT(x,y) NV_PP_DEFER(_NV_PP_GT(x,y))
#define _NV_PP_GT(x,y) NV_PP_LIST_SZ_GT(y, NV_PP_X_LIST(x,tmp))

/* If cond == 1, substitue with do_true else do_else */
#define NV_PP_IF_ELSE(cond, do_true, do_false) NV_PP_DEFER(_NV_PP_IF_ELSE(cond, do_true, do_false))
#define _NV_PP_IF_ELSE(cond, do_true, do_false) NV_PP_PRIM_CAT(__NV_PP_IF_ELSE_, cond)(do_true, do_false)
#define __NV_PP_IF_ELSE_1(do_true, do_false) NV_PP_DEFER(do_true)
#define __NV_PP_IF_ELSE_0(do_true, do_false) NV_PP_DEFER(do_false)

#define NV_PP_IF_ELSE_VARGS(cond, do_true, do_false, ...) NV_PP_DEFER(_NV_PP_IF_ELSE_VARGS(cond, do_true, do_false, __VA_ARGS__))
#define _NV_PP_IF_ELSE_VARGS(cond, do_true, do_false, ...) NV_PP_PRIM_CAT(__NV_PP_IF_ELSE_VARGS_, cond)(do_true, do_false, __VA_ARGS__)
#define __NV_PP_IF_ELSE_VARGS_1(do_true, do_false, ...) do_true(__VA_ARGS__)
#define __NV_PP_IF_ELSE_VARGS_0(do_true, do_false, ...) do_false(__VA_ARGS__)

#ifdef NV_PP_TEST
/* Test suite */
#define NV_PP_TEST_TAKES_3_PARAMS(a, b, c) NV_PP_MSG(ThreeParams)
#define NV_PP_TEST_TAKES_N_PARAMS(...) NV_PP_MSG(NParams)
#define NV_PP_TEST_N_PARAM_CHECK(...) \
	NV_PP_IF_ELSE_VARGS(NV_PP_LIST_SZ_EQ(3, __VA_ARGS__), NV_PP_TEST_TAKES_3_PARAMS, NV_PP_TEST_TAKES_N_PARAMS, ## __VA_ARGS__)
/*NV_PP_TEST_N_PARAM_CHECK(a, b, c);
NV_PP_IF_ELSE(0, NV_PP_MSG(Test1), NV_PP_MSG(Test!1));
NV_PP_IF_ELSE_VARGS(1, NV_PP_MSG, NV_PP_EMPTY, Test);*/
NV_PP_ERR_IF(NV_PP_NOT(NV_PP_EQ(4,NV_PP_ADD(1,3))),NV_PP Test Error);
NV_PP_ERR_IF(NV_PP_NOT(NV_PP_LT(NV_PP_SUB(6,4),18)),NV_PP Test Error);
NV_PP_ERR_IF(NV_PP_NOT(NV_PP_GT(12,8)),NV_PP Test Error);
#endif

#endif