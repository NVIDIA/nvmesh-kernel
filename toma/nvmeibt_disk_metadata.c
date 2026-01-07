#include "nvmeibt_debug.h"
#include "../common/nvmeib_shared.h"
#include "nvmeibt_disk_metadata.h"
#include "nvmeibt_node.h"
#include "nvmeibt_disk_segment.h"
#include "nvmeibt_common.h"
#include "nvmeibt_ds_metadata.h"
#include "nvmeibt_persistency_info.h"
#include "nvmeibt_toma.h"
#include "nvmeibt_local_disk.h"
#include "nvmeibt_global.h"

#define MBR_OFFSET    0
#define MIDST_WRITE_GPT_SIGNATURE	0xdeadbeafdeadbeaf

char *gpt_validity_str(enum GPT_VALIDITY validity)
{
	switch (validity) {
	case GPT_VALIDITY_OK:				return "OK";
	case GPT_VALIDITY_CRC_ERR:			return "CRC_ERR";
	case GPT_VALIDITY_MAGIC_ERR:		return "MAGIC_ERR";
	case GPT_VALIDITY_TECHNICAL_ERR:	return "TECHNICAL_ERR";
	case GPT_VALIDITY_DATA_ERR:			return "DATA_ERR";
	case GPT_VALIDITY_MIDST_WRITE:		return "MIDST_WRITE";
	case GPT_VALIDITY_UNKNOWN:
	default:
		return "UNKNOWN";
	}
}

/********************** Upgrade code, Todo: Move to different file ************/
static const unsigned int TOMA_DISK_METADATA_VERSION_V1_3_1 = 0x00010301U;		// Identical to V1_3_2 and V1_3_3, so currently no upgrade is needed
static const unsigned int TOMA_DISK_METADATA_VERSION_V2_8_2 = 0x00020802U;		//

/******************************* Upgrade code End *****************************/
static int store_gpt(struct netlink_io_context *nl_ctx,
					 int stock_fd,
					 const struct nvmeibt_disk_gpt *gpt,
					 int pblk_size,
					 const struct nvmeibt_disk_gpt_header *gpt_header,
					 const struct nvmeibt_disk_gpt_partition_entry *entries,
					 char *gpt_primary_or_alternate_or_mem_str,
					 bool init_serjio);	// Forward declaration

void hexdump(void *bufi, int len, char *str)
{
	unsigned char	*buf = (unsigned char *)bufi;
	int				i = 0, j = 0, line_start = 0;
	int				sum_line = 0;
	bool			is_eol = 1;
	bool			is_eof = (len == 0);
	const int		n_elem_per_line = (1 << 6);
	char			out_txt[(n_elem_per_line * 2 + 15) * 20];

	while (!is_eof) {
		is_eof = (i >= len);
		is_eol = ((i & (n_elem_per_line - 1)) == 0) || is_eof;
		if (is_eol) {
			if (sum_line) {
				line_start = j;	// Accept the prev line
				sum_line = 0;
			} else {
				// Forget an empty line
				out_txt[line_start] = '\0';
				j = line_start;
			}
			if (j > ((int)sizeof(out_txt) - (n_elem_per_line * 2 + 15)) || is_eof) {
				N_Tf(t_g3_tomadmd, "DUMP_ME @STR len=@X - @STR", str, len, out_txt);
				line_start = j = 0;	// Restart writing on out_txt
			}
			j += sprintf(out_txt + j, "\n%08x:", i);	// Start writing the new line
		}
		sum_line += ((buf[i] & 0xff) != 0);
		j += sprintf(out_txt + j, "%02x", (buf[i] & 0xff));
		i++;
	}
}

static void netlink_io_on_done(void *ctx, int is_ok, struct nvmeib_nl_uk_comm_rep *msg)
{
	struct netlink_context_io_data *nl_io_data = ctx;

	NFIN;
	NTOMA_ASSERT(trace_netlink_io_on_done_1, (!msg && !is_ok) || msg->opcode == csc_io_to_disk,
				"netlink done, on non-io msg of type=@TYPE", msg->opcode);

	if (!nl_io_data) {
		 N_Ef(trace_netlink_io_on_done_2, "Cannot wakeup caller thread as netlink ctx is NULL");
		 nvmeibt_abort(ES_FATAL);
	}

	if (pthread_mutex_lock(&nl_io_data->guard_mutex) != 0) {
		N_Ef(trace_netlink_io_on_done_3, "Cannot wakeup caller thread, cannot lock_mutex=@LOCK_MUTEX error: @AUTO_ERRNO", &nl_io_data->guard_mutex);
		nvmeibt_abort(ES_FATAL);
	}

	nl_io_data->rv = is_ok ? 0 : -1;

	// Wake the thread that called this netlink operation.
	if (pthread_cond_signal(&nl_io_data->completion_signal) != 0) {
		N_Ef(trace_netlink_io_on_done_4, "Cannot wakeup caller thread with cond_var=@COND_VAR pthread signal error: @AUTO_ERRNO", &nl_io_data->completion_signal);
		nvmeibt_abort(ES_FATAL);
	}

	if (pthread_mutex_unlock(&nl_io_data->guard_mutex) != 0) {
		N_Ef(trace_netlink_io_on_done_5, "Cannot wakeup caller thread, cannot unlock_mutex=@UNLOCK_MUTEX error: @AUTO_ERRNO", &nl_io_data->guard_mutex);
		nvmeibt_abort(ES_FATAL);
	}

	NFOUT;
}

struct netlink_io_context *nvmeibt_make_netlink_context_from_config(struct nvmeibt_local_disk_config *ldc)
{
	struct netlink_io_context *nl_ctx = NULL;
	struct nvmeib_disk_info di;
	unsigned int max_blocks_per_call;
	int rv = -1;

	NFIN;
	nl_ctx = NNVMEIBT_BM_CALLOC(trace_mnl_1, sizeof(*nl_ctx));
	if (!nl_ctx) {
		N_Ef(trace_mnl_11, "netlink context allocation failure!");
		goto out;
	}

#if defined(LLVM) || defined(__clang__)
	if ((void *)nl_ctx->nl_msg.data != (void *)&nl_ctx->nl_msg_payload) {
		N_Ef(trace_mnl_111, "Bad packing of netlink struct");
		goto out;
	}
#else
	{ _Static_assert((void *)nl_ctx->nl_msg.data == (void *)&nl_ctx->nl_msg_payload, "Bad packing of netlink struct"); }
#endif

	if (pthread_mutex_init(&nl_ctx->nl_io_data.guard_mutex, NULL) != 0) {
		N_Ef(trace_mnl_2, "Failed to create netlink context guard @AUTO_ERRNO");
		goto out;
	}
	if (pthread_condattr_init(&nl_ctx->attr) != 0) {
		N_Ef(trace_mnl_3, "Failed to create cond var attr @AUTO_ERRNO");
		goto out;
	}
	if (pthread_cond_init(&nl_ctx->nl_io_data.completion_signal, &nl_ctx->attr) != 0) {
		N_Ef(trace_mnl_4, "Failed to create netlink context cond var @AUTO_ERRNO");
		goto out;
	}
	if (pthread_mutex_lock(&nl_ctx->nl_io_data.guard_mutex) != 0) {
		N_Ef(trace_mnl_5, "Cannot wakeup caller thread, cannot lock_mutex=@LOCK_MUTEX error: @AUTO_ERRNO",
				&nl_ctx->nl_io_data.guard_mutex);
		goto out;
	}

	nvmeibt_strlcpy(nl_ctx->nl_msg_payload.disk_id, ldc->ldisk_id.str, sizeof(nl_ctx->nl_msg_payload.disk_id));
	max_blocks_per_call = !nvmeibt_km_comm_get_disk_info(
		nvmeibt_get_srv_comm(), nl_ctx->nl_msg_payload.disk_id, &di) ?
		di.max_n_hw_sectors : (unsigned)(ldc->max_request_size - 1); /* we reduce by one for the c ase the disk is formatted with MD */
	N_Df(trace_mnl_6, "max_blocks_per_call=@LEN", max_blocks_per_call);

	nl_ctx->nl_msg_payload.vendor_id = ldc->vendor;
	nl_ctx->nl_msg.opcode = csc_io_to_disk;
	nl_ctx->nl_msg.on_done = netlink_io_on_done;
	nl_ctx->nl_msg.ctx = (void *)&nl_ctx->nl_io_data;
	nl_ctx->nl_msg.len = sizeof(nl_ctx->nl_msg_payload);
	nl_ctx->nl_msg_payload.pid = getpid();
	nl_ctx->pblk_size = ldc->pblk_size;
	nl_ctx->max_request_size = max_blocks_per_call;
	nl_ctx->nl_msg_payload.is_hw = 1;
	nl_ctx->nl_msg_payload.gpt_update_flags = NO_MAIN_GPT_UPDATE;

	rv = 0;

out:
	if (rv < 0) {
		NNVMEIBT_BM_FREE(trace_mnl_7, nl_ctx);
		nl_ctx = NULL;
	}
	NFOUT;
	return nl_ctx;
}

int nvmeibt_netlink_do_io_sync(struct netlink_io_context *nl_ctx)
{
	int rv = -1;
	unsigned int remaining_n_pblks;
	unsigned int data_len_bytes;
    unsigned int max_blocks_per_call = nl_ctx->max_request_size;
    unsigned int allign_bits;
	char	*data = nl_ctx->nl_msg_payload.data;
	unsigned long	pba_s = nl_ctx->nl_msg_payload.start_sector;

	NFIN;

    // Every request data must be alligned to PAGE_SIZE
    allign_bits = (nl_ctx->pblk_size >= PAGE_SIZE) ? 0 : (PAGE_SIZE / nl_ctx->pblk_size - 1);
	max_blocks_per_call &= ~allign_bits;

	remaining_n_pblks = divroundup(nl_ctx->nl_msg_payload.data_len, nl_ctx->pblk_size);
	nl_ctx->nl_io_data.rv = 0;	// Avoid old garbage
	do {
		unsigned int n_pblks = min(remaining_n_pblks, max_blocks_per_call);
		rv = -1;

		if (n_pblks < 1) {
			N_Ef(trace_nnis_10, "n_pblk=@UINT remaining_n_pblks=@INT max_request_size=@INT", n_pblks, remaining_n_pblks, nl_ctx->max_request_size);
			goto out;
		}
		data_len_bytes = n_pblks * nl_ctx->pblk_size;
		nl_ctx->nl_msg_payload.data_len = data_len_bytes;
		nl_ctx->nl_msg_payload.data = data;
		nl_ctx->nl_msg_payload.start_sector = pba_s;

		N_Tf(t_g4_tomadmd,"disk=@STR sector=@ZX data_len=@X md_len=@X is_read=@STR gpt_update_flags=@X",
			nl_ctx->nl_msg_payload.disk_id, nl_ctx->nl_msg_payload.start_sector,
			nl_ctx->nl_msg_payload.data_len, nl_ctx->nl_msg_payload.md_len,
			nl_ctx->nl_msg_payload.is_read ? "true" : "false",
			nl_ctx->nl_msg_payload.gpt_update_flags);

		if (nvmeibt_send_msg_to_srv(&nl_ctx->nl_msg) != 0) {
			N_Ef(trace_nnis_1, "Unable to send netlink msg to srv. disk=@STR vendor=@VENDOR!",
					nl_ctx->nl_msg_payload.disk_id, nl_ctx->nl_msg_payload.vendor_id);
			goto out;
		}

		// Wait for netlink IO to finish
		if (pthread_cond_wait(&nl_ctx->nl_io_data.completion_signal, &nl_ctx->nl_io_data.guard_mutex) != 0) {
			N_Ef(trace_nnis_2, "Cannot wait for netlink IO to finish, cond_var=@COND_VAR error: @AUTO_ERRNO", &nl_ctx->nl_io_data.completion_signal);
			goto out;
		}

		rv = nl_ctx->nl_io_data.rv;
		if (pthread_cond_init(&nl_ctx->nl_io_data.completion_signal, &nl_ctx->attr)) { // for next call
			N_Ef(trace_nnis_3, "pthread_cond_init failed @AUTO_ERRNO");
		}

		data += data_len_bytes;
		pba_s += n_pblks;
		remaining_n_pblks -= n_pblks;
	} while(remaining_n_pblks && rv==0);

out:
	NFOUT;
	return rv;
}

void nvmeibt_netlink_io_free(struct netlink_io_context **nl_ctx_p)
{
	if (nl_ctx_p && *nl_ctx_p) {
		struct netlink_io_context *nl_ctx = *nl_ctx_p;

		if (pthread_mutex_unlock(&nl_ctx->nl_io_data.guard_mutex)) {
			N_Ef(xx_42, "pthread_mutex_unlock failed @AUTO_ERRNO");
		}
		if (pthread_cond_destroy(&nl_ctx->nl_io_data.completion_signal)) {
			N_Ef(xx_43, "pthread_cond_destroy failed @AUTO_ERRNO");
		}
		if (pthread_mutex_destroy(&nl_ctx->nl_io_data.guard_mutex)) {
			N_Ef(xx_44, "pthread_mutex_destroy failed @AUTO_ERRNO");
		}
		NNVMEIBT_BM_FREE(trace_nnif_1, nl_ctx);
		*nl_ctx_p = NULL;
	}
}

static inline void nvmeib_init_io_to_disk(struct nvmeib_io_to_disk *io_to_disk,
								   unsigned long start_sector,
								   char *data, unsigned int data_len,
								   char *md, unsigned int md_len,
								   enum nvmeib_io_is_read is_read, enum nvmeib_main_gpt_update_flags main_gpt_update_flags)
{
	io_to_disk->start_sector = start_sector;
	io_to_disk->data = data;
	io_to_disk->data_len = data_len;
	io_to_disk->md = md;
	io_to_disk->md_len = md_len;
	io_to_disk->is_read = is_read;
	io_to_disk->gpt_update_flags = main_gpt_update_flags;
}

int nvmeibt_disk_metadata_do_sync_IO_with_disk_netlink_or_not(struct netlink_io_context *nl_ctx, const int fd,
										void *buf,
										const uint64_t pbyte_s,
										int pblk_size,
										const int n_bytes,
										char *md, unsigned int md_len,
										enum nvmeib_io_is_read is_read,
										enum nvmeib_main_gpt_update_flags main_gpt_update_flags,
										uint64_t min_offset_allowed)
{
	int			rv = -1;
	void		*tmp_buf = buf;
	uint64_t	tmp_pbyte_s_offset_in_pblk;
	uint64_t	tmp_buf_offset_in_pblk;
	uint64_t	tmp_pbyte_s = pbyte_s;
	int			tmp_n_bytes = n_bytes;

	NFIN;
	NTOMA_ASSERT(qxx992n, ((uint64_t)buf % PAGE_SIZE) == 0, "buf is not aligned");
	if (n_bytes == 0) {
		N_Wf(d782nsj, "n_bytes=0");
		rv = 0;
		goto out;
	}

	tmp_pbyte_s_offset_in_pblk = pbyte_s % pblk_size;
	tmp_buf_offset_in_pblk = (uint64_t)buf % pblk_size;
	if (is_read) {
		if (n_bytes % pblk_size > 0 || tmp_pbyte_s_offset_in_pblk > 0 || tmp_buf_offset_in_pblk > 0) {
			// We have to read into tmp_buf, and we want to read the exact containing blocks
			tmp_pbyte_s = rounddown(pbyte_s, pblk_size);
			tmp_n_bytes = roundup(pbyte_s + n_bytes, pblk_size) - tmp_pbyte_s;
			tmp_buf = NNVMEIBT_BM_ALIGNED_ALLOC(bsdgfv5, PAGE_SIZE, tmp_n_bytes);
		}
	} else {
		if (tmp_pbyte_s_offset_in_pblk > 0) {
			N_Ef(vha72hs, "pbyte_s=@ZX not aligned to pblk", pbyte_s);
			goto out;
		}
		if (tmp_buf_offset_in_pblk > 0) {
			// We have to write from tmp_buf
			tmp_buf = NNVMEIBT_BM_ALIGNED_ALLOC(4bcscud, PAGE_SIZE, roundup(n_bytes, pblk_size));
			memcpy(tmp_buf, buf, n_bytes);
		}
	}

	if (nl_ctx) {
		bool	is_retryable;
		int		n_retries = 0;

		N_Tf(x505fb6, "pblk_size=@X pbyte_s=@OFFSET_INT(tmp=@OFFSET_INT) n_bytes=@X(tmp=@X) buf=@PTR(tmp=@PTR)", pblk_size, pbyte_s, tmp_pbyte_s, n_bytes, tmp_n_bytes, buf, tmp_buf);
		if (tmp_pbyte_s < min_offset_allowed) {
			N_Ef(x505n71, "tmp_pbyte_s=@OFFSET_INT min_offset_allowed=@OFFSET_INT", tmp_pbyte_s, min_offset_allowed);
			nvmeibt_abort(ES_FATAL);
		}
		nvmeib_init_io_to_disk(&nl_ctx->nl_msg_payload, tmp_pbyte_s / pblk_size, tmp_buf, tmp_n_bytes, md, md_len, is_read, main_gpt_update_flags);
		/*
		N_Tf(ia832kz, "start_sector=@LX data=@PTR data_len=@X md=@PTR md_len=@X is_read=@X gpt_update_flags=@X",
			 nl_ctx->nl_msg_payload.start_sector, nl_ctx->nl_msg_payload.data, nl_ctx->nl_msg_payload.data_len,
			 nl_ctx->nl_msg_payload.md, nl_ctx->nl_msg_payload.md_len, nl_ctx->nl_msg_payload.is_read, nl_ctx->nl_msg_payload.gpt_update_flags);
		*/
		do {
			if (nvmeibt_netlink_do_io_sync(nl_ctx) < 0) {
				N_Wf(37dbhas, "failed netlink IO(is_read=@INT) nl_ctx=@PTR pbyte_s=@ZX n_bytes=@X err=@AUTO_ERRNO", is_read, nl_ctx, tmp_pbyte_s, tmp_n_bytes);
				is_retryable = (errno < 1000000 || 1) ; // for now
				if (is_retryable && n_retries++ < 2) {
					// Retry
				} else {
					goto out;
				}
			} else {
				break;	// All is good
			}
		} while (1);
	} else if (fd > 2) {
		if (is_read == NVMEIB_IO_IS_READ) {
			if (NNVMEIBT_PREAD(d338dja, fd, tmp_buf, tmp_n_bytes, tmp_pbyte_s, true) < 0) {
				N_Wf(ebdozx0, "failed pread(fd=@INT) pbyte_s=@OFFSET_INT n_bytes=@INT err=@AUTO_ERRNO", fd, pbyte_s, n_bytes);
				goto out;
			}
		} else {
			if (NNVMEIBT_PWRITE(r5t6u8l, fd, tmp_buf, tmp_n_bytes, tmp_pbyte_s, min_offset_allowed) < 0) {
				N_Wf(bw98sd1, "failed write(fd=@INT) pbyte_s=@OFFSET_INT n_bytes=@X err=@AUTO_ERRNO", fd, pbyte_s, n_bytes);
				goto out;
			}
		}
	} else {
		N_Ef(rq0ierj, "nl_ctx=NULL & fd=@INT", fd);
		nvmeibt_abort(ES_FATAL);
	}
	if (is_read && tmp_buf != buf) {
		memcpy(buf, tmp_buf + tmp_pbyte_s_offset_in_pblk, n_bytes);
	}
	rv = 0;
out:
	if (tmp_buf != buf) {
		NNVMEIBT_BM_FREE(6b28abd, tmp_buf);
	}
	NFOUT;
	return rv;
}

