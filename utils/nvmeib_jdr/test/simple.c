#include "nvmeib_jdr.h"
#include "nvmeib_txt.h"
#include <assert.h>

static void test_txt(void){
	char buf[96] = {0};
	struct charvec buffer = {.base = buf, .len = sizeof(buf)};
	struct nvmeib_txt txt = nvmeib_txt_make(buffer);

	nvmeib_txt_append(&txt, "%s", "hello");
	nvmeib_txt_append(&txt, "%s", " world");

	assert(strcmp(buf, "hello world") == 0);
}

static void test_jdr(void){
	char buf[96] = {0};
	struct charvec buffer = {.base = buf, .len = sizeof(buf)};
	struct jdr jdr = jdr_make(buffer);
	{
		jdr_object_scope(&jdr, NULL);
		jdr_write_var(&jdr, answer, 42);
	}
	jdr_finalize(&jdr);
	assert(buffer.len > 0);
}

int main(void) {
	test_txt();
	test_jdr();

	return 0;
}