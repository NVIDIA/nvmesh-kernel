//usr/bin/make clean all >/dev/null && LD_LIBRARY_PATH=. exec ./testclnt "${@}"

/*
 * This demo corecomm client exists for sake of POC only
 * It can also server as a basic example for corecomm api usage
 */

#include "corecomm.h"
#include <errno.h>
#include <stdio.h>

int main(void) {

	corecomm_handle corecomm = corecomm_create();
	cdisk_handle disk;
	struct lock_data ldata;
	long rv;

	if (!corecomm) {
		fprintf(stderr, "Well fuck you too!\n");
		return 1;
	}

	// clang-format off
	// Stripe Replica Status   Disk NVMe ID      0xLBA Start  0xLBA End    Last Known Target     Debug-info
	// 0      0       Online   S3P8NY0J700237.1  8f180        38a5d7f      nvme109.acme.com  [a=1 p=0 acm=RW  sy=1 lm(O0) r1v=0x100 lid=0x70|a uid=a67280d1
	// clang-format on

	if ((rv = corecomm_register_arnic(corecomm, "nvme109.acme.com",
	                                  "0x00000000000000000000ffff0a0a6d02", 1,
	                                  CORECOMM_RDMA_ROCE)) < 0) {
		fprintf(stderr, "corecomm_register_arnic %d %m\n", errno);
		return 1;
	} else
		printf("corecomm_register_arnic: %ld\n", rv);

	if ((rv = corecomm_register_arnic(corecomm, "nvme109.acme.com",
	                                  "0x00000000000000000000ffff0a0b6d02", 1,
	                                  CORECOMM_RDMA_ROCE)) < 0) {
		fprintf(stderr, "corecomm_register_arnic %d %m\n", errno);
		return 1;
	} else
		printf("corecomm_register_arnic: %ld\n", rv);

	if ((disk = corecomm_discover(corecomm, "S3P8NY0J700237.1",
	                              "nvme109.acme.com")) < 0) {
		fprintf(stderr, "corecomm_discover %d %m\n", errno);
		return 1;
	} else
		printf("Got a disk handle: %lld\n", disk);

	if ((corecomm_pd_cmpxchg(corecomm, disk, 0x8f190, 0, 0xdeadbabe, &ldata)) <
	    0) {
		fprintf(stderr, "corecomm_pd_cmpxchg %d %m\n", errno);
	} else {
		printf("Got a lock response status: %d value 0x%llx\n", ldata.status,
		       ldata.value);
	}

	return 0;
}
