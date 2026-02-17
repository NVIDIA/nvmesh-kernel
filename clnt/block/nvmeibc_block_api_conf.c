#include "nvmeibc_block_common.h"
#include "nvmeibc_volume.h"
#include "block/controlpath/nvmeibc_b_cp_topo_common.h"
#include "nvmeibc_block_api_conf.h"

/******************************* Unsafe Detach ********************************
  for unsafe detach, we assume the following:
  1) the overall procedure is to block new IO's from arriving & then drain each
    layer in the order of IO progress. we block new IO's by replacing the
    request_queue object that leads incoming IO's into the block device with a
    dummy handler that fails any incoming IO. this ensures no new IO's will
	start executing our data path.
    The draining is not explicit but rather implicit.
    a) we set the IO timeout to be very short (1/10[sec]) causing all IO's to
		fail immediately.
    b) as IO's fail, they pull IO's from resubmit queue which immediately fail
		due to the volume state.
    c) as the queue is drained, we pull IO's from the per-cpu waitlist & these
    	immediately fail when attempted retry.
    waiting for all in-flight IO's to complete is done in 2 stages:
    	a) shutdown the volumes. this completes when all topologies are unused,
    	topology list becomes empty & no IO is on a topology.
    	b) wait for IO's that might have arrived yet not attached to the
    	topology. this is done by having a ref-count of IO's that started to
		execute our make request handler.
  2) the kernel maintains a ref-count for the block-device & its associated
    request-queue. each open handle (by application, mount, etc) increases the
    ref-ount & prevents the volume from being deleted. only when the application
    closes the handle, its possible that the detached volume will actually be
    deleted. the application will then fail to open the device again since the
    path would be invalid.
  3) All sync-operations are autofailed, to prevent a problematic block from
    causing this IO to hang on forever. this is critical to allow the topologies
	to be freed
  4) we delete the gendisk & request queue, to notify the Linux kernel that they
    are dying. this will ensure it wont allow anyone to fire any new IO's at the
    make_request handler anymore. the api_os object & its associated context
    (which maintans the IO ref-count) remains alive, to be closed by the last
	closed handle.
    To ensure that any volume has a close(), we execute an internal open() &
	close(), to handle the case of a volume that is currently idle.
  5) Within simulator, the detach need not explicitly free these objects. instead
    we need to:
    	a) maintain a ref-count for the block device & request_queue (just like
			in real kernel).
    	b) free the block_device/request_queue in os-layer only when ref-count
			decreases to 0.
    	c) have the simulator test code acquire/release the block device instead
    		of simply accessing these within the "struct clientSimulator". this
    		will simulate the application doing open/close on the underlying
    		block device. such call will acquire a reference to the block_device
			& so prevent it from being deleted while used.
    		when the IO thread "closes the handle", it will decrease the
    		ref-count & allow the block-device to be freed. the next attempt to
			open() the device will fail as it will no longer be found.
*/

/**************************** Volume Reboot ***********************************/
enum reboot_vol_e {							// Rebooting steps (state machine)
	reboot_vol_STAGE_START = 0,
	reboot_vol_STAGE_VOLUME_SUSPENDED,		// All Topo users (IO's, Toma messages, etc) of prev valid topologies were drained. Head topology is not IOable and suspended
	reboot_vol_STAGE_VOLUME_RECOVERIES_DRAINED,	// All possibly running recoveries on praids (not topo users) were drained
	reboot_vol_STAGE_VOLUME_TOMA_MSGS_DRAINED,	// All TOMA messages processing for non-zombie TRs were drained
	reboot_vol_STAGE_VOLUME_REVIVE,	// Needed Only for Reboot, not for shut down. All topologies were cleared, Revive the volume
	reboot_vol_STAGE_COMPLETED,
};

// a callback that is invoked when reboot SM completes
typedef void (*on_reboot_finish_t)(void *ctx /*, int error*/);

