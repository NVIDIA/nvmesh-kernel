# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

awk '
function print_link() {
    if (source) {
        # Ensure the "dist/pytools/" directory exists
        system("mkdir -p dist/pytools");
        # source is being used for pyinstaller, introduced a separate field `linked_exec` for the executable that needs to be linked.
        if (linked_exec) {
            cmd = "ln -s " (linkpath ? linkpath : (linkdir "/" linked_exec)) " dist/pytools/" tool;
        } else {
            cmd = "ln -s " (linkpath ? linkpath : (linkdir "/" source)) " dist/pytools/" tool;
        }
        print cmd;
        system(cmd);  # Execute the command to create the symbolic link
    }
    tool=""; source=""; linkdir=""; linkpath=""; linked_exec="";
}
/^ *[^ ]+: *$/ { print_link(); tool = $1; sub(":", "", tool); }
/^ *source: */ { source = $2; }
/^ *linkdir: */ { linkdir = $2; }
/^ *linkpath: */ { linkpath = $2; }
/^ *linked_exec: */ { linked_exec = $2; }
END             { print_link(); }
' tools.yaml
