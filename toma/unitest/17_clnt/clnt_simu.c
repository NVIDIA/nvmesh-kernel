#include "clnt_simu.h"
#include "nvmeibt_debug.h"	// Binary tracing
#include "../13_mgmt/mongodb_simu.h"

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
