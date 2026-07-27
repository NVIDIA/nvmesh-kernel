CC=gcc
CFLAGS=-std=gnu99 -Wall -g -lpthread
LDFLAGS=-pthread

all: manual_fops

manual_fops: manual_fops.o
	$(CC) $(CFLAGS) manual_fops.o -o manual_fops $(LDFLAGS)

manual_fops.o: manual_fops.c $(HEADERS)
	${CC} $(CFLAGS) -c manual_fops.c

clean:
	rm -f manual_fops manual_fops.o