/**
 * Return true if the entry is active - has a type UUID other than 0.
 *
 * @author max (7/24/17)
 *
 * @param entry
 *
 * @return BOOL
 */

void nvmeibt_disk_metadata_fill_dump_mbr_str(struct nvmeibt_Str *str, struct nvmeibt_disk_mbr *mbr)
{
	int										i;
	struct nvmeibt_mbr_partition_record		*part;

	nvmeibt_Str_reuse(str);
	nvmeibt_Str_sprintf(str, "signature=0x%04hx disk_signature(unused)=0x%x ", mbr->signature, mbr->disk_signature);
	for (i = 0; i < 4; i++) {
		part = &(mbr->partitions[i]);
		nvmeibt_Str_sprintf(str,
							"partition[%d]={boot_indicator=%x starting_chs=<0x%02hhx,0x%02hhx,0x%02hhx> "
							"os_type=0x%02hhx ending_chs=<0x%02hhx,0x%02hhx,0x%02hhx> pba_s=%x n_pblk=%x}, ",
							i, part->boot_indicator,
							part->starting_chs[0], part->starting_chs[1], part->starting_chs[2],
							part->os_type,
							part->ending_chs[0], part->ending_chs[1], part->ending_chs[2],
							part->pba_s, part->n_pblk);
	}
}

BOOL nvmeibt_disk_metadata_is_gpt_entry_in_use(const struct nvmeibt_disk_gpt_partition_entry *entry)
{
	return !ARE_UUID_EQ(&entry->partition_type_guid, &GPT_UNUSED_ENTRY_TYPE_GUID);
}

BOOL nvmeibt_disk_metadata_is_gpt_entry_active_and_matching_uuid(const union nvmeib_uuid *uuid, const struct nvmeibt_disk_gpt_partition_entry *entry)
{
	return (ARE_UUID_EQ(uuid, &entry->partition_guid) && nvmeibt_disk_metadata_is_gpt_entry_in_use(entry));
}

void nvmeibt_disk_metadata_fill_gpt_header_str(const struct nvmeibt_disk_gpt_header *gpt_header, struct nvmeibt_Str *str_ctx,
											   const char *gpt_main_or_metadata_str, const char *gpt_primary_or_alternate_or_mem_str)
{
	nvmeibt_Str_sprintf(str_ctx, "%s-%s-GPT rev=0x%x hdr_size=%x hdr_crc32=0x%x my_pba=%zx alt_pba=%zx first_useable_pba=%zx last_useable_pba=%zx partition_entry_pba=%zx " \
					"n_entries=%d entry_size=%x entries_crc32=0x%x gpt_uuid=%016llx-%016llx\n",
					gpt_main_or_metadata_str, gpt_primary_or_alternate_or_mem_str,
					gpt_header->revision,
					gpt_header->header_size,
					gpt_header->header_crc32,
					gpt_header->my_pba,
					gpt_header->alternate_pba,
					gpt_header->first_usable_pba,
					gpt_header->last_usable_pba,
					gpt_header->partition_entry_pba,
					gpt_header->n_partition_entries,
					gpt_header->size_of_partition_entry,
					gpt_header->partition_entry_array_crc32,
					gpt_header->disk_obj_uuid.ll[0], gpt_header->disk_obj_uuid.ll[1]
					);
}

/**
 * Prints the gpt header.
 *
 * @author max (7/24/17)
 *
 * @param gpt_header
 */
void nvmeibt_disk_metadata_print_gpt_header(const struct nvmeibt_disk_gpt_header *gpt_header,
											const char *gpt_main_or_metadata_str, const char *gpt_primary_or_alternate_or_mem_str)
{
	struct nvmeibt_Str *str_ctx = NNVMEIBT_STR_ALLOC(trace_disk_metadata_nvmeibt_disk_metadata_print_gpt_header);
	nvmeibt_disk_metadata_fill_gpt_header_str(gpt_header, str_ctx, gpt_main_or_metadata_str, gpt_primary_or_alternate_or_mem_str);
	nvmeibt_Str_chop_last_char(str_ctx);
	N_Tf(trace_1_disk_metadata_nvmeibt_disk_metadata_print_gpt_header, "@STR", nvmeibt_Str_str(str_ctx));
	NNVMEIBT_STR_FREE(trace_2_disk_metadata_nvmeibt_disk_metadata_print_gpt_header, str_ctx);
}

void nvmeibt_disk_metadata_fill_gpt_entry_str(const struct nvmeibt_disk_gpt_partition_entry *entry, int index, struct nvmeibt_Str *str_ctx)
{
	char str_name[GPT_MAX_PARTITION_NAME_LENGTH + 1];
	char16_str_to_str(entry->partition_name, GPT_MAX_PARTITION_NAME_LENGTH + 1, str_name);
	nvmeibt_Str_sprintf(str_ctx, "%4d %016llx-%016llx %016llx-%016llx%11zx%11zx  %-10zx %-30s\n", index,
	 entry->partition_type_guid.ll[0], entry->partition_type_guid.ll[1],
	 entry->partition_guid.ll[0], entry->partition_guid.ll[1],
	 entry->pba_s,
	 entry->pba_e,
	 entry->attributes,
	 str_name);
}

/**
 * Print the gpt entry located at the given index in the entries array.
 *
 * @author max (7/24/17)
 *
 * @param entry
 * @param index
 */
void nvmeibt_disk_metadata_print_gpt_entry(const struct nvmeibt_disk_gpt_partition_entry *entry, int index)
{
	struct nvmeibt_Str *str_ctx = NNVMEIBT_STR_ALLOC(trace_disk_metadata_nvmeibt_disk_metadata_print_gpt_entry);
	nvmeibt_disk_metadata_fill_gpt_entry_str(entry, index, str_ctx);
	nvmeibt_Str_chop_last_char(str_ctx);	// There exists an \n in order to concatenate such lines
	N_Tf(trace_1_disk_metadata_nvmeibt_disk_metadata_print_gpt_entry, "@STR", nvmeibt_Str_str(str_ctx));
	NNVMEIBT_STR_FREE(trace_2_disk_metadata_nvmeibt_disk_metadata_print_gpt_entry, str_ctx);
}

void nvmeibt_disk_metadata_fill_all_gpt_entries_str(const struct nvmeibt_disk_gpt_partition_entry *gpt_entries,
													const int n_partition_entries,
													const BOOL is_skip_inactive,
													struct nvmeibt_Str *str_ctx,
													const char *gpt_main_or_metadata_str,
													const char *gpt_primary_or_alternate_or_mem_str)
{
	int i;
	nvmeibt_Str_sprintf(str_ctx, "%s-%s-GPT %s Entries:\n", gpt_main_or_metadata_str, gpt_primary_or_alternate_or_mem_str, is_skip_inactive ? "Active" : "All");
	nvmeibt_Str_sprintf(str_ctx, " Idx %-33s %-33s %10s %10s  attributes name\n", "type_uuid", "partition_guid", "pba_s", "pba_e");
	for (i = 0; i < n_partition_entries; i++) {
		const struct nvmeibt_disk_gpt_partition_entry *curr_gpt_entry = &(gpt_entries[i]);
		if (is_skip_inactive && !nvmeibt_disk_metadata_is_gpt_entry_in_use(curr_gpt_entry)) {
			continue;
		}
		nvmeibt_disk_metadata_fill_gpt_entry_str(curr_gpt_entry, i, str_ctx);
	}
}

void nvmeibt_disk_metadata_print_all_gpt_entries(const struct nvmeibt_disk_gpt_partition_entry *gpt_entries, int n_partition_entries, BOOL is_skip_inactive,
												 const char *gpt_main_or_metadata_str, const char *gpt_primary_or_alternate_or_mem_str,
												 const char *ldisk_id)
{
	struct nvmeibt_Str *str_ctx = NNVMEIBT_STR_ALLOC(6s8k2kp);
	nvmeibt_disk_metadata_fill_all_gpt_entries_str(gpt_entries, n_partition_entries, is_skip_inactive, str_ctx, gpt_main_or_metadata_str, gpt_primary_or_alternate_or_mem_str);
	nvmeibt_Str_chop_last_char(str_ctx);	// The last \n
	N_Tf(dromw5, "disk=@STR", ldisk_id);
	NVMEIBT_LONG_TRACE_WRAPPER(usn5jxe, "", nvmeibt_Str_str(str_ctx), nvmeibt_Str_strlen(str_ctx));
	NNVMEIBT_STR_FREE(ysisol3, str_ctx);
}

/**
 * Updates the header and partition entries CRCs.
 *
 * @author max (7/10/17)
 *
 * @param gpt
 */
static void update_gpt_crcs(struct nvmeibt_disk_gpt *gpt)
{
	// First calculate the partition entries CRC.
	/*
	 * TODO(NVMESH-7436): CRC should be calculated on n_partition_entries, not max_n_entries.
	 * However, code incorrectly sets n_partition_entries to 128 instead of LARGE_GPT_MAX_NUM_GPT_ENTRIES,
	 * so we keep using max_n_entries for now.
	 */
	gpt->header.partition_entry_array_crc32 = crc32_seedless(gpt->entries, gpt->max_n_entries * gpt->header.size_of_partition_entry);
	// Now set the header crc to be 0.
	gpt->header.header_crc32 = 0;
	// Now recalculate the header crc.
	gpt->header.header_crc32 = crc32_seedless(&gpt->header, sizeof(gpt->header));
}

/**
 * Checks the validity of the gpt header crc
 *
 * @author max (6/25/17)
 *
 * @param gpt_header
 *
 * @return int
 */
static BOOL check_gpt_header_crc(struct nvmeibt_disk_gpt_header *gpt_header, size_t allocated_n_bytes_header)
{
	int stored_crc = gpt_header->header_crc32;
	int calculated_crc;

	// Zero the header crc inside the header to recalculate.
	gpt_header->header_crc32 = 0;
	NTOMA_ASSERT(rbsisfk, sizeof(*gpt_header) <= allocated_n_bytes_header , "@SIZE_T > @SIZE_T", sizeof(*gpt_header), allocated_n_bytes_header);
	calculated_crc = crc32_seedless(gpt_header, sizeof(*gpt_header));
	// Restore the header crc after calculation.
	gpt_header->header_crc32 = stored_crc;

	N_Tf(trace_disk_metadata_check_gpt_header_crc, "actual crc=@CRC calculated=@CALCULATED", stored_crc, calculated_crc);
	return stored_crc == calculated_crc;
}

static BOOL is_gpt_entries_crc_OK(const struct nvmeibt_disk_gpt_partition_entry *gpt_entries,
								  const struct nvmeibt_disk_gpt_header *gpt_header,
								  struct nvmeibt_disk_gpt *gpt,
								  const char *gpt_primary_or_alternate_or_mem_str,
								  size_t allocated_n_bytes_entries)
{
	uint32_t	calculated_crc;
	BOOL		result;
	int			nbytes;
	uint32_t	upgrade_calculated_crc;

	/*
	 * TODO(NVMESH-7436): CRC should be calculated on n_partition_entries, not max_n_entries.
	 * However, code incorrectly sets n_partition_entries to 128 instead of LARGE_GPT_MAX_NUM_GPT_ENTRIES,
	 * so we keep using max_n_entries for now.
	 */
	nbytes = (gpt->max_n_entries * gpt_header->size_of_partition_entry);
	NTOMA_ASSERT(85hs7h4, nbytes <= (int)allocated_n_bytes_entries, "CRC is calculated on @INT > @SIZE_T bytes. More than allocated", nbytes, allocated_n_bytes_entries);

	calculated_crc = crc32_seedless(gpt_entries, nbytes);
	result = (gpt_header->partition_entry_array_crc32 == calculated_crc);

	// Backward compatibility check 1: 128-entry GPT upgrade (v1.3)
	if (!result && gpt->max_n_entries == LARGE_GPT_MAX_NUM_GPT_ENTRIES) {
		// check if this is an upgrade from a 128-entry GPT (v1.3)
		nbytes = (128 * gpt_header->size_of_partition_entry);
		NTOMA_ASSERT(vbskxt9, nbytes <= (int)allocated_n_bytes_entries, "CRC is calculated on @INT > @SIZE_T bytes. More than allocated", nbytes, allocated_n_bytes_entries);
		upgrade_calculated_crc = crc32_seedless(gpt_entries, nbytes);
		if (gpt_header->partition_entry_array_crc32 == upgrade_calculated_crc) {
			N_Tf(trace_disk_metadata_check_gpt_entries_crc,
				 "@STR-@STR-GPT on @STR upgrade from 128-entry GPT detected via CRC check. max_n_entries reset to 128",
				 gpt->main_or_metadata, gpt_primary_or_alternate_or_mem_str,
				 gpt->ldisk_id.str);
			gpt->max_n_entries = 128;
			result = true;
		}
	}

	// Backward compatibility check 2: buggy n_partition_entries in gpt header; should be max_n_entries
	if (gpt->max_n_entries == LARGE_GPT_MAX_NUM_GPT_ENTRIES && gpt_header->n_partition_entries != gpt->max_n_entries) {
		N_Wf(trace_disk_metadata_buggy_n_partition_entries_detected,
				"@STR-@STR-GPT on @STR has n_partition_entries=@INT instead of max_n_entries=@INT. This may fail external tools. Consider updating n_partition_entries via `nvmeibt_toma gpt_util --upgrade-gpt`.",
				gpt->main_or_metadata, gpt_primary_or_alternate_or_mem_str,
				gpt->ldisk_id.str, gpt_header->n_partition_entries, gpt->max_n_entries);
	}

	if (!result) {
		N_WTf(warn_disk_metadata_check_gpt_entries_crc, "@STR-@STR-GPT CRC Mismatch for @STR n_active_partitions=@N_ACTIVE_PARTITIONS GPT entries. expected=@CRC, got=@CRC",
			  gpt->main_or_metadata, gpt_primary_or_alternate_or_mem_str,
			  gpt->ldisk_id.str, gpt->n_entries_in_use, gpt_header->partition_entry_array_crc32, calculated_crc);
	}
	return result;
}

int nvmeibt_disk_metadata_deprecate_entry_in_mem_gpt(struct nvmeibt_disk_gpt *gpt, const union nvmeib_uuid *uuid)
{
	int		rv = 1;
	int		entry_idx;
	char	part_name[GPT_MAX_PARTITION_NAME_LENGTH + 1] = {0};

	NFIN;
	N_Tf(vgahjw8, "Trying to deprecate partition with uuid=@UUID_LE from @STR n_active_partitions=@N_ACTIVE_PARTITIONS", uuid, gpt->main_or_metadata, gpt->n_entries_in_use);
	if (gpt->n_entries_in_use == 0) {
		N_Ef(xaim5gv, "Unable to deprecate partition entry for uuid=@UUID_LE since the @STR is empty.", uuid, gpt->main_or_metadata);
		rv = -1;
		goto out;
	}
	for (entry_idx = 0; entry_idx < gpt->max_n_entries; entry_idx++) {
		struct nvmeibt_disk_gpt_partition_entry *curr_gpt_entry = &(gpt->entries[entry_idx]);
		// Check the partition uuid matches the disk segment uuid, and that the partition entry is not marked as unused.
		if (nvmeibt_disk_metadata_is_gpt_entry_active_and_matching_uuid(uuid, curr_gpt_entry)) {
			curr_gpt_entry->attributes |= EXCELERO_JOURNAL_DATA_PARTITION_ATTRIBUTE_DEPRECATED_MASK;
			N_Tf(vdge73h, "Deprecated entry=@ENTRY_INT partition name=@NAME from @STR n_active_partitions=@N_ACTIVE_PARTITIONS", entry_idx, part_name, gpt->main_or_metadata, gpt->n_entries_in_use);
			rv = 0;	// Modified the GPT - need to save
			goto out;
		}
	}
out:
	if (rv < 0) {
		N_Wf(warn_disk_metadata_nvmeibt_disk_metadata_deprecate_entry_in_gpt, "Unable to deprecate partition entry with uuid @UUID_LE from @STR", uuid, gpt->main_or_metadata);
	}
	NFOUT;
	return rv;
}


/**
 * Removes a disk segment from the GPT stored on the local
 * disk. This function does NOT update the data on the disk,
 * rather only the GPT object.
 *
 * @author max (3/6/17)
 *
 * @param gpt - pointer to GPT from which the segment is removed.
 * @param uuid - uuid of entry to remove.
 *
 * @return int - returns 1 upon success, 0 on failure
 */
