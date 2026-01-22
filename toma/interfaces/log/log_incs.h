/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#ifndef NVMEIBT_LOGGING_INCS_H
#define NVMEIBT_LOGGING_INCS_H

#include "interfaces/log/nvmeibt_trace.h"

/******************************************************************************/
// ANSI Color Codes for Terminal Output
// Reference: https://telepathy.freedesktop.org/doc/telepathy-glib/telepathy-glib-debug-ansi.html
/******************************************************************************/

#define COL_RED         "\x1b[31m"
#define COL_GREEN       "\x1b[32m"
#define COL_YELLOW      "\x1b[1;33m"
#define COL_RED_BOLD    "\x1b[1;31m"		// Bold format is 1;
#define COL_WHITE_BOLD  "\x1b[1;37m"
#define COL_RESET       "\x1b[0;0m"
#define COL_PURPL       "\x1b[1;35m"		// Purple
#define COL_BLUE        "\x1b[16;34m"		// Blue
#define COL_WHITE_UNDER "\x1b[4m"			// Underline

#endif // NVMEIBT_LOGGING_INCS_H
// EOF.
