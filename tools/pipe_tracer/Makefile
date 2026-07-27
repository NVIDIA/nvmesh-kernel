.DEFAULT_GOAL := all
.PHONY: clean

TRACE = @echo '>>>' $@;
#V =

PROJ_ROOT  := $(realpath ../..)
BUILDDIR   := $(realpath .)/build
OBJDIR     := $(BUILDDIR)/obj
DEPDIR     := $(BUILDDIR)/dep
BINDIR     := $(BUILDDIR)/bin
IDIR       := $(BUILDDIR)/i
AUTOGENDIR := $(BUILDDIR)/autogen/


TRACE_NS   := nvmeib_pipe
TRACE_JSON := $(BUILDDIR)/$(TRACE_NS).trace.json
GEN_EVENTS := $(AUTOGENDIR)/includes/gen_events.h
TRACER_C := $(AUTOGENDIR)/includes/traces_ids.c
TRACER_O := $(AUTOGENDIR)/includes/traces_ids.o
TRACER_PP  := $(PROJ_ROOT)/tools/pre_processor/tracer_pp.py
GEN_PROBES := $(PROJ_ROOT)/tools/pre_processor/gen_probes2.py
MERGE_DICT := $(PROJ_ROOT)/tools/merge_dictionary.sh
DICT_JSON  := $(PROJ_ROOT)/tools/dictionary.json

LLVM       ?= no

CC := gcc
LD := gcc

ifeq ($(LLVM), $(filter $(LLVM),true yes))
  CC := clang
  LD := clang
  LLVM_VFLAGS := -DLLVM
endif

CFLAGS  := -ggdb3 -Wall -Werror -std=gnu89 -DUSER_SPACE_TRACING=1 -DUSE_EXTERN_TIMESTAMP=1 $(LLVM_VFLAGS)
LDFLAGS := -pthread -ldl
IFLGAS  := -I$(PROJ_ROOT)/tools -I$(AUTOGENDIR)/includes -I$(PROJ_ROOT)

ifeq ($(DEBUG), $(filter $(DEBUG), yes true 1))
  CFLAGS += -DPIPE_TRACER_DEBUG=1
endif

define objfile
$(patsubst %.cpp,$(OBJDIR)/%.o,$(patsubst %.c,$(OBJDIR)/%.o,$1))
endef

define depfile
$(patsubst %.cpp,$(DEPDIR)/%.d,$(patsubst %.c,$(DEPDIR)/%.d,$1))
endef

define ifile
$(patsubst %.cpp,$(IDIR)/%.i,$(patsubst %.c,$(IDIR)/%.i,$1))
endef

DST := $(BINDIR)/nvmeib_pipe_tracer
SRC := \
	nvmeib_pipe_tracer.c \
	$(PROJ_ROOT)/tools/nvmeib_trace_userspace.c \
	$(PROJ_ROOT)/tools/nvmeib_trace_userspace_poller.c

OBJ := $(call objfile,$(SRC))
DEP := $(call depfile,$(SRC))
I   := $(call ifile,$(SRC))

-include $(DEP)

all: $(DST)

$(DST): $(OBJ) $(TRACER_O)
	$(TRACE) \
	mkdir -p $(dir $@) && \
	$(LD) $^ -o "$@" $(LDFLAGS)

$(OBJDIR)/%.o: %.c $(GEN_EVENTS) Makefile
	$(TRACE) \
	mkdir -p $(dir $@) && \
	mkdir -p $(dir $(call depfile,$<)) && \
	$(CC) $(IFLGAS) $(CFLAGS) -MMD -MP -MF "$(call depfile,$<)" -c "$(realpath $<)" -o "$@"

$(IDIR)/%.i: %.c Makefile
	$(TRACE) \
	mkdir -p $(dir $@) && \
	$(CC) $(IFLGAS) $(CFLAGS) -E -D__FIRST_PASS__ -D"__attribute__(x)=" -D_NVMEIB_TRACE_BACKEND_USER -o $@ "$(realpath $<)"

$(GEN_EVENTS): $(TRACE_JSON) $(GEN_PROBES) Makefile
	$(TRACE) \
	mkdir -p $(dir $@) && \
	cd $(dir $@) && \
	$(GEN_PROBES) $(TRACE_JSON) $(TRACE_NS) --userspace -o "$@"

$(TRACE_JSON): $(I) $(MERGE_DICT) $(DICT_JSON) $(TRACER_PP) Makefile
	$(TRACE) \
	mkdir -p $(dir $@) && \
	$(TRACER_PP) "$@" $(I) && \
	$(MERGE_DICT) "$@" "$(DICT_JSON)"

$(TRACER_O): $(TRACER_C)
	$(CC) $(IFLGAS) $(CFLAGS) -c -o $@ $<

$(TRACER_C): $(GEN_EVENTS)

clean:
	$(TRACE)rm -rf "$(BUILDDIR)"
