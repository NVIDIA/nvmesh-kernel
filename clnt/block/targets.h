#ifndef __TARGETS_H__
#define __TARGETS_H__

struct nvmeibc_idisk;

struct dp_target_find_disk_result{
    struct nvmeibc_idisk* disk;
    struct nvmeib_io_stats* v_disk_stats;
};

struct dp_targets{
    struct dp_target_find_disk_result (*find_disk_by_name)(struct dp_targets const* self, const char *diskID);
};

#endif//__TARGETS_H__