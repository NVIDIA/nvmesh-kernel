#ifndef NVMEIBT_NM_HW_IFACE_H
#define NVMEIBT_NM_HW_IFACE_H
#include "nvmeibt_nm.h"
#include "nvmeibt_ib_common.h"

void nvmeibt_nm_hw_fill_hw_function_table(struct nvmeibt_nm_hw_function_table *in);
/* Allocate a local node object with the needed hw extensions */
struct nvmeibt_nm_local_node * nvmeibt_nm_hw_allocate_local_node(void);
/* Init a local node object with the needed hw extensions */
int nvmeibt_nm_hw_init_local_node(struct nvmeibt_nm_local_node *ln);
/* Free a local node oject with the needed hw extenstions */
void nvmeibt_nm_hw_free_local_node(struct nvmeibt_nm_local_node *ln);
/* Add hw extensions for init NM module */
int nvmeibt_nm_hw_init(struct nvmeibt_nm_local_node *ln);
/* Init the nics object under local node, this should at least set n_nics and allocates nics */
int nvmeibt_nm_hw_init_nics(struct nvmeibt_nm_local_node *ln);
/* Attach a nic by index to toma NM - this will allocate ans start using the iface */
int nvmeibt_nm_hw_attach_nic(struct nvmeibt_nm_local_node *pn, int idx);

/* Requests related apis - TODO:NM might be deleted */
struct nvmeibt_nm_req * nvmeibt_nm_hw_get_req(void);
void nvmeibt_nm_hw_clear_req(struct nvmeibt_nm_req *req);
void nvmeibt_nm_hw_set_remote_nic_event(struct nvmeibt_nm_req *req, struct nvmeibt_nic *nic);

/* Allocate a path with hw extensions */
struct nvmeibt_nm_path * nvmeibt_nm_hw_allocate_path(void);
/* Restart a path - hw extension */
void nvmeibt_nm_hw_restart_path(struct nvmeibt_nm_path *path);

void nvmeibt_nm_hw_set_conneting_path(struct nvmeibt_nm_path *path);

/* Extension for hw to free path */
void nvmeibt_nm_hw_free_path(struct nvmeibt_nm_path *path);
/* Extnesion for hw to resolve a path, this must call nvmeibt_nm_path_resolve_wait  when done*/
int nvmeibt_nm_hw_resolve_path(struct nvmeibt_nm_path *path);
/* Allocate remote addr with hw extensions */
struct nvmeibt_nm_remote_addr * nvmeibt_nm_hw_allocate_remote_addr(void);
/* Some SRMS needed apis for hw */
int nvmeibt_nm_hw_get_max_chunk_size(struct nvmeibt_nm_path *path);
void * nvmeibt_nm_hw_alloc_msgs_buffer(struct nvmeibt_nm_path *path);
void nvmeibt_nm_hw_path_release_send_buffer(struct nvmeibt_nm_path *path);
int nvmeibt_nm_hw_path_send_msg(struct nvmeibt_nm_path *path, struct nvmeibt_wire_msg *wire, uint16_t msg_id, bool signal);
int nvmeibt_nm_hw_srm_get_data_offset_size(void);

/* Connect a path using speicfic hw */
int nvmeibt_nm_hw_try_connect_path(struct nvmeibt_nm_path *path, bool first);

/* Keep a live path */
int nvmeibt_nm_hw_send_ping(struct nvmeibt_nm_path *path, int is_response, uint8_t ping_id, int retry_count);

/* On new connection hw specific implenetion */
int nvmeibt_nm_hw_reject_connection(void *data, uint32_t reject_reason, struct nvmeibt_nm_login_data *login_data);
int nvmeibt_nm_hw_accept_connection(struct nvmeibt_nm_path *path, struct nvmeibt_nm_per_port *port,
									struct nvmeibt_nm_login_data *login_data, void *data);

/* HW allocation for login_data */
struct nvmeibt_nm_login_data * nvmeibt_nm_hw_allocate_login_data(void);

/* Should return the string of port state */
const char * nvmeibt_nm_hw_get_pp_state(struct nvmeibt_nm_per_port *pp);

int nvmeibt_nm_hw_create_port(struct nvmeibt_nm_per_port *pp);
void nvmeibt_nm_hw_free_port(struct nvmeibt_nm_per_port *pp);

void nvmeibt_nm_hw_free_nic(struct nvmeibt_nm_per_nic *pn);

bool nvmeibt_nm_hw_should_revive_port(struct nvmeibt_nm_per_port *pp);

/* An hook for event loop */
void nvmeibt_nm_hw_wait_events(struct nvmeibt_nm_local_node *ln);

/* Get the port for the connection */
unsigned short nvmeibt_nm_hw_get_port(void);
#endif