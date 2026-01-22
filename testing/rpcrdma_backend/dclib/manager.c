/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include "xkr_incs.h"
#include "xkr_version.h"
#include "xib_incs.h"
#include "main.h"
#include "manager.h"
#include "utils.h"
#include "poller.h"
#include "wth.h"
#include "rdma_dev.h"
#include "larray.h"
#include "server_tester.h"
#include "client_tester.h"
#include "xtrace.h"

#include <linux/string.h>
#include <linux/parser.h>
#include <linux/mlx5/qp.h>
#include <linux/inet.h>
#include <rdma/ib.h>

#define MAIN_PROC_DIR_STR "rpcrdma_backend"
#define MAIN_PROC_START_ENTRY_STR "start"
#define MAIN_PROC_SERVER_ENTRY_STR "server"
#define MAIN_PROC_CLIENT_ENTRY_STR "client"
#define MAIN_PROC_IP_CHAR 'E'
#define MAIN_PROC_IB_CHAR 'I'
#define MAIN_PROC_IB_ADDR_BUFFER 64

static void add_one(struct ib_device *device);
static void remove_one(struct ib_device *device, void *v);

/* the currently registered services */
struct _services {
	LARRAY_DEFINE(services, struct rdma_service, 10);
};

/* the entry in the below  p_rdma_devs & u_rdma_devs */
struct per_cpu_link {
	/* the RDMA device */
	struct per_core_rdma *d;
	/* the core number */
	int cpu;
	struct list_head link;
};

struct per_cpu {
	/* hold a read-only pointer to the internal structure of the services */
	struct manager_per_cpu base;
	struct _services *services;
	/* a list of local preferred RDMA devices per core */
	struct list_head p_rdma_devs;
	/* a list of local distant RDMA devices per core */
	struct list_head u_rdma_devs;
};

/* for each path we maintain two device lists per core:
   p_rdma_devs: the local device i prefer to use to reach that destination from
				the current core
   u_rdma_devs: the local device that are on a different node than
				the current CPU
   
   we also maintain a per_cpu index in an array of destination core.  we use
   that index to balance the load on the receiving-side NIC
*/
struct per_cpu_path {
	void *path;
	/* index to the remote DCT inside path so we load balance receives
	   among all remote side DCTs
	 */
	int index_remote_dct;
	struct list_head p_rdma_devs;
	struct list_head u_rdma_devs;
};

/* an entry in the main object paths list */
struct per_cpu_path_link {
	void *path;
	struct per_cpu_path __percpu *percpu;
	struct list_head link;
};

struct server_tester;
struct client_tester;
struct manager {
	struct ib_sa_client sa_cli;
	struct ib_client client;
	struct proc_dir_entry *dir;
	void * start_proc_file;
	void * server_proc_file;
	void * client_proc_file;
	struct list_head devs;
	struct wth *wth;
	struct per_cpu __percpu *pcpu;
	struct completion start_stop_comp;
	bool poller_started;
	bool started;
	bool registered;
	struct sockaddr_storage s_sin;
	struct sockaddr_storage c_sin;
	char layer_type;
	struct list_head paths;
	struct list_head rpaths;
	spinlock_t guard;
	struct server_tester *st;
	struct client_tester *ct;
};

/**
 * Start manager requests
 */

struct add_device_request {
	struct request_base r;
	struct ib_device *device;
};

static void add_device_request_free(struct request_base *r)
{
	kfree(container_of(r, struct add_device_request, r));
}

struct remove_device_request {
	struct request_base r;
	struct ib_device *device;
	struct completion *c;
};

static void remove_device_request_free(struct request_base *r)
{
	kfree(container_of(r, struct remove_device_request, r));
}

struct rdma_dev_exit_request {
	struct request_base r;
	struct rdma_dev *dev;
};

static void rdma_dev_exit_request_free(struct request_base *r)
{
	kfree(container_of(r, struct rdma_dev_exit_request, r));
}

struct register_new_address_request {
	struct request_base r;
	void (*f)(void *ctx, struct local_address_info *a);
	void *ctx;
	struct completion *c;
};

static void register_new_address_request_free(struct request_base *r)
{
	kfree(container_of(r, struct register_new_address_request, r));
}

struct register_server_request {
	struct request_base r;
	struct sockaddr_storage src;
	struct server_info server;
	void *local_rdma_dev;
};

static void register_server_request_free(struct request_base *r)
{
	kfree(container_of(r, struct register_server_request, r));
}

struct register_client_request {
	struct request_base r;
	struct sockaddr_storage src;
	struct sockaddr_storage dst;
	struct client_info client;
	void *local_rdma_dev;
};

static void register_client_request_free(struct request_base *r)
{
	kfree(container_of(r, struct register_client_request, r));
}

struct register_service_request {
	struct request_base r;
	struct server_service service;
	union service_id *sid;
	struct completion *c;
};

static void register_service_request_free(struct request_base *r)
{
	kfree(container_of(r, struct register_service_request, r));
}

struct unregister_service_request {
	struct request_base r;
	union service_id sid;
	struct completion *c;
};

static void unregister_service_request_free(struct request_base *r)
{
	kfree(container_of(r, struct unregister_service_request, r));
}

/**
 * End manager requests
 */

static int add_device(void *p, struct request_base *r)
{
	struct manager *o = p;
	struct rdma_dev *dev;
	struct rdma_dev_info info = {};
	struct add_device_request *rr =
		container_of(r, struct add_device_request, r);

	FIN;
	info.owner = o;
	info.ib_dev = rr->device;
	info.max_receive_msg_size = PAGE_SIZE;
	if ((dev = rdma_dev_create(&info))) {
		xdtrace("Adding rdma_device %s\n", info.ib_dev->name);
		dev->priv = o;
		list_add_tail(&dev->link, &o->devs);
	}
	FOUT;
	return 0;
}