struct bdev_shut_down_ctx {					// Rebooting context
	struct nvmeibc_topologies next_conf;	// Includes 1 head topo in the list
	struct nvmeibc_block_device *dev;		// The volume being rebooted
	void (*handler)(void *_this_struct);	// ptr to rebooting function.
	enum reboot_vol_e stage;				// State of the rebooting
	int error;								// Error code of the last stage
	enum bdev_shut_down_menu {				// Daniel: This enum suspiciously resembles windows-xp menu upon Alt+F4 :-)
		bdev_menu_reboot,					// Shutdown & reboot into a new topology based on a new config. Solving bad reconfiguration of volume
		bdev_menu_shutdown,					// Detach volume, all infilight io/locks completed. Solving unsafe detach
		bdev_menu_clntupdate,				// Remove nvmeibc for upgrade, same as detach but new io's are stored externally instead of autofailing them
	} variant;								// Variation on the details of reboot menu
	on_reboot_finish_t  when_done_cb; 		// Upon completion call this callback
    void               *when_done_ctx;		// Context for upon completion callback
    struct list_head reboot_ops_n;			// In dev->reboot_ops. If the first element - executing, otherwise queued
};
#define bdev_shut_down_ctx_init(c, _dev, _cb, _ctx) \
	c->dev           = _dev; \
	c->handler       = __block_reboot_vol_o; \
	c->stage         = reboot_vol_STAGE_START; \
	c->when_done_cb  = _cb; \
	c->when_done_ctx = _ctx;

static inline const char* variant_tostring(const struct bdev_shut_down_ctx *c)
{
	switch (c->variant) {
	case bdev_menu_reboot:     return "rebooting";
	case bdev_menu_shutdown:   return "detaching";
	case bdev_menu_clntupdate: return "updating";
	default:				   return "????";
	}
}

void __on_last_recovery_finish_upon_detach(void* _dev)
{
	struct nvmeibc_block_device *dev = _dev;
	struct bdev_shut_down_ctx *c;
	BUG_ON(list_empty(&dev->reboot_ops));
	c = list_first_entry(&dev->reboot_ops, struct bdev_shut_down_ctx, reboot_ops_n);
	c->stage = reboot_vol_STAGE_VOLUME_RECOVERIES_DRAINED;
	/* Daniel: Dont call c->handler(c) directly! It may cause deadlock, coz we
	   are in context of recovery. Later flow might try to do something to
	   recovery object. Can use any asyncronous context (example: main-wq) */
	nvmeibc_block_set_generic_work_to_self(nvmeibc_cinst_get_blok_p(dev), dev->uuid, c->handler, c, false);
}

void __on_toma_msg_handlers_drained(struct toma_msg_handlers_drainer *tmhd)
{
	struct nvmeibc_block_device *dev = container_of(tmhd, struct nvmeibc_block_device, topologies.tmhd);
	struct bdev_shut_down_ctx *c;
	BUG_ON(list_empty(&dev->reboot_ops));
	c = list_first_entry(&dev->reboot_ops, struct bdev_shut_down_ctx, reboot_ops_n);
	WARN_ON(c->stage != reboot_vol_STAGE_VOLUME_RECOVERIES_DRAINED);
	c->stage = reboot_vol_STAGE_VOLUME_TOMA_MSGS_DRAINED;
	nvmeibc_block_set_generic_work_to_self(nvmeibc_cinst_get_blok_p(dev), dev->uuid, c->handler, c, false);
}

#include "block/os_api/nvmeibc_block_api_os_common.h"	// for nvmeibc_assert_on_main_wq()

