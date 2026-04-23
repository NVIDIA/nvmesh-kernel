/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/
#include "nvmeibt_debug.h"	// Binary tracing
#include "nvmeibt_udev_simu_internal.h"
#include "../10_local_hw/nvme_disk_simu.h"

/************************** internal simulators API **************************/
static struct nvmeibt_udev_simu *g_t_udev_sim = NULL;

static bool _has_udev_event_to_send(void) {
	struct nvmeibt_udev_simu *u = g_t_udev_sim;
	return (u->n_sent < u->n_total);
}

struct nvmeibt_udev_simu *nvmeibt_udev_simu_create(const struct sandbox_nvme_device *local_disks, unsigned n_disks) {
	struct nvmeibt_udev_simu *u = g_t_udev_sim = calloc(1, sizeof(*u));
	u->o.recv = fd_otherside_write_only_illegal_recv;	// Toma uses nvmeibt_udev_get_event() instead as we emulate toma component
	u->o.send = fd_otherside_read_only_illegal_send;	// Never write to this fd
	u->o.has_data = _has_udev_event_to_send;
	u->local_disks = local_disks;
	u->n_disks = n_disks;
	return u;
}

struct TSB_fd_otherside *nvmeibt_udev_simu_connect(struct nvmeibt_udev_simu *u) { return &u->o; }

void nvmeibt_udev_simu_destroy(struct nvmeibt_udev_simu *u) {
	BUG_ON(u != g_t_udev_sim);
	free(u);
	g_t_udev_sim = NULL;
}

/******************************** lib-udev API *******************************/
struct udev_list_entry {
	const struct sandbox_nvme_device *disk;
	struct udev_list_entry* next;
};
const char* udev_list_entry_get_name(const struct udev_list_entry *e) { return e->disk->device_name; }
struct udev_list_entry *udev_list_entry_get_next(struct udev_list_entry *e) { return e->next; }

struct udev {
	int ref;
	int reserved;
	struct udev_list_entry ent[0];	// Must be last
};

struct udev *udev_new(void) {		// Todo: This is udev simulator, unrelated to nvme, should be in os simulator
	int i, n_entries = g_t_udev_sim->n_disks;
	struct udev *u = (struct udev *)calloc(1, sizeof(*u) + (sizeof(u->ent[0]) * n_entries));
	u->ref++;
	N_Tf(dfi1053, "udev_new, @INT bdevs", n_entries);
	for (i = 0; i < n_entries; ++i) {
		u->ent[i].disk = &g_t_udev_sim->local_disks[i];
		if (i > 0) u->ent[i - 1].next = &u->ent[i];		// Emulate linked list with our array
	}
	return u;
}

void udev_unref(struct udev* u) {
	u->ref--;
	if (u->ref == 0)
		free(u);
}

struct udev_enumerate { int dummy; };	// We dont use it, we just use 'struct udev'

struct udev_enumerate* udev_enumerate_new(struct udev* u) {
	u->ref++;
	return (struct udev_enumerate*)u;
}

void udev_enumerate_add_match_subsystem(struct udev_enumerate *e, const char* sub) {
	BUG_ON(strncmp(sub, "block", 4) != 0);		// Currently Support only block devices which are disks
	(void)e;
}
void udev_enumerate_add_match_property( struct udev_enumerate *e, const char* key, const char* val) {
	BUG_ON((strncmp(key, "DEVTYPE", 7) != 0) || (strncmp(val, "disk", 4) != 0));
	(void)e;
}

void udev_enumerate_scan_devices(struct udev_enumerate *e) {
	(void)e;
}

void udev_enumerate_unref(struct udev_enumerate* e) {
	udev_unref((struct udev* )e);
}

struct udev_list_entry* udev_enumerate_get_list_entry(struct udev_enumerate *e) {
	return ((struct udev*)e)->ent;
}

struct udev_device {					// Can be created from scanning entries or from event
	const struct udev_list_entry *e;
	struct nvmeibt_udev_event_simu ev;
};

