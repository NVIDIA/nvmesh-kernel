#ifndef NVMEIBC_DP_DBG_TOOLS_H
#define NVMEIBC_DP_DBG_TOOLS_H
/*
 * Utilities to debug block device
 */
#include "nvmeibc_block.h"		/* external API of the block */
#include "common/nvmeib_error_report.h"

/* Block device private debug method enum2str() to print locks to log. */
const char* ncl_status_str(const enum nvmeibc_block_lock_status s);
const char* nvmeibc_rdma_intent_to_string(const enum nvmeibc_rdma_intent i);

struct operation;

/* Warning: Use with grate care coz on vairous stages of execution 'o' and
   'cmds' might be disconnected, better usage is __dump_operation() with debug
   topology counters */
void __dump_operation_unsafe(const struct operation *o, const struct nvmeibc_block_command *cmds, nvmeib_trace_level trace_level);

void __dump_operation(struct operation *o);

void __dump_operation_report(const struct operation *o, nvmeib_trace_level trace_level);

/*************************** DEBUG_UNCOMPLETED ********************************/
#ifdef DEBUG_UNCOMPLETED
	void __uncompleted_cmds_list_rmv(struct nvmeibc_disk_io_command *cmds);
	void __uncompleted_cmds_list_add(struct nvmeibc_disk_io_command *cmds);
	void __uncompleted_cmds_list_send(struct nvmeibc_disk_io_command *cmds);
	void __uncompleted_cmds_list_comp(struct nvmeibc_disk_io_command *cmds, int);
	void __uncompleted_cmds_list_dump(void);
#else
	static inline void __uncompleted_cmds_list_rmv( void*c){ (void)c; }
	static inline void __uncompleted_cmds_list_add( void*c){ (void)c; }
	static inline void __uncompleted_cmds_list_send(void*c){ (void)c; }
	static inline void __uncompleted_cmds_list_comp(void*c, int v){(void)c;(void)v;}
	static inline void __uncompleted_cmds_list_dump(void  ){          }
#endif // DEBUG_UNCOMPLETED

/**************************** DEBUG_TOPO_CNTRS ********************************/
#ifdef DEBUG_TOPO_CNTRS
	#define DEBUG_TOPO_CNTRS_op_constructor(o, _cmds, _new_cmds) do {\
		o->dbg_topo.cmds = _cmds; \
		o->dbg_topo.new_cmds = _new_cmds; \
		o->dbg_topo.is_operation = true; } while(0)

	#define DEBUG_TOPO_CNTRS_op_update_trim(o, locks) do {\
		o->dbg_topo.cmds =     (locks)->cmds; \
		o->dbg_topo.new_cmds = (locks)->new_cmds; } while(0)

	#define DEBUG_TOPO_CNTRS_add_elem_to_topo(el, t) do {\
			unsigned long _flags; \
			spin_lock_irqsave(&t->dbg_tcntrs_lck, _flags); \
			list_add(&(el)->dbg_topo.list, &t->dbg_tcntrs); \
			spin_unlock_irqrestore(&t->dbg_tcntrs_lck, _flags); \
		} while (0)


	#define DEBUG_TOPO_CNTRS_del_elem_from_topo(el) do {\
		if ((el)->topo) { \
			unsigned long _flags; \
			spin_lock_irqsave(&(el)->topo->dbg_tcntrs_lck, _flags); \
			list_del(&(el)->dbg_topo.list); \
			spin_unlock_irqrestore(&(el)->topo->dbg_tcntrs_lck, _flags); \
		} } while (0)

	#define DEBUG_TOPO_CNTRS_move_elem_on_topo(el, new_el) do {\
		if ((el)->topo) { \
			unsigned long _flags; \
			spin_lock_irqsave(&(el)->topo->dbg_tcntrs_lck, _flags); \
			list_del(&(el)->dbg_topo.list); \
			list_add(&(new_el)->dbg_topo.list, &(el)->topo->dbg_tcntrs); \
			spin_unlock_irqrestore(&(el)->topo->dbg_tcntrs_lck, _flags); \
		} } while (0)
	#define DEBUG_TOPO_CNTRS_was_never_added_to_topo(el) \
		(((el)->dbg_topo.list.next == (el)->dbg_topo.list.prev)&&((el)->dbg_topo.list.next == NULL))

	void __debug_topo_print_uncompleted_op(const struct nvmeibc_cinst_params_blk *p, bool analyze_current_topo);
#else
	#define DEBUG_TOPO_CNTRS_op_constructor(...)
	#define DEBUG_TOPO_CNTRS_op_update_trim(...)
	#define DEBUG_TOPO_CNTRS_add_elem_to_topo(...)
	#define DEBUG_TOPO_CNTRS_del_elem_from_topo(...)
	#define DEBUG_TOPO_CNTRS_move_elem_on_topo(...)
	#define DEBUG_TOPO_CNTRS_was_never_added_to_topo(el)  true

	#define  __debug_topo_print_uncompleted_op(...)
#endif

#define __ndump_operation(header, o) _NF(dump_operation_ ## header, "dump_operation=@OPERATION: " #header, o)

/************************** DEBUG_LOCKS_CORRUPTION ****************************/
/* If locks are corrupted, data corruption will surrely occur. Must prevent */
void __invoke_crash_on_lock_corruption(struct nvmeibc_cmd_lock *locksets,
									   int lock_i, const char* cmd, int action);

/***************************** DEBUG_TRANSFERS ********************************/
#if defined(DEBUG_TRANSFERS) && defined(DEBUG_TRANSFERS_DETECT_DBL_CB)
	static inline void DEBUG_TRANSFERS_init_cb_counters(int ncmds,
					struct nvmeibc_block_command *cmds)
	{
		int _i;
		for (_i = 0; _i < ncmds; _i++)
			atomic_set(&cmds[_i].iocmd->n_cb, 1);
	}

	static inline void DEBUG_TRANSFERS_init_cb_counter(
					struct nvmeibc_block_command *cmd)
	{
		atomic_set(&cmd->iocmd->n_cb, 1);
	}

	static inline void DEBUG_TRANSFERS_detect_double_callback(
			const struct nvmeibc_d_iocmd_comp *comp)
	{
		int _n_cbs;
		if (((_n_cbs = atomic_dec_return(&comp->cmd->iocmd->n_cb)) != 0)) {
			WARN(true, "nvmeibc bug, multiple callbacks=%d\n", _n_cbs);
		}
	}

	static inline void DEBUG_TRANSFERS_is_init_cb_counter(
			const struct nvmeibc_d_iocmd_comp *comp)
	{
		int _n_cbs;
		if ((_n_cbs = atomic_read(&comp->cmd->iocmd->n_cb)) != 1) {
			WARN(true, "nvmeibc bug, uninitialized n_cbs=%d\n", _n_cbs);
		}
	}

#else
	#define DEBUG_TRANSFERS_init_cb_counters(ncmds, cmds)
	#define DEBUG_TRANSFERS_init_cb_counter(cmd)
	#define DEBUG_TRANSFERS_detect_double_callback(comp)
	#define DEBUG_TRANSFERS_is_init_cb_counter(comp)
#endif

#endif  // H beginning

