.SUFFIXES:
.PHONY: clean all
.DEFAULT_GOAL := all
SSDA?=../../..
CFLAGS  := \
	-ggdb3 -Wall -Werror -fPIC \
	-DNVMEIBC_SECTOR_SHIFT=12 \
	-DUSER_SPACE \
	-Wno-address-of-packed-member -Wextra -Wshadow \
	-O3

IFLAGS  := \
	-I$(SSDA)/ \
	-I$(SSDA)/clnt/ \
	-I$(SSDA)/srv/ \
	-I$(SSDA)/common/ \
	-I$(SSDA)/common_public/ \
	-I$(SSDA)/toma/ \
	-I$(SSDA)/autogen/common/ \
	-I$(SSDA)/autogen/clnt/

ifeq ($(LLVM),)
CC      := gcc
else
autocompdb = -MJ $@.json
endif
TRACE    = @echo '>' $@ ;
DEPEND   = -MT $*.o -MMD -MP -MF $*.d

OBJ := scan_locks_ec.o
DEP := $(OBJ:.o=.d)
EXE := scan_locks_ec

-include $(DEP)

all: $(EXE)

$(EXE): $(OBJ)
	$(TRACE)$(CC) $(OBJ) -o $@

%.o: %.c Makefile
	$(TRACE)$(CC) $(CFLAGS) $(IFLAGS) $(DEPEND) $(autocompdb) -o $@ -c $<

clean:
	$(TRACE)rm -rf $(EXE) $(OBJ) $(DEP)
