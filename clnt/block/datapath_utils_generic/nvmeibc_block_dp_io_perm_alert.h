#ifndef NVMEIBC_DP_IO_PERM_ALERT_H
#define NVMEIBC_DP_IO_PERM_ALERT_H

#include "nvmeibc_block.h"		/* external API of the block */

struct nvmeibc_block_device;

/*************** Allert to mgmt when IO is disabled for too long **************/
/*
1) Upon attach we immediately send attachment status with io_perm == 0.
   This informs management that the configuration was parsed correctly.
2) Once IO becomes enabled for the first time (can be instantly) we also send an immediate update to mgmt.
3) We only send IO permission (status)updates on existing volume if the IO state is stable for more than X seconds.
   This means that when IO changes from Enabled (the first time) to disabled we arm the mechanism, if IO is disabled for 10 seconds we update the mgmt.
   The internal state of the io_perm is either armed (to be sent if enough time passes, regardless of the change) or sent.
   If during the armed period the IO state changes back to the previous state, we change from armed to sent, thus we will arm again on the next change.
4) Any client report or update for the volume (not from IO status change) will send the same status as was previously sent to mgmt.
5) The alerts for detach stuck remain the same as before, send mgmt update after 10 seconds then 30 then 60, etc'.
*/

// First bit = 'is bio enable', Second bit = 'Has Protection', Third bit = 'is read only without protection', Fourth bit = 'is recovery enabled'
enum nvmeibc_io_perm_arm {									// Lowest nibble is bit field, highest nibble is enum
	NVMEIBC_IO_PERM_ARM_DEV_INIT 					= 0x00,	// the device just constructed,
	NVMEIBC_IO_PERM_ARM_NO_BIO_NO_SYNC 				= 0x10,	// any io (bio/sync) disabled
	NVMEIBC_IO_PERM_ARM_ONLY_SYNC 					= 0x28,	// Recovery only io enabled, bio is disabled
	NVMEIBC_IO_PERM_ARM_BIO_NO_PROTECTION_RO		= 0x3D,	// WRITE bio rejected
	NVMEIBC_IO_PERM_ARM_BIO_NO_PROTECTION			= 0x49,	// related to the RAID state, where n_dead_sgmnts == n_parities, depends on passed time, the RAID may (not) accept "write" requests
	NVMEIBC_IO_PERM_ARM_BIO_OK               		= 0x5B,	// BIO + Recoveries enalbe + have some protection
	NVMEIBC_IO_PERM_ARM_DETACHING   				= 0x60,	// the device is going to be detached (either for upgrade or not)
	NVMEIBC_IO_PERM_ARM_DESTROYED 					= 0x70,	// the device was destroyed

	// Daniel: Todo, Remove below stuff, just for debug, same as 3 above but with marker of first time this event happened since attach 7th bit is marker of first
	NVMEIBC_IO_PERM_ARM_FIRST_TIME_BIT = 0x80,
	NVMEIBC_IO_PERM_ARM_BIO_NO_PROTECTION_RO_FIRST	= NVMEIBC_IO_PERM_ARM_BIO_NO_PROTECTION_RO|NVMEIBC_IO_PERM_ARM_FIRST_TIME_BIT,
	NVMEIBC_IO_PERM_ARM_BIO_NO_PROTECTION_FIRST		= NVMEIBC_IO_PERM_ARM_BIO_NO_PROTECTION   |NVMEIBC_IO_PERM_ARM_FIRST_TIME_BIT,
	NVMEIBC_IO_PERM_ARM_BIO_OK_FIRST 				= NVMEIBC_IO_PERM_ARM_BIO_OK              |NVMEIBC_IO_PERM_ARM_FIRST_TIME_BIT,
};

#define nvmeibc_io_perm_arm_has_bio(arm)                 (1 == (1 & (arm)))		// Test bio bit
#define nvmeibc_io_perm_arm_has_bio_with_protection(arm) (3 == (3 & (arm)))		// Test bio and protection bits

