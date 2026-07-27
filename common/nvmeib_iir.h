#ifndef NVMEIB_IIR
#define NVMEIB_IIR

#include <math.h>
#include <sys/param.h>
#include "nvmeib_str.h"

// A small testing program under ../testing/nvmeib_iir_test.c

/*
 * The IIR (Infinite Impulse Response) calculates the weighted average of all the values
 *  that were sampled, giving higher weight to more recent samples.
 * As such changes are gradual and contineous.
 * Roughly speaking, a new_sample_weight of 0.01 is equivalent to a window of 1/0.01=100 samples.
 * The smaller the new_sample_weight, the more stable the IIR output, but the learning rate is
 *  also slower, so you need to find the right balance.
 * This IIR functions correctly right from the very first input.
 * The samples can be added one by one (recommended). Alternatively, the application can calculate
 *  the average of the samples of say one hour, and feed the average in (including the number of
 *  samples that it represents).
 */
#define NVMEIB_IIR_DEFAULT_SAMPLE_WEIGHT 0.01

#define NVMEIB_IIR_DUMP(name_dump, _iir_dump, last_val) ({																				\
	N_Tf(name_dump ## 111, "IIR: @STR new_sample_weight=@DOUBLE n_samples=@DOUBLE val=@INT64_TD sd=@INT64_TD - last_val=@INT64_TD",		\
		 (_iir_dump).desc, (_iir_dump).new_sample_weight, (_iir_dump).n_samples,														\
		 (int64_t)nvmeib_iir_get_val(_iir_dump), (int64_t)nvmeib_iir_get_standard_deviation(_iir_dump), last_val);						\
})

typedef struct nvmeib_iir {
	char		desc[100];
	double		sum;
	double		n_samples;
	double		sum_square_of_diff_from_avg;
	double		new_sample_weight;	// Say 0.01, which roughly means that new_val=0.01*new_val+0.99*old_val
} nvmeib_iir_t;

static inline void nvmeib_iir_set_new_sample_weight(struct nvmeib_iir *iir, double new_sample_weight)
{
	if (iir->n_samples != 0.0 && new_sample_weight != 0.0) {
		double factor = MIN(1.0, (1.0 / new_sample_weight) / iir->n_samples); // If we sampled a lot with low weight, and want to learn faster got high-weight
		iir->n_samples *= factor;
		iir->sum *= factor;
	}
	iir->new_sample_weight = new_sample_weight;
}

static inline double nvmeib_iir_get_val(struct nvmeib_iir iir)
{
	return (iir.n_samples != 0.0 ? (iir.sum / iir.n_samples) : 0.0);
}

static inline double nvmeib_iir_get_saturation_level(struct nvmeib_iir iir)
{
	return (iir.n_samples * iir.new_sample_weight); 
}

static inline double nvmeib_iir_get_standard_deviation(struct nvmeib_iir iir)
{
	return (iir.n_samples <= 1.0 ? 0.0 : sqrt(iir.sum_square_of_diff_from_avg / (iir.n_samples - 1)));
}

static inline void nvmeib_iir_reset(struct nvmeib_iir *iir, double new_sample_weight, char *desc1, char *desc2)
{
	size_t		len1 = strlen(desc1);

	*iir = (struct nvmeib_iir){"", 0.0, 0.0, 0.0, new_sample_weight};
	nvmeib_strlcpy(iir->desc, desc1, sizeof(iir->desc));
	nvmeib_strlcpy(iir->desc + len1, desc2, sizeof(iir->desc) - len1);
}

#define NVMEIB_IIR_RESET(name, _iir_reset, _new_sample_weight_reset, _desc_reset_1, _desc_reset_2) ({	\
	nvmeib_iir_reset(_iir_reset, _new_sample_weight_reset, _desc_reset_1, _desc_reset_2);				\
	TODO(NVMEIB_IIR_DUMP(name, *_iir_reset, 0););														\
})

#define _NVMEIB_IIR_ADD_STEP(_iir_accumulator, _q_, _val_)	(_iir_accumulator) = ((_iir_accumulator) * _q_ + (_val_) * 1.0)
static inline void nvmeib_iir_add_sample(struct nvmeib_iir *iir, double sample_val)
{
	double		q = (1.0 - iir->new_sample_weight);
	double		diff_from_avg;

	// Apply the weights to both n_samples and sum
	_NVMEIB_IIR_ADD_STEP(iir->n_samples, q, 1.0);
	_NVMEIB_IIR_ADD_STEP(iir->sum, q, sample_val);
	diff_from_avg = sample_val - nvmeib_iir_get_val(*iir);
	_NVMEIB_IIR_ADD_STEP(iir->sum_square_of_diff_from_avg, q, (diff_from_avg * diff_from_avg));
}
#define NVMEIB_IIR_ADD_AND_DUMP(name, _iir_add, _val_add) ({					\
	N_Tf("NVMEIB_IIR_ADD_AND_LOG val=@INT64_TD", (int64_t)(_val_add));			\
	nvmeib_iir_add_sample(&(_iir_add), (_val_add));								\
	NVMEIB_IIR_DUMP(name, (_iir_add), 0);										\
})


static inline void nvmeib_iir_add_multi_samples(struct nvmeib_iir *iir, double sample_size, double sample_val)
{
	// The weights are a geometric series
	//  The new sample is as if we added sample_size samples, their weights are a sum = [(q ^ n - 1) / (q - 1)]
	double q = 1.0 - iir->new_sample_weight;
	double weight_of_old = pow(q, sample_size);
	double weight_of_new = (pow(q, sample_size) - 1.0) / (q - 1.0);

	// Apply the weights to both n_samples and sum
	iir->n_samples = (iir->n_samples * weight_of_old) + (1.0 * weight_of_new);
	iir->sum = (iir->sum * weight_of_old) + (sample_val * weight_of_new);
}

#endif	// #ifndef NVMEIB_IIR

