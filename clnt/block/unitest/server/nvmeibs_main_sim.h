#ifndef NVMEIBS_MAIN_SIM_H
#define NVMEIBS_MAIN_SIM_H
/* Storage server simulator. Each server includes:
 * 	1. A single nvme ssd physical disk simulator (as if server has 1 disk
 * 	   only). Disks ay vary in their sizes and params.
 * 	2. Ram data structures for each disk (Owner locks table, dbits, active
 * 	   locks, erasurem coding data structures etc).
 *	   Toma Simulator.
 *  3. Serjio simulator and server side communication with client
 *  4. Implementation of nvmeibs_main.c, nvmeibs_nordda.c, nvmeibs_client.c
 */
#include "../nvmeibc_simu_disk.h"

struct serverSimulator {
	struct tomaSimulator simToma;                            /* Toma simulator */
	struct ramDiskSimulator ramDisk;                         /* Nvme disk */
	const struct mdb_target_conf *hardware;                  /* Poiner to external hardware specification (save memory by allocating it once in mongo-db) */
	union jblock_md* jmdc;            /* Dma mapping of serjio jmdc_mem->virt[0] */
	struct nvmeibc_disk* disk;                               /* Yuri: @WARNING: this is temporary. Used for serjio. Surrently serjio does not support multiclient in simulator. @TODO: Add serjio MC support, remove this crap. */
	struct nvmeibc_disk* client_disks[NVMESH_N_MAX_CLIENTS]; /* Point back to clients disk (coz server intiates messages to clnt: serjio->jam */
	void *jrange_handle;
};

#define as_serverSimulator(_di)                                                                                        \
	({                                                                                                                 \
		struct ramDiskSimulator *__ramDisk = as_ramDisk(_di);                                                          \
		container_of(__ramDisk, struct serverSimulator, ramDisk);                                                      \
	})

#define nvmeibs_jmdc_ind_of(jri, entry, binje) ((jri) * NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE + ((entry) * (binje)))
#define nvmeibs_jdisk_ptr(S, jri, entry, entry_offset, binje) (&(S)->serjio.jranges_start[(nvmeibs_jmdc_ind_of(jri, entry, binje) + entry_offset) << NVMEIB_EC_JOURNAL_SECTOR_SHIFT])

void serverSimulator_init_hardware(struct serverSimulator*, int unique_id, struct mdb_target_conf*); //initializes the server simulator internal structs hardware and ramDisk
void serverSimulator_init_serjio(struct serverSimulator*); //initializes the serjio

void serverSimulator_destroy(struct serverSimulator*); //destroys the internal struct and the serverSimulator instance

void serverSimulator_disconnect(struct serverSimulator*);	// Immitate as if server is not reachable by the client's (though it is still running). Client will receive PAUSE
void serverSimulator_re_connect(struct serverSimulator*);	// Reverse of the above
void serverSimulator_notify_new_disk_sgmnts(struct serverSimulator*);
static inline struct ramDiskSimulator *serverSimulator_get_ram_by_toma(     const struct tomaSimulator*    t) {return &container_of(t, struct serverSimulator, simToma)->ramDisk;}
static inline struct serverSimulator  *serverSimulator_get_srvr_by_nvmedisk(const struct ramDiskSimulator* D) {return  container_of(D, struct serverSimulator, ramDisk);}
static inline struct tomaSimulator    *serverSimulator_get_toma_by_ram(     const struct ramDiskSimulator* D) {return &container_of(D, struct serverSimulator, ramDisk)->simToma;}

#define nvmeibs_simu_print_jmdc(S) nbdpec_md_to_string((S)->jmdc, sizeof((S)->jmdc[0]), NVMEIB_EC_TOTAL_JOURNAL_BLKS, 'J')

//server side block related pointers in a single place; mainly used by transactions to setup state before and validate it after
struct block_inject_ptrs{
	struct serverSimulator *self; //convenience pointer to the server this info was constructed from
	u64 dlba;					   //convenience; dlba address for the pointers
	u32 jri;						//convenience; jri for the pointers
	u32 jentry;						//convenience; jentry for the pointers
	u8* data; //data on disk
	struct {
		union nvmeibc_block_dp_ec_data_block_md* dmd; //data metadata on disk
		u64 rlba;
	};

	u8* jrnl; //journal data on disk or null
	union jblock_md*   jmd; //jrnl metadata on disk or null

