#include "nvmeib_jdr.h"
#include "nvmeib_txt.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static void assert_tokens_in_order(char const *text, char const * const tokens[], size_t token_count)
{
	size_t i;
	char const *cursor = text;

	for (i = 0; i < token_count; ++i) {
		cursor = strstr(cursor, tokens[i]);
		assert(cursor != NULL);
		cursor += strlen(tokens[i]);
	}
}

static void test_txt(void){
	char buf[96] = {0};
	struct charvec buffer = {.base = buf, .len = sizeof(buf)};
	struct nvmeib_txt txt = nvmeib_txt_make(buffer);

	nvmeib_txt_append(&txt, "%s", "hello");
	nvmeib_txt_append(&txt, "%s", " world");

	assert(strcmp(buf, "hello world") == 0);
}

static void test_txt_format_specifiers(void)
{
	char buf[256] = {0};
	struct charvec buffer = {.base = buf, .len = sizeof(buf)};
	struct nvmeib_txt txt = nvmeib_txt_make(buffer);
	struct charvec out;

	nvmeib_txt_append(&txt, "int=%d ", 42);
	nvmeib_txt_append(&txt, "hex=0x%x ", 255);
	nvmeib_txt_append(&txt, "str=%s ", "test");
	nvmeib_txt_append(&txt, "char=%c", 'A');

	out = nvmeib_txt_finalize(&txt);
	assert(out.base == buf);
	assert(strstr(buf, "int=42") != NULL);
	assert(strstr(buf, "hex=0xff") != NULL);
	assert(strstr(buf, "str=test") != NULL);
	assert(strstr(buf, "char=A") != NULL);
}

static void test_txt_finalize(void)
{
	char buf[128] = {0};
	struct charvec buffer = {.base = buf, .len = sizeof(buf)};
	struct nvmeib_txt txt = nvmeib_txt_make(buffer);
	struct charvec out;

	nvmeib_txt_append(&txt, "%s", "finalized");
	out = nvmeib_txt_finalize(&txt);

	assert(out.base == buf);
	assert(out.len == strlen("finalized"));
	assert(strcmp(buf, "finalized") == 0);
}

static void test_txt_empty(void)
{
	char buf[64] = {0};
	struct charvec buffer = {.base = buf, .len = sizeof(buf)};
	struct nvmeib_txt txt = nvmeib_txt_make(buffer);
	struct charvec out;

	out = nvmeib_txt_finalize(&txt);
	assert(out.base == buf);
	assert(out.len == 0);
	assert(buf[0] == '\0');
}

static void test_txt_overflow_semantics(void)
{
	char big_buf[128] = {0};
	char small_buf[8] = {0};
	struct nvmeib_txt big_txt = nvmeib_txt_make((struct charvec){.base = big_buf, .len = sizeof(big_buf)});
	struct nvmeib_txt small_txt = nvmeib_txt_make((struct charvec){.base = small_buf, .len = sizeof(small_buf)});
	struct charvec expected;
	struct charvec truncated;
	char const *payload = "abcdefghijklmnopqrstuvwxyz0123456789";

	nvmeib_txt_append(&big_txt, "%s", payload);
	nvmeib_txt_append(&small_txt, "%s", payload);

	expected = nvmeib_txt_finalize(&big_txt);
	truncated = nvmeib_txt_finalize(&small_txt);

	assert(expected.base == big_buf);
	assert(truncated.base == NULL);
	assert(truncated.len == expected.len);
	assert(truncated.len > sizeof(small_buf));
}

static void test_txt_null_buffer(void)
{
	struct nvmeib_txt txt = nvmeib_txt_make((struct charvec){.base = NULL, .len = 0});
	struct charvec out;

	nvmeib_txt_append(&txt, "%s", "null-base");
	out = nvmeib_txt_finalize(&txt);

	assert(out.base == NULL);
	assert(out.len == strlen("null-base"));
}

