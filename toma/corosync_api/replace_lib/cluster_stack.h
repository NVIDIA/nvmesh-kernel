/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#ifndef CLUSTER_STACK_H
#define CLUSTER_STACK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Plugin API structure (api towards pace maker)
typedef uint32_t nodeid_t;
typedef struct cluster_plugin_api_s {
	// Version and identification
	int (*plugin_version)(void);
	const char *(*plugin_name)(void);

	// Lifecycle management
	int (*plugin_init)(void);
	int (*plugin_shutdown)(void);

	// Node information
	int (*node_id_get)(nodeid_t *id);
	int (*node_name_get)(char **name);
	int (*node_uname_get)(nodeid_t nodeid, char **uname);
	int (*node_list_get)(unsigned int *node_count, nodeid_t **node_list);

	// Quorum management
	int (*quorum_getquorate)(void);
	int (*quorum_register_callback)(void (*callback)(unsigned int quorate));

	// Messaging
	int (*cluster_send      )(nodeid_t nodeid, const void *data, size_t length);
	int (*cluster_sendto_all)(                 const void *data, size_t length);
	int (*cluster_message_callback_set)(void (*callback)(nodeid_t nodeid, const void *msg, size_t length));

	// Membership
	int (*membership_register)(void (*callback)(const unsigned int *members, size_t count));
	int (*membership_get)(unsigned int *count, nodeid_t **members);

	// Logging
	int (*log_set_function)(void (*log_fn)(int priority, const char *fmt, ...));

} cluster_plugin_api_t;

// Main plugin entry point
const cluster_plugin_api_t *cluster_plugin_init(void);


#endif /* CLUSTER_STACK_H */