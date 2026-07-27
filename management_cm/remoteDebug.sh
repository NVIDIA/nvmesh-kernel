#!/bin/bash

USAGE_MSG="Usage: ./remoteDebug.sh -s <remote server hostname or IP> -c<component to debug> [-h] \n
Usage example: ./remoteDebug.sh -s nvme1111 -c mcs \n\n
-h\t    help\n
-c\t    component to debug. Options: mcs or agent\n
-s\t    remote server\n"

while getopts "s:c:h" opt; do
  case ${opt} in
    s )
            server=$OPTARG
      ;;
    h )
	        echo -e ${USAGE_MSG}
            exit 0
      ;;
	c )
            component=("$OPTARG")
      ;;
    \? )
      echo "Invalid option: $OPTARG" 1>&2
      ;;
    : )
      echo "Invalid option: $OPTARG requires an argument" 1>&2
      ;;
  esac
done
shift $((OPTIND -1))

echo "Starting Remote Debug for $component on $server"

if [[ $component == "mcs" ]]; then
	dest_port=5678
	process_name='managementCM.py'
elif [[ $component == "agent" ]]; then
	dest_port=5679
	process_name='managementAgent.py'
else
	echo "Unknown component to debug: $component. Options: mcs or agent"
	exit 1
fi

PID=$(ssh $server -o "StrictHostKeyChecking no" "ps -ef | grep -v grep | grep ${process_name}" | awk '{print $2}')
if [ ! $PID ] ; then
   echo "${process_name} is not running on the remote server"
   exit 1
else
   echo "Remote process PID = $PID"
fi

echo "Signaling $component to start listening for the debugger on port $dest_port"
ssh $server -o "StrictHostKeyChecking no" "sudo kill -USR1 $PID"

# create SSH Tunnel
echo "Creating SSH tunnel to $server"
ssh -4 -L 5678:localhost:$dest_port $server
