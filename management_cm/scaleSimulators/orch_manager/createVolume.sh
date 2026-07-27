#!/bin/bash

startIdx=$1
endIdx=$(($startIdx + 19))
hostname=`hostname | tr "." "-"`
hostname=${hostname%-*}
hostname=${hostname%-*}

capacity=$((1024 ** 3))
generate_jbod_data()
{
cat <<EOF
[{
"RAIDLevel": "Concatenated",
"capacity": ${capacity},
"name": "${volName}"
}] 
EOF
}
generate_r1_data()
{
cat <<EOF
[{
"RAIDLevel": "Mirrored RAID-1",
"capacity": ${capacity},
"name": "${volName}",
"numberOfMirrors": 1
}] 
EOF
}
mgmt=`cat /etc/nvmesh/nvmesh.conf | grep MANAGEMENT_SERVERS | cut -d'"' -f 2 | cut -d":" -f 1`
mgmt+=":4000"
echo $mgmt

for i in $(seq $startIdx $endIdx);do
	curl -i -H "Accept:application/json" -H "Content-Type:application/x-www-form-urlencoded" -X POST "https://${mgmt}/login" -k --data "username=admin" --data "password=admin" --cookie-jar ./cookieFile$i
	volName=$hostname
	volName+="-$i" 
	touch res_$volName
	touch data_$volName
	if [[ ${2} == '' ]];then data="$(generate_jbod_data)"; else data="$(generate_r1_data)"; fi
	echo $data > data_$volName
	echo $mgmt >> data_$volName
	curl -i -H 'Accept:application/json' -H 'Content-Type:application/json' -X POST "https://${mgmt}/volumes/save" -k --data "${data}" --cookie ./cookieFile$i >> res_$volName &
done
