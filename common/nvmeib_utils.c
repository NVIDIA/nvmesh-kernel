#include "nvmeib.h"
#include "nvmeib_utils.h"
#include "nvmeib_rdma.h"
#include "nvmeibm_trace.h"
#if KS_HAS_CALL_USER_HEADER
#	include <linux/umh.h>
#else
#	include <linux/kmod.h>
#endif
#include <linux/inet.h>
#include <rdma/ib.h>

#define NVMEIB_NUM_OF_GID_SECTIONS 8
#define NVMEIB_SIZE_OF_GID_SEC 04
#define NVMEIB_GID_SECTION_FORMAT "%04x"

/**
 * convert formated gid to raw
 *
 * @param buf formated gid
 * @param raw resulted gid
 */
void scan_gid_str(char *buf, u8 raw[16])
{
	int i;
	char gid_buf[5] = {0};
	u16 *raw2 = (u16*)raw;

	for (i = 0; i < NVMEIB_NUM_OF_GID_SECTIONS; ++i)
	{
			strncpy(gid_buf, &buf[i*5], 4);
			raw2[i] = cpu_to_be16(simple_strtoul(gid_buf, NULL, 16));
	}
}
EXPORT_SYMBOL(scan_gid_str);

void format_gid_raw(u8 raw[16], char *buf)
{
	int i, n;

	for (n = 0, i = 0; i < NVMEIB_NUM_OF_GID_SECTIONS; ++i) {
		n += sprintf(buf + n, NVMEIB_GID_SECTION_FORMAT,
			be16_to_cpu(((__be16 *)raw)[i]));
		if (i < (NVMEIB_NUM_OF_GID_SECTIONS -1))
			buf[n++] = ':';
	}
}
EXPORT_SYMBOL(format_gid_raw);

void format_gid(union ib_gid *gid, char *buf)
{
	format_gid_raw(gid->raw, buf);
}
EXPORT_SYMBOL(format_gid);

void guid_show(union ib_gid *gid)
{
	char gid_buf[GUID_SIZE] = {0};

	format_gid(gid, gid_buf);
	_ND(trace_nvmeib_utils_guid_show, "GID: @GID_BUF", gid_buf);
}
EXPORT_SYMBOL(guid_show);

void path_show(struct ib_sa_path_rec *pathrec)
{
	char gid_buf[GUID_SIZE] = {0};

	format_gid(&pathrec->dgid, gid_buf);
	_NI(trace_nvmeib_utils_path_show, "GID: @GID_BUF\n  complete: @YES_NO_STATUS", gid_buf, NVMEIB_PATH_REC_DLID(*pathrec) ? "yes" : "no");
}
EXPORT_SYMBOL(path_show);

const char *nvmeib_status_str(enum ib_wc_status *status)
{
	switch (*status) {
	case IB_WC_SUCCESS: return "SUCCESS";
	case IB_WC_LOC_LEN_ERR: return "LOC_LEN_ERR";
	case IB_WC_LOC_QP_OP_ERR: return "LOC_QP_OP_ERR";
	case IB_WC_LOC_EEC_OP_ERR: return "LOC_EEC_OP_ERR";
	case IB_WC_LOC_PROT_ERR: return "LOC_PROT_ERR";
	case IB_WC_WR_FLUSH_ERR: return "WR_FLUSH_ERR";
	case IB_WC_MW_BIND_ERR: return "MW_BIND_ERR";
	case IB_WC_BAD_RESP_ERR: return "BAD_RESP_ERR";
	case IB_WC_LOC_ACCESS_ERR: return "LOC_ACCESS_ERR";
	case IB_WC_REM_INV_REQ_ERR: return "REM_INV_REQ_ERR";
	case IB_WC_REM_ACCESS_ERR: return "REM_ACCESS_ERR";
	case IB_WC_REM_OP_ERR: return "REM_OP_ERR";
	case IB_WC_RETRY_EXC_ERR: return "RETRY_EXC_ERR";
	case IB_WC_RNR_RETRY_EXC_ERR: return "RNR_RETRY_EXC_ERR";
	case IB_WC_LOC_RDD_VIOL_ERR: return "LOC_RDD_VIOL_ERR";
	case IB_WC_REM_INV_RD_REQ_ERR: return "REM_INV_RD_REQ_ERR";
	case IB_WC_REM_ABORT_ERR: return "REM_ABORT_ERR";
	case IB_WC_INV_EECN_ERR: return "INV_EECN_ERR";
	case IB_WC_INV_EEC_STATE_ERR: return "INV_EEC_STATE_ERR";
	case IB_WC_FATAL_ERR: return "FATAL_ERR";
	case IB_WC_RESP_TIMEOUT_ERR: return "RESP_TIMEOUT_ERR";
	case IB_WC_GENERAL_ERR: return "ERR";
	default: return "???";
	}
}
EXPORT_SYMBOL(nvmeib_status_str);