static void test_jdr_nested_objects_and_arrays(void)
{
	char buf[512] = {0};
	struct charvec buffer = {.base = buf, .len = sizeof(buf)};
	struct jdr jdr = jdr_make(buffer);
	uint32_t answer = 42;
	bool enabled = true;
	char const *name = "nvmesh";
	int32_t items[] = {7, 8, 9};
	struct charvec out;
	char const * const expected_tokens[] = {
		"{\n",
		"\"answer\": 42,\n",
		"\"enabled\": true,\n",
		"\"name\": \"nvmesh\",\n",
		"\"nested\": {\n",
		"\"items\": [\n",
		"7,\n",
		"8,\n",
		"9\n",
		"]\n",
		"}\n",
		"}"
	};

	jdr_write_var(&jdr, answer, answer);
	jdr_write_var(&jdr, enabled, enabled);
	jdr_write_var(&jdr, name, name);
	{
		jdr_object_scope(&jdr, "nested");
		jdr_write_fundamental_array(&jdr, "items", items, sizeof(items) / sizeof(items[0]));
	}

	out = jdr_finalize(&jdr);
	assert(out.base == buf);
	assert(out.len == strlen(out.base));
	assert_tokens_in_order(out.base, expected_tokens, sizeof(expected_tokens) / sizeof(expected_tokens[0]));
}

static void test_jdr_special_writers(void)
{
	char buf[768] = {0};
	struct charvec buffer = {.base = buf, .len = sizeof(buf)};
	struct jdr jdr = jdr_make(buffer);
	uuid_be id = {.b = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
			    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f}};
	struct charvec out;
	char const * const expected_tokens[] = {
		"{\n",
		"\"dynamic_key\": \"dynamic_value\",\n",
		"\"mask\": \"0b101\",\n",
		"\"ratio\": 0.500,\n",
		"\"bad_ratio\": null,\n",
		"\"formatted\": \"v7\",\n",
		"\"id\": \"00010203-0405-0607-0809-0a0b0c0d0e0f\"\n",
		"}"
	};

	jdr_write_key_value_str(&jdr, "dynamic_key", "dynamic_value");
	jdr.ops.bitmap(&jdr, "mask", 5ULL);
	jdr.ops.ascii_float(&jdr, "ratio", 1, 2, 3);
	jdr.ops.ascii_float(&jdr, "bad_ratio", 1, 0, 3);
	jdr.ops.ascii_format(&jdr, "formatted", "v%d", 7);
	jdr_write_var(&jdr, id, id);

	out = jdr_finalize(&jdr);
	assert(out.base == buf);
	assert(out.len == strlen(out.base));
	assert_tokens_in_order(out.base, expected_tokens, sizeof(expected_tokens) / sizeof(expected_tokens[0]));
}

static void test_jdr_special_writers_edge_cases(void)
{
	char buf[768] = {0};
	struct charvec buffer = {.base = buf, .len = sizeof(buf)};
	struct jdr jdr = jdr_make(buffer);
	unsigned long ul_direct = 77UL;
	struct charvec out;
	char const * const expected_tokens[] = {
		"{\n",
		"\"ul_direct\": 77,\n",
		"\"empty_text\": \"\",\n",
		"\"zero_mask\": \"0\",\n",
		"\"one_mask\": \"0b1\",\n",
		"\"anonymous\": [\n",
		"3,\n",
		"\"x5\"\n",
		"]\n",
		"}"
	};

	jdr.ops.ul(&jdr, "ul_direct", ul_direct);
	jdr.ops.ascii(&jdr, "empty_text", NULL);
	jdr.ops.bitmap(&jdr, "zero_mask", 0ULL);
	jdr.ops.bitmap(&jdr, "one_mask", 1ULL);
	{
		jdr_array_scope(&jdr, "anonymous");
		jdr.ops.ascii_float(&jdr, NULL, 7, 2, 0);
		jdr.ops.ascii_format(&jdr, NULL, "x%d", 5);
	}

	out = jdr_finalize(&jdr);
	assert(out.base == buf);
	assert(out.len == strlen(out.base));
	assert_tokens_in_order(out.base, expected_tokens, sizeof(expected_tokens) / sizeof(expected_tokens[0]));
}

static void write_jdr_overflow_payload(struct jdr *jdr)
{
	uint32_t index = 123456789U;
	char const *payload = "abcdefghijklmnopqrstuvwxyz0123456789";

	jdr_write_var(jdr, index, index);
	jdr_write_var(jdr, payload, payload);
	jdr_write_key_value_str(jdr, "dynamic", payload);
}