static int remove_device(void *p, struct request_base *r)
{
	struct manager *o = p;
	struct rdma_dev *d;
	struct remove_device_request *rr =
		container_of(r, struct remove_device_request, r);
	bool found = false;

	FIN;
	list_for_each_entry(d, &o->devs, link) {
		if (d->ib_dev == rr->device) {
			found = true;
			d->remove_done = rr->c;
			rdma_dev_stop(d);
		}
	}
	if (!found)
		complete(rr->c);
	FOUT;
	return 0;
}

static void add_one(struct ib_device *device)
{
	struct manager *o = rpcrdma_backend_get_obj();
	struct add_device_request *r;

	FIN;
	if (o) {
		if ((r = kzalloc(sizeof(*r), GFP_KERNEL))) {
			r->r.type = rpcrdma_add_device;
			r->r.call = add_device;
			r->r.free = add_device_request_free;
			r->device = device;
			if (!wth_is_current(o->wth))
				wth_push_request(o->wth, &r->r);
			else
				BUG();
		}
		else
			xetrace("Failed to allocate add_device request\n");
	}
	else
		xetrace("Main object was not set\n");
	FOUT;
}

static void remove_one(struct ib_device *device, void *v)
{
	struct manager *o = rpcrdma_backend_get_obj();
	struct remove_device_request *r;
	DECLARE_COMPLETION_ONSTACK(comp);

	FIN;
	if (o) {
		if ((r = kzalloc(sizeof(*r), GFP_KERNEL))) {
			r->r.type = rpcrdma_remove_device;
			r->r.call = remove_device;
			r->r.free = remove_device_request_free;
			r->device = device;
			r->c = &comp;
			if (!wth_is_current(o->wth)) {
				wth_push_request(o->wth, &r->r);
				wait_for_completion(&comp);
			}
			else {
				remove_device(o, &r->r);
				kfree(r);
			}
		}
		else
			xetrace("Failed to allocate remove_device request\n");
	}
	else
		xetrace("Main object was not set\n");
	FOUT;
}

static void start(struct manager *o)
{
	FIN;
	if (rpcrdma_backend_get_obj() == o) {
		o->registered = true;
		ib_sa_register_client(&o->sa_cli);
		ib_register_client(&o->client);
	}
	FOUT;
}

static void stop(struct manager *o)
{
	FIN;
	if (o->registered) {
		ib_unregister_client(&o->client);
		ib_sa_unregister_client(&o->sa_cli);
		wait_for_completion(&o->start_stop_comp);
	}
	FOUT;
}

static ssize_t proc_start(void *p, char *page, size_t count)
{
	struct manager *o = p;

	FIN;
	start(o);
	FOUT;
	return count;
}

static ssize_t server_store_ipv4_address(
	struct manager *o, char *page, size_t count)
{
	int i, ret;
	unsigned int octets_s[4];
	__be32 s_ipv4_address = 0;
	unsigned int port_s = 0;
	struct sockaddr_in *q;

	FIN;
	ret = sscanf(page, "%c:%3u.%3u.%3u.%3u:%5u", &o->layer_type,
		&octets_s[3], &octets_s[2], &octets_s[1], &octets_s[0], &port_s);
	if (ret != 5) {
		xetrace("fail to read server bind address\n");
		ret = -EINVAL;
		goto out;
	}
	for (i = 0; i < ARRAY_SIZE(octets_s); i++) {
		if (octets_s[i] > 255) {
			ret = -ERANGE;
			goto out;
		}
		be32_add_cpu(&s_ipv4_address, octets_s[i] << (i * 8));
	}
	if (port_s == 0) {
		ret = -EINVAL;
		goto out;
	}
	if (port_s >= (u16)-1) {
		ret = -ERANGE;
		goto out;
	}
	q = (struct sockaddr_in *)&o->s_sin;
	q->sin_family = PF_INET;
	q->sin_addr.s_addr = s_ipv4_address;
	q->sin_port = htons(port_s);
	xetrace("server ip:port is %pIS:%u\n",
		(struct sockaddr *)&o->s_sin, ntohs(q->sin_port));

out:
	FOUT;
	return count;
}

static ssize_t server_store_ib_address(
	struct manager *o, char *page, size_t count)
{
	int ret;
	char srv[MAIN_PROC_IB_ADDR_BUFFER] = {0};
	struct sockaddr_ib *q;

	FIN;
	xetrace("count = %ld\n", count);
	ret = sscanf(page, "%c:%s", &o->layer_type, srv);
	if (ret != 2) {
		ret = -EINVAL;
		goto out;
	}
	xetrace("server input gid %s\n", srv);
	q = (struct sockaddr_ib *)&o->s_sin;
	if (in6_pton(srv, -1, q->sib_addr.sib_raw, -1, NULL))
		xetrace("server bind GID is %pI6\n", q->sib_addr.sib_raw);
	else {
		xetrace("fail to convert server address into a valid IB addrsss\n");
		goto out;
	}
	q->sib_family = AF_IB;
	q->sib_sid = cpu_to_be64(NVMESH_SERVICE_ID);
	q->sib_sid_mask = cpu_to_be64(NVMESH_SERVICE_ID_MASK);
	q->sib_pkey = cpu_to_be16(NVMESH_PKEY);
	xetrace("server GID is %pI6\n",
		((struct sockaddr_ib *)&o->s_sin)->sib_addr.sib_raw);

out:
	FOUT;
	return count;
}

