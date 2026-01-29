/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvmeibc_mcs_stub.h"
#include "nvmeib_mcs_header.h"

typedef unsigned long long u64;

static int incarnate_item(void *item, int num_items, void *var_data, int indx)
{
	int rv = 0;
	int ioffs, item_i;
	const int *offs = incr_offst[indx];
	const int *sub_types = incr_types[indx];
	int soffs;
	unsigned long long offs_to_new_type;
	int new_type ,num_of_new_types;
	void *next_item, *arr_ptr;
	size_t sizeof_item = sizeof_items[indx];

	if (num_items <= 0)
		goto out;
	for (item_i = 0; item_i < num_items; ++item_i)
	{
		void **this_item = (void **)((char *)item + item_i * sizeof_item);
		if (!var_data)
			var_data = (void *)((char *)this_item + sizeof_item);
		for (ioffs = 0; ioffs < NUM_OFFSETS; ++ioffs)
		{
			soffs = offs[ioffs];
			if (soffs < 0)
				break;
			new_type = sub_types[ioffs];
			num_of_new_types = *(int *)((char *)this_item + soffs - sizeof(int));

			if (!num_of_new_types) {
				arr_ptr = (char *)this_item + soffs;
				*(unsigned long long *)(arr_ptr) = 0;
				continue;
			}
			offs_to_new_type = *(unsigned long long *)((char *)this_item + soffs);

			next_item = var_data + offs_to_new_type;

			if ((rv = incarnate_item(next_item, num_of_new_types,
				 var_data, new_type)) < 0)
				 goto out;

			arr_ptr = (char *)this_item + soffs;
			if (num_of_new_types > 0)
				*(unsigned long long *)(arr_ptr) = (u64)next_item;
		}
	}

out:
	return rv;
}

static void t_assert(int cond)
{
	if (!cond)
		exit(255);
}


void compose_vol_status_reply(struct nvmeibc_volume_conf *vol)
{
	int i;
	char *buffer;
	FILE *file;
	struct nvmeibc_reference_id *ref_ids;
	struct mcs_message *h;

	struct nvmeib_client_to_mgmt_vol_info *reply;
	struct nvmeibc_volume_status_payload *attch;
	size_t reply_sz = sizeof(*h) + sizeof(*reply) +
			  sizeof(*reply->attachments) +
			  (vol->attachment.n_ref_ids *
			   sizeof(*reply->attachments->referenceIDs));
	buffer = calloc(1, reply_sz);
	h = (void *)(&buffer[0]);
	t_assert(h != NULL);
	h->header.opcode = MCS_VOLUME_STATUS_MESSAGE_MSG;
	h->header.header_version = MCS_HEADER_VERSION;
	h->header.scheme_version = MCS_SCHEME_VERSION;
	h->header.msg_len.msg_len = reply_sz;
	h->header.msg_len.var_offset = sizeof(*h) + sizeof(*reply);

	reply = (struct nvmeib_client_to_mgmt_vol_info *)(&h->msg);
	reply->upstream_header.messageTypeVersion = 1LL;
	reply->upstream_header.clientToken = 1LL;
	reply->upstream_header.keepaliveInterval = 5;
	reply->upstream_header.isUmClient = 1;
	reply->upstream_header.messageSequence = 5375;
	reply->reportID = 35;
	reply->client_status = 1;
	reply->n_volumes = 1;
	attch = (struct nvmeibc_volume_status_payload *)(&buffer[sizeof(*h) + sizeof(*reply)]);
	attch->vol_status = 1;
	attch->version = vol->version;
	strcpy(attch->uuid, vol->uuid);
	strcpy(attch->name, vol->name);
	attch->n_ref_ids = vol->attachment.n_ref_ids;
	ref_ids = (struct nvmeibc_reference_id *)(&buffer[sizeof(*h) + sizeof(*reply) + sizeof(*attch)]);
	for(i = 0; i < attch->n_ref_ids; ++i) {
		strcpy(ref_ids[i].val, vol->attachment.referenceIDs[i].val);
	}
	reply->attachments = 0;
	attch->referenceIDs = (void *)(sizeof(*attch));
	file = fopen("output.bin", "wbe");
	fwrite(buffer, sizeof(char), reply_sz, file);
	fclose(file);
	free(h);
}

