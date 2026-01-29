ifeq ($(LLVM),)
	CC=gcc
	CFLAGS += -Wno-old-style-declaration
else
	CC=clang
	autocompdb = -MJ $@.json
endif
SSDA?=../../..
MCFLAGS=-Wall -g -Werror -Wshadow -Wno-address-of-packed-member
MCFLAGS += -DNVMEIBC_SECTOR_SHIFT=12 -DUSER_SPACE -DNO_OFED=1
MCFLAGS += -std=gnu99 -Wno-address-of-packed-member -Wall -Werror -Werror=comment -std=gnu99 -Wno-deprecated -Wdeclaration-after-statement -Wextra -Wshadow -Werror=strict-aliasing  #Simulate kernel compilation & enable all warning. Can use gnu89 instead of 99
INCLUDES += -I./ -I$(SSDA)/common/ -I$(SSDA)/common_public/ -I$(SSDA)/clnt -I$(SSDA)/clnt/block/datapath_utils_generic/ -I$(SSDA)
all: parse_block dbgdi_log_test

parse_block: nvmeibc_block_dp_block_md_tables.o parse_block.o nvmeibc_block_dp_dbgdi_log.o
	$(CC) $(MCFLAGS) nvmeibc_block_dp_block_md_tables.o nvmeibc_block_dp_dbgdi_log.o parse_block.o -o parse_block $(LDFLAGS)

dbgdi_log_test: nvmeibc_block_dp_dbgdi_log.o dbgdi_log_test.o
	$(CC) $(MCFLAGS) dbgdi_log_test.o nvmeibc_block_dp_dbgdi_log.o -o dbgdi_log_test $(LDFLAGS)

nvmeibc_block_dp_block_md_tables.o: $(SSDA)/clnt/block/datapath_utils_generic/nvmeibc_block_dp_block_md_tables.c $(HEADERS)
	${CC} $(MCFLAGS) $(INCLUDES) -c $(SSDA)/clnt/block/datapath_utils_generic/nvmeibc_block_dp_block_md_tables.c

parse_block.o: parse_block.c $(HEADERS)
	${CC} $(MCFLAGS) $(INCLUDES) -c parse_block.c $(autocompdb)

nvmeibc_block_dp_dbgdi_log.o: $(SSDA)/clnt/block/datapath_utils_debug_di/nvmeibc_block_dp_dbgdi_log.c
	$(CC) $(MCFLAGS) $(INCLUDES) -c $(SSDA)/clnt/block/datapath_utils_debug_di/nvmeibc_block_dp_dbgdi_log.c

dbgdi_log_test.o:
	$(CC) $(MCFLAGS) $(INCLUDES) -c dbgdi_log_test.c

clean:
	rm -f parse_block parse_block.o nvmeibc_block_dp_block_md_tables.o dbgdi_log_test dbgdi_log_test.o nvmeibc_block_dp_dbgdi_log.o