#define PORT_ID_MAX_STR_LENGTH 255
#define PORT_ID_MAX_INT 254

static void parse_port_id(const char *param, struct nvmeib_used_dev_ports *udp, const char *start_token, int i)
{
	int num_of_chars;
	long port;
	char tmp[PORT_ID_MAX_STR_LENGTH];

	NFIN;
	BUG_ON(!udp);
	num_of_chars = &param[i] - start_token;
	if (num_of_chars >= PORT_ID_MAX_STR_LENGTH -1) {
		_NE(error_nvmeib_utils_parse_port_id, "port number of chars is very long: @NUM_OF_CHARS",
			num_of_chars);
		num_of_chars = PORT_ID_MAX_STR_LENGTH - 1;
	}
	memcpy(tmp, start_token, num_of_chars);
	tmp[num_of_chars] = '\0';
	if (kstrtol(tmp, 0, &port))
		_NE(error_1_nvmeib_utils_parse_port_id, "Unable to parse port id @TMP", tmp);
	else if (port > 0 && port <= PORT_ID_MAX_INT) {
		--port; /*assuming port is 1 based*/
		udp->dev_ports.used_ports_mask[port >> 3] |= (1 << (port & 0x7));
	} else
		_NE(error_2_nvmeib_utils_parse_port_id, "Illegal port number @PORT_LONG", port);

	NFOUT;
}

static void parse_dev_name(const char *param, struct nvmeib_used_dev_ports *udp,
	const char **start_token, int i)
{
	int num_of_chars;

	NFIN;
	BUG_ON(udp == NULL);
	num_of_chars = &param[i] - *start_token;
	if (num_of_chars >= IB_DEVICE_NAME_MAX) {
		_NE(error_nvmeib_utils_parse_dev_name, "Device name too long!!!");
		num_of_chars = IB_DEVICE_NAME_MAX - 1;
	}
	memcpy(&udp->dev_ports.name, *start_token, num_of_chars);
	udp->dev_ports.name[num_of_chars] = '\0';
	_ND(trace_nvmeib_utils_parse_dev_name, "filtered dev name is @DEV_PORTS_NAME", udp->dev_ports.name);
	*start_token = &param[i + 1];

	NFOUT;
}

static int parse_ip(const char *param, struct nvmeib_used_dev_ports *udp,
			   const char **start_token, int i, int delim, bool ipv4)
{
	const char *local_start_token = *start_token;
	int rv;

	NFIN;

	BUG_ON(udp == NULL);
	BUG_ON(&param[i] < *start_token);
	BUILD_BUG_ON(ARRAY_SIZE(udp->dev_ports.sw_gid_value) == sizeof(struct in6_addr));

	if (ipv4) {
		struct in_addr in4;
		/* Parse the IPv4 address */
		if (!in4_pton(*start_token,
			(&param[i] - *start_token) + 1,
			(u8*)&in4, -1, start_token))
		{
			_NE(err_nvmeib_utils_parse_ip_ipv4, "failed to parse @STR as IPv4", local_start_token);
			rv = -EINVAL;
			goto out;
		}
		/* Convert to a v4-mapped IPv6 address */
		ipv6_addr_set_v4mapped(in4.s_addr, (struct in6_addr *)udp->dev_ports.sw_gid_value);

		_ND(trace_parse_ipv4, "Parsed IPv4 String @IPV4_STR to GID @GID", local_start_token, udp->dev_ports.sw_gid_value);
		rv = 0;
		goto out;
	}
	if (!in6_pton(*start_token,
		(&param[i] - *start_token) + 1,
		(u8*)udp->dev_ports.sw_gid_value, -1, start_token))
	{
		_NE(err_nvmeib_utils_parse_ip_ipv6, "failed to parse @STR as IPv6", local_start_token);
		rv = -EINVAL;
		goto out;
	}
	_ND(trace_parse_ipv6, "Parsed IPv6 String @ADDRESS_STR to GID @GID", local_start_token, udp->dev_ports.sw_gid_value);
	rv = 0;

out:
	NFOUT;
	return rv;
}

