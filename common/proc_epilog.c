#include "common/proc_epilog.h"
#include "utils/nvmeib_jdr/nvmeib_jdr.h"
#include "utils/nvmeib_jdr/nvmeib_txt.h"

static ssize_t __get_time_of_day(char *time_str, size_t size)
{
	struct timeval tv;
	struct rtc_time tm;
	do_gettimeofday(&tv);
	rtc_time_to_tm((tv.tv_sec - (sys_tz.tz_minuteswest * 60)), &tm);		// Yuri Nudelman: EC-5432, code of (-time_zone) works only in user space. In kernel tz_minuteswest==0 so we will not get local machine time but rather UTC.
	return scnprintf(time_str, size, "%04d-%02d-%02d %02d:%02d:%02d.%03ld",
			 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
			 tm.tm_hour, tm.tm_min, tm.tm_sec,
			 tv.tv_usec / 1000);
}

ssize_t nvmeib_add_proc_status_footer(char *buffer, size_t len, const char *title)
{
	#define __PROC_TIME_FMT "%02d/%02d/%04d %02d:%02d:%02d"
	struct timeval tv;
	struct rtc_time tm;
	do_gettimeofday(&tv);
	rtc_time_to_tm((tv.tv_sec - (sys_tz.tz_minuteswest * 60)), &tm);		// Yuri Nudelman: EC-5432, code of (-time_zone) works only in user space. In kernel tz_minuteswest==0 so we will not get local machine time but rather UTC.
	return scnprintf(buffer, len, "HOSTNAME: '%s', PROC: '%s', TAKEN_ON: '" __PROC_TIME_FMT "(UTC)'\n", utsname()->nodename, title, tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec);
}
EXPORT_SYMBOL(nvmeib_add_proc_status_footer);


#define BUF_ADD(...) len += scnprintf(buf+len, buf_len-len, __VA_ARGS__)
ssize_t nvmeib_proc_add_json_proc_epilog(int version, char *buf, size_t buf_len)
{
	ssize_t len = 0;
	BUF_ADD(",\n\"format_version\": %d,\n\"time\": \"", version);
	len += __get_time_of_day(buf + len, buf_len - len);
	BUF_ADD("\"");
	return len;
}
EXPORT_SYMBOL(nvmeib_proc_add_json_proc_epilog);

ssize_t nvmeib_proc_add_txt_proc_epilog(int version, char *buf, size_t buf_len)
{
	ssize_t len = 0;
	BUF_ADD("format_version: %d\ntime: ", version);
	len += __get_time_of_day(buf + len, buf_len - len);
	BUF_ADD("\n");
	return len;
}
EXPORT_SYMBOL(nvmeib_proc_add_txt_proc_epilog);

ssize_t nvmeib_proc_add_yaml_proc_epilog(int version, char *buf, size_t buf_len)
{
	ssize_t len = 0;
	BUF_ADD("format_version: %d\n", version);
	BUF_ADD("time: ");
	len += __get_time_of_day(buf + len, buf_len - len);
	BUF_ADD("\n");
	return len;
}
EXPORT_SYMBOL(nvmeib_proc_add_yaml_proc_epilog);

ssize_t nvmeib_proc_add_smart_proc_epilog(int version, char *buf, size_t buf_len)
{
	ssize_t len = 0;
	BUF_ADD("format_version= %d\ntime= ", version);
	len += __get_time_of_day(buf + len, buf_len - len);
	BUF_ADD("\n");
	return len;
}
EXPORT_SYMBOL(nvmeib_proc_add_smart_proc_epilog);

void nvmeib_proc_add_jdr_proc_epilog(int version, struct jdr *jdr)
{
	jdr_write_var(jdr, format_version, version);
}
EXPORT_SYMBOL(nvmeib_proc_add_jdr_proc_epilog);

#define __PROC_TIME_STR_LEN 23
void nvmeib_proc_add_json_proc_epilog_jdr(int version, struct jdr *jdr)
{
	char time_str[__PROC_TIME_STR_LEN + 1] = {0};
	jdr_write_var(jdr, format_version, version);
	__get_time_of_day(time_str, sizeof(time_str));
	jdr_write_var(jdr, time, (const	char *)time_str);
}
EXPORT_SYMBOL(nvmeib_proc_add_json_proc_epilog_jdr);

void nvmeib_proc_add_txt_proc_epilog_txt(int version, struct nvmeib_txt *txt)
{
	char time_str[__PROC_TIME_STR_LEN + 1] = {0};
	nvmeib_txt_append(txt, "format_version: %d\n", version);
	nvmeib_txt_append(txt, "time: ");
	__get_time_of_day(time_str, sizeof(time_str));
	nvmeib_txt_append(txt, "%s\n", time_str);
}
EXPORT_SYMBOL(nvmeib_proc_add_txt_proc_epilog_txt);
#undef BUF_ADD