	struct block_ram_inject_ptrs{
		u32 *lock;							//    |
		u32 *txid;							// <==| 3 fields, which form blockset info, obviously make sense only for lock porters
		union nvmeibc_dbits_entry *dbits;	//    |
		union jblock_md *jmdc; //jmdc entry
	} ram;
	union nvmeib_lock_id *toma_stale_lock;
}; // use serverSimulator_get_block_inject_ptrs function for construction

static inline bool block_inject_ptrs_do_have_jrnl(const struct block_inject_ptrs* inj){
	BUG_ON(!inj);
	return inj->jrnl || inj->jmd || inj->ram.jmdc;
}

struct block_inject_ptrs
serverSimulator_get_block_inject_ptrs(struct serverSimulator *self, u64 addr, s32 jri, s32 jentry, u32 height);
/********************** Erasure coding support ********************************/
// Give journal chunks to clients, revoke and cleanup
void serverSimulator_disk_discover(struct serverSimulator *S, struct nvmeibc_disk* c_disk, uuid_be* cuuid, void **jrange_handle_ptr);	//On discovery() of disk D - server side action
void serverSimulator_disk_relese(  struct serverSimulator *S, u32 jri, void *jrange_handle);
void serverSimulator_disk_discover_by_unitest(struct serverSimulator* S);			// Same as regular discover but done by fake client represented by unitest environment
void serverSimulator_allow_journals(struct serverSimulator*, bool allow /*default is true */); // API towards unitests environment

int  nvmeibs_find_jri_by_uuid(struct serverSimulator* S,    const uuid_be*   cuuid);	// Given UUID get the jri. If not mapped returnes -1
int  nvmeibs_get_jri_by_uuid(struct serverSimulator* S,    const uuid_be*   cuuid);	// Given UUID get the jri. If not mapped returnes -1
bool nvmeibs_is_other_client(const uuid_be cuuid);
void serverSimulator_serjio_drain_wq(struct serverSimulator* S);

/************************ Simulator of nvmeibs_no_rdda.c/.h *******************/
/* No RDDA server side operations executed via admin or IO channel, not lock channel (like RDMAs)*/
int  nvmeibs_nordda_get_array_binfo(  struct serverSimulator *S, struct nvmeibc_disk_gen_cmd *cmd);

int  nvmeibs_nordda_jentry_erase( struct serverSimulator *S, struct nvmeibc_disk_gen_cmd *cmd);
int  nvmeibs_nordda_get_jrng_by_uuid( struct serverSimulator *S, struct nvmeibc_disk_gen_cmd *cmd);  // Simulation of: handle_get_uuid_jour()
void nvmeibs_nordda_process_jmd_free_abandoned(struct serverSimulator *S, u64 client_id, u32 rng_num, binje_t rng_binje, u64 rng_gen_id,
											   u32 *abnd_free_bitmap, struct nvmeib_jrnl_ent_md *ent_md);	// Simulation of nvmeibs_client_send_journal_abnd_free()

/************************ Simulator of nvmeibs_toma.c/.h *******************/
int nvmeibs_toma_intercept_msg(struct serverSimulator *S, u64 handle, struct nvmeibc_disk_toma_send_params *p);
int nvmeibs_toma_report_event_serjio_disk_range_cleaned(void *di, const char *seg_id);			// Todo: MAke this!!!
int nvmeibs_toma_to_serjio_clean_range(struct serverSimulator *S, const char *seg_uuid, const u64 start_lba, const u64 end_lba, bool seg_delete);

/************************ Simulator of nvmeibs_main.c/.h *******************/
int nvmeibs_remove_cid_clients(u64 cid, enum nvmeibs_logout_reason reason);
void nvmeibs_pass_loser_to_serjio(struct serverSimulator *S, struct nvmeibs_lost_srv_resource_payload *p);

/********************* API towards unitesting environment ******************/
void nvmeibs_nordda_jour_entry_do(struct serverSimulator *S, u32 jri, u32 ei, const char* action);
void nvmeibs_nordda_verify_no_abandjour(struct serverSimulator *S);
u32 nvmeibs_nordda_get_abandjour_in_jri(struct serverSimulator *S, u32 jri);
int nvmeibs_nordda_count_abandjour_in_jri(struct serverSimulator *S, u32 jri);

int nvmeibs_pass_gen_to_server(struct serverSimulator *S, struct nvmeibc_disk_gen_cmd *gen_cmd);
int nvmeibs_pass_gen_to_nordda_channel(struct serverSimulator *S, struct nvmeibc_disk_gen_cmd *gen_cmd);

#endif  // H beginning
