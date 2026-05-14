/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeib_public.h"
#define nvmeib_debug_level nvmeib_public_siw_debug_level
#include "nvmeib_utils.h"

#include <linux/netdevice.h>
#include "kernel/siw.h"

#if !IB_NEW_FR
#error Error: Support to SIW is with new FR only!
#endif

static int siw_map_mr(struct ib_device *ibdev, struct ib_mr *ofa_mr,
	phys_addr_t *pages, int n_pages);

static u32 convert_access(int acc)
{
	return SR_MEM_LREAD |
		(acc & IB_ACCESS_LOCAL_WRITE   ? SR_MEM_LWRITE : 0) |
		(acc & IB_ACCESS_REMOTE_WRITE  ? SR_MEM_RWRITE : 0) |
		(acc & IB_ACCESS_REMOTE_READ   ? SR_MEM_RREAD  : 0) |
		(acc & IB_ACCESS_REMOTE_ATOMIC ? SR_MEM_RATOMIC: 0) |
		(acc & IB_ACCESS_MW_BIND	   ? 0    		   : 0);
}

static int siw_alloc_n_map(struct nvmeib_alloc_n_map *mem)
{
	struct ib_pd *pd = mem->pd;
	u64 ioaddr = mem->ioaddr;
	u32 access = convert_access(mem->access_flags);
	struct ib_mr *mr = NULL;
	struct siw_mr *siw_mr;
	int i = 0, rv = -1;
	struct sg_dma_page_iter sg_iter;
	struct siw_pble *pble;
	u64 pbl_off = 0;

	NFIN;
	/* use_dma_pages is invalid for siw (only used for RDDA) */
	if (!mem->n_pages || !mem->pages || mem->use_dma_pages || !mem->mem_table.sgl || !mem->mem_table.nents) {
		_NE_dmesg(error_6_nvmeib_public_siw_imp_siw_alloc_n_map,
			  "Invalid parameter(s) n_pages: @N_PAGES, pages: @PAGES, sgl: @PTR, nents: @NENTS",
			  mem->n_pages, mem->pages, mem->mem_table.sgl, mem->mem_table.nents);
		rv = -EINVAL;
		goto out;
	}
	if ((mem->access_flags & IB_ACCESS_MW_BIND)) {
		_NT(trace_nvmeib_public_siw_imp_siw_alloc_n_map, "Access mode not supported @ACCESS_FLAGS", mem->access_flags);
		//SIW-TODO: uncomment this - currently we only use jbof which does not use atomics
		//rv = -EINVAL;
		//goto out;
	}

	//omril: reduce plb size by first checking num of addresses that can be in the same plbe
	if (IS_ERR(mr = siw_mr_alloc(pd, mem->n_pages))) {
		_NE(error_5_nvmeib_public_siw_imp_siw_alloc_n_map, "Fail to create memory region for @INT pages, @PTR_ERR", mem->n_pages, PTR_ERR(mr));
		rv = -ENOMEM;
		goto out;
	}
	/* required by siw APIs */
	mr->device = pd->device;

	/* MRs with huge-pages are not supported */
	mr->page_size = PAGE_SIZE;
	siw_mr = siw_mr_ofa2siw(mr);
	siw_mr->pbl->pbe_fixed_shift = PAGE_SHIFT;
	pble = siw_mr->pbl->pbe;
	for_each_sg_dma_page(mem->mem_table.sgl, &sg_iter, mem->map_sg_nents, 0) {
		/* DMA address is actually the virtual address thanks to nvmeib_public_ib_dma_map_sg or siw_dma_map_sg */
		pble[i].addr = sg_page_iter_dma_address(&sg_iter);
		pble[i].size = PAGE_SIZE;
		pble[i].pbl_off = pbl_off;
		i++;
		pbl_off += PAGE_SIZE;
	}
	/* Sanity */
	BUG_ON(pbl_off != mem->n_pages * PAGE_SIZE);
	BUG_ON(i != mem->n_pages);

	siw_mr->pbl->num_buf = mem->n_pages;
	mr->iova = ioaddr;
	mr->length = mem->n_pages * PAGE_SIZE;
	mr->page_size = PAGE_SIZE;

	/*
	 * Mirror what siw_map_mr() / siw_map_mr_sg() do: propagate the
	 * caller's iova/length into siw_mr->mem so that siw_check_mem()'s
	 * bounds check ([mem.va, mem.va + mem.len)) is correct. Without
	 * this, mem.va/mem.len remain at the (now-zero) placeholder values
	 * set by __siw_alloc_mr() and any inbound RDMA WRITE / RREAD-RESP
	 * targeting this MR fails with -EINVAL.
	 */
	siw_mr->mem.va = mr->iova;
	siw_mr->mem.len = mr->length;

	if ((rv = siw_mr_enable(mr, access)) < 0) {
		_NE(error_3_nvmeib_public_siw_imp_siw_alloc_n_map, "Fail to enable memory region @RV", rv);
		goto free_mr;
	}

	mem->mr = mr;
	mem->lkey = mem->mr->lkey;
	mem->rkey = mem->mr->rkey;
	mem->ioaddr = mem->mr->iova;
	_NT(trace_4_nvmeib_public_siw_imp_siw_alloc_n_map, "mem: mr=@MR_PTR, lkey=@LKEY, rkey=@RKEY, ioaddr=@IOADDR (was @IOADDR)",
		mem->mr, mem->lkey, mem->rkey, mem->ioaddr, ioaddr);
	goto out;

free_mr:
	siw_mr_free(mr);
out:
	NFOUT;
	return rv;
}

