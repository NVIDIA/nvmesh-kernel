#!/bin/bash

########################################################
# Builds and runs block unit tests inside a container
# (Podman/Docker) for development in non-linux environments.
#
# Uses a volume mount so source edits on the host are
# instantly visible inside the container.
########################################################

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

# Defaults
DISTRO="aks"
CT="podman"
BUILD_DIR="/excelero"
PLATFORM="linux/amd64"
CONTAINER_NAME=""
IMAGE_NAME=""

DEFAULT_TEST_ARGS="-async -conf ./ci.cfg -dbg 1 -tracedbg 3 -good-path-dbg 3 -nRep 2"

print_help() {
cat << 'EOF'
Builds and runs block unit tests inside a container (Podman/Docker).

usage: build_block_testing_container.sh [options] <command> [command-args...]

Options (before command):
  -d, --distro <name>   Linux distro, default: aks (matches docker/Dockerfile_<name>)
      --platform <p>    Container platform, default: linux/amd64 (use linux/arm64 for native)
      --docker          Use Docker instead of Podman
      --build-dir <dir> Mount point inside container, default: /excelero
  -h, --help            Print this help

Commands:
  build [make-args...]    Build unit test binary inside container
  rebuild [make-args...]  Clean + build inside container
  run [test-args...]      Run blk_unitest inside container
                          Default args: -async -conf ./ci.cfg -dbg 1 -tracedbg 3 -good-path-dbg 3 -nRep 2
  shell                   Open interactive shell inside the container
  stop                    Stop and remove the container
  status                  Show whether the container is running

Examples:
  ./build_block_testing_container.sh build --output
  ./build_block_testing_container.sh run -async -nRep 5
  ./build_block_testing_container.sh shell
  ./build_block_testing_container.sh -d rhel8 build
EOF
}

# --- Argument parsing: options first, then command ---

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help)
            print_help
            exit 0
            ;;
        -d|--distro)
            DISTRO="$2"
            shift 2
            ;;
        --docker)
            CT="docker"
            shift
            ;;
        --platform)
            PLATFORM="$2"
            shift 2
            ;;
        --build-dir)
            BUILD_DIR="$2"
            shift 2
            ;;
        -*)
            # Unknown option — might be a command arg (e.g. run --something),
            # so stop parsing options and treat the rest as command + args
            break
            ;;
        *)
            # First non-option argument is the command
            break
            ;;
    esac
done

COMMAND="${1:-}"
shift 2>/dev/null || true

CONTAINER_NAME="nvmesh-block-test-${DISTRO}"
IMAGE_NAME="nvmesh-build-${DISTRO}"

# --- Core functions ---

ensure_image() {
    if $CT image exists "$IMAGE_NAME" 2>/dev/null; then
        return 0
    fi

    local dockerfile="$PROJECT_ROOT/docker/Dockerfile_${DISTRO}"
    if [ ! -f "$dockerfile" ]; then
        echo "Error: Dockerfile not found: $dockerfile"
        echo "Available distros:"
        ls "$PROJECT_ROOT/docker/Dockerfile_"* 2>/dev/null | sed 's/.*Dockerfile_/  /'
        exit 1
    fi

    echo "Building $IMAGE_NAME image (platform: $PLATFORM) from docker/"
    $CT build --platform "$PLATFORM" -t "$IMAGE_NAME" -f "$dockerfile" "$PROJECT_ROOT/docker/"
}

ensure_container() {
    # Check if container is already running
    if $CT container inspect "$CONTAINER_NAME" --format '{{.State.Running}}' 2>/dev/null | grep -q 'true'; then
        return 0
    fi

    # Check if container exists but is stopped
    if $CT container inspect "$CONTAINER_NAME" 2>/dev/null >/dev/null; then
        echo "Starting existing container $CONTAINER_NAME"
        $CT start "$CONTAINER_NAME"
        return 0
    fi

    # Container doesn't exist — create it
    ensure_image

    echo "Creating container $CONTAINER_NAME (platform: $PLATFORM) with volume mount $PROJECT_ROOT -> $BUILD_DIR"
    $CT run -dit --platform "$PLATFORM" --name "$CONTAINER_NAME" \
        -v "$PROJECT_ROOT:$BUILD_DIR" \
        "$IMAGE_NAME" bash

    # Install additional packages needed for building unit tests
    echo "Installing additional build dependencies (python3, uuid-dev)..."
    $CT exec "$CONTAINER_NAME" bash -c "apt-get update -qq && apt-get install -y -qq python3 uuid-dev >/dev/null 2>&1"

    echo "Container $CONTAINER_NAME is running"
}

