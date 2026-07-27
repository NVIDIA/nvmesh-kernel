# ********** Define Linker flags *********
LDFLAGS_EXE = -rdynamic -no-pie
LIB_NAME=libclusterstack

# ********** Compilation flags *********

CFLAGS += -Wall -Werror -std=gnu99 -Wdeclaration-after-statement -Wstrict-prototypes
CFLAGS += -Wextra -Wshadow -Werror=strict-aliasing -Wno-type-limits -falign-functions=8 -fvisibility=hidden

BUILD_RELEASE?=1
ifeq ($(BUILD_RELEASE),1)
	CFLAGS += -O3 -g -DNDEBUG
else
	CFLAGS += -g3 -ggdb3
endif

define print_building_target
	@printf "+--->Building |\e[0;32m$@\e[0;0m|\n"
endef
define print_executed_rule
	@printf "   +--->: |\e[0;32m$@\e[0;0m|${1}\n"
endef

OBJECTS=cluster_stack.o
DEPEND_FILES_ALL = $(OBJECTS:.o=.d)

%.o: %.c Makefile
	@$(CC) $(CFLAGS) -MP -MMD -o $@ -c $<
	$(call print_executed_rule)

$(LIB_NAME).so: $(OBJECTS)
	$(call print_building_target);
	@ld -shared -o $(LIB_NAME).so $(OBJECTS) -ldl -lpthread
#   gcc -shared -o $(LIB_NAME).so $(OBJECTS) -ldl -lpthread $(LDFLAGS_EXE);

clean:
	@printf "+-->Cleaning |\e[1;45m$(LIB_NAME)\e[0;0m|\n"
	rm -f $(OBJECTS) $(DEPEND_FILES_ALL) $(LIB_NAME).so

help:
	clear;
	@printf "Building |\e[0;32m$(LIB_NAME)\e[0;0m|\n"
	@printf "Usage |\e[0;32mmake all\e[0;0m| for building a lib\n"
	@printf "\n\n\n\n"

all: $(LIB_NAME).so
	$(info +--->Done!)