static int siw_unmapn_n_free(struct nvmeib_alloc_n_map *mem)
{
	NFIN;

	if (mem->mr) {
		siw_mr_free(mem->mr);
		mem->mr = NULL;
	}
	NFOUT;
	return 0;
}



#if 0
/* Copy @pages to siw's pbl. Later, on post-send of REG_MR (ib_map_mr_sg() was not used
   [requires changing siw for NVMesh], populate pbl */
static int siw_map_mr(struct ib_device *ibdev, struct ib_mr *mr,
	phys_addr_t *pages, int n_pages)
{
	struct siw_mr *siw_mr = siw_mr_ofa2siw(mr);
	struct siw_pbl *pbl = siw_mr->pbl;
	int i;

	if (!pbl) {
		_NE(siw_map_mr_e1, "No mr pages allocated\n");
		return -EINVAL;
	}
	if (n_pages > pbl->max_buf) {
		_NBE(siw_map_mr_e2, "Exceed max mr pages (%d, %u)\n",
			n_pages, pbl->max_buf);
		return -ENOMEM;
	}

	for (i = 0; i < n_pages; i++)
		pbl->pbe[i].addr = pages[i];
	pbl->num_buf = n_pages;
	pbl->ulp_map = true;

	return 0;
}
#else

//omril: alternatively ... use sg_alloc_table_from_pages() to ...
//create a SG list with the dma_address as page_address while taking care of
//first-page's address. This way it can use siw_map_mr_sg!
//pages[] is actually ib_sg_dma_address(), which is sg_dma_address() in 4.8.17,
//algned to page i.e. page-shift bits masked out.
//
//Note that ofa_mr->length and ofa_mr->iova will be overriden (add check if they
//changed and return err if so)
//
//Note that alloc_n_map's @pages will in-fact be virtual-address (see siw_dma_alloc_coherent())

/* Populate siw_mr's pbl.
   Caller must set mr's length, iova and page_size before calling
   this so we can calc the offset of the first and last block */
