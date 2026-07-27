#include "xkr_incs.h"
#include "xkr_version.h"
#include "xib_incs.h"
#include "xtrace.h"

#include "main.h"
#include "manager.h"
#include "utils.h"

MODULE_AUTHOR("Excelero");
MODULE_DESCRIPTION("rpcrdma_backend");
MODULE_LICENSE("Dual BSD/GPL");


static struct manager *obj;

struct manager * rpcrdma_backend_create(void)
{
	struct manager_init_params params;

	FIN;
	if (!obj) {
		obj = manager_create(&params);
	}
	return obj;
}
EXPORT_SYMBOL(rpcrdma_backend_create);

void rpcrdma_backend_free(struct manager *o)
{
	if (o == obj) {
		manager_free(o);
		obj = NULL;
	}
}
EXPORT_SYMBOL(rpcrdma_backend_free);

int __init _in_(void) /* Constructor */
{
	int rv = 0;

	FIN;
	xdtrace("Hello from rpcrdma_backend\n");
	obj = rpcrdma_backend_create();
	FOUT;
	return rv;
}

struct manager * rpcrdma_backend_get_obj(void)
{
	return obj;
}

void __exit _out_(void) /* Destructor */
{
	FIN;
	xdtrace("Bye from rpcrdma_backend %p\n", obj);
	if (obj)
		rpcrdma_backend_free(obj);
	FOUT;
}

module_init(_in_);
module_exit(_out_);

