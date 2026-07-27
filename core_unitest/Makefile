# Main makefile for core layer test suite kernel communication layer
# build target is library corecomm (aka libcorecomm.so.0.1)

CC          := gcc
LD          := gcc
CFLAGS      := -fPIC -ggdb3 -Wall -Werror -std=gnu89
LDFLAGS     := -pthread
IFLAGS      :=
DEPEND       = -MT $*.o -MMD -MP -MF $*.d # Must be = not :=
VV           = @echo '>' "$@" ; # Must be = not :=

# Libra name
NAME := corecomm

# Final output file name
LIBSO  = lib$(NAME).so



OBJ := corecomm_netlink.o
OBJ := $(abspath $(OBJ)) # Convert objects to abspath, just for better IDE integration
DEP := $(OBJ:.o=.d)

-include $(DEP)

# By default build dynamic library. Maybe in the future add static version.
.DEFAULT_GOAL := so

all: so testclnt

so: $(LIBSO)

testclnt: corecomm_testclnt.o
	$(VV)$(LD) $< -o $@ -L. -ldl ./$(LIBSO) $(LDFLAGS)

%.o: %.c Makefile
	$(VV)$(CC) $(CFLAGS) $(IFLAGS) $(DEPEND) -o $@ -c $<

$(LIBSO): $(OBJ)
	$(VV)$(LD) -shared $(LDFLAGS) $< -o $@

clean:
	$(VV)$(RM) $(LIBSO) $(OBJ) $(DEP) testclnt corecomm_testclnt.o