static void test_jdr_finalize_overflow_semantics(void)
{
	char big_buf[512] = {0};
	char small_buf[16] = {0};
	struct jdr big_jdr = jdr_make((struct charvec){.base = big_buf, .len = sizeof(big_buf)});
	struct jdr small_jdr = jdr_make((struct charvec){.base = small_buf, .len = sizeof(small_buf)});
	struct charvec expected;
	struct charvec truncated;

	write_jdr_overflow_payload(&big_jdr);
	write_jdr_overflow_payload(&small_jdr);

	expected = jdr_finalize(&big_jdr);
	truncated = jdr_finalize(&small_jdr);

	assert(expected.base == big_buf);
	assert(truncated.base == NULL);
	assert(truncated.len == expected.len);
	assert(truncated.len > sizeof(small_buf));
}

static void test_jdr_all_numeric_types(void)
{
	char buf[1024] = {0};
	struct charvec buffer = {.base = buf, .len = sizeof(buf)};
	struct jdr jdr = jdr_make(buffer);
	struct charvec out;
	char const * const expected_tokens[] = {
		"{\n",
		"\"u8_val\": 255,\n",
		"\"s8_val\": -128,\n",
		"\"u16_val\": 65535,\n",
		"\"s16_val\": -32768,\n",
		"\"u32_val\": 4294967295,\n",
		"\"s32_val\": -2147483648,\n",
		"\"u64_val\": 18446744073709551615,\n",
		"\"s64_val\": -9223372036854775808,\n",
		"\"ul_val\": 100,\n",
		"\"ull_val\": 200,\n",
		"\"sll_val\": -300\n",
		"}"
	};

	uint8_t u8_val = 255;
	int8_t s8_val = -128;
	uint16_t u16_val = 65535;
	int16_t s16_val = -32768;
	uint32_t u32_val = 4294967295U;
	int32_t s32_val = -2147483648;
	uint64_t u64_val = 18446744073709551615ULL;
	int64_t s64_val = (-9223372036854775807LL - 1);
	unsigned long ul_val = 100UL;
	unsigned long long ull_val = 200ULL;
	long long sll_val = -300LL;

	jdr_write_var(&jdr, u8_val, u8_val);
	jdr_write_var(&jdr, s8_val, s8_val);
	jdr_write_var(&jdr, u16_val, u16_val);
	jdr_write_var(&jdr, s16_val, s16_val);
	jdr_write_var(&jdr, u32_val, u32_val);
	jdr_write_var(&jdr, s32_val, s32_val);
	jdr_write_var(&jdr, u64_val, u64_val);
	jdr_write_var(&jdr, s64_val, s64_val);
	jdr_write_var(&jdr, ul_val, ul_val);
	jdr_write_var(&jdr, ull_val, ull_val);
	jdr_write_var(&jdr, sll_val, sll_val);

	out = jdr_finalize(&jdr);
	assert(out.base == buf);
	assert(out.len == strlen(out.base));
	assert_tokens_in_order(out.base, expected_tokens, sizeof(expected_tokens) / sizeof(expected_tokens[0]));
}

static void test_jdr_null_and_boolean(void)
{
	char buf[256] = {0};
	struct charvec buffer = {.base = buf, .len = sizeof(buf)};
	struct jdr jdr = jdr_make(buffer);
	struct charvec out;
	char const * const expected_tokens[] = {
		"{\n",
		"\"null_field\": null,\n",
		"\"true_val\": true,\n",
		"\"false_val\": false\n",
		"}"
	};

	struct jdr_null_type null_val = {0};
	bool true_val = true;
	bool false_val = false;

	jdr.ops.null(&jdr, "null_field", null_val);
	jdr_write_var(&jdr, true_val, true_val);
	jdr_write_var(&jdr, false_val, false_val);

	out = jdr_finalize(&jdr);
	assert(out.base == buf);
	assert(out.len == strlen(out.base));
	assert_tokens_in_order(out.base, expected_tokens, sizeof(expected_tokens) / sizeof(expected_tokens[0]));
}

