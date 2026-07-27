#ifndef __FLOG_H__
#define __FLOG_H__

#include <linux/kernel.h>

#ifdef UM_APP
#define nflog(name, fmt, ...) ({ NVMEIB_LOG_GOODPATH(fmt, _DBG, tracer_nvmeshum_dp, name, ##__VA_ARGS__); })
#else
#define nflog(name, fmt, ...) ({ NVMEIB_LOG_GOODPATH(fmt, _DBG, /*Deafult*/, name, ##__VA_ARGS__); })
#endif

#endif /* __FLOG_H__ */