int nvmeibt_disk_metadata_remove_entry_from_mem_gpt(struct nvmeibt_disk_gpt *gpt, const union nvmeib_uuid *uuid)
{
	int		rv = 0;
	int		entry_idx;
	char	part_name[GPT_MAX_PARTITION_NAME_LENGTH + 1];

	NFIN;
	N_Tf(trace_disk_metadata_nvmeibt_disk_metadata_remove_entry_from_gpt, "Trying to remove partition with uuid=@UUID_LE from @STR n_active_partitions=@N_ACTIVE_PARTITIONS", uuid, gpt->main_or_metadata, gpt->n_entries_in_use);
	if (gpt->n_entries_in_use == 0) {
		N_Ef(error_disk_metadata_nvmeibt_disk_metadata_remove_entry_from_gpt, "Unable to delete partition entry for uuid=@UUID_LE since the @STR is empty.", uuid, gpt->main_or_metadata);
		rv = -1;
		goto out;
	}
	for (entry_idx = 0; entry_idx < gpt->max_n_entries; entry_idx++) {
		struct nvmeibt_disk_gpt_partition_entry *curr_gpt_entry = &(gpt->entries[entry_idx]);
		// Check the partition uuid matches the disk segment uuid, and that the partition entry is not marked as unused.
		if (nvmeibt_disk_metadata_is_gpt_entry_active_and_matching_uuid(uuid, curr_gpt_entry)) {
			// Found a candidate to delete
			char16_str_to_str(curr_gpt_entry->partition_name, GPT_MAX_PARTITION_NAME_LENGTH + 1, part_name);

			curr_gpt_entry->partition_type_guid = GPT_UNUSED_ENTRY_TYPE_GUID;
			curr_gpt_entry->pba_s = 0;
			curr_gpt_entry->pba_e = 0;
			gpt->n_entries_in_use--;
			N_Tf(trace_1_disk_metadata_nvmeibt_disk_metadata_remove_entry_from_gpt, "Deleted entry=@ENTRY_INT partition name=@NAME from @STR n_active_partitions=@N_ACTIVE_PARTITIONS", entry_idx, part_name, gpt->main_or_metadata, gpt->n_entries_in_use);
			goto out;
		}
	}
out:
	if (rv < 0) {
		N_Wf(warn_disk_metadata_nvmeibt_disk_metadata_remove_entry_from_gpt, "Unable to remove partition entry with uuid @UUID_LE from @STR", uuid, gpt->main_or_metadata);
	}
	NFOUT;
	return rv;
}

struct nvmeibt_disk_gpt_partition_entry *nvmeibt_disk_metadata_add_mem_gpt_entry(struct nvmeibt_disk_gpt *gpt,
																				 const union nvmeib_uuid *partition_type,
																				 const union nvmeib_uuid *uuid,
																				 uint64_t pba_s,
																				 uint64_t pba_e,
																				 const char *part_name,
																				 int part_name_len)
{
	int											entry_idx;
	struct nvmeibt_disk_gpt_partition_entry		*gpt_entry = NULL;

	NFIN;
	NTOMA_ASSERT(bwud9so, part_name_len <= GPT_MAX_PARTITION_NAME_LENGTH, "part_name=@PART_NAME len=@LEN > GPT_MAX_PARTITION_NAME_LENGTH=@GPT_MAX_PARTITION_NAME_LENGTH", part_name, part_name_len, GPT_MAX_PARTITION_NAME_LENGTH);
	NTOMA_ASSERT(nvjdfkj, pba_e > pba_s, "disk=@STR part_name=@PART_NAME pba_s=@PBA_S >= pba_e=@PBA_E", gpt->ldisk_id.str, part_name, pba_s, pba_e);

	N_Tf(46dgbs7, "@STR. Trying to add partition @PART_NAME of type=@UUID_LE and uuid=@UUID_LE pba_s=@PBA_S pba_e=@PBA_E",
		gpt->main_or_metadata, part_name, partition_type, uuid, pba_s, pba_e);

	if (pba_s < gpt->header.first_usable_pba || pba_e > gpt->header.last_usable_pba) {
		N_Ef(uc4nyd6, "@STR-GPT Partition out of range @STR. partition_pba=(@PBA_S, @PBA_S) useable_pba=(@PBA_S, @PBA_S)",
			 gpt->main_or_metadata, gpt->ldisk_id.str, pba_s, pba_e, gpt->header.first_usable_pba, gpt->header.last_usable_pba);
		goto out;
	}
	if (ARE_UUID_EQ(partition_type, &GPT_UNUSED_ENTRY_TYPE_GUID)) {
		N_Ef(vms9v3d, "@STR-GPT Unable to add partition invalid_type=@UUID_LE uuid=@UUID_LE to @STR",
			 gpt->main_or_metadata, partition_type, uuid, gpt->ldisk_id.str);
		goto out;
	}
	if (gpt->max_n_entries == gpt->n_entries_in_use) {
		N_Ef(y4hfgid, "@STR-GPT is full type=@UUID_LE uuid=@UUID_LE disk=@STR n_entries_in_use=@N_ACTIVE_PARTITIONS",
			 gpt->main_or_metadata, partition_type, uuid, gpt->ldisk_id.str, gpt->n_entries_in_use);
		goto out;
	}

	N_Tf(ycnwmx7, "Checking overlap with @N_ACTIVE_PARTITIONS active partitions in the @STR:@STR", gpt->n_entries_in_use, gpt->ldisk_id.str, gpt->main_or_metadata);
	for (entry_idx = 0; entry_idx < gpt->max_n_entries; entry_idx++) {
		struct nvmeibt_disk_gpt_partition_entry *curr_gpt_entry = &(gpt->entries[entry_idx]);
		if (ARE_UUID_EQ(uuid, &curr_gpt_entry->partition_guid)) {
			if (!nvmeibt_disk_metadata_is_gpt_entry_in_use(curr_gpt_entry)) {
				N_Ef(nvfdui, "Unused partition with uuid=@UUID_LE, was found on @STR:@STR while attempting to add a new partition with the same UUID.", uuid, gpt->ldisk_id.str, gpt->main_or_metadata);
			}
			else {	// No harm, but unexpected!
				gpt_entry = curr_gpt_entry;
				N_Ef(bfsuey2, "Partition with uuid=@UUID_LE, is already part of the @STR:@STR, it won't be added again.", uuid, gpt->ldisk_id.str, gpt->main_or_metadata);
				goto out;
			}
		}
		if (	nvmeibt_disk_metadata_is_gpt_entry_in_use(curr_gpt_entry) &&
				nvmeibt_do_ranges_overlap(pba_s, pba_e, curr_gpt_entry->pba_s, curr_gpt_entry->pba_e)) {
			N_Wf(brguys7, "@STR-GPT new entry @UUID_LE [pba_s=@PBA_S,pba_e=@PBA_E] @STR overlaps entry=@ENTRY_INT. Probably already deleted from config",
				 gpt->main_or_metadata, uuid, pba_s, pba_e, gpt->ldisk_id.str, entry_idx);
		}
	}
	for (entry_idx = 0; entry_idx < gpt->max_n_entries; entry_idx++) {
		struct nvmeibt_disk_gpt_partition_entry *curr_gpt_entry = &(gpt->entries[entry_idx]);
		if (!nvmeibt_disk_metadata_is_gpt_entry_in_use(curr_gpt_entry)) {
			N_Tf(btuydbu, "Found empty slot in @STR:@STR at index=@INDEX pba_s=@PBA_S pba_e=@PBA_E", gpt->ldisk_id.str, gpt->main_or_metadata, entry_idx, pba_s, pba_e);
			gpt_entry = curr_gpt_entry;
			break;
		}
	}
	if (gpt_entry) {
		N_Tf(nn3f5bc, "@STR:@STR Adding new partition to entry=@PTR_DIFF name=@NAME uuid=@UUID_LE",
			 gpt->ldisk_id.str, gpt->main_or_metadata, gpt_entry - gpt->entries, part_name, uuid);
		// We found an empty slot we can use.
		gpt_entry->partition_type_guid = *partition_type;
		gpt_entry->partition_guid = *uuid;
		gpt_entry->pba_s = pba_s;
		if (gpt_entry->pba_s == 0) {
			N_Ef(4ksda0i, "pba_s=0");
			nvmeibt_abort(ES_FATAL);
		}
		gpt_entry->pba_e = pba_e;
		gpt_entry->attributes = 0;
		// Convert the part_name_str to char16 str which is required by UEFI standard.
		str_to_char16_str(part_name, part_name_len, gpt_entry->partition_name);

		gpt->n_entries_in_use++;
	}
out:
	if (!gpt_entry) {
		N_Ef(rldple0, "Unable to add gpt entry with uuid=@UUID_LE to @STR:@STR, no empty slot found.", uuid, gpt->ldisk_id.str, gpt->main_or_metadata);
		// Dump the GPT
		nvmeibt_disk_metadata_print_gpt_header(&gpt->header, gpt->main_or_metadata, "Mem");
		nvmeibt_disk_metadata_print_all_gpt_entries(gpt->entries, gpt->max_n_entries, true, gpt->main_or_metadata, "Mem", gpt->ldisk_id.str);
	}
	NFOUT;
	return gpt_entry;
}

/**
 * Transform a primary GPT header to the corresponding alternative header,
 * updates PBA addresses accordingly.
 *
 * @author max (6/20/17)
 *
 * @param primary - primary gpt header
 * @param alternate - alternative gpt header
 * @param pblk_size
 */
static void translate_primary_gpt_header_to_alternate(const struct nvmeibt_disk_gpt_header *primary,
													  struct nvmeibt_disk_gpt_header *alternate,
													  int pblk_size)
{
	int entries_array_size_in_pblks;

	memset(alternate, 0, sizeof(struct nvmeibt_disk_gpt_header));
	// Init the ALTERNATE GPT header
	alternate->gpt_signature = primary->gpt_signature;
	alternate->revision = primary->revision;
	alternate->header_size = primary->header_size;
	alternate->reserved = primary->reserved;
	alternate->my_pba = primary->alternate_pba;
	alternate->alternate_pba = primary->my_pba;
	alternate->disk_obj_uuid = primary->disk_obj_uuid;
	alternate->n_partition_entries = primary->n_partition_entries;
	alternate->size_of_partition_entry = primary->size_of_partition_entry;
	entries_array_size_in_pblks = divroundup(alternate->size_of_partition_entry * LARGE_GPT_MAX_NUM_GPT_ENTRIES, pblk_size);
	alternate->partition_entry_pba = alternate->my_pba - entries_array_size_in_pblks;

	alternate->first_usable_pba = primary->first_usable_pba;
	alternate->last_usable_pba = primary->last_usable_pba;
	alternate->partition_entry_array_crc32 = primary->partition_entry_array_crc32;
	alternate->header_crc32 = 0;
	alternate->header_crc32 = crc32_seedless(alternate, sizeof(*alternate));
}

/**
 * Transform alternate GPT header to the corresponding primary header,
 * updates PBA addresses accordingly.
 *
 * @author max (6/20/17)
 *
 * @param alternate
 * @param primary
 * @param pblk_size
 */
void relocate_alternate_gpt_header_to_primary(const struct nvmeibt_disk_gpt_header *alternate,
											   struct nvmeibt_disk_gpt_header *primary)
{
	memset(primary, 0, sizeof(struct nvmeibt_disk_gpt_header));
	// Init the PRIMARY GPT header
	primary->gpt_signature = alternate->gpt_signature;
	primary->revision = alternate->revision;
	primary->header_size = alternate->header_size;
	primary->reserved = alternate->reserved;
	primary->my_pba = alternate->alternate_pba;
	primary->alternate_pba = alternate->my_pba;
	primary->disk_obj_uuid = alternate->disk_obj_uuid;
	primary->size_of_partition_entry = alternate->size_of_partition_entry;
	primary->n_partition_entries = alternate->n_partition_entries;
	primary->partition_entry_pba = primary->my_pba + 1;

	primary->first_usable_pba = alternate->first_usable_pba;
	primary->last_usable_pba = alternate->last_usable_pba;
	primary->partition_entry_array_crc32 = alternate->partition_entry_array_crc32;
	primary->header_crc32 = 0;
	primary->header_crc32 = crc32_seedless(primary, sizeof(*primary));
}

static int nvmeibt_disk_gpt_header_store(struct netlink_io_context *nl_ctx, int fd,
										 int pblk_size,
										 bool is_main_gpt_update,
										 const void *gpt_entries_hint,
										 int n_gpt_entries_hint,
										 const struct nvmeibt_disk_gpt_header *gpt_header,
										 bool init_serjio)
{
	int									rv = 0;
	int									n_bytes = sizeof(*gpt_header);
	uint64_t							header_pbyte_s = gpt_header->my_pba * pblk_size;
	enum nvmeib_main_gpt_update_flags	gpt_update_flags = init_serjio ? MAIN_GPT_UPDATE_SERJIO_INIT : NO_MAIN_GPT_UPDATE;
	char								*dma_buffer;
	char								*md = NULL;
	unsigned int						md_len = 0;

	NFIN;
	n_bytes = roundup(n_bytes, pblk_size);
	N_Tf(b68zjh2, "Storing header to pbyte_s=@OFFSET_INT n_bytes=@WRITE_SIZE", header_pbyte_s, n_bytes);

	NTOMA_ASSERT(a03kdma, gpt_header->gpt_signature == GPT_SIGNATURE || gpt_header->gpt_signature == MIDST_WRITE_GPT_SIGNATURE,
				"gpt signature mismatch midst_write Expected=@GPT_SIGNATURE|@GPT_SIGNATURE actual=@GPT_SIGNATURE",
				GPT_SIGNATURE, MIDST_WRITE_GPT_SIGNATURE, gpt_header->gpt_signature);
	dma_buffer = NNVMEIBT_BM_ALIGNED_CALLOC(34bsda8, PAGE_SIZE, n_bytes);
	memcpy(dma_buffer, gpt_header, sizeof(*gpt_header));
	if (is_main_gpt_update) {
		if (gpt_header->gpt_signature == MIDST_WRITE_GPT_SIGNATURE)
			gpt_update_flags |= MAIN_GPT_UPDATE_STAGE_HEADER_PRE_UPDATE;
		else
			gpt_update_flags |= MAIN_GPT_UPDATE_STAGE_HEADER_POST_UPDATE;
		md = (char *)gpt_entries_hint;
		md_len = n_gpt_entries_hint * sizeof(struct nvmeibt_disk_gpt_partition_entry);
	}
	if (nvmeibt_disk_metadata_do_sync_IO_with_disk_netlink_or_not(nl_ctx, fd, dma_buffer, header_pbyte_s, pblk_size, n_bytes, md, md_len, NVMEIB_IO_IS_WRITE, gpt_update_flags, pblk_size) < 0) {
		N_ETf(vdsaytq, "Error while writing gpt header to ldisk_id=@STR ", nl_ctx->nl_msg_payload.disk_id);
		rv = -1;
		goto out;
	}
	N_Tf(mcuz82b, "Stored gpt header to pbyte_s=@OFFSET_INT on persistency successfully", header_pbyte_s);
out:
	NNVMEIBT_BM_FREE(b47aghd, dma_buffer);
	NFOUT;
	return rv;
}

enum GPT_VALIDITY read_gpt_header_from_pos(struct netlink_io_context *nl_ctx,
		  	  	  	  	  	  	  	  	   const int fd,
										   const uint64_t pbyte_s,
										   const int pblk_size,
										   struct nvmeibt_disk_gpt_header *gpt_header,
										   const char *gpt_main_or_metadata_str,
										   const char *gpt_primary_or_alternate_or_mem_str,
										   size_t allocated_n_bytes_header)
{
	enum GPT_VALIDITY	rv;
	int					n_bytes = roundup(sizeof(*gpt_header), pblk_size);

	NFIN;
	N_Tf(fhyr761, "Trying to read @STR-@STR-GPT header from pbyte_s=@OFFSET_INT n_bytes=@READ",
		 gpt_main_or_metadata_str, gpt_primary_or_alternate_or_mem_str, pbyte_s, n_bytes);
	if (nvmeibt_disk_metadata_do_sync_IO_with_disk_netlink_or_not(nl_ctx, fd, gpt_header, pbyte_s, pblk_size, n_bytes, NULL, 0, NVMEIB_IO_IS_READ, 0, pblk_size) < 0) {
		rv = GPT_VALIDITY_TECHNICAL_ERR;
		goto out;
	}
	nvmeibt_disk_metadata_print_gpt_header(gpt_header, gpt_main_or_metadata_str, gpt_primary_or_alternate_or_mem_str);
	if (gpt_header->gpt_signature == MIDST_WRITE_GPT_SIGNATURE) {
		N_Tf(kiu82ws, "@STR-@STR-GPT header signature indicates MIDST_WRITE pbyte_s=@ZX",
			 gpt_main_or_metadata_str, gpt_primary_or_alternate_or_mem_str, pbyte_s);
		rv = GPT_VALIDITY_MIDST_WRITE;
		goto out;
	}
	if (gpt_header->gpt_signature != GPT_SIGNATURE) {
		N_WTf(sdkiwc5, " @STR-@STR-GPT header signature mismatch. Expected=@GPT_SIGNATURE got=@GPT_SIGNATURE at pos=@POS",
			  gpt_main_or_metadata_str, gpt_primary_or_alternate_or_mem_str, GPT_SIGNATURE, gpt_header->gpt_signature, pbyte_s);
		rv = GPT_VALIDITY_MAGIC_ERR;
		goto out;
	}
	if (!check_gpt_header_crc(gpt_header, allocated_n_bytes_header)) {
		N_WTf(fhu87ex, "CRC error for @STR-@STR-GPT header in pos=@POS fd=@FD",
			  gpt_main_or_metadata_str, gpt_primary_or_alternate_or_mem_str, pbyte_s, fd);
		rv = GPT_VALIDITY_CRC_ERR;
		goto out;
	}
	if (gpt_header->my_pba != (pbyte_s / pblk_size)) {	// My pba should match the one written in the header
		N_Wf(fhjsnx3, "my_pba does not match position of @STR-@STR-GPT header. pos=@POS my_pba=@MY_PBA",
			 gpt_main_or_metadata_str, gpt_primary_or_alternate_or_mem_str, (pbyte_s / pblk_size), gpt_header->my_pba);
		rv = GPT_VALIDITY_DATA_ERR;
		goto out;
	}
	rv = GPT_VALIDITY_OK;
	N_Tf(fjki98q, "Restored @STR-@STR-GPT header from persistency at pbyte_s=@OFFSET_INT.",
		 gpt_main_or_metadata_str, gpt_primary_or_alternate_or_mem_str, pbyte_s);
out:
	NFOUT;
	return rv;
}