static ssize_t proc_server(void* p, char* page, size_t count)
{
	struct manager *o = p;
	char c;

	FIN;
	if (false) {
		if (sscanf(page, "%c", &c) != 1)
			xetrace("failed to get the network layer type\n");
		else if (c == MAIN_PROC_IP_CHAR)
			server_store_ipv4_address(o, page, count);
		else if (c == MAIN_PROC_IB_CHAR)
			server_store_ib_address(o, page, count);
		else
			xetrace("Unsupported network layer type %c\n", c);
	}
	else if (!o->st)
		o->st = server_tester_create(o);
	FOUT;
	return count;
}

static ssize_t client_store_ipv4_address(
	struct manager *o, char *page, size_t count)
{
	int i, ret;
	unsigned int octets_c[4];
	unsigned int octets_s[4];
	__be32 c_ipv4_address = 0;
	__be32 s_ipv4_address = 0;
	unsigned int port_s = 0;
	struct sockaddr_in *q;

	FIN;
	ret = sscanf(page, "%c:%3u.%3u.%3u.%3u,%3u.%3u.%3u.%3u:%5u",
		&o->layer_type,
		&octets_c[3], &octets_c[2], &octets_c[1], &octets_c[0],
		&octets_s[3], &octets_s[2], &octets_s[1], &octets_s[0], &port_s);
	if (ret != 9) {
		ret = -EINVAL;
		goto out;
	}

	for (i = 0; i < ARRAY_SIZE(octets_c); i++) {
		if (octets_c[i] > 255) {
			ret = -ERANGE;
			goto out;
		}
		be32_add_cpu(&c_ipv4_address, octets_c[i] << (i * 8));
	}
	xetrace("client bind ip is %pI4\n", &c_ipv4_address);
	for (i = 0; i < ARRAY_SIZE(octets_s); i++) {
		if (octets_s[i] > 255) {
			ret = -ERANGE;
			goto out;
		}
		be32_add_cpu(&s_ipv4_address, octets_s[i] << (i * 8));
	}
	if (port_s == 0) {
		ret = -EINVAL;
		goto out;
	}
	if (port_s >= (u16)-1) {
		ret = -ERANGE;
		goto out;
	}
	q = (struct sockaddr_in *)&o->s_sin;
	q->sin_family = PF_INET;
	q->sin_addr.s_addr = s_ipv4_address;
	q->sin_port = htons(port_s);
	q = (struct sockaddr_in *)&o->c_sin;
	q->sin_family = PF_INET;
	q->sin_addr.s_addr = c_ipv4_address;
	q->sin_port = 0;
	xetrace("client ip %pIS server ip:port is %pIS:%u\n",
		(struct sockaddr *)&o->s_sin,
		(struct sockaddr *)&o->c_sin,
		ntohs(((struct sockaddr_in *)&o->s_sin)->sin_port));
	if (!o->ct)
		o->ct = client_tester_create(o, &o->c_sin);

out:
	FOUT;
	return count;
}

static ssize_t client_store_ib_address(
	struct manager *o, char *page, size_t count)
{
	int ret;
	char line[MAIN_PROC_IB_ADDR_BUFFER * 2 + 1] = {0};
	char *copied_line;
	char *splitted_str;
	char *pp;
	char clt[MAIN_PROC_IB_ADDR_BUFFER] = {0};
	char srv[MAIN_PROC_IB_ADDR_BUFFER] = {0};
	bool copied_clt = false;
	bool copied_srv = false;
	struct sockaddr_ib *q;

	FIN;
	ret = sscanf(page, "%c:%s", &o->layer_type, line);
	if (ret != 2) {
		xetrace("sscanf returned %d - should return 2\n", ret);
		ret = -EINVAL;
		goto out;
	}
	if (!(copied_line = kstrdup(line, GFP_KERNEL))) {
		xetrace("kstrdup(0 failed\n");
		ret = -ENOMEM;
		goto out;
	}
	else
		splitted_str = copied_line;
	while ((pp = strsep(&splitted_str, ",\n")) != NULL) {
		if (!*pp)
			continue;
		if (!copied_clt) {
			strncpy(clt, pp, MAIN_PROC_IB_ADDR_BUFFER - 1);
			copied_clt = true;
		}
		else if (!copied_srv) {
			strncpy(srv, pp, MAIN_PROC_IB_ADDR_BUFFER - 1);
			copied_srv = true;
		}
		if (copied_clt && copied_srv)
			break;
	}
	kfree(copied_line);
	if (!(copied_clt && copied_srv)) {
		xetrace("failed to fetch: client: %s, server: %s\n",
			copied_clt ? "OK" : "FAILED", copied_srv ? "OK" : "FAILED");
		ret = -EINVAL;
		goto out;
	}
	xetrace("client input gid %s\n", clt);
	q = (struct sockaddr_ib *)&o->c_sin;
	if (in6_pton(clt, -1, q->sib_addr.sib_raw, -1, NULL))
		xetrace("client bind GID is %pI6\n", q->sib_addr.sib_raw);
	else {
		xetrace("fail to convert client bind address into "
				"a valid IB addrsss\n");
		goto out;
	}
	q->sib_family = AF_IB;
	/* any port for the client bind address */
	q->sib_sid = cpu_to_be64(
		((0 & NVMESH_SERVICE_ID_MASK) | RDMA_IB_IP_PS_IB));
	q->sib_sid_mask = cpu_to_be64(NVMESH_SERVICE_ID_MASK);
	q->sib_pkey = cpu_to_be16(NVMESH_PKEY);
	xetrace("server input gid %s\n", srv);
	q = (struct sockaddr_ib *)&o->s_sin;
	if (in6_pton(srv, -1, q->sib_addr.sib_raw, -1, NULL))
		xetrace("server GID is %pI6\n", q->sib_addr.sib_raw);
	else {
		xetrace("fail to convert server address into a valid IB addrsss\n");
		goto out;
	}
	q->sib_family = AF_IB;
	q->sib_sid = cpu_to_be64(
		((NVMESH_SERVICE_ID & NVMESH_SERVICE_ID_MASK) |
		 RDMA_IB_IP_PS_IB));
	q->sib_sid_mask = cpu_to_be64(NVMESH_SERVICE_ID_MASK);
	q->sib_pkey = cpu_to_be16(NVMESH_PKEY);

