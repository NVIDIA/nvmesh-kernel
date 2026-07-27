int nvmeib_buffer_alloc_sgl_from_pages(struct nvmeib_buffer *buf, struct page **pages, 
				       unsigned int n_pages, unsigned int size, unsigned int offset, gfp_t gfp_mask)
{
	int rv = sg_alloc_table_from_pages(&buf->sgt, pages,
					   n_pages, 0, size, gfp_mask);
	if (rv < 0)
		goto out;
	buf->size = size;
	buf->offset = offset;
	out:
	return rv;
}
EXPORT_SYMBOL(nvmeib_buffer_alloc_sgl_from_pages);

void nvmeib_buffer_copy_md_ext_to_buffer(const struct nvmeib_buffer *buf,
					 size_t sect_sz, size_t md_sz, void *md_dest, size_t md_dest_sz)
{
	// Initialize the scatter-gather iterator and processing mode.
	struct sg_mapping_iter miter;
	enum {
		PROC_MODE_SKIP = 0,
		PROC_MODE_COPY = 1
	} proc_mode = PROC_MODE_SKIP;
	size_t bytes_per_mode[2] = {
		sect_sz,
		md_sz
	};
	size_t bytes_in_mode = bytes_per_mode[proc_mode];
	
	// Start the scatter-gather iterator.
	sg_miter_start(&miter, buf->sgt.sgl, buf->sgt.nents, SG_MITER_FROM_SG | SG_MITER_ATOMIC);
	
	sg_miter_skip(&miter, buf->offset);
	
	// Loop through the scatter-gather list and process data.
	while (sg_miter_next(&miter) && miter.consumed < buf->size && md_dest_sz > 0) {
		size_t rem_bytes_in_seg = miter.length;
		off_t seg_off = 0;
		
		// Loop through the scatter-gather entry and process data
		while (rem_bytes_in_seg > 0 && miter.consumed + seg_off < buf->size) {
			size_t bytes_to_proc = min(bytes_in_mode, rem_bytes_in_seg);
			
			if (proc_mode == PROC_MODE_COPY) {
				// In copy mode, copy the MD to the destination
				memcpy(md_dest, miter.addr + seg_off, bytes_to_proc);
				md_dest += bytes_to_proc;
				md_dest_sz -= bytes_to_proc;
			}
			
			// Update the counters for the current mode and sg entry
			rem_bytes_in_seg -= bytes_to_proc;
			bytes_in_mode -= bytes_to_proc;
			seg_off += bytes_to_proc;
			
			if (bytes_in_mode == 0) {
				//Mode complete, switch mode
				proc_mode = 1 - proc_mode;
				bytes_in_mode = bytes_per_mode[proc_mode];
			}
		}
	}
	
	//Stop the scatter-gather iterator
	sg_miter_stop(&miter);
}
EXPORT_SYMBOL(nvmeib_buffer_copy_md_ext_to_buffer);

void nvmeib_buffer_fill(struct nvmeib_buffer *buf, const void *pattern, size_t pattern_len)
{
	struct sg_mapping_iter miter;
	size_t remaining_len = pattern_len;
	const void *pattern_ptr = pattern;
	
	// Start the scatter-gather iterator.
	sg_miter_start(&miter, buf->sgt.sgl, buf->sgt.nents, SG_MITER_TO_SG | SG_MITER_ATOMIC);
	
	sg_miter_skip(&miter, buf->offset);
	
	while (sg_miter_next(&miter) && miter.consumed < buf->size) {
		off_t seg_off = 0;
		size_t bytes_to_fill = min3(remaining_len, miter.length, buf->size - miter.consumed);
		
		// Fill the scatterlist entry with the pattern, looping if necessary.
		while (bytes_to_fill > 0) {
			size_t copy_len = min_t(size_t, bytes_to_fill, remaining_len);
			
			// Copy part of the pattern into the entry.
			memcpy(miter.addr + seg_off, pattern_ptr, copy_len);
			
			// Update the pattern state
			bytes_to_fill -= copy_len;
			pattern_ptr += copy_len;
			seg_off += copy_len;
			
			if (pattern_ptr == pattern + pattern_len) {
				// Reset the pattern pointer to the beginning if it reaches the end.
				pattern_ptr = pattern;
			}
			
			// Calculate the remaining length for the next entry
			remaining_len = pattern_len - (pattern_ptr - pattern);
		}
	}
	
	//Stop the scatter-gather iterator
	sg_miter_stop(&miter);
}
EXPORT_SYMBOL(nvmeib_buffer_fill);
