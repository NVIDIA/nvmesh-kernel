#pragma once
/* Utils to generate random information for unitest */
#include "../bunitest.h"
#include "nvmesh_sim.h"


//static inline u16 rand_bitmap(const u8 width){ return rand() % GENMASK(width-1, 0);}
#define all_segs_bmp(replicas)     ((u32)((1 << (replicas)) - 1))
#define all_segs_bm(pr)     all_segs_bmp((pr)->replicas)
#define random_bmp(replicas)  (rand()%all_segs_bmp(replicas))
#define random_bitmap(pr)  (rand()%all_segs_bm(pr))

u32 __rand_n_bits_bitmap(int n_bits, int n_segs);			// Bitmap with 'n_bits' on in random places

//generates valid random tx bitmap for data; if with_parities == true, all parities will be added
roles_bmp_t rand_valid_txbm(const struct TstPRaid sraid, const bool with_parities);
void rand_valid_txbm_multi_slice(const struct TstPRaid sraid, const bool with_parities, u32 tx_bm[], const u32 tx_height, bool with_snake);