	xttrace("client GID %pI6 server GID is %pI6\n",
		((struct sockaddr_ib *)&o->c_sin)->sib_addr.sib_raw,
		((struct sockaddr_ib *)&o->s_sin)->sib_addr.sib_raw);
	xttrace("q->sib_sid=%llx\n", be64_to_cpu(q->sib_sid));
	if (!o->ct)
		o->ct = client_tester_create(o, &o->s_sin);

out:
	FOUT;
	return count;
}

static ssize_t proc_client(void *p, char *page, size_t count)
{
	struct manager *o = p;
	char c;

	FIN;
	if (sscanf(page, "%c", &c) != 1)
		xetrace("failed to get the network layer type\n");
	else if (c == MAIN_PROC_IP_CHAR)
		client_store_ipv4_address(o, page, count);
	else if (c == MAIN_PROC_IB_CHAR)
		client_store_ib_address(o, page, count);
	else
		xetrace("Unsupported network layer type %c\n", c);
	FOUT;
	return count;
}

static void on_start(void *v)
{
	struct manager *o = v;

	FIN;
	o->started = true;
	complete(&o->start_stop_comp);
	FOUT;
}

static void on_exit(void *v)
{
	//struct manager *o = v;

	FIN;
	FOUT;
}

static int create_per_cpu(struct manager *o)
{
	struct per_cpu __percpu *percpu;
	struct per_cpu *pcpu;
	struct _services *s;
	int cpu;
	int i, rv;

	FIN;
	percpu = alloc_percpu(struct per_cpu);
	if (percpu == NULL) {
		rv = -ENOMEM;
		xetrace("Failed to allocate per_cpu data\n");
		goto out;
	}
	for_each_online_cpu(cpu) {
		pcpu = per_cpu_ptr(percpu, cpu);
		memset(pcpu, 0, sizeof(*pcpu));
		if ((s = kzalloc(sizeof(*s), GFP_KERNEL))) {
			LARRAY_INIT(s->services);
			for (i = 0; i < ARRAY_SIZE(s->services.a); ++i)
				x_ref_init(&s->services.a[i].in_use);
			pcpu->base.srvs = s->services.a;
			pcpu->base.len = ARRAY_SIZE(s->services.a);
			pcpu->services = s;
		}
		else {
			rv = -ENOMEM;
			goto freee;
		}
		spin_lock_init(&pcpu->base.guard);
		INIT_LIST_HEAD(&pcpu->p_rdma_devs);
		INIT_LIST_HEAD(&pcpu->u_rdma_devs);
	}
	o->pcpu = percpu;
	rv = 0;
	goto out;

freee:
	for_each_online_cpu(cpu) {
		pcpu = per_cpu_ptr(percpu, cpu);
		if (pcpu->services)
			kfree(pcpu->services);
	}
	free_percpu(percpu);

out:
	FOUT;
	return rv;
}

static void free_per_cpu(struct manager *o)
{
	struct per_cpu __percpu *percpu = o->pcpu;
	struct per_cpu *pcpu;
	int cpu;

	FIN;
	if (!list_empty(&o->paths) || !list_empty(&o->rpaths))
		xetrace("Removing in use rdma paths\n");
	for_each_online_cpu(cpu) {
		pcpu = per_cpu_ptr(percpu, cpu);
		if (!list_empty(&pcpu->p_rdma_devs) ||
			!list_empty(&pcpu->u_rdma_devs)) {
			xetrace("Removing in use per_cpu rdma_dev resources\n");
		}
	}
	free_percpu(o->pcpu);
	FOUT;
}

struct manager_per_cpu * manager_get_services_cpu(
	struct manager *o, int cpu)
{
	return &((struct per_cpu *)per_cpu_ptr(o->pcpu, cpu))->base;
}

struct manager * manager_create(struct manager_init_params *p)
{
	struct manager *o;
	struct wth_info info = {0};