/* parse module parameter that holds the list of port guids to use */
int nvmeib_set_used_pots_guids(const char *param, unsigned max_len,
	struct list_head *used_dev_list)
{
	int rv = 0, starti, i;
	char tmp[GUID_SIZE];
	struct nvmeib_used_dev_ports *udp = NULL;

	NFIN;
	starti = 0;
	_ND(trace_nvmeib_utils_nvmeib_set_used_pots_guids, "going to iterate over @PARAM_STR", param);
	for (i = 0; i < max_len && param[i]; ++i) {
		if (!udp && !(udp = kzalloc(sizeof(*udp), GFP_KERNEL))) {
			_NE(error_nvmeib_utils_nvmeib_set_used_pots_guids, "memory allocation poblem in allocation used dev ports");
			rv = -ENOMEM;
			goto out;
		}

		if (param[i] == ',') {
			if (starti < (i - 1)) {
				memcpy(tmp, &param[starti], i - starti);
				tmp[i - starti] = '\0';
				_ND(trace_1_nvmeib_utils_nvmeib_set_used_pots_guids, "scanning gid @TMP", tmp);
				scan_gid_str(tmp, udp->gid);
				starti = i + 1;
				udp->fit_by_port_guid = true;
				list_add_tail(&udp->link, used_dev_list);
				udp = NULL;
			}
			else {
				_NE(error_1_nvmeib_utils_nvmeib_set_used_pots_guids, "port gid parameter syntax error: @PARAM_STR", param);
				kfree(udp);
				goto out;
			}
		}
	}
	if (udp) {
		if (starti < (i - 1)) {
			char thisgid[GUID_SIZE];
			memcpy(tmp, &param[starti], i - starti);
			tmp[i - starti] = '\0';
			_ND(trace_2_nvmeib_utils_nvmeib_set_used_pots_guids, "scanning gid @TMP", tmp);
			scan_gid_str(tmp, udp->gid);
			format_gid_raw(udp->gid, thisgid);
			_ND(trace_3_nvmeib_utils_nvmeib_set_used_pots_guids, "thisgid = @THISGID", thisgid);
			udp->fit_by_port_guid = true;
			list_add_tail(&udp->link, used_dev_list);
			udp = NULL;
		}
		else {
			_NE(error_2_nvmeib_utils_nvmeib_set_used_pots_guids, "port gid parameter syntax error: @PARAM_STR", param);
			kfree(udp);
			goto out;
		}
	}

out:
	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_set_used_pots_guids);