struct udev_device *udev_device_new_from_syspath(struct udev *u, const char *path) {
	for (const struct udev_list_entry *e = u->ent; e != NULL; e = e->next) {
		if (!strncmp(e->disk->device_name, path, 128)) {
			struct udev_device *d = calloc(1, sizeof(*d));
			d->e = e;
			return d;
		}
	}
	BUG_ON(true); N_Ef(dsf3494, "no device found for path=@STR", path);
	return NULL;
}

const char* udev_device_get_devnode(const struct udev_device* d) { return d->e ? d->e->disk->device_path : d->ev.dev_file_name; }
void udev_device_unref(                   struct udev_device* d) { free(d); }
const char* udev_device_get_devpath(const struct udev_device* d) { return udev_device_get_devnode(d); }
const char* udev_device_get_syspath(const struct udev_device *d) { return udev_device_get_devnode(d); }
const char* udev_device_get_action(const struct udev_device *d) { return d->ev.action_is_add ? "add" : "del"; }
const char* udev_device_get_subsystem(const struct udev_device *d) { (void)d;  return "block"; }
const char* udev_device_get_devtype(  const struct udev_device *d) { (void)d;  return "disk"; }
const char* udev_device_get_property_value(const struct udev_device *d, const char *property) { (void)d;  (void)property;  return NULL; }

struct udev_monitor {
	struct nvmeibt_udev_simu *us;
	bool enable_recv;
};

struct udev_monitor *udev_monitor_new_from_netlink(struct udev *u, const char *name) {
	struct udev_monitor *um = calloc(1, sizeof(*um));
	BUG_ON(strncmp(name, "udev", 4));
	um->us = g_t_udev_sim;
	(void)TSB_all_fds_tbl_create_fd("_udev_monitor", 0);
	(void)u;
	return um;
}

int udev_monitor_get_fd(const struct udev_monitor *um) { return um->us->o.sock->fd; } // For epoll waiting
struct udev_monitor *udev_monitor_unref(struct udev_monitor *um) {
	override_close(udev_monitor_get_fd(um));
	free(um);
	return NULL;
}

int udev_monitor_filter_add_match_subsystem_devtype(struct udev_monitor *um, const char *subsystem, const char *devtype) {
	BUG_ON(um->us != g_t_udev_sim);
	BUG_ON((strncmp(subsystem, "block", 5) != 0) || (strncmp(devtype, "disk", 4) != 0));
	return 0;
}

int udev_monitor_enable_receiving(struct udev_monitor *um) { um->enable_recv = true; return 0;}

struct udev_device *udev_monitor_receive_device(struct udev_monitor *um) {	// Create from event
	struct nvmeibt_udev_simu *u = um->us;
	struct nvmeibt_udev_event_simu *next_e = &u->events[u->n_sent % (int)ARRAY_SIZE(u->events)];
	struct udev_device *rv = calloc(1, sizeof(*rv));
	BUG_ON(u->n_sent >= u->n_total);		// No new event to send. Why did epoll trigger this flow???
	u->n_sent++;
	rv->ev = *next_e;
	memset(next_e, 0, sizeof(*next_e));		// Clean this event
	return rv;
}

void nvmeibt_udev_simu_send_disk_event_to_toma(unsigned disk_idx, bool action_is_add) {
	struct nvmeibt_udev_simu *u = g_t_udev_sim;
	struct nvmeibt_udev_event_simu *e = &u->events[u->n_total % (int)ARRAY_SIZE(u->events)];
	e->dev = &u->local_disks[disk_idx];
	BUG_ON(disk_idx >= u->n_disks);
	e->action_is_add = action_is_add;
	e->dev_file_name = e->dev->device_path;
	u->n_total++;
}

void nvmeibt_udev_simu_send_sata_event_to_toma(bool action_is_add) {
	struct nvmeibt_udev_simu *u = g_t_udev_sim;
	struct nvmeibt_udev_event_simu *e = &u->events[u->n_total % (int)ARRAY_SIZE(u->events)];
	e->dev = NULL;
	e->action_is_add = action_is_add;
	e->dev_file_name = TOMA_ROOT_DIR "dev/sda";		// Sata drive
	u->n_total++;
}

bool nvmeibt_udev_simu_did_toma_consume_all_events(void) {
	struct nvmeibt_udev_simu *u = g_t_udev_sim;
	return (u->n_total == u->n_sent);
}
