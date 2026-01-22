/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#ifndef NVMEIBT_NVME_DEFINES
#define NVMEIBT_NVME_DEFINES

#include <linux/types.h>

#define NVME_IOCTL_ID           _IO('N', 0x40)
#define NVME_IOCTL_ADMIN_CMD    _IOWR('N', 0x41, struct nvme_admin_cmd)
#define NVME_IOCTL_SUBMIT_IO	_IOW('N', 0x42, struct nvme_user_io)
#define NVME_IOCTL_IO_CMD       _IOWR('N', 0x43, struct nvme_passthru_cmd)

#define nvme_admin_cmd nvme_passthru_cmd

struct nvme_user_io {
        __u8    opcode;
        __u8    flags;
        __u16   control;
        __u16   nblocks;
        __u16   rsvd;
        __u64   metadata;
        __u64   addr;
        __u64   slba;
        __u32   dsmgmt;
        __u32   reftag;
        __u16   apptag;
        __u16   appmask;
};

struct nvme_passthru_cmd {
        __u8    opcode;
        __u8    flags;
        __u16   rsvd1;
        __u32   nsid;
        __u32   cdw2;
        __u32   cdw3;
        __u64   metadata;
        __u64   addr;
        __u32   metadata_len;
        __u32   data_len;
        __u32   cdw10;
        __u32   cdw11;
        __u32   cdw12;
        __u32   cdw13;
        __u32   cdw14;
        __u32   cdw15;
        __u32   timeout_ms;
        __u32   result;
};


/* Admin commands */
enum nvme_admin_opcode {
        nvme_admin_delete_sq            = 0x00,
        nvme_admin_create_sq            = 0x01,
        nvme_admin_get_log_page         = 0x02,
        nvme_admin_delete_cq            = 0x04,
        nvme_admin_create_cq            = 0x05,
        nvme_admin_identify             = 0x06,
        nvme_admin_abort_cmd            = 0x08,
        nvme_admin_set_features         = 0x09,
        nvme_admin_get_features         = 0x0a,
        nvme_admin_async_event          = 0x0c,
        nvme_admin_ns_mgmt              = 0x0d,
        nvme_admin_activate_fw          = 0x10,
        nvme_admin_download_fw          = 0x11,
        nvme_admin_dev_self_test        = 0x14,
        nvme_admin_ns_attach            = 0x15,
        nvme_admin_keep_alive           = 0x18,
        nvme_admin_directive_send       = 0x19,
        nvme_admin_directive_recv       = 0x1a,
        nvme_admin_virtual_mgmt         = 0x1c,
        nvme_admin_nvme_mi_send         = 0x1d,
        nvme_admin_nvme_mi_recv         = 0x1e,
        nvme_admin_dbbuf                = 0x7C,
        nvme_admin_format_nvm           = 0x80,
        nvme_admin_security_send        = 0x81,
        nvme_admin_security_recv        = 0x82,
        nvme_admin_sanitize_nvm         = 0x84,
};

enum {
        NVME_QUEUE_PHYS_CONTIG  = (1 << 0),
        NVME_CQ_IRQ_ENABLED     = (1 << 1),
        NVME_SQ_PRIO_URGENT     = (0 << 1),
        NVME_SQ_PRIO_HIGH       = (1 << 1),
        NVME_SQ_PRIO_MEDIUM     = (2 << 1),
        NVME_SQ_PRIO_LOW        = (3 << 1),
        NVME_FEAT_ARBITRATION   = 0x01,
        NVME_FEAT_POWER_MGMT    = 0x02,
        NVME_FEAT_LBA_RANGE     = 0x03,
        NVME_FEAT_TEMP_THRESH   = 0x04,
        NVME_FEAT_ERR_RECOVERY  = 0x05,
        NVME_FEAT_VOLATILE_WC   = 0x06,
        NVME_FEAT_NUM_QUEUES    = 0x07,
        NVME_FEAT_IRQ_COALESCE  = 0x08,
        NVME_FEAT_IRQ_CONFIG    = 0x09,
        NVME_FEAT_WRITE_ATOMIC  = 0x0a,
        NVME_FEAT_ASYNC_EVENT   = 0x0b,
        NVME_FEAT_AUTO_PST      = 0x0c,
        NVME_FEAT_HOST_MEM_BUF  = 0x0d,
        NVME_FEAT_TIMESTAMP     = 0x0e,
        NVME_FEAT_KATO          = 0x0f,
        NVME_FEAT_RRL           = 0x12,
        NVME_FEAT_PLM_CONFIG    = 0x13,
        NVME_FEAT_PLM_WINDOW    = 0x14,
        NVME_FEAT_SW_PROGRESS   = 0x80,
        NVME_FEAT_HOST_ID       = 0x81,
        NVME_FEAT_RESV_MASK     = 0x82,
        NVME_FEAT_RESV_PERSIST  = 0x83,
        NVME_LOG_ERROR          = 0x01,
        NVME_LOG_SMART          = 0x02,
        NVME_LOG_FW_SLOT        = 0x03,
        NVME_LOG_CMD_EFFECTS    = 0x05,
        NVME_LOG_TELEMETRY_HOST = 0x07,
        NVME_LOG_TELEMETRY_CTRL = 0x08,
        NVME_LOG_ENDURANCE_GROUP = 0x09,
        NVME_LOG_DISC           = 0x70,
        NVME_LOG_RESERVATION    = 0x80,
        NVME_LOG_SANITIZE       = 0x81,
        NVME_FWACT_REPL         = (0 << 3),
        NVME_FWACT_REPL_ACTV    = (1 << 3),
        NVME_FWACT_ACTV         = (2 << 3),
};


