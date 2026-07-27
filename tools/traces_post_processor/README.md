# Binary Tracer Post Processor aka Pager

## What it does

Pager is a command line utility that will allow opening binary traces and perform operations on traces while still in **binary** format.

This way, only the traces that you actually need are translated into text format.

Basic functionality is:

- Efficiently cutting input by timeframe.
- Efficiently filtering **binary** logs before converting to text saving times and removing unnecessary noise from output.

## Where is it located


* All the source codes is located under `tools/traces_post_processor`.
* On the machines, pager is installed with all the relevant dictionaries under `/var/log/nvmesh/trace_daemon` near the logs.
* There is a copy of pager with all dictionaries in `dictionaries.tar.gz` under `opt` if you ever need it.


## Basic usage

Binary logs are located under `/var/log/nvmesh/trace_daemon`. For basic usage:

    cd /var/log/nvmesh/trace_daemon; ./pager.py

To see traces of a specific component:

    ./pager.py                                              # By default show client + server longterm only
    ./pager.py --client                                     # Client
    ./pager.py --server                                     # Server
    ./pager.py --toma                                       # Toma
    ./pager.py --client --toma                              # Client and toma but not server
    ./pager.py -l nvmeibs_trace_long nvmeibc_trace_goodpath # Selecting specific channels (for advanced usage)

See `./pager.py --help` for more information.

## Watch mode

To see traces interactively as they arrive:

    ./pager.py -w

## Selecting time frame

There are multiple ways to select time frame:

    ./pager.py --tail                                        # The most recent traces
    ./pager.py -t now-30s                                    # Last 30 seconds
    ./pager.py -t now-24h                                    # Last 24 hours
    ./pager.py -t 16:00                                      # Starting today 4PM
    ./pager.py -t 1575970000000000000 1575980000000000000    # Between 2 specific timestamps
    ./pager.py -t 'Jan 1 2019 10:00' 'now-10m'               # Jan 1st 2019 10:00 till 10 minutes ago

See `./pager.py --help` for the exact list of supported formats

## Filters

Pager can filter traces in their binary form, hence it is much faster than grepping text logs.
Especially true when logs are huge and messed up.

Summary of available filters:

    # Show traces from a specific function
    ./pager.py -f func = execute_bio

    # Show only specific trace
    ./pager.py -f trace = __T_trace_nvmeibc_main_init

    # Show traces with format matching a pattern (posix wildcards)
    # Note that it does not use string operations as formats are known in compile time
    ./pager.py -f fmt like *io_perms=*

    # Filter by CPU
    ./pager.py cpu = 8

    # Token matching
    ./pager.py -f @VLBA = 0x1234
    ./pager.py -f @DISK_NAME = \"Disk 123\" # Quotes are needed if string contains whitespaces
    ./pager.py -f @DISK_NAME like *Disk_*
    ./pager.py -f has @IO_PERMS

    # Composite tokens matching
    ./pager.py -f @GOODPATH_IO_DUMP {@VOL_ID = 10, @START_LBA = 20}

    # Combined filters
    ./pager.py -f (func = execute_bio or has @IO_PERMS) and not cpu = 8

Notes:
    - Keywords are case insensitive (i.e. trace = TRACE)
    - Alternative operators can be used (and = &&, or = ||, not = !, like = =~)

## Mutiple hosts

Suppose you have the following hierarchy from logs collector:

    root/
    |--nvme1040/opt/nvmesh/trace_daemon/some_traces
    |--nvme1041/opt/nvmesh/trace_daemon/some_more_traces
    |--nvme1042/opt/nvmesh/trace_daemon/even_more_traces

You can run (from any directry, not important exactly from where)

    ./pager.py /path/to/nvme1040/opt/nvmesh/trace_daemon/ /path/to/nvme1041/opt/nvmesh/trace_daemon/ /path/to/nvme1042/opt/nvmesh/trace_daemon/

This will present all the traces from all hosts on one screen.

## Statistics

To run pager in statistics mode use:

    ./pager.py --statistics

The output will be json with histogram representing usage statistics per token, trace and function.
For each, it contains usage count and total data length consumed.

## Flags

    --color  # Paint messages in various colors
    --silent # Mute pager notifications (not recommended unless for automation)

## Misc

**Normally, you should never worry about dictionaries.**
If you need for some reason, additional dicitonary files (or formatters) can be supplied:

    ./pager.py --dict_preload /path/to/dict.json --fmtlib_preload /path/to/libfmtrs.so

## See also

Please check out **crash_analyzer** readme - tools/crash_analyzer/README.md
