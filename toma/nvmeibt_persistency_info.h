#ifndef NVMEIBT_PERSISTENCY_INFO
#define NVMEIBT_PERSISTENCY_INFO

#include "nvmeibt_common.h"
#include "nvmeibt_ds.h"
#include "nvmeibt_local_disk.h"
#include "nvmeibt_wq.h"
#include "nvmeibt_ds_metadata.h"

struct disk_segment_info
{
	struct seg_persistency_write_params 	write_params;
	struct xdlist							link;
};

// Holds a copy of the disk's parameters & data to store on the disk (in case it changes - shouldn't)
struct local_disk_info
{
	struct nvmeibt_local_disk_config					from_config;
	int 												fd;
	struct netlink_io_context							*nl_ctx;
	int													gpt_change_no;
	BOOL												is_gpt_written;
	BOOL                                                is_PMBR_saved_on_disk;
	struct nvmeibt_disk_mbr								mbr;
	struct nvmeibt_disk_gpt 							main_gpt;
	struct nvmeibt_disk_gpt 							metadata_gpt;
	struct nvmeibt_local_disk							*local_disk_in_info;
	struct xdlist										link;
};

struct raft_persistency {
	unsigned long long				current_term;									// 8
	unsigned long long				last_rx_append_entries_term;					// 16
	int64_t							kafka_mgmt_zone_number;							// 24
/*
   - When we have a new leader (higher term)
	 - The follower persists the TOPO even if it carries the same number
	 - The follower idx's are considered committed only if the committed
	   term is the leader's
	 - The leader reads the remote active (applied) values only if the
	   follower committed to this leader
	 - In the future, the leader will send the missing incremental updates
	   accordingly (missing offsets, and topo changes including the committed)
*/
	union nvmeib_uuid				voted_for_raft_member_uuid;					// 40
	union nvmeib_uuid				mgmt_DB_uuid;								// 56
	uint32_t						raft_ctx_crc;								// 60
	uint32_t						guaranteed_sw_ver;							// 64
	int64_t							raft_calculated_leader_append_entries_rep_time_ns;	// 72
	int64_t							raft_calculated_leader_topo_calc_time_ns;	// 80
	int64_t							reserved2;									// 88
} __attribute__((packed));

#define DUMP_RAFT_PERSISTENCY(name, _persist_and_wire_buf) ({																																\
	const struct nvmeibt_persist_and_wire_buf	*b = _persist_and_wire_buf;																													\
	union nvmeib_uuid							_voted_for_uuid = persist_and_wire_buf_get_raft_voted_for_uuid(b);																			\
	union nvmeib_uuid							_mgmt_DB_uuid = persist_and_wire_buf_get_raft_mgmt_DB_uuid(b);																				\
	N_Tf(name, 	"current_term=@LLX last_rx_append_entries_term=@LLX kafka_mgmt_zone_number=@INT64_TD voted_for_raft_member_uuid=@UUID_LE mgmt_DB_uuid=@UUID_LE raft_ctx_crc=@X "			\
				"guaranteed_sw_ver=@X raft_calculated_leader_append_entries_rep_time_ns=@INT64_TD raft_calculated_leader_topo_calc_time_ns=@INT64_TD reserved2=@INT64_TD",					\
				persist_and_wire_buf_get_current_raft_TERM(b), persist_and_wire_buf_get_last_rx_append_entries_raft_TERM(b), persist_and_wire_buf_get_raft_kafka_mgmt_zone_number(b),		\
				&_voted_for_uuid, &_mgmt_DB_uuid, persist_and_wire_buf_get_raft_ctx_crc(b), persist_and_wire_buf_get_raft_guaranteed_sw_ver(b),												\
				persist_and_wire_buf_get_raft_calculated_leader_append_entries_rep_time_ns(b), persist_and_wire_buf_get_raft_calculated_leader_topo_calc_time_ns(b), b->raft_ctx.reserved2);\
})

#define TLV_TYPE_UNKNOWN				((int8_t)0)
#define TLV_TYPE_KAFKA_MGMT_CONFIG_FULL ((int8_t)1)
#define TLV_TYPE_TOPO_FULL				((int8_t)2)
#define TLV_TYPE_TOPO_CONFIG_FULL		((int8_t)3)
#define TLV_TYPE_RAFT_MEMBERS_FULL		((int8_t)4)

struct nvmeibt_wire_type_len_value {
	int16_t						reserved_1;				// 2
	int8_t						v3_3_kafka_topic_change_no;	// 3
	int8_t						UNUSED_tlv_type;		// 4
	int							tlv_len;				// 8
	uint32_t					tlv_crc;				// 12
	int							reserved_2;				// 16
	int64_t						tlv_idx;				// 24	// Not part of a standard TLV, holds the kafka_offset, topo_version, etc.
	int64_t						seq_no;					// 32	// Since our kafka queue is not ordered, when needed (raft_members), we added a seq_no
} __attribute__((packed));

