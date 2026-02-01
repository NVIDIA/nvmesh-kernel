# ********** Define compiler and compilation flags *********
UNAME := $(shell uname)# Which operating system we use?
HT = $(shell uname -m)
$(info Building on $(UNAME)/$(HT))
ifeq ($(UNAME),Darwin)
	USE_DARWIN=1
	CFLAGS += -m64
	DP_EC_GF_ASM := nvmeibc_block_dp_ec_gf_asm.S_shipped
	DP_EC_GF_CFILES :=
	LIBS_PATH = -L/usr/lib
else ifeq ($(HT),x86_64)
	USE_64_BITS=1
	CFLAGS += -m64
	DP_EC_GF_ASM := nvmeibc_block_dp_ec_gf_asm.S_shipped
	DP_EC_GF_CFILES :=
	LIBS_PATH = -L/usr/lib64
else ifeq ($(HT),aarch64)
	USE_ARM=1
	CFLAGS += -march=armv8.1-a+crc+fp+simd
	DP_EC_GF_ASM :=
	DP_EC_GF_CFILES := nvmeibc_block_dp_ec_gf_arm_neon4.c nvmeibc_block_dp_ec_gf_arm_recov_neon.c nvmeibc_block_dp_ec_gf_arm_um.c
	LIBS_PATH = -L/usr/lib64
else
	$(error Unknown OS/Host)
endif

USE_RELEASE=1
CFLAGS ?= -std=gnu99 -Wno-address-of-packed-member -Wall -Werror -Werror=comment -std=gnu99 -Wno-deprecated -Wdeclaration-after-statement -Wextra -Wshadow -Werror=strict-aliasing  #Simulate kernel compilation & enable all warning. Can use gnu89 instead of 99

ifeq ($(LLVM),)
	CC=gcc
	CFLAGS += -Wno-old-style-declaration
else
	CC=clang
	autocompdb = -MJ $@.json
endif
ifeq ($(USE_RELEASE),1)
	CFLAGS += -O4
else
	CFLAGS += -g3 -ggdb3
endif

CFLAGS += -fvisibility=default
CFLAGS += -DCOMMIT_ID=0x$(COMMIT_ID)ULL -DVER_TAGID=$(VER_TAGID) -DBRANCH_NAME=$(BRANCH_NAME)
CFLAGS += -DNVMESH_VERSION=$(VER_TAGID) -DNVMESH_RELEASE=1.1.0-97 -DBUILD_NUMBER=0.0
CFLAGS += -DNVMEIBC_SECTOR_SHIFT=12 -DUSER_SPACE -DNO_OFED=1
CFLAGS += -Wno-address-of-packed-member

GCCGREATERTHEN5 := $(shell expr `gcc -dumpversion | cut -f1 -d.` \>= 6)
ifeq ($(GCCGREATERTHEN5),1)
LDFLAGS := -no-pie
endif

# ********** Included directories of H files *********
# Main SSDA project directory. (relative to this Makefile)
SSDA?=../../..
INCLUDES :=
INCLUDES += -I$(SSDA)/perfTest/io_stress/cmp_blocks/ -I$(SSDA)/clnt/ -I$(SSDA)/common/ -I$(SSDA)
INCLUDES += -I/usr/include

# ********** Define linker and compilation flags *********
LIBS = -lpthread # Libraries to link into executable (with -l): pthread is needed

# ********** Define Files participating in the build *********
EC_DIR=$(SSDA)/clnt/block/datapath_ec
SOURCES  = ./cmp_blocks.c ./kr_incs.c ./cmp_blocks_impl.c
SOURCES += $(patsubst %,$(EC_DIR)/%,$(DP_EC_GF_CFILES))
SOURCES += $(EC_DIR)/nvmeibc_block_dp_ec_gf.c $(EC_DIR)/nvmeibc_block_dp_ec_gf_praid.c
SOURCES += $(SSDA)/clnt/block/datapath_utils_generic/nvmeibc_block_dp_block_md_tables.c $(SSDA)/clnt/block/datapath_utils_debug_di/nvmeibc_block_dp_dbgdi_log.c

EXE_NAME = cmp_blocks
OBJECTS =$(SOURCES:.c=_$(EXE_NAME).o)

ASMSRC   = $(patsubst %,$(EC_DIR)/%,$(DP_EC_GF_ASM))
ASMOBJ   = $(ASMSRC:.S_shipped=.o_shipped)

# ********** Define Rules for building *********
.PHONY: depend clean
#$(info -->CFLAGS | $(CFLAGS))

all: $(SOURCES) $(EXE_NAME)
$(EXE_NAME): $(OBJECTS) $(ASMOBJ)
	 @$(CC) $(LDFLAGS) $(OBJECTS) $(ASMOBJ) -o $(EXE_NAME) $(LIBS_PATH) $(LIBS)

%_$(EXE_NAME).o : %.c
	@$(CC) $(CFLAGS) $(INCLUDES) -c $< $(autocompdb) -o $@

%.o_shipped: %.S_shipped
	$(CC) -c -x assembler-with-cpp -o $@ $<

clean:
	$(info -->Cleaning... |$(OBJECTS)|$(ASMOBJ))
	@rm -f $(OBJECTS) $(EXE_NAME) $(ASMOBJ)
