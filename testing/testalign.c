/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/user.h>
#include <fcntl.h>
#include <unistd.h>

main(int ac, char **av)
{
	int do_write;
	ssize_t len;
	size_t offset = 256;
	char *buf;
	int fd;

	if (ac != 4 || (av[2][0] != 'r' && av[2][0] != 'w')) {
		fprintf(stderr, "Usage: %s filen [r/w] length\n");
		return 1;
	}
	len = atol(av[3]);
	do_write = (av[2][0] == 'w');
	fd = open(av[1], __O_DIRECT|(do_write ? O_WRONLY : O_RDONLY));
	if (fd < 0) {
		perror(av[1]);
		return 1;
	}
	buf = malloc(len+PAGE_SIZE);
	buf += (PAGE_SIZE + offset - (ulong)buf & (PAGE_SIZE-1)) & (PAGE_SIZE-1);
	printf("buf @ %p\n", buf);
	if (do_write) {
		len = write(fd, buf, len);
		printf("%d bytes written\n", len);
	}
	else {
		len = read(fd, buf, len);
		printf("%d bytes read\n", len);
	}

	if (len < 0)
		perror("I/O");
	return len >= 0;
}
