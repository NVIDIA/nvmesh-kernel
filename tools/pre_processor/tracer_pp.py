#!/usr/bin/python3

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

import os
import re
import sys
import json
import argparse

import multiprocessing

def unescape(text):
    regex = re.compile(r'\\(.)')
    def replace(m):
        ch = m.group(1)
        if ch == 't': return '\t'
        elif ch == 'n': return '\n'
        elif ch == 'r': return '\r'
        return ch
    return regex.sub(replace, text)


class ParserCtx:
    """ Helper class used to store single file parser context
        Algorithm description:
            Input is a preprocessed C file.
            File is processed line by line.
            
            The main loop reads the next line, and tries to identify a token
            inside it. Token may be a block start/end ('{' or '}'), a comment
            block start ('/*' or '//'), a directive ('#') etc.

            When a token is detected, its handler is invoked. The handler by itself
            may continue reading more lines, as much as needed. For example, multiline
            comment handler reads new lines until token '*/' is read. When done,
            handler returns control to the main loop.

            The main loop also always builds 'the current statement'.
            Any data that is unhandled (not comment ; { } etc) is attached to the
            accumulated statement. When a statement breaker such as ; or { arrives,
            the accumulated statement is cut. If the accumulated statement looks important,
            for example it contains trace call or function declaration, it is
            processed further.
    """

    OPENING_TOKENS_RAW = {
        '/*': 'read_multiline_comment',
        '"': 'read_string',
        '\'': 'read_char',
        '//': 'read_singleline_comment',
        ';': 'semicolon',
        '{': 'open_block',
        '}': 'close_block',
    }

    TRACE_CALLS = {
        "NVMEIB_LOG_LONGTERM": "LONG",
        "NVMEIB_LOG_SHORTTERM": "SHORT",
        "NVMEIB_LOG_GOODPATH": "GOODPATH",
        "NVMEIB_LOG_METRICS": "METRICS",
        "NVMEIB_LOG_EPHEMERAL": "EPH",
        "NVMEIB_LOG_ETERNAL": "ETER",
    }

    def __init__(self, filename):
        self.depth = 0  # Current block depth
        self.block_name = None  # Current block name (ex struct, function etc)

        # Current statement being processed
        self.curr_statement = []  # Statement currently being read
        self.curr_statement_ln = 0  # Current statement line
        self.curr_statement_source = None  # Current statement file
        self.curr_statement_accum_fmt = []  # Current statement accumulated format
        self.curr_statement_final_fmt = None  # Current statement final format

        # Trace call contained within the current statement
        self.curr_statement_trace_call = None

        self.line = None  # Last read line
        self.filename = filename  # Original file name
        self.force_file_traces = False

        self.traces = []

        self.grab_all_lines()

        self.do_precalculations()

    def do_precalculations(self):
        """ Aggregates all precalculations that can be done offline
            (mostly regex)
        """
        # Prepare opening tokens search
        self.OPENING_TOKENS = {token: getattr(
            self, self.OPENING_TOKENS_RAW[token]) for token in self.OPENING_TOKENS_RAW}
        self.TOKENS_REGEX = re.compile(
            '|'.join([re.escape(x) for x in self.OPENING_TOKENS]))

        # Prepare trace calls search
        self.TRACE_CALLS_REGEX = re.compile(
            '|'.join([re.escape(x) for x in self.TRACE_CALLS]))

        # Prepare trace call statement regex dict
        self.TRACE_CALL_STATEMENT_REGEX_DICT = {trace:
                                                re.compile(r'''
                                                    .*?{0}\s*\( # Opening
                                                    \s*, # Format placeholder
                                                    \s*([^,]*)\s*, # Flags
                                                    \s*([^,]*)\s*, # Scope
                                                    \s*([_a-zA-Z0-9]+) # Name
                                                    .* # The rest - don't care
                                                    '''.format(trace),
                                                    re.X) for trace in self.TRACE_CALLS}

    def grab_all_lines(self):
        """ Read all lines in given files at once, called from constructor
        """
        with open(self.filename, "r") as f:
            self.lines = f.read().splitlines()
        self.lines.append('') # It is easier to add one dummy line to the end than to always check current line is not the last
        self.monotonic_ln = 0  # Line number
        self.last_ln = len(self.lines)  # Last line number
        self.ln = 0  # Line number affected by directives
        self.line_source = self.filename  # What file this line came from
        self.line_source_system_path = False  # Optimization: skip system path

    def is_done(self):
        """ True if EOF reached, false otherwise
        """
        return self.last_ln <= self.monotonic_ln

    def parse_directive(self):
        """ Special treatment function for lines started with '#'.
            The interesting ones are formatted as '# 123 "filename" ...'.
            The rest can be ignored
        """
        match = re.match(r'\#\s([0-9]*)\s\"([^\"]*)\"', self.line)
        if (match):
            self.ln = int(match.group(1)) - 1
            self.line_source = match.group(2)
            self.line_source_system_path = 'include/linux' in self.line_source or self.line_source.startswith(
                '/usr')

    def nextline(self):
        """ Get the next line to work on
        """
        while not self.is_done():
            self.line = self.lines[self.monotonic_ln]
            self.ln += 1
            self.monotonic_ln += 1
            if self.line and self.line[0] == '#':
                self.parse_directive()
            elif not self.line_source_system_path:
                break

        return None

    def read_multiline_comment(self):
        """ '/*' Symbol handler function
        """
        while not self.is_done():
            split = self.line.split('*/', 1)
            if len(split) > 1:  # Found the terminating token
                self.line = split[1]
                return
            self.nextline()

    def read_singleline_comment(self):
        """ '//' Symbol handler function
        """
        self.nextline()  # Just skip this line

    def read_char(self):
        """ ''' Symbol handler function
        """
        while not self.is_done():
            split = self.line.split('\'', 1)
            if len(split) > 1:  # Found the terminating token
                self.line = split[1]
                return
            self.nextline()
        raise ValueError('Unterminated character literal {0}:{1}'.format(
            self.curr_statement_source, self.curr_statement_ln))

    def read_string(self):
        """ '"' Symbol handler function
        """
        while not self.is_done():
            split = self.line.split('"', 1)
            if self.curr_statement_trace_call and not self.curr_statement_final_fmt:
                self.curr_statement_accum_fmt.append(unescape(split[0]))
            if len(split) > 1:  # Found the terminating token
                self.line = split[1]
                if len(split[0]) and split[0][-1] == '\\':  # It is an escaped "
                    if self.curr_statement_trace_call and not self.curr_statement_final_fmt:
                        self.curr_statement_accum_fmt.append("\"")
                    continue
                else:  # It is actually string termination
                    return
            self.nextline()
        raise ValueError('Unterminated string {0}:{1}'.format(
            self.curr_statement_source, self.curr_statement_ln))

    def push_statement(self, line):
        """ Append more data to the statement currently being built
        """
        if line and line != '' and not line.isspace():
            if not self.curr_statement_trace_call:
                match = self.TRACE_CALLS_REGEX.search(line)
                if match:
                    trace = match.group()
                    self.curr_statement_trace_call = trace
                    self.curr_statement_ln = self.ln
                    self.curr_statement_source = self.line_source
                    self.curr_statement.append(line)
                    return

            if self.curr_statement_trace_call and self.curr_statement_accum_fmt and not self.curr_statement_final_fmt:
                # Finished reading trace format, join it to one string
                self.curr_statement_final_fmt = ''.join(
                    self.curr_statement_accum_fmt)
            if self.curr_statement_trace_call or self.depth == 0:
                self.curr_statement.append(line)

        if line == 'FORCE_FILE_TRACES':
            # special macro that will force the traces on this file
            self.force_file_traces = True


    def parse_flags(self, flags):
        res = dict()
        operands = [ _.strip().rstrip() for _ in flags.split('&') if _]
        try:
            if len(operands):
                res['macro'] = operands[0] # Macro is always the first flag
            else:
                res['macro'] = ''
            for o in operands[1:]:
                if '=' in o:
                    l, r = [ _.strip().rstrip() for _ in o.split('=') if _]
                    res[l] = r
                else:
                    res[o] = True
        except Exception as e:
            raise ValueError('Malformed trace flags {0}:{1} ({2}) - {3}'.format(
                    self.curr_statement_ln, self.curr_statement_source, flags, str(e)))
        if self.force_file_traces:
            res['forced'] = True
        return res

    def parse_trace(self):
        """ Given current statement contains a trace call, extract the exact
            data from that call
        """
        statement = ' '.join(self.curr_statement)
        match = self.TRACE_CALL_STATEMENT_REGEX_DICT[self.curr_statement_trace_call].match(
            statement)
        if match:
            trace = self.parse_flags(match.group(1))
            trace.update({
                'type': self.TRACE_CALLS[self.curr_statement_trace_call],
                'file': os.path.basename(self.curr_statement_source),
                'line': self.curr_statement_ln,
                'func': self.block_name,
                'fmt': self.curr_statement_final_fmt,
                'scope': match.group(2),
                'name': match.group(3),
            })
            self.traces.append(trace)
        else:
            raise ValueError('Malformed trace call {0}:{1} ({2})'.format(
                self.curr_statement_ln, self.curr_statement_source, statement))

    def cut_statement(self):
        """ Invoked when we are sure current statement is over
            Stop appending data to current statement and start a new one
            Analyze the last statement - if it is important, extract info from it,
            else just discrad
        """
        if not self.curr_statement:
            return
        if self.curr_statement_trace_call:
            # If we had a trace call in this statement - process it
            if not self.curr_statement_ln or not self.curr_statement_source or self.curr_statement_final_fmt is None or not self.block_name:
                raise ValueError('Unexpected value: ' + str([self.curr_statement_trace_call, self.curr_statement_ln,
                                                             self.curr_statement_source, self.curr_statement_accum_fmt,
                                                             self.curr_statement_final_fmt, self.block_name]))
            self.parse_trace()

        self.curr_statement_trace_call = None
        self.curr_statement = []
        self.curr_statement_ln = 0
        self.curr_statement_source = None
        self.curr_statement_final_fmt = None
        self.curr_statement_accum_fmt = []

    def semicolon(self):
        """ ';' Symbol handler function
        """
        self.cut_statement()

    def open_block(self):
        """ '{' Symbol handler function
        """
        self.depth += 1
        if self.depth == 1:
            statement = ' '.join(self.curr_statement)
            match = re.match(
                r'.*?([_a-zA-Z0-9]+)\s*\(.*\)\s*$', statement)
            if match:
                self.block_name = match.group(1)  # Function
            else:
                self.block_name = None  # Anything else

        self.cut_statement()

    def close_block(self):
        """ '}' Symbol handler function
        """
        self.cut_statement()
        self.depth -= 1
        if self.depth == 0:
            self.block_name = None
        if self.depth < 0:
            raise ValueError('Unexpected closing "}}" at {0}:{1}'.format(
                self.curr_statement_source, self.curr_statement_ln))

    def get_opening_token(self):
        """ Given a line, extract interesting tokens from it
        """
        match = self.TOKENS_REGEX.search(self.line)
        if (match):
            return match.group(), self.OPENING_TOKENS[match.group()]
        return None, None

    def parse(self):
        """ The main loop of the parser - read line, find token, invoke handler
        """
        self.nextline()
        while not self.is_done():
            token, handler = self.get_opening_token()
            if token:
                split = self.line.split(token, 1)
                self.push_statement(split[0])
                self.line = split[1]
                handler()
                continue

            self.push_statement(self.line)
            self.nextline()
        self.cut_statement()


