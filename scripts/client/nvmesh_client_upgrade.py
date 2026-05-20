#!/usr/bin/env python3

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

import subprocess
import sys
import os

# All logging setup has been removed.

NVMESHCLIENT_SERVICE = "nvmeshclient.service"

def run_command(command, description):
    """
    Runs a shell command and checks for success, streaming output.
    Stops the script on failure.
    """
    print(f"--- Starting: {description} ---")
    try:
        result = subprocess.run(
            command,
            check=True,
            stdout=sys.stdout,
            stderr=sys.stderr
        )

        print(f"--- Success: {description} ---")

    except subprocess.CalledProcessError as e:
        # The command's error message was already streamed to stderr.
        # We just need to log our own context and exit.

        print(f"--- FAILED: {description} ---", file=sys.stderr)
        print(f"Return Code: {e.returncode}", file=sys.stderr)
        sys.exit(1)
    except Exception as e:
        print("--- An unexpected error occurred ---", file=sys.stderr)
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)


def verify_service_stopped_cleanly(unit):
    """
    Verify that the last stop of `unit` completed cleanly.

    `systemctl stop` exits 0 whenever the unit ends up inactive, even when
    ExecStop returned non-zero. The unit's "Result" property reflects the
    real outcome: "success" on a clean stop, otherwise "exit-code",
    "signal", "timeout", "core-dump", etc.

    Raises RuntimeError if the unit did not stop cleanly.
    """
    try:
        completed = subprocess.run(
            ["systemctl", "show", "-p", "Result", "--value", unit],
            check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True,
        )
    except subprocess.CalledProcessError as e:
        print(f"systemctl show failed for {unit} (rc={e.returncode})", file=sys.stderr)
        if e.stderr:
            print(e.stderr, file=sys.stderr)
        raise RuntimeError(
            f"failed to query Result for {unit} (rc={e.returncode})"
        ) from e

    result = completed.stdout.strip()
    print(f"{unit} Result={result}")
    if result != "success":
        raise RuntimeError(
            f"{unit} did not stop cleanly (Result={result})"
        )


def main():
    # check to ensure we are running as root ---
    if os.geteuid() != 0:
        print("This script must be run as root (or with sudo). Exiting.", file=sys.stderr)
        sys.exit(1)

    # Step 1: Check modules
    run_command(
        ["/opt/nvmesh/client-repo/services/nvmeshclient", "check_modules"],
        "Checking NVMesh modules"
    )

    # Step 2: Create the upgrade marker file
    run_command(
        ["/opt/nvmesh/client-repo/services/nvmeshclient", "create_upgrade_marker"],
        "Creating upgrade marker"
    )

    # Step 3: Restart (stop and start) the systemd service.
    # Don't use "restart": neither "restart" nor "stop" propagate ExecStop
    # failures via their exit code -- systemd considers a stop successful
    # whenever the unit ends up inactive, even if ExecStop returned non-zero.
    # We therefore inspect the unit's Result property after the stop.
    run_command(
        ["systemctl", "stop", NVMESHCLIENT_SERVICE],
        "stopping " + NVMESHCLIENT_SERVICE
    )

    try:
        verify_service_stopped_cleanly(NVMESHCLIENT_SERVICE)
    except RuntimeError as e:
        print(f"--- FAILED: {e} ---", file=sys.stderr)
        sys.exit(1)

    run_command(
        ["systemctl", "start", NVMESHCLIENT_SERVICE],
        "starting " + NVMESHCLIENT_SERVICE
    )

    print("--- All steps completed successfully. ---")
    print("NVMesh client service has been restarted.")

if __name__ == "__main__":
    main()
