#ifndef NVMEIBT_REGISTER
#define NVMEIBT_REGISTER

#include "nvmeibt_common.h"
#include "nvmeibt_ds.h"
#include "nvmeibt_params.h"
#include "clnt/nvmeibt_client_protocol.h"
#include "../common/nvmeib_shared.h"
#include "clnt/nvmeibt_client.h"

/*******************    Registrant    **********************/

enum REG_TIMEOUT_REASON {
	// The order is meaningful. UNREG is stronger than SW_TOPO
	REG_TIMEOUT_REASON_NONE		= 0x0,
	REG_TIMEOUT_REASON_SW_TOPO	= 0x1 << 0,
	REG_TIMEOUT_REASON_UNREG	= 0x1 << 1,
	REG_TIMEOUT_REASON_DEL_ME	= 0x1 << 2,
};

struct nvmeibt_seg_active;
struct nvmeibt_topology;
struct nvmeibt_registrant_ctx;

struct nvmeibt_registrant_awaiting_lockid {
	struct nvmeibt_registrant_ctx 		*reg_ctx;
	union nvmeib_lock_id				stale_lockid_provided_by_registrant;
	struct xdlist 						registrant_link;
	struct xdlist 						awaiting_lockid_link;
};

struct nvmeibt_registrant_ctx {
	unsigned long long					client_messaging_handle;
	unsigned int						client_protocol_version;
	int									config_version;
	int									volume_config_version;
	union nvmeib_lock_id				reg_lock_id;	// Note: purified. I.e., does not include stale_bit etc.
	int									praid_version;
	struct nvmeibt_urn_uuid				registrant_node_id;
	u64									client_conversation_index;		// Last conversation ID with client about the reg_lock_id
    //RH: Dont send the following members and pointers-members above, on wire
	struct nvmeibt_client				*client;
	struct nvmeibt_node					*registrant_node;
	struct nvmeibt_seg_active			*seg_active;
	union nvmeib_uuid					seg_uuid;
	BOOL								is_force_cmd_called;
	BOOL								is_processing_registrant_removal;
	BOOL								is_purging_lock_id_cache;
	BOOL								is_register_for_cold_recovery;
	BOOL								is_client_waiting_for_ack;
	BOOL								is_deleted;
	BOOL								ref_cnt;
	BOOL								is_recoverer;
	u8									is_client_warrant_safe_to_rereg;
	struct timespec						reg_disconnect_time;
	struct timespec						timeout_time;
	struct xdlist						registrant_on_timeout_link;
	struct xdlist						active_link;
	struct xdlist						active_link_by_cid;
	struct xdlist						stale_link;
	struct xdlist						longing_link;
	XDLIST_DECLARE(, struct nvmeibt_registrant_awaiting_lockid, registrant_link) locks_awaited_by_registrant;
	enum REG_TIMEOUT_REASON				timeout_reason;
	int									n_stale_locks;	// The stale_locks are held disk_segment->stale_locks_hash
	u64									reservation_mode_version;
};

#define nvmeibt_registrant_ctx_get_host_name(reg_ctx)	(reg_ctx)->client->net.host_name
#define nvmeibt_registrant_ctx_get_cuuid(    reg_ctx)	(reg_ctx)->client->client_provided_uuid
//#define nvmeibt_registrant_ctx_get_suuid(    reg_ctx)   (reg_ctx)->seg_active->disk_segment->uuid
struct nvmeibt_register_msg {
	enum NVMEIBT_CLIENT_MSG_TYPES	msg_type;
	int								reason;
	unsigned long long				topology_version;
	int								disk_segment_idx;
	int								registrant_node_idx;
	struct nvmeibt_registrant_ctx	registrant_ctx;
	unsigned int					data_length;
	u64								cookie;
	char							*msg_data;
};

#define NREGISTER_MSG_DUMP(name, msg_type, reason, reg_ctx, data_len, cookie) do {							\
		const struct nvmeibt_seg_active 	*seg_active = (reg_ctx)->seg_active;							\
		N_Tf(name, "msgtype=@MSGTYPE reason=@REASON_STR "													\
					"clnt_host=@MY_HOSTNAME seg=@UUID_8 "													\
					"praid_ver=@X(seg=@X) "																	\
					"lock_id=@C_LID data_len=@DATA_LEN handle=@HANDLE cookie=@COOKIE",						\
			nvmeibt_protocol_client_msg_str((enum NVMEIBT_CLIENT_MSG_TYPES)msg_type),						\
			nvmeibt_protocol_client_msg_reason_str((enum NVMEIBT_CLIENT_TR_REASON)reason),					\
			(reg_ctx)->client->net.host_name,																\
			nvmeib_uuid_first_4_bytes(&((reg_ctx)->seg_uuid)),												\
			(reg_ctx)->praid_version,																		\
			(seg_active ? seg_active->topo_for_clients.header.praid_version : 0),							\
			nvmeib_lockid_purify((reg_ctx)->reg_lock_id),													\
			(data_len),																						\
			(reg_ctx)->client_messaging_handle, 															\
			(cookie));																						\
		} while (0)

static inline bool nvmeibt_register_is_client_delete_in_the_air(const struct nvmeibt_registrant_ctx *reg_ctx)
{
	return (!reg_ctx || nvmeibt_client_is_delete_in_the_air(reg_ctx->client));
}

struct nvmeibt_topology;
struct nvmeibt_disk_segment;
struct nvmeibt_disk_segment_topo_ctx;
struct nvmeibt_praid;
struct nvmeibt_praid_topo_ctx;
struct nvmeibt_disk;
struct nvmeibt_local_disk;

