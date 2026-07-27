#include "dclib.h"
#include "xprt_rdma.h"
#include "../dclib/xkr_incs.h"
#include "../dclib/xkr_version.h"
#include "../dclib/xib_incs.h"
#include "../dclib/manager.h"
#include "../dclib/rdma_dev.h"
#include "../dclib/utils.h"
#include "../dclib/wth.h"
#include "../dclib/client_tester.h"
#include "../dclib/cli_srv_test_common.h"
#include "../dclib/xtrace.h"

struct dclib_info_imp {
	struct manager *o;
	struct sockaddr_storage dst;
	struct wth *wth;
	/* list of struct local_address */
	struct list_head local_addrs;
	struct list_head clients;
	struct completion start_stop_comp;
	TIMER_LIST_INSTANCE(timer);
	bool has_timer;
};
/*
struct _client {
	struct dclib_info_imp *ct;
	struct client_info ci;
	void *path;
	struct local_address *la;
	struct server_service ser;
	union service_id sid;
	struct cli_srv_payload him;
	struct post_send_info info;
	void *buf;
	struct scatterlist *sg;
	struct list_head link;
};
*/

int dclib_create(struct rpcrdma_xprt *xprt)
{
	return 0;
}

