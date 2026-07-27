#ifndef NVMEIBC_MAIN_CAPI_MANIPULATE_VOLS_H
#define NVMEIBC_MAIN_CAPI_MANIPULATE_VOLS_H

/* Methods for manipulating volumes according to received config command:
   attach/detach/detach all/update/upgrade.... */

#include "nvmeibc_mcs_stub.h"
#include "nvmeib.h"

struct nvmeibc_volume_attach_t {				// New Exclusive Access params from cli
	struct nvmeibc_reservation res;				// Reservation version with which volume was first attached as visible for IO. Once set to value other than RESERVATION_MODE_IRRELEVANT will stay constant
};

struct nvmeibc_volume_header {					// Header of a single volume.
	char devname[NVMEIBC_BD_NAME_LEN];			// Textual name of the volume. Like "vol1" "MyDocs"
	char uuid[NVMEIBC_BD_UUID_LEN];				// Just some unique UUID. Does not change when volume is updates
	int version;								// Each time management updates the volume configuration (like segment relocation) this number grows
	int attachment_version;						// Global per client attachment version, not per volume. NVMESH-4837: Who added it to here?
	unsigned long long attachment_version_per_volume;			// Local per volume attachment version, NVMESH-4837: Is it used?
	struct nvmeibc_volume_attach_t vat;			// Attachment info including read_only/exclusive/preempt.
	enum nvmeibc_config_volume_type type;		// Type cannot be changed during volumes life cycle. Once set is constant. Must dettach before reattaching
	bool first_io_enabled_was_sent_to_cli;		// Send to cli only on the first IO enabled (in case CLI is waiting for IO enabled) - stats false, and once sent will remain true
	enum_io_perm last_sent_io_perm;				// The last IO perm sent to mgmt required to arm the IO stable notification when updating a volume's configuration
	struct ext_blob_t {							// Store reference ids (external management blob of strings)
		struct nvmeibc_reference_id *referenceIDs;
		int n_ref_ids;
	} ext_blob;
	spinlock_t ext_blob_modify_guard; //used to prevent race between proc read & volume update operations.
	#define nvmeibc_volume_ext_blob_size(n_refs) (sizeof(struct nvmeibc_reference_id)*(n_refs))
};
/* Cosntructor: nvmeibc_volume_header_create_from_msg(); */
/* Cosntructor: */ static inline void nvmeibc_volume_header_init_0( struct nvmeibc_volume_header* hdr)
{
	spin_lock_init(&hdr->ext_blob_modify_guard);

	hdr->ext_blob.referenceIDs = NULL;
	hdr->ext_blob.n_ref_ids = 0;
}


enum nvmeibc_volume_header_destroy_type {
	NVMEIBC_VOLUME_HEADER_DESTROY_REF_IDS,
	NVMEIBC_VOLUME_HEADER_DESTROY_TOTAL
};

/* Destructor:  */ static inline void nvmeibc_volume_header_destroy(struct nvmeibc_volume_header* hdr, enum nvmeibc_volume_header_destroy_type what)
{
	kfree(hdr->ext_blob.referenceIDs);
	if(what == NVMEIBC_VOLUME_HEADER_DESTROY_TOTAL){
		nvmeibc_volume_header_init_0(hdr);
	} else {
		hdr->ext_blob.referenceIDs = NULL;
		hdr->ext_blob.n_ref_ids = 0;
	}
}

/************************** Attach flags **************************************/
void nvmeibc_volume_attach_t_init_from_conf( struct nvmeibc_volume_attach_t *vat, const struct nvmeibc_volume_conf *conf);
void nvmeibc_volume_attach_t_init(           struct nvmeibc_volume_attach_t *vat);
void nvmeibc_volume_attach_t_copy(           struct nvmeibc_volume_attach_t *vat, const struct nvmeibc_volume_attach_t *src);
bool nvmeibc_volume_attach_t_are_equal(const struct nvmeibc_volume_attach_t *v1 , const struct nvmeibc_volume_attach_t *v2 );
ssize_t nvmeibc_volume_attach_t_tostring(const struct nvmeibc_volume_attach_t *vat, char *buf, size_t len);
char *nvmeibc_volume_attach_t_mode_to_string(enum_reservation_mode mode);
char *nvmeibc_volume_attach_t_preempt_to_string(enum_preempt_status preempt);

/* Update mgmt & CLI with status of attach/detach/update volume completion */
#define NVMEIBC_IO_PERM_USE_CURR_PERMS 	((u32)0xFFFFFFFF)
#define ERROR_VOL_UPDATE_ALREADY_LATEST ((int)1417)   // Not an error, update is successfull, but volume configuration did not change
struct nvmeibc_cinst_params_main;