int main(int argc, char **argv)
{
	size_t result;
	char *buffer;
	struct mcs_message *h;
	long file_size;
	int msg_index, opcode;
	int vol_abs_ofs, tgt_abs_offs, chunk_abs_offs, refid_abs_offs,
		nics_abs_offs, disk_abs_offs, praids_abs_offs,
		disk_segments_abs_offs;
	struct nvmeib_mgmt_to_client_attach_volumes *cmd;
	struct nvmeibc_chunk_conf *chunk;
	struct nvmeibc_volume_conf *vol;
	struct nvmeibc_reference_id *reference_ids;
	struct nvmeibc_target_conf *tgt;
	struct nvmeibc_nic_conf *nics;
	struct nvmeibc_disk_conf *disks;
	struct nvmeibc_praid_conf *praids;
	struct nvmeibc_segment_conf *disk_segments;
	FILE *file = fopen("vol.dat", "rbe");

	(void)argc; (void)argv;
	fseek(file, 0, SEEK_END);  // Move the file pointer to the end of the file
	file_size = ftell(file);  // Get the position of the file pointer (size of the file)
	fseek(file, 0, SEEK_SET);

	buffer = (char *)malloc(file_size);
	result = fread(buffer, 1, file_size, file);

	t_assert((u64)result == (u64)file_size);

	h = (struct mcs_message *)buffer;
	opcode = h->header.opcode;
	msg_index = opcodes_to_incr_id_downstr[opcode];
	cmd = (struct nvmeib_mgmt_to_client_attach_volumes *)&h->msg;

	vol_abs_ofs = sizeof(*h) + sizeof(*cmd);
	vol = (struct nvmeibc_volume_conf *)(&buffer[vol_abs_ofs]);

	tgt_abs_offs = vol_abs_ofs + cmd->n_volumes * sizeof(*vol);
	tgt = (struct nvmeibc_target_conf *)(&buffer[tgt_abs_offs]);

	chunk_abs_offs = tgt_abs_offs + cmd->n_targets * sizeof(*tgt);
	chunk = (struct nvmeibc_chunk_conf *)(&buffer[chunk_abs_offs]);

	refid_abs_offs = chunk_abs_offs + vol->n_chunks * sizeof(*chunk);
	reference_ids = (struct nvmeibc_reference_id *)(&buffer[refid_abs_offs]);

	nics_abs_offs = refid_abs_offs + vol->attachment.n_ref_ids * sizeof(*reference_ids);
	nics = (struct nvmeibc_nic_conf *)(&buffer[nics_abs_offs]);

	disk_abs_offs = nics_abs_offs + tgt->n_nics * sizeof(*nics);
	disks = (struct nvmeibc_disk_conf *)(&buffer[disk_abs_offs]);

	praids_abs_offs = disk_abs_offs + tgt->n_disks * sizeof(*disks);
	praids = (struct nvmeibc_praid_conf *)(&buffer[praids_abs_offs]);

	disk_segments_abs_offs =
		praids_abs_offs + chunk->n_praids * sizeof(*praids);
	disk_segments =
		(struct nvmeibc_segment_conf *)(&buffer[disk_segments_abs_offs]);

	incarnate_item(&h->msg, 1, NULL, msg_index);

	printf("header general information\n");
	printf("==========================\n");
	printf("msg_len= %d\n", h->header.msg_len.msg_len);
	printf("msg_offset= %d\n", h->header.msg_len.var_offset);
	printf("msg_opcode= %d msg_index=%d\n", opcode, msg_index);
	printf("scheme_ver= %x\n", h->header.scheme_version);

	printf("\n");

	printf("attach command information\n");
	printf("==========================\n");
	printf("messageTypeVersion=%d\n", cmd->messageTypeVersion);
	printf("n_volumes=%d\n", cmd->n_volumes);
	printf("n_targets=%d\n", cmd->n_targets);
	printf("cmd real=%p calc=%p\n", cmd, &h->msg);
	t_assert((void *)cmd == (void *)&h->msg);
	printf("\n");

	printf("volume information\n");
	printf("==================\n");
	printf("n_refids=%d\n", vol->attachment.n_ref_ids);
	printf("n_chuncks=%d\n", vol->n_chunks);
	printf("refIds_ptr=%p\n", vol->attachment.referenceIDs);
	printf("vol ptr from cmd:%p\n", cmd->volumes);
	printf("vol real=%p calc=%p\n", vol, cmd->volumes);
	t_assert((void *)vol == (void *)cmd->volumes || !cmd->volumes);
	printf("\n");

	printf("target information\n");
	printf("==================\n");
	printf("n_disks=%d\n", tgt->n_disks);
	printf("n_nics=%d\n", tgt->n_nics);
	printf("tgt real=%p calc=%p\n", tgt, cmd->targets);
	t_assert((void *)tgt == (void *)cmd->targets);
	printf("\n");

	printf("chunk information\n");
	printf("=================\n");
	printf("n_praids=%d\n", chunk->n_praids);
	printf("chunck uuid=%s\n", chunk->uuid);
	printf("chunk real=%p calc=%p\n", chunk, vol->chunks);
	t_assert((void *)chunk == (void *)vol->chunks || !vol->chunks);
	printf("\n");

	printf("refid information\n");
	printf("==================\n");
	printf("refid=%s\n", reference_ids->val);
	printf("refid real=%p calc=%p\n", reference_ids,
	       vol->attachment.referenceIDs);
	t_assert((void *)reference_ids == (void *)vol->attachment.referenceIDs ||
		 !vol->attachment.referenceIDs);
	printf("\n");

	printf("nics information\n");
	printf("================\n");
	printf("uuid=%s\n", nics->uuid);
	printf("nodeID=%s\n", nics->nodeID);
	printf("nics real=%p calc=%p\n", nics, tgt->nics);
	t_assert((void *)nics == (void *)tgt->nics || !tgt->nics);
	printf("\n");

	printf("disks information\n");
	printf("=================\n");
	printf("diskID=%s\n", disks->diskID);
	printf("disks real=%p calc=%p\n", disks, tgt->disks);
	t_assert((void *)disks == (void *)tgt->disks || !tgt->disks);
	printf("\n");

	printf("praid information\n");
	printf("=================\n");
	printf("uuid=%s\n", praids->uuid);
	printf("parityBlocks=%d\n", praids->parityBlocks);
	printf("praid real=%p calc=%p\n", praids, chunk->praids);
	t_assert((void *)praids == (void *)chunk->praids || !chunk->praids);
	printf("\n");

	printf("disk segment information\n");
	printf("========================\n");
	printf("uuid=%s\n", disk_segments->uuid);
	printf("lbe=%llu\n", disk_segments->lbe);
	printf("status=%s\n", disk_segments->status);
	printf("segments real=%p calc=%p\n", disk_segments, praids->segments);
	t_assert((void *)disk_segments == (void *)praids->segments || !praids->segments);

	compose_vol_status_reply(vol);

	free(buffer);
	return 0;

}
