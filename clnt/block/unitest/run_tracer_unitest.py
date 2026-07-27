#!/usr/bin/python3

import re
import sys
import os
import subprocess

PAGER_EXE = os.path.join(os.path.dirname(os.path.realpath(__file__)), 'pager')
UNITEST_EXE = os.path.join(os.path.dirname(
    os.path.realpath(__file__)), 'run_block_unitest.sh')
EXPECTED_FILE = os.path.join(os.path.dirname(
    os.path.realpath(__file__)), 'run_tracer_unitest.expected')
RUN_RESULTS_FILE = os.path.join(os.path.dirname(
    os.path.realpath(__file__)), 'run_tracer_unitest.run_results')
RUN_RESULTS_FILE_UNFILTERED = os.path.join(os.path.dirname(
    os.path.realpath(__file__)), 'run_tracer_unitest.run_results.unfiltered')
BINLOG_FILE = os.path.join(os.path.dirname(
    os.path.realpath(__file__)), 'longterm.binlog0.0')

DEVNULL = open(os.devnull, 'w')


def switch_buffers(buf, b1, b2, bsize):
    return buf[0:(b1-1)*bsize] + buf[(b2-1)*bsize:(b2)*bsize] + buf[(b1)*bsize:(b2-1)*bsize] + buf[(b1-1)*bsize:(b1)*bsize] + buf[(b2)*bsize:]


def shuffle_binlog():
    with open(BINLOG_FILE, 'rb') as file:
        buf = file.read()
    # Simply swtich places between second and fifth buffers. Why? Because I can.
    # Pager must handle this. Incorrect buffers order must be tolerated.
    buf = switch_buffers(buf, 2, 5, 4096)
    # And now remove buffer #3. Pager must detect lost information.
    buf = buf[0:4096*2] + buf[4096*3:]
    with open(BINLOG_FILE, 'wb') as file:
        file.write(buf)
    return 0


def damage_binlog():
    with open(BINLOG_FILE, 'rb') as file:
        buf = file.read()

    # Damage buffer #5 by mangling dictionary checksum
    buf = buf[0:4096*4 + 8] + 'boom'.encode('utf-8') + buf[4096*4 + 12:]

    with open(BINLOG_FILE, 'wb') as file:
        file.write(buf)
    return 0

def create_dup_buffer():
    with open(BINLOG_FILE, 'rb') as file:
        buf = file.read()
    # Take buffer 0 an duplicate it between buffers 3 and 4
    buf = buf[0:4096*3] + buf[0:4096] + buf[4096*3:]
    with open(BINLOG_FILE, 'wb') as file:
        file.write(buf)
    return 0


TESTS_TO_RUN = [
    ('Clean all logs', 'rm -rf _host1 _host2 nvmeibc.trace.json dict.*.json *.binlog*.* libfmtrs.so'),
    ('Compile', './build_block_testing.sh build > /dev/null'),
    ('Run unitest', './blk_unitest -tracedbg 6 -tracertest >/dev/null 2>&1'),
    ('Shuffle binlog', shuffle_binlog),
    ('Damage binlog', damage_binlog),
    ('Create duplicate buffer', create_dup_buffer),
    ('Simple', [
        PAGER_EXE, '--silent'
    ]),
    ('Filter simple', [
        PAGER_EXE, '--silent', '-f', '@NAME = "John" or @BITFIELD_2 = 1 or trace = "unitest_trace_hello_world"'
    ]),
    ('Filter advanced', [
        PAGER_EXE, '--silent', '-f', 'trace = "SYSTEM_TRACE" or func = "some_func"'
    ]),
    ('Filter very advanced', [
        PAGER_EXE, '--silent', '-f', 'fmt =~ *hello* or @NAME =~ *tha or @VAL = 0x123'
    ]),
    ('Filter composite', [
        PAGER_EXE, '--silent', '-f', '@TEST_COMPOSITE {@INT = 10, @STRING = "ten"} and not @NAME = "Alibaba"'
    ]),
    ('Filter less', [
        PAGER_EXE, '--silent', '-f', '@VAL_INT < 3'
    ]),
    ('Filter greater', [
        PAGER_EXE, '--silent', '-f', '@VAL_INT > 1'
    ]),
    ('Filter in', [
        PAGER_EXE, '--silent', '-f', '@VAL_INT in [1 + 1]'
    ]),
    ('Filter sticky', [
        PAGER_EXE, '--silent', '-f', '@NAME=Brian sticky @STICKY_TEST_STR => [@STICKY_TEST_STR]'
    ]),
    ('Filter sticky2', [
        PAGER_EXE, '--silent', '-f', '@NAME=John sticky @STICKY_TEST_STR => [@STICKY_TEST_STR]'
    ]),
    ('Filter sticky until', [
        PAGER_EXE, '--silent', '-f', '@NAME=John sticky @STICKY_TEST_STR => [@STICKY_TEST_STR] sticky_until fmt like "*Sticky breaker*"'
    ]),
    ('Tail', [
        PAGER_EXE, '--silent', '--tail'
    ]),
    ('Compile different dictionary',
     './build_block_testing.sh build EXTRA_CFLAGS=-DNVMEIBC_TRACER_UNITEST_ALTERNATIVE_COMPILATION SIM_DICT_CKSUM=123 >/dev/null'),
    ('Run unitest', './run_block_unitest.sh -tracedbg 6 -tracertest >/dev/null 2>&1'),
    ('Filter on multiple dictionaries', [
        PAGER_EXE, '--silent', '-f', 'trace = "unitest_msg_from_parallel_universe" or trace = "unitest_trace_hello_world"'
    ]),
    ('Prepare multihost test',
    '''
    mkdir _host1
    mkdir _host2
    cp longterm.binlog0.0 _host1
    cp longterm.binlog0.1 _host2
    cp dict.* _host1
    cp dict.* _host2
    echo watermelon > _host2/hostname'''),
    ('Multihost test', [
        PAGER_EXE, 'banana:_host1', '_host2', '--silent', '-f', 'has @NAME'
    ]),
]

