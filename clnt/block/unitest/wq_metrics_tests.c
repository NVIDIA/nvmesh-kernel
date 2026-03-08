#include "wq_metrics_tests.h"
#include "kr_incs.h"

NVMEIBC_WQ_METRIC(wq_zero, "reason=zero");
NVMEIBC_WQ_METRIC(wq_one, "reason=one");
NVMEIBC_WQ_METRIC(wq_two, "reason=two");

static void __ut_wq_test_section(void)
{
	size_t total_count = 0;
	size_t found = 0;
	struct nvmeib_wq_metrics *curr;

	for (curr = __start_nvmeibc_wq_metrics; curr != __stop_nvmeibc_wq_metrics; ++curr) {
		total_count += 1;
		found += (curr == wq_zero || curr == wq_one || curr == wq_two);
	}
	BUG_ON(total_count < 3);
	BUG_ON(found != 3);
}

static void __ut_wq_test_update(void)
{
	struct nvmeib_wq_metric_counters merged;

	nvmeib_wq_metrics_update(wq_one, 256);
	nvmeib_wq_metrics_update(wq_one, 512);
	nvmeib_wq_metrics_update(wq_one, 1024);

	merged = nvmeib_wq_metrics_merge_cpus(wq_one);

	/* bin 1: 256, bin 2: 512, bin 3: 1024 */
	BUG_ON(merged.wait_time.bins[1] != 1);
	BUG_ON(merged.wait_time.bins[2] != 1);
	BUG_ON(merged.wait_time.bins[3] != 1);
	BUG_ON(merged.wait_time.max != 1024);
}

static void __ut_wq_test_json_serialize(void)
{
	size_t const buf_size = 1024 * 1024;
	struct charvec buffer = { .base = malloc(buf_size), .len = buf_size };

	nvmeib_wq_metrics_json_serialize(buffer, true /* dump_all_cpus */, false /* dump_per_cpu */,
					 __start_nvmeibc_wq_metrics, __stop_nvmeibc_wq_metrics);

#ifdef DO_JSON_DUMP
	printf("%s\n", buffer.base);
#endif

	nvmeib_wq_metrics_update(wq_two, 100);
	nvmeib_wq_metrics_update(wq_two, 300);

	nvmeib_wq_metrics_json_serialize(buffer, true /* dump_all_cpus */, false /* dump_per_cpu */,
					 __start_nvmeibc_wq_metrics, __stop_nvmeibc_wq_metrics);

#ifdef DO_JSON_DUMP
	printf("%s\n", buffer.base);
	fflush(stdout);
#endif

	free(buffer.base);
}

static void __ut_wq_test_json_serialize_pcpu(void)
{
	size_t const buf_size = 1024 * 1024;
	struct charvec buffer = { .base = malloc(buf_size), .len = buf_size };

	nvmeib_wq_metrics_update(wq_one, 256);

	nvmeib_wq_metrics_json_serialize(buffer, true /* dump_all_cpus */, true /* dump_per_cpu */,
					 __start_nvmeibc_wq_metrics, __stop_nvmeibc_wq_metrics);

#ifdef DO_JSON_DUMP
	printf("per-cpu:\n%s\n", buffer.base);
	fflush(stdout);
#endif

	free(buffer.base);
}

static void __ut_wq_test_trace_dump(void)
{
	/* wq_one and wq_two have data from previous tests */
	nvmeibc_wq_metrics_trace_dump();
}

static void __ut_wq_test_clear(void)
{
	struct nvmeib_wq_metric_counters merged;

	/* wq_one already has data from __ut_wq_test_update */
	merged = nvmeib_wq_metrics_merge_cpus(wq_one);
	BUG_ON(merged.wait_time.max == 0);

	nvmeib_wq_metrics_clear(__start_nvmeibc_wq_metrics, __stop_nvmeibc_wq_metrics);

	merged = nvmeib_wq_metrics_merge_cpus(wq_one);
	BUG_ON(merged.wait_time.max != 0);

	merged = nvmeib_wq_metrics_merge_cpus(wq_two);
	BUG_ON(merged.wait_time.max != 0);
}

void test_wq_metrics(void)
{
	__ut_wq_test_section();
	__ut_wq_test_update();
	__ut_wq_test_json_serialize();
	__ut_wq_test_json_serialize_pcpu();
	__ut_wq_test_trace_dump();
	__ut_wq_test_clear();
}
