/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

//
// COMPILE using "gcc -lm nvmeib_iir_test.c"
//
#include <stdio.h>
#include <stdlib.h>
#include "../common/nvmeib_iir.h"
#define DUMP_IIR(__iir, __i) ({																\
	fprintf(stdout, "	%03d: new_sample_weight=%f sum=%f n_samples=%f val=%f saturation_level=%f\n",							\
		(__i), (__iir).new_sample_weight, (__iir).sum, (__iir).n_samples, nvmeib_iir_get_val(__iir), nvmeib_iir_get_saturation_level(__iir));		\
})

#define ADD_AND_DUMP_SAMPLE(_iir, _i, _val) ({		\
	fprintf(stdout, "Add val=%f:	", (_val));	\
	nvmeib_iir_add_sample((_iir), (_val));		\
	DUMP_IIR(*(_iir), (_i));				\
})

#define ADD_AND_DUMP_SAMPLES(_iir, _i, _n_samples, _val) ({				\
	fprintf(stdout, "Add n_samples=%f val=%f:	", (_n_samples), (_val));	\
	nvmeib_iir_add_multi_samples((_iir), (_n_samples), (_val));			\
	DUMP_IIR(*(_iir), (_i));							\
})

int main(int argc, char **argv)
{
	nvmeib_iir_t	iir;
	int		i;

	nvmeib_iir_reset(&iir, 0.01, "test1", "(second_desc)");
	fprintf(stdout, "A==================== Reset(new_sample_weight=%f)\n", iir.new_sample_weight);
	DUMP_IIR(iir, -1);

	for (i = 0; i < 100; i++) {
		ADD_AND_DUMP_SAMPLE(&iir, i, 100.0);
	}
	nvmeib_iir_set_new_sample_weight(&iir, 0.1);
	fprintf(stdout, "\nB======= set_new_sample_weight(%f)\n", iir.new_sample_weight);
	for (i = 0; i < 100; i++) {
		ADD_AND_DUMP_SAMPLE(&iir, i, 10.0);
	}
	nvmeib_iir_set_new_sample_weight(&iir, 0.01);
	fprintf(stdout, "\nC======= set_new_sample_weight(%f)\n", iir.new_sample_weight);
	for (i = 0; i < 100; i++) {
		ADD_AND_DUMP_SAMPLE(&iir, i, 100.0);
	}
	for (i = 0; i < 60; i++) {
		ADD_AND_DUMP_SAMPLES(&iir, i, 50.0, 1000.0);
	}
	nvmeib_iir_reset(&iir, 0.01, "test2", "(second desc)");
	fprintf(stdout, "\nD==================== Reset(new_sample_weight=%f)\n", iir.new_sample_weight);
	DUMP_IIR(iir, i);
	for (i = 0; i < 60; i++) {
		ADD_AND_DUMP_SAMPLES(&iir, i, 50.0, 1000.0);
	}

	return 0;
}
