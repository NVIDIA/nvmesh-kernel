/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include <stdio.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <errno.h>


#define TOMA_RPC_PATH "/var/log/nvmesh/toma_rpc"
#define MSG_SIZE 256

int main(int argc, char *argv[])
{
	struct sockaddr_un sun;
	int fd;
	int i, len, rc, arg;
	char inbuf[MSG_SIZE+1];

	if (getuid()) {
#if 0
		char **new_argv = (char **)malloc(sizeof(char*)*(argc+2));
		memcpy(new_argv+1, argv, argc*sizeof(char *));
		new_argv[argc+1] = NULL;
		new_argv[0] = "sudo";
		printf("(You're not root, attempting automatic sudo)\n");
		execvp(new_argv[0], new_argv);
		return 0;		//never reached
#endif
		printf("Permission denied\n");
		return -1;
	}

	if (argc<2) {
		printf("No command specified. Try 'help'.\n");
		return -1;
	}

	inbuf[0] = 0;
	len = 0;
	for (i=1; i<argc; i++) {
		rc = snprintf(inbuf+len, MSG_SIZE - len - 1, "%s ", argv[i]);
		if (rc<0)
			return -1;
		len += rc;
	}
	if (len>0)
		inbuf[len-1] = 0;
	strcat(inbuf, "\n");

	fd = socket(PF_UNIX, SOCK_STREAM, 0);
	if (fd<0) {
		perror("socket");
		return -1;
	}

	sun.sun_family = PF_UNIX;
	strncpy(sun.sun_path, TOMA_RPC_PATH, sizeof(sun.sun_path)-1);
	rc = connect(fd, (struct sockaddr *)&sun, sizeof(sun));
	if (rc<0) {
		perror("connect");
		close(fd);
		return -1;
	}

	send(fd, inbuf, strlen(inbuf), 0);
	arg=1;
	ioctl(fd, FIONBIO, &arg);

	while (1) {
		rc = recv(fd, inbuf, sizeof(inbuf)-1, 0);
		if (rc == 0) {
			// graceful
			printf("\n[end]\n");
			break;
		}
		else if (rc<0) {
			if (errno != EAGAIN && errno != EINTR) {
				printf("\n[error %m]\n");
				break;
			}
			continue;
		}

		inbuf[rc] = 0;
		printf("%s", inbuf);
	}
	close(fd);

	return 0;
}
