#include "error_tags_tests.h"
#include "nvmeibc_error_tags.h"

static struct nvmesh_error_tag const *  do_smth(int i)
{
	if (i<0){
		NVMEIBC_ERROR_TAG(negative);
		nvmesh_error_tag_update(negative, -EBADE);
		return negative;
	} else if (i == 0){
		NVMEIBC_ERROR_TAG(zero);
		nvmesh_error_tag_update(zero, -EBADE);
		return zero;
	} else {
		NVMEIBC_ERROR_TAG(positive);
		nvmesh_error_tag_update(positive, -EBADE);
		return positive;
	}
}

static void __ut_error_tags_test_section(void)
{
	size_t mm_total_count = 0;
	size_t mm_xyz_found = 0;
	struct nvmesh_error_tag const* err_tags[] = { do_smth(-1), do_smth(0), do_smth(1)};

	for(struct nvmesh_error_tag* curr = __start_nvmeibc_error_tags; curr != __stop_nvmeibc_error_tags; ++curr){
		mm_total_count += 1;
		mm_xyz_found += (curr == err_tags[0] || curr == err_tags[1] || curr == err_tags[2]);
	}
	BUG_ON(mm_total_count < 3);
	BUG_ON(mm_xyz_found != 3);
}

static void __ut_error_tags_populate(void)
{
	do_smth(-1);
	do_smth(0);
	do_smth(0);
	do_smth(1);
	do_smth(1);
	do_smth(1);
}

static void __ut_error_tags_test_json_serialize(void)
{
	struct charvec buffer = {.base = malloc(1024*1024), .len = 1024*1024};
	nvmesh_error_tags_json_serialize(buffer, true, __start_nvmeibc_error_tags, __stop_nvmeibc_error_tags);
	if (0){
		printf("%s\n", buffer.base);
	}
	
	 __ut_error_tags_populate();

	nvmesh_error_tags_json_serialize(buffer, true, __start_nvmeibc_error_tags, __stop_nvmeibc_error_tags);
	if (0){
		printf("%s\n", buffer.base);
	}
	fflush(stdout);
	free(buffer.base);
}

void test_error_tags(void)
{
	__ut_error_tags_test_section();
	__ut_error_tags_test_json_serialize();
}
