#pragma once

#include <sys/time.h>

#define nanoseconds(x) (x)
#define microseconds(x) (x)
#define ndiff(first,second) ((second).tv_sec - (first).tv_sec) * 1000000000 + ((second).tv_nsec - (first).tv_nsec)

//busy wait for some period of time; returns true, if the condition was meat
#define busy_wait_for(period, polling_interval, test_expr) 			          \
({                                                                            \
	struct timespec start, now, polling_interval_spec;				          \
	u64 lapsed_time = 0;                                             	      \
	polling_interval_spec.tv_sec = nanoseconds(polling_interval)/1000000000;  \
	polling_interval_spec.tv_nsec = nanoseconds(polling_interval)%1000000000; \
	clock_gettime(CLOCK_MONOTONIC, &start);                                   \
																			  \
	while((!(test_expr)) && (lapsed_time <= nanoseconds(period))){	          \
		nanosleep(&polling_interval_spec, NULL);				          	  \
		schedule();  												          \
		clock_gettime(CLOCK_MONOTONIC, &now);						          \
		lapsed_time = ndiff(start, now);                          	          \
	}																          \
	((lapsed_time <= nanoseconds(period)) || (test_expr));					  \
})

#define busy_wait_forever(polling_interval, test_expr) 	\
({														\
	while(!(test_expr) && !kthread_should_stop()){		\
		usleep(microseconds(polling_interval));			\
		schedule();										\
	}													\
	(test_expr);										\
})

#define busy_wait_forever_more(polling_interval, test_expr) \
({															\
	while(!(test_expr)){									\
		usleep(microseconds(polling_interval));				\
		schedule();											\
	}														\
	(test_expr);											\
})