static void __block_reboot_vol_o(void *context)
{
	struct bdev_shut_down_ctx *c = context;
	struct bdev_shut_down_ctx *c_next = NULL;
	struct nvmeibc_block_device *dev = c->dev;
_func_start:
	nvmeibc_assert_on_main_wq(nvmeibc_cinst_get_blok_m((dev->os)->driver_context));
	WARN((c != list_first_entry_or_null(&dev->reboot_ops, struct bdev_shut_down_ctx, reboot_ops_n)), "nvmeibc bug! Handling non-current reboot_op");

	if (c->error != 0) {
		_NT(trace_api_conf_block_reboot_vol_o, "volume @DEV_NAME, failed, stage=@CLNT_SHUTDOWN_STAGE, err=@ERR", dev->name, c->stage, c->error);
		c->stage = reboot_vol_STAGE_COMPLETED;
	}
	switch (c->stage) {
		case reboot_vol_STAGE_START:{
			int err = 0;
			_NT(trace_1_api_conf_block_reboot_vol_o, "@DEV_NAME: @STR", dev->name, variant_tostring(c));
			c->stage = reboot_vol_STAGE_VOLUME_SUSPENDED;
			if (!is_suspended(dev->topologies)) {
				err = nvmeibc_block_suspend(dev, c, c->handler);
				if (!err)
					return;	/* restart func as callback upon suspend completion */
				c->error = err;	// Abort rebooting
			} else { /* Daniel: Possible race condition here. We don't have flag
				   which marks when volume is actually suspended, only when
				   suspend started. So it is Already suspended go to next stage*/
			}
			goto _func_start;
		}

		case reboot_vol_STAGE_VOLUME_SUSPENDED:{
			_NT(trace_2_api_conf_block_reboot_vol_o, "volume @DEV_NAME is suspended", dev->name);
			if (nvmeibc_recovs_drainer_dec(&dev->dp.running_recovs)) {
				_NT(trace_3_api_conf_block_reboot_vol_o, "volume @DEV_NAME, no recoveries running, drained inline", dev->name);
			} else {
				_NT(trace_4_api_conf_block_reboot_vol_o, "volume @DEV_NAME, waiting for recoveries to finish...", dev->name);		// Last recovery will return execution to state machine
			}
			return;
		}

		case reboot_vol_STAGE_VOLUME_RECOVERIES_DRAINED:{
			struct nvmeibc_topologies *nt = &dev->topologies;
			_NT(trace_5_api_conf_block_reboot_vol_o, "volume @DEV_NAME recoveries drained", dev->name);
			if (c->variant != bdev_menu_reboot) {// Update nvmeibc/force detach - both dont need topologies
				block_api_os_drain_io(dev->os);					// Protect volume from newlly arriving IO's to be able to kill resubmittion object
				c->error = nvmeibc_topologies_cleanup(nt);
			} else {
				nvmeibc_topology_force_replace_t_act(nt, &c->next_conf); 	// Activate new, delete old
				c->error = nvmeibc_topologies_cleanup(nt);
				dp_io_stats_clear(&dev->dp.io_stats);
			}
			toma_msg_handlers_drainer_put(&dev->topologies.tmhd);	// Let it come down to zero (counter the init of 1)
			return;
		}

		case reboot_vol_STAGE_VOLUME_TOMA_MSGS_DRAINED:{
			_NT(trace_7_api_conf_block_reboot_vol_o, "volume @DEV_NAME TOMA messages drained", dev->name);
				c->stage = (c->variant != bdev_menu_reboot) ?
						reboot_vol_STAGE_COMPLETED :
						reboot_vol_STAGE_VOLUME_REVIVE;
			goto _func_start;
		}

		case reboot_vol_STAGE_VOLUME_REVIVE:{ 						// Apply next_conf
			struct nvmeibc_topologies *nt = &dev->topologies;
			nvmeibc_topology_force_replace_t_inj(nt, &c->next_conf);
			nvmeibc_recovs_drainer_reinit(&dev->dp.running_recovs);
			toma_msg_handlers_drainer_reinit(&dev->topologies.tmhd);
			c->error = nvmeibc_block_revive(dev);
			c->stage = reboot_vol_STAGE_COMPLETED;
			goto _func_start;
		}
		case reboot_vol_STAGE_COMPLETED:{ // Free memory
			_NT(trace_6_api_conf_block_reboot_vol_o, "@DEV_NAME: @STR done, err=@RV", dev->name, variant_tostring(c), c->error);
			if (c->variant == bdev_menu_reboot) {
				nvmeibc_topology_free_new_conf_t(&c->next_conf);
			}
			if (c->error != 0){ // Daniel, Currently no special treatment needed
			}

			if (c == list_first_entry(&dev->reboot_ops, struct bdev_shut_down_ctx, reboot_ops_n)) {
				list_del(&c->reboot_ops_n);
				/* If there is a next queued op, we start its execution
				 * right after completing this one.
				 * In this case dev can't get free'd during the completion
				 * as the shutdown op is never queued. */
				c_next = list_first_entry_or_null(&dev->reboot_ops, struct bdev_shut_down_ctx, reboot_ops_n);
			}

			if (c->when_done_cb)
				c->when_done_cb(c->when_done_ctx /*, c->error*/);
			kfree(c);

			/* Launch the next reboot */
			if (c_next)
				c_next->handler(c_next);

			return;
		}
	default:;
	} // switch (c->stage)
	BUG();	// Illegal state in reboot state machine
}

