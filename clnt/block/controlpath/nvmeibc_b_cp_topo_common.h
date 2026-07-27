#ifndef NVMEIBC_B_CP_TOPO_COMMON_H
#define NVMEIBC_B_CP_TOPO_COMMON_H
/* Internal API of topology.
   Allows various components of to communicate with each other.
   Those functions are obscured from internal API to reduce the clutter of
   functions */


#define __get_num_r1s(t)				((t)->stripe_width)
#define __get_topo_of_seg(seg)			((seg)->chunk->topology)
#define __get_topo_of_r1(r1)			__get_topo_of_seg((r1)->segments)

#define __get_r1_by_t( t, c,r)		(&(t)->chunks[c].raid1s[r])
#define __get_seg_by_t(t, c,r,s)	(&(__get_r1_by_t(t,c,r)->segments[s]))
#define __get_r1_by_tr( t, tr)		__get_r1_by_t(t, (tr)->ch, (tr)->r1)
#define __get_seg_by_tr(t, tr)		(&(__get_r1_by_tr(t,tr)->segments[(tr)->seg]))

#define chunk_for_each_raid1(chunk, r1, r) \
	for (r = 0, r1 = (chunk)->raid1s ? (chunk)->raid1s : 0; \
		(chunk)->raid1s && \
			(r < __get_num_r1s((chunk)->topology)); ++r1, ++r)

#define chunk_for_each_seg(chunk, r1, r, seg, si) \
	chunk_for_each_raid1(chunk, r1, r) \
		raid1_for_each_seg(r1, seg, si)

#define topo_for_each_chunk(t, chunk, c) \
	for (c = 0, chunk = t ? (t)->chunks : 0; \
		t && (c < __get_topo_num_chunks(t)); ++chunk, ++c)

#define topo_for_each_raid1(t, chunk, c, r1, r) \
	topo_for_each_chunk(t, chunk, c) \
		chunk_for_each_raid1(chunk, r1, r)

#define topo_for_each_seg(t, chunk, c, r1, r, seg, si) \
	topo_for_each_raid1(t, chunk, c, r1, r) \
		raid1_for_each_seg(r1, seg, si)

/* When we dup topo, some unregistered segments may have info, which require
   cleaning before sending register*/
void nvmeibc_segment_clear_b4_reg(struct nvmeibc_disk_segment *seg,
								  const char* debug_reason);

#endif


