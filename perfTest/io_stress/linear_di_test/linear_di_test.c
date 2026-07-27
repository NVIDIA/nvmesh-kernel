#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

struct tm *tm;
time_t t;
char str_time[100];
char str_date[100];

unsigned long global = 0;
#define U64s_IN_BLOCK	(4096/8)

/* Generate file of 128[mbytes] = 1000[locks] */
void fill_1gb(char *filename, unsigned long j)
{
	FILE *f;
	unsigned long b[U64s_IN_BLOCK];
	int i;
	int maxi = 128 * 1024 / 4;

	f = fopen(filename, "w");

	for (i = 0; i < U64s_IN_BLOCK; ++i)
		b[i] = j;
	b[U64s_IN_BLOCK - 1] = 0xdeadbeef;

	for (i = 0; i < maxi; ++i) {	// Generate unique block
		if (!(i & 0x7ff)) printf("Done %d / %d (value=%lu)\n", i, maxi, j);
		if (!(i & 0xf)) {
			t = time(NULL);
			tm = localtime(&t);
			strftime(str_time, sizeof(str_time), "%H %M %S", tm);
			strftime(str_date, sizeof(str_date), "%d %m %Y", tm);
			sprintf((char *)b, "%31s", str_date);
			sprintf((char *)(b + 4), "%31s", str_time);
		}

		b[8] = global++;
		fwrite(b, 1, 4096, f);
	}
	fclose(f);
}

/*****************************************************************************/
void read_1gb(char *filename)
{
	FILE *f;
	unsigned long b[U64s_IN_BLOCK];
	int i;
	int maxi = 128 * 1024 / 4;
	unsigned long last = 0, current;
	int dropped1 = 0;

	f = fopen(filename, "rr");

	for (i = 0; i < maxi; ++i) {
		if (!(i & 0x7ff)) printf("Reading %d / %d\n", i, maxi);
		fread(b, 1, 4096, f);

		if (b[U64s_IN_BLOCK - 1] != 0xdeadbeef) {
			printf("Bad block %d, missing deadbeef\n", i);
		}

		current = b[8];
		if (current != last + 1 && i) {
			if (!dropped1) {
				printf("Dropping at %d, current=%lld, last=%lld\n", i, (unsigned long long)current, (unsigned long long)last);
				dropped1 = 1;
			}
			else {
				printf("Bad block %d, current=%lld, last=%lld %s, %s\n", i, (unsigned long long)current, (unsigned long long)last, (char*)b, (char*)(b + 4));
			}
		}
		last = current;
	}
	fclose(f);
}

int main(int argc, char *argv[])
{
	int j, maxj;
	if (argc <=2) {
		__print_args_and_exit:
			printf("Generate unique 128[mb] file: w <file_name> n\n");
			printf("Test that file was written  : r <file_name>\n");
			return -5;
	}
	switch (argv[1][0]){
		case 'w':
			sscanf(argv[3], "%d", &maxj);
			printf("\tGenerating: %s, %d\n", argv[2], maxj);
			for (j = 0; j < maxj; ++j) { // Overwrite the same file maxj times
				fill_1gb(argv[2], j);
			}
			break;
		case 'r':
			printf("\tVerifying: %s\n", argv[2]);
			read_1gb(argv[2]);
			break;
		default:
			goto __print_args_and_exit;
	}
	return 0;
}

