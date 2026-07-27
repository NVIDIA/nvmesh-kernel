#ifndef NVMEIB_COMMON_OS_BLOCK_API_H
#define NVMEIB_COMMON_OS_BLOCK_API_H

#include "kr_version.h"

#if KS_HAS_BLKMODE
	#define BLK_MODE_T blk_mode_t
	#define BLK_MODE_EXCL BLK_OPEN_EXCL
	#define MODE_WRITES_ALLOWED (BLK_OPEN_READ | BLK_OPEN_WRITE)
	#define BLK_MODE_OPEN_OBJ_T gendisk
	#define BLK_MODE_GENDISK(x) (x)

	// Allow upper layer to take reference an atom api, same way as user space apps can
	#define SELF_REF_MODE			BLK_OPEN_READ									/* Self reference are always readonly because they issue no IO */
#else /* KS_HAS_BLKMODE */
	#define BLK_MODE_T fmode_t
	#define BLK_MODE_EXCL FMODE_EXCL
	#define MODE_WRITES_ALLOWED (FMODE_PWRITE|FMODE_WRITE)
	#define BLK_MODE_OPEN_OBJ_T block_device
	#define BLK_MODE_GENDISK(x) (x)->bd_disk

	// Allow upper layer to take reference an atom api, same way as user space apps can
	#define SELF_REF_MODE			FMODE_READ									/* Self reference are always readonly because they issue no IO */
#endif /* KS_HAS_BLKMODE */

#if (!KS_PDE_DATA)
	#define file_get_priv_data(file) PDE(file->f_path.dentry->d_inode)->data
#elif KS_PDE_DATA_IS_LOWER
	#define file_get_priv_data(file) pde_data(file_inode(file))
#else
	#define file_get_priv_data(file) PDE_DATA(file_inode(file))
#endif

#if KS_SUBMIT_BIO_VOID_RV
	#define REQ_RET void
#elif KS_BLK_QC_T
	#define REQ_RET blk_qc_t
#elif KS_BLOCK_DEV_MAKE_REQUEST_VOID
	#define REQ_RET void
#else
	#define REQ_RET int
#endif
#define REQ_RET_ZERO	((REQ_RET)0)		// Errors are handled via cb bio_endio(). req() always returns zero

#if KS_BIO_HAS_BI_BDEV_PTR
	#define bio_gendisk(bio) ((bio)->bi_bdev->bd_disk)	/* (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0)) && (LINUX_VERSION_CODE < KERNEL_VERSION(5, 12, 0)) */
	#define BIO_CLONE_FAST_DISK_OR_BDEV(dst, src) ((dst)->bi_bdev) = ((src)->bi_bdev);
#elif KS_BIO_HAS_BI_GENDISK_PTR
	#define bio_gendisk(bio) ((bio)->bi_disk)
	#define BIO_CLONE_FAST_DISK_OR_BDEV(dst, src) ({ \
		((dst)->bi_disk) = ((src)->bi_disk);			\
		((dst)->bi_partno) = ((src)->bi_partno);		\
	})
#else
	#error KS_BIO_HAS_BI_BDEV_PTR and KS_BIO_HAS_BI_GENDISK_PTR are equal to 0
#endif

#endif // NVMEIB_COMMON_OS_BLOCK_API_H
