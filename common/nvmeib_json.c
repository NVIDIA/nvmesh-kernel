#include "nvmeib_json.h"

#define BUF_ADD(...) count += scnprintf(buf+count, len-count, __VA_ARGS__)

static ssize_t json_indent(char *buf, size_t len, size_t indent)
{
    size_t i;
    ssize_t count = 0;
    for (i = 0; i < indent; i++) BUF_ADD("\t");
    return count;
}

static ssize_t json_start_obj(char *buf, size_t len, const char *name, size_t indent)
{
    ssize_t count = 0;
    count += json_indent(buf + count, len - count, indent);
    if (name) {
        BUF_ADD("\"%s\" : {\n", name);
    } else {
        BUF_ADD("{\n");
    }
    return count;
}

#define __ELEM_CHAIN (is_last ? "" : ",")
static ssize_t json_end_obj(char *buf, size_t len, int is_last, size_t indent)
{
    ssize_t count = 0;
    count += json_indent(buf + count, len - count, indent);
    BUF_ADD("}%s\n", __ELEM_CHAIN);
    return count;
}

static ssize_t json_data_str(char *buf, size_t len,
                            const char *name, const char *val,
                            int is_last, size_t indent)
{
    ssize_t count = 0;
    count += json_indent(buf + count, len - count, indent);
    BUF_ADD("\"%s\" : \"%s\"%s\n", name, val, __ELEM_CHAIN);
    return count;
}

static ssize_t json_data_bool(char *buf, size_t len,
                            const char *name, bool val,
                            int is_last, size_t indent)
{
    ssize_t count = 0;
    count += json_indent(buf + count, len - count, indent);
    BUF_ADD("\"%s\" : %s%s\n", name, val ? "true" : "false", __ELEM_CHAIN);
    return count;
}

static ssize_t json_data_uval(char *buf, size_t len,
                            const char *name, u64 val,
                            int is_last, size_t indent)
{
    ssize_t count = 0;
    count += json_indent(buf + count, len - count, indent);
    BUF_ADD("\"%s\" : %llu%s\n", name, val, __ELEM_CHAIN);
    return count;
}

static ssize_t json_data_uval_float(char *buf, size_t len,
                                const char *name, const u64 numerator,
				const u64 denominator, const int precision,
                                const int is_last, size_t indent)
{
	u64 int_part;
	u64 remainder;
	ssize_t count = 0;
	int i;

	count += json_indent(buf + count, len - count, indent);

	/* Add quoted name and separator ':' */
	BUF_ADD("\"%s\" : ", name);

	if (denominator == 0) {
		/* Invalid division - output null as value */
		BUF_ADD("null");
		goto suffix;
	}

	int_part = numerator / denominator;
	remainder = numerator % denominator;

	/* Print the integer part */
	BUF_ADD("%llu", int_part);

	if (precision > 0) {
		BUF_ADD(".");
		for (i = 0; i < precision; i++) {
			remainder *= 10;
			BUF_ADD("%d", (int)(remainder / denominator));
			remainder = remainder % denominator;
		}
	}

suffix:
	/* Add ",\n' or "\n" depending on is_last */
	BUF_ADD("%s\n", __ELEM_CHAIN);
	return count;
}

static ssize_t json_data_sval(char *buf, size_t len,
                            const char *name, s64 val,
                            int is_last, size_t indent)
{
    ssize_t count = 0;
    count += json_indent(buf + count, len - count, indent);
    BUF_ADD("\"%s\" : %lld%s\n", name, val, __ELEM_CHAIN);
    return count;
}

static ssize_t json_start_array(char *buf, size_t len, const char *name, size_t indent)
{
    ssize_t count = 0;
    count += json_indent(buf + count, len - count, indent);
    if (name) {
        BUF_ADD("\"%s\" : [\n", name);
    } else {
        BUF_ADD("[\n");
    }
    return count;
}

static ssize_t json_end_array(char *buf, size_t len, int is_last, size_t indent)
{
    ssize_t count = 0;
    count += json_indent(buf + count, len - count, indent);
    BUF_ADD("]%s\n", __ELEM_CHAIN);
    return count;
}

static ssize_t json_data_sprintf(char *buf, size_t len,
			     int is_last, size_t indent,
			     const char *name,
			     const char *fmt, ...)
{
	va_list args;
	ssize_t count = 0;
	count += json_indent(buf + count, len - count, indent);

	BUF_ADD("\"%s\" : \"", name);

	va_start(args, fmt);
	count += vscnprintf(buf + count, len - count, fmt, args); 
	va_end(args);
	BUF_ADD("\"%s\n", __ELEM_CHAIN);
	return count;
}

bool json_iostats_fixed_size = false;    // By default IO stats jsons to have fixed size for easier readability
module_param(json_iostats_fixed_size, bool, 0644);
MODULE_PARM_DESC(json_iostats_fixed_size, "Pad I/O statistics in JSON representation to a fixed size");

static ssize_t right_padd(char *buf, size_t len, ssize_t fixed)
{
    ssize_t count = 0;
    if (json_iostats_fixed_size) {
        const int padd = (int)min((ssize_t)(len - count - 1), fixed);
        if (padd > 0) {
            memset(buf, ' ', padd);
            buf[padd] = 0;
            count = padd;
        }
    }
    return count;
}

const struct nvmeib_json_ops nvmeib_json_ops = {
    .indent = json_indent,
    .start_obj = json_start_obj,
    .end_obj = json_end_obj,
    .start_array = json_start_array,
    .end_array = json_end_array,
    .data_str = json_data_str,
    .data_uval = json_data_uval,
    .data_uval_float = json_data_uval_float,
    .data_sval = json_data_sval,
    .right_padd = right_padd,
    .data_bool = json_data_bool,
    .data_sprintf = json_data_sprintf,
};
EXPORT_SYMBOL(nvmeib_json_ops);
