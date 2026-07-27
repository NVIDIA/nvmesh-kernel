#pragma push_macro("__FILE_LITERAL__")
#undef __FILE_LITERAL__
#define __FILE_LITERAL__ nvmeibc_block_api_os_sub_vols_common_inc_c

static int __exec_for_each_sub_vol(struct nvmeiba_atom_os_api *car,
						int (*fn)(struct nvmeiba_atom_os_api *sub))
{
	int rv = 0;
	__verify_on_main_wq_atom(car);				// Must be on main work queue to serialize with sub volume add/del, Alternatively must acquire sub.list_lock_unused
	if (unlikely(car->sub.flags.is_sub_atom)) {
		// Sub vols cannot have sub-sub-vols
	} else if (!list_empty(&car->sub.part_list)) {
		struct nvmeiba_part *part, *tmp;
		list_for_each_entry_safe(part, tmp, &car->sub.part_list, part_list) {		// Safe - because the fn() can remove sub volume from the list
			// LKJ: take spinlock of sub-volume here
			struct nvmeiba_atom_os_api *sub = container_of(part, struct nvmeiba_atom_os_api, sub);
			rv |= fn(sub);
		}
	}
	return rv;
}

static int __exec_for_carrier_and_sub_vols(struct nvmeiba_atom_os_api *car,
						int (*fn)(struct nvmeiba_atom_os_api *sub))
{
	int rv = fn(car);
	rv |= __exec_for_each_sub_vol(car, fn);
	return rv;
}


#pragma pop_macro("__FILE_LITERAL__")
