/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "common/kr_incs.h"
#include "nvmeib_utils.h"
#include "nvmeibc_trace.h"
#include "nvmeibc_cinst.h"
#include "nvmeibc_cinst_max.h"
#include "nvmeib_str.h"
#include "common/proc_epilog.h"
#include "utils/nvmeib_jdr/nvmeib_jdr.h"

static char default_dir_lsblk[CINST_NAME_LEN] = "nvmesh"; // will have no affect if after module init
module_param_string(default_dir_lsblk, default_dir_lsblk, CINST_NAME_LEN, 0644);
MODULE_PARM_DESC(default_dir_lsblk, "Defines the directory within /dev in which block devices will be generated. Can be left empty to use /dev. Default value: nvmesh");

int nvmeibc_cinst_get_main_inst_num(const struct nvmeibc_cinst_params_main *p) { return container_of(p, struct nvmeibc_cinst_params ,main)->index;}
int nvmeibc_cinst_get_core_inst_num(const struct nvmeibc_cinst_params_core *p) { return container_of(p, struct nvmeibc_cinst_params ,core)->index;}
int nvmeibc_cinst_get_blok_inst_num(const struct nvmeibc_cinst_params_blk  *p) { return container_of(p, struct nvmeibc_cinst_params ,blok)->index;}

/******************************* nvmeibc_cinst ********************************/
struct nvmeibc_cinst_priv_ctx {					// Private contexts of each software layer
	void *main;
	void *blok;
	void *core;
};

struct nvmeibc_cinst {
	struct nvmeibc_cinst_params p;
	struct nvmeibc_cinst_priv_ctx ctx;
	ulong creation_jiffies;				// When was this instance created. 0 if not allocated
	struct list_head pool_link;			// Linked to free or allocated pool
};
#define __is_isntance_free(c) ((c)->creation_jiffies == 0)

struct t_nvmeibc_cinst_arr {
	struct mutex lock;					// Accessed from module wq and from /proc/... read functions
	u16 n_alloc;						// Number of instances existing
	u16 n_free;							// For debug, == (NVMEIBC_MAX_CINSTS-n_alloc)
	struct nvmeibc_cinst array[NVMEIBC_MAX_CINSTS];		// Preallocated list of instances. If becomes huge, alloc upon usage
	struct list_head free_list;			// Linked list of instances for future use
	struct list_head aloc_list;			// Linked list of instances already in use
} cinst;

const struct nvmeibc_cinst_params *nvmeibc_cinst_params_get_default(void)
{
	const struct t_nvmeibc_cinst_arr *C = &cinst;
	const struct nvmeibc_cinst_params *rv = NULL;
	if (C->n_alloc < NVMEIBC_MAX_CINSTS)
		rv = &(list_first_entry(&C->free_list, struct nvmeibc_cinst, pool_link)->p);
	return rv;
}

static void nvmeibc_cinst_init(struct nvmeibc_cinst *c, int i)
{
	struct t_nvmeibc_cinst_arr *C = &cinst;
	c->p.index = i;
	c->p.main._private = &c->ctx.main;
	c->p.blok._private = &c->ctx.blok;
	c->p.core._private = &c->ctx.core;
	c->p.main.proc_dir_root_name = c->p.blok.dev_name.str;
	spin_lock_init(&c->p.main.param_change_lock);
	c->p.main.mgmt.use_https = true;								// Default is true
	list_add_tail(&c->pool_link, &C->free_list);			// FIFO
	c->creation_jiffies = 0UL;
	C->n_free++;
}

void nvmeibc_cinst_array_init(void)
{
	struct t_nvmeibc_cinst_arr *C = &cinst;
	int i;
	memset(C, 0, sizeof(*C));
	mutex_init(&C->lock);

	// Generic initialization for all instances: Free list 0,1,2,....
	INIT_LIST_HEAD(&C->free_list);
	INIT_LIST_HEAD(&C->aloc_list);
	for (i = 0; i < NVMEIBC_MAX_CINSTS; i++) {
		nvmeibc_cinst_init(&C->array[i], i);
	}
	if (1) {			// Default initialization for first instance
		struct nvmeibc_cinst *c = &C->array[0];
		strlcpy(c->p.blok.dev_name.str , "nvmeibc", sizeof(c->p.blok.dev_name));
		strlcpy(c->p.blok.dir_lsblk.str, default_dir_lsblk, sizeof(c->p.blok.dir_lsblk));
	}
}

