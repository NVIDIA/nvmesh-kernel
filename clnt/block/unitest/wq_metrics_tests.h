#ifndef WQ_METRICS_TESTS_H
#define WQ_METRICS_TESTS_H

#include "nvmeib_wq_metrics.h"

extern struct nvmeib_wq_metrics __start_ut_wq_metrics[];
extern struct nvmeib_wq_metrics __stop_ut_wq_metrics[];

void test_wq_metrics(void);

#endif /* WQ_METRICS_TESTS_H */
