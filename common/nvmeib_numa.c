#include "nvmeibm_trace.h"
#include "nvmeib_utils.h"
#include "nvmeib_utils_bin_traces.h"
#include "nvmeib_numa.h"
#include "nvmeib_public.h"
#include "nvmeib_str.h"

unsigned nvmeib_numa_alloc_granularity = 64;
module_param_named(numa_alloc_granularity, nvmeib_numa_alloc_granularity, uint, 0644);
MODULE_PARM_DESC(numa_alloc_granularity, "Number of pages on the same node");

EXPORT_SYMBOL(nvmeib_numa_alloc_granularity);

#if defined(CONFIG_NUMA) && defined(__x86_64__)
#	define NVMEIB_NUMA_ALLOC_POLICY_DEFAULT 2
#else
#	define NVMEIB_NUMA_ALLOC_POLICY_DEFAULT 0
#endif

unsigned nvmeib_numa_alloc_policy = NVMEIB_NUMA_ALLOC_POLICY_DEFAULT;
module_param_named(numa_alloc_policy, nvmeib_numa_alloc_policy, uint, 0644);
MODULE_PARM_DESC(numa_alloc_policy,
	"NUMA node choice policy in case of large allocations:\n"
	"\t\t 0 : Default\n"
	"\t\t 1 : Round robin on all nodes\n"
	"\t\t 2 : Round robin on nodes within the same socket as an appropriate device");

EXPORT_SYMBOL(nvmeib_numa_alloc_policy);

struct list_head nvmeib_numa_iter_diag_store;
spinlock_t nvmeib_numa_iter_diag_store_guard;

void nvmeib_numa_iter_diag_store_init(void) {
	INIT_LIST_HEAD(&nvmeib_numa_iter_diag_store);
	spin_lock_init(&nvmeib_numa_iter_diag_store_guard);
}
EXPORT_SYMBOL(nvmeib_numa_iter_diag_store_init);

void nvmeib_numa_iter_diag_store_destroy(void) {
	while (!list_empty(&nvmeib_numa_iter_diag_store)) {
		struct nvmeib_numa_alloc_diag *diag = list_first_entry(
		    &nvmeib_numa_iter_diag_store, struct nvmeib_numa_alloc_diag, link);
		list_del_init(&diag->link);
		kfree(diag);
	}
}
EXPORT_SYMBOL(nvmeib_numa_iter_diag_store_destroy);

ssize_t nvmeib_numa_iter_diag_fill(void *dummy, char *buffer, size_t len) {
	unsigned long flags;
	int count = 0;
	struct nvmeib_numa_alloc_diag *diag;

	spin_lock_irqsave(&nvmeib_numa_iter_diag_store_guard, flags);

	list_for_each_entry(diag, &nvmeib_numa_iter_diag_store, link) {
		int node;
		count +=
		    scnprintf(buffer + count, len - count, "%20s|", diag->dev_name);
		for_each_node_with_cpus(node) {
			if (node < ARRAY_SIZE(diag->alloc))
				count += scnprintf(buffer + count, len - count, "%2d:%8llu|",
				                   node, diag->alloc[node]);
		}
		count += scnprintf(buffer + count, len - count, "\n");
		count += scnprintf(buffer + count, len - count, "%15s fbck| %llu\n",
		                   diag->dev_name, diag->fallback);
	}

	spin_unlock_irqrestore(&nvmeib_numa_iter_diag_store_guard, flags);

	return count;
}
EXPORT_SYMBOL(nvmeib_numa_iter_diag_fill);

static void nvmeib_numa_iter_next_rr_no_granularity(struct nvmeib_numa_iter *iter) {
	NFIN;
	if (iter->policy == NVMEIB_NUMA_POLICY_THIS_CPU_NODE) return;
	iter->node = next_node(iter->node, iter->data.nodemask);
	if (iter->node >= MAX_NUMNODES)
		iter->node = first_node(iter->data.nodemask);
	while (!nr_cpus_node(iter->node)) {
		/* Skip nodes without CPUS */
		iter->node = next_node(iter->node, iter->data.nodemask);
		if (iter->node >= MAX_NUMNODES)
			iter->node = first_node(iter->data.nodemask);
	}
	NFOUT;
}

static void nvmeib_numa_iter_init_default(struct nvmeib_numa_iter *iter) {
	NFIN;
	iter->policy = NVMEIB_NUMA_POLICY_THIS_CPU_NODE;
	iter->node = numa_node_id();
	NFOUT;
}

