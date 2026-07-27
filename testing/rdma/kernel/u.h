#ifndef U_H_INC
#define U_H_INC

#define trace_inf(x, y, fmt, ...) do { pr_info("(%d/%d)" x "[%s](%d): " fmt, current->pid, raw_smp_processor_id(), y, __FUNCTION__, __LINE__, ## __VA_ARGS__); } while (0)
#define trace_wrn(x, y, fmt, ...) do { pr_warn("(%d/%d)" x "[%s](%d): " fmt, current->pid, raw_smp_processor_id(), y, __FUNCTION__, __LINE__, ## __VA_ARGS__); } while (0)
#define trace_err(x, y, fmt, ...) do { pr_err("(%d/%d)" x "[%s](%d): " fmt, current->pid, raw_smp_processor_id(), y, __FUNCTION__, __LINE__, ## __VA_ARGS__); } while (0)
#define trace_dbg(x, y, fmt, ...) do { printk("(%d/%d)" x "[%s](%d): " fmt, current->pid, raw_smp_processor_id(), y, __FUNCTION__, __LINE__, ## __VA_ARGS__); } while (0)

#define FILENAME kbasename(__FILE__)
#define _I(fmt, ...) trace_inf("%s", FILENAME, fmt, ## __VA_ARGS__)
#define _W(fmt, ...) trace_wrn("%s", FILENAME, fmt, ## __VA_ARGS__)
#define _E(fmt, ...) trace_err("%s", FILENAME, fmt, ## __VA_ARGS__)
#define _D(fmt, ...) if (!DEBUG_TEST); else trace_dbg("%s", FILENAME, fmt, ## __VA_ARGS__)

#define FIN _D("-->\n")
#define FOUT _D("<--\n")
#define LINE _D("---\n")
#define IFIN _I("-->\n")
#define IFOUT _I("<--\n")

/* named code blocks */
#define BLOCK(name)	goto name; name##_skip: if (0) name:
#define BREAK(name) goto name##_skip

#define SERVICE_GUID 7919ULL

/* send as a private data to the server on each client login request */
struct c_login_request {
	/* the message size in the test - up to PAGE_SIZE which
	   is the message size of the SRQ
	*/
	int message_size;
	/* send q size */
	int n_messages;
	/* are we using SRQ */
	int srq;
	/* which test do we run */
	int test_type;
};

/* send as a private data to the client on each server login response.
   it is an area of size PAGE_SIZE for RDMA_(READ/WRITE) tests.
*/
struct s_login_response {
	u64 raddr;
	u32 rkey;
};

/* iu for information unit.  used as a context for send and receive */
struct u_iu {
	int index;
	u64 dma;
	void *buf;
	size_t size;
};

/* SRQ info */
struct srq_info {
	struct ib_srq *srq;
	struct u_iu **rx_ring;
	int srq_queue_size;
	int srq_msg_size;
};

/* wrapper on IB device */
struct u_dev {
	struct list_head port_list;
	struct ib_device *ib_dev;
	struct ib_device_attr *dev_attr;
	struct ib_pd *pd;
	struct ib_mr *mr;
	struct srq_info srq;
	struct list_head link;
};

/* info for RDMA_WRITE test */
struct rdma_write_test {
	int seconds;
	u64 messages;
	u64 raddr;
	u32 rkey;
};

/* info for SEND test with or without interrupts */
struct send_test {
	u64 messages;
	atomic64_t s_messages;
	u64 sc_messages;
	u64 rc_messages;
	u64 start_time;
	u64 end_time;
};

/* test types */
enum test_types {
	tt_rdma_write = 1,
	tt_send = 2,
	tt_send_poll = 3,
};

/* tests wrapper */
struct u_tests {
	enum test_types type;
	int message_size;
	int n_messages;
	int srq;
	struct rdma_write_test rwt;
	struct send_test st;
};

#endif