static int nvmeibt_disk_metadata_gpt_entries_store(struct netlink_io_context *nl_ctx,
												   int fd,
												   int pblk_size,
												   const struct nvmeibt_disk_gpt_partition_entry *entries,
												   uint64_t pba,
												   int n_partition_entries,
												   const char *gpt_main_or_metadata_str,
												   const char *gpt_primary_or_alternate_or_mem_str,
												   const char *ldisk_id,
												   bool init_serjio)
{
	int			rv = 0;
	int			n_bytes;
	char		*dma_buffer = NULL;

	NFIN;
	n_bytes = sizeof(struct nvmeibt_disk_gpt_partition_entry) * n_partition_entries;
	n_bytes = roundup(n_bytes, pblk_size);
	N_Tf(trace_disk_metadata_nvmeibt_disk_metadata_gpt_entries_store, "Storing @STR-@STR-GPT_entries to ldisk_id=@STR pba=@ZX",
			gpt_main_or_metadata_str,
			gpt_primary_or_alternate_or_mem_str,
			nl_ctx ? nl_ctx->nl_msg_payload.disk_id : "N/A", pba);
	if (n_bytes == 0) {
		goto out;
	}
	// Write the GPT entries to the primary location
	N_Tf(kff58sh, "Writing gpt entries to pba=@OFFSET_INT n_bytes=@WRITE_SIZE", pba, n_bytes);
	dma_buffer = NNVMEIBT_BM_ALIGNED_CALLOC(9e837dg, PAGE_SIZE, n_bytes);
	memcpy(dma_buffer, entries, n_bytes);
	if (nvmeibt_disk_metadata_do_sync_IO_with_disk_netlink_or_not(nl_ctx, fd, dma_buffer, pba * pblk_size, pblk_size, n_bytes, NULL, 0, NVMEIB_IO_IS_WRITE,
																  MAIN_GPT_UPDATE_STAGE_ENTRIES | (init_serjio ? MAIN_GPT_UPDATE_SERJIO_INIT : 0), pblk_size*2) < 0) {
		N_Ef(error_2_disk_metadata_nvmeibt_disk_metadata_gpt_entries_store, "Failed writing gpt entries to ldisk_id=@STR ", nl_ctx->nl_msg_payload.disk_id);
		rv = -1;
		goto out;
	}
	N_Tf(cbsuzu0, "Stored gpt entries to pba=@OFFSET_INT successfully. n_bytes=@WRITE_SIZE", pba, n_bytes);
	nvmeibt_disk_metadata_print_all_gpt_entries(entries, n_partition_entries, true, gpt_main_or_metadata_str, gpt_primary_or_alternate_or_mem_str, ldisk_id);
out:
	NNVMEIBT_BM_FREE(trace_3_disk_metadata_write_gpt_entries_to_pba, dma_buffer);
	NFOUT;
	return rv;
}

enum GPT_VALIDITY read_gpt_entries_from_pos(struct netlink_io_context *nl_ctx,
											const int fd,
											const uint64_t pbyte_s,
											const int pblk_size,
											const int n_bytes_entries,
											const struct nvmeibt_disk_gpt_header *gpt_header,
											struct nvmeibt_disk_gpt_partition_entry *gpt_entries,
											struct nvmeibt_disk_gpt *gpt,
											const char *gpt_primary_or_alternate_or_mem_str,
											size_t allocated_n_bytes_entries)
{
	enum GPT_VALIDITY	rv;

	NFIN;
	N_Tf(trace_disk_metadata_read_gpt_entries_from_pos, "Trying to read @STR-@STR-GPT entries from pbyte_s=@ZX n_bytes=@N_BYTES", gpt->main_or_metadata, gpt_primary_or_alternate_or_mem_str, pbyte_s, n_bytes_entries);

	if (nvmeibt_disk_metadata_do_sync_IO_with_disk_netlink_or_not(nl_ctx, fd, gpt_entries, pbyte_s, pblk_size, n_bytes_entries, NULL, 0, NVMEIB_IO_IS_READ, NO_MAIN_GPT_UPDATE, pblk_size*2) < 0) {
		rv = GPT_VALIDITY_TECHNICAL_ERR;
		goto out;
	}
	if (!is_gpt_entries_crc_OK(gpt_entries, gpt_header, gpt, gpt_primary_or_alternate_or_mem_str, allocated_n_bytes_entries)) {
		N_WTf(warn_disk_metadata_read_gpt_entries_from_pos, "CRC err when reading @STR-@STR-GPT @STR entries, pbyte_s=@ZX fd=@FD",
			gpt->main_or_metadata, gpt_primary_or_alternate_or_mem_str, gpt->ldisk_id.str, pbyte_s, fd);
		rv = GPT_VALIDITY_CRC_ERR;
		goto out;
	}
	nvmeibt_disk_metadata_print_all_gpt_entries(gpt_entries, gpt->max_n_entries, 1, gpt->main_or_metadata, gpt_primary_or_alternate_or_mem_str, gpt->ldisk_id.str);
	rv = GPT_VALIDITY_OK;
	N_Tf(trace_1_disk_metadata_read_gpt_entries_from_pos, "Restored @STR-@STR-GPT header from persistency at pbyte_s=@ZX.", gpt->main_or_metadata, gpt_primary_or_alternate_or_mem_str, pbyte_s);
out:
	NFOUT;
	return rv;
}

/**
 * Initializes GPT structure with given block size and amount of entries on the
 * disk between pba_s and pba_e.
 *
 * @author max (6/22/17)
 *
 * @param local_disk
 * @param pba_s
 * @param pba_e
 * @param gpt
 * @param pblk_size
 * @param max_entries
 */
void nvmeibt_disk_metadata_init_mem_gpt(struct nvmeibt_local_disk *local_disk,
											uint64_t pba_s,
											uint64_t pba_e,
											struct nvmeibt_disk_gpt *gpt,
											int pblk_size,
											int max_entries,
											const union nvmeib_uuid *disk_obj_uuid)
{
	nvmeibt_disk_metadata_init_gpt_structure(pba_s, pba_e, gpt, pblk_size, max_entries, disk_obj_uuid);
	gpt->ldisk_id = *nvmeibt_local_disk_UUID(local_disk);

	// Signal that the GPT of this disk needs to be rewritten as changes have been made.
	NNVMEIBT_LOCAL_DISK_INC_GPT_CHANGE_NO(trace_disk_metadata_nvmeibt_disk_metadata_init_mem_gpt, local_disk);
}

void nvmeibt_disk_metadata_init_disk_metadata_struct(struct nvmeibt_local_disk *cur_local_disk, struct nvmeibt_disk_format_data *format_data)
{
	struct nvmeibt_disk_metadata *dm = &cur_local_disk->from_config.disk_metadata;

	dm->signature = DISK_METADATA_SIGNATURE;
	nvmeibt_strlcpy(dm->ldisk_id_str, format_data->ldisk_id.str, sizeof(dm->ldisk_id_str));
	nvmeibt_strlcpy(dm->native_serial_str, format_data->native_serial.str, sizeof(dm->native_serial_str));
	dm->nsid = format_data->nsid;
	dm->format_pblk_size = nvmeibt_local_disk_pblk_size(cur_local_disk);
	dm->format_metadata_size = cur_local_disk->from_config.metadata_n_bytes;
	dm->is_md_supported = nvmeibt_local_disk_is_md_supported(cur_local_disk);
	dm->disk_metadata_version = TOMA_DISK_METADATA_VERSION_V2_8_2;
	dm->last_pba_zeroed = 0; /* Will be updated later, from outside */
	dm->mgmt_db_uuid = *nvmeibt_global_get_mgmt_DB_uuid();
	dm->format_request_counter = format_data->format_request_counter;
	dm->disk_version_unused = 0;	// Was just formatted, the mgmt starts versioning from here
}

void nvmeibt_disk_metadata_init_gpt_structure(uint64_t pba_s,
											  uint64_t pba_e,
											  struct nvmeibt_disk_gpt *gpt,
											  int pblk_size, int max_entries,
											  const union nvmeib_uuid *disk_obj_uuid)
{
	int			idx;
	int			entries_array_n_pblk;
	int			gpt_total_n_pblk;
	const int	gpt_header_n_pblk = 1;

	NFIN;
	N_Tf(trace_disk_metadata_nvmeibt_disk_metadata_init_gpt_structure_0, "max_entries=@INT", max_entries);
	memset(&gpt->header, 0, sizeof(struct nvmeibt_disk_gpt_header));
	// Init the GPT header
	gpt->header.gpt_signature = GPT_SIGNATURE;
	gpt->header.revision = 0x00010000; /* GPT revision by UEFI standard*/
	gpt->header.header_size = 92; /*GPT header size by UEFI standard*/
	gpt->header.reserved = 0;
	gpt->header.my_pba = pba_s;
	gpt->header.alternate_pba = pba_e;
	gpt->header.disk_obj_uuid = *disk_obj_uuid;
	gpt->header.partition_entry_pba = pba_s + gpt_header_n_pblk;
	gpt->header.n_partition_entries = GPT_HDR_BIOS_WORKAROUND_NUM_ENTRIES;		// Open BUG NVMESH-7436 - incorrect value (not the actual number of entries), which causes standard GPT tools to think CRC is corrupted.
	gpt->header.size_of_partition_entry = UEFI_MIN_GPT_ENTRY_SIZE; /* Minimum partition entry size by UEFI standard*/
	/* This affects gpt->header.first_usable_pba and last_usable_pba and leaves enough room for a larger actual
	   GPT size, I.e., not according to gpt->header.n_partition_entries*/
	entries_array_n_pblk = divroundup(gpt->header.size_of_partition_entry * LARGE_GPT_MAX_NUM_GPT_ENTRIES, pblk_size);
	gpt_total_n_pblk = gpt_header_n_pblk + entries_array_n_pblk;

	// First useable pba is after the GPT header and after the entries array.
	gpt->header.first_usable_pba = pba_s + gpt_total_n_pblk;
	// Last useable pba is before the backup header and the backup entries array at the end of the disk. Aligned down to SECTOR_SIZE=4KB
	gpt->header.last_usable_pba = rounddown(pba_e - gpt_total_n_pblk + 1, (SECTOR_SIZE / pblk_size)) - 1;

	memset(gpt->entries, 0, sizeof(struct nvmeibt_disk_gpt_partition_entry) * max_entries);

	// Go over all the entries of the GPT and init them to empty (unused) entries.
	for (idx = 0; idx < max_entries; idx++) {
		struct nvmeibt_disk_gpt_partition_entry *gpt_entry = &(gpt->entries[idx]);
		gpt_entry->partition_type_guid = GPT_UNUSED_ENTRY_TYPE_GUID;
		gpt_entry->partition_guid = nvmeib_uuid_null_val;
		gpt_entry->pba_s =  0;
		gpt_entry->pba_e =  0;
		gpt_entry->attributes = 0;
	}

	gpt->n_entries_in_use = 0;
	gpt->max_n_entries = max_entries;
	gpt->is_valid = true;

	N_Tf(9xewmh2, "Initialized @STR:@STR on pba_s=@PBA_S pba_e=@PBA_E disk_obj_uuid=@STR first_usable=@FIRST_USABLE last_usable=@LAST_USABLE",
		 gpt->ldisk_id.str, gpt->main_or_metadata, pba_s, pba_e,
		 nvmeibt_union_uuid_to_urn_uuid(&gpt->header.disk_obj_uuid).str, gpt->header.first_usable_pba, gpt->header.last_usable_pba);

	NFOUT;
}

BOOL nvmeibt_disk_metadata_are_gpt_headers_equal(struct nvmeibt_disk_gpt_header *pri, struct nvmeibt_disk_gpt_header *alt)
{
	BOOL	are;

	are = (
		   pri->alternate_pba == alt->my_pba &&
		   pri->my_pba == alt->alternate_pba &&
		   ARE_UUID_EQ(&(pri->disk_obj_uuid), &(alt->disk_obj_uuid)) &&
		   pri->first_usable_pba == alt->first_usable_pba &&
		   pri->gpt_signature == alt->gpt_signature &&
		   // pri->header_crc32 == alt->header_crc32 &&		// Naturally, different
		   pri->header_size == alt->header_size &&
		   pri->last_usable_pba == alt->last_usable_pba &&
		   pri->n_partition_entries == alt->n_partition_entries &&
		   pri->partition_entry_array_crc32 == alt->partition_entry_array_crc32 &&
		   // pri->partition_entry_pba == alt->partition_entry_pba &&		// Each has its own entries
		   pri->revision == alt->revision &&
		   pri->size_of_partition_entry == alt->size_of_partition_entry);
	return are;
}

BOOL nvmeibt_disk_metadata_are_gpt_entries_equal(const struct nvmeibt_disk_gpt_partition_entry *pri,
												 const struct nvmeibt_disk_gpt_partition_entry *alt,
												 int n_bytes)
{
	if (!pri || !alt || n_bytes <= 0) {
		return false;
	}

	return (memcmp(pri, alt, n_bytes) == 0);
}

static void read_all_4_gpt_structs(struct netlink_io_context *nl_ctx,
		  	  	  	  	  	  	   int fd,
								   int pblk_size,
								   struct nvmeibt_disk_gpt *gpt,
								   uint64_t primary_gpt_header_pba,
								   uint64_t alternate_gpt_header_pba,
								   enum GPT_VALIDITY *primary_header_validity,
								   enum GPT_VALIDITY *alternate_header_validity,
								   enum GPT_VALIDITY *primary_entries_validity,
								   enum GPT_VALIDITY *alternate_entries_validity,
								   struct nvmeibt_disk_gpt_header *primary_header,
								   struct nvmeibt_disk_gpt_header *alternate_header,
								   struct nvmeibt_disk_gpt_partition_entry *primary_entries,
								   struct nvmeibt_disk_gpt_partition_entry *alternate_entries,
								   size_t allocated_n_bytes_header,
								   size_t allocated_n_bytes_entries)
{
	// Read GPT header to determine what to do with this disk metadata.
	int								n_bytes_entries_fr_gpt_header = -1;
	uint64_t						primary_header_pbyte_s = primary_gpt_header_pba * pblk_size;
	uint64_t						alternate_header_pbyte_s = alternate_gpt_header_pba * pblk_size;
	uint64_t						primary_entries_pbyte_s = 0;
	uint64_t						alternate_entries_pbyte_s = 0;
	uint64_t						calculated_primary_entries_pbyte_s;
	uint64_t						calculated_alternate_entries_max_pbyte_s;

	NFIN;
	*primary_header_validity = read_gpt_header_from_pos(nl_ctx, fd, primary_header_pbyte_s, pblk_size, primary_header, gpt->main_or_metadata, "Primary", allocated_n_bytes_header);
	*alternate_header_validity = read_gpt_header_from_pos(nl_ctx, fd, alternate_header_pbyte_s, pblk_size, alternate_header, gpt->main_or_metadata, "Alternate", allocated_n_bytes_header);

	// Fixup the ..._entries_pbyte_s and n_bytes_entries as much as possible
	calculated_primary_entries_pbyte_s = (primary_gpt_header_pba + 1) * pblk_size;
	if (*primary_header_validity == GPT_VALIDITY_OK) {
		int max_n_entries = ((primary_header->first_usable_pba - primary_header->partition_entry_pba) * pblk_size) / primary_header->size_of_partition_entry;
		if (primary_header->n_partition_entries > 128 && primary_header->n_partition_entries < max_n_entries)		// needed for upgrade from 1.2
			max_n_entries = primary_header->n_partition_entries;
		if (max_n_entries>0 && max_n_entries<=LARGE_GPT_MAX_NUM_GPT_ENTRIES)
			gpt->max_n_entries = max_n_entries;

		primary_entries_pbyte_s = (primary_header->partition_entry_pba * pblk_size);
		n_bytes_entries_fr_gpt_header = roundup(sizeof(struct nvmeibt_disk_gpt_partition_entry) * gpt->max_n_entries, pblk_size);
		if (primary_entries_pbyte_s != calculated_primary_entries_pbyte_s) {
			N_Ef(error_disk_metadata_read_all_4_gpt_structs, "primary_entries_pbyte_s gpt=@ZX calculated=@ZX", primary_entries_pbyte_s, calculated_primary_entries_pbyte_s);
			*primary_header_validity = GPT_VALIDITY_DATA_ERR;
			primary_entries_pbyte_s = calculated_primary_entries_pbyte_s;
		}
	}
	else {
		primary_entries_pbyte_s = calculated_primary_entries_pbyte_s;
		// Do not update n_bytes_entries_fr_gpt_header, since we didn't read it
	}

	if (*alternate_header_validity == GPT_VALIDITY_OK) {
		int max_n_entries = ((alternate_header->first_usable_pba - alternate_header->alternate_pba - 1) * pblk_size) / alternate_header->size_of_partition_entry;
		if (alternate_header->n_partition_entries > 128 && alternate_header->n_partition_entries < max_n_entries)		// needed for upgrade from 1.2
			max_n_entries = alternate_header->n_partition_entries;
		if (*primary_header_validity == GPT_VALIDITY_OK && max_n_entries != gpt->max_n_entries) {
			N_Ef(error_21_disk_metadata_read_all_4_gpt_structs, "Different GPT sizes on primary and alternate with valid CRC! n1=@INT n2=@INT",
					gpt->max_n_entries, max_n_entries);
		}
		else if (max_n_entries>0 && max_n_entries<=LARGE_GPT_MAX_NUM_GPT_ENTRIES)
			gpt->max_n_entries = max_n_entries;

		alternate_entries_pbyte_s = (alternate_header->partition_entry_pba * pblk_size);
		n_bytes_entries_fr_gpt_header = roundup(sizeof(struct nvmeibt_disk_gpt_partition_entry) * gpt->max_n_entries, pblk_size);
		calculated_alternate_entries_max_pbyte_s = (alternate_gpt_header_pba * pblk_size) - n_bytes_entries_fr_gpt_header;
		if (alternate_entries_pbyte_s > calculated_alternate_entries_max_pbyte_s) {	// Do not exceed the disk
			// Not checking exactly, since the gpt might have been saved with a
			//  version that behaves differently.
			N_Ef(error_1_disk_metadata_read_all_4_gpt_structs, "alternate_entries_pbyte_s:(gpt=@ZX > calculated=@ZX). header->partition_entry_pba=@ZX alternate_gpt_header_pba=@ZX",
				alternate_entries_pbyte_s, calculated_alternate_entries_max_pbyte_s, alternate_header->partition_entry_pba, alternate_gpt_header_pba);
			*alternate_header_validity = GPT_VALIDITY_DATA_ERR;
			alternate_entries_pbyte_s = (alternate_gpt_header_pba * pblk_size) - n_bytes_entries_fr_gpt_header;
		}
	}
	else {
		alternate_entries_pbyte_s = (alternate_gpt_header_pba * pblk_size) - n_bytes_entries_fr_gpt_header;
	}

	if (n_bytes_entries_fr_gpt_header == -1) {
		N_Tf(trace_disk_metadata_read_all_4_gpt_structs, "Both GPT headers are invalid, cannot read the entries array");
		goto out;
	}
	// Read the entries arrays
	if (*primary_header_validity == GPT_VALIDITY_OK)
		*primary_entries_validity = read_gpt_entries_from_pos(nl_ctx, fd, primary_entries_pbyte_s, pblk_size, n_bytes_entries_fr_gpt_header,
															  primary_header, primary_entries, gpt, "Primary", allocated_n_bytes_entries);
	else
		*primary_entries_validity = GPT_VALIDITY_DATA_ERR;
	if (*alternate_header_validity == GPT_VALIDITY_OK)
		*alternate_entries_validity = read_gpt_entries_from_pos(nl_ctx, fd, alternate_entries_pbyte_s, pblk_size, n_bytes_entries_fr_gpt_header,
																alternate_header, alternate_entries, gpt, "Alternate", allocated_n_bytes_entries);
	else
		*alternate_entries_validity = GPT_VALIDITY_DATA_ERR;
out:
	NFOUT;
}

