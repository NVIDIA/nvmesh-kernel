#include <linux/netdevice.h>
#include <linux/scatterlist.h>
#include <infiniband/hw/mlx4/mlx4_ib.h>
#include <linux/mlx4/cq.h>

#include "nvmeib_public.h"
#define nvmeib_debug_level nvmeib_public_mlx4_debug_level
#include "nvmeib_utils.h"

static u32 convert_access(int acc)
{
	return (acc & IB_ACCESS_REMOTE_ATOMIC ? MLX4_PERM_ATOMIC       : 0) |
	       (acc & IB_ACCESS_REMOTE_WRITE  ? MLX4_PERM_REMOTE_WRITE : 0) |
	       (acc & IB_ACCESS_REMOTE_READ   ? MLX4_PERM_REMOTE_READ  : 0) |
	       (acc & IB_ACCESS_LOCAL_WRITE   ? MLX4_PERM_LOCAL_WRITE  : 0) |
	       (acc & IB_ACCESS_MW_BIND	      ? MLX4_PERM_BIND_MW      : 0) |
	       MLX4_PERM_LOCAL_READ;
}

static int mlx4_alloc_n_map(struct nvmeib_alloc_n_map *mem)
{
	struct ib_pd *pd = mem->pd;
	u64 ioaddr = mem->ioaddr;
	int access_flags = mem->access_flags;
	struct mlx4_ib_dev *dev = to_mdev(pd->device);
	struct mlx4_ib_mr *mr = kzalloc(sizeof(*mr), GFP_KERNEL);
	struct mlx4_mtt *mtt;
	u64 *mtt_pages;
	int i, j, n, rv;
	struct sg_dma_page_iter sg_iter;

	NFIN;
	if (!mem->n_pages || !mem->pages || 
		(!mem->use_dma_pages && (!mem->mem_table.sgl || !mem->mem_table.nents))) {
		_NE_dmesg(error_9_nvmeib_public_mlx4_imp_mlx4_alloc_n_map,
			  "Invalid parameter(s) n_pages: @N_PAGES, pages: @PAGES, use_dma_pages: @BOOL_YN sgl: @PTR, nents: @NENTS",
			  mem->n_pages, mem->pages, mem->use_dma_pages, mem->mem_table.sgl, mem->mem_table.nents);
		rv = -EINVAL;
		goto out;
	}
	mtt_pages = (u64 *)__get_free_page(GFP_KERNEL);
	if (!mr || !mtt_pages) {
		_NE(error_5_nvmeib_public_mlx4_imp_mlx4_alloc_n_map, "Fail to allocate memory region");
		rv = -ENOMEM;
		goto no_mem;
	}
	if ((rv = mlx4_mr_alloc(dev->dev, to_mpd(pd)->pdn, ioaddr,
		mem->n_pages << PAGE_SHIFT, convert_access(access_flags),
		mem->n_pages, PAGE_SHIFT, &mr->mmr)) < 0) {
		_NE(error_2_nvmeib_public_mlx4_imp_mlx4_alloc_n_map, "Fail to create memory region");
		rv = -ENOMEM;
		goto no_mem;
	}
	i = j = n = 0;
	mtt = &mr->mmr.mtt;
	if (mem->use_dma_pages) {
		while (i < mem->n_pages) {
			for (j = 0; j < PAGE_SIZE / sizeof(dma_addr_t) && i < mem->n_pages;
			     ++i, ++j)
			     mtt_pages[j] = mem->dma_pages[i];
			if ((rv = mlx4_write_mtt(dev->dev, mtt, n, j, mtt_pages)) < 0) {
				_NE(error_3_nvmeib_public_mlx4_imp_mlx4_alloc_n_map, "Fail to write memory into device @RV", rv);
				goto err_mr;
			}
			else
				n += j;
		}
	} else {
		for_each_sg_dma_page(mem->mem_table.sgl, &sg_iter, mem->map_sg_nents, 0) {
			mtt_pages[j] = sg_page_iter_dma_address(&sg_iter);
			j++;
			i++;
			if (j == PAGE_SIZE / sizeof(dma_addr_t)) {
				/* Flush current page to MTT */
				if ((rv = mlx4_write_mtt(dev->dev, mtt, n, j, mtt_pages)) < 0) {
					_NE(error_6_nvmeib_public_mlx4_imp_mlx4_alloc_n_map, "Fail to write memory into device @RV", rv);
					goto err_mr;
				}
				n += j;
				j = 0;
			}
		}
		if (j > 0) {
			/* Flush last page to MTT */
			if ((rv = mlx4_write_mtt(dev->dev, mtt, n, j, mtt_pages)) < 0) {
				_NE(error_7_nvmeib_public_mlx4_imp_mlx4_alloc_n_map, "Fail to write memory into device @RV", rv);
				goto err_mr;
			}
		}
	}
	BUG_ON(i != mem->n_pages);
	if ((rv = mlx4_mr_enable(dev->dev, &mr->mmr)) < 0) {
		_NE(error_4_nvmeib_public_mlx4_imp_mlx4_alloc_n_map, "Fail to enable memory region on device @RV", rv);
		goto err_mr;
	}
	mr->ibmr.rkey = mr->ibmr.lkey = mr->mmr.key;
	mem->mr = &mr->ibmr;
	mem->lkey = mem->mr->lkey;
	mem->rkey = mem->mr->rkey;
	goto out;

err_mr:
	mlx4_mr_free(dev->dev, &mr->mmr);

no_mem:
	kfree(mr);
	if (mtt_pages)
		free_page((unsigned long)mtt_pages);

out:
	
	NFOUT;
	return rv;
}

