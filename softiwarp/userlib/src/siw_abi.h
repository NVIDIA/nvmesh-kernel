/*
 * Software iWARP library for Linux
 *
 * Authors: Bernard Metzler <bmt@zurich.ibm.com>
 *
 * Copyright (c) 2008-2016, IBM Corporation
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *   Redistribution and use in source and binary forms, with or
 *   without modification, are permitted provided that the following
 *   conditions are met:
 *
 *   - Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 *   - Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 *   - Neither the name of IBM nor the names of its contributors may be
 *     used to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef _SIW_ABI_H
#define _SIW_ABI_H

#include <infiniband/kern-abi.h>
#include "../../common/siw_user.h"
#include "siw_user_abi.h"

DECLARE_DRV_CMD(siw_alloc_pd, IB_USER_VERBS_CMD_ALLOC_PD,
		empty, siw_uresp_alloc_pd);
DECLARE_DRV_CMD(siw_alloc_ucontext, IB_USER_VERBS_CMD_GET_CONTEXT,
		empty, siw_uresp_alloc_ctx);
DECLARE_DRV_CMD(siw_cmd_reg_umr, IB_USER_VERBS_CMD_REG_MR,
		siw_ureq_reg_mr, siw_uresp_reg_mr);
DECLARE_DRV_CMD(siw_cmd_create_cq, IB_USER_VERBS_CMD_CREATE_CQ,
		empty, siw_uresp_create_cq);
DECLARE_DRV_CMD(siw_cmd_create_qp, IB_USER_VERBS_CMD_CREATE_QP,
		empty, siw_uresp_create_qp);
DECLARE_DRV_CMD(siw_cmd_create_srq, IB_USER_VERBS_CMD_CREATE_SRQ,
		empty, siw_uresp_create_srq);

#endif	/* _SIW_ABI_H */