void update_processing_multi_vol_cmd(const struct nvmeibc_cinst_params_main *p,
				     bool val);

int nvmeibc_cc_api_reply_vol_cmd_status(const struct nvmeibc_cinst_params_main* p,
				struct nvmeibc_volume_header *hdr, enum_vol_status status,
				u32 io_perm, bool send_to_cli, bool send_to_mcs, int inc_report_id_if_needed);

int setup_block_device_generic(const struct nvmeibc_cinst_params_main *p,
		const struct nvmeib_mgmt_to_client_volume_configuration *m, const bool update_only);

struct nvmeib_mgmt_to_client_update_targets_nics;
int update_targets_nics_generic(const struct nvmeibc_cinst_params_main *p,
		struct nvmeib_mgmt_to_client_update_targets_nics *m);

int setup_multi_tier_block_device(const struct nvmeibc_cinst_params_main *p, const int n_vols, const char *uuids, const int prev_rv, const bool hidden_attach);
struct nvmeibc_vol_detach_cmd {	// Instructions how to detach. Todo: make bit fields enum instead of list of booleans
	bool hidden;		// True=Detach only volumes in hidden mode, False=All volumes
	bool recov;			// True=Detach only volumes in recoverer mode or hidden mode, False=All volumes
	bool force;			// True=Detach all volumes by force. False=Allow failure on busy (hidden volumes are busy when recoveries run, Normal volumes are busy when user space app has opens)
	bool abandon;		// True=Does 'force==true' and allow future attach to reconnect (used for software upgrade). Irrelevant for hidden volumes
	bool err_attach;	// This detach is actually a cleanup upon attach error so volume can be partially initialized
	bool shutdown;		// True=Detach all volumes as part of shutdown (force==true)
} __attribute__ ((packed));

static inline struct nvmeibc_vol_detach_cmd nvmeibc_vol_detach_cmd_shutdown(void) {
	struct nvmeibc_vol_detach_cmd rv = {.hidden = 0, .recov = 0, .force = 1, .abandon = 0, .err_attach = 0, .shutdown = 1};
	return rv;
}
static inline struct nvmeibc_vol_detach_cmd nvmeibc_vol_detach_cmd_upgrade( void) {
	struct nvmeibc_vol_detach_cmd rv = {.hidden = 0, .recov = 0, .force = 1, .abandon = 1, .err_attach = 0, .shutdown = 0};
	return rv;
}
static inline struct nvmeibc_vol_detach_cmd nvmeibc_vol_detach_cmd_nice(    void) {
	struct nvmeibc_vol_detach_cmd rv = {.hidden = 0, .recov = 0, .force = 0, .abandon = 0, .err_attach = 0, .shutdown = 0};
	return rv;
}
static inline struct nvmeibc_vol_detach_cmd nvmeibc_vol_detach_cmd_hidden(  void) {
	struct nvmeibc_vol_detach_cmd rv = {.hidden = 1, .recov = 0, .force = 0, .abandon = 0, .err_attach = 0, .shutdown = 0};
	return rv;
}
static inline struct nvmeibc_vol_detach_cmd nvmeibc_vol_detach_cmd_recov(  void) {
	struct nvmeibc_vol_detach_cmd rv = {.hidden = 0, .recov = 1, .force = 0, .abandon = 0, .err_attach = 0, .shutdown = 0};
	return rv;
}
static inline struct nvmeibc_vol_detach_cmd nvmeibc_vol_detach_cmd_error(   void) {
	struct nvmeibc_vol_detach_cmd rv = {.hidden = 0, .recov = 0, .force = 0, .abandon = 0, .err_attach = 1, .shutdown = 0};
	return rv;
}
static inline struct nvmeibc_vol_detach_cmd nvmeibc_vol_detach_cmd_default(void) {
	struct nvmeibc_vol_detach_cmd rv = {.hidden = 0, .recov = 0, .force = 1, .abandon = 0, .err_attach = 0, .shutdown = 0};
	return rv;
}

static inline bool nvmeibc_vol_detach_recoverer_or_hidden(const struct nvmeibc_vol_detach_cmd *how) {
	return (how->recov || how->hidden);
}

/* Try detach a given volume (either due to command given by mcs, volume delete
   or from CLI). When detach task finishes (success or failure) it will update
   on_finish. if rv < 0 - detach could not be executed now, you may want to
   retry it later */
int try_detach_volume_with_multicomplete(struct nvmeibc_volume *volume, struct nvmeibc_vol_detach_cmd how, struct nvmeibc_multi_completion* on_finish);
void shut_down_detach_all_remainig_volumes_of_inst(const struct nvmeibc_cinst_params_main *p, bool is_nvmeibc_upgrade);

#endif
