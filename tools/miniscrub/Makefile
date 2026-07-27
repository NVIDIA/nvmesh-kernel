ifeq ($(NON_PROD),1)
	INFRA_SO_PATH=../infra_shared/infra_shared.so
else
	INFRA_SO_PATH=./infra_shared.so
endif
CFLAGS_TEST  =-DCOMPILE_FOR_TEST
CFLAGS_TEST +=-g -ggdb # -O
$(warning Objects = $(CFLAGS_TEST), "dfsdfs ", $(INFRA_SO_PATH))

all: miniscrub.o miniscrub_test

miniscrub.o: miniscrub.c
	cc -c -O -o miniscrub.o miniscrub.c

miniscrub_test: miniscrub.c
	cc $(CFLAGS_TEST) -o miniscrub_test miniscrub.c $(INFRA_SO_PATH)

clean:
	/bin/rm -f miniscrub_test miniscrub.o
