/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIB_NVME_ERROR_CODES_H
#define NVMEIB_NVME_ERROR_CODES_H
#include "nvmeib_nvme.h"

enum {
	/*
	 * Media and Data Integrity Errors:
	 */
	NVME_SC_DATA			= 0x0200,
};


enum nvmeib_error_codes {
	EDEAD = 0xDEAD,
	EPENDING = 0xDEAF,
	EPERM_READ_FAIL           = NVME_SC_READ_ERROR,
	EPERM_READ_FAIL_NO_RETRY  = NVME_SC_DNR | NVME_SC_READ_ERROR,
	EPERM_WRITE_FAIL          = NVME_SC_WRITE_FAULT,
	EPERM_WRITE_FAIL_NO_RETRY = NVME_SC_DNR | NVME_SC_WRITE_FAULT,
	EPERM_ACCESS_DENIED_NO_RETRY = NVME_SC_DNR | NVME_SC_ACCESS_DENIED,
};
#define nvmeib_error_code_refine(code)  ((code)&0x4FFF)	// Take only relevant bits (remove the log bit and reserved bits)
#define nvmeib_error_code_has_dnr(code) ((code)&0x4000)	// Is do not retry bit turned on

#define is_software_error_um(err) (err < 0)

#define DNR_DATA_MASK (NVME_SC_DNR | NVME_SC_DATA)

#define error_code_has_dnr(code) ((code & DNR_DATA_MASK) == DNR_DATA_MASK)

#define is_transient_disk_error_um(err) (is_software_error_um(err) ||  \
				(!(error_code_has_dnr(err) || /* ! Hardware error */ \
				  (err == EPERM_READ_FAIL) || (err == EPERM_WRITE_FAIL))))

#endif//NVMEIB_NVME_ERROR_CODES_H
