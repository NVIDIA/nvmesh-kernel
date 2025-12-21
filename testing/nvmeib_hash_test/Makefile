CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=gnu99 -g
LDFLAGS = -pthread

TARGET = nvmeib_hash_test

$(TARGET): nvmeib_hash_test.c
	$(CC) $(CFLAGS) -o $(TARGET) nvmeib_hash_test.c $(LDFLAGS)

.PHONY: clean test

test: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)