#define DUMP_TLV(__name, __tlv) ({																																\
	struct nvmeibt_wire_type_len_value		*t = (__tlv);																										\
	N_Tf(__name, "TLV: v3_3_kafka_topic_change_no=@INT8_TD len=@INT CRC=@X idx=@LLX seq_no=@INT64_TD",															\
		 nvmeibt_tlv_get_v_3_3_kafka_topic_change_no(t), nvmeibt_tlv_get_len(t), nvmeibt_tlv_get_CRC(t), nvmeibt_tlv_get_idx(t), nvmeibt_tlv_get_seq_no(t));	\
})


static inline int64_t nvmeibt_tlv_get_idx(const struct nvmeibt_wire_type_len_value *tlv)
{
	return LE_SWAP64(tlv->tlv_idx);
}

static inline int64_t nvmeibt_tlv_get_seq_no(const struct nvmeibt_wire_type_len_value *tlv)
{
	return LE_SWAP64(tlv->seq_no);
}

static inline int32_t nvmeibt_tlv_get_len(const struct nvmeibt_wire_type_len_value *tlv)
{
	return LE_SWAP32(tlv->tlv_len);
}

static inline int8_t nvmeibt_tlv_get_type(const struct nvmeibt_wire_type_len_value *tlv)
{
	return LE_SWAP8(tlv->UNUSED_tlv_type);
}

static inline int8_t nvmeibt_tlv_get_v_3_3_kafka_topic_change_no(const struct nvmeibt_wire_type_len_value *tlv)
{
	return LE_SWAP8(tlv->v3_3_kafka_topic_change_no);
}

static inline int32_t nvmeibt_tlv_get_CRC(const struct nvmeibt_wire_type_len_value *tlv)
{
	return LE_SWAP32((int)(tlv->tlv_crc));
}

struct nvmeibt_persist_and_wire_buf {
	int32_t												buf_sw_ver;					// 4
	int													persist_and_wire_total_len;	// 8
	struct raft_persistency								raft_ctx;					// 96
	struct nvmeibt_wire_type_len_value					topo_ctx;					// 128
	struct nvmeibt_wire_type_len_value					topo_config_ctx;			// 160
	struct nvmeibt_wire_type_len_value					kafka_mgmt_config_ctx;		// 192
	struct nvmeibt_wire_type_len_value					raft_members_ctx;			// 224
	char												data[0];		// The entire buff is allocated with spare for this data
																								// packed
																								// Here we have the data of the 4 sections (topo, config, ...) in the same order
} __attribute__((packed));

static inline int persist_and_wire_buf_get_total_len(const struct nvmeibt_persist_and_wire_buf *buf)
{
	return LE_SWAP32(buf ? buf->persist_and_wire_total_len : 0);
}

static inline void persist_and_wire_buf_recalc_raft_ctx_crc_as_needed(struct nvmeibt_persist_and_wire_buf *buf, bool is_recalc_crc)
{
	if (is_recalc_crc) {
		buf->raft_ctx.raft_ctx_crc = 0;
		buf->raft_ctx.raft_ctx_crc = LE_SWAP32(crc32(0, &buf->raft_ctx, sizeof(buf->raft_ctx)));
	}
}

static inline uint32_t persist_and_wire_buf_get_raft_ctx_crc(const struct nvmeibt_persist_and_wire_buf *buf)
{
	return LE_SWAP32(buf ? buf->raft_ctx.raft_ctx_crc : 0);
}

static inline unsigned long long persist_and_wire_buf_get_current_raft_TERM(const struct nvmeibt_persist_and_wire_buf *buf)
{
	return LE_SWAP64(buf ? buf->raft_ctx.current_term : 0);
}

static inline union nvmeib_uuid persist_and_wire_buf_get_raft_voted_for_uuid(const struct nvmeibt_persist_and_wire_buf *buf)
{
	return swap_uuid_LE_BE(&(buf->raft_ctx.voted_for_raft_member_uuid));
}

static inline union nvmeib_uuid persist_and_wire_buf_get_raft_mgmt_DB_uuid(const struct nvmeibt_persist_and_wire_buf *buf)
{
	return swap_uuid_LE_BE(&(buf->raft_ctx.mgmt_DB_uuid));
}

static inline void persist_and_wire_buf_set_current_raft_TERM(struct nvmeibt_persist_and_wire_buf *buf, unsigned long long current_raft_term, bool is_recalc_crc)
{
	buf->raft_ctx.current_term = LE_SWAP64(current_raft_term);
	persist_and_wire_buf_recalc_raft_ctx_crc_as_needed(buf, is_recalc_crc);
}

static inline unsigned long long persist_and_wire_buf_get_last_rx_append_entries_raft_TERM(const struct nvmeibt_persist_and_wire_buf *buf)
{
	return LE_SWAP64(buf ? buf->raft_ctx.last_rx_append_entries_term : 0);
}

static inline void persist_and_wire_buf_set_voted_for_and_last_rx_append_entries_raft_TERM(struct nvmeibt_persist_and_wire_buf *buf,
																						   const union nvmeib_uuid *voted_for_raft_member_uuid, unsigned long long last_rx_append_entries_term,
																						   bool is_recalc_crc)
{
	buf->raft_ctx.voted_for_raft_member_uuid = swap_uuid_LE_BE(voted_for_raft_member_uuid);
	buf->raft_ctx.last_rx_append_entries_term = LE_SWAP64(last_rx_append_entries_term);
	persist_and_wire_buf_recalc_raft_ctx_crc_as_needed(buf, is_recalc_crc);
}