static void test_jdr_nested_arrays(void)
{
	char buf[512] = {0};
	struct charvec buffer = {.base = buf, .len = sizeof(buf)};
	struct jdr jdr = jdr_make(buffer);
	uint32_t arr1[] = {1, 2, 3};
	uint32_t arr2[] = {4, 5};
	struct charvec out;
	char const * const expected_tokens[] = {
		"{\n",
		"\"outer\": [\n",
		"1,\n",
		"2,\n",
		"3\n",
		"],\n",
		"\"nested_obj\": {\n",
		"\"inner\": [\n",
		"4,\n",
		"5\n",
		"]\n",
		"}\n",
		"}"
	};

	{
		jdr_array_scope(&jdr, "outer");
		size_t i;
		for (i = 0; i < sizeof(arr1) / sizeof(arr1[0]); ++i) {
			jdr_write_var(&jdr, arr1[i], arr1[i]);
		}
	}
	{
		jdr_object_scope(&jdr, "nested_obj");
		jdr_write_fundamental_array(&jdr, "inner", arr2, sizeof(arr2) / sizeof(arr2[0]));
	}

	out = jdr_finalize(&jdr);
	assert(out.base == buf);
	assert(out.len == strlen(out.base));
	assert_tokens_in_order(out.base, expected_tokens, sizeof(expected_tokens) / sizeof(expected_tokens[0]));
}

static void test_jdr_bitmap_array(void)
{
	char buf[512] = {0};
	struct charvec buffer = {.base = buf, .len = sizeof(buf)};
	struct jdr jdr = jdr_make(buffer);
	unsigned long long bitmaps[] = {0b101, 0b110, 0b111};
	struct charvec out;
	char const * const expected_tokens[] = {
		"{\n",
		"\"masks\": [\n",
		"\"0b101\",\n",
		"\"0b110\",\n",
		"\"0b111\"\n",
		"]\n",
		"}"
	};

	{
		jdr_array_scope(&jdr, "masks");
		size_t i;
		for (i = 0; i < sizeof(bitmaps) / sizeof(bitmaps[0]); ++i) {
			jdr.ops.bitmap(&jdr, NULL, bitmaps[i]);
		}
	}

	out = jdr_finalize(&jdr);
	assert(out.base == buf);
	assert(out.len == strlen(out.base));
	assert_tokens_in_order(out.base, expected_tokens, sizeof(expected_tokens) / sizeof(expected_tokens[0]));
}

static void test_jdr_empty_structures(void)
{
	char buf[256] = {0};
	struct charvec buffer = {.base = buf, .len = sizeof(buf)};
	struct jdr jdr = jdr_make(buffer);
	struct charvec out;
	char const * const expected_tokens[] = {
		"{\n",
		"\"empty_obj\": {\n",
		"},\n",
		"\"empty_arr\": [\n",
		"]\n",
		"}"
	};

	{
		jdr_object_scope(&jdr, "empty_obj");
	}
	{
		jdr_array_scope(&jdr, "empty_arr");
	}

	out = jdr_finalize(&jdr);
	assert(out.base == buf);
	assert(out.len == strlen(out.base));
	assert_tokens_in_order(out.base, expected_tokens, sizeof(expected_tokens) / sizeof(expected_tokens[0]));
}

static void test_jdr_ptr_type(void)
{
	char buf[256] = {0};
	struct charvec buffer = {.base = buf, .len = sizeof(buf)};
	struct jdr jdr = jdr_make(buffer);
	int value = 42;
	int *ptr = &value;
	void *null_ptr = NULL;
	struct charvec out;

	jdr.ops.ptr(&jdr, "ptr", ptr);
	jdr.ops.ptr(&jdr, "null_ptr", null_ptr);

	out = jdr_finalize(&jdr);
	assert(out.base == buf);
	assert(out.len == strlen(out.base));
	assert(strstr(out.base, "\"ptr\": \"") != NULL);
	/* NULL pointer format varies by platform: could be "0x0", "(nil)", etc. */
	assert(strstr(out.base, "\"null_ptr\": \"") != NULL);
}