static void __nvmeibc_block_do_reboot_or_shutdown_vol(struct nvmeibc_block_device *dev, struct bdev_shut_down_ctx *c)
{
	if (!list_empty(&dev->reboot_ops)) {
		/* A reboot op is in progress. Queue the new reboot/shutdown op */
		list_add_tail(&c->reboot_ops_n, &dev->reboot_ops);
	} else {
		/* Reboot/shutdown now */
		list_add(&c->reboot_ops_n, &dev->reboot_ops);
		c->handler(c);
	}
}

/* Generic function which invokes rebooting to new configuiration */
static int nvmeibc_block_reboot_vol(struct nvmeibc_block_device *dev,
									struct nvmeibc_topologies *next_conf)
{
	struct bdev_shut_down_ctx *c = kzalloc(sizeof(*c), GFP_ATOMIC);
	if (!c)
		return -ENOMEM; /* Reboot attempt completely failed */

	if (!list_empty(&dev->reboot_ops)) {
		struct bdev_shut_down_ctx *c_last = list_last_entry(&dev->reboot_ops, struct bdev_shut_down_ctx, reboot_ops_n);
		if (c_last->variant != bdev_menu_reboot) {
			_NW(warn_nvmeibc_block_reboot_vol, DMESG_PREFIX("@DEV_NAME") ": Attempt to reboot while a shutdown is queued {@STR}", dev->name, variant_tostring(c_last));
			kfree(c);
			return -EINVAL;
		}
	}

	bdev_shut_down_ctx_init(c, dev, NULL, NULL);
	c->next_conf = *next_conf;
	INIT_LIST_HEAD(&c->next_conf.topologies);
	list_move(next_conf->topologies.next, &c->next_conf.topologies);
	c->variant = bdev_menu_reboot;
	__nvmeibc_block_do_reboot_or_shutdown_vol(dev, c);
	return 0;
}

/* Generic function which invokes shutdown of volume */
static int nvmeibc_block_shutdown_vol(struct nvmeibc_block_device *dev,
			on_reboot_finish_t cb, void *ctx, enum bdev_shut_down_menu shutdown_variant)
{
	struct bdev_shut_down_ctx *c = kzalloc(sizeof(*c), GFP_ATOMIC);
	_NT(trace_api_conf_nvmeibc_block_shutdown_vol, "@DEV_NAME: Initiating @SHUTDOWN_VARIANT", dev->name, shutdown_variant);
	if (!c)
		return -ENOMEM; /* Reboot attempt completely failed */

	if (!list_empty(&dev->reboot_ops)) {
		struct bdev_shut_down_ctx *c_last = list_last_entry(&dev->reboot_ops, struct bdev_shut_down_ctx, reboot_ops_n);
		if (c_last->variant != bdev_menu_reboot) {
			_NE(err_nvmeibc_block_shutdown_vol, DMESG_PREFIX("@DEV_NAME") ": Attempt to shutdown while a shutdown is already queued {@STR}", dev->name, variant_tostring(c_last));
			kfree(c);
			return -EINVAL;
		}
	}

	bdev_shut_down_ctx_init(c, dev, cb, ctx);
	c->variant = shutdown_variant;
	__nvmeibc_block_do_reboot_or_shutdown_vol(dev, c);
	return 0;
}

