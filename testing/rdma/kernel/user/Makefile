CC=gcc
CFLAGS=-Wall -g -O0 -I/usr/include/infiniband -I/usr/include/infiniband/opensm
LDFLAGS= -libcm -libverbs -losmvendor -losmcomp -lopensm -libumad -lpthread

all: cltusr loop

cltusr: cltusr.o csa.o
	$(CC) $(CFLAGS) cltusr.o csa.o -o cltusr $(LDFLAGS)

cltusr.o: cltusr.c
	${CC} $(CFLAGS) -c cltusr.c $(LDFLAGS)
	
loop: loop.o
	$(CC) $(CFLAGS) loop.c -o loop $(LDFLAGS)

csa.o: csa.c
	${CC} $(CFLAGS) -c csa.c $(LDFLAGS)

clean:
	rm -f cltusr cltusr.o loop loop.o csa.o

