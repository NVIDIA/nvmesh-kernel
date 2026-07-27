#!/usr/bin/env bash
find_core () {
    CORE_PATH="$(ls core.* 2>/dev/null | tail -1)"
    if [ -z "$CORE_PATH" ]; then
	#echo "No core file, quitting"
	exit 1
    fi
    echo "--------------------------------------------> $CORE_PATH"
}
# ----------
BUNNY_TASK="g_sys->clients[0].OS.kernel.T.tasks[69]"
GDB_FIND_MAIN_THREAD="x $BUNNY_TASK.os_id"
find_core
(echo $GDB_FIND_MAIN_THREAD; cat) | gdb ./blk_unitest $CORE_PATH
# Todo in gdb: 'thread find <pid of bunny>'
