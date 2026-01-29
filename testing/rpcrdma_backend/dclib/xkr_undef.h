/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#ifndef XKR_UNDEF_H
#define XKR_UNDEF_H

#if 0
#undef module_init
#define module_init(initfn)                                             \
        static int __init __init_backport(void)                         \
        {                                                               \
                return initfn();                                        \
        }                                                               \
        int init_module(void) __attribute__((alias("__init_backport")));
#endif

#undef module_init
#define module_init(initfn)                                     \
        static inline initcall_t __inittest(void)               \
        { return initfn; }                                      \
        int init_module(void) __attribute__((alias(#initfn)));

#undef module_exit
#define module_exit(exitfn)                                     \
        static inline exitcall_t __exittest(void)               \
        { return exitfn; }                                      \
        void cleanup_module(void) __attribute__((alias(#exitfn)));

#endif