/**
 * Read all 4 GPT structures into caller-allocated buffers
 * Caller must allocate buffers before calling and free them after
 */
void nvmeibt_disk_metadata_read_all_4_gpt_structs_into_buffers(
	struct netlink_io_context *nl_ctx,
	int fd,
	int pblk_size,
	struct nvmeibt_disk_gpt *gpt,
	uint64_t pba_s,
	uint64_t pba_e,
	enum GPT_VALIDITY *primary_header_validity,
	enum GPT_VALIDITY *alternate_header_validity,
	enum GPT_VALIDITY *primary_entries_validity,
	enum GPT_VALIDITY *alternate_entries_validity,
	struct nvmeibt_disk_gpt_header *primary_header,
	struct nvmeibt_disk_gpt_header *alternate_header,
	struct nvmeibt_disk_gpt_partition_entry *primary_entries,
	struct nvmeibt_disk_gpt_partition_entry *alternate_entries)
{
	int		n_bytes_header;
	int		n_bytes_entries;

	NFIN;

	n_bytes_header = roundup(sizeof(struct nvmeibt_disk_gpt_header), pblk_size);
	n_bytes_entries = roundup(sizeof(struct nvmeibt_disk_gpt_partition_entry) * max(LARGE_GPT_MAX_NUM_GPT_ENTRIES, MAX_NUM_GPT_ENTRIES), pblk_size);

	read_all_4_gpt_structs(nl_ctx, fd, pblk_size, gpt, pba_s, pba_e,
						   primary_header_validity, alternate_header_validity,
						   primary_entries_validity, alternate_entries_validity,
						   primary_header, alternate_header,
						   primary_entries, alternate_entries,
						   n_bytes_header, n_bytes_entries);

	NFOUT;
}

int nvmeibt_disk_metadata_restore_gpt(struct netlink_io_context *nl_ctx,
		  	  	  	  	  	  	  	  int fd,
									  int pblk_size,
									  struct nvmeibt_disk_gpt *gpt,
									  uint64_t primary_gpt_header_pba,
									  uint64_t alternate_gpt_header_pba,
									  BOOL do_recover)
{
	enum GPT_VALIDITY		primary_header_validity = GPT_VALIDITY_UNKNOWN;
	enum GPT_VALIDITY		alternate_header_validity = GPT_VALIDITY_UNKNOWN;
	enum GPT_VALIDITY		primary_entries_validity = GPT_VALIDITY_UNKNOWN;
	enum GPT_VALIDITY		alternate_entries_validity = GPT_VALIDITY_UNKNOWN;
	struct nvmeibt_disk_gpt_header  			*primary_header;
	struct nvmeibt_disk_gpt_header  			*alternate_header;
	struct nvmeibt_disk_gpt_partition_entry		*primary_entries;
	struct nvmeibt_disk_gpt_partition_entry		*alternate_entries;
	int											i;
	struct nvmeibt_disk_gpt_partition_entry		*gpt_entry;
	int					n_bytes_header;
	int 				n_bytes_entries;
	BOOL				are_entries_equal;
	BOOL				are_headers_equal_up_to_relocation;
	BOOL				is_primary_OK;
	BOOL				is_alternate_OK;
	char				which_needs_to_be_recovered;

	NFIN;
	gpt->is_valid = false;

	n_bytes_header = roundup(sizeof(struct nvmeibt_disk_gpt_header), pblk_size);
	n_bytes_entries = roundup(sizeof(struct nvmeibt_disk_gpt_partition_entry) * max(LARGE_GPT_MAX_NUM_GPT_ENTRIES, MAX_NUM_GPT_ENTRIES), pblk_size);
	primary_header = NNVMEIBT_BM_ALIGNED_CALLOC(trace_disk_metadata_nvmeibt_disk_metadata_restore_gpt, PAGE_SIZE, n_bytes_header);
	alternate_header = NNVMEIBT_BM_ALIGNED_CALLOC(trace_1_disk_metadata_nvmeibt_disk_metadata_restore_gpt, PAGE_SIZE, n_bytes_header);
	primary_entries = NNVMEIBT_BM_ALIGNED_CALLOC(trace_2_disk_metadata_nvmeibt_disk_metadata_restore_gpt, PAGE_SIZE, n_bytes_entries);
	alternate_entries = NNVMEIBT_BM_ALIGNED_CALLOC(trace_3_disk_metadata_nvmeibt_disk_metadata_restore_gpt, PAGE_SIZE, n_bytes_entries);

	nvmeibt_disk_metadata_read_all_4_gpt_structs_into_buffers(nl_ctx, fd, pblk_size, gpt, primary_gpt_header_pba, alternate_gpt_header_pba,
															   &primary_header_validity, &alternate_header_validity,
															   &primary_entries_validity, &alternate_entries_validity,
															   primary_header, alternate_header,
															   primary_entries, alternate_entries);
	is_primary_OK = ((primary_header_validity == GPT_VALIDITY_OK) && (primary_entries_validity == GPT_VALIDITY_OK));
	is_alternate_OK = ((alternate_header_validity == GPT_VALIDITY_OK) && (alternate_entries_validity == GPT_VALIDITY_OK));

	// Analyze the the GPTs, detect partial save/corruptions, and decide which header and entries to use
	if (!is_alternate_OK && !is_primary_OK) {
		N_Wf(ghuyt75, "Both @STR-GPTs are invalid validities=(PriHead=@STR AltHead=@STR PriEntries=@STR AltEntries=@STR", gpt->main_or_metadata,
			gpt_validity_str(primary_header_validity), gpt_validity_str(alternate_header_validity), gpt_validity_str(primary_entries_validity), gpt_validity_str(alternate_entries_validity));
		nvmeibt_disk_metadata_print_gpt_header(primary_header, gpt->main_or_metadata, "Primary");
		nvmeibt_disk_metadata_print_all_gpt_entries(primary_entries, gpt->max_n_entries, true, gpt->main_or_metadata, "Primary", gpt->ldisk_id.str);
		nvmeibt_disk_metadata_print_gpt_header(alternate_header, gpt->main_or_metadata, "Alternate");
		nvmeibt_disk_metadata_print_all_gpt_entries(alternate_entries, gpt->max_n_entries, true, gpt->main_or_metadata, "Alternate", gpt->ldisk_id.str);
		goto out;
	}

	n_bytes_entries = (is_primary_OK ? gpt->max_n_entries * (int)sizeof(struct nvmeibt_disk_gpt_partition_entry) :
					   is_alternate_OK ? gpt->max_n_entries * (int)sizeof(struct nvmeibt_disk_gpt_partition_entry) :
					   n_bytes_entries);
	are_entries_equal = nvmeibt_disk_metadata_are_gpt_entries_equal(primary_entries, alternate_entries, n_bytes_entries);
	are_headers_equal_up_to_relocation = nvmeibt_disk_metadata_are_gpt_headers_equal(primary_header, alternate_header);

	// Log the findings, and decide which to use
	if (is_primary_OK && is_alternate_OK && are_headers_equal_up_to_relocation && are_entries_equal) {
		N_Tf(trace_4_disk_metadata_nvmeibt_disk_metadata_restore_gpt, "The two @STR-GPTs match perfectly", gpt->main_or_metadata);
		which_needs_to_be_recovered = 'N';
	} else if (is_primary_OK && is_alternate_OK) {
		N_Tf(trace_5_disk_metadata_nvmeibt_disk_metadata_restore_gpt, "The two @STR-GPTs are OK but differ. are_headers_equal_up_to_relocation=@BOOL are_entries_equal=@BOOL",
			gpt->main_or_metadata, are_headers_equal_up_to_relocation, are_entries_equal);
		nvmeibt_disk_metadata_print_gpt_header(alternate_header, gpt->main_or_metadata, "Alternate");
		nvmeibt_disk_metadata_print_all_gpt_entries(alternate_entries, gpt->max_n_entries, true, gpt->main_or_metadata, "Alternate", gpt->ldisk_id.str);
		which_needs_to_be_recovered = 'A';
	} else if (is_primary_OK) {
		N_Tf(trace_6_disk_metadata_nvmeibt_disk_metadata_restore_gpt, "Primary OK. alternate_validity:(header=@HEADER entries=@STR)", gpt_validity_str(alternate_header_validity), gpt_validity_str(alternate_entries_validity));
		which_needs_to_be_recovered = 'A';
	} else {
		N_Tf(trace_7_disk_metadata_nvmeibt_disk_metadata_restore_gpt, "Alternate OK. primary_validity:(header=@HEADER entries=@STR)", gpt_validity_str(primary_header_validity), gpt_validity_str(primary_entries_validity));
		which_needs_to_be_recovered = 'P';
	}

	if (which_needs_to_be_recovered == 'P') {
		relocate_alternate_gpt_header_to_primary(alternate_header, primary_header);	// Fix the primary
	}
	memcpy(&(gpt->header), primary_header, sizeof(gpt->header));    // We always use the primary
	memcpy(&(gpt->entries), (which_needs_to_be_recovered == 'P' ? alternate_entries : primary_entries), sizeof(gpt->entries));
	// Count gpt->n_entries_in_use
	gpt->n_entries_in_use = 0;
	for (i = 0; i < gpt->max_n_entries; i++) {
		gpt_entry = &(gpt->entries[i]);
		if (nvmeibt_disk_metadata_is_gpt_entry_in_use(gpt_entry)) {
			gpt->n_entries_in_use++;
		}
	}

	nvmeibt_disk_metadata_print_all_gpt_entries(gpt->entries, gpt->max_n_entries, true, gpt->main_or_metadata, "Mem", gpt->ldisk_id.str);

	if (!do_recover)
		which_needs_to_be_recovered = 'N';

	// Recover if needed, so that we start from a stable point
	if (which_needs_to_be_recovered == 'P') {
		if (store_gpt(nl_ctx, fd, gpt, pblk_size, &(gpt->header), gpt->entries, "Primary", false) < 0) {
			N_Wf(warn_1_disk_metadata_nvmeibt_disk_metadata_restore_gpt, "disk=@STR Failed to recover the @STR-Primary-GPT", gpt->ldisk_id.str, gpt->main_or_metadata);
			goto out;
		}
	} else if (which_needs_to_be_recovered == 'A') {
		translate_primary_gpt_header_to_alternate(primary_header, alternate_header, pblk_size);
		if (store_gpt(nl_ctx, fd, gpt, pblk_size, alternate_header, gpt->entries, "Alternate", false) < 0) {
			N_Wf(warn_2_disk_metadata_nvmeibt_disk_metadata_restore_gpt, "disk=@STR Failed to recover the @STR-Alternate-GPT", gpt->ldisk_id.str, gpt->main_or_metadata);
			goto out;
		}
	}

	gpt->is_valid = true;
out:
	NNVMEIBT_BM_FREE(trace_8_disk_metadata_nvmeibt_disk_metadata_restore_gpt, primary_header);
	NNVMEIBT_BM_FREE(trace_9_disk_metadata_nvmeibt_disk_metadata_restore_gpt, alternate_header);
	NNVMEIBT_BM_FREE(trace_10_disk_metadata_nvmeibt_disk_metadata_restore_gpt, primary_entries);
	NNVMEIBT_BM_FREE(trace_11_disk_metadata_nvmeibt_disk_metadata_restore_gpt, alternate_entries);
	NFOUT;
	return (gpt->is_valid ? 0 : -1);
}

/**
 *
 *
 * @author max (12/28/17)
 *
 * @param disk_metadata
 */
void print_disk_metadata(const struct nvmeibt_disk_metadata *disk_metadata)
{
	N_Tf(dhyu734,
		 "disk_metadata version=@INT " LOCAL_DISK_LOG_FMT " "
		 "is_valid=@BOOL_YN format_pblk_size=@FORMAT_PBLK_SIZE "
		 "format_metadata_size=@FORMAT_METADATA_SIZE "
		 "is_md_supported=@BOOL_YN mgmt_db_uuid=@UUID_LE, "
		 "last_pba_zeroed=@LAST_PBA_ZEROED "
		 "format_request_counter=@FORMAT_REQUEST_COUNTER crc=@CRC disk_version=@INT native_nguid_unused=@UUID_LE",
		disk_metadata->disk_metadata_version,
		 disk_metadata->ldisk_id_str, disk_metadata->native_serial_str, disk_metadata->nsid,
		disk_metadata->signature == DISK_METADATA_SIGNATURE,
		disk_metadata->format_pblk_size,
		disk_metadata->format_metadata_size,
		disk_metadata->is_md_supported,
		&disk_metadata->mgmt_db_uuid,
		disk_metadata->last_pba_zeroed,
		disk_metadata->format_request_counter,
		disk_metadata->crc32,
		disk_metadata->disk_version_unused,
		 &disk_metadata->native_nguid_unused
	);
}

int nvmeibt_disk_metadata_read_disk_metadata(struct netlink_io_context *nl_ctx, int fd,
											 int pblk_size,
											 uint64_t pbyte_s,
											 struct nvmeibt_disk_metadata *disk_metadata)
{
	int				rv = -1;
	int				read_crc32;
	int				calculated_crc32;

	NFIN;
	N_Tf(ywbsd93, "Trying to read disk_metadata from pbyte_s=@OFFSET_INT read_size=@LD", pbyte_s, sizeof(*disk_metadata));
	if (nvmeibt_disk_metadata_do_sync_IO_with_disk_netlink_or_not(nl_ctx, fd, disk_metadata, pbyte_s, pblk_size, sizeof(*disk_metadata), NULL, 0, NVMEIB_IO_IS_READ, 0, pblk_size*4) < 0) {
		goto out;
	}
	print_disk_metadata(disk_metadata);
	read_crc32 = disk_metadata->crc32;
	disk_metadata->crc32 = 0; // Need to set crc to 0 in struct, before calculation
	// Now recalculate the header crc.
	if (disk_metadata->disk_metadata_version == TOMA_DISK_METADATA_VERSION_V1_3_1) {
		calculated_crc32 = crc32_seedless(disk_metadata, (void *)&(disk_metadata->marker_of_end_of_fields_PRE_1_3_1[0]) - (void *)disk_metadata + 300);	// The size back then
		N_Tf(mdio340, "disk_metadata_version=@X upgrading", disk_metadata->disk_metadata_version);
		nvmeibt_strlcpy(disk_metadata->native_serial_str, "", sizeof(disk_metadata->native_serial_str));	// We should read it (from smart) so init
		disk_metadata->ldisk_id_str[0] = '\0';
		disk_metadata->nsid = -1;
		disk_metadata->native_nguid_unused = nvmeib_uuid_null_val;
	} else {
		calculated_crc32 = crc32_seedless(disk_metadata, sizeof(*disk_metadata));
	}
	// Check crc.
	if (read_crc32 != calculated_crc32) {
		N_Ef(rtb4yu4, "Error: pbyte_s=@POS read_crc=@X calculated=@X", pbyte_s, read_crc32, calculated_crc32);
		goto out;
	}
	// Check if the disk_metadata header is valid.
	if (disk_metadata->signature != DISK_METADATA_SIGNATURE) {
		N_Ef(bd8954u, "disk_metadata signature mismatch. Expected=@GPT_SIGNATURE got=@GPT_SIGNATURE pbyte_s=@POS", DISK_METADATA_SIGNATURE, disk_metadata->signature, pbyte_s);
		goto out;
	}
	disk_metadata->crc32 = read_crc32;		// Restore crc32 to original value after validation passed
	rv = 0;
	N_Tf(vbyhduy, "Restored disk_metadata fd=@FD", fd);
out:
	NFOUT;
	return rv;
}

/**
 * Store the GPT header and entries on disk
 *
 * @author max (5/28/18)
 *
 * @param dev_file_name
 * @param pblk_size
 * @param gpt_header
 * @param entries
 *
 * @return int
 */