#define INST_LIST_PROC_FRMT_VER 1
void nvmeibc_cinst_array_debug_print(void *ctx, struct jdr *jdr)
{
	struct t_nvmeibc_cinst_arr *C = &cinst;
	const ulong now = jiffies;
	struct nvmeibc_cinst *c;
	(void)ctx;
	mutex_lock(&C->lock);
	jdr_write_var(jdr, n_inst, C->n_alloc);
	jdr_write_var(jdr, n_free, C->n_free);
	{
		jdr_array_scope(jdr, "instances");
		list_for_each_entry(c, &C->aloc_list, pool_link) {
			struct nvmeibc_cinst_params_main *pm = &c->p.main;
			ulong flags;
			jdr_object_scope(jdr, NULL);
			jdr_write_var(jdr, index, c->p.index);
			jdr_write_var(jdr, name, (char const *)c->p.blok.dev_name.str);
			jdr_write_var(jdr, dev_dir, (char const *)c->p.blok.dir_lsblk.str);
			// ---- main params ----
			jdr_write_var(jdr, auto_generated, pm->mgmt.is_auto_generated);
			spin_lock_irqsave(&pm->param_change_lock, flags);
			{
				jdr_object_scope(jdr, "management");
				jdr_write_var(jdr, cluster, pm->mgmt.cluster);
				jdr_write_var(jdr, protocol, (char const *)(((pm->mgmt.use_https) ? "https" : "http")));
				jdr_write_var(jdr, db_uuid, (char const *)&pm->mgmt.db_uuid[0]);
			}
			spin_unlock_irqrestore(&pm->param_change_lock, flags);
			{
				jdr_object_scope(jdr, "cfg_profile");
				jdr_write_var(jdr, id, (const char *)pm->cfg_profile.id);
				jdr_write_var(jdr, name, (char const *)pm->cfg_profile.name);
				jdr_write_var(jdr, ver, (int)pm->cfg_profile.version);
			}
			jdr_write_var(jdr, age_sec, (u64)(now - c->creation_jiffies) / HZ);
		}
	}
	if (0) {						// For debug, print the free list
		jdr_array_scope(jdr, "free");
		list_for_each_entry(c, &C->free_list, pool_link) {
			jdr_object_scope(jdr, NULL);
			jdr_write_var(jdr, index, c->p.index);
		}
	}
	nvmeib_proc_add_json_proc_epilog_jdr(INST_LIST_PROC_FRMT_VER, jdr);
	mutex_unlock(&C->lock);
}

const struct nvmeibc_cinst_params* nvmeibc_cinst_array_get_itr_next(const struct nvmeibc_cinst_params* p)
{	// Implementation: Much like list_for_each_entry_safe_continue() but obscured in a function instead of macro
	const struct t_nvmeibc_cinst_arr *C = &cinst;
	const struct nvmeibc_cinst *c = NULL;
	const struct nvmeibc_cinst_params* rv = NULL;	// Default, iterator end
	if (!p) {
		goto _init_iterator;	// Iterator init by giving NULL
	} else {					// Advance to next element
		c = container_of(p, typeof(*c), p);	// == C->array[p->index]
		if (__is_isntance_free(c))
			goto _init_iterator;			// Iterator init by giving free isntance, thus Support deletion of instances in a loop (using the iterator)
		c = list_next_entry(c, pool_link);
		if (&c->pool_link != &C->aloc_list)	// Not list end
			rv = &c->p;
		goto _out;
	}
_init_iterator:
	if (C->n_alloc)
		rv = &list_first_entry(&C->aloc_list, typeof(*c), pool_link)->p;
_out:
	return rv;
}

const struct nvmeibc_cinst_params* nvmeibc_cinst_array_get_itr_i_unsafe(int i)
{
	const struct t_nvmeibc_cinst_arr *C = &cinst;
	return &C->array[i].p;
}

void nvmeibc_cinst_array_destroy(void)
{
	const struct t_nvmeibc_cinst_arr *C = &cinst;
	WARN(C->n_alloc != 0, "nvmeibc bug %d instances in use", C->n_alloc);
}

//int nvmeibc_cinst_array_get_num_instances(void)
//{
	//nvmeibc_assert_on_module_wq(); Todo insert this
	//return C->n_alloc;
//}

/******************************* nvmeibc_cinst ********************************/
void *nvmeibc_cinst_array_add(const struct nvmeibc_cinst_params *p)
{
	struct t_nvmeibc_cinst_arr *C = &cinst;
	struct nvmeibc_cinst *rv = NULL;
	mutex_lock(&C->lock);
	if (C->n_alloc < NVMEIBC_MAX_CINSTS) {
		rv = list_first_entry(&C->free_list, typeof(*rv), pool_link);	// == C->array[p->index]
		WARN_ON(&rv->p != p); //rv->p = *p;
		C->n_alloc++;
		C->n_free--;
		list_move_tail(&rv->pool_link, &C->aloc_list);			// FIFO
		rv->creation_jiffies = jiffies;
	} else {
		_NT(t_01_cinst, "Nvmeibc: Maximal amount of isntances reached");
	}
	mutex_unlock(&C->lock);
	return rv;
}

void nvmeibc_cinst_array_del(const struct nvmeibc_cinst_params *p)
{
	struct t_nvmeibc_cinst_arr *C = &cinst;
	struct nvmeibc_cinst *c = container_of(p, typeof(*c), p);	// == C->array[p->index]
	mutex_lock(&C->lock);
	WARN_ON((__is_isntance_free(c))||(C->n_alloc == 0)); 	// Corruption! Should be allocated
	list_del_init(&c->pool_link);
	C->n_alloc--;
	nvmeibc_cinst_init(c, p->index);
	mutex_unlock(&C->lock);
}

const struct nvmeibc_cinst_params* nvmeibc_cinst_get_by_name(const char *name)
{
	struct t_nvmeibc_cinst_arr *C = &cinst;
	struct nvmeibc_cinst *c;
	struct nvmeibc_cinst_params *rv = NULL;
	mutex_lock(&C->lock);
	list_for_each_entry(c, &C->aloc_list, pool_link) {
		if (!strcmp(c->p.blok.dev_name.str, name)) {
			rv = &c->p;
			goto _out;
		}
	}
_out:
	mutex_unlock(&C->lock);
	return rv;
}
