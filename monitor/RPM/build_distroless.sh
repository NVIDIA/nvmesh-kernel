#!/bin/bash

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0

# Build and verify a distroless container for nvmesh-exporter
# Usage: ./build_distroless.sh <deb_file>

set -e

OBSERVABILITY_REPO="${OBSERVABILITY_REPO:-ssh://git@gitlab-master.nvidia.com:12051/nsvsrecs/nvmesh-k8s-observability/containers/nvmesh-exporter.git}"
OBSERVABILITY_BRANCH="${OBSERVABILITY_BRANCH:-master}"

poll_with_timeout() {
    local check_cmd="$1"
    local interval="${2:-1}"
    local max_attempts="${3:-60}"
    
    local attempt=0
    while [ $attempt -lt $max_attempts ]; do
        if eval "$check_cmd" > /dev/null 2>&1; then
            return 0
        fi
        attempt=$((attempt + 1))
        sleep "$interval"
    done
    return 1
}

buildDistrolessContainer() {
    local deb_file="$1"
    
    if [ ! -f "$deb_file" ]; then
        echo "ERROR: deb file not found: $deb_file"
        return 1
    fi
    
    distroless_build_context=$(mktemp -dt nvmesh-exporter-build-XXXXXX)
    echo "Created build context directory: $distroless_build_context"
    
    echo "Cloning nvmesh-exporter observability repo (branch: $OBSERVABILITY_BRANCH)..."
    git clone --depth 1 --branch "$OBSERVABILITY_BRANCH" "$OBSERVABILITY_REPO" "$distroless_build_context" 2>&1
    if [ $? -ne 0 ]; then
        echo "ERROR: Failed to clone $OBSERVABILITY_REPO"
        return 1
    fi
    
    # Check if Dockerfile supports NVMESH_MONITOR_VERSION build arg
    if ! grep -q "NVMESH_MONITOR_VERSION" "$distroless_build_context/Dockerfile"; then
        echo "WARNING: Dockerfile does not contain NVMESH_MONITOR_VERSION, skipping distroless build"
        rm -rf "$distroless_build_context"
        return 2
    fi

    sed -i 's|nvcr.io/nvidian/|nvcr.io/nvidia/|g' "$distroless_build_context/Dockerfile"
    sed -i -E 's|distroless/python([0-9.]+):v|distroless/python:\1-v|g' "$distroless_build_context/Dockerfile"
    
    local arch=$(uname -m)
    [ "$arch" == "aarch64" ] && DOCKER_ARCH="arm64" || DOCKER_ARCH="amd64"
    
    local deb_basename=$(basename "$deb_file")
    local nvmesh_monitor_version=$(echo "$deb_basename" | sed -E 's/nvmesh-monitor_([^_]+)_.*/\1/')
    local expected_deb_name="nvmesh-monitor_${nvmesh_monitor_version}_${DOCKER_ARCH}.deb"
    
    mkdir -p "$distroless_build_context/files"
    cp "$deb_file" "$distroless_build_context/files/$expected_deb_name"
    echo "Copied deb file as: files/$expected_deb_name"
    
    distroless_image_name="nvmesh-exporter-test:$$"
    
    echo "Building distroless container image: $distroless_image_name"
    docker build --no-cache --platform "linux/$DOCKER_ARCH" --build-arg NVMESH_MONITOR_VERSION="$nvmesh_monitor_version" -t "$distroless_image_name" "$distroless_build_context"
    
    if [ $? -ne 0 ]; then
        echo "ERROR: Docker build failed"
        rm -rf "$distroless_build_context"
        return 1
    fi
    
    echo "Docker image built successfully: $distroless_image_name"
    return 0
}

verifyDistrolessContainer() {
    echo "Starting container for verification..."
    
    distroless_container_name="nvmesh-exporter-verify-$$"
    
    distroless_log_dir=$(mktemp -dt nvmesh-exporter-logs-XXXXXX)
    chmod 777 "$distroless_log_dir"
    docker run -d --name "$distroless_container_name" -p 9300:9300 \
        -v "$distroless_log_dir:/var/log/nvmesh/monitor" \
        "$distroless_image_name"
    if [ $? -ne 0 ]; then
        echo "ERROR: Failed to start container"
        return 1
    fi
    echo "Container logs available at: $distroless_log_dir"
    
    if ! poll_with_timeout "docker ps --format '{{.Names}}' | grep -q '^${distroless_container_name}\$'"; then
        echo "ERROR: Container is not running"
        docker logs "$distroless_container_name" 2>&1
        return 1
    fi
    echo "Container process is running"
    
    if ! poll_with_timeout "curl -sf http://localhost:9300/metrics" 5; then
        echo "ERROR: Metrics endpoint is not responding"
        docker logs "$distroless_container_name" 2>&1
        return 1
    fi
    echo "SUCCESS: Metrics endpoint is responding"
    return 0
}

cleanupDistrolessContainer() {
    echo "Cleaning up distroless container and image..."
    
    if [ -n "$distroless_container_name" ]; then
        docker stop "$distroless_container_name" 2>/dev/null
        docker rm "$distroless_container_name" 2>/dev/null
        echo "Container removed: $distroless_container_name"
    fi
    
    if [ -n "$distroless_image_name" ]; then
        docker rmi "$distroless_image_name" 2>/dev/null
        echo "Image removed: $distroless_image_name"
    fi
    
    if [ -n "$distroless_build_context" ] && [ -d "$distroless_build_context" ]; then
        rm -rf "$distroless_build_context"
        echo "Build context removed: $distroless_build_context"
    fi
}

# Main
if [ $# -lt 1 ]; then
    echo "Usage: $0 <deb_file>"
    exit 1
fi

DEB_FILE="$1"

trap cleanupDistrolessContainer EXIT INT TERM

echo "Building distroless container..."
buildDistrolessContainer "$DEB_FILE"
build_result=$?
if [ $build_result -eq 2 ]; then
    # Dockerfile doesn't support this build, skip gracefully
    exit 0
elif [ $build_result -ne 0 ]; then
    echo "ERROR: Failed to build distroless container"
    exit 1
fi

verifyDistrolessContainer
if [ $? -ne 0 ]; then
    echo "ERROR: Distroless container verification failed"
    exit 1
fi

echo "Distroless container build and verification completed successfully!"
