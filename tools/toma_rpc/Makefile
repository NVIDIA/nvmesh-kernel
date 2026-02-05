.SUFFIXES:
.PHONY: clean all rebuild
.DEFAULT_GOAL := all

CFLAGS   := -Wall -ggdb -O3 -Wno-strict-aliasing
INCLUDES := -I.
LDFLAGS  :=
LDSTATIC :=
LDDYN    :=

has_pie := $(shell gcc -v 2>&1 | grep pie > /dev/null && echo 1 || echo 0)
ifeq ($(has_pie),1)
  LDSTATIC += -no-pie
  LDFLAGS += -no-pie
endif

TOOL_OBJ := \
	toma_rpc.o

ifneq ($(LLVM),)
  CFLAGS += -Wno-format-security -Wtype-safety
  autocompdb = -MJ $@.json
else
  CC=gcc
endif

COMPILE     := $(CC) $(CFLAGS) $(INCLUDES) -MD -MP -c

LINK        := $(CC) $(CFLAGS) $(LDFLAGS)

ifeq ($(TOOL_DYNAMIC_LINK),1)
  DO_LINK = $(LDDYN)
else
  DO_LINK = $(LDSTATIC)
endif

%.o: %.c Makefile
	@echo '>>>>>' $@
	@$(COMPILE) $(shell readlink -m $<) $(autocompdb) -o $@

-include $(TOOL_OBJ:.o=.d)

toma_rpc: $(TOOL_OBJ)
	@echo '>>>>>' $@
	$(LINK) $(TOOL_OBJ) $(DO_LINK) -o $@ -ldl

all: toma_rpc

clean:
	@rm -f $(TOOL_OBJ)
	@rm -f $(TOOL_OBJ:.o=.d)
	@rm -f toma_rpc
	@rm -f *.[od]

rebuild: clean
	$(MAKE)