	FIN;
	if (!(o = kzalloc(sizeof(*o), GFP_KERNEL))) {
		xetrace("Failed to allocate main object memory\n");
		goto out;
	}
	o->client.name = "rpcrdma_backend",
	o->client.add = add_one;
	o->client.remove = remove_one;
	INIT_LIST_HEAD(&o->devs);
	INIT_LIST_HEAD(&o->paths);
	INIT_LIST_HEAD(&o->rpaths);
	spin_lock_init(&o->guard);
	if (!(o->dir = proc_mkdir(MAIN_PROC_DIR_STR, NULL))) {
		xetrace("Fail to create /proc/%s dir\n", MAIN_PROC_DIR_STR);
		goto freeo;
	}
	if (!(o->start_proc_file = proc_create_entry(
		(char *)MAIN_PROC_START_ENTRY_STR, o->dir, proc_start, o))) {
		xetrace("Fail to create /proc/%s/%s entry\n",
			MAIN_PROC_DIR_STR, MAIN_PROC_START_ENTRY_STR);
		goto freeo;
	}
	if (!(o->server_proc_file = proc_create_entry(
		(char *)MAIN_PROC_SERVER_ENTRY_STR, o->dir, proc_server, o))) {
		xetrace("Fail to create /proc/%s/%s entry\n",
			MAIN_PROC_DIR_STR, MAIN_PROC_SERVER_ENTRY_STR);
		goto freeo;
	}
	if (!(o->client_proc_file = proc_create_entry(
		(char *)MAIN_PROC_CLIENT_ENTRY_STR, o->dir, proc_client, o))) {
		xetrace("Fail to create /proc/%s/%s entry\n",
			MAIN_PROC_DIR_STR, MAIN_PROC_CLIENT_ENTRY_STR);
		goto freeo;
	}
	if (intr_pollers_start()) {
		xetrace("Fail to create manager poller\n");
		goto freeo;
	}
	else
		o->poller_started = true;
	if (create_per_cpu(o)) {
		xetrace("Fail to create manager per_cpu\n");
		goto freeo;
	}
	init_completion(&o->start_stop_comp);
	reinit_completion(&o->start_stop_comp);
	info.name = "mainobj";
	info.owner = o;
	info.on_start = on_start;
	info.on_exit = on_exit;

	if (!(o->wth = wth_create(&info))) {
		xetrace("Failed to create manager worker_thread\n");
		goto freeo;
	}
	wait_for_completion(&o->start_stop_comp);

	if (o->started)
		goto out;
	else
		goto freeo;

freeo:
	manager_free(o);
	o = NULL;

out:
	FOUT;
	return o;
}

void manager_free(struct manager *o)
{
	FIN;
	if (o->ct) {
		client_tester_free(o->ct);
		o->ct = NULL;
	}
	if (o->st) {
		server_tester_free(o->st);
		o->st = NULL;
	}
	if (o->client_proc_file) {
		proc_remove_entry(o->client_proc_file);
		o->client_proc_file = NULL;
	}
	if (o->server_proc_file) {
		proc_remove_entry(o->server_proc_file);
		o->server_proc_file = NULL;
	}
	if (o->start_proc_file) {
		proc_remove_entry(o->start_proc_file);
		o->start_proc_file = NULL;
	}
	if (o->dir) {
		proc_remove(o->dir);
		o->dir = NULL;
	}
	stop(o);
	if (o->wth) {
		wth_free(o->wth);
		o->wth = NULL;
	}
	if (o->pcpu) {
		free_per_cpu(o);
		o->pcpu = NULL;
	}
	if (o->poller_started) {
		intr_pollers_stop();
		o->poller_started = false;
	}
	kfree(o);
	FOUT;
}

static int rdma_dev_exit(void *p, struct request_base *r)
{
	struct manager *o = p;
	struct rdma_dev_exit_request *rr =
		container_of(r, struct rdma_dev_exit_request, r);
	struct rdma_dev *dev = rr->dev;

	FIN;
	xdtrace("Removing device %s\n", dev->ib_dev->name);
	list_del_init(&dev->link);
	complete(dev->remove_done);
	rdma_dev_free(dev);
	if (list_empty(&o->devs))
		complete(&o->start_stop_comp);
	FOUT;
	return 0;
}

int manager_rdma_dev_exit(struct manager *o, struct rdma_dev *dev)
{
	struct rdma_dev_exit_request *r;
	int rv;

	FIN;
	if ((r = kzalloc(sizeof(*r), GFP_KERNEL))) {
		r->r.type = rpcrdma_exit_device;
		r->r.call = rdma_dev_exit;
		r->r.free = rdma_dev_exit_request_free;
		r->dev = dev;
		if (!wth_is_current(o->wth))
			wth_push_request(o->wth, &r->r);
		else
			BUG();
		rv = 0;
	}
	else
		rv = -1;
	FOUT;
	return rv;
}

struct per_cpu * get_pcpu(struct manager *o, int cpu)
{
	struct per_cpu __percpu *percpu = o->pcpu;
	struct per_cpu *pcpu;

	FIN;
	pcpu = per_cpu_ptr(percpu, cpu);
	FOUT;
	return pcpu;
}

int manager_pcpu_add_dev(
	struct manager *o, int cpu, struct per_core_rdma *d, bool preferred)
{
	struct per_cpu_link *p;
	struct per_cpu *q;
	struct list_head *l;
	unsigned long flags;
	int rv;

	FIN;
	xdtrace("Adding %s RDMA resources for CPU core %d\n",
		preferred ? "preferred" : "none_preferred", cpu);
	if ((p = kzalloc(sizeof(*p), GFP_KERNEL))) {
		p->d = d;
		p->cpu = cpu;
		q =  get_pcpu(o, cpu);
		l = preferred ? &q->p_rdma_devs : &q->u_rdma_devs;
		spin_lock_irqsave(&q->base.guard, flags);
		list_add_tail(&p->link, l);
		spin_unlock_irqrestore(&q->base.guard, flags);
		rv = 0;
	}
	else
		rv = -1;
	FOUT;
	return rv;
}

int manager_pcpu_rem_dev(
	struct manager *o, int cpu, struct per_core_rdma *d)
{
	struct per_cpu __percpu *percpu = o->pcpu;
	struct per_cpu_link *p;
	struct per_cpu *q;
	int i;
	unsigned long flags;
	int rv = -1;

