#include "mongodb_simu.h"
#include "nvmeibt_debug.h"	// Binary tracing

/* UUID constants, All uuids are generated as 32bits integers */
#define DISK_UUID_LOCAL_002      0xf37000d0			// Encodes 3 nibbles node and 'd' for disk: easier eye catcher debugging
#define DISK_UUID_LOCAL_003      0xf37000d1
#define DISK_UUID_REMOTE38_D0    0xf38000d0
#define DISK_UUID_REMOTE38_D1    0xf38000d1
#define DISK_UUID_REMOTE39_D0    0xf39000d0
#define DISK_UUID_REMOTE39_D1    0xf39000d1
#define NODE_UUID_BASE           0xf37000c0			// First 3 nibbles = node, 'c' for 'computer', last nibble = index of node in cluster for faster search
#define NIC__UUID_BASE           0xf37000e0			// First 3 nibbles = node, 'e' for 'ethernet', last nibble = index of nic  in this node
#define VOL__UUID_BASE           0xbd000000			// Vol (Block device) Has first 4 nibbles as bdXX where XX is volume index (up to 256 vols), Last 4 nibbles are 0CRS, where C,R,S are chunk, raid and seg indices respectively. Counting starts from 1.

void sb_cluster_conf_create( struct sb_cluster_conf *sb) {
	int i, j;
	gethostname(sb->my_hostname, sizeof(sb->my_hostname) - 1);
	sb->n_nodes = (int)ARRAY_SIZE(sb->nodes);
	sb->live =  &sb->nodes[0];
	sb->other = &sb->nodes[1];
	   sb->live->hostname = sb->my_hostname;
	sb->other[0].hostname = "n38@nvidia.com";
	sb->other[1].hostname = "n39@nvidia.com";
	for (i = 0; i < sb->n_nodes; i++) {
		struct sb_node_conf *node = &sb->nodes[i];
		node->uuid = NODE_UUID_BASE + (i << 20) + i;
		for (j = 0; j < (int)ARRAY_SIZE(node->nics); j++)
			node->nics[j].uuid =  (node->uuid & 0xFFFF0000) | ((NIC__UUID_BASE & 0xFFFF) + j);
		for (j = 0; j < (int)ARRAY_SIZE(node->disks); j++)
			node->disks[j].uuid = (node->uuid & 0xFFFF0000) | ((DISK_UUID_LOCAL_002 & 0xFFFF) + j);
	}
			BUG_ON(sb->nodes[0].disks[0].uuid != DISK_UUID_LOCAL_002);
			BUG_ON(sb->nodes[0].disks[1].uuid != DISK_UUID_LOCAL_003);
			BUG_ON(sb->nodes[1].disks[0].uuid != DISK_UUID_REMOTE38_D0);
			BUG_ON(sb->nodes[1].disks[1].uuid != DISK_UUID_REMOTE38_D1);
			BUG_ON(sb->nodes[2].disks[0].uuid != DISK_UUID_REMOTE39_D0);
			BUG_ON(sb->nodes[2].disks[1].uuid != DISK_UUID_REMOTE39_D1);

	{	// Create 2 volumes:		All uuids are generated as 32bits integers 0xaaaV0CRS, where V is volume index, C,R,S are chunk, raid and seg indices respectively. Counting starts from 1.
		unsigned c, r, s, disk_seg_n_blocks = 1024;		// 4[mb] disk segments
		struct sb_seg_conf *ps;
		{	// Allocate areas on disks, Todo: Here use counter on each disk to auto allocate next segment (instead of manual calculation), when we will add/remove volumes dynamically.
			ps = &sb->vols[0].chunks[0].raids[0].segs[0];
			ps[0].disk_uuid = sb->nodes[1].disks[0].uuid;		ps[0].block_start = 0;		ps->block_end = ps->block_start + disk_seg_n_blocks - 1;
			ps[1].disk_uuid = sb->nodes[2].disks[0].uuid;		ps[1].block_start = 0;		ps->block_end = ps->block_start + disk_seg_n_blocks - 1;

			ps = &sb->vols[1].chunks[0].raids[0].segs[0];
			ps[0].disk_uuid = sb->nodes[0].disks[1].uuid;		ps[0].block_start = 6176;	ps->block_end = ps->block_start + disk_seg_n_blocks - 1;
			ps[1].disk_uuid = sb->nodes[1].disks[0].uuid;		ps[1].block_start = 1024;	ps->block_end = ps->block_start + disk_seg_n_blocks - 1;
			ps[2].disk_uuid = sb->nodes[1].disks[1].uuid;		ps[2].block_start = 0;		ps->block_end = ps->block_start + disk_seg_n_blocks - 1;
		}
		sb->n_vols = 2;
		sb->vols[0].name = "V_REMOTE1";								// RAID-1, segments only on remote disks (D0_n38, D0_n39)
		sb->vols[1].name = "V_R1";									// RAID-1, one local segment + 2 remote on n38
		for (i = 0; i < sb->n_vols; i++) {
			struct sb_volume_conf *pv = &sb->vols[i];
			pv->uuid = (VOL__UUID_BASE | ((i+1) << 16));
			pv->num_chunks = 1;
			for (c = 0; c < pv->num_chunks; c++) {
				struct sb_chunk_conf *pc = &pv->chunks[c];
				pc->n_raids = 1;									// Raid-0, not supported yet. Striping of 1
				pc->uuid = (pv->uuid | ((c+1) << 8));
				for (r = 0; r < pc->n_raids; r++) {
					struct sb_praid_conf *pr = &pc->raids[r];
					pr->uuid = (pc->uuid | ((r+1) << 4));
					pr->D = 1;
					pr->P = 1 + i;									// First volume is Remote R1-2mirror (1+1), Second R1-3mirror (1+2)
					for (s = 0; s < (pr->D + pr->P); s++) {
						ps = &pr->segs[s];
						ps->uuid = (pr->uuid | (s+1));
						ps->block_end = ps->block_start + (disk_seg_n_blocks - 1);	// Assuming 1 chunk here, Non EC
					}
				}
				pc->vlba_start = (c == 0) ? 0 : (pc[-1].vlba_end + 1);
				pc->vlba_end = pc->vlba_start + (pc->n_raids * pc->raids[0].D * disk_seg_n_blocks) - 1;
			}
			pv->num_blocks = pv->chunks[pv->num_chunks-1].vlba_end + 1;
		}
	}
}

/*static struct sb_seg_conf * __get_first_seg_by_uuid(unsigned uuid_u32) {			// The above uuid design was for easy retrieval of object by uuid.
	return &g_mgmt_sim->cfg->vols[((uuid_u32>>16)&0xF)-1].chunks[((uuid_u32>>8)&0xF)-1].raids[((uuid_u32>>4)&0xF)-1].segs[((uuid_u32)&0xF)-1];
}*/

int sb_cluster_conf_find_node_idx_by_name(const struct sb_cluster_conf *sb, const char *host_name) {
	for (int i = 0; i < sb->n_nodes; i++) {
		if (!strcmp(sb->nodes[i].hostname, host_name))
			return i;
	}
	BUG_ON(true); return -1;
}

void sb_cluster_conf_destroy(struct sb_cluster_conf *sb) {
	(void)sb;
}