static int mlx4_unmapn_n_free(struct nvmeib_alloc_n_map *mem)
{
	struct mlx4_ib_mr *mr;
	struct mlx4_ib_dev *dev = to_mdev(mem->pd->device);

	NFIN;
	if (mem->mr) {
		mr = container_of(mem->mr, struct mlx4_ib_mr, ibmr);
		mlx4_mr_free(dev->dev, &mr->mmr);
	}
	kfree(mem->mr);
	NFOUT;
	return 0;
}

#if IB_NEW_FR
static int mlx4_map_mr(struct ib_device *ibdev, struct ib_mr *mr,
	phys_addr_t *pages, int n_pages)
{
	struct mlx4_ib_mr *mr4 = to_mmr(mr);
	int n;

	NFIN;
	mr4->npages = 0;
	/* JH IOMMU: DMA_TO_DEVICE is correct. addresses are r/o for NIC */
	ib_dma_sync_single_for_cpu(ibdev, mr4->page_map,
		sizeof(u64) * mr4->max_pages, DMA_TO_DEVICE);
	for (n = 0; n < n_pages; ++n)
		mr4->pages[mr4->npages++] =
			cpu_to_be64(pages[n] | MLX4_MTT_FLAG_PRESENT);
	ib_dma_sync_single_for_device(ibdev, mr4->page_map,
		sizeof(u64) * mr4->max_pages, DMA_TO_DEVICE);
	NFOUT;
	return 0;
}
#else
static int mlx4_map_mr(struct ib_device *ibdev, struct ib_mr *mr,
	phys_addr_t *pages, int n_pages)
{
	_NE(error_nvmeib_public_mlx4_imp_mlx4_map_mr,
		"Calling mlx4_map_mr(...) for old_kernel or OFED installation");
	return -1;
}
#endif

static void *get_cqe_from_buf(struct mlx4_ib_cq_buf *buf, int n)
{
	return mlx4_buf_offset(&buf->buf, n * buf->entry_size);
}

static void *get_cqe(struct mlx4_ib_cq *cq, int n)
{
	return get_cqe_from_buf(&cq->buf, n);
}

static void *get_sw_cqe(struct mlx4_ib_cq *cq, int n)
{
	struct mlx4_cqe *cqe = get_cqe(cq, n & cq->ibcq.cqe);
	struct mlx4_cqe *tcqe = ((cq->buf.entry_size == 64) ? (cqe + 1) : cqe);

	return (!!(tcqe->owner_sr_opcode & MLX4_CQE_OWNER_MASK) ^
		!!(n & (cq->ibcq.cqe + 1))) ? NULL : cqe;
}

static int mlx4_peek_cq(struct ib_cq *ib_cq, int max)
{
	struct mlx4_ib_cq *cq = to_mcq(ib_cq);
	u32 peek_cons = cq->mcq.cons_index;
	int n_cqe;

	for (n_cqe = 0; n_cqe < max; n_cqe++, peek_cons++) {
		if (!get_sw_cqe(cq, peek_cons))
			break;
	}
	return n_cqe;
}

