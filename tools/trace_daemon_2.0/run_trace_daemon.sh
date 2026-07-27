#!/bin/bash

# Just use as a wrapper to trace_daemon executable so we would able to use linux/python cmds before running the trace_daemon itself.
tracer_pid=""

function sigterm_handler {
    echo "Sigterm Tracer Daemon ($tracer_pid)"

    kill $tracer_pid
    for i in {1..1000}
    do
        kill -0 $tracer_pid || { KILLED=1; echo "tracer is stopped"; break; }
        echo "Main pid is still running"
        sleep 0.01
    done

    if [ -z "$KILLED" ]; then
        echo "Killing tracer"
        kill -9 $tracer_pid
    fi

    if [ -n "$UMOUNT_NEEDED" ]; then
        umount $TRACE_LOGS_PATH
    fi
    exit $?
}

# Taken from https://stackoverflow.com/questions/16571739/parsing-variables-from-config-file-in-bash
shopt -s extglob
configfile="/var/log/nvmesh/trace_daemon/tracedaemon.conf" # set the actual path name of your (DOS or Unix) config file
private_configfile="/var/log/nvmesh/trace_daemon/.tracedaemon.conf"
cap_file="/opt/nvmesh/client-repo/.capabilities"
tr -d '\r' < $configfile > $private_configfile
while IFS='= ' read -r lhs rhs
do
    if [[ ! $lhs =~ ^\ *# && -n $lhs ]]; then
        rhs="${rhs%%\#*}"    # Del in line right comments
        rhs="${rhs%%*( )}"   # Del trailing spaces
        rhs="${rhs%\"*}"     # Del opening string quotes 
        rhs="${rhs#\"*}"     # Del closing string quotes 
        declare $lhs="$rhs"
    fi
done < $private_configfile

if [ -n "$NFS_TARGET" ]; then
    sed -i "/TRACE_LOGS_PATH/d" $private_configfile
    echo "TRACE_LOGS_PATH=\"$TRACE_LOGS_PATH/$(hostname)\"" >> $private_configfile
    mounts=$(cat /proc/mounts  | grep $TRACE_LOGS_PATH)
    if [ -n "$mounts" ]; then
        echo "$TRACE_LOGS_PATH already mounted"
    else
        mkdir -p $TRACE_LOGS_PATH
        mount $NFS_OPTS -v -t nfs $NFS_TARGET $TRACE_LOGS_PATH
        ret=$?
        if [ $ret -ne 0 ]; then
            echo "Mount cmd failed"
            if [ "$NFS_FALLBACK_LOCAL_STORAGE" = "Yes" ]; then
                echo "Fallback to local storage on /var/log/nvmesh/trace_daemon"
                sed -i "/TRACE_LOGS_PATH/d" $private_configfile
            else
                exit 1
            fi
        else
            UMOUNT_NEEDED="true"
        fi
    fi
    mkdir -p $TRACE_LOGS_PATH/$(hostname)
fi

if [ -n "$TRACE_LOGS_PATH" ]; then
    cp /var/log/nvmesh/trace_daemon/pager* $TRACE_LOGS_PATH/$(hostname)
    cp /var/log/nvmesh/trace_daemon/dict*json $TRACE_LOGS_PATH/$(hostname)
fi

if [ -f "$cap_file" ]; then
    is_prod=$(grep is_production "$cap_file" | cut -d: -f2 | xargs)
    export NVMESH_IS_PRODUCTION=$is_prod
fi

trap sigterm_handler SIGINT SIGTERM
SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
if [ "$DISABLE_COREDUMP" != "Yes" ]; then
    ulimit -c unlimited
fi
# Machines with many cpus will require us at least num_cpus logs per channel!
# As default is usually 1024 we may run out of fds really early.
# Set the ulimit to hard limit
ulimit -n $(ulimit -Hn)

# have to run on bg as bash script ignored signals when run other processes not in bg
$SCRIPT_DIR/trace_daemon $@ &
tracer_pid="$!"
wait
