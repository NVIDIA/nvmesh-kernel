#include "kr_incs_time_jiff.h"
#include "common/nvmeib_math.h"

struct timezone sys_tz = { -120, 0 };				// Israel is -2 hours
unsigned int tsc_khz = 0;
unsigned long loops_per_jiffy = 0;
unsigned long long tsc_offset = 0;					// Needed only for traces

static void __estimate_cpu_frequency(void)
{	// Dont use cat /proc/cpuinfo | grep 'cpu MHz', as it can vary due to dynamic frequency scaling (e.g., Turbo Boost)
	const unsigned long long billion = 1000000000L;
	const struct timespec req = {0, billion/10}; // 1/10th sec
	struct timespec start, end;
	unsigned long long tsc_start, tsc_end;

	clock_gettime(CLOCK_MONOTONIC, &start);		// Deliberatly monotonic
	tsc_start = nvmeib_public_rdtsc();
	nanosleep(&req, NULL); // Sleep for 1/10th sec to measure elapsed cycles
	clock_gettime(CLOCK_MONOTONIC, &end);
	tsc_end = nvmeib_public_rdtsc();
	{ // Compute elapsed time in nanoseconds
		const unsigned long long elapsed_ns = (end.tv_sec - start.tv_sec) * billion + (end.tv_nsec - start.tv_nsec);
		const unsigned long long elapsed_cycles = tsc_end - tsc_start;
		const unsigned long long cycles_per_sec = (elapsed_cycles * billion / elapsed_ns);
		tsc_khz = (unsigned int)DIV_ROUND_CLOSEST(cycles_per_sec, 1000);
		loops_per_jiffy =       DIV_ROUND_CLOSEST(cycles_per_sec, HZ);
	}
	{ // Compute tsc_offset
		tsc_end = nvmeib_public_rdtsc();
		clock_gettime(CLOCK_REALTIME, &end);	// Deliberatly realtime
		{
			const unsigned long long end_ns = end.tv_sec * billion + end.tv_nsec;
			tsc_offset = MUL_X_DIV_Y(end_ns, tsc_khz, 1000000L) - tsc_end;
		}
	}
}

static unsigned long _jiffies_start_cnt = 0;
unsigned long _jiffies(void) {
	return (nvmeib_public_rdtsc() / loops_per_jiffy) - _jiffies_start_cnt;
}

void int_cpu_freq_tsc_offset_jiffies(void) {
	if (tsc_khz == 0) {
		__estimate_cpu_frequency();
		_jiffies_start_cnt = _jiffies(); // Jiffies start from 0 after this function is called
	} else {	// Initialized via kernel /proc where real nvmesh exists
		loops_per_jiffy = DIV_ROUND_CLOSEST((tsc_khz*1000), HZ);
	}
}