container_exec() {
    $CT exec -t "$CONTAINER_NAME" bash -c "$1"
}

container_exec_interactive() {
    $CT exec -it "$CONTAINER_NAME" bash -c "$1"
}

# Extract git metadata (used for build commands)
GIT_COMMIT_ID=$(cd "$PROJECT_ROOT" && git log -n1 --format=%h 2>/dev/null) || GIT_COMMIT_ID="deadbeef"
GIT_BRANCH=$(cd "$PROJECT_ROOT" && git symbolic-ref --short --quiet HEAD 2>/dev/null) || GIT_BRANCH="unknown"
GIT_VER_TAG=$(cd "$PROJECT_ROOT" && git describe --abbrev=0 2>/dev/null) || GIT_VER_TAG="v0.0.0"

# Build flags — sanitizers disabled by default (ASan doesn't work under Rosetta)
MAKE_BUILD_FLAGS="COMMIT_ID=$GIT_COMMIT_ID VER_TAGID=$GIT_VER_TAG BRANCH_NAME=$GIT_BRANCH USE_RELEASE=1 -j 10 --output-sync=recurse"
UNITEST_DIR="$BUILD_DIR/clnt/block/unitest"

do_build() {
    local extra_args="$*"
    echo "Building block unit tests (platform: $PLATFORM)..."
    echo "  COMMIT_ID=$GIT_COMMIT_ID  BRANCH=$GIT_BRANCH  TAG=$GIT_VER_TAG"
    container_exec "cd $UNITEST_DIR && make write_compilation_cmd $MAKE_BUILD_FLAGS $extra_args"
    container_exec "cd $UNITEST_DIR && make all $MAKE_BUILD_FLAGS $extra_args"
    echo "-->Done"
}

# --- Command dispatch ---

case "$COMMAND" in
    build)
        ensure_container
        do_build "$@"
        ;;
    rebuild)
        ensure_container
        echo "Cleaning..."
        container_exec "cd $UNITEST_DIR && make clean"
        do_build "$@"
        ;;
    run)
        ensure_container
        TEST_ARGS="${*:-$DEFAULT_TEST_ARGS}"
        echo "Running: blk_unitest $TEST_ARGS"
        set +e
        container_exec "cd $BUILD_DIR/clnt/block/unitest && ./blk_unitest $TEST_ARGS"
        rc=$?
        set -e
        exit $rc
        ;;
    shell)
        ensure_container
        echo "Opening shell in $CONTAINER_NAME (${BUILD_DIR}/clnt/block/unitest)..."
        container_exec_interactive "cd $BUILD_DIR/clnt/block/unitest && exec bash"
        ;;
    stop)
        if $CT container inspect "$CONTAINER_NAME" 2>/dev/null >/dev/null; then
            echo "Stopping container $CONTAINER_NAME"
            $CT container stop -t 0 "$CONTAINER_NAME" 2>/dev/null || true
            echo "Removing container $CONTAINER_NAME"
            $CT container rm "$CONTAINER_NAME" 2>/dev/null || true
            echo "Done"
        else
            echo "Container $CONTAINER_NAME does not exist"
        fi
        ;;
    status)
        if $CT container inspect "$CONTAINER_NAME" --format '{{.State.Running}}' 2>/dev/null | grep -q 'true'; then
            echo "Container $CONTAINER_NAME is running"
            echo "  Image:    $IMAGE_NAME"
            echo "  Platform: $PLATFORM"
            echo "  Mount:    $PROJECT_ROOT -> $BUILD_DIR"
        elif $CT container inspect "$CONTAINER_NAME" 2>/dev/null >/dev/null; then
            echo "Container $CONTAINER_NAME exists but is stopped"
        else
            echo "Container $CONTAINER_NAME does not exist"
            if $CT image exists "$IMAGE_NAME" 2>/dev/null; then
                echo "  Image $IMAGE_NAME is available"
            else
                echo "  Image $IMAGE_NAME not yet built"
            fi
        fi
        ;;
    "")
        echo "Error: No command specified"
        echo ""
        print_help
        exit 1
        ;;
    *)
        echo "Error: Unknown command '$COMMAND'"
        echo ""
        print_help
        exit 1
        ;;
esac
