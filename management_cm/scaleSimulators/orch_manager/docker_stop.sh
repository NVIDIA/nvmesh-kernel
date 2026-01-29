#!/usr/bin/env bash

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0


if [ "$1" == "all" ]; then
    echo "Stopping all containers on $(hostname)"
    for id in `docker ps -a --format "{{.Names}}" | grep scale-`; do
        # the flag --time controls how long to wait for the main container process to stop on it's own
        docker stop $id &	
    done
    
    wait
else
    for idx in "$@"; do
        if [[ $idx == scale-* ]]; then
            # full container name given
            container_name=$idx
        else
            container_name="scale-$idx"
        fi
        echo "Stopping $container_name on $(hostname)"
        docker stop "$container_name"
    done
fi