	FIN;
	q =  get_pcpu(o, cpu);
	spin_lock_irqsave(&q->base.guard, flags);
	list_for_each_entry(p, &q->p_rdma_devs, link)
		if (p->d == d) {
			list_del_init(&p->link);
			kfree(p);
			rv = 0;
			break;
		}
	spin_unlock_irqrestore(&q->base.guard, flags);
	if (rv)
		xttrace("per_core_rdma object %p was not found on cpu %d\n", d, cpu);
	for_each_online_cpu(i) {
		if ((q = per_cpu_ptr(percpu, i))) {
			spin_lock_irqsave(&q->base.guard, flags);
			list_for_each_entry(p, &q->u_rdma_devs, link)
				if (p->d == d) {
					list_del_init(&p->link);
					kfree(p);
					break;
				}
			spin_unlock_irqrestore(&q->base.guard, flags);
		}
	}
	FOUT;
	return rv;
}

static void free_path_dev_cpu(struct per_cpu_path *q)
{
	struct per_cpu_link *r;

	FIN;
	while ((r = list_first_entry_or_null(
		&q->p_rdma_devs, struct per_cpu_link, link))) {
		list_del(&r->link);
		kfree(r);
	}
	while ((r = list_first_entry_or_null(
		&q->u_rdma_devs, struct per_cpu_link, link))) {
		list_del(&r->link);
		kfree(r);
	}
	FOUT;
}

static int build_path_dev_cpu(void *path, struct list_head *l,
	struct per_cpu_path *q, int cpu, bool is_preferred)
{
	struct per_cpu_link *p;
	struct per_cpu_link *r;
	int rv = 0;

	//FIN;
	list_for_each_entry(p, l, link) {
		if ((r = kzalloc(sizeof(*r), GFP_ATOMIC))) {
			r->cpu = cpu;
			r->d = p->d;
			if (rdma_dev_is_path_port(path, r->d)) {
				if (is_preferred)
					list_add_tail(&r->link, &q->p_rdma_devs);
				else
					list_add_tail(&r->link, &q->u_rdma_devs);
			}
		}
		else {
			xetrace("Failed to allocate per_path per_cpu memory\n");
			rv = -1;
			goto freel;
		}
	}
	rv = 0;
	goto out;

freel:
	free_path_dev_cpu(q);

out:
	//FOUT;
	return rv;
}

static void free_path_dev(struct per_cpu_path_link *p)
{
	struct per_cpu_path __percpu *percpup = p->percpu;
	struct per_cpu_path *q;
	int i;

	FIN;
	for_each_online_cpu(i) {
		q = per_cpu_ptr(percpup, i);
		if (q)
			free_path_dev_cpu(q);
	}
	FOUT;

}

void * manager_link_new_path(struct manager *o, void *path)
{
	struct per_cpu __percpu *percpu = o->pcpu;
	struct per_cpu_path __percpu *percpup;
	struct per_cpu_path_link *p;
	struct per_cpu *q;
	struct per_cpu_path *r;
	int i, rv = -1;
	unsigned long flags;

	FIN;
	percpup = alloc_percpu(struct per_cpu_path);
	if (percpup == NULL) {
		rv = -ENOMEM;
		xetrace("Failed to allocate per_cpu_path data\n");
		goto out;
	}
	if (!(p = kzalloc(sizeof(*p), GFP_KERNEL))) {
		xetrace("Failed to allocate per_cpu_path_link data\n");
		goto freepcpu;
	}
	p->path = path;
	p->percpu = percpup;
	for_each_online_cpu(i) {
		q = per_cpu_ptr(percpu, i);
		r = per_cpu_ptr(percpup, i);
		r->path = path;
		INIT_LIST_HEAD(&r->p_rdma_devs);
		INIT_LIST_HEAD(&r->u_rdma_devs);
		spin_lock_irqsave(&q->base.guard, flags);
		rv = build_path_dev_cpu(path, &q->p_rdma_devs, r, i, true) ||
			build_path_dev_cpu(path, &q->u_rdma_devs, r, i, false);
		spin_unlock_irqrestore(&q->base.guard, flags);
		if (rv)
			goto freepd;
	}
	spin_lock_irqsave(&o->guard, flags);
	list_add_tail(&p->link, &o->paths);
	spin_unlock_irqrestore(&o->guard, flags);
	rv = 0;
	goto out;

freepd:
	free_path_dev(p);
	kfree(p);

freepcpu:
	free_percpu(percpup);

out:
	FOUT;
	return rv ? NULL : p;
}

void manager_unlink_path(struct manager *o, void *path)
{
	struct per_cpu_path_link *p;
	unsigned long flags;

	FIN;
	spin_lock_irqsave(&o->guard, flags);
	list_for_each_entry(p, &o->paths, link)
		if (path == p->path) {
			list_del_init(&p->link);
			list_add_tail(&p->link, &o->rpaths);
			break;
		}
	spin_unlock_irqrestore(&o->guard, flags);
	FOUT;
}

void manager_free_path(struct manager *o, void *path)
{
	struct per_cpu_path_link *p;
	unsigned long flags;
	bool found = false;

	FIN;
	spin_lock_irqsave(&o->guard, flags);
	list_for_each_entry(p, &o->rpaths, link)
		if (path == p->path) {
			list_del_init(&p->link);
			found = true;
			break;
		}
	spin_unlock_irqrestore(&o->guard, flags);
	if (found) {
		free_path_dev(p);
		kfree(p);
	}
	FOUT;
}

static int register_address(void *p, struct request_base *r)
{
	struct manager *o = p;
	struct register_new_address_request *rr =
		container_of(r, struct register_new_address_request, r);
	struct rdma_dev *d;

	FIN;
	list_for_each_entry(d, &o->devs, link)
		rdma_dev_register_address(d, rr->f, rr->ctx);
	if (rr->c)
		complete(rr->c);
	FOUT;
	return 0;
}

