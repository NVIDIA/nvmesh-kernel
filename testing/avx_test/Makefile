CC = gcc

ifeq ($(CC_TYPE),clang)
CFLAGS = -O3 -std=c99
else
CFLAGS = -Ofast -std=c99
endif
LDFLAGS = -pthread

TARGET = avx_runner

$(TARGET): avx_runner.c avx_runner.h

clean:
	rm -f $(TARGET)
