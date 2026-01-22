#!/usr/bin/env python3

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

import subprocess
import sys
import os

# All logging setup has been removed.

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

    # Step 3: Restart the systemd service
    run_command(
        ["systemctl", "restart", "nvmeshclient.service"],
        "Restarting nvmeshclient service"
    )

    print("--- All steps completed successfully. ---")
    print("NVMesh client service has been restarted.")

if __name__ == "__main__":
    main()
