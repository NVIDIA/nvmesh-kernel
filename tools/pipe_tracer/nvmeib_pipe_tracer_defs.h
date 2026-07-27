#ifndef NVMEIB_PIPE_TRACER_DEFS_H
#define NVMEIB_PIPE_TRACER_DEFS_H

#if !USER_SPACE_TRACING
	#error "nvmeib_pipe_tracer: Compiling without USER_SPACE_TRACING."
#endif
#if !USE_EXTERN_TIMESTAMP
	#error "nvmeib_pipe_tracer: Compiling without USE_EXTERN_TIMESTAMP."
#endif

#undef NVMESH_TRACE_NAMESPACE
#define NVMESH_TRACE_NAMESPACE nvmeib_pipe
#undef NVMESH_TRACE_MODULE
#define NVMESH_TRACE_MODULE "NVMesh-PIPE"

extern struct trace_channel *nvmeib_pipe_trace_long;
extern __thread unsigned long long nvmeib_trace_timestamp_ns;
#ifndef __FIRST_PASS__
	#include "../../common_public/nvmeib_trace.h"
	#include <string.h>
	#include <stdio.h>
	#include <limits.h>
	#include "gen_events.h"
#endif

#endif /*NVMEIB_PIPE_TRACER_DEFS_H*/
