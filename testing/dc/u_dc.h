#ifndef U_DC_H_INCLUDED
#define U_DC_H_INCLUDED

#define WAIT_FOR_SOMETHING (5 * HZ)
#define N_COMP_ENTRIES 256
#define DC_KEY 0xdeadbeef
#define NVMEIB_SERVICE_ID 6988676976697999ULL
#define NVMEIB_SERVICE_ID_MASK (~RDMA_IB_IP_PS_MASK)
#define NVMEIB_PKEY 0xffff

struct u_iu {
	int index;
	u64 dma;
	void *buf;
	size_t size;
};

struct srq_info {
	struct ib_srq *srq;
	u32 srqn;
	struct u_iu **rx_ring;
	int srq_queue_size;
	int srq_msg_size;
};

struct u_dev {
	struct list_head port_list;
	struct ib_device *ib_dev;
	struct ib_device_attr dev_attr;
	struct ib_pd *pd;
	struct ib_mr *mr;
	struct srq_info srq;
	int num_comp_vectors;
	struct list_head link;
};

struct ib_port {
	/* owner device */
	struct u_dev *udev;
	/* port stuff */
	u8 port;
	union ib_gid gid;
	struct ib_gid_attr gid_attr;
	int gid_index;
	struct device dev;
	struct completion released;
	struct ib_port_attr attr;
	int global;
	struct list_head link;
};

struct add_one_work {
	struct work_struct work;
	struct dc_info *info;
	struct ib_device *device;
};

struct remove_one_work {
	struct work_struct work;
	struct dc_info *info;
	struct ib_device *device;
	void *priv;
	struct completion *c;
};

struct dc_info {
	struct proc_dir_entry *dir;
	void * proc_file;
	struct workqueue_struct *wq;
	struct work_struct start_work;
	struct remove_one_work remove_o_work;
	ssize_t (*start)(struct dc_info *p, char *page, size_t count);
	void (*stop)(struct dc_info *info);
	void (*post_add_one)(struct dc_info *info);
	void (*pre_remove_one)(struct dc_info *info);
	void (*on_cm_event)(struct dc_info *info);
	__be32 s_ipv4_address;
	__be16 s_ipv4_port;
	struct sockaddr_storage s_sin;
	bool started;
	struct rdma_cm_id *cm_id;
	struct rdma_cm_event returned_cm_event;
	struct rdma_cm_id *returned_cm_id;
	char payload[4096];
	struct sockaddr_storage *bind_sin;
	struct u_dev *selected_dev;
	struct ib_port *selected_port;
	struct ib_cq *cq;
	struct ib_qp *qp;
	struct ib_ah *ah;
	int last_error;
	struct list_head devs;
	char layer_type;
	struct completion add_one_started_comp;
	bool add_one_started;
	int add_one_calls;
};

struct rdma_dct_info {
	__be64 dct_key;
	__be32 dct_num;
    __be32 lid;
	__be64 subnet_prefix;
	__be64 interface_id;
    __be32 psn;
    __be32 srqn;
    __be32 gid_index;
    __be32 mtu;
	__be64 raddr;
	__be32 rkey;
};

int init_dc_info(struct dc_info *info, const char *name,
	ssize_t (*start)(struct dc_info *p, char *page, size_t count),
	void (*stop)(struct dc_info *));
void free_dc_info(struct dc_info *info, bool allocated);
void register_sa(void);
void unregister_sa(void);
int register_rdma(void);
void unregister_rdma(void);
struct u_dev * init_dev(struct ib_device *device);
void free_udev(struct u_dev *dev);
int get_device_phys_port_count(struct ib_device *device);
struct ib_port * add_port(
	struct u_dev *dev, u8 port, struct sockaddr_storage *a);
int post_recv(struct u_dev *dev, struct u_iu *iu, struct srq_info *srqi);
void format_gid_raw(const u8 raw[16], char *buf);
int rdma_cm_handler(struct rdma_cm_id *cm_id, struct rdma_cm_event *event);
int select_dev_port(struct dc_info *info);
int create_cq(struct dc_info *info);
int create_qp(struct dc_info *info);
int modify_qp(struct dc_info *info);
void init_conn_param(struct dc_info *info, struct rdma_conn_param *conn_param);

#endif

