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
	const char *name;
	const char *path;
	struct udev_list_entry* next;
};
const char* udev_list_entry_get_name(const struct udev_list_entry *e) { return e->path; }
struct udev_list_entry *udev_list_entry_get_next(struct udev_list_entry *e) { return e->next; }

struct udev {
	int ref;
	int reserved;
	struct udev_list_entry ent[0];
};

struct udev *udev_new(void) {		// Todo: This is udev simulator, unrelated to nvme, should be in os simulator
	int i, n_entries = g_t_udev_sim->n_disks;
	struct udev *u = (struct udev *)calloc(1, sizeof(*u) + (sizeof(u->ent[0]) * n_entries));
	u->ref++;
	N_Tf(dfi1053, "udev_new, @INT bdevs", n_entries);
	for (i = 0; i < n_entries; ++i) {
		u->ent[i].name = g_t_udev_sim->local_disks[i].device_path;
		u->ent[i].path = g_t_udev_sim->local_disks[i].device_name;
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

struct udev_device { const struct udev_list_entry *e; };

struct udev_device *udev_device_new_from_syspath(struct udev *u, const char *path) {
	for (const struct udev_list_entry *e = u->ent; e != NULL; e = e->next) {
		if (!strncmp(e->path, path, 128)) {
			struct udev_device *d = malloc(sizeof(*d));
			d->e = e;
			return d;
		}
	}
	BUG_ON(true); N_Ef(dsf3494, "no device found for path=@STR", path);
	return NULL;
}

const char* udev_device_get_devpath(const struct udev_device* d) { return d->e->path; }
const char* udev_device_get_devnode(const struct udev_device* d) { return d->e->name; }
void udev_device_unref(             struct udev_device* d) { free(d); }

/******************************** public API *********************************/
#include "nvmeibt_local_disk_util.h"
int  nvmeibt_udev_create(void) {  return TSB_all_fds_tbl_create_fd("_udev_monitor", 0); }
void nvmeibt_udev_destroy(void) { override_close(g_t_udev_sim->o.sock->fd); }
int  nvmeibt_udev_get_fd(void) {          return g_t_udev_sim->o.sock->fd; } // For epoll waiting

enum nvmeibt_disk_type nvmeibt_udev_get_event(struct nvmeibt_udev_event *e) {	// Add sda
	struct nvmeibt_udev_simu *u = g_t_udev_sim;
	struct nvmeibt_udev_event *next_e = &u->events[u->n_sent % (int)ARRAY_SIZE(u->events)];
	enum nvmeibt_disk_type disk_type;
	BUG_ON(u->n_sent >= u->n_total);		// No new event to send. Why did epoll trigger this flow???
	u->n_sent++;;
	*e = *next_e;
	memset(next_e, 0, sizeof(*next_e));		// Clean this event
	disk_type = nvmeibt_local_disk_get_stock_disk_type_by_dev_file_name(e->dev_file_name, NULL);
	switch (disk_type) {
		case NVMEIBT_NVME_DISK_TYPE:
		case NVMEIBT_EXTERNAL_DISK_TYPE:
		case NVMEIBT_VIRTUAL_DISK_TYPE:
			break;							// Todo: add info here
		case NVMEIBT_NVMESH_DISK_TYPE:
		default:
			e->action = nvmeibt_udev_none;	// Ignoring already existing NVMesh drives
	}
	N_Tf(__AUTOID__, "udev_event on @STR, action=@INT, disk_type=@INT", e->dev_file_name, e->action, (int)disk_type);
	return disk_type;
}

void nvmeibt_udev_put_event(struct nvmeibt_udev_event *e) {
	memset(e, 0, sizeof(*e));
}

void nvmeibt_udev_simu_send_disk_event_to_toma(unsigned disk_idx, enum nvmeibt_udev_event_action action) {
	struct nvmeibt_udev_simu *u = g_t_udev_sim;
	struct nvmeibt_udev_event *e = &u->events[u->n_total % (int)ARRAY_SIZE(u->events)];
	const struct sandbox_nvme_device *disk = &u->local_disks[disk_idx];
	BUG_ON(disk_idx >= u->n_disks);
	u->n_total++;
	e->action = action;
	e->dev_file_name = disk->device_path;
	e->ctx = (void*)disk;
}

void nvmeibt_udev_simu_send_sata_event_to_toma(enum nvmeibt_udev_event_action action) {
	struct nvmeibt_udev_simu *u = g_t_udev_sim;
	struct nvmeibt_udev_event *e = &u->events[u->n_total % (int)ARRAY_SIZE(u->events)];
	u->n_total++;
	e->action = action;
	e->dev_file_name = TOMA_ROOT_DIR "dev/sda";		// Sata drive
	e->ctx = 0;
}

bool nvmeibt_udev_simu_did_toma_consume_all_events(void) {
	struct nvmeibt_udev_simu *u = g_t_udev_sim;
	return (u->n_total == u->n_sent);
}
