FROM ubuntu:20.04

ENV container docker
ENV	IS_SIMULATOR True

ARG DEBIAN_FRONTEND=noninteractive

# Installing build and installation dependencies (wget & gnupg2 are essentials to add repositories, build-essential for building kafka-confluent)
RUN apt-get update && apt-get install -y sudo procps wget gnupg2 build-essential python3 python3-dev vim less && rm -rf /var/lib/apt/lists/*

# Setting Confluent Kafka APT repository
RUN wget -qO - https://packages.confluent.io/deb/7.3/archive.key | sudo apt-key add -
RUN echo "deb https://packages.confluent.io/clients/deb buster main" >> /etc/apt/sources.list
RUN echo "deb [arch=amd64] https://packages.confluent.io/deb/7.3 stable main" >> /etc/apt/sources.list

# Install pip
ADD https://bootstrap.pypa.io/pip/2.7/get-pip.py /tmp/get-pip.py
RUN python3 /tmp/get-pip.py

# Install kafka confluent python module
RUN apt-get update && apt-get install -y librdkafka-dev jq
RUN pip3 install configparser debugpy
RUN pip3 install confluent-kafka==2.6.0

# Install more python modules
RUN pip3 install requests

# Deploy files into container
COPY . /cm/
COPY process_config_files.py /opt/nvmesh/bin/process_config_files
COPY scaleSimulators/node/nvmesh.conf.template /etc/nvmesh/nvmesh.conf
COPY scaleSimulators/node/nvme-cli-sim /bin/nvme
COPY scaleSimulators/node/ibdev2netdev-sim.py /bin/ibdev2netdev
COPY scaleSimulators/node/ethtool-sim.sh /bin/ethtool
COPY scaleSimulators/node/ibstat-sim.py /bin/ibstat
COPY scaleSimulators/node/nvmesh-cli-sim.py /usr/bin/nvmesh
COPY scaleSimulators/node/systemctl-sim /usr/bin/systemctl

RUN cp /cm/*.py /cm/scaleSimulators/ \
	&& cp -r /cm/clnt/ /cm/scaleSimulators/ \
	&& mkdir -p /var/run/scaleSimulator \
	&& mkdir -p /var/run/nvmesh/ \
	&& mkdir -p /version

# Setup version informations
ARG GIT_DESCRIBE
ARG BRANCH_NAME
ARG COMMIT_ID

LABEL git-describe=$GIT_DESCRIBE
LABEL branch=$BRANCH_NAME
LABEL commit=$COMMIT_ID

RUN echo $GIT_DESCRIBE > /version/git-describe \
	&& echo $GIT_DESCRIBE | cut -d '-' -f-2 > /version/version \
	&& echo $BRANCH_NAME > /version/branch-name \
	&& echo $COMMIT_ID > /version/commit-id

# Setup users settings
USER root
RUN chown -R $USER:$USER /var/opt

# Start
CMD ["python3","/cm/scaleSimulators/node/init_node.py"]