static int siw_map_mr(struct ib_device *ibdev, struct ib_mr *ofa_mr,
	phys_addr_t *pages, int n_pages)
{
	struct siw_mr *mr = siw_mr_ofa2siw(ofa_mr);
	struct siw_pbl *pbl = mr->pbl;
	struct siw_pble *pble = pbl->pbe;
	u64 pbl_size = 0;
	unsigned int mr_page_size = ofa_mr->page_size;
	u32 first_offset;
	u64 first_size, last_size, size;
	int last_page = n_pages - 1;
	int i, rv = -1;

	NFIN;

	_ND(trace_nvmeib_public_siw_imp_siw_map_mr, "ibdev=@IBDEV ofa_mr=@OFA_MR pages=@PAGES n_pages=@N_PAGES", ibdev, ofa_mr, pages, n_pages);

	if (!pages) {
		_NE(error_nvmeib_public_siw_imp_siw_map_mr, "no pages");
		rv = -EINVAL;
		goto out;
	}
	if (!pbl) {
		_NT(trace_1_nvmeib_public_siw_imp_siw_map_mr, "No mr-pages");
		rv = -EINVAL;
		goto out;
	}
	if (pbl->max_buf < n_pages) {
		_NT(trace_2_nvmeib_public_siw_imp_siw_map_mr, "Exceed max mr pages (@N_PAGES, @MAX_BUF)", n_pages, pbl->max_buf);
		rv = -ENOMEM;
		goto out;
	}
	if (!is_power_of_2(mr_page_size)) {
		_NT(trace_3_nvmeib_public_siw_imp_siw_map_mr, "mr_page_size not power of 2 (@MR_PAGE_SIZE)", mr_page_size);
		rv = -EINVAL;
		goto out;
	}

	if (1) {
		pbl->pbe_fixed_shift = ilog2(mr_page_size);
		//_NE_dmesg(__AUTOID__, "pbl->pbe_fixed_shift=@MR_PAGE_SIZE", pbl->pbe_fixed_shift);
	}
	else {
		pbl->pbe_fixed_shift = 0;
	}

	first_offset = ofa_mr->iova & (mr_page_size - 1);
	if (n_pages > 1) {
		first_size = mr_page_size - first_offset;
		last_size = ofa_mr->length - first_size - (n_pages - 2) * mr_page_size;
		if (last_size <= 0 || last_size > mr_page_size) {
			_NE(error_1_nvmeib_public_siw_imp_siw_map_mr,
				"Bad last-page-size=@SIZE_LLONG (mr_page_size=@MR_PAGE_SIZE, length=@LENGTH, "
			   "first_size=@FIRST_SIZE, n_pages=@N_PAGES)",
				last_size, mr_page_size, (u64)(ofa_mr->length), first_size, n_pages);
			rv = -EINVAL;
			goto out;
		}
	} else {
		/* Single-page MR: the entire transfer is contained within one
		 * mr_page_size-sized page starting at first_offset. The sole
		 * PBL entry's size is ofa_mr->length, NOT (mr_page_size -
		 * first_offset); otherwise the pbl_size == length check below
		 * rejects every sub-page transfer (e.g. a 4 KiB direct-IO on
		 * a 64 KiB-page ARM client where mr_page_size == 64 KiB).
		 */
		if (!ofa_mr->length || ofa_mr->length > mr_page_size - first_offset) {
			_NE(error_2_nvmeib_public_siw_imp_siw_map_mr,
				"Bad single-page MR: length=@LENGTH mr_page_size=@MR_PAGE_SIZE first_offset=@OFFSET_INT",
				(u64)(ofa_mr->length), mr_page_size, (unsigned long)first_offset);
			rv = -EINVAL;
			goto out;
		}
		first_size = ofa_mr->length;
		last_size = 0;
	}

	_ND(trace_4_nvmeib_public_siw_imp_siw_map_mr, "Mapping n_pages=@N_PAGES...", n_pages);
	for (i = 0; i < n_pages; i++) {
		unsigned long page_vaddr = virt_addr_valid(((void *)pages[i])) ? pages[i] : (unsigned long)phys_to_virt(pages[i]);

		if (page_vaddr & (mr_page_size - 1)) {
			_NT(trace_5_nvmeib_public_siw_imp_siw_map_mr, "page @PAGE_NUM is not aligned @PHYS", i, page_vaddr);
			rv = -EINVAL;
			goto out;
		}

		if (i == 0) {
			pble->addr = page_vaddr | first_offset;
			pble->size = first_size;
			pble->pbl_off = 0;
			pbl->num_buf = 1;
			size = pble->size;
		}
		else {
			size = (i == last_page) ? last_size : mr_page_size;
			/*
			 * [Gregory] disable PBL coalessing for now
			 * Use PBL entry for each page
			 */
			if (pbl->pbe_fixed_shift || (pble->addr + pble->size != page_vaddr)) {
				pble++;
				pbl->num_buf++;
				pble->addr = page_vaddr;
				pble->size = size;
				pble->pbl_off = pbl_size;

			}
			else
				pble->size += size;
		}

		pbl_size += size;

		_ND(trace_6_nvmeib_public_siw_imp_siw_map_mr, "mr @MR_PTR: page @PAGE_NUM, addr=@ADDR_PTR, size=@SIZE_LLONG, total @PBL_SIZE",
			mr, i, (void *)pble->addr, pble->size, pbl_size);
	}

	if (pbl_size != ofa_mr->length) {
		_NT(trace_7_nvmeib_public_siw_imp_siw_map_mr, "Total calc size (@PBL_SIZE) and mr's length (@LENGTH) differ",
			pbl_size, ofa_mr->length);
		rv = -EINVAL;
		goto out;
	}

	//omril:
	//in siw_map_mr_sg we take this values from ib_sg_to_pages()
	//not sure what ib_sg_to_pages set ofa_mr->length to.

	_ND(trace_8_nvmeib_public_siw_imp_siw_map_mr, "mr @MR_PTR: mapped @N_PAGES pages (#plbe=@PLBE), iova=@IOVA, length=@LENGTH",
		mr, n_pages, pbl->num_buf, ofa_mr->iova, ofa_mr->length);

	mr->mem.len = ofa_mr->length;
	mr->mem.va = ofa_mr->iova;
	rv = 0;
out:
	NFOUT;
	return rv;
}

#ifndef NVMEIB_PUBLIC_SIWP_C

/* Function to redirect SIW print to trace log */
/* TBD: Parse for tokens */

static void siw_dprint_fn(u32 dbgcat, int pid, int cpu, int in_interrupt,
			  const char *task_name,
			  const char *func,
			  const char *file, int line, 
			  const char *fmt, ...)
{
	va_list valist;
	va_start(valist, fmt);

	NVMEIB_LOG_LONGTERM("@SIW_FILE:@SIW_LINE [@SIW_FUNC] [@K_PID/@K_TASK_NAME@IN_INTERRUPT]: (@SIW_DBGCAT) @SIW_STR",
			_I, /*Default scope*/, trace_siw_dprint,
			file, line, func, pid, task_name, 
			(in_interrupt ? "/irq" : ""), dbgcat, fmt, valist);
	va_end(valist);
}

static int siw_redirect_dprint_init(void)
{
	int rv = 0;

	/* Set the fn */
	if ((rv = siw_set_dprint_fn(siw_dprint_fn)) < 0)
		_NT(trace_siw_set_dprint_fn_set_fail, "set_siw_dprint_fn failed (@RV)", rv);

	return rv;
}

static void siw_redirect_dprint_free(void)
{
	/* Clear the fn */
	siw_set_dprint_fn(NULL);
}
#endif //NVMEIB_PUBLIC_SIWP_C

#endif

