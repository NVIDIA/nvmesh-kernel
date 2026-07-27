#if !defined(BLKDEV_SIMULATOR) || (BLKDEV_SIMULATOR != 1)
	#undef NVMESH_TRACE_NAMESPACE
	#define NVMESH_TRACE_NAMESPACE nvmeibm
	#undef NVMESH_TRACE_MODULE
	#define NVMESH_TRACE_MODULE "NVMesh"

	#ifndef __FIRST_PASS__
		#ifndef _TRACE_NVMEIBM_H
			#define _TRACE_NVMEIBM_H
			#include "../common_public/nvmeib_trace.h"
		#endif
	#endif
	#include "../common/nvmeib_trace_warns.h"
#endif/*BLKDEV_SIMULATOR*/