static int store_gpt(struct netlink_io_context *nl_ctx,
					 int stock_fd,
					 const struct nvmeibt_disk_gpt *gpt,
					 int pblk_size,
					 const struct nvmeibt_disk_gpt_header *gpt_header,
					 const struct nvmeibt_disk_gpt_partition_entry *entries,
					 char *gpt_primary_or_alternate_or_mem_str,
					 bool init_serjio)
{
	struct nvmeibt_disk_gpt_header	midst_write_header = *gpt_header;
	int rv = -1;
	bool							is_main_gpt_update;

	NFIN;
	is_main_gpt_update = (strncmp(gpt->main_or_metadata, MAIN_GPT_NAME, sizeof(MAIN_GPT_NAME) - 1) == 0);
	// First, mark MIDST_WRITE in the GPT header, so that we do not trust the CRC to detect incomplete writes
	midst_write_header.gpt_signature = MIDST_WRITE_GPT_SIGNATURE;
	if (nvmeibt_disk_gpt_header_store(nl_ctx, stock_fd, pblk_size, is_main_gpt_update, entries, gpt->max_n_entries, &midst_write_header, init_serjio) < 0) {
		N_Ef(fju87r4, "Unable to store @STR-@STR-GPT MIDST_WRITE_header disk=@STR", gpt->main_or_metadata, gpt_primary_or_alternate_or_mem_str, gpt->ldisk_id.str);
		goto out;
	}

	// Second, store the GPT table
	if (nvmeibt_disk_metadata_gpt_entries_store(nl_ctx, stock_fd, pblk_size, entries, gpt_header->partition_entry_pba, gpt->max_n_entries,
												gpt->main_or_metadata, gpt_primary_or_alternate_or_mem_str, gpt->ldisk_id.str, init_serjio) < 0) {
		N_Ef(rcixdl0, "Unable to store @STR-@STR-GPT entries on disk=@STR", gpt->main_or_metadata, gpt_primary_or_alternate_or_mem_str, gpt->ldisk_id.str);
		goto out;
	}

	// Lastly, store the updated GPT header
	if (nvmeibt_disk_gpt_header_store(nl_ctx, stock_fd, pblk_size, is_main_gpt_update, entries, gpt->max_n_entries, gpt_header, init_serjio) < 0) {
		N_Ef(aloit95, "Unable to store @STR-@STR-GPT header on disk=@STR", gpt->main_or_metadata, gpt_primary_or_alternate_or_mem_str, gpt->ldisk_id.str);
		goto out;
	}

	rv = 0;
out:
	NFOUT;
	return rv;
}

/**
 * Store a single GPT copy (either primary or alternate)
 * This is the building-block function for writing GPT copies.
 * Use this when you need to write only one copy (e.g., repair, testing, JSON restore).
 *
 * @param nl_ctx        Netlink context (can be NULL if using fd)
 * @param fd            File descriptor (used if nl_ctx is NULL)
 * @param pblk_size     Physical block size
 * @param gpt           GPT structure to write (entries and header info)
 * @param is_alternate  true = write alternate copy, false = write primary copy
 * @param init_serjio   Initialize serjio flag (typically false for external callers)
 * @return              0 on success, -1 on failure
 */
int nvmeibt_disk_metadata_store_gpt_one_copy(struct netlink_io_context *nl_ctx,
											 int fd,
											 int pblk_size,
											 struct nvmeibt_disk_gpt *gpt,
											 BOOL is_alternate,
											 BOOL init_serjio)
{
	int								rv = -1;
	struct nvmeibt_disk_gpt_header	gpt_header;
	char							*copy_name;

	NFIN;

	if (!nl_ctx && fd < 0) {
		N_Ef(store_gpt_copy_bad_ctx, "Invalid netlink context or fd");
		goto out;
	}

	// Update CRCs before storing (idempotent - safe to call multiple times)
	update_gpt_crcs(gpt);

	if (is_alternate) {
		// Translate primary header to alternate header (different my_pba, alternate_pba, partition_entry_pba)
		translate_primary_gpt_header_to_alternate(&gpt->header, &gpt_header, pblk_size);
		copy_name = "Alternate";
	} else {
		gpt_header = gpt->header;
		copy_name = "Primary";
	}

	N_Tf(store_gpt_copy_trace, "Storing @STR-@STR-GPT @STR pblk_size=@BLOCK_SIZE",
		 gpt->main_or_metadata, copy_name, gpt->ldisk_id.str, pblk_size);

	if (store_gpt(nl_ctx, fd, gpt, pblk_size, &gpt_header, gpt->entries, copy_name, init_serjio) < 0) {
		N_Ef(store_gpt_copy_failed, "Unable to store @STR-@STR-GPT @STR",
			 gpt->main_or_metadata, copy_name, gpt->ldisk_id.str);
		goto out;
	}

	nvmeibt_disk_metadata_print_gpt_header(&gpt_header, gpt->main_or_metadata, copy_name);

	rv = 0;
out:
	NFOUT;
	return rv;
}

/**
 * Store both GPT copies (primary and backup) to disk
 * Writes backup first, then primary (standard order for safety).
 * This is a convenience wrapper around nvmeibt_disk_metadata_store_gpt_copy().
 *
 * @param nl_ctx        Netlink context (can be NULL if using fd)
 * @param fd            File descriptor (used if nl_ctx is NULL)
 * @param pblk_size     Physical block size
 * @param gpt           GPT structure to write
 * @param init_serjio   Initialize serjio flag
 * @return              0 on success, -1 on failure
 */
int nvmeibt_disk_metadata_store_gpt(struct netlink_io_context *nl_ctx,
									int fd,
									int pblk_size,
									struct nvmeibt_disk_gpt *gpt,
									bool init_serjio)
{
	int		rv = -1;

	NFIN;

	// Write backup first, then primary (standard order for safety)
	if (nvmeibt_disk_metadata_store_gpt_one_copy(nl_ctx, fd, pblk_size, gpt, true, init_serjio) < 0) {
		goto out;
	}
	if (nvmeibt_disk_metadata_store_gpt_one_copy(nl_ctx, fd, pblk_size, gpt, false, init_serjio) < 0) {
		goto out;
	}

	// Print entries summary (only once for both copies)
	nvmeibt_disk_metadata_print_all_gpt_entries(gpt->entries, gpt->max_n_entries, true,
												gpt->main_or_metadata, "Mem", gpt->ldisk_id.str);

	rv = 0;
out:
	NFOUT;
	return rv;
}

int nvmeibt_disk_metadata_write_disk_metadata_due_to_zeroing_progress(struct local_disk_info *ld_info, uint64_t pba_s, uint64_t pba_e)
{
	int rv = -1;
	struct nvmeibt_disk_metadata	*disk_metadata = &(ld_info->from_config.disk_metadata);
	unsigned int write_size_bytes = sizeof(*disk_metadata);
	char *dma_buffer = NULL;
	off_t offset_bytes;

	NFIN;
	offset_bytes = pba_s * disk_metadata->format_pblk_size;
	write_size_bytes = roundup(write_size_bytes, disk_metadata->format_pblk_size);
	if (write_size_bytes > (pba_e - pba_s + 1) * disk_metadata->format_pblk_size) {
		N_Ef(isbw74h, "disk_metadata partition to small! write_size=@WRITE_SIZE partition size=@SIZEOF_LONG", write_size_bytes, (pba_e - pba_s + 1) * disk_metadata->format_pblk_size);
		nvmeibt_abort(ES_FATAL);
	}
	disk_metadata->crc32 = 0; // clear the previous val
	disk_metadata->crc32 = crc32_seedless(disk_metadata, sizeof(*disk_metadata));
	print_disk_metadata(disk_metadata);
	dma_buffer = NNVMEIBT_BM_ALIGNED_CALLOC(5vws8k2, PAGE_SIZE, write_size_bytes); /* Already zeroed buffer */
	// We must copy according to the actual size of disk_metadata, to the buffer,
	// which is guaranteed to be NO smaller than the struct size.
	memcpy(dma_buffer, disk_metadata, sizeof(*disk_metadata));
	if (nvmeibt_disk_metadata_do_sync_IO_with_disk_netlink_or_not(ld_info->nl_ctx, ld_info->fd, dma_buffer, offset_bytes, disk_metadata->format_pblk_size, write_size_bytes, NULL, 0, NVMEIB_IO_IS_WRITE, 0, disk_metadata->format_pblk_size*4) < 0) {
		goto out;
	}
	rv = 0;
	N_Tf(9sk3l0s, "disk_metadata written to offset=@OFFSET_INT written=@WRITTEN bytes", offset_bytes, write_size_bytes);
out:
	NNVMEIBT_BM_FREE(gjs042l, dma_buffer);
	NFOUT;
	return rv;
}

int nvmeibt_disk_metadata_read_mbr_blk(struct netlink_io_context *nl_ctx, int fd, int pblk_size, struct nvmeibt_disk_mbr *read_mbr,
									   const char *ld_display, struct nvmeibt_disk_mbr *reference_mbr)
{
	int					rv = -1;
	char				*dma_buffer = 0;
	int					read_size = sizeof(*read_mbr);
	struct nvmeibt_Str	*dump_str = NULL;

	NFIN;

	if (pblk_size == 0) {
		N_Ef(error_disk_metadata_nvmeibt_disk_metadata_read_mbr_blk, "block size is zero");
		goto out;
	}
	read_size = roundup(read_size, pblk_size);

	dma_buffer = NNVMEIBT_BM_ALIGNED_CALLOC(trace_disk_metadata_nvmeibt_disk_metadata_read_mbr_blk, PAGE_SIZE, read_size);

	/*
	 * At present we use pread() to read data from physical disk.
	 * Later we may use special ioctl() if supported by the underlying disk.
	 */
	if (nvmeibt_disk_metadata_do_sync_IO_with_disk_netlink_or_not(nl_ctx, fd, dma_buffer, MBR_OFFSET, pblk_size, read_size, NULL, 0, NVMEIB_IO_IS_READ, 0, 0) < 0) {
		goto out;
	}

	memcpy(read_mbr, dma_buffer, sizeof(*read_mbr));
	N_Tf(trace_1_disk_metadata_nvmeibt_disk_metadata_read_mbr_blk, "MBR read from fd=@FD offset=@OFFSET_INT size read=@READ_SIZEOF", fd, (size_t) MBR_OFFSET, sizeof(*read_mbr));
	dump_str = NNVMEIBT_STR_ALLOC(ts6ej3n);
	nvmeibt_disk_metadata_fill_dump_mbr_str(dump_str, read_mbr);
	if (reference_mbr && (memcmp(read_mbr, reference_mbr, sizeof(*read_mbr)) != 0)) {
		N_Ef(vshgs82, "MBR mismatch disk=@STR", ld_display);
		N_Ef(akamzi2, "read-MBR @READ_MBR", nvmeibt_Str_str(dump_str));
		nvmeibt_Str_reuse(dump_str);
		nvmeibt_disk_metadata_fill_dump_mbr_str(dump_str, reference_mbr);
		N_Ef(w7sjroc, "ref-MBR @REFERENCE_MBR", nvmeibt_Str_str(dump_str));
		goto out;
	} else {
		N_Tf(vssg83k, "@STR-MBR @READ_MBR", ld_display, nvmeibt_Str_str(dump_str));
	}

	rv = 0;

out:
	NNVMEIBT_STR_FREE(ydhsilk, dump_str);
	NNVMEIBT_BM_FREE(trace_4_disk_metadata_nvmeibt_disk_metadata_read_mbr_blk, dma_buffer);

	NFOUT;
	return rv;
}


/**
 * Writes the MBR (PMBR) to the first PBA of the given disk.
 *
 * @author max (7/5/17)
 *
 * @param disk
 * @param mbr
 *
 * @return int
 */
int nvmeibt_disk_metadata_write_mbr(struct netlink_io_context *nl_ctx, int fd, int pblk_size, const struct nvmeibt_disk_mbr *mbr)
{
	int rv = 0;
	int write_size = sizeof(*mbr);
	char *dma_buffer;

	NFIN;
	write_size = roundup(write_size, pblk_size);

	dma_buffer = NNVMEIBT_BM_ALIGNED_CALLOC(trace_disk_metadata_nvmeibt_disk_metadata_write_mbr, PAGE_SIZE, write_size);

	memset(dma_buffer, 0, write_size);
	memcpy(dma_buffer, mbr, sizeof(*mbr));

	if (nvmeibt_disk_metadata_do_sync_IO_with_disk_netlink_or_not(nl_ctx, fd, dma_buffer, MBR_OFFSET, pblk_size, write_size, NULL, 0, NVMEIB_IO_IS_WRITE, 0, 0) < 0) {
		rv = -1;
		goto out;
	}
	N_Tf(trace_1_disk_metadata_nvmeibt_disk_metadata_write_mbr, "MBR written to fd=@FD offset=@OFFSET_INT written=@WRITTEN bytes.", fd, (size_t) MBR_OFFSET, write_size);
out:
	NNVMEIBT_BM_FREE(trace_2_disk_metadata_nvmeibt_disk_metadata_write_mbr, dma_buffer);
	NFOUT;
	return rv;
}

/**
 * Validates MBR integrity by checking the MBR header.
 *
 * @author max (6/22/17)
 *
 * @param mbr
 *
 * @return int
 */
BOOL nvmeibt_disk_metadata_is_mbr_any_mbr(const struct nvmeibt_disk_mbr *mbr)
{
	int rv = 1;
	if (mbr->signature != (short)MBR_SIGNATURE) {
		N_Tf(trace_disk_metadata_nvmeibt_disk_metadata_is_mbr_any_mbr, "MBR signature Expected=@MBR_SIGNATURE got=@MBR_SIGNATURE. Not a PMBR.", MBR_SIGNATURE, mbr->signature);
		rv = 0;
	}
	return rv;
}

/**
 * Checks whether the given MBR is of type gpt protective.
 *
 * @author max (6/22/17)
 *
 * @param mbr
 *
 * @return int
 */
BOOL nvmeibt_disk_metadata_is_protective_mbr(const struct nvmeibt_disk_mbr *mbr)
{
	const struct nvmeibt_mbr_partition_record  *protection_record = &mbr->partitions[0];

	// We check only that this is an MBR record with os_type 0xee, since there are various issue across linux implementation
	// of specifying correct start/end_chs in pmbr records, issue date back to 2002 when some computers would not boot if
	// end_chs was set to 0xffffff, hence linux distributions started tweaking the value a bit. Apparently start_chs has some
	// issue back at at the time as well, hence it does not adhere to the UEFI standard in some cases.

	return (nvmeibt_disk_metadata_is_mbr_any_mbr(mbr) && (protection_record->os_type == PMBR_OS_TYPE));
}

/**
 * returns first gpt entry that matches given type uuid.
 *
 * @author max (7/23/17)
 *
 * @param gpt
 * @param type_uuid
 *
 * @return const struct nvmeibt_disk_gpt_partition_entry*
 */
static const struct nvmeibt_disk_gpt_partition_entry* get_gpt_entry_by_type_uuid(const struct nvmeibt_disk_gpt *gpt, const union nvmeib_uuid *type_uuid)
{
	const struct nvmeibt_disk_gpt_partition_entry *result = NULL;
	int i;

	NFIN;
	for (i = 0; i < gpt->max_n_entries; ++i) {
		const struct nvmeibt_disk_gpt_partition_entry *curr_metadata_gpt_entry = &(gpt->entries[i]);
		if (	ARE_UUID_EQ(type_uuid, &curr_metadata_gpt_entry->partition_type_guid) ||
				(ARE_UUID_EQ(type_uuid, &EXCELERO_METADATA_PARTITION_TYPE_GUID) &&	// Support of old GUID read
				 ARE_UUID_EQ(&EXCELERO_METADATA_PARTITION_TYPE_GUID_OLD, &curr_metadata_gpt_entry->partition_type_guid))) {
			result = curr_metadata_gpt_entry;
			goto out;
		}
	}

out:
	NFOUT;
	return result;
}


/**
 * Locates the Excelero metadata partition in the GPT, return NULL if non found.
 *
 * @author max (6/25/17)
 *
 * @param gpt
 *
 * @return struct nvmeibt_disk_gpt_partition_entry*
 */
const struct nvmeibt_disk_gpt_partition_entry* nvmeibt_disk_metadata_get_gpt_entry_of_metadata_gpt(const struct nvmeibt_disk_gpt *gpt)
{
	return get_gpt_entry_by_type_uuid(gpt, &EXCELERO_METADATA_PARTITION_TYPE_GUID);
}

/**
 * Locates the Excelero journal data partition in the Journal-GPT, return NULL
 * if non found.
 *
 * @author max (6/25/17)
 *
 * @param gpt
 *
 * @return struct nvmeibt_disk_gpt_partition_entry*
 */
const struct nvmeibt_disk_gpt_partition_entry* nvmeibt_disk_metadata_get_journal_data_entry(const struct nvmeibt_disk_gpt *gpt)
{
	return get_gpt_entry_by_type_uuid(gpt, &EXCELERO_JOURNAL_DATA_PARTITION_TYPE_GUID);
}


/**
 * Locates the Excelero serjio_db partition in the Journal-GPT, return NULL if
 * non found.
 *
 * @author max (6/25/17)
 *
 * @param gpt
 *
 * @return struct nvmeibt_disk_gpt_partition_entry*
 */
const struct nvmeibt_disk_gpt_partition_entry* nvmeibt_disk_metadata_get_serjio_db_entry(const struct nvmeibt_disk_gpt *gpt)
{
	return get_gpt_entry_by_type_uuid(gpt, &EXCELERO_SERJIO_DB_PARTITION_TYPE_GUID);}

/**
 * Locates the Excelero disk_metadata partition in the metadata GPT, return NULL
 * if non found.
 *
 * @author max (6/25/17)
 *
 * @param metadata_gpt
 *
 * @return struct nvmeibt_disk_gpt_partition_entry*
 */
const struct nvmeibt_disk_gpt_partition_entry* nvmeibt_disk_metadata_get_disk_metadata_entry(const struct nvmeibt_disk_gpt *metadata_gpt)
{
	return get_gpt_entry_by_type_uuid(metadata_gpt, &EXCELERO_DISK_METADATA_PARTITION_TYPE_GUID);}

/**
 * Initializes an mbr structure of type gpt protective.
 *
 * @author max (7/23/17)
 *
 * @param mbr
 * @param n_disk_pblks
 * @param pblk_size
 */
void nvmeibt_disk_metadata_init_pmbr(struct nvmeibt_disk_mbr *mbr, uint64_t n_disk_pblks, int pblk_size)
{
	struct nvmeibt_mbr_partition_record *protective_record = &mbr->partitions[0];
	int end_chs;

	NFIN;
	memset(mbr, 0, sizeof(*mbr));
	// Zero what needs to be zero for PMBR.
	memset(mbr->boot_code, 0, sizeof(mbr->boot_code));
	mbr->disk_signature = 0;
	// MBR signature
	mbr->signature = MBR_SIGNATURE;

	protective_record->boot_indicator = 0;
	memcpy(protective_record->starting_chs, &pblk_size, sizeof(protective_record->starting_chs)); //Address of PBA1 is starting CHS.
	protective_record->os_type = PMBR_OS_TYPE;
	end_chs = (n_disk_pblks <= 0xFFFFFF) ? n_disk_pblks : 0xFFFFFF;
	memcpy(protective_record->ending_chs, &end_chs, sizeof(protective_record->ending_chs));
	protective_record->pba_s = 1;
	protective_record->n_pblk = (n_disk_pblks < 0xFFFFFFFF) ? n_disk_pblks - 1 : 0xFFFFFFFF;

	NFOUT;
}

