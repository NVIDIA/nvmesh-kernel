C=gcc
CFLAGS=-Wall -g -ggdb
LDFLAGS= -lrdmacm -libverbs

all: cs

cs: cs.o
	$(CC) $(CFLAGS) cs.o -o cs $(LDFLAGS)	

cs.o: cs.c
	${CC} -c cs.c

clean:
	rm -f cs cs.o

