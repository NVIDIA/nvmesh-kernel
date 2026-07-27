#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[])
{
	FILE *f = fopen("file", "w");
	
	for (unsigned long block = 0; block < 100000; ++block) {
		char data[4096];
		unsigned long *p = (unsigned long *)data;
		for (int i = 0; i < sizeof(data) / sizeof(unsigned long); ++i) {
			*(p++) = block;
		}
		fwrite(data, 1, sizeof(data), f);
	}

	fclose(f);
}
