#ifndef NVMEIBC_BLOCK_DP_DEFS_H
#define NVMEIBC_BLOCK_DP_DEFS_H

#ifndef NVMEIBC_SECTOR_SHIFT
	#error Wrong compilation flags: missing NVMEIBC_SECTOR_SHIFT
#endif

#define NVMEIBC_SECTOR_SIZE	   (1 << NVMEIBC_SECTOR_SHIFT)
#define BYTES_IN_LOCKSET_SHIFT (17)												// Each lock protects 128KB contiguous memory.
#define BYTES_IN_LOCKSET	   (1 << BYTES_IN_LOCKSET_SHIFT)
#define LOCKSET_SLICES_SHIFT   (BYTES_IN_LOCKSET_SHIFT - NVMEIBC_SECTOR_SHIFT)	// 256 blocks of 512[b] or 32 x 4K
#define LOCKSET_SLICES		   (1 << LOCKSET_SLICES_SHIFT)

#define LOCKSET_SHIFT          (17 - NVMEIBC_SECTOR_SHIFT + LOCK_CHANGE_STRIDE_SHIFT) 	// must be >=log(LOCKSET_SLICES). On mirrored segs owner locks are zigzagged every 2^LOCKSET_SHIFT blocks. we add 1 to avoid Intel 128k performance penalty
#define LS_UNLOCKED 0x00000000										// The value of an unlocked lock

/* Temp: Management, Targets and Toma all talk in 4K blocks. Some defines to handle this*/
#define LOCKSET_4KS_SHIFT      (5)
#define LOCKSET_4KS            (1 << LOCKSET_4KS_SHIFT)				// Temp: Number of 4KB blocks in a single lock-set, Most of the system is oblivious of the block size and works in 4K
#define LOCKSET_4KS_MASK	   (~((1 << LOCKSET_4KS_SHIFT) - 1))
#define MGMT2CLNT_SHIFT		   (12 - NVMEIBC_SECTOR_SHIFT)			// Temp: Management gives configuration in 4K blocks
#define __to4K(addr)   ((addr)>>MGMT2CLNT_SHIFT)					// Needed as long as the rest of NVMEsh works in 4K and only block device in other sizes
#define __from4K(addr) ((addr)<<MGMT2CLNT_SHIFT)
#define __bytesTo4K(addr)   ((addr)>>12)
#define NVMEIBC_SECTOR2BYTE(_blocks)      ((_blocks) << NVMEIBC_SECTOR_SHIFT)
#define NVMEIBC_BYTE2SECTOR(_bytes)       ((_bytes ) >> NVMEIBC_SECTOR_SHIFT)

#endif /* NVMEIBC_BLOCK_DP_DEFS_H */