struct nvmeibt_disk_gpt_partition_entry* nvmeibt_disk_metadata_get_gpt_entry_by_uuid(struct nvmeibt_disk_gpt *gpt, const union nvmeib_uuid *uuid)
{
	struct nvmeibt_disk_gpt_partition_entry		*gpt_entry = NULL;
	int											i;

	NFIN;
	// This function should be used for non-segments entries, as we never scan/search looking for a seg.
	for (i = 0; i < gpt->max_n_entries; i++) {
		struct nvmeibt_disk_gpt_partition_entry *curr_metadata_gpt_entry = &(gpt->entries[i]);
		if(!nvmeibt_disk_metadata_is_gpt_entry_in_use(curr_metadata_gpt_entry)) {
			continue;
		}
		if (nvmeibt_disk_metadata_is_gpt_entry_active_and_matching_uuid(uuid, curr_metadata_gpt_entry)) {
			gpt_entry = curr_metadata_gpt_entry;
			goto out;
		}
	}
out:
	NFOUT;
	return gpt_entry;
}

uint64_t align_pba_up_to_4k(uint64_t pba, int pblk_size)
{
	return ((pba * pblk_size + 4095) & 0xfffffffffffff000ull) / pblk_size;
}

static int compare_gpt_partition_entry(const void *ptr1, const void *ptr2)
{
	const struct nvmeibt_disk_gpt_partition_entry *entry1 = *(struct nvmeibt_disk_gpt_partition_entry **)ptr1;
	const struct nvmeibt_disk_gpt_partition_entry *entry2 = *(struct nvmeibt_disk_gpt_partition_entry **)ptr2;

	int unused1 = !nvmeibt_disk_metadata_is_gpt_entry_in_use(entry1);
	int unused2 = !nvmeibt_disk_metadata_is_gpt_entry_in_use(entry2);

// #define TEST_QSORT
#ifdef TEST_QSORT
	N_Tf(t_rf_tomadsikmd, "Comparing: {@DS_UUID_UUID, @ZU} to {@DS_UUID_UUID, @ZU}",
			&entry1->partition_type_guid, entry1->pba_s,
			&entry2->partition_type_guid, entry2->pba_s);
#endif

	if (unused1 + unused2)
		return unused1 - unused2;

	return entry1->pba_s - entry2->pba_s;
}

/**
 * Tries to allocate a partition of a given size in the given gpt, trims the
 * beginning of the allocated partition zeroes the first block.
 *
 * @author max (1/24/18)
 *
 * @param gpt
 * @param requested_n_pblk
 *
 * @return uint64_t
 */
static uint64_t get_pba_s_for_new_partition(struct nvmeibt_disk_gpt *gpt,
								   uint64_t requested_n_pblk, int pblk_size)
{
	uint64_t alloc_pba_s = 0;
	struct nvmeibt_disk_gpt_partition_entry **sorted_entries;
	int partition_entry_ind;
	int alloc_size = sizeof(struct nvmeibt_disk_gpt_partition_entry *) * gpt->max_n_entries;
	uint64_t free_space_pba_s_aligned;

	NFIN;

	N_Tf(trace_disk_metadata_get_pba_s_for_new_partition, "Trying to allocate partition n_pblk=@ZX on @STR:@STR n_active=@N_ACTIVE n_total=@N_TOTAL",
			requested_n_pblk, gpt->ldisk_id.str, gpt->main_or_metadata, gpt->n_entries_in_use, gpt->max_n_entries);

	sorted_entries = NNVMEIBT_BM_ALIGNED_CALLOC(trace_1_disk_metadata_get_pba_s_for_new_partition, PAGE_SIZE, alloc_size);

	// Duplicate current segments to sorted segments.
	for (partition_entry_ind = 0; partition_entry_ind < gpt->max_n_entries; partition_entry_ind++) {
		sorted_entries[partition_entry_ind] = &gpt->entries[partition_entry_ind];
	}

#ifdef TEST_QSORT
	/* Randomize the order */
	for (i = 0; i < gpt->header.n_partition_entries; i++) {
		struct nvmeibt_disk_gpt_partition_entry *t;
		int j = (i * 16127) %  gpt->header.n_partition_entries;
		t = sorted_entries[i];
		sorted_entries[i] = sorted_entries[j];
		sorted_entries[j] = t;
	}

	for (i = 0; i < gpt->header.n_partition_entries; i++) {
		N_Tf(t_rg_tomadsikmd, "Randomized: sorted_entries[@INT]={@DS_UUID_UUID, @ZX}", i,
			&sorted_entries[i]->partition_type_guid, sorted_entries[i]->pba_s);
	}

#endif

	qsort(sorted_entries, gpt->max_n_entries, sizeof(sorted_entries[0]), compare_gpt_partition_entry);

#ifdef TEST_QSORT
	for (i = 0; i < gpt->header.n_partition_entries; i++) {
		N_Tf(t_rh_tomadsikmd, "Sorted: sorted_entries[@INT]={@DS_UUID_UUID, @ZX}", i,
			&sorted_entries[i]->partition_type_guid, sorted_entries[i]->pba_s);
	}
#undef TEST_QSORT
#endif

	// Check that there are no overlaps. (The first time we encounter an unused partition here it guarantees that all partition after it are also unused).
	for (partition_entry_ind = 0; partition_entry_ind < gpt->max_n_entries - 1; partition_entry_ind++) {
		struct nvmeibt_disk_gpt_partition_entry *cur = sorted_entries[partition_entry_ind],
												*next = sorted_entries[partition_entry_ind + 1];
		if (!nvmeibt_disk_metadata_is_gpt_entry_in_use(cur) ||
			!nvmeibt_disk_metadata_is_gpt_entry_in_use(next)) {
			break; // We reached the unused partitions.
		}
		if (next->pba_s <= cur->pba_e) { // overlap!!
			int j;
			N_Ef(6wmin1c, "@STR:@STR-GPT corruption! Found partitions overlap during add. max_n_entries=@INT n_entries_in_use=@INT idx=@INT next->pba_s=@UINT64_TX next->pba_e=@UINT64_TX cur->pba_s=@UINT64_TX cur->pba_e=@UINT64_TX",
				gpt->ldisk_id.str, gpt->main_or_metadata, gpt->max_n_entries, gpt->n_entries_in_use, partition_entry_ind, next->pba_s, next->pba_e, cur->pba_s, cur->pba_e);
			nvmeibt_disk_metadata_print_gpt_header(&gpt->header, gpt->main_or_metadata, "Mem");
			for (j = 0; j < gpt->n_entries_in_use; j++) {
				nvmeibt_disk_metadata_print_gpt_entry(sorted_entries[j], j);
			}
			nvmeibt_abort(ES_FATAL);
		}
	}

	free_space_pba_s_aligned = align_pba_s_up_to_blkset(gpt->header.first_usable_pba, pblk_size);

	if (gpt->n_entries_in_use == gpt->max_n_entries) {
		N_Ef(error_1_disk_metadata_get_pba_s_for_new_partition, "@STR:@STR is full, all @N_ACTIVE_PARTITIONS partition entries are used", gpt->ldisk_id.str, gpt->main_or_metadata, gpt->n_entries_in_use);
		alloc_pba_s = 0;
		goto out;
	}

	// Look for a slot large enough to fit the requested n_pblk.
	// If we encounter an unused entry it is guaranteed that all entries after it are also unused.
	for (partition_entry_ind = 0; partition_entry_ind < gpt->max_n_entries; partition_entry_ind++) {
		struct nvmeibt_disk_gpt_partition_entry *cur = sorted_entries[partition_entry_ind];
		BOOL is_cur_used = nvmeibt_disk_metadata_is_gpt_entry_in_use(cur);
		uint64_t free_space_pba_e_aligned = align_pba_e_down_to_blkset((is_cur_used ? cur->pba_s - 1 : gpt->header.last_usable_pba), pblk_size);

		if (cur->pba_s || cur->pba_e) {
			// _Tf("req_sz=0x%zx end_of_free_space=%zx cur->(start=0x%zx, end=0x%zx) last_usable_pba=0x%zx\n", requested_n_pblk, end_of_free_space, cur->start_pba, cur->end_pba, gpt->header.last_usable_pba);
		}

		// To avoid overflow.
		if (free_space_pba_e_aligned < free_space_pba_s_aligned) {
			free_space_pba_e_aligned = free_space_pba_s_aligned;
		}

		if ((free_space_pba_e_aligned - free_space_pba_s_aligned + 1) >= requested_n_pblk) {
			alloc_pba_s = free_space_pba_s_aligned;
			N_Tf(5vtsd7h, "@STR:@STR found space to allocate, for new partition of n_pblk=@N_PBLK at pba=@PBA", gpt->ldisk_id.str, gpt->main_or_metadata, requested_n_pblk, alloc_pba_s);
			break;
		}
		else {
			// Update the "assumed" start of free space.
			free_space_pba_s_aligned = align_pba_s_up_to_blkset(cur->pba_e + 1, pblk_size);
		}
	}

	if (alloc_pba_s == 0) {
		N_Wf(warn_disk_metadata_get_pba_s_for_new_partition, "Failed to alloc @STR:@STR", gpt->ldisk_id.str, gpt->main_or_metadata);
		goto out;
	}

out:
	NNVMEIBT_BM_FREE(trace_3_disk_metadata_get_pba_s_for_new_partition, sorted_entries);
	NFOUT;
	return alloc_pba_s;
}

struct nvmeibt_disk_gpt_partition_entry *nvmeibt_disk_metadata_allocate_partition_and_add_to_mem_gpt(struct nvmeibt_disk_gpt *gpt,
																									 uint64_t req_n_pblks,
																									 int pblk_size,
																									 const union nvmeib_uuid *partition_type,
																									 const union nvmeib_uuid *uuid,
																									 const char * part_name,
																									 int part_name_len,
																									 enum NVMEIBR_PARTITION_ALIGNMENT alignment_bytes)
{
	uint64_t									pba_s;
	uint64_t									pba_s_aligned;
	uint64_t									pba_e_aligned = 0;
	struct nvmeibt_disk_gpt_partition_entry		*gpt_entry;

	NFIN;

	// First check if partition with given uuid already exists. Should not happen for segments though.
	gpt_entry = nvmeibt_disk_metadata_get_gpt_entry_by_uuid(gpt, uuid);
	if (gpt_entry) {
		N_Tf(6dvcgsq, "Partition=@PARTITION is already a part of @STR-GPT disk=@STR", part_name, gpt->main_or_metadata, gpt->ldisk_id.str);
		goto out;
	}
	if (req_n_pblks == 0) {
		N_Ef(djufyrt, "Cannot add partition=@PARTITION of size=0 to @STR-GPT disk=@STR", part_name, gpt->main_or_metadata, gpt->ldisk_id.str);
		goto out;
	}
	// We need to take extra 2 times the number of alignment bytes because we need to allow for alignment
	// both in start address of allocation and of total allocated size.
	if (!(pba_s = get_pba_s_for_new_partition(gpt, req_n_pblks + (((uint64_t)alignment_bytes * 2) / pblk_size), pblk_size))) {
		N_Ef(euu876q, "Error allocating space for partition=@STR in @STR-GPT disk=@STR ", part_name, gpt->main_or_metadata, gpt->ldisk_id.str);
		goto out;
	}
	if (alignment_bytes != NVMEIBR_PARTITION_ALIGNMENT_NONE) {
		pba_s_aligned = align_pba_s_up_to_blkset(pba_s, pblk_size);
		pba_e_aligned = align_pba_s_up_to_blkset(pba_s_aligned + req_n_pblks, pblk_size) - 1;	// One before the beginning of next partition pba_s
	}
	else {
		pba_s_aligned = pba_s;
		pba_e_aligned = pba_s + req_n_pblks - 1;
	}
	// Add gpt entry
	gpt_entry = nvmeibt_disk_metadata_add_mem_gpt_entry(gpt, partition_type, uuid, pba_s_aligned, pba_e_aligned, part_name, part_name_len);
out:
	if (!gpt_entry) {
		N_Ef(2gs6xmc, "Unable to add partition=@STR to @STR-GPT disk=@STR", part_name, gpt->main_or_metadata, gpt->ldisk_id.str);
	}
	NFOUT;
	return gpt_entry;
}

/******************************************************************************/

// Partition ID's hash
struct partition_guid_name {
	union nvmeib_uuid uuid;
	const char *partition_type_name;
};

