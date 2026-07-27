#ifndef NVMEIBM_MCS_SIMU_H
#define NVMEIBM_MCS_SIMU_H

// Simulating conenction of client-management
#include "nvmeibm_mgmt_simu.h"
#include "nvmeib_mcs.h"

/* API for unitesting environment, allows injecting mcs messages with errors */
enum NVMEIB_MCS_MSG_ERROR_TYPES{
	NVMEIB_MCS_MSG_WITH_NO_ERROR        = 0,
	NVMEIB_MCS_MSG_WITH_BAD_HEADER      = 1,
	NVMEIB_MCS_MSG_WITH_BAD_SCHEME      = 2,
	NVMEIB_MCS_MSG_WITH_BAD_CONFIG      = 4,  // TODO: use this once the mgmt to client protocol is finalized
	NVMEIB_MCS_MSG_WITH_BAD_OPCODE      = 5,
	NVMEIB_MCS_MSG_WITH_BUFFER_OVERFLOW = 6,
	NVMEIB_MCS_MSG_DROP_TARGET          = 7,
	NVMEIB_MCS_MSG_DROP_NIC	            = 8,
	NVMEIB_MCS_MSG_DROP_DISK            = 9,
	NVMEIB_MCS_MSG_WITH_BAD_MESSAGE_VER = 10
};

#include "./mgmt/nvmeibm_conf_db.h"
int generate_mcs_attach_message_for_volume(struct mcs_simu *mcs, const struct volumeDescriptor *vol, const int opcode, const int preempt, const char *token, const enum NVMEIB_MCS_MSG_ERROR_TYPES error); // Used in bunitest to override CLI to test wrong configuration
int generate_mcs_attach_message_for_volume_with_res_version(struct mcs_simu *mcs, const struct volumeDescriptor *vol, int opcode, int preempt, const char *token, const enum NVMEIB_MCS_MSG_ERROR_TYPES error, unsigned long long version);
int generate_mcs_delete_message_for_volume(struct mcs_simu *mcs, const struct volumeDescriptor *vol, const int opcode); // Used in bunitest, mgmt issues delete volume msg thorugh mcs (before or after detach)
int generate_mcs_update_token(struct mcs_simu *mcs,	long long messageSequence,
	long long reportID, long long clientToken, unsigned int keepalive_interval, const enum NVMEIB_MCS_MSG_ERROR_TYPES error);

int generate_mcs_fake_update_targets_nics(struct mcs_simu *mcs);
int generate_mcs_invalid_operation(struct mcs_simu *mcs);
//Set expected counters in the mgmt simulator. If the value passed is 0, the expected counter would not be changed
void mcs_set_expected_counters(struct mcs_simu *mcs, long long expected_sequence_num,
	long long expected_reportID, long long expected_client_token, unsigned long expected_keepalive_interval);

int mcs_send_raw_msg_buf_unsafe(const struct c_api_proc *mcs, char *msg, unsigned len);			// Push a given buffer to clnts proc file as message (typically one of the messages above). Use with greate care!

int mgmt_incoming_msg_from_clnt_cb(void *mcs, const char *buf, size_t len);

#endif // NVMEIBM_MCS_SIMU_H
