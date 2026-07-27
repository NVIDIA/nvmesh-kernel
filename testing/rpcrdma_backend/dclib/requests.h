#ifndef REQUESTS_H_INCLUDEDH
#define REQUESTS_H_INCLUDEDH

struct request_base {
	unsigned type;
	void *owner;
	int (*call)(void *, struct request_base *);
	void (*free)(struct request_base *);
	struct list_head link;
};

#undef REQUESTS_SEP_S
#undef REQUESTS_SEP_E
#undef REQUESTS_SEP

#define REQUEST_FOR_SEP_S(x, y) x = y,
#define REQUEST_FOR_SEP_E(x) x,
#define REQUEST_FOR(x) x,

enum rpcrdma_request_id {
#include "requests.hxx"
};
#undef REQUEST_FOR_SEP_S
#undef REQUEST_FOR_SEP_E
#undef REQUEST_FOR
 
const char * rpcrdma_request_to_str(int i);

#endif
