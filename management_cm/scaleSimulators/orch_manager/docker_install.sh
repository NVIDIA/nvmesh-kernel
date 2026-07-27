#!/usr/bin/env bash

## Make sure only root can run the script
#if [[ ${EUID} -ne 0 ]]; then
#   echo "This script must be run as root" 1>&2
#   exit 1
#fi

if [ -n "$(which apt-get)" ]; then
	# Debian
	sudo dpkg -l docker || sudo dpkg -l docker.io || sudo dpkg -l podman-docker || sudo dpkg -l docker-ce
	if [ $? -eq 0 ]; then
		echo "Docker already installed"
	else
		echo "Installing Docker"
		sudo apt-get install docker -y || sudo apt-get install docker.io -y
	fi
elif [ -n "$(which yum)" ]; then
   	sudo rpm -q docker || sudo rpm -q podman-docker || sudo rpm -q docker-ce
	if [ $? -eq 0 ]; then
		echo "Docker already installed"
	else
		echo "Installing Docker"
		sudo yum install docker -y
	fi
fi

if [ ! $(getent group docker) ]; then
    echo "Adding Docker group"
    sudo groupadd docker
fi

echo "Adding all users from excelero group to docker group"
# first add the current user to docker group
sudo usermod -a -G docker $USER

which lid &>/dev/null

if [ $? -eq 0 ]; then
    # iterate on all users from excelero and add them to docker group
    from_group=excelero
    to_group=docker
    for u in $(lid -g -n $from_group); do
        sudo usermod -a -G $to_group $u;
    done
fi

echo "Starting docker service"
sudo service docker start

echo "Adding docker service to startup"
sudo systemctl enable docker

