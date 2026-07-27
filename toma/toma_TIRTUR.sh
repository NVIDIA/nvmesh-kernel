#!/bin/bash
usage_str="Usage: $0 [-rt <runtime>] [-dt <downtime>] [-release/-delease (default=debug)] [-s log_size]"
# Parse the arguments
runtime_base=10
downtime_base=5
debug_type="debug"
log_size="4G"
save_tirtur_logs=0
log_file='/var/log/nvmesh/toma_0.log'

while [ "$#" -gt 0 ]; do
	cmd="$1"
	val="$2"
	case "$cmd" in
		-h|-\?) echo $usage_str; exit 1;;
		-rt) runtime_base="$val"; shift;;
		-dt) downtime_base="$val"; shift;;
		-release) debug_type="release";;
		-delease) debug_type="delease";;
		-save_tirtur_log) save_tirtur_logs=1;;
		-s) log_size="$val"; shift;;
		*) echo "unknown option: '$cmd'" >&2; echo $usage_str; exit 1;;
	esac
        case "$cmd" in
		-rt|-dt)
			if [[ !("$val" =~ ^[0-9]+$) ]] ; then
				echo "OOPS $cmd argument $val" >&2
				exit 1
			fi;;
        esac
	shift
done

toma_cmd="./bin/$debug_type/nvmeibt_toma -s $log_size"
sudo ../bin/load_server.sh
echo -e "\n------ Kill -9 nvmeibt_toma ---------\n"
sudo pkill -9 nvmeibt_toma
ulimit -c unlimited

echo $toma_cmd, runtime_base="$runtime_base", downtime_base="$downtime_base"

declare -i counter
counter=0
while true; do
        counter+=1
        runtime=$(($RANDOM % $runtime_base))
        downtime=$(($RANDOM % $downtime_base))
        echo "------------------------------- round $counter, runtime=$runtime, downtime=$downtime save_tirtur_logs=$save_tirtur_logs--------------------------------------------"
        pgrep nvmeibt_toma
        is_running=$?
        if [[ "$is_running" == 0 ]] ; then
            echo -e "\n------ Kill -9 nvmeibt_toma ---------\n"
            sudo pkill -9 nvmeibt_toma
            sleep 3
        else
            sudo $toma_cmd &
            sleep $runtime
        fi
	sudo pkill -9 nvmeibt_toma
        for (( wait_toma_dead_counter = 0 ; wait_toma_dead_counter < 3 ; wait_toma_dead_counter += 1 )) ; do
            echo -e "\n------ Kill -9 nvmeibt_toma ---------\n"
            sudo pkill -9 nvmeibt_toma
            if [ "$?" == "1" ] ; then
                break
            else
                sleep 1
            fi
        done
        grep -lq '\\*TOMAerr\\*\|Fatal Error detected' "$log_file"
        grep_rc=$?
        if [[ "$grep_rc" == 0 ]] ; then
        	echo OOPS
        	#date_folder="/var/log/nvmesh/prev_logs/`date +_%d%b%Y_%H%M%S`"
        	#mkdir -p "$date_folder"
        	#cp /var/log/nvmesh/toma_0.* "$date_folder"
        	exit 1
        fi
        if [[ "$save_tirtur_logs" == 1 ]] ; then
            tirtur_logs_folder="/var/log/nvmesh/tirtur_logs"
            sudo mkdir -p $tirtur_logs_folder
            date_file_name="prev_logs_`date +_%d%b%Y_%H%M%S`"
            sudo tar -zvcf "$tirtur_logs_folder/$date_file_name.tar.gz" "/var/log/nvmesh/prev_logs/"
        fi
	sleep $downtime
done

