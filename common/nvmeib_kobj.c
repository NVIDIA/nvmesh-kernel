#include "nvmeib_kobj.h"
#include "nvmeib_utils.h"
#include "nvmeibm_trace.h"

struct kobj_file {
	char name[NAME_MAX + 1];
	ssize_t (*show)(void *priv, char *buf);
	ssize_t (*store)(void *priv, const char *buf, size_t count);
};

struct nvmeib_kobj {
	struct kobject kobj;
	bool kobj_initialize;
	atomic_t kobj_refs;
	void *priv;
	struct kobj_file *files;
	int n_files;
	struct kobj_attribute *kobj_attributes; 
	struct attribute **attributes;
	struct attribute_group attr_group;
};

static void kobj_release(struct kobject *kobj)
{
}

static ssize_t show(struct kobject *kobj, struct kobj_attribute *attr,
	char *buf)
{
	struct nvmeib_kobj *obj = container_of(kobj, struct nvmeib_kobj, kobj);
	int i;
	ssize_t count = 0;
	
	NFIN;
	atomic_inc(&obj->kobj_refs);
	for (i = 0; i < obj->n_files; ++i)
		if (strcmp(attr->attr.name, obj->files[i].name) == 0) {
			count = obj->files[i].show(obj->priv, buf);
			break;
		}
	atomic_dec(&obj->kobj_refs);
	NFOUT;
	return count;
}

static ssize_t store(struct kobject *kobj, struct kobj_attribute *attr,
	const char *buf, size_t count)
{
	struct nvmeib_kobj *obj = container_of(kobj, struct nvmeib_kobj, kobj);
	int i;
	ssize_t n = 0;

	atomic_inc(&obj->kobj_refs);
	for (i = 0; i < obj->n_files; ++i)
		if (strcmp(attr->attr.name, obj->files[i].name) == 0) {
			n = obj->files[i].store(obj->priv, buf, count);
			break;
		}
	atomic_dec(&obj->kobj_refs);
	NFOUT;
	return n;
}

static ssize_t attr_show(struct kobject *kobj, struct attribute *attr,
	char *buf)
{
	struct kobj_attribute *kattr;
	ssize_t ret = -EIO;

	NFIN;
	kattr = container_of(attr, struct kobj_attribute, attr);
	if (kattr->show)
		ret = kattr->show(kobj, kattr, buf);
	NFOUT;
	return ret;
}

static ssize_t attr_store(struct kobject *kobj, struct attribute *attr,
	const char *buf, size_t count)
{
	struct kobj_attribute *kattr;
	ssize_t ret = -EIO;

	NFIN;
	kattr = container_of(attr, struct kobj_attribute, attr);
	if (kattr->store)
		ret = kattr->store(kobj, kattr, buf, count);
	NFOUT;
	return ret;
}

const struct sysfs_ops sysfs_ops = {
	.show = attr_show,
	.store = attr_store,
};

static struct kobj_type kobj_ktype = {
	.release	= kobj_release,
	.sysfs_ops	= &sysfs_ops,
};

struct nvmeib_kobj *nvmeib_kobj_create(struct nvmeib_kobj_params *params)
{
	struct nvmeib_kobj *kobj = NULL;
	int i;
	int rv = 0;

	NFIN;
	if (!(kobj = kzalloc(sizeof(*kobj), GFP_KERNEL))) {
		_NE(error_nvmeib_kobj_nvmeib_kobj_create, "OOM: cannot allocate kobj data");
		goto out;
	}
	kobj->files = kzalloc(sizeof(*kobj->files) * params->n_files, GFP_KERNEL);
	kobj->kobj_attributes =
		kzalloc(sizeof(*kobj->kobj_attributes) * params->n_files, GFP_KERNEL);
	kobj->attributes =
		kzalloc(sizeof(*kobj->attributes) * (params->n_files + 1), GFP_KERNEL);
	kobj->attr_group.attrs = kobj->attributes;
	if (!(kobj->files && kobj->kobj_attributes && kobj->attributes)) {
		_NE(error_1_nvmeib_kobj_nvmeib_kobj_create, "OOM: cannot allocate kobj files data");
		goto free_kobj;
	}
	kobj->n_files = params->n_files;
	kobject_init(&kobj->kobj, &kobj_ktype);
	if ((rv = kobject_add(&kobj->kobj, params->parent, "%s",
		params->dir_name)) < 0) {
		_NE(error_2_nvmeib_kobj_nvmeib_kobj_create, "@DIR_NAME kobject_add error: @RV", params->dir_name, rv);
		goto free_kobj;
	}
	for (i = 0; i < params->n_files; ++i) {
		strncpy(kobj->files[i].name, params->files[i].name, NAME_MAX);
		kobj->files[i].show = params->files[i].show;
		kobj->files[i].store = params->files[i].store;		
		kobj->kobj_attributes[i].attr.name = kobj->files[i].name;
		kobj->kobj_attributes[i].attr.mode = 0664;
		kobj->kobj_attributes[i].show = show;
		kobj->kobj_attributes[i].store = store;
		kobj->attributes[i] = &kobj->kobj_attributes[i].attr;
	}
	kobj->attributes[params->n_files] = NULL;
	/* Create the files associated with our kobject */
	if ((rv = sysfs_create_group(&kobj->kobj, &kobj->attr_group)) < 0) {
		_NE(error_3_nvmeib_kobj_nvmeib_kobj_create, "OOPS: fail to create @DIR_NAME sysfs attributes @RV",
			params->dir_name, rv);
		goto free_kobj;
	}
	kobj->priv = params->priv;
	kobj->kobj_initialize = true;
	goto out;

free_kobj:
	nvmeib_kobj_free(kobj);
	kobj = NULL;	

out:
	NFOUT;
	return kobj;
}
EXPORT_SYMBOL(nvmeib_kobj_create);

void nvmeib_kobj_free(struct nvmeib_kobj *kobj)
{
	NFIN;
	if (kobj) {
		if (kobj->kobj_initialize) {
			/* delete the sysfs entries */
			kobject_put(&kobj->kobj);
			kobj->kobj_initialize = false;
			/* wait for current sysfs readers to finish */
			while (atomic_read(&kobj->kobj_refs))
				rep_nop();
		}
		kfree(kobj->files);
		kfree(kobj->kobj_attributes);
		kfree(kobj->attributes);
		kfree(kobj);
	}
	NFOUT;
}
EXPORT_SYMBOL(nvmeib_kobj_free);