/* parse module parameter that holds the list of devices to use */
int nvmeib_set_used_dev_list(const char *param, unsigned max_len,
	struct list_head *used_dev_list)
{
	int rv = 0, i = 0;
	struct nvmeib_used_dev_ports *udp = NULL;
	const char *start_token;
	enum { PARSE_READ_DEV, PARSE_READ_IP, PARSE_READ_NETMASK, PARSE_READ_IP_NETMASK_DONE, PARSE_READ_PORT } parse_stat;
	enum { IP_UNKNOWN, IPV4, IPV6 } ip_stat = IP_UNKNOWN;
	unsigned netmask = 0;
	int syntax_line = -1;

#define GOTO_SYNTAX_ERR() { \
		syntax_line = __LINE__;\
		goto syntax_err; \
	}

	NFIN;
	start_token = param;
	_ND(trace_nvmeib_utils_nvmeib_set_used_dev_list, "going to iterate over @PARAM_STR", param);

	if (param[0] == ';') {
		_NT(nvmeib_set_used_dev_list_empty, "got ports=; this implicity means we only care about gids");
		goto out;
	}
	parse_stat = PARSE_READ_DEV;
	for (i = 0; i < max_len; ++i) {
		if (!udp && !(udp = kzalloc(sizeof(*udp), GFP_KERNEL))) {
			_NE(error_nvmeib_utils_nvmeib_set_used_dev_list, "memory allocation poblem in allocation used dev ports");
			rv = -ENOMEM;
			goto out;
		}
		udp->fit_by_port_guid = false;

		if (parse_stat == PARSE_READ_IP) {
			/* Valid characters for an IP v4/v6 address are 'a'-'f', 'A'-'F', '0' - '9', ':' and '.' */
			switch (param[i]) {
			case '0' ... '9':
				/* We can't determine if we are IPv4 or IPv6 just from these */
				continue;
			case '.':
				/* Only IPv4 has '.' - ip_stat can only be IPV4 or IP_UNKNOWN */
				if (ip_stat == IPV6)
					GOTO_SYNTAX_ERR();
				ip_stat = IPV4;
				continue;
			case 'a' ... 'f':
			case 'A' ... 'F':
			case ':':
				/* Only IPv6 has 'a'-'f', 'A' - 'F' or ':' - ip_stat can only be IPV6 or IP_UNKNOWN */
				if (ip_stat == IPV4)
					goto syntax_err;
				ip_stat = IPV6;
				continue;
			case '/':
			case ']':
				if (ip_stat == IP_UNKNOWN) {
					/* IP should have been determined by now either by a ':' or a '.' */
					GOTO_SYNTAX_ERR();
				}
				if (parse_ip(param, udp, &start_token, i, (int)param[i], ip_stat == IPV4) < 0)
					goto syntax_err;

				if (param[i] == ']') {
					/* no netmask provided, must match all bits */
					udp->dev_ports.sw_gid_mask[0] = ~(u64)0;
					udp->dev_ports.sw_gid_mask[1] = ~(u64)0;
					parse_stat = PARSE_READ_IP_NETMASK_DONE;
				} else {
					BUG_ON(param[i] != '/');
					parse_stat = PARSE_READ_NETMASK;
					netmask = 0;
				}
				break;
			default:
				GOTO_SYNTAX_ERR();
			}
		} else if (parse_stat == PARSE_READ_NETMASK) {
			switch (param[i]) {
			case '0' ... '9':
				netmask *= 10;
				netmask += (unsigned)(param[i] - '0');
				continue;
			case ']':
				if (ip_stat == IPV4) {
					/* The IPv4 address has been converted to an IPv4 mapped IPv6 address.
					 * so the first 96-bits are always constant - 0x00000000 0x00000000 0x0000FFFF */
					netmask += 96;
				}
				if (netmask > 128)
					GOTO_SYNTAX_ERR();

				/* Set the GID mask array using bitmap_set. (NOTE: The netmask counts from MSB to LSB) */
				bitmap_set((unsigned long *)udp->dev_ports.sw_gid_mask, 128 - netmask, netmask);

#ifdef __LITTLE_ENDIAN
				/* To convert from Little Endian, we have to bitswap the fields, but also swap the field order */
				do {
					u64 tmp = cpu_to_be64(udp->dev_ports.sw_gid_mask[0]);
					udp->dev_ports.sw_gid_mask[0] = cpu_to_be64(udp->dev_ports.sw_gid_mask[1]);
					udp->dev_ports.sw_gid_mask[1] = tmp;
				} while(0);
#endif

				_ND(trace_nvmeib_set_used_dev_list_parsed_netmask,
				    "Converted netmask @STR to int @UINT to gid mask @GID",
					start_token, netmask, udp->dev_ports.sw_gid_value);
				parse_stat = PARSE_READ_IP_NETMASK_DONE;
				continue;
			default:
				GOTO_SYNTAX_ERR();
			}
		}

		if (param[i] == ';' || param[i] == '\0' || i + 1 == max_len) {
			switch (parse_stat) {
			case PARSE_READ_IP:
			case PARSE_READ_NETMASK:
				/* These states are handled above, should never get here */
				BUG();
				break;
			case PARSE_READ_IP_NETMASK_DONE:
				break;
			case PARSE_READ_PORT:
				parse_port_id(param, udp, start_token, i);
				break;
			case PARSE_READ_DEV:
				parse_dev_name(param, udp, &start_token, i);
				memset(&udp->dev_ports.used_ports_mask, 0xff, 32);
				break;
			}
			_ND(trace_4_nvmeib_utils_nvmeib_set_used_dev_list, "DFILT: adding filter mask for device @DEV_PORTS_NAME "
				"first 64 ports mask: @RECV_MASK sw gid value: @GID siw gid mask: @GID", udp->dev_ports.name,
				*(u64*)(&udp->dev_ports.used_ports_mask), udp->dev_ports.sw_gid_value, udp->dev_ports.sw_gid_mask);

			list_add_tail(&udp->link, used_dev_list);
			udp = NULL;

			start_token = &param[i + 1];
			parse_stat = PARSE_READ_DEV;
			if (param[i] == '\0')
				break;
			continue;
		}
		else if (param[i] == ',') {
			if (parse_stat != PARSE_READ_PORT) {
				GOTO_SYNTAX_ERR();
			}
			parse_port_id(param, udp, start_token, i);
			start_token = &param[i + 1];
			parse_stat = PARSE_READ_DEV;
			continue;
		}
		else if (param[i] == ':') {
			if (parse_stat == PARSE_READ_DEV) {
				parse_dev_name(param, udp, &start_token, i);
				if (i + 1 < max_len && param[i + 1] == '[') {
					/* Syntax for IP is '<dev>:[<ip>]' or '<dev>:[<ip>/<mask>]' */
					parse_stat = PARSE_READ_IP;
					ip_stat = IP_UNKNOWN;
					start_token++;
					i++;
				} else {
					parse_stat = PARSE_READ_PORT;
				}
			}
			else
				GOTO_SYNTAX_ERR();
		}
		continue;
syntax_err:
		_NE(error_1_nvmeib_utils_nvmeib_set_used_dev_list,
		    "filter_ports parameter syntax error at char @IDX of @PARAM_STR on line @LINENO",
		    i, param, syntax_line);
		kfree(udp);
		rv = -EINVAL;
		goto out;
	}
	/* Sanity check */
	BUG_ON(parse_stat != PARSE_READ_DEV);
	BUG_ON(udp != NULL);
	_ND(trace_2_nvmeib_utils_nvmeib_set_used_dev_list, "listing the devices that will be used");
	list_for_each_entry(udp, used_dev_list, link) {
		_ND(trace_3_nvmeib_utils_nvmeib_set_used_dev_list, "DFILT: add filter mask for device @DEV_PORTS_NAME "
				"first 64 ports mask: @RECV_MASK sw gid value: @GID siw gid mask: @GID", udp->dev_ports.name,
				*(u64*)(&udp->dev_ports.used_ports_mask), udp->dev_ports.sw_gid_value, udp->dev_ports.sw_gid_mask);
	}

