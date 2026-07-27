#include "nvmeibc_block.h"
#include "nvmeibc_b_cp_cpu_masks.h"

struct nvmeibc_b_cp_cpu_masks *nvmeibc_b_cp_cpu_masks_create(void)
{
	struct nvmeibc_b_cp_cpu_masks *cpu_masks = kzalloc(sizeof(*cpu_masks), GFP_KERNEL);
	if (!cpu_masks) {
		_NE(e_01_nvmeibc_b_cp_cpu_masks_create, DMESG_PREFIX() ": Out of memory");
		return NULL;
	}

	spin_lock_init(&cpu_masks->lock);
	cpu_masks->next_gen = 1;

	cpu_masks->per_mask = kzalloc(sizeof(*cpu_masks->per_mask) * NVMEIB_CPU_MASK_MAX_CPUS, GFP_KERNEL);
	if (!cpu_masks->per_mask) {
		_NE(e_02_nvmeibc_b_cp_cpu_masks_create, DMESG_PREFIX() ": Out of memory");
		kfree(cpu_masks);
		return NULL;
	}

	return cpu_masks;
}

void nvmeibc_b_cp_cpu_masks_destroy(struct nvmeibc_b_cp_cpu_masks *cpu_masks)
{
	if (cpu_masks) {
		kfree(cpu_masks->per_mask);
		kfree(cpu_masks);
	}
}

static int __cpu_mask_get_index(const struct nvmeib_cpu_mask *mask)
{
	return find_first_bit(mask->cpus, NVMEIB_CPU_MASK_MAX_CPUS);
}

static void __del_mask_ref(struct nvmeibc_b_cp_cpu_masks *cpu_masks, struct nvmeibc_b_cp_volume_cpu_masks *volume_cpu_masks, int mask_i, const struct nvmeib_cpu_mask *mask)
{
	int i;

	BUG_ON(!test_bit(mask_i, volume_cpu_masks->masks_ids));
	clear_bit(mask_i, volume_cpu_masks->masks_ids);

	if (!(--cpu_masks->per_mask[mask_i].n_refs)) {
		NVMEIB_CPU_MASK_FOR_EACH_CPU(i, *mask) {
			NVMEIB_CPU_MASK_CLEAR(cpu_masks->per_mask[i].mask_info.mask);
		}
		cpu_masks->per_mask[mask_i].mask_info.gen = 0;
	}
}

static void __add_mask_ref(struct nvmeibc_b_cp_cpu_masks *cpu_masks, struct nvmeibc_b_cp_volume_cpu_masks *volume_cpu_masks, int mask_i, const struct nvmeib_cpu_mask *mask, u64 *gen)
{
	int i;

	BUG_ON(test_bit(mask_i, volume_cpu_masks->masks_ids));
	set_bit(mask_i, volume_cpu_masks->masks_ids);

	if (!(cpu_masks->per_mask[mask_i].n_refs++)) {
		NVMEIB_CPU_MASK_FOR_EACH_CPU(i, *mask) {
			cpu_masks->per_mask[i].mask_info.mask = *mask;
		}
		cpu_masks->per_mask[mask_i].mask_info.gen = cpu_masks->next_gen++;
	}

	*gen = cpu_masks->per_mask[mask_i].mask_info.gen;
}

int nvmeibc_b_cp_cpu_masks_add_volume_mask(struct nvmeibc_b_cp_cpu_masks *cpu_masks, struct nvmeibc_b_cp_volume_cpu_masks *volume_cpu_masks, const struct nvmeib_cpu_mask *mask, u64 *gen)
{
	int mask_id, i;
	struct nvmeib_cpu_mask_info_and_refs *m;
	unsigned long flags;

	if (NVMEIB_CPU_MASK_IS_EMPTY(*mask))
		return -EINVAL;

	mask_id = __cpu_mask_get_index(mask);
	m = &cpu_masks->per_mask[mask_id];

	if (!NVMEIB_CPU_MASK_IS_EMPTY(m->mask_info.mask)) {
		if (!NVMEIB_CPU_MASK_EQ(*mask, m->mask_info.mask))	// Partial overlap
			return -EEXIST;

		if (test_bit(mask_id, volume_cpu_masks->masks_ids))	// Already added to volume
			return -EEXIST;
	} else {
		NVMEIB_CPU_MASK_FOR_EACH_CPU(i, *mask) {
			if (i == mask_id)
				continue;

			if (!NVMEIB_CPU_MASK_IS_EMPTY(cpu_masks->per_mask[i].mask_info.mask))	// Partial overlap
				return -EEXIST;
		}

		BUG_ON(test_bit(mask_id, volume_cpu_masks->masks_ids));
		BUG_ON(cpu_masks->per_mask[mask_id].n_refs);

	}

	spin_lock_irqsave(&cpu_masks->lock, flags);
	__add_mask_ref(cpu_masks, volume_cpu_masks, mask_id, mask, gen);
	spin_unlock_irqrestore(&cpu_masks->lock, flags);

	return 0;
}