LOG_LINE_TS_RGX = re.compile(r"""
    ((?:banana:|watermelon:)?) # Optional hostname
    \s* # Starting with whitespaces
    (?:[0-9:./]+\s\([0-9]+\)\s) # Timestamp
    (.*) # Remainder
    """, re.X)

LOG_LINE_TID_RGX = re.compile(r"""
    ^(.*?) # Whatever
    (\[[^\[\]]*/[^\[\]]*\]) # Thread id
    (.*) # Remainder
    """, re.X)

LOG_LINE_SRC_LINE_RGX = re.compile(r"""
    ^(.*?) # Whatever
    (:\d+\s) # Line number
    (.*) # Remainder
    """, re.X)

LOG_LINE_PAGER_MSG_RGX = re.compile('(.*PAGER MSG At ).*(clnt\/block\/unitest.*)')


def run_test(key, test, file):
    print('Runnig test <{0}>...'.format(key))
    file.write('======= <{0}> =======\n'.format(key))
    file.flush()
    if type(test) == list:
        print('cmd {0}'.format(test))
        return subprocess.call(test, stdout=file)  # Subprocess, capture output
    elif type(test) == str:
        print('cmd {0}'.format(test))
        file.write('>>> {0}\n'.format(test))
        rv = os.system(test)  # Plain shell comman, no output caputre
        return rv
    elif callable(test):
        return test()
    file.flush()


def main():

    print('Running binary tracer unitest...')

    with open(RUN_RESULTS_FILE_UNFILTERED, 'w') as file:
        for test in TESTS_TO_RUN:
            rv = run_test(test[0], test[1], file)
            if rv != 0:
                print('Terminated with exit code {0}'.format(rv))
                exit(rv)

    print('Filtering output...')

    with open(RUN_RESULTS_FILE_UNFILTERED, 'r') as inf:
        with open(RUN_RESULTS_FILE, 'w') as outf:
            for line in inf:
                match = LOG_LINE_TS_RGX.match(line)
                if match:
                    line = match.group(1) + match.group(2)
                match = LOG_LINE_TID_RGX.match(line)
                if match:
                    line = match.group(1) + match.group(3)
                match = LOG_LINE_SRC_LINE_RGX.match(line)
                if match:
                    line = match.group(1) + match.group(3)
                match = LOG_LINE_PAGER_MSG_RGX.match(line)
                if match:
                    line = match.group(1) + match.group(2)
                outf.write(line + '\n')

    os.unlink(RUN_RESULTS_FILE_UNFILTERED)

    if not os.path.isfile(EXPECTED_FILE):
        print('No expected file found. Assuming first run ever. Creating.')
        os.rename(RUN_RESULTS_FILE, EXPECTED_FILE)
        return 1

    if subprocess.call(['diff', '--color', EXPECTED_FILE, RUN_RESULTS_FILE]):
        print('\nError diff with expected')
        print('Expected file: ' + EXPECTED_FILE)
        print('Results file: ' + RUN_RESULTS_FILE)
        return 2

    print('Results match expected. SUCCESS.')

    return 0


if __name__ == '__main__':
    exit(main())