out:
	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_set_used_dev_list);

/* free the list of devices to use*/
void nvmeib_free_used_dev_list(struct list_head *used_dev_list)
{
	struct nvmeib_used_dev_ports *udp;

	NFIN;
	while ((udp =
		list_first_entry_or_null(used_dev_list, struct nvmeib_used_dev_ports, link))){
		list_del_init(&udp->link);
		kfree(udp);
	}

	NFOUT;
}
EXPORT_SYMBOL(nvmeib_free_used_dev_list);

/* returns true if a port in a device is not filtered out */
static bool use_port(struct nvmeib_used_dev_ports *used_dp, u8 port)
{
	bool rv;

	NFIN;
	if (!used_dp) {
		rv = true;
		goto out;
	}
	--port; /*port is 1 based*/
	rv = used_dp->dev_ports.used_ports_mask[port >> 3] & (1 << (port & 0x7));

out:
	NFOUT;
	return rv;
}


static int get_dev_gids_matching_filter(struct ib_device *device, int port, struct ib_port_attr *port_attr,
								const char *match_ndev_name,
								const union ib_gid *match_hw_gid,
								const union ib_gid *match_sw_gid_value,
								const union ib_gid *match_sw_gid_mask,
								struct list_head *out_list)

{
	struct nvmeib_rdma_ib_port_gid *gid_list_ent = NULL;
	int start_gid_index = 0;
	unsigned long stop_time = jiffies + 2*HZ;
	int n = 0;
	int rv;

