#pragma once

#define NVMEIB_EVENT(x) x,

enum nvmeib_event_type {
#include "nvmeib_event.hxx"
};

#undef NVMEIB_EVENT

const char *nvmeib_event_type_to_string(enum nvmeib_event_type type);

#undef NVMEIB_EVENT

#define NVMEIB_EVENT(x) \
	static inline const char * EV_##x(void) { return nvmeib_event_type_to_string(x);}

#include "nvmeib_event.hxx"

#undef NVMEIB_EVENT
