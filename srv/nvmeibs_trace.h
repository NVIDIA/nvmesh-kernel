#if !defined(BLKDEV_SIMULATOR) || (BLKDEV_SIMULATOR!=1)
	#undef NVMESH_TRACE_NAMESPACE
	#define NVMESH_TRACE_NAMESPACE nvmeibs
	#undef NVMESH_TRACE_MODULE
	#define NVMESH_TRACE_MODULE "NVMesh-S"

	#ifndef __FIRST_PASS__
		#include "../common_public/nvmeib_trace.h"
	#endif
	#include "../common/nvmeib_trace_warns.h"
#endif
