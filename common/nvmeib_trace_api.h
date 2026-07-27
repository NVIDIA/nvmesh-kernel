#ifndef NVMEIB_TRACE_API_H
#define NVMEIB_TRACE_API_H

struct nvmeib_trace_header {
	unsigned int ncpus;
	unsigned int tsc_khz;
} __attribute__((packed));
// time in ns  =  timestamp * 1000000 / tsc_khz;

struct nvmeib_trace_dump_range {
	size_t count;
	size_t cells[511];
} __attribute__((packed));

struct nvmeib_trace_offset {
	int buffer_offset : 12;
	int buffer_number : 20;
} __attribute__((packed));

union nvmeib_capuch_key {
	struct {
		unsigned int chid : 8;
		unsigned int cpu : 24;
	};
	unsigned int raw;
} __attribute__((packed));

#endif
