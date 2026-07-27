#ifndef NVMEIBC_BLOCK_API_CONF_H
#define NVMEIBC_BLOCK_API_CONF_H
/* Component used for changing configurations of block device after it is
   already attached
 */

int nvmeibc_block_reconf(struct nvmeibc_volume_conf *conf,
						 struct nvmeibc_volume *volume);

/* Let's understand our geometry (total disk size and stuff like that) and build
   a more efficient data structure for locating blocks afterwards. */
int nvmeibc_block___conf(struct nvmeibc_volume_conf *conf,
	struct nvmeibc_volume *volume, struct nvmeibc_block_device *dev);

int nvmeibc_wait_for_io_drain(struct nvmeibc_block_device *dev,
							  void (*on_finish_cb)(void *ctx), void *ctx);

// Callback for detach after all recoveries were drained;
void __on_last_recovery_finish_upon_detach(void* _dev);	// void* to be compatible with kref and such
void __on_toma_msg_handlers_drained(struct toma_msg_handlers_drainer *tmhd);
#endif  // H beginning