static void test_jdr_deeply_nested(void)
{
	char buf[512] = {0};
	struct charvec buffer = {.base = buf, .len = sizeof(buf)};
	struct jdr jdr = jdr_make(buffer);
	uint32_t val = 999;
	struct charvec out;
	char const * const expected_tokens[] = {
		"{\n",
		"\"level1\": {\n",
		"\"level2\": {\n",
		"\"level3\": {\n",
		"\"value\": 999\n",
		"}\n",
		"}\n",
		"}\n",
		"}"
	};

	{
		jdr_object_scope(&jdr, "level1");
		{
			jdr_object_scope(&jdr, "level2");
			{
				jdr_object_scope(&jdr, "level3");
				jdr_write_var(&jdr, value, val);
			}
		}
	}

	out = jdr_finalize(&jdr);
	assert(out.base == buf);
	assert(out.len == strlen(out.base));
	assert_tokens_in_order(out.base, expected_tokens, sizeof(expected_tokens) / sizeof(expected_tokens[0]));
}

static void test_jdr_multiple_arrays(void)
{
	char buf[512] = {0};
	struct charvec buffer = {.base = buf, .len = sizeof(buf)};
	struct jdr jdr = jdr_make(buffer);
	uint8_t small[] = {1, 2};
	uint32_t medium[] = {10, 20, 30};
	uint64_t large[] = {100, 200, 300, 400};
	struct charvec out;
	char const * const expected_tokens[] = {
		"{\n",
		"\"small\": [\n",
		"1,\n",
		"2\n",
		"],\n",
		"\"medium\": [\n",
		"10,\n",
		"20,\n",
		"30\n",
		"],\n",
		"\"large\": [\n",
		"100,\n",
		"200,\n",
		"300,\n",
		"400\n",
		"]\n",
		"}"
	};

	jdr_write_fundamental_array(&jdr, "small", small, sizeof(small) / sizeof(small[0]));
	jdr_write_fundamental_array(&jdr, "medium", medium, sizeof(medium) / sizeof(medium[0]));
	jdr_write_fundamental_array(&jdr, "large", large, sizeof(large) / sizeof(large[0]));

	out = jdr_finalize(&jdr);
	assert(out.base == buf);
	assert(out.len == strlen(out.base));
	assert_tokens_in_order(out.base, expected_tokens, sizeof(expected_tokens) / sizeof(expected_tokens[0]));
}

static void test_nvmeib_write_file(void)
{
	char const *path = "nvmeib_jdr_write_file_test.tmp";
	char const payload[] = "nvmeib-write-file-payload";
	char read_back[sizeof(payload)] = {0};
	static char full_payload[1 << 20];
	size_t payload_len = sizeof(payload) - 1;
	FILE *file;
	size_t read_len;
	int rc;

	rc = nvmeib_write_file(path, payload, payload_len);
	assert(rc == 0);

	file = fopen(path, "r");
	assert(file != NULL);
	read_len = fread(read_back, 1, payload_len, file);
	assert(fclose(file) == 0);
	assert(read_len == payload_len);
	read_back[read_len] = '\0';
	assert(strcmp(read_back, payload) == 0);
	assert(remove(path) == 0);

	memset(full_payload, 'x', sizeof(full_payload));
	rc = nvmeib_write_file("/dev/full", full_payload, sizeof(full_payload));
	assert(rc != 0);

	rc = nvmeib_write_file("/definitely/nonexistent/nvmeib_jdr_test.tmp", payload, payload_len);
	assert(rc != 0);
}

int main(void) {
	test_txt();
	test_txt_format_specifiers();
	test_txt_finalize();
	test_txt_empty();
	test_txt_overflow_semantics();
	test_txt_null_buffer();
	test_jdr_nested_objects_and_arrays();
	test_jdr_special_writers();
	test_jdr_special_writers_edge_cases();
	test_jdr_finalize_overflow_semantics();
	test_jdr_all_numeric_types();
	test_jdr_null_and_boolean();
	test_jdr_nested_arrays();
	test_jdr_bitmap_array();
	test_jdr_empty_structures();
	test_jdr_ptr_type();
	test_jdr_deeply_nested();
	test_jdr_multiple_arrays();
	test_nvmeib_write_file();

	return 0;
}
