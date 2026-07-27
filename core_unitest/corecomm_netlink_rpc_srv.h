#ifndef CORECOMM_NETLINK_RPC_SRV_H
#define CORECOMM_NETLINK_RPC_SRV_H

/* Please try to keep it the only external include in this file */
#include <linux/netlink.h>

#include "corecomm_netlink_rpc.h"

/**
 * Specifies netlink response to userspace context
 */

struct sk_buff;
struct nlmsghdr;
struct sock;

/* Utility data structure */
struct nl_response {
	struct sk_buff *skb_out;
	struct nlmsghdr *nlh;
	bool sent;
	void *data;
};

/**
 * Initialize nl_response - allocate resources
 * return 0 on success or error code on error
 * @param __self Response data structure to initialize
 * @param size Size of data to allocate
 * @param alloc_flags Memory allocation flags
 * @param msg_type Message type to prepare
 */
int nl_response_init(struct nl_response *__self, size_t size, gfp_t alloc_flags,
                     int msg_type);

/**
 * Frees resources allocated for nl_response.
 * Note that if response was already sent, nothing is freed as it is now out of
 * our responsibility and will be handled by netlink.
 * @note It is safe to call even when already sent
 * @param __self Object to free
 */
void nl_response_free(struct nl_response *__self);

/**
 * Send resonse previously initialized.
 * @param __self Object to send
 * @param sk Socket to send on
 * @param pid Port id
 * @return 0 or error code
 */
int nl_response_send(struct nl_response *__self, struct sock *sk, pid_t pid);

/********* SERVER SIDE MACROS ***********/

/**
 * Send a reply to message, do not use directly - use wrappers
 * @param msg_type NLMSG_ERROR for errors, NLRPC_REPLY for response
 * @param rsp Arbitrary response data, value
 * @param rspsize Size of rsp
 * @param ctx Connection context containing sk and pid
 * @param copy_hook Function / macro with args (void *dst, void *src, int size)
 * @param gfp_flags GFP flags used to alloc netlink message
 */
#define __nlrpc_reply(msg_type, rsp, rspsize, ctx, copy_hook, gfp_flags)       \
	({                                                                         \
		struct nl_response __self;                                             \
		int ___rv;                                                             \
		if ((___rv =                                                           \
		         nl_response_init(&__self, rspsize, gfp_flags, msg_type))) {   \
			printk(KERN_ALERT "Alloc error in response (%s:%d)\n", __FILE__,   \
			       __LINE__);                                                  \
		} else {                                                               \
			copy_hook((__self.data), (nlrpc_pointer_to_start(rsp)),            \
			          (rspsize));                                              \
			___rv = nl_response_send(&__self, ctx->sk, ctx->pid);              \
			nl_response_free(&__self);                                         \
			if (___rv)                                                         \
				printk(KERN_ALERT "Error in response %d (%s:%d)\n", ___rv,     \
				       __FILE__, __LINE__);                                    \
		}                                                                      \
		corecomm_put_connection(ctx);                                          \
		___rv;                                                                 \
	})

/** Alias to __nlrpc_reply for error message */
#define nlrpc_error(code, ctx, gfp_flags)                                      \
	({                                                                         \
		struct nlmsgerr ___reply = {.error = code};                            \
		printk(KERN_ALERT "Replying with error %d\n", code);                   \
		__nlrpc_reply(NLMSG_ERROR, ___reply, sizeof(___reply), ctx, memcpy,    \
		              gfp_flags);                                              \
	})
/** Alias to __nlrpc_reply for reply value message */
#define nlrpc_reply(value, ctx, gfp_flags)                                     \
	__nlrpc_reply(NLRPC_REPLY, value, sizeof(value), ctx, memcpy, gfp_flags)
/** Alias to __nlrpc_reply for reply with buffer to avoid data copy */
#define nlrpc_reply_buf(buf, size, ctx, gfp_flags)                             \
	__nlrpc_reply(NLRPC_REPLY, buf, size, ctx, memcpy, gfp_flags)

/** These functions are used to auto-print all input arguments, rely on
 * nlrpc_dbg_param and nlrpc_dbg_fmt macros. */
#define nlrpc_fmt_arglist(tuple) nlrpc_fmt_arglist_ tuple
#define nlrpc_fmt_arglist_(type, var)                                          \
	___count += scnprintf(___buf + ___count, ___len - ___count,                \
	                      nlrpc_stringify(msg->var));                          \
	___count += scnprintf(___buf + ___count, ___len - ___count, "=");          \
	___count += scnprintf(___buf + ___count, ___len - ___count,                \
	                      nlrpc_dbg_fmt((type, msg->var)),                     \
	                      nlrpc_dbg_param((type, var)));                       \
	___count += scnprintf(___buf + ___count, ___len - ___count, ", ");
