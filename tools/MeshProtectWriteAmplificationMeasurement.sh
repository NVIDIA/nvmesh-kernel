#!/bin/bash

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

#
# This script is measuring write amplification on a MeshProtect volume.
# It will provides the client-side OS statistics about the same volume 
# and will compare it to each drive included in the transaction on the 
# target side.
# Additionnaly, this tool will repeat on all the following block size 
# the single IO Write operation:
# 4k 8k 16k 32k 64k 128k 256k 512k 1M 2M 4M
#
# The script require ssh access without password to all the NVMesh nodes.
# 
# Before running it, please pay attention that the relevant volume 
# is attached with IO enabled to the relevant client and that the 
# volume is in use ONLY by the script itself !!!
#
# The user have to set the "client" & "volume" parameter - line 19-20 - 
# before launching the tool
#


client=n1049
volume=eliott


getDisks (){
	disks=()
	for i in `ssh ${client} "cat /proc/nvmeibc/volumes/${volume}/status | grep Online" | awk {'print$4'} | cut -d "." -f 1` ; do 
		disks+=($i)
	done
}

getTargets (){
	targets=()
	for i in `ssh ${client} "cat /proc/nvmeibc/volumes/${volume}/status | grep Online" | awk {'print$7'}` ; do 
		targets+=($i)
	done
}

getClientReadStats (){
	clientReadStats=`ssh ${client} "cat /proc/nvmeibc/volumes/${volume}/iostats | grep num_ops" | awk {'print $3'} `
	echo ${clientReadStats}
}

 getClientWriteStats (){
 	clientWriteStats=`ssh ${client} "cat /proc/nvmeibc/volumes/${volume}/iostats | grep num_ops" | awk {'print $4'} `
	echo $clientWriteStats
}

getTargetsStats (){
	readStats=()
	writeStats=()
	count=0

	for i in ${disks[@]} ; do
		disk=${disks[$count]}
		target=${targets[$count]}

		diskSeqId=`ssh ${target} "cat /proc/nvmeibs/disks.csv | grep ${disk}" | cut -d "," -f 5`
		readStatHex=`ssh ${target} "cat /proc/nvmeibs/smart${diskSeqId}" | grep "Host Read Commands" | cut -d "=" -f 2 | cut -d "x" -f 2`
		writeStatHex=`ssh ${target} "cat /proc/nvmeibs/smart${diskSeqId}" | grep "Host Write Commands" | cut -d "=" -f 2 | cut -d "x" -f 2`

		readStats+=($((0x${readStatHex})))
		writeStats+=($((0x${writeStatHex})))
		((count++))	
	done	
}

sendIO (){
	blockSize=$1
	ssh ${client} "sudo dd if=/dev/urandom of=/dev/nvmesh/${volume} bs=${blockSize} count=1" > /dev/null 2>&1
}

collectVolumeInformation (){
	getDisks
	getTargets
}

collectAllStatsBefore (){
	clientReadsBefore=$(getClientReadStats)
	clientWritesBefore=$(getClientWriteStats)
	getTargetsStats
	targetReadsBefore=(${readStats[*]})
	targetWritesBefore=(${writeStats[*]})
}

collectAllStatsAfter (){
        clientReadsAfter=$(getClientReadStats)
        clientWritesAfter=$(getClientWriteStats)
        getTargetsStats
        targetReadsAfter=(${readStats[*]})
        targetWritesAfter=(${writeStats[*]})
}

statsDiff (){
	clientLine="$(( ${clientReadsAfter} - ${clientReadsBefore} ))/$(( ${clientWritesAfter} - ${clientWritesBefore} ))"
	
	targetsLine=""
	disksLine=""
	statsLine=""
	totalTargetsReads=0
	totalTargetsWrites=0
	count=0
	for i in ${disks[@]} ; do
		targetsLine="${targetsLine} ${targets[$count]}"
		disksLine="${disksLine} ${disks[$count]}" 
		statsLine="${statsLine} $(( ${targetReadsAfter[$count]} - ${targetReadsBefore[$count]} ))/$(( ${targetWritesAfter[$count]} - ${targetWritesBefore[$count]} ))"
		totalTargetsReads=$((${totalTargetsReads} + $(( ${targetReadsAfter[$count]} - ${targetReadsBefore[$count]} ))))
		totalTargetsWrites=$((${totalTargetsWrites} + $(( ${targetWritesAfter[$count]} - ${targetWritesBefore[$count]} ))))
		((count++))
	done
	totalTargetsStats="${totalTargetsReads}/${totalTargetsWrites}"


	echo """
- - - - - - - - - - -
BlockSize ${bs}
Volume ${volume} (R/W)
Client Stats ${clientLine}
- - - - - - - - - - -
Targets ${targetsLine}
Disks ${disksLine}
Stats ${statsLine}
Total Stats ${totalTargetsStats}
- - - - - - - - - - -
""" | column -t
}


main (){
	collectVolumeInformation
	for bs in 4k 8k 16k 32k 64k 128k 256k 512k 1M 2M 4M ; do 
		collectAllStatsBefore
		sendIO ${bs}
		collectAllStatsAfter
		statsDiff
	done
}



main