	while (time_before(jiffies, stop_time)) {
		if (!(gid_list_ent = kzalloc(sizeof(*gid_list_ent), GFP_KERNEL))) {
			_NE(e0_get_dev_gids_matching_filter, "OOM");
			goto err;
		}

		rv = nvmeib_rdma_select_port_gid(
			device, port, port_attr, start_gid_index, gid_list_ent,
			/* if match GID is present, then we allow these special GIDs in case they are actually specified */
			!!match_hw_gid, /* allow_default_gid */
			!!match_hw_gid, /* allow_ipv6_link_local */
			match_ndev_name, match_hw_gid, match_sw_gid_value, match_sw_gid_mask);

		if (rv < 0) {
			/* done scanning gids-tbl */
			kfree(gid_list_ent);
			gid_list_ent = NULL;
			break;
		}

		_NT(t0_get_dev_gids_matching_filter,
			">> [@INT] adding gid[@INDEX]=@GUID_RAW (rv=@RV)",
			n, gid_list_ent->gid_index, &gid_list_ent->gid, rv);
		n++;
		list_add_tail(&gid_list_ent->link, out_list);
		gid_list_ent = NULL;
		start_gid_index = rv + 1;
	}
	WARN_ON(!time_before(jiffies, stop_time));
	goto out;

err:
	BUG_ON(gid_list_ent);
	while ((gid_list_ent = list_first_entry_or_null(out_list,
		struct nvmeib_rdma_ib_port_gid, link))) {
		list_del(&gid_list_ent->link);
		kfree(gid_list_ent);
	}
	n = 0;

out:
	return n;
}


/* returns true is a device is not filtered out */
bool nvmeib_use_dev(struct list_head *used_dev_list, struct ib_device *device,
	u8 port, struct list_head *out_list)
{
	int itr_idx = 0, n = 0;
	bool use_dev = list_empty(used_dev_list);
	bool add_all_gids = list_empty(used_dev_list);
	struct nvmeib_used_dev_ports *itr;
	struct ib_port_attr port_attr;
	int rv;
	NFIN;

	if ((rv = ib_query_port(device, port, &port_attr)) < 0) {
		_NE(err_nvmeib_use_dev_query_port_fail, "Failed (@RV) with ib_query_port", rv);
		goto out;
	}


	_NT(fin_nvmeib_use_dev, "--> @IB_NAME:@PORT",
		device->name, port);

	if (!out_list || !list_empty(out_list)) {
		_NT(trace_0_nvmeib_utils_nvmeib_use_dev, "No output list, abort");
		goto out;
	}

	list_for_each_entry(itr, used_dev_list, link) {
		if (!itr->fit_by_port_guid) {
			/* -------------------------------------------------------------- */
			/* ib-name based filtered                                   	  */
			/* -------------------------------------------------------------- */
			if (!strncmp(itr->dev_ports.name, device->name, IB_DEVICE_NAME_MAX) &&
				use_port(itr, port)) {
				_NT(trace_nvmeib_utils_nvmeib_use_dev,
					"Match: by '@NAME' and via used-ports-mask",
					itr->dev_ports.name);
				use_dev = true;
				add_all_gids = true;
				break;
			}

			/* -------------------------------------------------------------- */
			/* iface-name based filtered                                      */
			/* -------------------------------------------------------------- */
			_NT(trace_1_nvmeib_utils_nvmeib_use_dev,
				"[@INT] Scan GID table (of @IB_NAME:@PORT) to match itr by "
				"iface-NAME '@NAME'",
				itr_idx, device->name, port, itr->dev_ports.name);

			n += get_dev_gids_matching_filter(device, port, &port_attr,
							itr->dev_ports.name,
							NULL /* match_hw_gid */,
							(const union ib_gid *)itr->dev_ports.sw_gid_value,
							(const union ib_gid *)itr->dev_ports.sw_gid_mask,
							out_list);

			//break; once matched by iface-name, no point in looping...
		}
		else {
			/* -------------------------------------------------------------- */
			/* hw-gid based filtering                                         */
			/* -------------------------------------------------------------- */
			_NT(trace_2_nvmeib_utils_nvmeib_use_dev,
				"[@INT] Scan GID table (of @IB_NAME:@PORT) to match itr by "
				"GID '@GUID_RAW'",
				itr_idx, device->name, port, itr->gid);

			n += get_dev_gids_matching_filter(device, port, &port_attr,
							"", /* match_ndev_name */
							(union ib_gid *)itr->gid, /* match_hw_gid */
							NULL,	/* match_sw_gid_value */
							NULL, /* match_sw_gid_mask */
							out_list);
		}

		use_dev = n > 0;
		itr_idx++;
		/* even if matched, keep looping for other matching (gid) filters */
	}

	/* in case there are no filters or we've matched by ib-dev name,
	   get all the gids of dev */
	if (use_dev && add_all_gids) {
		_NT(trace_3_nvmeib_utils_nvmeib_use_dev,
			"[?] Scan GID table (of @IB_NAME:@PORT) to match NULL",
			device->name, port);
		n += get_dev_gids_matching_filter(device, port, &port_attr,
						"", /* match_ndev_name */
						NULL, /* match_hw_gid */
						NULL,	/* match_sw_gid_value */
						NULL, /* match_sw_gid_mask */
						out_list);
	}

out:

	_NT(fout_nvmeib_use_dev, "<-- @IB_NAME:@PORT (found @INT matching gids)",
		device->name, port, n);

	NFOUT;
	return use_dev;
}
EXPORT_SYMBOL(nvmeib_use_dev);