static inline int64_t persist_and_wire_buf_get_raft_kafka_mgmt_zone_number(const struct nvmeibt_persist_and_wire_buf *buf)
{
	return LE_SWAP64(buf->raft_ctx.kafka_mgmt_zone_number);
}

static inline void persist_and_wire_buf_set_kafka_mgmt_zone_number(struct nvmeibt_persist_and_wire_buf *buf, int64_t kafka_mgmt_zone_number, bool is_recalc_crc)
{
	buf->raft_ctx.kafka_mgmt_zone_number = LE_SWAP64(kafka_mgmt_zone_number);
	persist_and_wire_buf_recalc_raft_ctx_crc_as_needed(buf, is_recalc_crc);
}

static inline void persist_and_wire_buf_set_raft_mgmt_DB_uuid(struct nvmeibt_persist_and_wire_buf *buf, const union nvmeib_uuid *mgmt_DB_uuid, bool is_recalc_crc)
{
	buf->raft_ctx.mgmt_DB_uuid = swap_uuid_LE_BE(mgmt_DB_uuid);
	persist_and_wire_buf_recalc_raft_ctx_crc_as_needed(buf, is_recalc_crc);
}

static inline uint32_t persist_and_wire_buf_get_struct_version(const struct nvmeibt_persist_and_wire_buf *buf)
{
	return LE_SWAP32(buf ? buf->buf_sw_ver : -1);
}

static inline int64_t persist_and_wire_buf_get_raft_calculated_leader_append_entries_rep_time_ns(const struct nvmeibt_persist_and_wire_buf *buf)
{
	return (buf ? LE_SWAP64(buf->raft_ctx.raft_calculated_leader_append_entries_rep_time_ns) : 0);
}

static inline void persist_and_wire_buf_set_raft_calculated_append_entries_rep_time_ns(struct nvmeibt_persist_and_wire_buf *buf, int64_t raft_calculated_append_entries_rep_time_ns, bool is_recalc_crc)
{
	buf->raft_ctx.raft_calculated_leader_append_entries_rep_time_ns = LE_SWAP64(raft_calculated_append_entries_rep_time_ns);
	persist_and_wire_buf_recalc_raft_ctx_crc_as_needed(buf, is_recalc_crc);
}

static inline int64_t persist_and_wire_buf_get_raft_calculated_leader_topo_calc_time_ns(const struct nvmeibt_persist_and_wire_buf *buf)
{
	return (buf ? LE_SWAP64(buf->raft_ctx.raft_calculated_leader_topo_calc_time_ns) : 0);
}

static inline void persist_and_wire_buf_set_raft_calculated_topo_calc_time_ns(struct nvmeibt_persist_and_wire_buf *buf, int64_t raft_calculated_topo_calc_time_ns, bool is_recalc_crc)
{
	buf->raft_ctx.raft_calculated_leader_topo_calc_time_ns = LE_SWAP64(raft_calculated_topo_calc_time_ns);
	persist_and_wire_buf_recalc_raft_ctx_crc_as_needed(buf, is_recalc_crc);
}

static inline uint32_t persist_and_wire_buf_get_raft_guaranteed_sw_ver(const struct nvmeibt_persist_and_wire_buf *buf)
{
	return (buf ? LE_SWAP32(buf->raft_ctx.guaranteed_sw_ver) : 0);
}

static inline void persist_and_wire_buf_set_guaranteed_software_version(struct nvmeibt_persist_and_wire_buf *buf, uint32_t sw_ver, bool is_recalc_crc)
{
	buf->raft_ctx.guaranteed_sw_ver = LE_SWAP32(sw_ver);
	persist_and_wire_buf_recalc_raft_ctx_crc_as_needed(buf, is_recalc_crc);
}

struct nvmeibt_persistency_wq_entry {
	struct nvmeibt_wq_entry 							wq_entry;
	/* Input */
	struct nvmeibt_Buf									follower_persist_buf_full;
	XDLIST_DECLARE(, struct local_disk_info, link) 		local_disks_info_hash;

	/* Output */
	int 												rv;
	BOOL												is_config_persisted;
	BOOL												is_topo_persisted;
	BOOL												is_req_vote;
};

static inline const char *local_disk_info_display(const struct local_disk_info *ld_info)
{
	return (ld_info ? ld_info->from_config.ld_display : "???");
}

static inline int local_disk_info_pblk_size(const struct local_disk_info *ld_info)
{
	return (ld_info ? (ld_info->from_config.pblk_size) : -1);
}

static inline int local_disk_info_fd(const struct local_disk_info *ld_info)
{
	return (ld_info ? (ld_info->fd) : -1);
}

static inline struct netlink_io_context *local_disk_info_nl_ctx(const struct local_disk_info *ld_info)
{
	return (ld_info ? (ld_info->nl_ctx) : NULL);
}

#endif // #ifndef NVMEIBT_LOCAL_DISK_INFO