int nvmeibc_b_cp_cpu_masks_del_volume_mask(struct nvmeibc_b_cp_cpu_masks *cpu_masks, struct nvmeibc_b_cp_volume_cpu_masks *volume_cpu_masks, const struct nvmeib_cpu_mask *mask)
{
	int mask_id;
	unsigned long flags;

	if (NVMEIB_CPU_MASK_IS_EMPTY(*mask))
		return -EINVAL;

	mask_id = __cpu_mask_get_index(mask);

	if (NVMEIB_CPU_MASK_IS_EMPTY(cpu_masks->per_mask[mask_id].mask_info.mask))
		return -ENOENT;

	if (!NVMEIB_CPU_MASK_EQ(*mask, cpu_masks->per_mask[mask_id].mask_info.mask))	// Partial overlap, not the same mask
		return -ENOENT;

	if (!test_bit(mask_id, volume_cpu_masks->masks_ids))	// Mask was not added to the volume
		return -ENOENT;

	BUG_ON(!cpu_masks->per_mask[mask_id].n_refs);

	spin_lock_irqsave(&cpu_masks->lock, flags);
	__del_mask_ref(cpu_masks, volume_cpu_masks, mask_id, mask);
	spin_unlock_irqrestore(&cpu_masks->lock, flags);

	return 0;
}

void nvmeibc_b_cp_cpu_masks_del_all_volume_masks(struct nvmeibc_b_cp_cpu_masks *cpu_masks, struct nvmeibc_b_cp_volume_cpu_masks *volume_cpu_masks)
{
	int i;
	unsigned long flags;

	spin_lock_irqsave(&cpu_masks->lock, flags);

	for_each_set_bit(i, volume_cpu_masks->masks_ids, NVMEIB_CPU_MASK_MAX_MASKS) {
		const struct nvmeib_cpu_mask mask = cpu_masks->per_mask[i].mask_info.mask;
		__del_mask_ref(cpu_masks, volume_cpu_masks, i, &mask);
	}

	bitmap_zero(volume_cpu_masks->masks_ids, NVMEIB_CPU_MASK_MAX_MASKS);

	spin_unlock_irqrestore(&cpu_masks->lock, flags);
}

int nvmeibc_b_cp_cpu_masks_get_all_for_volume(struct nvmeibc_b_cp_cpu_masks *cpu_masks, const struct nvmeibc_b_cp_volume_cpu_masks *volume_cpu_masks, struct nvmeib_cpu_mask_info *mask_infos, int max_masks)
{
	int n_masks = 0;
	int i;
	unsigned long flags;

	spin_lock_irqsave(&cpu_masks->lock, flags);

	for_each_set_bit(i, volume_cpu_masks->masks_ids, NVMEIB_CPU_MASK_MAX_MASKS) {
		const struct nvmeib_cpu_mask_info_and_refs *m = &cpu_masks->per_mask[i];

		if (NVMEIB_CPU_MASK_IS_EMPTY(m->mask_info.mask))
			continue;

		if (n_masks == max_masks) {
			spin_unlock_irqrestore(&cpu_masks->lock, flags);
			return -ENOMEM;
		}

		mask_infos[n_masks++] = m->mask_info;
	}

	spin_unlock_irqrestore(&cpu_masks->lock, flags);

	return n_masks;
}

ssize_t nvmeibc_b_cp_cpu_masks_volume_masks_to_json(struct nvmeibc_b_cp_cpu_masks *cpu_masks, const struct nvmeibc_b_cp_volume_cpu_masks *volume_cpu_masks, char *buffer, size_t len)
{
	int count = 0;
	bool first = true;
	int i;
	unsigned long flags;

	spin_lock_irqsave(&cpu_masks->lock, flags);

	count += scnprintf(buffer + count, len - count, "[\n");
	for_each_set_bit(i, volume_cpu_masks->masks_ids, NVMEIB_CPU_MASK_MAX_MASKS) {
		const struct nvmeib_cpu_mask_info *mask_info = &cpu_masks->per_mask[i].mask_info;

		if (!first)
			count += scnprintf(buffer + count, len - count, ",");
		first = false;

		count += scnprintf(buffer + count, len - count,	"{\"mask\": \"%*pb\", \"gen\": %llu}\n", NVMEIB_CPU_MASK_MAX_CPUS, mask_info->mask.cpus, mask_info->gen);
	}
	count += scnprintf(buffer + count, len - count, "]\n");

	spin_unlock_irqrestore(&cpu_masks->lock, flags);

	return count;
}

ssize_t nvmeibc_b_cp_cpu_masks_to_json(struct nvmeibc_b_cp_cpu_masks *cpu_masks, char *buffer, size_t len)
{
	int count = 0;
	bool first = true;
	int i;
	unsigned long flags;

	spin_lock_irqsave(&cpu_masks->lock, flags);

	count += scnprintf(buffer + count, len - count, "[\n");
	for (i = 0; i < NVMEIB_CPU_MASK_MAX_MASKS; i++) {
		const struct nvmeib_cpu_mask_info_and_refs *m =	&cpu_masks->per_mask[i];

		if (NVMEIB_CPU_MASK_IS_EMPTY(m->mask_info.mask) || (__cpu_mask_get_index(&m->mask_info.mask) != i))
			continue;

		if (!first)
			count += scnprintf(buffer + count, len - count, ",");
		first = false;

		count += scnprintf(buffer + count, len - count,	"{\"mask\":\"%*pb\", \"gen\": %llu, \"volume_count\": %d}\n", NVMEIB_CPU_MASK_MAX_CPUS, m->mask_info.mask.cpus, m->mask_info.gen, m->n_refs);
	}
	count += scnprintf(buffer + count, len - count, "]\n");

	spin_unlock_irqrestore(&cpu_masks->lock, flags);

	return count;
}
