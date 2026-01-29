/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#ifndef CORECOMM_NETLINK_RPC_H
#define CORECOMM_NETLINK_RPC_H

/**
 * @file corecomm_netlink_rpc.h
 * Infrastructure used to hide netlink messages from the user, to separate
 * business logic from implementation details which take 70% of the code
 * otherwise.
 * @limitations:
 * 1. Current maximum allowed number of arguments per message - 15. can be
 * easily extended by enlarging foreach macro.
 * 2. Currently API returning void is not supported. Return in and ignore it if
 * needed. Not that it is impossible to implement - can be done with some gcc
 * extensions, just not worth it.
 */

/******* HELPER MACROS ********/
/**
 * @WARNING: This section contains strong black magic.
 *             ._                                            ,
             (`)..                                    ,.-')
              (',.)-..                            ,.-(..`)
               (,.' ,.)-..                    ,.-(. `.. )
                (,.' ..' .)-..            ,.-( `.. `.. )
                 (,.' ,.'  ..')-.     ,.-( `. `.. `.. )
                  (,.'  ,.' ,.'  )-.-('   `. `.. `.. )
                   ( ,.' ,.'    _== ==_     `.. `.. )
                    ( ,.'   _==' ~  ~  `==_    `.. )
                     \  _=='   ----..----  `==_   )
                  ,.-:    ,----___.  .___----.    -..
              ,.-'   (   _--====_  \/  _====--_   )  `-..
          ,.-'   .__.'`.  `-_I0_-'    `-_0I_-'  .'`.__.  `-..
      ,.-'.'   .'      (          |  |          )      `.   `.-..
  ,.-'    :    `___--- '`.__.    / __ \    .__.' `---___'    :   `-..
-'_________`-____________'__ \  (O)  (O)  / __`____________-'________`-
                            \ . _  __  _ . /
                             \ `V-'  `-V' |
                              | \ \ | /  /
                               V \ ~| ~/V
                                |  \  /|
                                 \~ | V             - JGG
                                  \  |
                                   VV
 */
#define nlrpc_noop(___x) ___x

#define nlrpc_stringify(___x) nlrpc_stringify_(___x)
#define nlrpc_stringify_(___x) #___x

#define nlrpc_cat_2(___x, ___y) nlrpc_cat_2_(___x, ___y)
#define nlrpc_cat_2_(___x, ___y) ___x##___y

#define nlrpc_cat(___x) nlrpc_cat_(___x)
#define nlrpc_cat_(...) __VA_ARGS__

#define nlrpc_cat_n(...) nlrpc_cat_n_(__VA_ARGS__)
#define nlrpc_cat_n_(...) __VA_ARGS__

#define nlrpc_narg(...) nlrpc_narg_(dum, ##__VA_ARGS__, nlrpc_n_list())
#define nlrpc_narg_(...) nlrpc_arg_n(__VA_ARGS__)

#define nlrpc_n_list()                                                         \
	62, 61, 60, 59, 58, 57, 56, 55, 54, 53, 52, 51, 50, 49, 48, 47, 46, 45,    \
	    44, 43, 42, 41, 40, 39, 38, 37, 36, 35, 34, 33, 32, 31, 30, 29, 28,    \
	    27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16, 15, 14, 13, 12, 11,    \
	    10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, -1

#define nlrpc_arg_n(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13,    \
                    _14, _15, _16, _17, _18, _19, _20, _21, _22, _23, _24,     \
                    _25, _26, _27, _28, _29, _30, _31, _32, _33, _34, _35,     \
                    _36, _37, _38, _39, _40, _41, _42, _43, _44, _45, _46,     \
                    _47, _48, _49, _50, _51, _52, _53, _54, _55, _56, _57,     \
                    _58, _59, _60, _61, _62, _63, N, ...)                      \
	N

