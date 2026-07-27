#ifndef NVMEIBC_BLOCK_DP_CPU_MASKS_H_
#define NVMEIBC_BLOCK_DP_CPU_MASKS_H_

#include "nvmeib_cpu_masks.h"
#include "kr_incs.h"

struct nvmeibc_b_dp_cpu_masks {
	struct nvmeib_cpu_mask_info __percpu *percpu; // For each CPU, the mask (by value) that includes that CPU and the mask's ID
};

int nvmeibc_b_dp_cpu_masks_init(struct nvmeibc_b_dp_cpu_masks *);
void nvmeibc_b_dp_cpu_masks_fini(struct nvmeibc_b_dp_cpu_masks *);

void nvmeibc_b_dp_cpu_masks_add(struct nvmeibc_b_dp_cpu_masks *, struct nvmeib_cpu_mask *, u64 gen);
void nvmeibc_b_dp_cpu_masks_del(struct nvmeibc_b_dp_cpu_masks *, struct nvmeib_cpu_mask *);

void nvmeibc_b_dp_cpu_masks_get_on_cpu(const struct nvmeibc_b_dp_cpu_masks *, struct nvmeib_cpu_mask_info *);

#endif /* NVMEIBC_BLOCK_DP_CPU_MASKS_H_ */
