#ifndef NVMEIBC_DP_OPERATION_LOCKS_TRANSFER_H
#define NVMEIBC_DP_OPERATION_LOCKS_TRANSFER_H

struct nvmeibc_cmd_lock;
enum nvmeib_block_io_op;
/******************* Locks transferring between IO's **************************/
/* Description of the algorithm for transferring locks between IOs.
   We rely on the fact that 2 lowest bits in each pointer are not used to encode
   Special values in lock->asker pointer:
   1. 0 / Valid pointer: are initialization states. No transfer request yet or
    pointer to the IO which made the request for my lock.
   2. IO_LT_TRANSFERRED - I completed my attempt to transfer/resubmit locks to
    the next io. Will not retry the attempt even if failed. My IO is still
    written as the last one on the topology (per CPU).
   3. IO_LT_DONE = IO_LT_TRANSFERRED + my IO is not written as last one in the
	topology per CPU (either was deleted by me or overwritten by a newer IO).

    Note: status !=IO_LT_TRANSFERRED, and !=IO_LT_DONE precisely means that we
	 haven't handled the transfer request yet.
    Note: The statuses above describe the state of giving locks to the next IO.
     The state of receiving locks from previous IO is managed by previous IO.
     I can be in state IO_LT_DONE (gave my locks), but still haven't received
     locks from the previous IO. */

/* If the first lock set is the same as the last lockset of
 * the previous command, link it to the completion of that lockset instead
 * of issuing it here. In any case, replace it with our first lock. */
int  __IO_LT_try_request_transfer(struct nvmeibc_cmd_lock *locksets);

/* When IO is wants to release the lock, try to give it to another IO instead
   (assuming another IO requested the lock using method above)  */
void __IO_LT_try_transfer_give(   struct nvmeibc_cmd_lock *locksets, int lsi);

/* When IO done with a lock, call this function. It handles 2 cases:
   1. IO requested a lock but does changed it's mind and does not need it
      anymore (example, IO was retried, timed-out, etc).
   2. IO has to give its locks but could not acquire them so it cannot transfer
 */
void __IO_LT_complete_locks(      struct nvmeibc_cmd_lock *locksets);

/* If IO does not even want to participate in locks transfer, call this method*/
void __IO_LT_refuse_transfer(struct nvmeibc_cmd_lock *locksets);

/* When IO is giving locks to another IO - this happens in async fashion. This
   function completes the transfer transaction */
void __IO_LT_complete_transfer_transaction(struct nvmeibc_cmd_lock*);

/* Is IO allowed to use this functionality */
bool __IO_LT_is_allowed_to_transfer_locks(const enum nvmeib_block_io_op op);

#endif  // H beginning