int manager_register_new_address(struct manager *o,
	void (*f)(void *ctx, struct local_address_info *a), void *ctx)
{
	struct register_new_address_request *r;
	int rv;

	FIN;
	if ((r = kzalloc(sizeof(*r), GFP_KERNEL))) {
		r->r.type = rpcrdma_register_address;
		r->r.call = register_address;
		r->r.free = register_new_address_request_free;
		r->f = f;
		r->ctx = ctx;
		if (!wth_is_current(o->wth))
			wth_push_request(o->wth, &r->r);
		else
			BUG();
		rv = 0;
	}
	else
		rv = -1;
	FOUT;
	return rv;
}

int manager_unregister_new_address(struct manager *o, void *ctx)
{
	struct register_new_address_request *r;
	DECLARE_COMPLETION_ONSTACK(comp);
	int rv;

	FIN;
	if ((r = kzalloc(sizeof(*r), GFP_KERNEL))) {
		r->r.type = rpcrdma_register_address;
		r->r.call = register_address;
		r->r.free = register_new_address_request_free;
		r->ctx = ctx;
		r->c = &comp;
		if (!wth_is_current(o->wth)) {
			wth_push_request(o->wth, &r->r);
			wait_for_completion(&comp);
		}
		else {
			register_address(o, &r->r);
			kfree(r);
		}
		rv = 0;
	}
	else
		rv = -1;
	FOUT;
	return rv;
}

static int register_server(void *p, struct request_base *r)
{
	struct manager *o = p;
	struct register_server_request *rr =
		container_of(r, struct register_server_request, r);
	struct rdma_dev *d;

	FIN;
	list_for_each_entry(d, &o->devs, link)
		if (d == rr->local_rdma_dev) {
			rdma_dev_register_server(d, &rr->server, &rr->src);
			break;
		}
	FOUT;
	return 0;
}

int manager_register_server(struct manager *o, struct server_info *server,
	void *local_rdma_dev, struct sockaddr_storage *src)
{
	struct register_server_request *r;
	int rv;

	FIN;
	if ((r = kzalloc(sizeof(*r), GFP_KERNEL))) {
		r->r.type = rpcrdma_register_server;
		r->r.call = register_server;
		r->r.free = register_server_request_free;
		r->src = *src;
		r->server = *server;
		r->local_rdma_dev = local_rdma_dev;
		if (!wth_is_current(o->wth))
			wth_push_request(o->wth, &r->r);
		else
			BUG();
		rv = 0;
	}
	else
		rv = -1;
	FOUT;
	return rv;
}

int manager_unregister_server(struct manager *o, void *server,
	void *local_rdma_dev, struct sockaddr_storage *src)
{
	return rdma_dev_unregister_server(local_rdma_dev, server, src);
}

static int register_client(void *p, struct request_base *r)
{
	struct manager *o = p;
	struct register_client_request *rr =
		container_of(r, struct register_client_request, r);
	struct rdma_dev *d;

	FIN;
	list_for_each_entry(d, &o->devs, link)
		if (d == rr->local_rdma_dev) {
			rdma_dev_connect(d, &rr->client, &rr->src, &rr->dst);
			break;
		}
	FOUT;
	return 0;
}

int manager_connect(struct manager *o, struct client_info *client,
	void *local_rdma_dev, struct sockaddr_storage *src,
	struct sockaddr_storage *dst)
{
	struct register_client_request *r;
	int rv;

	FIN;
	if ((r = kzalloc(sizeof(*r), GFP_KERNEL))) {
		r->r.type = rpcrdma_register_client;
		r->r.call = register_client;
		r->r.free = register_client_request_free;
		r->src = *src;
		r->dst = *dst;
		r->client = *client;
		r->local_rdma_dev = local_rdma_dev;
		if (!wth_is_current(o->wth))
			wth_push_request(o->wth, &r->r);
		else
			BUG();
		rv = 0;
	}
	else
		rv = -1;
	FOUT;
	return rv;
}

int manager_disconnect(struct manager *o, void *local_rdma_dev,
	void *client, void *path)
{
	return rdma_dev_disconnect(local_rdma_dev, client, path);
}

static int find_path(struct dst_paths *paths, void **path,
	struct per_core_rdma **d, int **i, bool preferred)
{
	struct per_cpu_path_link *p;
	struct per_cpu_path *q;
	struct per_cpu_link *r = NULL;
	struct list_head *l;

	FIN;
	while (paths && !r) {
		p = paths->path;
		if (!p) {
			xetrace("Caller did not fill the path...\n");
			r = NULL;
			break;
		}
		q = this_cpu_ptr(p->percpu);
		l = preferred ? &q->p_rdma_devs : &q->u_rdma_devs;
		if (!list_empty(l)) {
			r = list_first_entry(l, struct per_cpu_link, link);
			list_del(&r->link);
			list_add_tail(&r->link, l);
			*d = r->d;
			*path = q->path;
			BUG_ON(*path == NULL);
			*i = &q->index_remote_dct;
			break;
		}
		paths = paths->next_paths;
	}
	FOUT;
	return r ? 0 : -1;
}

int manager_post_send(struct post_send_info *ps)
{
	void *path = NULL;
	struct per_core_rdma *d = NULL;
	int *index_remote_dct = NULL;
	int rv;

	FIN;
	if (find_path(ps->paths, &path, &d, &index_remote_dct, true) &&
		find_path(ps->paths, &path, &d, &index_remote_dct, false)) {
		xetrace("Cannot post_send - failed to find path.\n");
		rv = -1;
	}
	else
		rv = rdma_dev_post_send(ps, path, d, index_remote_dct);
	FOUT;
	return rv;
}