/**************************** Update configuration ****************************/
static void __update_topo_from_volume_hdr(struct nvmeibc_block_device *dev)
{
	unsigned long flags;
	struct nvmeibc_topologies *nt = &dev->topologies;
	const u64 reservation_version = nvmeibc_block_get_res_vat(dev)->res.version;
	spin_lock_irqsave(&nt->lock, flags);
	nt->reservation_version = reservation_version;
	nt->reservation_version_max_seen = max(nt->reservation_version_max_seen, reservation_version);
	spin_unlock_irqrestore(&nt->lock, flags);
	_NT(t_01_urv_vol_hdr, "device @DEV_NAME: cnt_@RES_MOD_VER", dev->name, reservation_version);
	BUILD_BUG_ON(NVMEIB_C_TO_M_VOLUME_ACTION_REQUEST_RC != RESERVATION_MODE_IRRELEVANT);
}

bool force_reconf_reboot = true;			// By default, disable hot transition
module_param(force_reconf_reboot, bool, 0644);
MODULE_PARM_DESC(force_reconf_reboot, "Force all block device configuration changes to be done via device reboot, i.e. restarting the block device.");

int nvmeibc_block_reconf(struct nvmeibc_volume_conf *conf, struct nvmeibc_volume *volume)
{
	struct nvmeibc_topologies next_conf; // Temp alternative to 'nt', view new conf as a standalone partial topology
	struct nvmeibc_topology *t = NULL;
	int rv = -EDOM;
	int version = volume->hdr.version;
	struct nvmeibc_block_device *dev = volume->block_dev;
	const bool has_reservation_version_changed = (conf->reservation.version != dev->topologies.reservation_version);
	bool warm_fallback = false;	/* default is hot relocation */
	bool need_reconnect_os, upgrade_from_recovery_only_to_visible, need_update_vat = false;
	u64 ro_header_sectors;

	_NT(t_01_cbrcnf, "@DEV_NAME: Reconfigure to @C_VOL_VER, @HDR_UUID", dev->name, version, dev->uuid);
	if (strcmp(dev->uuid, volume->hdr.uuid)) {
		_NT(t_02_cbrcnf, "nvmeibc got reconf with incorrect @HDR_UUID @HDR_TYPE", volume->hdr.uuid, dev->type);
		rv = -EINVAL;
		goto _out;
	} else { // Allow volume rename (Do not revert in case of reconf failure)
		if (strcmp(dev->name, volume->hdr.devname)) {
			_NT(t_03_cbrcnf, "@DEV_NAME: Reconfiguring name to @DEV_NAME", dev->name, volume->hdr.devname);
			snprintf(dev->name, sizeof(dev->name), "%s", volume->hdr.devname);
		}
	}

	need_reconnect_os = (!nvmeibc_block_is_hidden(&volume->hdr)) && dev->os->is_io_api_disabled;	// Recoverer volume that was upgraded to normal io-able volume
	upgrade_from_recovery_only_to_visible = (!nvmeibc_block_is_recoverer(&volume->hdr)) && nvmeibc_block_is_recoverer(dev);
	if (has_reservation_version_changed) {
		_NT(t_10_cbrcnf, "@DEV_NAME: reservation change: @RES_MOD_VER->@RES_MOD_VER", dev->name, dev->topologies.reservation_version, conf->reservation.version);
		// Here we know that when IO will be enabled, volume will become preempted
	}
	if (unlikely(need_reconnect_os || upgrade_from_recovery_only_to_visible)) {			// Done in 3 steps: update reserv_ver --> unregister --> enable io. Otherwise if we enable IO too soon we may cause a data corruption
		dev->type = volume->hdr.type;			// Remove the recoverer property
		need_update_vat = has_reservation_version_changed; // Update from RESERVATION_MODE_IRRELEVANT -> Another
		BUG_ON(conf->reservation.version == RESERVATION_MODE_IRRELEVANT);
		__update_topo_from_volume_hdr(dev);	// The only case that updation of 'vat' is allowed
	}
	if (dev->type != volume->hdr.type) { // Should only be Hidden->Recoverer (AND HIDDEN)
		WARN_ON(!nvmeibc_block_is_hidden(dev));
		WARN_ON(nvmeibc_block_is_recoverer(dev));
		WARN_ON(!nvmeibc_block_is_hidden(&volume->hdr));
		WARN_ON(!nvmeibc_block_is_recoverer(&volume->hdr));
		dev->type = volume->hdr.type;	// Update block dev type as well
	}

	/* Check if we haven't fully applied the previous configuration yet */
	t = nvmeibc_topology_get(&dev->topologies);
	warm_fallback = (nvmeibc_topology_is_reconfiguring_now(t) || need_update_vat);			// Ilelgal to update VAT in hot fashion
	nvmeibc_topology_put(t);
	if (warm_fallback) { /* Existing state (Previous configuration) fall-back to warm */
		rv = nvmeibc_warm_apply_conf_diffs(&dev->topologies);
	}
	if (need_reconnect_os) {	// Step 3 of reconnect_os
		_NT(t_04_cbrcnf, "@DEV_NAME: reconnecting os for io, need_update_vat=@BOOL_YN", dev->name, need_update_vat);
		dev->os->is_io_api_disabled = 0;
		ro_header_sectors = nvmeibc_block_get_ro_header_sectors(conf);
		if (ro_header_sectors != dev->os->ro_header_sectors) {
			_NE(t_refconv_header_size_changed,
			    "refconf changed ro_header_size from @LONG to @LONG",
			    dev->os->ro_header_sectors, ro_header_sectors);
			rv = -EINVAL;
		} else {
			rv = nvmeibc_block_upgrade_os_to_ioable(
				dev, dev->os->slice_size,
				ro_header_sectors); // OS is created according to current updated vat
		}
		if (rv < 0) {
			_NE_to_user(t_05_cbrcnf, DMESG_PREFIX("@DEV_NAME"), "Unexpected internal error, volume will not be able to serve io. Try detaching and reattaching it. Error code: 1019. Internal code: @RV", dev->name, rv);
			goto _out;
		}
	}

	memset(&next_conf, 0, sizeof(next_conf));												// Note: next_conf will never replace dev->topologies. It is just a place holder for holding a list of single topology
	INIT_LIST_HEAD(&next_conf.topologies);
	next_conf.nd = dev;

	next_conf.device_name = dev->name;
	nvmeibc_topo_init_io_perm(&next_conf);
	rv = nvmeibc_topology_update_configuration(&next_conf, conf, version, true, &volume->info.disks);
	if (rv < 0) {
		_NT(t_06_cbrcnf, "@DEV_NAME: update_configuration failed", dev->name);
		goto _out;
	}

	if (!force_reconf_reboot) {			// Attempt Hot/Warm transition
		t = list_first_entry(&next_conf.topologies, struct nvmeibc_topology, list_n);
		rv = nvmeibc_calc_and_append_conf_diffs(&dev->topologies, t);
		t = NULL;
	} else {
		_NT(t_0a_cbrcnf, "@DEV_NAME: reconf autofail by module param", dev->name);
		rv = -EINVAL;
	}
	if (unlikely(rv < 0)) {
		_NT(t_07_cbrcnf, "@DEV_NAME: append_conf failed", dev->name);
		if (rv == -EINVAL){
			int recover = nvmeibc_block_reboot_vol(dev, &next_conf);
			if (recover == 0) {
				_NT(t_08_cbrcnf, "@DEV_NAME: recovering half-warm to conf @C_VOL_VER", dev->name, version);
				rv = 0; /* Report as if nothing was reconfed/failed */
				goto _out;	/* next_conf is free() during reboot */
			} else {
				_NT(t_09_cbrcnf, "@DEV_NAME: half-warm failed(@RV)! reason: @STR" , dev->name, recover, (recover == -EINVAL) ? "Volume already rebooting, will try again" : "No memory for updaing volume configuration");
				if (recover == -EINVAL) { // Mark reschedule attempt to update volume configuration
					rv = -EBUSY;
				}
			}
		}
	}
	/* We injected the new configuration or ecountered unrecoverable error. next_conf - topo is not needed anymore.  */
	nvmeibc_topology_free_new_conf_t(&next_conf);

	if (rv > 0) { /* Subsribe segs only if have at least 1 seg */
		if (nvmeibc_subscribe_requested_conf_diffs(&dev->topologies) < 0)
			rv = -ENOMEM;	/* Failure to subscribe due to lack of memory */
	}
	#ifdef DEBUG_TOPO_CNTRS
		nvmeibc_topologies_duplicate(&dev->topologies, NULL);	// For debug only: Cause append_diffs and apply_diffs to be on different topos. Simulates a rare race condition when topology is changed between those 2 steps (example: msg on different praid)
	#endif

	if (rv > 0) { /* Apply reconfiguration only if have at least 1 seg */
		/* Try to replace segments which were already requested by toma */
		int hot = nvmeibc_apply_requested_conf_diffs(&dev->topologies);
		warm_fallback = (warm_fallback || (hot == -EPERM));
		if (warm_fallback) { /* Fallback to warm. Replace all the segments */
			rv = nvmeibc_warm_apply_conf_diffs(&dev->topologies);
		}
		rv = 0;	/* 0 means success */
	}
	if (rv >= 0) { /* ==0 is also included coz newconf may not change any seg */
		nvmeibc_topologies_conf_register_mismatch_segs(&dev->topologies);
	}
_out:
	return rv;
}

