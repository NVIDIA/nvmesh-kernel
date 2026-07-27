#ifndef NVMEIBT_IMPORTANT_LOGS
#define NVMEIBT_IMPORTANT_LOGS
#include "nvmeibt_common.h"

#define NVMEIBT_IMPORTANT_LOGS_CONVERT_TO_LEADER(name) N_IMf(name, "    [-----  RAFT LEADER ------]")
#define NVMEIBT_IMPORTANT_LOGS_NEW_LEADER(name, _leader_name) N_IMf(name, "new LEADER=@STR", _leader_name)
#define NVMEIBT_IMPORTANT_LOGS_DUMP_LEADER_REPORT_LINE_FOR_VOLUME(name, _volname, _str) N_Tf(name, "@BLOCK_DEVICE_STR: @STR", _volname, _str);

#endif // ifdef NVMEIBT_IMPORTANT_LOGS