#define nlrpc_fmt_arglist_dbg_print(op, ...)                                   \
	({                                                                         \
		char ___buf[260];                                                      \
		int ___count = 0, ___len = sizeof(___buf);                             \
		___count += scnprintf(___buf + ___count, ___len - ___count,            \
		                      nlrpc_stringify(op));                            \
		___count += scnprintf(___buf + ___count, ___len - ___count, ": ");     \
		nlrpc_foreach(nlrpc_fmt_arglist, ##__VA_ARGS__);                       \
		___count += snprintf(___buf + ___count, ___len - ___count, "\n");      \
		printk(KERN_INFO "%s", ___buf);                                        \
	})

/** Declariation of sync api implementation
 * Function implmentation body must follow
 * @param api Literal, api name
 * @param rsptype Type of response for given api
 * @param rspname Name of reponse variable, will be available in the function
 * @param ctx Connection context
 * body
 * @param ... Arguments list of type (type, name), ...
 * @return If function returns 0, success returned to client, with attached
 * @rspname variable. If function returns non 0, error is returned to the client
 * containing the return code.
 */
#define NLRPC_SRV_SYNC(api, rsptype, rspname, ctx, ...)                        \
	struct nlrpc_inp(api){                                                     \
	    nlrpc_foreach(nlrpc_unwrap_tuple_semi, ##__VA_ARGS__)};                \
	static inline int nlrpc_api_handler_name(api)(                             \
	    rsptype * rspname,                                                     \
	    struct corecomm_connection_ctx *                                       \
	        ctx nlrpc_foreach(nlrpc_unwrap_tuple_comma, ##__VA_ARGS__));       \
	void nlrpc_api_handler_name__(api)(void *_msg, int pid) {                  \
		struct nlrpc_inp(api) *msg = _msg;                                     \
		rsptype *rsp;                                                          \
		int rv;                                                                \
		struct corecomm_connection_ctx *ctx;                                   \
		nlrpc_fmt_arglist_dbg_print(api, ##__VA_ARGS__);                       \
		rsp = kzalloc(sizeof(rsptype), GFP_KERNEL);                            \
		if (!rsp) {                                                            \
			printk(KERN_ALERT "Error allocating rsp\n");                       \
			return;                                                            \
		}                                                                      \
		ctx = corecomm_get_connection(pid);                                    \
		if (!ctx) {                                                            \
			printk(KERN_ERR "Unknown PID %d\n", pid);                          \
			kfree(rsp);                                                        \
			return;                                                            \
		}                                                                      \
		rv = nlrpc_api_handler_name(api)(                                      \
		    rsp, ctx nlrpc_foreach(nlrpc_unwrap_arglist, ##__VA_ARGS__));      \
		if (rv) /*Failure*/                                                    \
			nlrpc_error(rv, ctx, GFP_KERNEL);                                  \
		else                                                                   \
			nlrpc_reply((*rsp), ctx, GFP_KERNEL);                              \
		kfree(rsp);                                                            \
	}                                                                          \
	static inline int nlrpc_api_handler_name(api)(                             \
	    rsptype * rspname,                                                     \
	    struct corecomm_connection_ctx *                                       \
	        ctx nlrpc_foreach(nlrpc_unwrap_tuple_comma, ##__VA_ARGS__))

/** Declariation of sync api implementation, passing message by ref instead of
 * arg list. Function implmentation body must follow
 * @param api Literal, api name
 * @param rsptype Type of response for given api
 * @param rspname Name of reponse variable, will be available in the function
 * @param ctx Connection context
 * @return If function returns 0, success returned to client, with attached
 * @rspname variable. If function returns non 0, error is returned to the client
 * containing the return code.
 */
#define NLRPC_SRV_SYNC_BYREF(api, rsptype, rspname, ctx, ...)                  \
	struct nlrpc_inp(api){                                                     \
	    nlrpc_foreach(nlrpc_unwrap_tuple_semi, ##__VA_ARGS__)};                \
	static inline int nlrpc_api_handler_name(api)(                             \
	    rsptype * rspname, struct corecomm_connection_ctx *,                   \
	    struct nlrpc_inp(api) * msg);                                          \
	void nlrpc_api_handler_name__(api)(void *_msg, int pid) {                  \
		struct nlrpc_inp(api) *msg = _msg;                                     \
		rsptype *rsp;                                                          \
		int rv;                                                                \
		struct corecomm_connection_ctx *ctx;                                   \
		nlrpc_fmt_arglist_dbg_print(api, ##__VA_ARGS__);                       \
		rsp = kzalloc(sizeof(rsptype), GFP_KERNEL);                            \
		if (!rsp) {                                                            \
			printk(KERN_ALERT "Error allocating rsp\n");                       \
			return;                                                            \
		}                                                                      \
		ctx = corecomm_get_connection(pid);                                    \
		if (!ctx) {                                                            \
			printk(KERN_ERR "Unknown PID %d\n", pid);                          \
			kfree(rsp);                                                        \
			return;                                                            \
		}                                                                      \
		rv = nlrpc_api_handler_name(api)(rsp, ctx, msg);                       \
		if (rv) /*Failure*/                                                    \
			nlrpc_error(rv, ctx, GFP_KERNEL);                                  \
		else                                                                   \
			nlrpc_reply((*rsp), ctx, GFP_KERNEL);                              \
		kfree(rsp);                                                            \
	}                                                                          \
	static inline int nlrpc_api_handler_name(api)(                             \
	    rsptype * rspname, struct corecomm_connection_ctx * ctx,               \
	    struct nlrpc_inp(api) * msg)

/** Declariation of async api implementation
 * Function implmentation body must follow
 * @param api Literal, api name
 * @param ctx Connection context
 * @param ... Arguments list of type (type, name), ...
 * @return If function returns non 0, error is returned to the client
 * synchronously. if function returns 0, it is expected to reply the client with
 * nlrpc_reply or nlrpc_error macros, on given @ctx, possibly at some later
 * time.
 */
#define NLRPC_SRV_ASYNC(api, ctx, ...)                                         \
	struct nlrpc_inp(api){                                                     \
	    nlrpc_foreach(nlrpc_unwrap_tuple_semi, ##__VA_ARGS__)};                \
	static inline int nlrpc_api_handler_name(api)(                             \
	    struct corecomm_connection_ctx *                                       \
	    ctx nlrpc_foreach(nlrpc_unwrap_tuple_comma, ##__VA_ARGS__));           \
	void nlrpc_api_handler_name__(api)(void *_msg, int pid) {                  \
		struct nlrpc_inp(api) *msg = _msg;                                     \
		int rv;                                                                \
		struct corecomm_connection_ctx *ctx;                                   \
		nlrpc_fmt_arglist_dbg_print(api, ##__VA_ARGS__);                       \
		ctx = corecomm_get_connection(pid);                                    \
		rv  = nlrpc_api_handler_name(api)(                                     \
            ctx nlrpc_foreach(nlrpc_unwrap_arglist, ##__VA_ARGS__));          \
		if (rv) /*Failure, no async reply*/                                    \
			nlrpc_error(rv, ctx, GFP_KERNEL);                                  \
	} /*Else async reply*/                                                     \
	static inline int nlrpc_api_handler_name(api)(                             \
	    struct corecomm_connection_ctx *                                       \
	    ctx nlrpc_foreach(nlrpc_unwrap_tuple_comma, ##__VA_ARGS__))

/* Some utility renderer for case clause */
#define nlrpc_render_case_msg_handler(api)                                     \
	case nlrpc_enum_name(api) - nlrpc_enum_name(first) - 1:                    \
		nlrpc_api_handler_name__(api)(nlmsg_data(nlh), pid);                   \
		break;

/**
 * Declares nelink msg handler function that can be later passed as an argument
 * to netlink cfg. The function can receive connections and dispatch an
 * appropriate netlink api.
 * @param func_name Literal, function name
 */
#define NLRPC_SRV_DECLARE_MSG_HANDLER(...)                                     \
	NLRPC_SRV_DECLARE_MSG_HANDLER_(__VA_ARGS__)
#define NLRPC_SRV_DECLARE_MSG_HANDLER_(func_name, ...)                         \
	static void func_name(struct sk_buff *skb) {                               \
		struct nlmsghdr *nlh;                                                  \
		int pid;                                                               \
		nlh = (struct nlmsghdr *)skb->data; /* Get the data */                 \
		pid = nlh->nlmsg_pid;               /* Save pid of sending process */  \
		if (nlh->nlmsg_type > nlrpc_enum_name(first) &&                        \
		    nlh->nlmsg_type < nlrpc_enum_name(last)) {                         \
			switch (nlh->nlmsg_type - nlrpc_enum_name(first) - 1) {            \
				nlrpc_foreach(nlrpc_render_case_msg_handler, ##__VA_ARGS__);   \
			}                                                                  \
		} else {                                                               \
			WARN(true, "Unknown msg type received: %d\n", nlh->nlmsg_type);    \
		}                                                                      \
	}

#endif /*CORECOMM_NETLINK_RPC_SRV_H*/