def init_argparse():
    """ Main arguments
    """
    parser = argparse.ArgumentParser('Tracer pre processor')
    parser.add_argument('output', type=str,
                        help='Output file name')
    parser.add_argument('input', type=str, nargs='+',
                        help='Input preprocessed files list')
    parser.add_argument('-j', action='store', dest='nprocesses', default=multiprocessing.cpu_count(), type=int, nargs='?',
                        help='Number of parallel workers to run')
    return parser

def chunk_list(seq, num):
    """ Helper function - separate iterable seq into approximately
        equal up to num chunks.
    """
    avg = len(seq) / float(num)
    out = []
    last = 0.0

    while last < len(seq):
        out.append(seq[int(last):int(last + avg)])
        last += avg

    return out

def worker_main(job):
    """ Main loop of a single worker - input job is files list
    """
    merged = []
    for file in job:
        parser = ParserCtx(file)
        try:
            parser.parse()
            merged.extend(parser.traces)
        except Exception as e:
            raise type(e)(
                str(e) + '\nWhile parsing file {0} {1}'.format(file, parser.monotonic_ln)
            ).with_traceback(sys.exc_info()[2])
    return merged

def main():
    """ Main reads arguments, splits jobs to workers, and aggregates the result
    """
    parser = init_argparse()
    args = parser.parse_args()
    
    pool = multiprocessing.Pool(args.nprocesses)
    jobs = chunk_list(args.input, args.nprocesses)

    if args.nprocesses != 1:
        mapres = pool.map(worker_main, jobs)
    else:
        mapres = [worker_main(jobs[0])]

    # Get the results from all processes
    res = []
    for subl in mapres:
        for trace in subl:
        	res.append(trace)
        	
    with open(args.output, 'w') as fp:
        json.dump(res, fp, indent=2)

    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (IOError, ValueError) as e:
        exit(str(e))

