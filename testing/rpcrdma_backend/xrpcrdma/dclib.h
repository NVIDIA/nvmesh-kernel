#ifndef DCLIB_H_INCLUDED
#define DCLIB_H_INCLUDED

struct dclib_info_imp;
struct dclib_info {
	struct dclib_info_imp *imp;
};

struct rpcrdma_xprt;
int dclib_create(struct rpcrdma_xprt *xprt);

#endif