static void
nvmeib_numa_iter_init_first_node_with_cpus(struct nvmeib_numa_iter *iter) {
	iter->node = first_node(iter->data.nodemask);
	BUG_ON(iter->node >= MAX_NUMNODES);
	while (!nr_cpus_node(iter->node)) {
		/* Skip nodes without CPUS */
		iter->node = next_node(iter->node, iter->data.nodemask);
		if (iter->node >= MAX_NUMNODES)
			BUG(); /* What? Not a single node with CPUs? */
	}

	{
		static atomic_t bias = ATOMIC_INIT(0);
		unsigned shift = atomic_inc_return(&bias);
		int i;

		shift = shift % nodes_weight(iter->data.nodemask);
		for (i = 0; i < shift; ++i)
			nvmeib_numa_iter_next_rr_no_granularity(iter);

	}
}

static void nvmeib_numa_iter_init_round_robin(struct nvmeib_numa_iter *iter) {
	NFIN;
	iter->policy = NVMEIB_NUMA_POLICY_RR_ALL_NODES;
	iter->data.nodemask = node_states[N_ONLINE];
	/* @TODO: nvmeib_numa_alloc_granularity should be an argument to function */
	iter->gran.target = nvmeib_numa_alloc_granularity;
	iter->gran.running = 0;
	nvmeib_numa_iter_init_first_node_with_cpus(iter);
	NFOUT;
}

int nvmeib_socket_from_numa(int dev_node) {
	/* Find dev sock (if you know a better way to do it I am all ears) */
	int dev_sock = 0;
	int cpu;
	for_each_online_cpu(cpu) {
		const int cpu_node = cpu_to_node(cpu);
		const int cpu_sock = nvmeib_public_cpu_to_sock(cpu);
		if (cpu_node == dev_node) {
			dev_sock = cpu_sock;
			break;
		}
	}
	return dev_sock;
}

EXPORT_SYMBOL(nvmeib_socket_from_numa);

#ifdef CONFIG_NUMA
static void nvmeib_numa_iter_init_device_socket(struct nvmeib_numa_iter *iter,
                                                int dev_node) {
	int dev_sock = 0;
	int cpu;

	NFIN;

	dev_sock = nvmeib_socket_from_numa(dev_node);

	nodes_clear(iter->data.nodemask);

	for_each_online_cpu(cpu) {
		const int cpu_node = cpu_to_node(cpu);
		const int cpu_sock = nvmeib_public_cpu_to_sock(cpu);
		if (dev_sock == cpu_sock) {
			node_set(cpu_node, iter->data.nodemask);
		}
	}

	if (nodes_empty(iter->data.nodemask)) {
		WARN_ONCE(true, "Empty sockets bitmask, fallback.");
		nvmeib_numa_iter_init_default(iter);
		goto out;
	}

	/* @TODO: nvmeib_numa_alloc_granularity should be an argument to function */
	iter->gran.target = nvmeib_numa_alloc_granularity;
	iter->gran.running = 0;

	nvmeib_numa_iter_init_first_node_with_cpus(iter);

	iter->policy = NVMEIB_NUMA_POLICY_RR_DEV_SOCKET_NODES;
out:
	NFOUT;
}
#endif

static struct nvmeib_numa_alloc_diag *
nvmeib_numa_iter_get_diag(struct device *dev, gfp_t gfp) {
	unsigned long flags;
	struct nvmeib_numa_alloc_diag *diag, *new_diag;
	const char *name = dev_name(dev);
	NFIN;
	/* Allocate before we take the spinlock (prevent scheduling while atomic) */
	new_diag = kzalloc(sizeof(*diag), gfp);
	spin_lock_irqsave(&nvmeib_numa_iter_diag_store_guard, flags);
	list_for_each_entry(diag, &nvmeib_numa_iter_diag_store, link) {
		if (!strncmp(diag->dev_name, name, ARRAY_SIZE(diag->dev_name))) {
			goto unlock;
		}
	}
	if (&diag->link == &nvmeib_numa_iter_diag_store) {
		/* Not found in list - Add new */
		if (new_diag) {
			strlcpy(new_diag->dev_name, name, ARRAY_SIZE(new_diag->dev_name));
			list_add_tail(&new_diag->link, &nvmeib_numa_iter_diag_store);
		}
		/* Swap diag and new_diag so we don't free it later */
		diag = new_diag;
		new_diag = NULL;
	}
unlock:
	spin_unlock_irqrestore(&nvmeib_numa_iter_diag_store_guard, flags);
	/* free new_diag (if not set to null by add new flow) */
	kfree(new_diag);
	NFOUT;
	return diag;
}

