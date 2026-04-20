#include "clnt_simu.h"
#include "nvmeibt_debug.h"	// Binary tracing
#include "../13_mgmt/mongodb_simu.h"
#include "common/nvmeib_shared.h"
#include "../15_server/sandbox_nvmeibs_toma.h"

struct clnt_simu *clnt_simu_create(struct sb_cluster_conf *cfg, int node_idx) {
	struct clnt_simu *C = calloc(1, sizeof(*C));
	C->cfg = cfg;
	cfg->nodes[node_idx].clnt = C;
	C->node_idx = node_idx;
	return C;
}

void clnt_simu_destroy(struct sb_cluster_conf *cfg, int node_idx) {
	struct clnt_simu *C = cfg->nodes[node_idx].clnt;
	C->cfg->nodes[C->node_idx].clnt = NULL;
	free(C);
}

struct clnt_simu *clnt_simu_get_local_clnt(struct sb_cluster_conf *cfg) { return cfg->live->clnt; }

static void __subscribe_to_disk(struct clnt_simu *clnt, struct sb_disk_conf *disk, bool do_subscribe) {
	const struct sb_node_conf *node = &clnt->cfg->nodes[clnt->node_idx];
	const u32 cid = 0xc00 |(u32)(clnt->node_idx + 1);		// CID = live node index + 1 (non-zero)
	const u64 handle = ((u64)cid << 32) | 0x0001;			// Upper 32 bits = CID for lookup by Toma, much like nvmeibs does, 	// Todo: add disk segment index
	N_Tf(__AUTOID__, "Clnt: subscribe=@BOOL_YN handle=@HANDLE cid=@CID, disk_uuid=@X", do_subscribe, handle, cid, disk->uuid);
	if (do_subscribe) {
		if (++disk->clnts[clnt->node_idx].n_ref == 1) {
			nvmeibs_simu_subscribe_client(handle, node->hostname, disk, true);		// Otherwise, Already subscribed to this disk via another segment
		}
	} else {
		if (--disk->clnts[clnt->node_idx].n_ref == 0)
			nvmeibs_simu_subscribe_client(handle, node->hostname, disk, false);		// Otherwise, Still   subscribed to this disk via another segment
	}
}

static void __subscribe_to_vol_disks_of_live_toma(struct clnt_simu *clnt, const struct sb_volume_conf *vol, bool do_subscribe) {
	const struct sb_cluster_conf *cfg = clnt->cfg;
	unsigned ci, ri, si;
	for (ci = 0; ci < vol->num_chunks; ci++) {
		const struct sb_chunk_conf *c = &vol->chunks[ci];
		for (ri = 0; ri < c->n_raids; ri++) {
			const struct sb_praid_conf *r = &c->raids[ri];
			for (si = 0; si < (r->D + r->P); si++) {
				if (sb_cluster_node_is_live_toma(sb_cluster_get_node_idx_from_disk_uuid(cfg, r->segs[si].disk_uuid))) {
					struct sb_disk_conf *disk = &cfg->live->disks[sb_cluster_get_disk_idx_from_disk_uuid_n(cfg, r->segs[si].disk_uuid)];
					__subscribe_to_disk(clnt, disk, do_subscribe);			// Already subscribed to this disk via another segment
				}
			}
		}
	}
}

void clnt_simu_vol_attach(struct sb_cluster_conf *cfg, int node_idx, int vol_idx) {
	struct sb_volume_conf *vol = &cfg->vols[vol_idx];
	struct clnt_simu *clnt = cfg->nodes[node_idx].clnt;
	struct sb_attachment_info *attach_info = &vol->clnts[node_idx];
	N_Tf(__AUTOID__, "Clnt[@INT] attach_@DEV_NAME", clnt->node_idx, vol->name);
	BUG_ON(attach_info->attachment_version != 0);					// Already attached!
	__subscribe_to_vol_disks_of_live_toma(clnt, vol, true);
	// Report to mongo-db directly
	attach_info->attachment_version = 15;							// Todo: need to receive this from mgmt
	attach_info->ioEnabled = false;
}

void clnt_simu_vol_detach(struct sb_cluster_conf *cfg, int node_idx, int vol_idx) {
	struct sb_volume_conf *vol = &cfg->vols[vol_idx];
	struct clnt_simu *clnt = cfg->nodes[node_idx].clnt;
	struct sb_attachment_info *attach_info = &vol->clnts[node_idx];
	N_Tf(__AUTOID__, "Clnt[@INT] detach_@DEV_NAME", clnt->node_idx, vol->name);
	BUG_ON(attach_info->attachment_version == 0);					// Not attached!
	__subscribe_to_vol_disks_of_live_toma(clnt, vol, false);
	memset(attach_info, 0, sizeof(*attach_info));					// Report to mongo-db directly
}

void clnt_simu_vol_unregister(struct sb_cluster_conf *cfg, int node_idx, int vol_idx /*, int praid_idx*/) {
	struct clnt_simu *C = cfg->nodes[node_idx].clnt;
	struct clnt_praid_reg_ctx *reg = &C->regs[vol_idx+1000];	// Todo: properly extract from registered client
	BUG_ON(reg->lock_id == 0); // All the acquired locks are abandoned and become stale
	N_Tf(__AUTOID__, "Clnt=@X, unreg_lock=@LOCKID, leaving @INT stale locks" , cfg->nodes[node_idx].uuid, reg->lock_id, reg->n_ios);
	reg->lock_id = 0;
}

#include "../10_local_hw/nvme_disk_simu.h"
static void __simulate_io_to_disk(struct clnt_simu *C, struct sandbox_nvme_device *disk, u32 dlba_blockset) {
	union nvmeib_lock_blkset_entry *ptr = &((union nvmeib_lock_blkset_entry *)disk->ram.addr)[dlba_blockset];
	struct clnt_praid_reg_ctx *reg = &C->regs[5];	// Todo: properly extract from registered client
	BUG_ON((disk->ram.addr == NULL) || (disk->ram.len <= dlba_blockset));
	ptr->lock_id.all = reg->lock_id;
	ptr->blkset_info.bits.dirty = 0;		// Todo: inject dbits
	ptr->blkset_info.bits.txid++;
	reg->n_ios++;
}

void clnt_simu_vol_lock_blockset_v(struct sb_cluster_conf *cfg, int node_idx, int vol_idx, u32 vlba_blockset) {
	const struct sb_attachment_info *A = &cfg->vols[vol_idx].clnts[node_idx];
	struct clnt_simu *C = cfg->nodes[node_idx].clnt;
	struct sandbox_nvme_device *disk = NULL;	// Todo: calculate from config
	const u32 dlba_blockset = vlba_blockset+5;	// Todo: calculate vlba->dlba from config
	BUG_ON(!A->ioEnabled);
	__simulate_io_to_disk(C, disk, dlba_blockset);
}

void clnt_simu_vol_lock_blockset_d(struct sb_cluster_conf *cfg, int node_idx, int disk_idx, u32 dlba_blockset) {
	struct clnt_simu *C = cfg->nodes[node_idx].clnt;
	struct sandbox_nvme_device *disk = cfg->live->disks[disk_idx].local_nvme;
	__simulate_io_to_disk(C, disk, dlba_blockset);
}
