# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0

# Make files tutorial: http://www.cs.swarthmore.edu/~newhall/unixhelp/howto_makefiles.html
# ********** Define compiler and compilation flags *********
USE_CPP =0

ARCH := $(shell uname -m | sed -e s/i.86/x86/ \
                                  -e s/sun4u/sparc64/ \
                                  -e s/arm.*/arm/ -e s/sa110/arm/ \
                                  -e s/s390x/s390/ -e s/parisc64/parisc/ \
                                  -e s/ppc.*/powerpc/ -e s/mips.*/mips/ \
                                  -e s/sh[234].*/sh/ -e s/aarch64.*/arm64/ )

ifeq ($(ARCH),x86_64)
        CFLAGS = -m64
        USE_64_BIT=1
else ifeq ($(ARCH),arm64)
        USE_ARM=1
        USE_64_BIT=1
        CFLAGS = -march=armv8.1-a+crc+fp+simd
endif

ifeq ($(shell uname),Darwin)
        USE_DARWIN=1
endif

$(info -->ec)

USE_RELEASE=0#Compile debug or release
CFLAGS += -Wall -Werror -std=gnu99 -Wno-deprecated -Wdeclaration-after-statement #Simulate kernel compilation & enable all warning. Can use gnu89 instead of 99
CFLAGS += -Wextra -Wshadow -Wno-old-style-declaration -Werror=strict-aliasing # Location of inline in function declaration
#CFLAGS += -Wformat=0 -fpermissive -Wno-conversion-null
ifeq ($(USE_CPP),1)
	CC=g++#Compile as C++
else
	CC=gcc#Compile as C
endif

ifeq ($(USE_RELEASE),1)
	CFLAGS += -O4
else
	CFLAGS += -g3 -ggdb3
endif

# ********** Included directories of H files *********
# Main SSDA project directory. (relative to this Makefile)
SSDA=../../..
INCLUDES :=
INCLUDES += -I$(SSDA)/clnt/block/unitest -I$(SSDA)/clnt/ -I$(SSDA)/srv/ -I$(SSDA)/common/ -I$(SSDA)/common_public/ -I$(SSDA)/toma/ -I$(SSDA)/autogen/common/ -I$(SSDA)/autogen/clnt/#-I$(SSDA)/clnt/block
INCLUDES += -I/usr/include

# ********** Define linker and compilation flags *********
LDFLAGS = -rdynamic

ifeq ($(USE_ARM),1)		# Define directories of lib files
	LIBS_PATH = -L/usr/lib64
else ifeq ($(USE_DARWIN), 1)
	LIBS_PATH = -L/usr/lib
else ifeq ($(USE_64_BIT), 1)
	LIBS_PATH = -L/usr/lib64
endif

LIBS  = -lpthread # Libraries to link into executable (with -l): pthread is needed

# ********** Define Files participating in the build *********
SOURCES =./nvmeibc_block_dp_ec_gf_asm.S_shipped
ifeq ($(USE_ARM),1)
	SOURCES =./nvmeibc_block_dp_ec_gf_asm_arm.S_shipped
	$(warning set Sources)
endif
OBJECTS = $(SOURCES:.S_shipped=.o)
$(warning Objects = $(OBJECTS))
$(warning SOURCES = $(SOURCES))

# ********** Define Rules for building *********
.PHONY: depend clean

all: clean $(OBJECTS)

clean:
	rm -f $(OBJECTS)

$(OBJECTS) : $(SOURCES)
	gcc -x assembler-with-cpp -c $< -o $@
#ifeq $(USE_64_BIT,1 )
#	mv $< $($<:.o_shipped=.o)
#endif