int nvmeibc_block___conf(struct nvmeibc_volume_conf *conf, struct nvmeibc_volume *volume, struct nvmeibc_block_device *dev)
{
	const int version = volume->hdr.version;
	int rv;
	dev->volume = volume;
	__update_topo_from_volume_hdr(dev);
	rv = nvmeibc_topology_update_configuration(&dev->topologies, conf, version, false, &volume->info.disks);
	if (rv < 0)
		_NE(t_02_cbconf, DMESG_PREFIX("@DEV_NAME") ": Could not set topology, @HDR_UUID, rv=@RV", volume->hdr.devname, volume->hdr.uuid, rv);
	return rv;
}

int nvmeibc_wait_for_io_drain(struct nvmeibc_block_device *dev,
							  void (*on_finish_cb)(void *ctx), void *ctx)
{
	int	rv = 0;
	if (on_finish_cb) {
		rv = nvmeibc_block_shutdown_vol(dev, on_finish_cb, ctx, (dev->status == NCBD_DETACHING_UPGRD) ? bdev_menu_clntupdate : bdev_menu_shutdown);
	} else { /* Handles attach error. Safe detach: Caller assumes no io's so no need to do shutdown.*/
		WARN(nvmeibc_block_try_detach(dev, nvmeibc_vol_detach_cmd_error()), "nvmeibc bug! IO is possible - kernel curruption");
		nvmeibc_topologies_cleanup(&dev->topologies);
	}
	return rv;
}