void nvmeib_numa_iter_init(struct nvmeib_numa_iter *iter,
                           enum nvmeib_numa_alloc_policy_type policy,
                           struct device *dev) {
	NFIN;

	if (!dev && policy == NVMEIB_NUMA_POLICY_RR_DEV_SOCKET_NODES) {
		_NW(nvmeib_numa_iter_init_override_policy,
			"No dev, override numa-alloc-policy 2 -> 1,"
			"called from '@__BUILTIN_RETURN_ADDRESS_FUNC'",
			__builtin_return_address(0));
		policy = NVMEIB_NUMA_POLICY_RR_ALL_NODES;
		WARN_ON_ONCE(1);
	}

	switch (policy) {
		case NVMEIB_NUMA_POLICY_THIS_CPU_NODE:
			nvmeib_numa_iter_init_default(iter);
			break;
		case NVMEIB_NUMA_POLICY_RR_ALL_NODES:
			nvmeib_numa_iter_init_round_robin(iter);
			break;
	#ifdef CONFIG_NUMA
		case NVMEIB_NUMA_POLICY_RR_DEV_SOCKET_NODES:
			BUG_ON(!dev);
			nvmeib_numa_iter_init_device_socket(iter, dev->numa_node);
			break;
	#endif
		default:
			WARN_ONCE(true, "Invalid NUMA policy %d, fallback.\n", policy);
			nvmeib_numa_iter_init_default(iter);
			break;
	}
	iter->dev = dev;
	if (iter->dev) {
		iter->diag = nvmeib_numa_iter_get_diag(dev, GFP_KERNEL); /* May return NULL on
		                                             failure, will ignore it */
		WARN_ONCE(!iter->diag, "Error allocating diag for device %s\n",
		          dev_name(iter->dev));
	}
	else
		iter->diag = NULL;
	NFOUT;
}
EXPORT_SYMBOL(nvmeib_numa_iter_init);

void nvmeib_numa_iter_next_rr(struct nvmeib_numa_iter *iter, int step) {
	NFIN;
	iter->gran.running += step;
	if (iter->gran.running >= iter->gran.target) {
		iter->gran.running = 0;
		nvmeib_numa_iter_next_rr_no_granularity(iter);
	}
	NFOUT;
}
EXPORT_SYMBOL(nvmeib_numa_iter_next_rr);

static struct page *
nvmeib_numa_iter_alloc_order_pages(struct nvmeib_numa_iter *iter, int order,
                                   gfp_t gfp) {
	struct page *pg;
	struct nvmeib_numa_alloc_diag *diag = iter->diag;

	NFIN;
	pg = alloc_pages_node(iter->node, gfp, order);
	if (pg == NULL) {
		WARN_ONCE(true, "Could not allocate page on NUMA %d", iter->node);
		if (diag) diag->fallback++;
		/* No more mem on specific numa, fallback */
		pg = alloc_pages(gfp, order);
	}
	if (pg == NULL) {
		_NE(error_nvmeib_numa_iter_alloc_order_pages,
		    "Could not allocate memory page");
		goto out;
	}
	if (diag && iter->node < ARRAY_SIZE(diag->alloc))
		diag->alloc[iter->node] += (1 << order);
out:
	NFOUT;
	return pg;
}

int nvmeib_numa_iter_alloc_n_pages(struct nvmeib_numa_iter *iter,
				   struct page **pages, int n_pages,
                                   u64 init_val, gfp_t gfp)
{
	int i, rv;

	NFIN;

	if (!pages) {
		WARN_ON(1);
		rv = -EINVAL;
		goto out;
	}

	for (i = 0; i < n_pages; i++) {
		pages[i] = nvmeib_numa_iter_alloc_order_pages(iter, 0, gfp);
		if (pages[i] == NULL) {
			rv = -ENOMEM;
			goto free_mem;
		}
		if (init_val != 0) {
			u64 *u64ptr;
			for (u64ptr = (u64 *)page_address(pages[i]); u64ptr < (u64 *)(page_address(pages[i]) + PAGE_SIZE);
			     u64ptr++) {
				*u64ptr = init_val;
			}
		}
		nvmeib_numa_iter_next_rr(iter, 1);
	}

	rv = 0;
	goto out;

free_mem:
	nvmeib_numa_iter_free_n_pages(pages, n_pages);
out:
	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_numa_iter_alloc_n_pages);

void nvmeib_numa_iter_free_n_pages(struct page **pages, int n_pages) {
	int i;

	NFIN;
	if (pages) {
		for (i = 0; i < n_pages; i++) {
			if (pages[i])
				__free_page(pages[i]);
		}
	}
	NFOUT;
}
EXPORT_SYMBOL(nvmeib_numa_iter_free_n_pages);

