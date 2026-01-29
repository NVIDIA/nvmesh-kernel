/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

/* request types */
REQUEST_FOR(rpcrdma_stop)

REQUEST_FOR(rpcrdma_add_device)
REQUEST_FOR(rpcrdma_remove_device)
REQUEST_FOR(rpcrdma_exit_device)
REQUEST_FOR(rpcrdma_register_address)
REQUEST_FOR(rpcrdma_register_client)
REQUEST_FOR(rpcrdma_register_server)
REQUEST_FOR(rpcrdma_register_service)
REQUEST_FOR(rpcrdma_unregister_service)

REQUEST_FOR(rpcrdma_start_dummy_server)
REQUEST_FOR(rpcrdma_start_dummy_client)
REQUEST_FOR(rpcrdma_dummy_remove_port)
REQUEST_FOR(rpcrdma_dummy_test)
REQUEST_FOR(rpcrdma_dummy_test_comp)
REQUEST_FOR(rpcrdma_ib_event)
REQUEST_FOR(rpcrdma_address)
REQUEST_FOR(rpcrdma_discovery_req)
REQUEST_FOR(rpcrdma_discovery_rep)
REQUEST_FOR(rpcrdma_connect_payload)
REQUEST_FOR(rpcrdma_accept_payload)
REQUEST_FOR(rpcrdma_path_send_error)

REQUEST_FOR(rpcrdma_cm_event)
REQUEST_FOR(rpcrdma_connect)
REQUEST_FOR(rpcrdma_disconnect)
REQUEST_FOR(rpcrdma_server)
REQUEST_FOR(rpcrdma_server_unreg)
REQUEST_FOR(rpcrdma_memory_registration)
REQUEST_FOR(rpcrdma_memory_unregistration)

REQUEST_FOR(rpcrdma_server_tester_timer)
REQUEST_FOR(rpcrdma_server_tester_add_addr)
REQUEST_FOR(rpcrdma_server_tester_listen)
REQUEST_FOR(rpcrdma_server_tester_accept)

REQUEST_FOR(rpcrdma_client_tester_timer)
REQUEST_FOR(rpcrdma_client_tester_add_addr)
REQUEST_FOR(rpcrdma_client_tester_connect)
REQUEST_FOR(rpcrdma_client_tester_disconnect)
REQUEST_FOR(rpcrdma_client_tester_request)
REQUEST_FOR(rpcrdma_server_tester_reply)

REQUEST_FOR_SEP_E(rpcrdma_last_request)

