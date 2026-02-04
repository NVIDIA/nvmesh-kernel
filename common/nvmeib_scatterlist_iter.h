#ifndef NVMEIB_SCATTERLIST_ITER_H
#define NVMEIB_SCATTERLIST_ITER_H
#include "common/compat/kr_incs_sgl.h"
struct nvmeib_scatterlist_block_iter {
    u8 *data;
    int idx;
    struct {
        u32 block_size;
        u32 ent_idx;
        u32 max_ents;
        u8* max_ent_data;
        u8* curr_ent_data;
        int curr_buffer_idx;
        struct scatterlist *sg;
    } _impl;
};

static inline bool __is_power_of_two(unsigned long n) {
    return n && !(n & (n - 1));
}

static inline void __nvmeib_scatterlist_iter_validate_sg(u32 block_size, struct scatterlist *sglist, u32 num_sgs) {
    struct scatterlist *sg;
    u32 __i __attribute__((unused));
    for_each_sg(sglist, sg, num_sgs, __i) {
        BUG_ON((sg->length & (block_size - 1)) != 0);
    }
}

static inline void nvmeib_scatterlist_block_iter_init(struct nvmeib_scatterlist_block_iter *iter, u32 block_size, struct scatterlist *sg, u32 n_ents) {
    BUG_ON(n_ents == 0);

    BUG_ON(!__is_power_of_two(block_size));
    iter->_impl.block_size = block_size;

    __nvmeib_scatterlist_iter_validate_sg(block_size, sg, n_ents);
    iter->_impl.sg = sg;
    iter->_impl.curr_ent_data = (u8 *)sg_virt(iter->_impl.sg);
    iter->_impl.curr_buffer_idx = 0;

    iter->_impl.max_ents = n_ents;
    iter->_impl.ent_idx = 0;
    iter->_impl.max_ent_data = iter->_impl.curr_ent_data + iter->_impl.sg->length;
}

static inline bool __sg_blk_current_sg_empty(struct nvmeib_scatterlist_block_iter *iter) {
    return iter->_impl.curr_ent_data == iter->_impl.max_ent_data;
}

static inline bool nvmeib_scatterlist_block_iter_next(struct nvmeib_scatterlist_block_iter *iter) {
    while (__sg_blk_current_sg_empty(iter)) {
        iter->_impl.ent_idx++;
        if (iter->_impl.ent_idx == iter->_impl.max_ents) {
            return false;
        }
        iter->_impl.sg = sg_next(iter->_impl.sg);
        iter->_impl.curr_ent_data = (u8 *)sg_virt(iter->_impl.sg);
        iter->_impl.max_ent_data = iter->_impl.curr_ent_data + iter->_impl.sg->length;
    }
    iter->data = iter->_impl.curr_ent_data;
    iter->_impl.curr_ent_data += iter->_impl.block_size;

    iter->idx = iter->_impl.curr_buffer_idx;
    iter->_impl.curr_buffer_idx++;
    return true;
}

#endif // NVMEIB_SCATTERLIST_ITER_H
