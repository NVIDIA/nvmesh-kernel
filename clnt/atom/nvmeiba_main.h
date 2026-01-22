/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBA_MAIN_H
#define NVMEIBA_MAIN_H
/* Tiny module which allows NVMEsh to hot upgrade:
   Manages all atoms. Each atom is created by nvmeibc, but can be abandoned and
   be adopted.
   Must not include anything from the rest of NVMesh codebase! */
#include "nvmeiba_infra.h"
#include "nvmeib_jdr.h"
struct nvmeiba_all_os_apis {				// Main object of nvmeiba
	spinlock_t lock;
	struct list_head list;					// List of atoms
	struct nvmeiba_all_debug_cntrs {		// Various debug counters to verify abandon/adopt flows
		int	osapi;							// Num total atoms
		int sub_osapi;						// Num of sub atoms
		int orphan_osapi;					// From the Total, How many orphans exist (for debug)
		int nvmeibc;						// Number of nvmeibc instances connected to nvmeiba
	} n;
	struct {								// Tiny proc directory of nvmeiba
		struct proc_dir_entry *dir;
		void *version_file;					// Version, may vary from nvmeibc/version
		void *users_file;					// User tasks
		void *status_file;					// Status of all atoms in human readable format
		void *status_json;					// Status of all atoms in machine readable format
	} proc;
	struct block_device_operations default_fops;	// Default file operations for all atoms. Can be overwritten by inheriting class for each instance
#if !KS_REQUEST_QUEUE_HAS_REQUEST_FN
	struct block_device_operations upgrade_fops;	// Upgrade file operations for all atoms. Can be overwritten by inheriting class for each instance
	struct block_device_operations detaching_fops;	// Detaching file operations for all atoms. Can be overwritten by inheriting class for each instance
#endif
};

/************************** PRIVATE API for ATOM ******************************/
struct nvmeiba_atom_os_api;
char *nvmeiba_atom_get_string_status(const struct nvmeiba_atom_os_api *);

/******************* PRIVATE API for List of all ATOMS ************************/
void nvmeiba_os_apis_add(struct nvmeiba_atom_os_api *os);		// Call upon creating of block device (attach). Adding atom     to list of all atoms
void nvmeiba_os_apis_del(struct nvmeiba_atom_os_api *os);		// Call upon deletion of block device (detach). Removing atom from list of all atoms
struct nvmeiba_atom_os_api *nvmeiba_os_apis_adopt_by(const char* dev_dir, const char *dev_name);	// Find orphan atom in the list by name (for adoption)
void                        nvmeiba_os_apis_abandon_by(struct nvmeiba_atom_os_api *);	// Find non orphan atom in the list by name (for abandoning)
void nvmeiba_os_apis_set_default_fops(struct block_device_operations *fops);	// Fill struct with default values.
void nvmeiba_os_apis_set_default_pops(const struct block_device_operations **fops);	// Redirect pointer to struct with default values
#if !KS_REQUEST_QUEUE_HAS_REQUEST_FN
void nvmeiba_os_apis_set_upgrade_fops(struct block_device_operations *fops);		// Fill struct with upgrade values.
void nvmeiba_os_apis_set_upgrade_pops(const struct block_device_operations **fops);	// Redirect pointer to struct with upgrade values
void nvmeiba_os_apis_set_detaching_fops(struct block_device_operations *fops);		// Fill struct with upgrade values.
void nvmeiba_os_apis_set_detaching_pops(const struct block_device_operations **fops);	// Redirect pointer to struct with upgrade values
#endif
#endif
