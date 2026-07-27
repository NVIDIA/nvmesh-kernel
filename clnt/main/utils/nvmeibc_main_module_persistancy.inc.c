#include "main/utils/nvmeibc_main_module_persistancy.h"

#pragma push_macro("__FILE_LITERAL__")
#undef __FILE_LITERAL__
#define __FILE_LITERAL__ nvmeibc_main_module_persistancy_inc_c

#define per_MAX_ENTRIES			(3)
static int per_n_entries = 0;

struct persistency_entry{			/* Hash table entry*/
	char user_name[64];				/* key. "" for empty entry */
	const void* buf;				/* values */
	int len;
};
static struct persistency_entry persistency[per_MAX_ENTRIES] = {{{0},0,0}};
static DEFINE_SPINLOCK(per_lock);	// Insert new task to the list

/* Find key in the persistency hash table - O(n). Assume, spinlock is held. */
static struct persistency_entry* __per_find_by_key(const char* user_name){
	struct persistency_entry *res = NULL;
	int i;
	for (i=0; i<per_n_entries; i++) {
		if (!strncmp(&persistency[i].user_name[0], user_name, 64)){
			res = &persistency[i];
			break;
		}
	}
	return res;
}

static struct persistency_entry* __per_alloc_new_entry(void)
{
	return (per_n_entries < per_MAX_ENTRIES) ?
				&persistency[per_n_entries++] : NULL;
}

int nvmeibc_volume_persistency_store(const char* user_name, const void*buf,
									  const int len)
{
	unsigned long per_flags;
	struct persistency_entry* e;
	int rv = 0;
	spin_lock_irqsave(&per_lock, per_flags);
	if ((!buf)||(len<=0)) {rv = -EINVAL; goto _out; }
	e = __per_find_by_key(user_name);
	if (!e) e = __per_alloc_new_entry(); /* Not found, by default alloc new */
	if (!e)               {rv = -ENOMEM; goto _out; }
	if (e->buf)
		kfree((void*)e->buf);			/* By default, override previous */
	e->buf = buf;
	e->len = len;
_out:
	spin_unlock_irqrestore(&per_lock, per_flags);
	return rv;
}

int nvmeibc_volume_persistency_del(const char* user_name)
{
	unsigned long per_flags;
	struct persistency_entry* e;
	int rv = 0;
	spin_lock_irqsave(&per_lock, per_flags);
	e = __per_find_by_key(user_name);
	if (!e) {rv = -ENOENT; goto _out; }
	kfree((void*)e->buf);
	per_n_entries--;	 /* Pack the table - copy and remove last entry */
	if ((per_n_entries>0)&&(per_n_entries!=(e-persistency)))
		*e = persistency[per_n_entries];
_out:
	spin_unlock_irqrestore(&per_lock, per_flags);
	return rv;
}

int nvmeibc_volume_persistency_fetch(const char* user_name, const void**buf)
{
	unsigned long per_flags;
	struct persistency_entry* e;
	int rv = 0;
	*buf = NULL;
	spin_lock_irqsave(&per_lock, per_flags);
	if (!buf) {rv = -EINVAL; goto _out; }
	e = __per_find_by_key(user_name);
	if (!e) {rv = -ENOENT; goto _out; }
	*buf = e->buf;
	rv = e->len;
_out:
	spin_unlock_irqrestore(&per_lock, per_flags);
	return rv;
}

#pragma pop_macro("__FILE_LITERAL__")
