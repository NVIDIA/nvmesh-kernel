CC=gcc
CFLAGS=-Wall -g

all: linear_di_test

linear_di_test: linear_di_test.o
	$(CC) $(CFLAGS) linear_di_test.o -o linear_di_test $(LDFLAGS)

linear_di_test.o: linear_di_test.c $(HEADERS)
	${CC} $(CFLAGS) -c linear_di_test.c

clean:
	rm -f linear_di_test linear_di_test.o
