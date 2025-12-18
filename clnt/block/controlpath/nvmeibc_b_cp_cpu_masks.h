#ifndef NVMEIBC_B_CP_CPU_MASKS_H_
#define NVMEIBC_B_CP_CPU_MASKS_H_


#include "nvmeib_cpu_masks.h"
#include "kr_incs.h"

struct jdr;

// The max number of possible non-overlapping masks in a set is the number of CPUs
#define NVMEIB_CPU_MASK_MAX_MASKS NVMEIB_CPU_MASK_MAX_CPUS

/*
See: Disjoint-set data structure - Wikipedia

b - block
cp - control path

struct nvmeibc_b_cp_cpu_masks represents a collection of disjoint CPU masks.
For every mask, we can select a representative, which will identify the whole mask. For this purpose, we use the CPU, with the minimal.

And now the recursion: how can we represent a set of representatives? Using the bitmap, where the representative bit is turned on.
struct nvmeibc_b_cp_volume_cpu_masks - implemented a "representatives set".
*/

// A set of non-empty, non-mutually-overlapping masks
struct nvmeibc_b_cp_cpu_masks {
	spinlock_t lock;	// Protects actual modification, both global and per-volume (main wq only) vs get all masks API (any context)
	struct nvmeib_cpu_mask_info_and_refs {
		struct nvmeib_cpu_mask_info mask_info;
		int n_refs;
	} *per_mask;	// An array where a mask m is stored at index i iff i is the 1st 1-bit in m, otherwise an empty mask. Augmented with refcounts by volumes.
					// In addition, to detect collisions, a non-augmented masks are also stored at all other indexes of the masks's 1-bits.
	u64 next_gen;
};

struct nvmeibc_b_cp_cpu_masks *nvmeibc_b_cp_cpu_masks_create(void);
void nvmeibc_b_cp_cpu_masks_destroy(struct nvmeibc_b_cp_cpu_masks *);

struct nvmeibc_b_cp_volume_cpu_masks {
	DECLARE_BITMAP(masks_ids, NVMEIB_CPU_MASK_MAX_MASKS);	// A bit i is 1 iff the volume has the mask in which the 1st 1-bit is i (index in the masks array in global nvmeib_b_cp_cpu_masks)
};

// Add/del: caller must be on main wq
int nvmeibc_b_cp_cpu_masks_add_volume_mask(struct nvmeibc_b_cp_cpu_masks *cpu_masks, struct nvmeibc_b_cp_volume_cpu_masks *volume_cpu_masks, const struct nvmeib_cpu_mask *mask, u64 *gen);
int nvmeibc_b_cp_cpu_masks_del_volume_mask(struct nvmeibc_b_cp_cpu_masks *cpu_masks, struct nvmeibc_b_cp_volume_cpu_masks *volume_cpu_masks, const struct nvmeib_cpu_mask *mask);
void nvmeibc_b_cp_cpu_masks_del_all_volume_masks(struct nvmeibc_b_cp_cpu_masks *cpu_masks, struct nvmeibc_b_cp_volume_cpu_masks *volume_cpu_masks);

int nvmeibc_b_cp_cpu_masks_get_all_for_volume(struct nvmeibc_b_cp_cpu_masks *cpu_masks, const struct nvmeibc_b_cp_volume_cpu_masks *volume_cpu_masks, struct nvmeib_cpu_mask_info *mask_infos, int max_masks);

void nvmeibc_b_cp_cpu_masks_tojson(struct nvmeibc_b_cp_cpu_masks *cpu_masks, struct jdr *jdr);
void nvmeibc_b_cp_cpu_masks_volume_masks_tojson(struct nvmeibc_b_cp_cpu_masks *cpu_masks, const struct nvmeibc_b_cp_volume_cpu_masks *volume_cpu_masks, struct jdr *jdr);

#endif /* NVMEIBC_B_CP_CPU_MASKS_H_ */