struct nvme_smart_log {
        __u8                    critical_warning;
        __u8                    temperature[2];
        __u8                    avail_spare;
        __u8                    spare_thresh;
        __u8                    percent_used;
        __u8                    rsvd6[26];
        __u8                    data_units_read[16];
        __u8                    data_units_written[16];
        __u8                    host_reads[16];
        __u8                    host_writes[16];
        __u8                    ctrl_busy_time[16];
        __u8                    power_cycles[16];
        __u8                    power_on_hours[16];
        __u8                    unsafe_shutdowns[16];
        __u8                    media_errors[16];
        __u8                    num_err_log_entries[16];
        __le32                  warning_temp_time;
        __le32                  critical_comp_time;
        __le16                  temp_sensor[8];
        __le32                  thm_temp1_trans_count;
        __le32                  thm_temp2_trans_count;
        __le32                  thm_temp1_total_time;
        __le32                  thm_temp2_total_time;
        __u8                    rsvd232[280];
};

struct nvme_id_power_state {
        __le16                  max_power;      /* centiwatts */
        __u8                    rsvd2;
        __u8                    flags;
        __le32                  entry_lat;      /* microseconds */
        __le32                  exit_lat;       /* microseconds */
        __u8                    read_tput;
        __u8                    read_lat;
        __u8                    write_tput;
        __u8                    write_lat;
        __le16                  idle_power;
        __u8                    idle_scale;
        __u8                    rsvd19;
        __le16                  active_power;
        __u8                    active_work_scale;
        __u8                    rsvd23[9];
};

// Defined in section 5.2.13.2.1 Identify Controller Data Structure (CNS 01h)
// of NVM-Express-Base-Specification-Revision-2.3-2025.08.01, p. 321-353 (PDF p. 345ff).
struct nvme_id_ctrl {
        __le16                  vid;
        __le16                  ssvid;
        char                    sn[20];
        char                    mn[40];
        char                    fr[8];
        __u8                    rab;
        __u8                    ieee[3];
        __u8                    cmic;
        __u8                    mdts;
        __le16                  cntlid;
        __le32                  ver;
        __le32                  rtd3r;
        __le32                  rtd3e;
        __le32                  oaes;
        __le32                  ctratt;
        __u8                    rsvd100[156];
        __le16                  oacs;
        __u8                    acl;
        __u8                    aerl;
        __u8                    frmw;
        __u8                    lpa;
        __u8                    elpe;
        __u8                    npss;
        __u8                    avscc;
        __u8                    apsta;
        __le16                  wctemp;
        __le16                  cctemp;
        __le16                  mtfa;
        __le32                  hmpre;
        __le32                  hmmin;
        __u8                    tnvmcap[16];
        __u8                    unvmcap[16];
        __le32                  rpmbs;
        __le16                  edstt;
        __u8                    dsto;
        __u8                    fwug;
        __le16                  kas;
        __le16                  hctma;
        __le16                  mntmt;
        __le16                  mxtmt;
        __le32                  sanicap;
        __le32                  hmminds;
        __le16                  hmmaxd;
        __u8                    rsvd338[174];
        __u8                    sqes;
        __u8                    cqes;
        __le16                  maxcmd;
        __le32                  nn;
        __le16                  oncs;
        __le16                  fuses;
        __u8                    fna;
        __u8                    vwc;
        __le16                  awun;
        __le16                  awupf;
        __u8                    nvscc;
        __u8                    rsvd531;
        __le16                  acwu;
        __u8                    rsvd534[2];
        __le32                  sgls;
        __u8                    rsvd540[228];
        char                    subnqn[256];
        __u8                    rsvd1024[768];
        __le32                  ioccsz;
        __le32                  iorcsz;
        __le16                  icdoff;
        __u8                    ctrattr;
        __u8                    msdbd;
        __u8                    rsvd1804[244];
        struct nvme_id_power_state      psd[32];
        __u8                    vs[1024];
};

