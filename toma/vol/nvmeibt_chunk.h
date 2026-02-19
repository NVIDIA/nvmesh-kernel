#ifndef NVMEIBT_CHUNK
#define NVMEIBT_CHUNK

#include "nvmeibt_common.h"
#include "nvmeibt_params.h"
#include "nvmeibt_ds.h"
#include "nvmeibt_block_device.h"

struct nvmeibt_chunk_config {
	union nvmeib_uuid id;
	int				  version;
	union nvmeib_uuid its_block_device_id;
	long long		  vlb_s;
	long long		  vlb_e;
	int				  stripe_width;	   // n_logical segs in each praid (n_segs - redundancy)
	int				  stripe_size;	  // n_praids
};

struct nvmeibt_praid;

struct nvmeibt_chunk {
	struct nvmeibt_chunk_config	 from_config;
	struct nvmeibt_urn_uuid		 urn_uuid;
	struct mm_chunk_conf		 its_mm_chunk_conf;
	struct nvmeibt_block_device *its_block_device;
	int							 its_idx_in_block_device;
	int							 n_praids;
	struct nvmeibt_praid		*praids[NVMEIBT_MAX_STRIPE_WIDTH_PER_CHUNK];
	uint8_t						 trim_flags;
	struct xdlist				 topo_link;
	int							 config_tag;
};

static inline struct nvmeibt_block_device *nvmeibt_chunk_get_blkdev(const struct nvmeibt_chunk *c)
{
	return (c ? c->its_block_device : NULL);
}

static inline bool nvmeibt_chunk_is_being_deleted(const struct nvmeibt_chunk *c)
{
	return (!c || nvmeibt_blkdev_is_being_deleted(c->its_block_device));
}

bool					 nvmeibt_chunk_is_deprecated_in_config(struct nvmeibt_chunk *c);
const union nvmeib_uuid *nvmeibt_chunk_UUID(struct nvmeibt_chunk *c);

static inline const char *nvmeibt_chunk_id_str(const struct nvmeibt_chunk *c)
{
	return (c ? c->urn_uuid.str : "");
}

static inline const char *nvmeibt_chunk_get_blkdev_name(const struct nvmeibt_chunk *c)
{
	return (c ? nvmeibt_blkdev_name(c->its_block_device) : "???");
}
enum nvmeibt_add_rv nvmeibt_chunk_add(struct mm_chunk_conf *conf, struct nvmeibt_block_device *blkdev, int idx_in_vol, int config_tag, struct nvmeibt_chunk **output_chunk);
int					nvmeibt_chunk_remove(struct nvmeibt_chunk *c);
void 				nvmeibt_chunk_free_all_at_exit(void);
void				nvmeibt_chunk_trim_specific_chunk(struct nvmeibt_chunk *c, uint8_t trim_flag);
void				nvmeibt_chunk_trim_unused_entries(int config_tag, uint8_t trim_flag);

#endif	  // #ifndef NVMEIBT_CHUNK
