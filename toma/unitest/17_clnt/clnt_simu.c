#include "clnt_simu.h"
#include "nvmeibt_debug.h"	// Binary tracing
#include "../13_mgmt/mongodb_simu.h"
#include "common/nvmeib_shared.h"

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

void clnt_simu_vol_attach(struct sb_cluster_conf *cfg, int node_idx, int vol_idx) {
	cfg->vols[vol_idx].clnts[node_idx].attachment_version = 15;		// Todo...

}

void clnt_simu_vol_detach(struct sb_cluster_conf *cfg, int node_idx, int vol_idx) {
	cfg->vols[vol_idx].clnts[node_idx].attachment_version = 0;		// Todo...
}

void clnt_simu_vol_unregister(struct sb_cluster_conf *cfg, int node_idx, int vol_idx /*, int praid_idx*/) {
	struct clnt_simu *C = cfg->nodes[node_idx].clnt;
	struct clnt_praid_reg_ctx *reg = &C->regs[vol_idx+1000];	// Todo: properly extract from registered client
	BUG_ON(reg->lock_id == 0); // All the acquired locks are abandoned and become stale
	N_Tf(__AUTOID__, "Clnt=@X, unreg_lock=@LOCKID, leaving @INT stale locks" , cfg->nodes[node_idx].uuid, reg->lock_id, reg->n_ios);
	reg->lock_id = 0;
}

#include "../sandbox_nvme.h"
static void __simulate_io_to_disk(struct clnt_simu *C, struct sandbox_nvme_device *disk, u32 dlba_blockset) {
	union nvmeib_lock_blkset_entry *ptr = &((union nvmeib_lock_blkset_entry *)disk->ram.addr)[dlba_blockset];
	struct clnt_praid_reg_ctx *reg = &C->regs[1000];	// Todo: properly extract from registered client
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
