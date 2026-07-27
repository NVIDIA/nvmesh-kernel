#ifndef NVMEIBC_DP_DBGDI_H
#define NVMEIBC_DP_DBGDI_H
/* Full spec and design document: https://docs.google.com/document/d/1xOKzfihjCJTfp7bEa-QuDWi3S7ofxOCNXnxULM0R_JI/edit
   Debug di injects data to each command according to its type not the IO type!
   In EC datapath write IO includes some read commands and some are writes.
   The following data is injected:
   READ command:
    1. Precondition the sgl before sending read to detect local failures
    2. When command returns, inject readers info to know on which topology
       the read was issued (different from the topology on which the block
       was written)
   WRITE command:
    1. Inject writers information. Each written block includes it. Trimmed
       blocks and non preconditioned drives do not inlclude it. Trims may cause
       data corruption (generate non ACID data). This is a known issue
   TRIM command:
       Nothing is injected

   Sync command:
       Appends syncer data
       Writer data of block:
          R1 - 1. Moves original writer from target seg 'W' to sync overwritten section, if possible
               2. Copies writer from the source segment (other leg 'RW') into target seg
    	  EC - No-write-hole:
    			In most cases Cleans it because when recosntructed using XOR, there is no original writer info to copy from
    			When Readable parities are re-written just to change their metadata, leave the writer as is
          EC - Roll-fwd: Upon roll forward Copies original writer form journal block
   Scrubbing:
       Original writer is preserved, adds relevant sync section

   More info here:
       https://docs.google.com/document/d/1xOKzfihjCJTfp7bEa-QuDWi3S7ofxOCNXnxULM0R_JI/edit

    Each read block holds injected data of
    1. Writer - only if block was written prior to read
    2. Syncer - only if block was copied by sync from another segment
    3. Reader - always exists
   */
#include "../datapath_utils_generic/nvmeibc_block_dp_common.h"
#include "nvmeibc_block_dp_dbgdi_blk.h"
#ifdef DBGDI_REMOVED_IN_PRODUCTION
	#define DEBUG_DI_SIZE_CALC(is_di_debug)  				({ (void)is_di_debug; NVMEIBC_SECTOR_SIZE; })
	static inline void dp_dbgdi_clear_sync_overwritten(struct nvmeibc_block_command *cmd) { (void)cmd; }
	static inline void dp_dbgdi_copy_sync_overwritten( struct nvmeibc_block_command *dst, const struct nvmeibc_block_command *src)  { (void)dst; (void)src; }
	#define __print_block_debug(s)
	static inline void dp_dbgdi_do_add_info(        struct nvmeibc_block_command *cmd, bool should_execute)  { (void)cmd; (void)should_execute; }
	static inline void dp_dbgdi_do_add_restore_info(struct nvmeibc_block_command *cmd, bool is_destroyed)    { (void)cmd; (void)is_destroyed;}
	static inline void dp_dbgdi_do_rdr_info(        struct nvmeibc_block_command *cmd)                       { (void)cmd; }
	static inline void dp_dbgdi_clear_destroyed_block_history(void *d) { (void)d; }
	static inline void dp_dbgdi_do_add_info_unitest(          void *d, const char *seg_uuid){ (void)d; (void)seg_uuid; }
	static inline bool dp_dbgdi_should_add_info_core(  struct nvmeibc_disk_io_command *iocmd) { (void)iocmd; return false; }
	#define            dp_dbgdi_do_add_info_core_pre( req, p)
	#define            dp_dbgdi_do_add_info_core_post(req, p)   (0)
	static inline bool dp_dbgdi_should_add_rider_info(const struct operation *o) { (void)o; return false; }
	static inline void dp_dbgdi_do_add_rider_info(void *d, const struct nvmeibc_block_device *car, const struct operation *o) { (void)o; (void)car; (void)d; }
	static inline void dp_dbgdi_do_add_rider_rdr_info(void *bext, const struct nvmeibc_block_device *car) { (void)bext; (void)car; }
	static inline bool dp_dbgdi_should_add_rider_rdr_info(void *bext) { (void)bext; return false; }
	              int  dp_dbgdi_get_sizeof_injected_data(void);
	              void dp_dbgdi_mark_edic(void *data, enum edic_result pass, u32 read_edic, u32 calc_edic, u64 rlba);

#else
#define DEBUG_DI_SIZE (512)
#define DEBUG_DI_SIZE_CALC(        is_di_debug)          ((is_di_debug) ? DEBUG_DI_SIZE : NVMEIBC_SECTOR_SIZE)
void dp_dbgdi_clear_sync_overwritten(struct nvmeibc_block_command *cmd);

void dp_dbgdi_copy_sync_overwritten(struct nvmeibc_block_command *dst, const struct nvmeibc_block_command *src);
void __print_block_debug(const void *s);

/* EC/R1 block layer injection */
void dp_dbgdi_do_add_info(        struct nvmeibc_block_command *cmd, bool should_execute);	// Inject debug info into each block in sgl of cmds[n] before sending read/write/sync
void dp_dbgdi_do_add_restore_info(struct nvmeibc_block_command *cmd, bool is_destroyed_blk);// Inject debug info into each restored block
void dp_dbgdi_do_rdr_info(        struct nvmeibc_block_command *cmd);						// Inject debug info of reader after read succeededs
void dp_dbgdi_do_add_info_unitest(          void *d, const char *seg_uuid);

/* core injection */
struct t_core_dbgdi_params_pre;
struct t_core_dbgdi_params_post;
bool dp_dbgdi_should_add_info_core(  struct nvmeibc_disk_io_command *iocmd);
void dp_dbgdi_do_add_info_core_pre( struct nvmeibc_block_io_req *req, struct t_core_dbgdi_params_pre  * p);
int  dp_dbgdi_do_add_info_core_post(struct nvmeibc_block_io_req *req, struct t_core_dbgdi_params_post * p);


/* Rider-carrier injection */
struct d_carrier_base_block_io;
bool dp_dbgdi_should_add_rider_info(const struct operation *o);
void dp_dbgdi_do_add_rider_info(struct d_carrier_base_block_io *d, const struct nvmeibc_block_device *car, const struct operation *o);
void dp_dbgdi_do_add_rider_rdr_info(const struct bio_extention *bext, const struct nvmeibc_block_device *car);
bool dp_dbgdi_should_add_rider_rdr_info(const struct bio_extention *bext);

/* Returns the size (in bytes) of the injected area inside each block */
int  dp_dbgdi_get_sizeof_injected_data(void);

// Returns pointer to hot recovery debug di injection area
void* dp_dbgdi_get_recovery_hot_area(void* data);
void* dp_dbgdi_get_core_area(        void* data);
void* dp_dbgdi_get_core_area_container (const void *c);

// Marks EDIC check Pass/Fail
void dp_dbgdi_mark_edic(void *data, enum edic_result pass, u32 read_edic, u32 calc_edic, u64 rlba);
#endif	// DBGDI_REMOVED_IN_PRODUCTION
#endif  // H beginning

