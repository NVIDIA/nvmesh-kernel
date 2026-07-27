#ifndef KR_INCS_TIME_JIFFIES_H
#define KR_INCS_TIME_JIFFIES_H

#include "kr_incs_time_rdtsc.h"
#if !defined(__KERNEL__)

	// linux/time.h linux/ktime.h
	// linux/jiffies - accuracy in microseconds
	#ifndef HZ
		#define HZ  (1000000) // linux/jiffies - accuracy in microseconds
	#endif	// #ifndef HZ
	#define jiffies _jiffies()

	#define MSEC_PER_SEC	1000L
	#define USEC_PER_MSEC	1000L
	#define NSEC_PER_USEC	1000L
	#define NSEC_PER_MSEC	1000000L
	#define USEC_PER_SEC	1000000L
	#define NSEC_PER_SEC	1000000000L

	// Time issues: linux/delay.h
	#include <unistd.h>
	static inline void udelay(unsigned long usec ){ usleep(usec);      }
	static inline void msleep(unsigned int  msec ){ usleep(msec*1000); }
	static inline void mdelay(unsigned int  msec ){ usleep(msec*1000); }

	unsigned long _jiffies(void);
	static inline unsigned int jiffies_to_msecs(const unsigned long j){ return (MSEC_PER_SEC * j) / HZ; }
	static inline unsigned int jiffies_to_usecs(const unsigned long j){ return (USEC_PER_SEC * j) / HZ; }
	static inline unsigned long msecs_to_jiffies(const unsigned int m){ return ((unsigned long)m) * HZ / MSEC_PER_SEC; }
	static inline unsigned long usecs_to_jiffies(const unsigned int u){ return ((unsigned long)u) * HZ / USEC_PER_SEC; }

	extern unsigned int tsc_khz;							// Real kernel variable
	extern unsigned long loops_per_jiffy;					// Real kernel variable

	// NVMesh special (not a common kernel api)
	extern unsigned long long tsc_offset;					// Needed only for traces
	void int_cpu_freq_tsc_offset_jiffies(void);

#endif	// __KERNEL__
#include "kr_incs_time.h" // Needs the above defines
#endif