#define SET_ROCE_LOSSY_MODE_SCRIPT "/opt/nvmesh/client-repo/roce_lossy/set_roce_lossy_mode on"
static inline int __run_usermode_script_unsafe(const char *script_name, bool wait)
{	// When (wait == false), everything is static, supports only SET_ROCE_LOSSY_MODE_SCRIPT. Future solution, allocate on heap and copy script name. How to know when to free?
	static char *argv_persistent[] = { "/bin/sh", "-c", (char*)SET_ROCE_LOSSY_MODE_SCRIPT, NULL };
	       char *argv_on_stack[]   = { "/bin/sh", "-c", (char *)script_name,               NULL };
	static char *envp[] = {
		"HOME=/", "TERM=linux",
		"PATH=/sbin:/bin:/usr/sbin:/usr/bin",
		NULL
	};
	int rv;
	if (in_interrupt()) {
		_NE(t_12_utils, "Cannot run user mdoe script in interrupt context...\n");
		rv = -1;
		goto out;
	}
	if (!script_name || script_name[0] == '\0') {
		_NE(t_13_utils, "Invalid script to run in user mode...\n");
		rv = -1;
		goto out;
	}
	if (!wait) {	// Stack will evaporate, every param should be persistent
		rv = call_usermodehelper(argv_persistent[0], argv_persistent, envp, UMH_NO_WAIT);
	} else {
		rv = call_usermodehelper(argv_on_stack[0],     argv_on_stack, envp, UMH_WAIT_PROC);
	}
out:
	return rv;
}

int nvmeib_run_usermode_script(const char *script_name) {
	return __run_usermode_script_unsafe(script_name, true);
}
EXPORT_SYMBOL(nvmeib_run_usermode_script);

int nvmeib_set_roce_lossy_mode_on(void)
{
	return __run_usermode_script_unsafe(SET_ROCE_LOSSY_MODE_SCRIPT, false);
}
EXPORT_SYMBOL(nvmeib_set_roce_lossy_mode_on);

#include "compat/kr_incs_str.inc.c"
EXPORT_SYMBOL(nvmeib_remove_unsafe_symbols);

NVMEIB_DECLARE_KERNEL_WARNINGS_TRAP

