#ifndef U_IB_INCS_H
#define U_IB_INCS_H

/*#ifndef CONFIG_INFINIBAND_ON_DEMAND_PAGING
#	define CONFIG_INFINIBAND_ON_DEMAND_PAGING
#endif*/

#include <linux/compat-2.6.h>
#include <rdma/ib_verbs.h>
#include <rdma/ib_sa.h>
#include <rdma/ib_cm.h>
#include <rdma/ib_mad.h>
#include <rdma/ib_fmr_pool.h>
#include <rdma/rdma_cm.h>

#ifndef CONFIG_COMPAT_PM_QOS
#	define CONFIG_COMPAT_PM_QOS
#endif
#ifndef CONFIG_COMPAT_PM_QOS_V2
#	define CONFIG_COMPAT_PM_QOS_V2
#endif

struct net_device;
struct ethtool_cmd;
extern int __ethtool_get_settings(struct net_device *dev,
				  struct ethtool_cmd *cmd);

#endif
