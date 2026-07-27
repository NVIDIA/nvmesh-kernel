/*
 * toma_link.c
 *
 * Look for Toma executable and run it, replacement for symbolic-link.
 *
 *  Created on: Sep 6, 2020
 *      Author: yair
 */


#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
	char *top_search_path = "/opt/nvmesh/target-repo";
	DIR *d;
	struct dirent *e;

	d = opendir(top_search_path);
	while ((e = readdir(d)) != NULL) {
		if (strncmp(e->d_name, "target_", 7)==0) {
			char path[512];
			snprintf(path, sizeof(path), "%s/%s/toma/bin/release/nvmeibt_toma", top_search_path, e->d_name);
			execv(path, argv);
			break;			// never reached
		}
		if (strcmp("toma", e->d_name)==0) {
			// kmod installation, special case
			char path[256];
			sprintf(path, "%s/toma/bin/release/nvmeibt_toma", top_search_path);
			execv(path, argv);
			break;			// never reached
		}
	}

	closedir(d);
	printf("nvmeibt_toma executable not found!\n");
	return -1;
}