#define nlrpc_foreach(w, ...)                                                  \
	nlrpc_cat_2(nlrpc_foreach_, nlrpc_narg(__VA_ARGS__))(w, ##__VA_ARGS__)
#define nlrpc_foreach_0(w)
#define nlrpc_foreach_1(w, x) w(x) nlrpc_foreach_0(w)
#define nlrpc_foreach_2(w, x, ...) w(x) nlrpc_foreach_1(w, __VA_ARGS__)
#define nlrpc_foreach_3(w, x, ...) w(x) nlrpc_foreach_2(w, __VA_ARGS__)
#define nlrpc_foreach_4(w, x, ...) w(x) nlrpc_foreach_3(w, __VA_ARGS__)
#define nlrpc_foreach_5(w, x, ...) w(x) nlrpc_foreach_4(w, __VA_ARGS__)
#define nlrpc_foreach_6(w, x, ...) w(x) nlrpc_foreach_5(w, __VA_ARGS__)
#define nlrpc_foreach_7(w, x, ...) w(x) nlrpc_foreach_6(w, __VA_ARGS__)
#define nlrpc_foreach_8(w, x, ...) w(x) nlrpc_foreach_7(w, __VA_ARGS__)
#define nlrpc_foreach_9(w, x, ...) w(x) nlrpc_foreach_8(w, __VA_ARGS__)
#define nlrpc_foreach_10(w, x, ...) w(x) nlrpc_foreach_9(w, __VA_ARGS__)
#define nlrpc_foreach_11(w, x, ...) w(x) nlrpc_foreach_10(w, __VA_ARGS__)
#define nlrpc_foreach_12(w, x, ...) w(x) nlrpc_foreach_11(w, __VA_ARGS__)
#define nlrpc_foreach_13(w, x, ...) w(x) nlrpc_foreach_12(w, __VA_ARGS__)
#define nlrpc_foreach_14(w, x, ...) w(x) nlrpc_foreach_13(w, __VA_ARGS__)
#define nlrpc_foreach_15(w, x, ...) w(x) nlrpc_foreach_14(w, __VA_ARGS__)
#define nlrpc_foreach_16(w, x, ...) w(x) nlrpc_foreach_15(w, __VA_ARGS__)
#define nlrpc_foreach_17(w, x, ...) w(x) nlrpc_foreach_16(w, __VA_ARGS__)
#define nlrpc_foreach_18(w, x, ...) w(x) nlrpc_foreach_17(w, __VA_ARGS__)
#define nlrpc_foreach_19(w, x, ...) w(x) nlrpc_foreach_18(w, __VA_ARGS__)
#define nlrpc_foreach_20(w, x, ...) w(x) nlrpc_foreach_19(w, __VA_ARGS__)
#define nlrpc_foreach_21(w, x, ...) w(x) nlrpc_foreach_20(w, __VA_ARGS__)
#define nlrpc_foreach_22(w, x, ...) w(x) nlrpc_foreach_21(w, __VA_ARGS__)
#define nlrpc_foreach_23(w, x, ...) w(x) nlrpc_foreach_22(w, __VA_ARGS__)
#define nlrpc_foreach_24(w, x, ...) w(x) nlrpc_foreach_23(w, __VA_ARGS__)
#define nlrpc_foreach_25(w, x, ...) w(x) nlrpc_foreach_24(w, __VA_ARGS__)
#define nlrpc_foreach_26(w, x, ...) w(x) nlrpc_foreach_25(w, __VA_ARGS__)
#define nlrpc_foreach_27(w, x, ...) w(x) nlrpc_foreach_26(w, __VA_ARGS__)
#define nlrpc_foreach_28(w, x, ...) w(x) nlrpc_foreach_27(w, __VA_ARGS__)
#define nlrpc_foreach_29(w, x, ...) w(x) nlrpc_foreach_28(w, __VA_ARGS__)
#define nlrpc_foreach_30(w, x, ...) w(x) nlrpc_foreach_29(w, __VA_ARGS__)
#define nlrpc_foreach_31(w, x, ...) w(x) nlrpc_foreach_30(w, __VA_ARGS__)
#define nlrpc_foreach_32(w, x, ...) w(x) nlrpc_foreach_31(w, __VA_ARGS__)
#define nlrpc_foreach_33(w, x, ...) w(x) nlrpc_foreach_32(w, __VA_ARGS__)
#define nlrpc_foreach_34(w, x, ...) w(x) nlrpc_foreach_33(w, __VA_ARGS__)
#define nlrpc_foreach_35(w, x, ...) w(x) nlrpc_foreach_34(w, __VA_ARGS__)
#define nlrpc_foreach_36(w, x, ...) w(x) nlrpc_foreach_35(w, __VA_ARGS__)
#define nlrpc_foreach_37(w, x, ...) w(x) nlrpc_foreach_36(w, __VA_ARGS__)
#define nlrpc_foreach_38(w, x, ...) w(x) nlrpc_foreach_37(w, __VA_ARGS__)
#define nlrpc_foreach_39(w, x, ...) w(x) nlrpc_foreach_38(w, __VA_ARGS__)
#define nlrpc_foreach_40(w, x, ...) w(x) nlrpc_foreach_39(w, __VA_ARGS__)
#define nlrpc_foreach_41(w, x, ...) w(x) nlrpc_foreach_40(w, __VA_ARGS__)
#define nlrpc_foreach_42(w, x, ...) w(x) nlrpc_foreach_41(w, __VA_ARGS__)
#define nlrpc_foreach_43(w, x, ...) w(x) nlrpc_foreach_42(w, __VA_ARGS__)
#define nlrpc_foreach_44(w, x, ...) w(x) nlrpc_foreach_43(w, __VA_ARGS__)
#define nlrpc_foreach_45(w, x, ...) w(x) nlrpc_foreach_44(w, __VA_ARGS__)
#define nlrpc_foreach_46(w, x, ...) w(x) nlrpc_foreach_45(w, __VA_ARGS__)
#define nlrpc_foreach_47(w, x, ...) w(x) nlrpc_foreach_46(w, __VA_ARGS__)
#define nlrpc_foreach_48(w, x, ...) w(x) nlrpc_foreach_47(w, __VA_ARGS__)
#define nlrpc_foreach_49(w, x, ...) w(x) nlrpc_foreach_48(w, __VA_ARGS__)
#define nlrpc_foreach_50(w, x, ...) w(x) nlrpc_foreach_49(w, __VA_ARGS__)
#define nlrpc_foreach_51(w, x, ...) w(x) nlrpc_foreach_50(w, __VA_ARGS__)
#define nlrpc_foreach_52(w, x, ...) w(x) nlrpc_foreach_51(w, __VA_ARGS__)
#define nlrpc_foreach_53(w, x, ...) w(x) nlrpc_foreach_52(w, __VA_ARGS__)
#define nlrpc_foreach_54(w, x, ...) w(x) nlrpc_foreach_53(w, __VA_ARGS__)
#define nlrpc_foreach_55(w, x, ...) w(x) nlrpc_foreach_54(w, __VA_ARGS__)
#define nlrpc_foreach_56(w, x, ...) w(x) nlrpc_foreach_55(w, __VA_ARGS__)
#define nlrpc_foreach_57(w, x, ...) w(x) nlrpc_foreach_56(w, __VA_ARGS__)
#define nlrpc_foreach_58(w, x, ...) w(x) nlrpc_foreach_57(w, __VA_ARGS__)
#define nlrpc_foreach_59(w, x, ...) w(x) nlrpc_foreach_58(w, __VA_ARGS__)
#define nlrpc_foreach_60(w, x, ...) w(x) nlrpc_foreach_59(w, __VA_ARGS__)
#define nlrpc_foreach_61(w, x, ...) w(x) nlrpc_foreach_60(w, __VA_ARGS__)
#define nlrpc_foreach_62(w, x, ...) w(x) nlrpc_foreach_61(w, __VA_ARGS__)
#define nlrpc_foreach_63(w, x, ...) w(x) nlrpc_foreach_62(w, __VA_ARGS__)
#define nlrpc_foreach_64(w, x, ...) w(x) nlrpc_foreach_63(w, __VA_ARGS__)





#define nlrpc_unwrap_tuple_x_(___x, ___y) ___x
#define nlrpc_unwrap_tuple_x(tuple) nlrpc_unwrap_tuple_x_ tuple

#define nlrpc_unwrap_tuple_y_(___x, ___y) ___y
#define nlrpc_unwrap_tuple_y(tuple) nlrpc_unwrap_tuple_y_ tuple

#define nlrpc_unwrap_tuple_semi_(___x, ___y) ___x ___y;
#define nlrpc_unwrap_tuple_semi(tuple) nlrpc_unwrap_tuple_semi_ tuple

#define nlrpc_unwrap_tuple_comma_(___x, ___y) , ___x ___y
#define nlrpc_unwrap_tuple_comma(tuple) nlrpc_unwrap_tuple_comma_ tuple

#define nlrpc_unwrap_tuple_comma_const_(___x, ___y) , const ___x ___y
#define nlrpc_unwrap_tuple_comma_const(tuple)                                  \
	nlrpc_unwrap_tuple_comma_const_ tuple

#define nlrpc_unwrap_tuple_comma_trail_(___x, ___y) ___x ___y,
#define nlrpc_unwrap_tuple_comma_trail(tuple)                                  \
	nlrpc_unwrap_tuple_comma_trail_ tuple

#define nlrpc_unwrap_arglist_(___x, ___y) , msg->___y
#define nlrpc_unwrap_arglist(tuple) nlrpc_unwrap_arglist_ tuple

/**
 * Fancy trick - get pointer to variable contents, whether it is array, string
 * or value.
 * Currently supports int [], char [] or string, extend as needed
 */
#define nlrpc_pointer_to_start(x)                                              \
	(__builtin_choose_expr(                                                    \
	    __builtin_types_compatible_p(typeof(x), char *) ||                     \
	        __builtin_types_compatible_p(typeof(x), const char *) ||           \
	        __builtin_types_compatible_p(typeof(x), char[]) ||                 \
	        __builtin_types_compatible_p(typeof(x), const char[]) ||           \
	        __builtin_types_compatible_p(typeof(x), int[]) ||                  \
	        __builtin_types_compatible_p(typeof(x), const int[]) ||            \
	        __builtin_types_compatible_p(typeof(x), unsigned int[]) ||         \
	        __builtin_types_compatible_p(typeof(x), const unsigned int[]) ||   \
	        __builtin_types_compatible_p(typeof(x), unsigned long[]) ||        \
	        __builtin_types_compatible_p(typeof(x), const unsigned long[]) ||  \
	        __builtin_types_compatible_p(typeof(x), unsigned long *) ||        \
	        __builtin_types_compatible_p(typeof(x), const unsigned long *) ||  \
	        __builtin_types_compatible_p(typeof(x), const void *) ||           \
	        __builtin_types_compatible_p(typeof(x), void *),                   \
	    (x), &(x)))

/**
 * Renderers for
 */

#define nlrpc_unwrap_tuple_memcpy_(___x, ___y)                                 \
	memcpy(nlrpc_pointer_to_start(data->___y), nlrpc_pointer_to_start(___y),   \
	       sizeof(data->___y));

#define nlrpc_unwrap_tuple_memcpy(tuple) nlrpc_unwrap_tuple_memcpy_ tuple

#define nlrpc_enum_name(api) nlrpc_cat_2(NL_MSGID_api_, api)
#define nlrpc_enum_name_comma(api) , nlrpc_cat_2(NL_MSGID_api_, api)

#define nlrpc_api_handler_name(api) nlrpc_cat_2(api, __handler)
#define nlrpc_api_handler_name__(api) nlrpc_cat_2(api, __handler__)

#define nlrpc_inp(api) nlrpc_cat_2(api, __inp)

#define nlrpc_dbg_fmt(tuple) nlrpc_dbg_fmt_ tuple
#define nlrpc_dbg_fmt_(type, var)                                              \
	__builtin_choose_expr(                                                     \
	    __builtin_types_compatible_p(type, char[]) ||                          \
	        __builtin_types_compatible_p(type, char *),                        \
	    "%s",                                                                  \
	    __builtin_choose_expr(                                                 \
	        __builtin_types_compatible_p(type, unsigned long long) ||          \
	            __builtin_types_compatible_p(type, long long),                 \
	        "0x%llx",                                                          \
	        __builtin_choose_expr(                                             \
	            __builtin_types_compatible_p(type, unsigned int) ||            \
	                __builtin_types_compatible_p(type, int) ||                 \
	                __builtin_types_compatible_p(type, unsigned short) ||      \
	                __builtin_types_compatible_p(type, short) ||               \
	                __builtin_types_compatible_p(type, unsigned char) ||       \
	                __builtin_types_compatible_p(type, char),                  \
	            "%d",                                                          \
	            __builtin_choose_expr(                                         \
	                __builtin_types_compatible_p(type, unsigned long) ||       \
	                    __builtin_types_compatible_p(type, long),              \
	                "0x%lx",                                                   \
	                __builtin_choose_expr(                                     \
	                    __builtin_types_compatible_p(type, unsigned long[]),   \
	                    "[0]=0x%lx...", "[%p-ptr]")))))

#define nlrpc_dbg_param(tuple) nlrpc_dbg_param_ tuple
#define nlrpc_dbg_param_(type, var)                                            \
	__builtin_choose_expr(                                                     \
	    __builtin_types_compatible_p(type, char[]) ||                          \
	        __builtin_types_compatible_p(type, char *) ||                      \
	        __builtin_types_compatible_p(type, unsigned long long) ||          \
	        __builtin_types_compatible_p(type, long long) ||                   \
	        __builtin_types_compatible_p(type, unsigned int) ||                \
	        __builtin_types_compatible_p(type, int) ||                         \
	        __builtin_types_compatible_p(type, unsigned short) ||              \
	        __builtin_types_compatible_p(type, short) ||                       \
	        __builtin_types_compatible_p(type, unsigned char) ||               \
	        __builtin_types_compatible_p(type, char) ||                        \
	        __builtin_types_compatible_p(type, unsigned long) ||               \
	        __builtin_types_compatible_p(type, long),                          \
	    msg->var,                                                              \
	    __builtin_choose_expr(                                                 \
	        __builtin_types_compatible_p(type, unsigned long[]),               \
	        *((unsigned long *)nlrpc_pointer_to_start(msg->var)),              \
	        nlrpc_pointer_to_start(msg->var)))

/******* BODY - SHARED BETWEEN SERVER AND CLIENT ********/

/* Generic message type for NLRPC protocol replies */
#define NLRPC_REPLY 0x100

/**
 * Declare API codes enum, must be seen similary by server and
 * client
 * @param first_num Integer to start with (ex. 0x4000)
 * @param ... Comma separated list of exposed api names
 */
#define NLRPC_API_CODES(...) NLRPC_API_CODES_(__VA_ARGS__)
#define NLRPC_API_CODES_(first_num, ...)                                       \
	enum __api_codes {                                                         \
		nlrpc_enum_name(first) =                                               \
		    (first_num)nlrpc_foreach(nlrpc_enum_name_comma, ##__VA_ARGS__),    \
		nlrpc_enum_name(last)                                                  \
	}

#endif /*CORECOMM_NETLINK_RPC_H*/