typedef unsigned long nvmeibc_jiffies_t;
struct nvmeibc_io_perm_alert {
	u64 bdev_id; //will container pointer to the block device for easy bin tracing; should never be used for any other purpose!!!

	__concurrent_access nvmeibc_jiffies_t last_armed_at;						// The timestamp when the alert was armed
	__concurrent_access volatile enum nvmeibc_io_perm_arm arm;					// contains current device status
	__concurrent_access volatile enum nvmeibc_io_perm_arm arm_stable;			// contains previous stable status

	nvmeibc_jiffies_t unprotected_write_to_read_only_at; 	// The time stamp when topo becomes read only. Becomes non zero only when (arm == NO_PROTECTION occured). Back to zero when degradedness is fixed

	struct {
		nvmeibc_jiffies_t stabilization_period; 			// period of time, after which the status is considered to be stable
		nvmeibc_jiffies_t unprotected_write_period;			// period of time, after which degraded-p raid becomes readonly
		nvmeibc_jiffies_t stucked_detach_alert_freq;		// period of time, to allert the mgmt on stuck detaches
		int  (*alert_sender)(const struct nvmeibc_block_device* dev, char *msg);						// Virtual function. Can be changed to send to log/mgmt/NULL
		void (*notify_io_changed)(struct nvmeibc_block_device *dev, enum nvmeibc_io_perm_arm curr);
	} config;

	struct {
		u32 n_stucked_detach_alert_sent;		 			// Counter of sent allerts regarding current problem. Zeroed once problem is solved
		u32 longest_io_problem_duration_msec;				// Historically the longest problem we had with IO
	} stats;
};

/* Constructor destructor: Do not call from interrupt context! */
nvmeibc_jiffies_t nvmeibc_io_perm_alert_create(struct nvmeibc_io_perm_alert *iod, u64 bdev_id);
void nvmeibc_io_perm_alert_destroy(struct nvmeibc_io_perm_alert *iod);

/* Call when IO is disabled or enabled (possibly from interrupt context) */
nvmeibc_jiffies_t //for testing purpose returns "now", used during the calculations
nvmeibc_io_perm_alert_switch( struct nvmeibc_io_perm_alert *iod, enum nvmeibc_io_perm_arm reason);

/* Called by watchdog periodically, not from interrupt */
void nvmeibc_io_perm_alert_periodic_wakeup(struct nvmeibc_block_device *dev, nvmeibc_jiffies_t now);

/* Update parameters */
void nvmeibc_io_perm_alert_set_stucked_detach_alert_freq(      struct nvmeibc_io_perm_alert *iod, u32 freq);
void nvmeibc_io_perm_alert_set_unprotect_period(               struct nvmeibc_io_perm_alert *iod, int sec /* -1 == use default */);		// Called via attach/ioctl/io-enabled-first-time
void nvmeibc_io_perm_alert_clear_stats(                        struct nvmeibc_io_perm_alert *iod);

/* To string to debug in proc file */
int nvmeibc_io_perm_alert_tostring(const struct nvmeibc_io_perm_alert *iod, char *buf, int buf_len, char fmt);		// fmt: 'H'=Human, 'J'=json

static inline bool nvmeibc_io_perm_alert_is_no_bio_for_long_time(const struct nvmeibc_io_perm_alert *iod)
{
	return (false == nvmeibc_io_perm_arm_has_bio(iod->arm_stable));
}

//the read-only topology may be created in 2 cases:
//* on transition from I/O disabled mode to I/O enabled, but without protection; This may happen in case unprotected_write_period is 0
//* on transition from I/O enabled, but without protection after unprotected_write_period ended
bool nvmeibc_io_perm_alert_should_create_readonly_topo(const struct nvmeibc_io_perm_alert *iod, nvmeibc_jiffies_t now);

#endif  // H beginning