void *nvmeib_numa_iter_dma_alloc(struct nvmeib_numa_iter *iter,
                                 struct ib_device *dev, size_t size,
                                 dma_addr_t *dma_handle, gfp_t gfp) {
	struct page *page = NULL;
	int order;
	size = PAGE_ALIGN(size);
	order = get_order(size);

	page = nvmeib_numa_iter_alloc_order_pages(iter, order, gfp);
	if (!page) goto out;
	memset(page_address(page), 0, size);

	*dma_handle = ib_dma_map_single(dev, page_address(page), size,
	                             DMA_BIDIRECTIONAL);
	if (dev->dma_device)
		if (dma_mapping_error(dev->dma_device, *dma_handle)) goto free_mem;

	goto out;
free_mem:
	__free_pages(page, order);
	page = NULL;
out:
	NFOUT;
	if (page) return page_address(page);
	return NULL;
}
EXPORT_SYMBOL(nvmeib_numa_iter_dma_alloc);

void nvmeib_numa_iter_dma_free(struct ib_device *dev, size_t size,
                               void *cpu_addr, dma_addr_t dma_handle) {
	int order;
	struct page *page = virt_to_page(cpu_addr);

	size = PAGE_ALIGN(size);
	order = get_order(size);

	if(dev->dma_device)
		dma_unmap_single(dev->dma_device, dma_handle, size, DMA_BIDIRECTIONAL);
	__free_pages(page, order);
}
EXPORT_SYMBOL(nvmeib_numa_iter_dma_free);

int nvmeib_alloc_pages_respecting_numa_policy(
    struct nvmeib_alloc_n_map *mem, gfp_t gfp) {
	int rv;
	struct nvmeib_numa_iter niter;
	struct page **pages;

	NFIN;
	
	if (mem->pages) {
		_NE(err_nvmeib_alloc_pages_respecting_numa_policy_pgs_not_empty,
		    "pages already initialised");
		rv = -EALREADY;
		goto out;
	}

	if (mem->mem_table.nents || mem->mem_table.sgl) {
		_NE(err_nvmeib_alloc_pages_respecting_numa_policy_sgl_non_empty,
		    "SG already initialised");
		rv = -EALREADY;
		goto out;
	}
	
	if (!(pages = kcalloc(mem->n_pages, sizeof(*pages), gfp))) {
		_NE(err_nvmeib_alloc_pages_respecting_numa_policy_pgs_alloc, 
		    "pages allocation failure");
		rv = -ENOMEM;
		goto free_mem;
	}
	
	if (mem->alloc_policy == NVMEIB_NUMA_POLICY_THIS_CPU_NODE) {
		int i;
		for (i = 0; i < mem->n_pages; i++) {
			if (!(pages[i] = alloc_pages(gfp, 0))) {
				_NE(err_2_nvmeib_alloc_pages_respecting_numa_policy_pgs_alloc, 
				    "pages allocation failure");
				rv = -ENOMEM;

				/* Free the pages allocated so far */
				for (i--; i >= 0; i--)
					__free_pages(pages[i], 0);
				goto free_pg_arr;
			}
		}
		mem->pages = pages;
		rv = 0;
		goto out;
	}

	nvmeib_numa_iter_init(&niter, mem->alloc_policy,
							IBDEV2DMADEV(mem->pd->device));

	rv = nvmeib_numa_iter_alloc_n_pages(&niter, pages, mem->n_pages, 0, gfp);
	if (rv < 0)
		goto free_mem;
	
	mem->pages = pages;
	rv = 0;
	goto out;

free_mem:
	nvmeib_numa_iter_free_n_pages(pages, mem->n_pages);

free_pg_arr:
	kfree(pages);

out:
	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_alloc_pages_respecting_numa_policy);

void nvmeib_free_pages_respecting_numa_policy(
    struct nvmeib_alloc_n_map *mem) {
	int i;
	NFIN;
	if (mem->alloc_policy == NVMEIB_NUMA_POLICY_THIS_CPU_NODE) {
		for (i = 0; i < mem->n_pages; i++)
			__free_pages(mem->pages[i], 0);
	} else {
		nvmeib_numa_iter_free_n_pages(mem->pages, mem->n_pages);
	}
	kfree(mem->pages);
	mem->pages = NULL;
	NFOUT;
}
EXPORT_SYMBOL(nvmeib_free_pages_respecting_numa_policy);
