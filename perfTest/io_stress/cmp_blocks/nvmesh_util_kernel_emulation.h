#ifndef KR_INCS_H
#define KR_INCS_H

//user space 3rd party kernel simulator
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>	// free

#ifndef NVASSERT_H_INCLUDED	// UM_APP, nvassert.h
	#define BUG_ON(condition)	do { const int hit__ = !!(condition); if (hit__) {fprintf(stderr, "************************** BUG!!!! at %s, %s() line %d, val=%d, condition=%s\n", __FILE__, __FUNCTION__, __LINE__, hit__, #condition); /*raise(SIGABRT);*/} } while(0)
	#define WARN(condition, format, ...) ({ if (condition) {fprintf(stderr, format, ##__VA_ARGS__); BUG_ON(condition);} })
#endif
#include "../../../common/compat/kr_incs_asserts.h"
#include "../../../common/compat/kr_incs_malloc.h"
#include "../../../common/compat/kr_incs_sched.h"
#include "../../../common/nvmeib_math.h"
#include "../../../common/compat/kr_incs_bit_ops.h"
#include "../../../common/compat/kr_incs_crc32.h"
#include "../../../common/compat/kr_incs_time.h"
#include "../../../common/nvmeib_str.h"

#define for_each_possible_cpu(i)  for (i=0; i<NR_CPUS; i++)
#ifndef pr_info
	#define pr_info(fmt, ...)   fprintf(stderr, fmt, ##__VA_ARGS__)
	#define pr_debug(fmt, ...)  fprintf(stderr, "***** " fmt, ##__VA_ARGS__)
#endif

#define DEFINE_PER_CPU(		   type, name) type name[1];
#define DEFINE_PER_CPU_ALIGNED(type, name) type name[1];

#define local_irq_save(f) f = 1
#define local_irq_restore(f)

#define this_cpu_ptr(var) ({typeof(**var) * rv = &((*var)[0]); rv; })
#define CONFIG_NR_CPUS (1)
#define NR_CPUS CONFIG_NR_CPUS
#define smp_processor_id() 0

#define NVMEIBC_SECTOR_SIZE	(1 << NVMEIBC_SECTOR_SHIFT)

// Prevent unneded includes of debug di injection. We need only debug di parsing
#ifndef NVMEIBC_DP_DBGDI_H
	#define NVMEIBC_DP_DBGDI_H
	#define DEBUG_DI_SIZE_CALC(is_di_debug)  (is_di_debug ? 512 : NVMEIBC_SECTOR_SIZE)
	#define dp_dbgdi_clear_restored_block_history(d)
#endif
typedef struct { int c; } atomic_t;
__attribute__((unused)) static int atomic_inc_return(atomic_t *v) { return __atomic_add_fetch(&v->c, 1, __ATOMIC_SEQ_CST); }

static inline void print_args(const int argc, const char *argv[]) {
	int i;
	fprintf(stderr, "%d[args]: ",argc);
	for (i = 0; i < argc; i++) {
		fprintf(stderr, "|%s| ", argv[i]);
	}
	fprintf(stderr,"\n");
}

#define stop_on_error(fmt, ...)  ({ fprintf(stderr, "Line=%d: " fmt, __LINE__, ##__VA_ARGS__); rv = -__LINE__; goto _out; })
int util_initialize_gf_layer(void);
#endif /*KR_INCS_H*/


