// Sandbox internal utilities
//
// To be used by sandbox implementation code.

#ifndef TOMA_SANDBOX_UTIL_H
#define TOMA_SANDBOX_UTIL_H

#include <stdio.h>

#define SANDBOX_PRINT(fmt, ...)      fprintf(stderr, "SANDBOX: " fmt, __VA_ARGS__)      // Todo: Remove me, use binary tracing
#define SANDBOX_PRINT_TMP(fmt, ...)  fprintf(stderr, "SANDBOX: " COL_PURPL fmt COL_RESET, __VA_ARGS__)
#define N_SANDBOX(name, fmt, ...) _NMIRROR_LOGLEVEL(IMf, LOG_DEBUG, name, NVMEIB_LOG_ETERNAL, "SANDBOX: ", fmt, ## __VA_ARGS__)

#define BUG_ON(condition)	do { const int hit__ = !!(condition); if (hit__) {fprintf(stderr, "************************** BUG!!!! at %s:%d - %s(), val=%d, condition=%s\n", __FILE__, __LINE__, __FUNCTION__, hit__, #condition); raise(SIGABRT);} } while(0)
//#define WARN(condition, fmt, ...) 	do { const int hit = !!(condition); if (hit) {/*dump_stack(); */SANDBOX_PRINT("************************** BUG!!!! at %s() line %d, val=%d, condition=%s\n", __FUNCTION__, __LINE__, hit, #condition); raise(SIGABRT);} } while(0)

#endif