// Defined in NVM-Express-NVM-Command-Set-Specification-Revision-1.2-2025.08.01-Ratified.pdf, Figure 116: LBA Format Data Structure, NVM Command Set Specific, p. 91.
struct nvme_lbaf {
        __le16                  ms;
        __u8                    ds;
        __u8                    rp;
};


struct nvme_id_ns {
        __le64                  nsze;
        __le64                  ncap;
        __le64                  nuse;
        __u8                    nsfeat;
        __u8                    nlbaf;
        __u8                    flbas;
        __u8                    mc;
        __u8                    dpc;
        __u8                    dps;
        __u8                    nmic;
        __u8                    rescap;
        __u8                    fpi;
        __u8                    rsvd33;
        __le16                  nawun;
        __le16                  nawupf;
        __le16                  nacwu;
        __le16                  nabsn;
        __le16                  nabo;
        __le16                  nabspf;
        __le16                  noiob;
        __u8                    nvmcap[16];
        __u8                    rsvd64[40];
        __u8                    nguid[16];
        __u8                    eui64[8];
        struct nvme_lbaf        lbaf[16];
        __u8                    rsvd192[192];
        __u8                    vs[3712];
};



struct nvme_dev_info {
	char                path[1024];
	struct nvme_id_ctrl ctrl;
	int                 nsid;
	struct nvme_id_ns   ns;
};

enum {
        /*
         * Generic Command Status:
         */
        NVME_SC_SUCCESS                 = 0x0,
        NVME_SC_INVALID_OPCODE          = 0x1,
        NVME_SC_INVALID_FIELD           = 0x2,
        NVME_SC_CMDID_CONFLICT          = 0x3,
        NVME_SC_DATA_XFER_ERROR         = 0x4,
        NVME_SC_POWER_LOSS              = 0x5,
        NVME_SC_INTERNAL                = 0x6,
        NVME_SC_ABORT_REQ               = 0x7,
        NVME_SC_ABORT_QUEUE             = 0x8,
        NVME_SC_FUSED_FAIL              = 0x9,
        NVME_SC_FUSED_MISSING           = 0xa,
        NVME_SC_INVALID_NS              = 0xb,
        NVME_SC_CMD_SEQ_ERROR           = 0xc,
        NVME_SC_SGL_INVALID_LAST        = 0xd,
        NVME_SC_SGL_INVALID_COUNT       = 0xe,
        NVME_SC_SGL_INVALID_DATA        = 0xf,
        NVME_SC_SGL_INVALID_METADATA    = 0x10,
        NVME_SC_SGL_INVALID_TYPE        = 0x11,

        NVME_SC_SGL_INVALID_OFFSET      = 0x16,
        NVME_SC_SGL_INVALID_SUBTYPE     = 0x17,

        NVME_SC_SANITIZE_FAILED         = 0x1C,
        NVME_SC_SANITIZE_IN_PROGRESS    = 0x1D,

        NVME_SC_LBA_RANGE               = 0x80,
        NVME_SC_CAP_EXCEEDED            = 0x81,
        NVME_SC_NS_NOT_READY            = 0x82,
        NVME_SC_RESERVATION_CONFLICT    = 0x83,