static struct partition_guid_name partition_mapping[] = {
	{ .uuid = {.ll = {0x0000000000000000, 0x0000000000000000}}, .partition_type_name = "Unused entry" },
	//
	{ .uuid = EXCELERO_METADATA_PARTITION_TYPE_GUID_CONST,			.partition_type_name = "excelero_metadata" },	// Excelero metadata
	{ .uuid = EXCELERO_SERJIO_DB_PARTITION_TYPE_GUID_CONST,			.partition_type_name = "excelero_metadata" },	// Excelero Serjio
	{ .uuid = EXCELERO_JOURNAL_DATA_PARTITION_TYPE_GUID_CONST,		.partition_type_name = "excelero_metadata" },	// Excelero Journal
	{ .uuid = EXCELERO_METADATA_PARTITION_TYPE_GUID_OLD_CONST,		.partition_type_name = "excelero_metadata" },	// OLD
	{ .uuid = EXCELERO_JOURNAL_DATA_PARTITION_TYPE_GUID_OLD_CONST,	.partition_type_name = "excelero_metadata" },	// OLD
	{ .uuid = EXCELERO_SERJIO_DB_PARTITION_TYPE_GUID_OLD_CONST,		.partition_type_name = "excelero_metadata" },	// OLD
	//
	{ .uuid = EXCELERO_DATA_PARTITION_TYPE_GUID_JOURNALED_CONST,	.partition_type_name = "data" },				// Excelero Segment Journaled
	{ .uuid = EXCELERO_DATA_PARTITION_TYPE_GUID_NO_JOURNAL_CONST,	.partition_type_name = "data" },				// Excelero Segment non-Journaled
	{ .uuid = EXCELERO_DATA_PARTITION_TYPE_GUID_DATA_OLD_CONST,		.partition_type_name = "data" },				// OLD
	//
	{ .uuid = {.ll = {0x024DEE4133E711D3, 0x9D690008C781F39F}}, .partition_type_name = "MBR partition scheme" },
	{ .uuid = {.ll = {0xC12A7328F81F11D2, 0xBA4B00A0C93EC93B}}, .partition_type_name = "EFI System partition" },
	{ .uuid = {.ll = {0x2168614864496E6F, 0x744E656564454649}}, .partition_type_name = "BIOS boot partition[e]" },
	{ .uuid = {.ll = {0xD3BFE2DE3DAF11DF, 0xBA40E3A556D89593}}, .partition_type_name = "Intel Fast Flash (iFFS) partition (for Intel Rapid Start technology)[24][25]" },
	{ .uuid = {.ll = {0xF4019732066E4E12, 0x8273346C5641494F}}, .partition_type_name = "Sony boot partition[f]" },
	{ .uuid = {.ll = {0xBFBFAFE7A34F448A, 0x9A5B6213EB736C22}}, .partition_type_name = "Lenovo boot partition[f]" },
	{ .uuid = {.ll = {0xE3C9E3160B5C4DB8, 0x817DF92DF00215AE}}, .partition_type_name = "Windows - Microsoft Reserved Partition (MSR)" },
	{ .uuid = {.ll = {0xEBD0A0A2B9E54433, 0x87C068B6B72699C7}}, .partition_type_name = "Windows - Basic data partition[g]" },
	{ .uuid = {.ll = {0x5808C8AA7E8F42E0, 0x85D2E1E90434CFB3}}, .partition_type_name = "Windows - Logical Disk Manager (LDM) metadata partition" },
	{ .uuid = {.ll = {0xAF9B60A014314F62, 0xBC683311714A69AD}}, .partition_type_name = "Windows - Logical Disk Manager data partition" },
	{ .uuid = {.ll = {0xDE94BBA406D14D40, 0xA16ABFD50179D6AC}}, .partition_type_name = "Windows - Windows Recovery Environment" },
	{ .uuid = {.ll = {0x37AFFC90EF7D4E96, 0x91C32D7AE055B174}}, .partition_type_name = "Windows - IBM General Parallel File System (GPFS) partition" },
	{ .uuid = {.ll = {0xE75CAF8FF6804CEE, 0xAFA3B001E56EFC2D}}, .partition_type_name = "Windows - Storage Spaces partition" },
	{ .uuid = {.ll = {0x75894C1E3AEB11D3, 0xB7C17B03A0000000}}, .partition_type_name = "HP-UX - Data partition" },
	{ .uuid = {.ll = {0xE2A1E72832E311D6, 0xA6827B03A0000000}}, .partition_type_name = "HP-UX - Service Partition" },
	{ .uuid = {.ll = {0x0FC63DAF84834772, 0x8E793D69D8477DE4}}, .partition_type_name = "Linux - Linux filesystem data[g]" },
	{ .uuid = {.ll = {0xA19D880F05FC4D3B, 0xA006743F0F84911E}}, .partition_type_name = "Linux - RAID partition" },
	{ .uuid = {.ll = {0x44479540F29741B2, 0x9AF7D131D5F0458A}}, .partition_type_name = "Linux - Root partition (x86)[28]" },
	{ .uuid = {.ll = {0x4F68BCE3E8CD4DB1, 0x96E7FBCAF984B709}}, .partition_type_name = "Linux - Root partition (x86-64)[28]" },
	{ .uuid = {.ll = {0x69DAD7102CE44E3C, 0xB16C21A1D49ABED3}}, .partition_type_name = "Linux - Root partition (32-bit ARM)[28]" },
	{ .uuid = {.ll = {0xB921B0451DF041C3, 0xAF444C6F280D3FAE}}, .partition_type_name = "Linux - Root partition (64-bit ARM/AArch64)[28]" },
	{ .uuid = {.ll = {0x0657FD6DA4AB43C4, 0x84E50933C84B4F4F}}, .partition_type_name = "Linux - Swap partition" },
	{ .uuid = {.ll = {0xE6D6D379F50744C2, 0xA23C238F2A3DF928}}, .partition_type_name = "Linux - Logical Volume Manager (LVM) partition" },
	{ .uuid = {.ll = {0x933AC7E12EB44F13, 0xB8440E14E2AEF915}}, .partition_type_name = "Linux - /home partition[28]" },
	{ .uuid = {.ll = {0x3B8F842520E04F3B, 0x907F1A25A76F98E8}}, .partition_type_name = "Linux - /srv (server data) partition[28]" },
	{ .uuid = {.ll = {0x8DA63339000760C0, 0xC436083AC8230908}}, .partition_type_name = "Linux - Reserved" },
	{ .uuid = {.ll = {0x83BD6B9D7F4111DC, 0xBE0B001560B84F0F}}, .partition_type_name = "FreeBSD - Boot partition" },
	{ .uuid = {.ll = {0x516E7CB46ECF11D6, 0x8FF800022D09712B}}, .partition_type_name = "FreeBSD - Data partition" },
	{ .uuid = {.ll = {0x516E7CB56ECF11D6, 0x8FF800022D09712B}}, .partition_type_name = "FreeBSD - Swap partition" },
	{ .uuid = {.ll = {0x516E7CB66ECF11D6, 0x8FF800022D09712B}}, .partition_type_name = "FreeBSD - Unix File System (UFS) partition" },
	{ .uuid = {.ll = {0x516E7CB86ECF11D6, 0x8FF800022D09712B}}, .partition_type_name = "FreeBSD - Vinum volume manager partition" },
	{ .uuid = {.ll = {0x516E7CBA6ECF11D6, 0x8FF800022D09712B}}, .partition_type_name = "FreeBSD - ZFS partition" },
	{ .uuid = {.ll = {0x48465300000011AA, 0xAA1100306543ECAC}}, .partition_type_name = "MacOS Darwin - Hierarchical File System Plus (HFS+) partition" },
	{ .uuid = {.ll = {0x7C3457EF000011AA, 0xAA1100306543ECAC}}, .partition_type_name = "MacOS Darwin - Apple APFS" },
	{ .uuid = {.ll = {0x55465300000011AA, 0xAA1100306543ECAC}}, .partition_type_name = "MacOS Darwin - Apple UFS container" },
	{ .uuid = {.ll = {0x6A898CC31DD211B2, 0x99A6080020736631}}, .partition_type_name = "MacOS Darwin - ZFS[h]" },
	{ .uuid = {.ll = {0x52414944000011AA, 0xAA1100306543ECAC}}, .partition_type_name = "MacOS Darwin - Apple RAID partition" },
	{ .uuid = {.ll = {0x524149445F4F11AA, 0xAA1100306543ECAC}}, .partition_type_name = "MacOS Darwin - Apple RAID partition: offline" },
	{ .uuid = {.ll = {0x426F6F74000011AA, 0xAA1100306543ECAC}}, .partition_type_name = "MacOS Darwin - Apple Boot partition (Recovery HD)" },
	{ .uuid = {.ll = {0x4C6162656C0011AA, 0xAA1100306543ECAC}}, .partition_type_name = "MacOS Darwin - Apple Label" },
	{ .uuid = {.ll = {0x5265636F766511AA, 0xAA1100306543ECAC}}, .partition_type_name = "MacOS Darwin - Apple TV Recovery partition" },
	{ .uuid = {.ll = {0x53746F72616711AA, 0xAA1100306543ECAC}}, .partition_type_name = "MacOS Darwin - Apple Core Storage (i.e. Lion FileVault) partition" },
	{ .uuid = {.ll = {0xB6FA30DA92D24A9A, 0x96F1871EC6486200}}, .partition_type_name = "MacOS Darwin - SoftRAID_Status" },
	{ .uuid = {.ll = {0x2E31346519B9463F, 0x81268A7993773801}}, .partition_type_name = "MacOS Darwin - SoftRAID_Scratch" },
	{ .uuid = {.ll = {0xFA709C7E65B14593, 0xBFD5E71D61DE9B02}}, .partition_type_name = "MacOS Darwin - SoftRAID_Volume" },
	{ .uuid = {.ll = {0xBBBA6DF5F46F4A89, 0x8F598765B2727503}}, .partition_type_name = "MacOS Darwin - SoftRAID_Cache" },
	{ .uuid = {.ll = {0x6A82CB451DD211B2, 0x99A6080020736631}}, .partition_type_name = "Solaris illimos - Boot partition" },
	{ .uuid = {.ll = {0x6A85CF4D1DD211B2, 0x99A6080020736631}}, .partition_type_name = "Solaris illimos - Root partition" },
	{ .uuid = {.ll = {0x6A87C46F1DD211B2, 0x99A6080020736631}}, .partition_type_name = "Solaris illimos - Swap partition" },
	{ .uuid = {.ll = {0x6A8B642B1DD211B2, 0x99A6080020736631}}, .partition_type_name = "Solaris illimos - Backup partition" },
	{ .uuid = {.ll = {0x6A898CC31DD211B2, 0x99A6080020736631}}, .partition_type_name = "Solaris illimos - /usr partition[h]" },
	{ .uuid = {.ll = {0x6A8EF2E91DD211B2, 0x99A6080020736631}}, .partition_type_name = "Solaris illimos - /var partition" },
	{ .uuid = {.ll = {0x6A90BA391DD211B2, 0x99A6080020736631}}, .partition_type_name = "Solaris illimos - /home partition" },
	{ .uuid = {.ll = {0x6A9283A51DD211B2, 0x99A6080020736631}}, .partition_type_name = "Solaris illimos - Alternate sector" },
	{ .uuid = {.ll = {0x6A9630D11DD211B2, 0x99A6080020736631}}, .partition_type_name = "Solaris illimos - Reserved partition" },
	{ .uuid = {.ll = {0x6A9807671DD211B2, 0x99A6080020736631}}, .partition_type_name = "Solaris illimos - Reserved partition" },
	{ .uuid = {.ll = {0x6A96237F1DD211B2, 0x99A6080020736631}}, .partition_type_name = "Solaris illimos - Reserved partition" },
	{ .uuid = {.ll = {0x6A8D2AC71DD211B2, 0x99A6080020736631}}, .partition_type_name = "Solaris illimos - Reserved partition" },
	{ .uuid = {.ll = {0x6A945A3B1DD211B2, 0x99A6080020736631}}, .partition_type_name = "Solaris illimos - Reserved partition" },
	{ .uuid = {.ll = {0x49F48D32B10E11DC, 0xB99B0019D1879648}}, .partition_type_name = "NetBSD - Swap partition" },
	{ .uuid = {.ll = {0x49F48D5AB10E11DC, 0xB99B0019D1879648}}, .partition_type_name = "NetBSD - FFS partition" },
	{ .uuid = {.ll = {0x49F48D82B10E11DC, 0xB99B0019D1879648}}, .partition_type_name = "NetBSD - LFS partition" },
	{ .uuid = {.ll = {0x49F48DAAB10E11DC, 0xB99B0019D1879648}}, .partition_type_name = "RAID partition" },
	{ .uuid = {.ll = {0x2DB519C4B10F11DC, 0xB99B0019D1879648}}, .partition_type_name = "NetBSD - Concatenated partition" },
	{ .uuid = {.ll = {0x2DB519ECB10F11DC, 0xB99B0019D1879648}}, .partition_type_name = "NetBSD - Encrypted partition" },
	{ .uuid = {.ll = {0xFE3A2A5D4F3241A7, 0xB725ACCC3285A309}}, .partition_type_name = "ChromeOS - kernel" },
	{ .uuid = {.ll = {0x3CB8E2023B7E47DD, 0x8A3C7FF2A13CFCEC}}, .partition_type_name = "ChromeOS - rootfs" },
	{ .uuid = {.ll = {0x2E0A753D9E4843B0, 0x8337B15192CB1B5E}}, .partition_type_name = "ChromeOS - future use" },
	{ .uuid = {.ll = {0x424653313BA310F1, 0x802A4861696B7521}}, .partition_type_name = "Haiku-  BFS" },
	{ .uuid = {.ll = {0x85D5E45E237C11E1, 0xB4B3E89A8F7FC3A7}}, .partition_type_name = "MidnightBSD - Boot partition" },
	{ .uuid = {.ll = {0x85D5E45A237C11E1, 0xB4B3E89A8F7FC3A7}}, .partition_type_name = "MidnightBSD - Data partition" },
	{ .uuid = {.ll = {0x85D5E45B237C11E1, 0xB4B3E89A8F7FC3A7}}, .partition_type_name = "MidnightBSD - Swap partition" },
	{ .uuid = {.ll = {0x0394EF8B237E11E1, 0xB4B3E89A8F7FC3A7}}, .partition_type_name = "MidnightBSD - Unix File System (UFS) partition" },
	{ .uuid = {.ll = {0x85D5E45C237C11E1, 0xB4B3E89A8F7FC3A7}}, .partition_type_name = "MidnightBSD - Vinum volume manager partition" },
	{ .uuid = {.ll = {0x85D5E45D237C11E1, 0xB4B3E89A8F7FC3A7}}, .partition_type_name = "MidnightBSD - ZFS partition" },
	{ .uuid = {.ll = {0x45B0969E9B034F30, 0xB4C6B4B80CEFF106}}, .partition_type_name = "CEPH - Journal" },
	{ .uuid = {.ll = {0x45B0969E9B034F30, 0xB4C65EC00CEFF106}}, .partition_type_name = "CEPH - dm-crypt journal" },
	{ .uuid = {.ll = {0x4FBD7E299D2541B8, 0xAFD0062C0CEFF05D}}, .partition_type_name = "CEPH - OSD" },
	{ .uuid = {.ll = {0x4FBD7E299D2541B8, 0xAFD05EC00CEFF05D}}, .partition_type_name = "CEPH - dm-crypt OSD" },
	{ .uuid = {.ll = {0x89C57F982FE54DC0, 0x89C1F3AD0CEFF2BE}}, .partition_type_name = "CEPH - Disk in creation" },
	{ .uuid = {.ll = {0x89C57F982FE54DC0, 0x89C15EC00CEFF2BE}}, .partition_type_name = "CEPH - dm-crypt disk in creation" },
	{ .uuid = {.ll = {0xCAFECAFE9B034F30, 0xB4C6B4B80CEFF106}}, .partition_type_name = "CEPH - Block" },
	{ .uuid = {.ll = {0x30CD0809C2B2499C, 0x88792D6B78529876}}, .partition_type_name = "CEPH - Block DB" },
	{ .uuid = {.ll = {0x5CE17FCE40874169, 0xB7FF056CC58473F9}}, .partition_type_name = "CEPH - Block write-ahead log" },
	{ .uuid = {.ll = {0xFB3AABF9D25F47CC, 0xBF5E721D1816496B}}, .partition_type_name = "CEPH - Lockbox for dm-crypt keys" },
	{ .uuid = {.ll = {0x4FBD7E298AE04982, 0xBF9D5A8D867AF560}}, .partition_type_name = "CEPH - Multipath OSD" },
	{ .uuid = {.ll = {0x45B0969E8AE04982, 0xBF9D5A8D867AF560}}, .partition_type_name = "CEPH - Multipath journal" },
	{ .uuid = {.ll = {0xCAFECAFE8AE04982, 0xBF9D5A8D867AF560}}, .partition_type_name = "CEPH - Multipath block" },
	{ .uuid = {.ll = {0x7F4A666A16F347A2, 0x8445152EF4D03F6C}}, .partition_type_name = "CEPH - Multipath block" },
	{ .uuid = {.ll = {0xEC6D6385E34645DC, 0xBE91DA2A7C8B3261}}, .partition_type_name = "CEPH - Multipath block DB" },
	{ .uuid = {.ll = {0x01B41E1B002A453C, 0x9F1788793989FF8F}}, .partition_type_name = "CEPH - Multipath block write-ahead log" },
	{ .uuid = {.ll = {0xCAFECAFE9B034F30, 0xB4C65EC00CEFF106}}, .partition_type_name = "CEPH - dm-crypt block" },
	{ .uuid = {.ll = {0x93B0052D02D94D8A, 0xA43B33A3EE4DFBC3}}, .partition_type_name = "CEPH - dm-crypt block DB" },
	{ .uuid = {.ll = {0x306E86834FE24330, 0xB7C000A917C16966}}, .partition_type_name = "CEPH - dm-crypt block write-ahead log" },
	{ .uuid = {.ll = {0x45B0969E9B034F30, 0xB4C635865CEFF106}}, .partition_type_name = "CEPH - dm-crypt LUKS journal" },
	{ .uuid = {.ll = {0xCAFECAFE9B034F30, 0xB4C635865CEFF106}}, .partition_type_name = "CEPH - dm-crypt LUKS block" },
	{ .uuid = {.ll = {0x166418DAC4694022, 0xADF4B30AFD37F176}}, .partition_type_name = "CEPH - dm-crypt LUKS block DB" },
	{ .uuid = {.ll = {0x86A32090364740B9, 0xBBBD38D8C573AA86}}, .partition_type_name = "CEPH - dm-crypt LUKS block write-ahead log" },
	{ .uuid = {.ll = {0x4FBD7E299D2541B8, 0xAFD035865CEFF05D}}, .partition_type_name = "CEPH - dm-crypt LUKS OSD" },
	{ .uuid = {.ll = {0x824CC7A036A811E3, 0x890A952519AD3F61}}, .partition_type_name = "OpenBSD - Data partition" },
	{ .uuid = {.ll = {0xCEF5A9AD73BC4601, 0x89F3CDEEEEE321A1}}, .partition_type_name = "QNX - Power-safe (QNX6) file system[34]" },
	{ .uuid = {.ll = {0xC91818F9802547AF, 0x89D2F030D7000C2C}}, .partition_type_name = "Plan 9 partition" },
	{ .uuid = {.ll = {0x9D27538040AD11DB, 0xBF97000C2911D1B8}}, .partition_type_name = "VMware ESX - vmkcore (coredump partition)" },
	{ .uuid = {.ll = {0xAA31E02A400F11DB, 0x9590000C2911D1B8}}, .partition_type_name = "VMware ESX - VMFS filesystem partition" },
	{ .uuid = {.ll = {0x9198EFFC31C011DB, 0x8F78000C2911D1B8}}, .partition_type_name = "VMware ESX - VMware Reserved" },
	{ .uuid = {.ll = {0x2568845D23324675, 0xBC398FA5A4748D15}}, .partition_type_name = "Android-IA - Bootloader" },
	{ .uuid = {.ll = {0x114EAFFE15524022, 0xB26E9B053604CF84}}, .partition_type_name = "Android-IA - Bootloader2" },
	{ .uuid = {.ll = {0x49A4D17F93A345C1, 0xA0DEF50B2EBE2599}}, .partition_type_name = "Android-IA - Boot" },
	{ .uuid = {.ll = {0x4177C7229E924AAB, 0x864443502BFD5506}}, .partition_type_name = "Android-IA - Recovery" },
	{ .uuid = {.ll = {0xEF32A33BA409486C, 0x91419FFB711F6266}}, .partition_type_name = "Android-IA - Misc" },
	{ .uuid = {.ll = {0x20AC26BE20B711E3, 0x84C56CFDB94711E9}}, .partition_type_name = "Android-IA - Metadata" },
	{ .uuid = {.ll = {0x38F428E6D326425D, 0x91406E0EA133647C}}, .partition_type_name = "Android-IA - System" },
	{ .uuid = {.ll = {0xA893EF21E428470A, 0x9E550668FD91A2D9}}, .partition_type_name = "Android-IA - Cache" },
	{ .uuid = {.ll = {0xDC76DDA95AC1491C, 0xAF42A82591580C0D}}, .partition_type_name = "Android-IA - Data" },
	{ .uuid = {.ll = {0xEBC597D020534B15, 0x8B64E0AAC75F4DB1}}, .partition_type_name = "Android-IA - Persistent" },
	{ .uuid = {.ll = {0xC5A0AEEC13EA11E5, 0xA1B1001E67CA0C3C}}, .partition_type_name = "Android-IA - Vendor" },
	{ .uuid = {.ll = {0xBD59408B4514490D, 0xBF129878D963F378}}, .partition_type_name = "Android-IA - Config" },
	{ .uuid = {.ll = {0x8F68CC74C5E548DA, 0xBE91A0C8C15E9C80}}, .partition_type_name = "Android-IA - Factory" },
	{ .uuid = {.ll = {0x9FDAA6EF4B3F40D2, 0xBA8DBFF16BFB887B}}, .partition_type_name = "Android-IA - Factory (alt)[39]" },
	{ .uuid = {.ll = {0x767941D0208511E3, 0xAD3B6CFDB94711E9}}, .partition_type_name = "Android-IA - Fastboot / Tertiary[40][41]" },
	{ .uuid = {.ll = {0xAC6D7924EB714DF8, 0xB48DE267B27148FF}}, .partition_type_name = "Android-IA - OEM" },
	{ .uuid = {.ll = {0x7412F7D5A1564B13, 0x81DC867174929325}}, .partition_type_name = "ONIE - Boot" },
	{ .uuid = {.ll = {0xD4E6E2CD446946F3, 0xB5CB1BFF57AFC149}}, .partition_type_name = "ONIE - Config" },
	{ .uuid = {.ll = {0x9E1A2D38C6124316, 0xAA268B49521E5A8B}}, .partition_type_name = "PowerPC - PReP boot" },
	{ .uuid = {.ll = {0xBC13C2FF59E64262, 0xA352B275FD6F7172}}, .partition_type_name = "Freedesktop - Shared boot loader configuration[42]" },
	{ .uuid = {.ll = {0x734E5AFEF61A11E6, 0xBC6492361F002671}}, .partition_type_name = "Atari TOS - Basic data partition (GEM: BGM: F32)" }
};

const char* nvmeibt_disk_metadata_get_partition_type_name(union nvmeib_uuid partition_type_guid)
{
	unsigned int	i;
	int				idx = 0;

	NFIN;
	for (i = 0; i < sizeof(partition_mapping) / sizeof(struct partition_guid_name); i++ ) {
		if (ARE_UUID_EQ(&partition_type_guid, &partition_mapping[i].uuid)) {
			idx = i;
			break;
		}
	}
	if (!idx) {
		N_Wf(trace_toma_get_partition_type_name, "Disk contains an unknown partition type @UUID_LE", &partition_type_guid);
	}
	NFOUT;
	return partition_mapping[idx].partition_type_name;
}

/******************************************************************************/

