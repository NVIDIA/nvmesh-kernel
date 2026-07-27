#include "linux/mm_types.h"
#include "nvmeib.h"
#include <linux/kallsyms.h>
#include <linux/random.h>
#include "nvmeib_shared.h"
#include "nvmeib_str.h"
#if KS_TRACE_EVENTS
#	include <linux/trace_events.h>
#	define ftrace_event_file trace_event_file
#	define ftrace_event_field trace_event_field
#	define ftrace_event_call trace_event_call
#	define ftrace_event_reg trace_event_reg

#if KS_DUMMY_TRACE_REG
/********** Dummy implementation of include <linux/ftrace_event.h>*************/
enum trace_reg {
	TRACE_REG_REGISTER, TRACE_REG_UNREGISTER
};
#endif // KS_DUMMY_TRACE_REG

#else
#include <linux/ftrace_event.h>
#endif

#include "nvmeib_public_keeper.h"
#include "nvmeib_public.h"

/******************************************************
 * nvmeib_keeper Rendezvous code
 * Needs to be GPL due to symbol_get restrictions
 ******************************************************/
static struct nvmeib_keeper_ops *nvmeib_keeper_ops;
static struct nvmeib_ref nvmeib_keeper_refcnt;

DEFINE_COMMON_REGISTER_KEEPER_FN(nvmeib_register_keeper) 
{
	int rv;
	NFIN;
	if (nvmeib_keeper_ops != NULL || !nvmeib_ref_is_dying(&nvmeib_keeper_refcnt)) {
		_NW_dmesg(warn_nvmeib_register_keeper, "Keeper module already registered");
		rv = -EALREADY;
		goto out;
	}
	
	nvmeib_keeper_ops = ops;
	nvmeib_ref_init(&nvmeib_keeper_refcnt);
	rv = 0;
	
	_NI(trace_nvmeib_register_keeper, "Keeper module registered. ops @PTR", ops);
	out:
	NFOUT;
	return rv;
}
EXPORT_SYMBOL_GPL(nvmeib_register_keeper);

DEFINE_COMMON_UNREGISTER_KEEPER_FN(nvmeib_unregister_keeper)
{
	NFIN;
	BUG_ON(ops != nvmeib_keeper_ops);

	_NI(trace_nvmeib_unregister_keeper, "Keeper module unregistering. ops @PTR", ops);

	nvmeib_ref_release_start(&nvmeib_keeper_refcnt);
	nvmeib_ref_release_wait(&nvmeib_keeper_refcnt);
	nvmeib_keeper_ops = NULL;
	
	_NI(trace_nvmeib_unregister_keeper_wait_done, "Keeper module unregistered.");

	NFOUT;
}
EXPORT_SYMBOL_GPL(nvmeib_unregister_keeper);

DEFINE_KEEPER_REQUEST_REGISTER_FN(nvmeib_keeper_request_register);

static void request_keeper_to_register(void)
{
	DEFINE_KEEPER_REQUEST_REGISTER_FN((*req_reg_fn));
	int rv;
	
	NFIN;
	
	/* Try and lookup the symbol */
	req_reg_fn = nvmeib_public_symbol_get(nvmeib_keeper_request_register);
	
	if (!req_reg_fn) {
		_NT(trace_request_keeper_to_register, "Keeper module not found");
		goto out;
	}
	/* Request the keeper module to register. Pass it the fn ptr to register */
	if ((rv = (*req_reg_fn)(nvmeib_register_keeper)) < 0) {
		_NT(error_request_keeper_to_register, "Failed (@RV) to get Keeper to register", rv);
		goto out;
	}
	
	out:
	if (req_reg_fn)
		nvmeib_public_symbol_put(nvmeib_keeper_request_register);
	NFOUT;
}

/* Called on public module_init */
void nvmeib_public_keeper_init(void)
{
	NFIN;
	nvmeib_keeper_ops = NULL;
	nvmeib_keeper_refcnt = NVMEIB_REF_INIT_DEAD();
	
	request_keeper_to_register();
	NFOUT;
}

/* Called on public module_exit */
void nvmeib_public_keeper_fini(void)
{

	NFIN;
	if (nvmeib_keeper_ops) {
		/* close_cb triggers the keeper to call nvmeib_unregister_keeper() as a callback */
		CALL_KEEPER_OP(nvmeib_keeper_ops, close_cb)(nvmeib_unregister_keeper);
	}
	NFOUT;
}

struct nvmeib_keeper_ops *nvmeib_public_get_keeper(void)
{
	struct nvmeib_keeper_ops *ret = NULL;
	NFIN;
	if (!nvmeib_ref_get(&nvmeib_keeper_refcnt)) {
		_NW(warn_get_keeper_busy, "Could not get nvmeib_keeper.");
		goto out;
	}
	ret = nvmeib_keeper_ops;
	out:
	NFOUT;
	return ret;
}
EXPORT_SYMBOL(nvmeib_public_get_keeper);

void nvmeib_public_put_keeper(void)
{
	NFIN;
	nvmeib_ref_put(&nvmeib_keeper_refcnt);
	NFOUT;
}
EXPORT_SYMBOL(nvmeib_public_put_keeper);

/* Called by client to load keeper (in response to: 'nvmesh_clnt_shutdown -u') */
int nvmeib_public_load_keeper(void)
{
	int rv;
	
	NFIN;
	if ((rv = request_module("nvmeib_keeper")) < 0) {
		_NT(nvmeib_load_keeper_req_fail, "Failed (@RV) to load keeper module", rv);
		goto out;
	}

	if (!nvmeib_ref_get(&nvmeib_keeper_refcnt)) {
		_NT(nvmeib_load_keeper_fail_connect, "Keeper was loaded, but did not connect");
		rv = -ENOENT;
		goto out;
	}

	nvmeib_ref_put(&nvmeib_keeper_refcnt);
out:
	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_public_load_keeper);

#ifdef CONFIG_MODPROBE_PATH
#define MODPROBE_PATH CONFIG_MODPROBE_PATH
#else
#define MODPROBE_PATH "/sbin/modprobe"
#endif

static int request_unload_module_wait(const char *name)
{
	struct subprocess_info *info;
	static char *envp[] = {
		"HOME=/",
		"PATH=/usr/sbin:/usr/bin:/sbin:/bin",
		NULL,
	};
	char *argv[] = { MODPROBE_PATH, "-r", (char *)name, NULL };
	int rv;
	
	NFIN;
	info = call_usermodehelper_setup(argv[0], argv, envp, GFP_KERNEL, NULL, NULL, NULL);
	if (!info) {
		_NE(err_request_unload_module_nowait_oom, "OOM");
		rv = -ENOMEM;
		goto out;
	}
	
	/* Wait for modprobe completion to get the exit status */
	if ((rv = call_usermodehelper_exec(info, UMH_WAIT_PROC)) < 0) {
		_NE(err_request_unload_module_nowait_exec, 
		    "call_usermodehelper_exec failed (@RV)", rv);
		goto out;
	}
	
	out:
	NFOUT;
	return rv;
}

/* Called by client to unload keeper (after IB init) */
int nvmeib_public_unload_keeper(void)
{
	int rv;
	NFIN;
	rv = request_unload_module_wait("nvmeib_keeper");
	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_public_unload_keeper);
