# Trace Daemon Protocol

Trace daemon runs a "capuch" thread for each channel and core, which spends most of its time in a blocking read call on a kernel-exposed traces control file.
Whenever the read returns, it contains a struct that has an array of "cells", which are page offsets in the traces control file. The control file is also mmap()-ed by trace daemon, and page fault function translates the virtual page addresses resulting from those offsets back into cell numbers, and ultimately to the actual physical buffer pages. Those page faults are usually not performed directly from trace daemon, but indirectly by kernel when the mmap()-ed addresses are sent to write() to a traces file, opened with O_DIRECT. This allows page data to not travel via userspace.

The struct returned from read() on the control file contains 511 cells and a count. A special case where this count is 0 means that the actual number of cells is 1, and that the cell is still "active" (and incomplete). Trace daemon writes such incomplete cells as usual, the only distinction is that the write offset on the traces file stays the same, so that any subsequent write (partial or not) overwrites that last partial traces buffer page. This provides immediate visibility of last traces in this channel, even if the buffer in kernel stays active, potentially forever (no new traces).

Relevant functions:
- control_proc_read(): kernel side read handler
- trace_mmap_page_fault(): kernel side page fault handler
- _write_log_with_wraparound(): trace daemon read result handler

### Compression

When traces compression is enabled, each "capuch" trace daemon thread maintains a compression context ("compressor" instance) and uses it to "write" the buffers of cells returned by the control file read to the compressor, resulting in compressed data, which is in turn written to the traces files. Here, page faults on the mmap()-ed file are performed directly by the trace daemon from user space, since the buffer data is accessed from user space for compression.

The compression context has ingress buffering, so not all of the data written to the compressor comes out immediately as a compressed result (delay #1). Compressor API supports flushing, but this is currently not performed by trace daemon, except (implicitly) when the trace file is closed.

In addition, any partial cell cannot be written to the compressor, since its compressed result cannot be overwritten as required in the traces file (delay #2). There is no actual loss of traces because of this, since when (if) a partial buffer is eventually filled, it is then given to the trace daemon once again as a full cell.

**TODO**: When compression is enabled, maintain an additional single page file per capuch for partial writes. Pager needs to consider this buffer, but in a special way: if the buffer also exists in the compressed form, take that, since this is the full buffer, and drop the partial one without warning about duplicate buffer.  
Also consider flushing compressor on each write. This might cause some degradation of the compression rate when buffers come one by one, but when the trace rate is high no significant degradation is expected, and this is what's important for compression.