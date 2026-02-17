#include "common/kr_incs.h"
#include "nvmeibc_management_capi_parse_conf_test.h"
#include "management_utils_common/nvmeibc_management_capi_parse_conf.h"
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

/* Buffer size so that (base + 0) + unit_size <= base + size for volumes/targets. */
#define SETUP_VOL_CONF_BUF_SIZE  (sizeof(struct nvmeib_mgmt_to_client_volume_configuration) + \
	((sizeof(struct nvmeibc_volume_conf) > sizeof(struct nvmeibc_target_conf)) ? \
		sizeof(struct nvmeibc_volume_conf) : sizeof(struct nvmeibc_target_conf)))

/**
 * Positive: minimal valid config — n_volumes=0, n_targets=0, volumes and targets
 * as in-range RVAs (offset 0). Implementation requires (ptr + unit_size <= rva_end),
 * so serialized_config_len must be at least max(sizeof(volume_conf), sizeof(target_conf)).
 */
static void test_setup_volume_configuration_positive(void)
{
	struct nvmeib_mgmt_to_client_volume_configuration *conf;
	char buf[SETUP_VOL_CONF_BUF_SIZE];
	size_t len;
	int r;

	memset(buf, 0, sizeof(buf));
	conf = (struct nvmeib_mgmt_to_client_volume_configuration *)buf;
	conf->n_volumes = 0;
	conf->n_targets = 0;
	conf->volumes = (struct nvmeibc_volume_conf *)(uintptr_t)0;
	conf->targets = (struct nvmeibc_target_conf *)(uintptr_t)0;

	len = sizeof(buf);
	r = nvmeibc_setup_volume_configuration(conf, len);

	BUG_ON(r != 0);
	BUG_ON(conf->volumes != (struct nvmeibc_volume_conf *)buf);
	BUG_ON(conf->targets != (struct nvmeibc_target_conf *)buf);
}

/**
 * Negative: volumes RVA is chosen so that (ptr + sizeof(volume_conf)) > rva_end.
 * nvmeibc_setup_volume_configuration should return -EINVAL.
 */
static void test_setup_volume_configuration_negative_rva_out_of_range(void)
{
	struct nvmeib_mgmt_to_client_volume_configuration *conf;
	char buf[SETUP_VOL_CONF_BUF_SIZE];
	size_t len;
	uintptr_t bad_rva;
	int r;

	memset(buf, 0, sizeof(buf));
	conf = (struct nvmeib_mgmt_to_client_volume_configuration *)buf;
	conf->n_volumes = 0;
	conf->n_targets = 0;
	len = sizeof(buf);
	/* RVA so that base + rva + sizeof(volume_conf) > base + len */
	bad_rva = len - sizeof(struct nvmeibc_volume_conf) + 1;
	conf->volumes = (struct nvmeibc_volume_conf *)(uintptr_t)bad_rva;
	conf->targets = (struct nvmeibc_target_conf *)(uintptr_t)0;

	r = nvmeibc_setup_volume_configuration(conf, len);

	BUG_ON(r != -EINVAL);
}

void nvmeibc_management_capi_parse_conf_tests(void)
{
	test_setup_volume_configuration_positive();
	test_setup_volume_configuration_negative_rva_out_of_range();
}
