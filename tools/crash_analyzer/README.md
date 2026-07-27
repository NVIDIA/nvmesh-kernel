Crash Analyzer
==============

Crash analyzer is a crash utility plugin that can be used to extract traces from kernel core dumps in case of a system crash.

## Quickstart for dummies

0. Install dependencies

        sudo yum install crash-devel

1. Compile the tool

        cd ~/projects/nvmesh/tools/crash_analyzer; make

2. Open coredump using crash utility as you would normally do

        crash /usr/lib/debug/lib/modules/$(uname -r)/vmlinux ./vmcore

3. Load nvmesh_common module **correct** debug symbols

        crash> mod -s nvmeib_common /opt/nvmesh/common-repo/some_version_string/common/nvmeib_common.ko
    
    If you don't know where the .ko is located search for all files named `nvmeib_common.ko` inside `/opt/nvmesh`.
    **Attention:** The version of the .ko **must** fit the one used in vmcore.

4. Load the tool you previously compiled

        crash> extend /home/johny/projects/nvmesh/tools/crash_analyzer/crash_analyzer.so

5. Dump the traces, you can close crash utility after that

        crash> tracedump interesting /my/work/dir/logs

6. Find `dictionaries.tar.gz` (under `/opt` on living env or `opt` inside log collector results) and extract to the same folder:

        tar -xvzf /opt/nvmesh/common-repo/some_version_string/dictionaries.tar.gz /my/work/dir/logs

7. And now have fun!

        cd /my/work/dir/logs; ./pager.py



## Appendix A: Tips and Tricks

### How to analyze shorterm traces with GNU Crash and crash_analyzer.so

In case you have a kernel core dump, you may want to see shorterm logs at the moment of crash.

0. Start with extracting logs from vmcore as described above. Leave the crash shell open, you will need it.

1. In GNU Crash command line, load additional debug symbols for client / server (if you didn't do it yet):

        crash> mod -s nvmeibc /opt/nvmesh/client-repo/some_version_string/client/nvmeibc.ko
        crash> mod -s nvmeibs /opt/nvmesh/target-repo/some_version_string/target/nvmeibs.ko

2. Open shortterm traces with pager. Note that pager will not open shortterm by default, you need to specify it explicitly:

        ./pager.py -l nvmeibc_trace_short

3. Typical shortterm trace will look something like this:

        Shortterm gen_cmd_completion: gen_cmd=0x12345678

4. In GNU Crash shell, visualize the content of gen_cmd using struct command:

        crash> struct nvmeibc_disk_gen_cmd 0x12345678

5. Depending on `opcode`, gen cmd will contain additional either in `ctx` or `disk_cmd.owner`, or some other field if you are looking not at `gen_cmd`, but `block_cmd` for example. Consult the source code to determine exact field name and value. Functions you would want to look at are:
   * nvmeibc_disk_gen_cmd_completion
   * nvmeibc_locks_channel_lock_cmd_completion
   * nvmeibc_block_completion

6. Use `struct` command to get the value of `ctx` or `disk_cmd` or whatever else.

7. Use `sym` command to convert function pointers to filename and line.
   
8. Repeat as needed, until all bugs are solved.

### More info:

[Here](https://people.redhat.com/anderson/crash_whitepaper/) is a great reference on using GNU Crash:

### Example:

Assume you see in you shortterm log the following line:

    Shortterm gen_cmd_completion: gen_cmd=0x12345678

You use `struct` command, and get, among other data, `opcode = 1`. This means opcode is `NVMEIB_GEN_OP_GET_UUID_JOUR` (check enum definition).

Now look at `nvmeibc_disk_gen_cmd_completion`. It appears that for this opcode, additional info is stored at `gen_cmd->disk_cmd.owner`, and it is of type `nvmeibc_disk_io_command`. Lets look at `struct` command output. You will see something like this:

    ...
    disk_cmd = {
        ...
        owner = 0x23456789

Now use:

    crash> struct nvmeibc_disk_io_command 0x23456789

This gets you more info, including completion disk objects. Repeat as needed.