static int register_service(void *p, struct request_base *r)
{
	struct manager *o = p;
	struct register_service_request *rr =
		container_of(r, struct register_service_request, r);
	struct per_cpu *pcpu;
	struct rdma_service *rdmas;
	int n = num_online_cpus();
	struct {int cpu; int index;} *cores = NULL;
	u64 sid = x_get_guid();
	int i, j, index;
	bool err;
	int cpu;

	FIN;
	if (!sid)
		sid = x_get_guid();
	memset(rr->sid, 0, sizeof(*rr->sid));
	if (!(cores = kzalloc(sizeof(*cores) * n, GFP_KERNEL))) {
		xetrace("Failed to alloc ate memory for register_service\n");
		goto out;
	}
	i = -1;
	err = false;
	for_each_online_cpu(cpu) {
		pcpu = per_cpu_ptr(o->pcpu, cpu);
		index = LARRAY_GET(pcpu->services->services);
		if (index >= 0) {
			++i;
			cores[i].cpu = cpu;
			cores[i].index = index;
			rdmas = &pcpu->services->services.a[index];
			x_ref_init(&rdmas->in_use);
			rdmas->service_id = sid;
			rdmas->service = rr->service;
		}
		else {
			err = true;
			break;
		}
	}
	if (err)
		goto unreg;
	if (i > -1) {
		index = cores[0].index;
		for (j = 1; j <= i; ++j)
			if (cores[j].index != index) {
				err = true;
				break;
			}
		if (!err) {
			rr->sid->global.sid = cpu_to_be64(sid);
			rr->sid->global.index = cpu_to_be32(index);
			goto out;
		}
	}

unreg:
	for (j = 0; j <= i; ++j) {
		pcpu = per_cpu_ptr(o->pcpu, cores[j].cpu);
		LARRAY_PUT(pcpu->services->services, cores[j].index);
	}

out:
	kfree(cores);
	complete(rr->c);
	FOUT;
	return 0;
}

int manager_register_service(struct manager *o,
	struct server_service *service, union service_id *sid)
{
	struct register_service_request *r;
	DECLARE_COMPLETION_ONSTACK(comp);
	int rv;

	FIN;
	if ((r = kzalloc(sizeof(*r), GFP_KERNEL))) {
		r->r.type = rpcrdma_register_service;
		r->r.call = register_service;
		r->r.free = register_service_request_free;
		r->service = *service;
		r->sid = sid;
		r->c = &comp;
		if (!wth_is_current(o->wth)) {
			wth_push_request(o->wth, &r->r);
			wait_for_completion(&comp);
		}
		else {
			register_service(o, &r->r);
			kfree(r);
		}
		rv = sid->global.sid ? 0 : -1;
	}
	else {
		xetrace("Failed to allocate register_service_request request\n");
		rv = -1;
	}
	FOUT;
	return rv;
}

static int unregister_service(void *p, struct request_base *r)
{
	struct manager *o = p;
	struct unregister_service_request *rr =
		container_of(r, struct unregister_service_request, r);
	struct per_cpu *pcpu;
	int index = be32_to_cpu(rr->sid.global.index);
	unsigned long flags;
	int cpu;

	FIN;
	for_each_online_cpu(cpu) {
		pcpu = per_cpu_ptr(o->pcpu, cpu);
		spin_lock_irqsave(&pcpu->base.guard, flags);
		pcpu->services->services.a[index].service_id = 0;
		spin_unlock_irqrestore(&pcpu->base.guard, flags);
		x_ref_release_start(&pcpu->services->services.a[index].in_use);
		x_ref_release_wait(&pcpu->services->services.a[index].in_use);
		LARRAY_PUT(pcpu->services->services, index);
	}
	complete(rr->c);
	FOUT;
	return 0;
}

int manager_unregister_service(struct manager *o, union service_id *sid)
{
	struct unregister_service_request *r;
	DECLARE_COMPLETION_ONSTACK(comp);
	int rv;

	FIN;
	if (sid->global.sid) {
		if ((r = kzalloc(sizeof(*r), GFP_KERNEL))) {
			r->r.type = rpcrdma_unregister_service;
			r->r.call = unregister_service;
			r->r.free = unregister_service_request_free;
			r->sid = *sid;
			r->c = &comp;
			if (!wth_is_current(o->wth)) {
				wth_push_request(o->wth, &r->r);
				wait_for_completion(&comp);
			}
			else {
				unregister_service(o, &r->r);
				kfree(r);
			}
			rv = 0;
		}
		else {
			xetrace("Failed to allocate unregister_service_request\n");
			rv = -1;
		}
	}
	else
		rv = 0;
	FOUT;
	return rv;
}

struct memory_reg * manager_reg_mem(struct manager *o, void *local_rdma_dev,
	void *addr, unsigned len)
{
	return rdma_dev_reg_mem(local_rdma_dev, addr, len);
}

int manager_ureg_mem(struct memory_reg *mem)
{
	return rdma_dev_unreg_mem(mem);
}

void manager_sync_mem_for_cpu(
	struct memory_reg *mem, enum dma_data_direction dir)
{
	rdma_dev_sync_mem_for_cpu(mem, dir);
}

void manager_sync_mem_for_dev(
	struct memory_reg *mem, enum dma_data_direction dir)
{
	rdma_dev_sync_mem_for_dev(mem, dir);
}

