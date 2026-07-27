#!/usr/bin/env bash

cd ../../

# using the -f flag allows us to include files from a directory out of the 'context'
# we need it because we want to include all files from ../../

GIT_DESCRIBE=$(git describe | cut -c 2-)
BRANCH_NAME=$(git symbolic-ref --short --quiet HEAD) || BRANCH_NAME=$(git rev-parse --abbrev-ref HEAD)
COMMIT_ID=$(git log -n1 --format=%h)

# NOTE: building while the snx VPN is running could cause network problems for the container
docker build -f scaleSimulators/node/Dockerfile --tag excelero/scale-sim:latest --build-arg GIT_DESCRIBE=$GIT_DESCRIBE --build-arg BRANCH_NAME=$BRANCH_NAME --build-arg COMMIT_ID=$COMMIT_ID .

if [ $? -ne 0 ]; then
    echo "Docker image build failed"
    exit 1
fi