     /*
         * Command Specific Status:
         */
        NVME_SC_CQ_INVALID              = 0x100,
        NVME_SC_QID_INVALID             = 0x101,
        NVME_SC_QUEUE_SIZE              = 0x102,
        NVME_SC_ABORT_LIMIT             = 0x103,
        NVME_SC_ABORT_MISSING           = 0x104,
        NVME_SC_ASYNC_LIMIT             = 0x105,
        NVME_SC_FIRMWARE_SLOT           = 0x106,
        NVME_SC_FIRMWARE_IMAGE          = 0x107,
        NVME_SC_INVALID_VECTOR          = 0x108,
        NVME_SC_INVALID_LOG_PAGE        = 0x109,
        NVME_SC_INVALID_FORMAT          = 0x10a,
        NVME_SC_FW_NEEDS_CONV_RESET     = 0x10b,
        NVME_SC_INVALID_QUEUE           = 0x10c,
        NVME_SC_FEATURE_NOT_SAVEABLE    = 0x10d,
        NVME_SC_FEATURE_NOT_CHANGEABLE  = 0x10e,
        NVME_SC_FEATURE_NOT_PER_NS      = 0x10f,
        NVME_SC_FW_NEEDS_SUBSYS_RESET   = 0x110,
        NVME_SC_FW_NEEDS_RESET          = 0x111,
        NVME_SC_FW_NEEDS_MAX_TIME       = 0x112,
        NVME_SC_FW_ACIVATE_PROHIBITED   = 0x113,
        NVME_SC_OVERLAPPING_RANGE       = 0x114,
        NVME_SC_NS_INSUFFICENT_CAP      = 0x115,
        NVME_SC_NS_ID_UNAVAILABLE       = 0x116,
        NVME_SC_NS_ALREADY_ATTACHED     = 0x118,
        NVME_SC_NS_IS_PRIVATE           = 0x119,
        NVME_SC_NS_NOT_ATTACHED         = 0x11a,
        NVME_SC_THIN_PROV_NOT_SUPP      = 0x11b,
        NVME_SC_CTRL_LIST_INVALID       = 0x11c,
        NVME_SC_BP_WRITE_PROHIBITED     = 0x11e,

        /*
         * I/O Command Set Specific - NVM commands:
         */
        NVME_SC_BAD_ATTRIBUTES          = 0x180,
        NVME_SC_INVALID_PI              = 0x181,
        NVME_SC_READ_ONLY               = 0x182,
        NVME_SC_ONCS_NOT_SUPPORTED      = 0x183,


        /*
         * I/O Command Set Specific - Fabrics commands:
         */
        NVME_SC_CONNECT_FORMAT          = 0x180,
        NVME_SC_CONNECT_CTRL_BUSY       = 0x181,
        NVME_SC_CONNECT_INVALID_PARAM   = 0x182,
        NVME_SC_CONNECT_RESTART_DISC    = 0x183,
        NVME_SC_CONNECT_INVALID_HOST    = 0x184,

        NVME_SC_DISCOVERY_RESTART       = 0x190,
        NVME_SC_AUTH_REQUIRED           = 0x191,

        /*
         * Media and Data Integrity Errors:
         */
        NVME_SC_WRITE_FAULT             = 0x280,
        NVME_SC_READ_ERROR              = 0x281,
        NVME_SC_GUARD_CHECK             = 0x282,
        NVME_SC_APPTAG_CHECK            = 0x283,
        NVME_SC_REFTAG_CHECK            = 0x284,
        NVME_SC_COMPARE_FAILED          = 0x285,
        NVME_SC_ACCESS_DENIED           = 0x286,
        NVME_SC_UNWRITTEN_BLOCK         = 0x287,

        NVME_SC_DNR                     = 0x4000,
};

enum {
        NVME_ID_CNS_NS                  = 0x00,
        NVME_ID_CNS_CTRL                = 0x01,
        NVME_ID_CNS_NS_ACTIVE_LIST      = 0x02,
        NVME_ID_CNS_NS_DESC_LIST        = 0x03,
        NVME_ID_CNS_NS_PRESENT_LIST     = 0x10,
        NVME_ID_CNS_NS_PRESENT          = 0x11,
        NVME_ID_CNS_CTRL_NS_LIST        = 0x12,
        NVME_ID_CNS_CTRL_LIST           = 0x13,
};

enum {
        NVME_NO_LOG_LSP       = 0x0,
        NVME_NO_LOG_LPO       = 0x0,
        NVME_TELEM_LSP_CREATE = 0x1,
};

#define NVME_IDENTIFY_DATA_SIZE 4096

#endif // #ifndef NVMEIBT_NVME_DEFINES