BOOL nvmeibt_register_is_processing_registrant_removal(const struct nvmeibt_registrant_ctx *reg_ctx);
void nvmeibt_register_validate_n_active_vs_n_applied(void);

int nvmeibt_register_send_msg_to_registrant(
				struct nvmeibt_registrant_ctx *registrant_ctx,
				enum NVMEIBT_CLIENT_MSG_TYPES msg_type,
				enum NVMEIBT_CLIENT_TR_REASON reason,
				int data_length,
				void *data);

//TOMA_NOT_READY msg is sent in different cases, one of them is when TOMA receives msg from client
//in protocol version, it does not understand. In such case, it will attach some additional
//information from the original msg.
void nvmeibt_register_send_toma_not_ready(
	struct nvmeibt_registrant_ctx *reg_ctx,
	enum NVMEIBT_CLIENT_TR_REASON reason,
	struct nvmeibt_client_msg_summary* prior_msg_smr /*may be null*/);

int nvmeibt_register_launch_disconnected_client_removal_from_all_segments(int client_id, struct nvmeibt_node *registrant_node);

void free_reg_ctx(struct nvmeibt_registrant_ctx *reg_ctx);
BOOL nvmeibt_register_is_same_registrant(const struct nvmeibt_registrant_ctx *r1, const struct nvmeibt_registrant_ctx *r2);	// Important! Assumes questen is asked on registrants of the same segment!
void nvmeibt_register_recalc_seg_active_registrants_align_with_sync_cmd(struct nvmeibt_seg_active *seg_active);
BOOL nvmeibt_register_is_seg_active_registrable_clients_sync_wise(struct nvmeibt_seg_active *seg_active, int *reason);
BOOL nvmeibt_register_is_seg_active_accepting_registrations(struct nvmeibt_seg_active *seg_active, int *reason);
void calc_next_wait_for_registrant_timeout(void);
BOOL nvmeibt_register_is_any_registered_on_seg_active(const struct nvmeibt_seg_active *seg_active);
BOOL nvmeibt_register_is_any_registered_on_disk(const struct nvmeibt_disk *disk);
BOOL nvmeibt_register_is_any_registered(void);
bool nvmeibt_register_is_seg_lot_registrable_topo_wise(struct nvmeibt_seg_lot *seg_lot, int *reason);
struct nvmeibt_registrant_ctx *nvmeibt_register_lookup_stale_registrant_by_reg_lock_id(
	struct nvmeibt_seg_active *seg_active, union nvmeib_lock_id reg_lock_id);
struct nvmeibt_registrant_ctx *nvmeibt_register_lookup_active_registrant_by_reg_lock_id(
	struct nvmeibt_seg_active *seg_active, union nvmeib_lock_id reg_lock_id);
struct nvmeibt_registrant_ctx *nvmeibt_register_lookup_active_registrant_by_uuid( struct nvmeibt_seg_active *seg_active, const union nvmeib_uuid *uuid);
void nvmeibt_register_totally_remove_registrant(struct nvmeibt_registrant_ctx *input_registrant_ctx);
void nvmeibt_register_make_all_seg_active_registrants_sync_praid_topology(struct nvmeibt_seg_active *seg_active);
void nvmeibt_register_clients_sync_check_and_act_upon(struct nvmeibt_seg_active *seg_active);
void nvmeibt_register_move_all_longing_registrants_no_seg_to_seg(struct nvmeibt_seg_active *seg_active);
void nvmeibt_remove_longing_registrant_on_invalid_seg(unsigned long long closed_messaging_handle);
int nvmeibt_register_handle_incoming_message(struct nvmeibt_register_msg *msg);
struct timespec nvmeibt_register_get_next_timeout_timespec(void);
void nvmeibt_register_close_seg_active_for_registration(struct nvmeibt_seg_active *seg_active, bool is_brute_force_disconnect_required);
void nvmeibt_register_close_all_seg_actives_for_registration(void);
void nvmeibt_register_open_disk_eligible_seg_actives_for_use(struct nvmeibt_local_disk *local_disk);
struct nvmeibt_registrant_ctx *nvmeibt_register_get_out_reg_ctx_by_in_msg(struct nvmeibt_register_msg *msg);
void nvmeibt_register_MR_open_seg_active_for_registrations_if_eligable(struct nvmeibt_seg_active *seg_active);
int nvmeibt_register_timeout_occurred(void);
void nvmeibt_register_terminate_registrant(struct nvmeibt_registrant_ctx *reg_ctx, BOOL is_force);
void nvmeibt_register_brute_force_cleanup_all_seg_registrants(struct nvmeibt_seg_active *seg_active);
void nvmeibt_register_remove_all_recovery_active_registrants_of_node(struct nvmeibt_urn_uuid *src_node_id);
int nvmeibt_register_launch_seg_metadata_ctrl_save(struct nvmeibt_seg_active *seg_active);
void nvmeibt_register_open_all_eligible_seg_actives_for_use(void);
void nvmeibt_register_handle_new_reservation_mode(struct nvmeibt_seg_active *seg_active, u64 reservation_mode_version);

bool remove_longing_registrant_on_seg_by_ctx(struct nvmeibt_registrant_ctx *input_reg_ctx, bool is_by_cid);

int nvmeibt_register_print_status(
	int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx,
	struct nvmeibt_seg_active *seg_active);
void nvmeibt_register_set_brute_force_test(bool is_tested);
#endif	// #ifndef NVMEIBT_REGISTER

