/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <string.h>
#include <errno.h>

#define UTIL_VERSION  "version=1.0"
#define MSG_SIZE 256

int main(int argc, char *argv[])
{
	struct sockaddr_un sun;
	int fd, i, len, total_bytes_received = 0;
	char inbuf[MSG_SIZE+1];

	if (getuid()) {
		printf(UTIL_VERSION ", Permission denied\n");
		return -1;
	}
	if (argc<2) {
		printf(UTIL_VERSION ", No command specified. Try 'help'.\n");
		return -1;
	}

	inbuf[0] = 0;
	len = 0;
	for (i=1; i<argc; i++) {
		const int available = MSG_SIZE - len - 2;
		const int n_bytes = snprintf(inbuf+len, available, "%s ", argv[i]);
		if (n_bytes < 0)
			return -1;
		if (n_bytes >= available) {	// Check if output was truncated
			printf("Command too long (truncated)\n");
			return -1;
		}
		len += n_bytes;
	}
	if (len <= 0) {
		printf("Wrong command specified, len=%d. Try 'help'.\n", len);
		return -1;
	}
	inbuf[len-1] = '\n';
	inbuf[len] = 0;

	fd = socket(PF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("socket");
		return -1;
	}
	sun.sun_family = PF_UNIX;
	strncpy(sun.sun_path, "/var/log/nvmesh/toma_rpc", sizeof(sun.sun_path)-1);
	sun.sun_path[sizeof(sun.sun_path)-1] = '\0';  // Ensure null termination
	if (connect(fd, (struct sockaddr *)&sun, sizeof(sun)) < 0) {
		perror("connect");
		close(fd);
		return -1;
	}
	if (send(fd, inbuf, strlen(inbuf), 0) < 0) {
		perror("send");
		close(fd);
		return -1;
	}

	while (1) {		// Receive the reply, if reply is larger then this loop will do multiple iterations
		const int n_bytes = recv(fd, inbuf, sizeof(inbuf)-1, 0);
		if (n_bytes == 0) {
			printf("\n[end %db]\n", total_bytes_received);
			break;
		} else if (n_bytes < 0) {
			printf("\n[error %m]\n");
			break;
		}
		inbuf[n_bytes] = 0;
		printf("%s", inbuf);
		total_bytes_received += n_bytes;
	}
	close(fd);
	return 0;
}
