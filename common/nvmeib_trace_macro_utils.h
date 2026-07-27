#ifndef NVMEIB_TRACE_MACRO_UTILS
#define NVMEIB_TRACE_MACRO_UTILS

#include "nvmeib_macro_utils.h"

/** Macros infrastructure to define all channels configuration in one place */
#define render_ch_enum(...) NVMEIB_FOREACH(_render_ch_enum, (__VA_ARGS__))
#define _render_ch_enum(x) __render_ch_enum x
#define __render_ch_enum(_name, ...) _name##_chid,

#define render_ch_names(...) NVMEIB_FOREACH(_render_ch_names, (__VA_ARGS__))
#define _render_ch_names(x) __render_ch_names x
#define __render_ch_names(_name, ...) NVMEIB_STRINGIFY1(_name),

#define render_ch_pcpu_vars(...)                                               \
	NVMEIB_FOREACH(_render_ch_pcpu_vars, (__VA_ARGS__))
#define _render_ch_pcpu_vars(x) __render_ch_pcpu_vars x
#define __render_ch_pcpu_vars(_name, ...) &_name##_percpu,

#define render_per_cpu_export(...)                                             \
	NVMEIB_FOREACH(_render_per_cpu_export, (__VA_ARGS__));
#define _render_per_cpu_export(x) __render_per_cpu_export x
#define __render_per_cpu_export(_name, ...)                                    \
	DEFINE_PER_CPU(struct nvmeib_capuch *, _name##_percpu);                    \
	EXPORT_PER_CPU_SYMBOL(_name##_percpu);

#define render_per_cpu_declare(...)                                             \
	NVMEIB_FOREACH(_render_per_cpu_declare, (__VA_ARGS__));
#define _render_per_cpu_declare(x) __render_per_cpu_declare x
#define __render_per_cpu_declare(_name, ...)                                    \
	DECLARE_PER_CPU(struct nvmeib_capuch *, _name##_percpu);                    \

#define NVMEIB_OPEN_BRACKETS(...) __VA_ARGS__

#endif /* NVMEIB_TRACE_MACRO_UTILS */