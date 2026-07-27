#!/usr/bin/env python3
# PYTHON_ARGCOMPLETE_OK
import os
import sys
import json
import argparse
import subprocess
import atexit
import logging

from itertools import chain
from pager import valid_datetime
BUNDLE_MODE = hasattr(sys, '_MEIPASS')
  
def init_logger(verbose):

    logger = logging.getLogger('pager-profiler')
    logger.setLevel(logging.DEBUG)
    logger.propagate = False

    formatter = logging.Formatter('%(asctime)s %(name)s: %(levelname)s - %(message)s')

    stdout_handler = logging.StreamHandler(sys.stdout)
    if verbose:
        stdout_handler.setLevel(logging.DEBUG)
    else:
        stdout_handler.setLevel(logging.INFO)
    stdout_handler.setFormatter(formatter)

    logger.addHandler(stdout_handler)

    return logger


logger = None


@atexit.register
def flush_logger(*args, **kwargs):
    if logger:
        logging.shutdown()


def valid_dir(p):
    """ Given as string p return abspath(p) if it is a valid directory or raise
    """
    abspath = os.path.abspath(p)
    if os.path.isdir(abspath):
        return abspath
    raise argparse.ArgumentTypeError('Invalid path `{0}`'.format(p))


TOPICS = {
    "client": {
        "channels": ["nvmeibc_trace_long", "nvmeibc_trace_eter"]
    },
    "goodpath": {
        "channels": ["nvmeibc_trace_goodpath"]
    },
    "metrics": {
        "channels": ["nvmeibc_trace_metrics", "nvmeibs_trace_metrics"]
    },
    "server": {
        "channels": ["nvmeibs_trace_long"]
    },
    "toma": {
        "channels": ["toma.binlog"]
    },
    "simulator": {
        "channels": ["longterm.binlog", "goodpath.binlog", "eneternal.binlog"]
    }
}


class ProfileData:
    cuts = ['traces', 'tokens', 'functions', 'files']
    selector_titles = {'size': 'Size (B)', 'count': 'Total Count', 'throughput': 'Throughput (B/s)', 'percentage': 'Percentage(%) from Total'}
    selectors = selector_titles.keys()

    def __init__(self):
        self.tstart = 18446744073709551615
        self.tend = 0
        self.count_system_trace = 0

    def get_cut(self, cut):
        return getattr(self, cut)

    def get_cut_selector(self, cut, selector):
        return sorted(getattr(self, cut).items(), reverse=True, key=lambda t: t[1][selector])

    @property
    def tinterval_ns(self):
        return self.tend - self.tstart

    @property
    def tinterval_s(self):
        return self.tinterval_ns / 1000000000

    @property
    def total_bytes(self):
        return sum(trace['size'] for trace in self.traces.values())

    @property
    def throughput(self):
        return self.total_bytes / self.tinterval_s if self.tinterval_s != 0 else 0

    def merge(self, data):
        if not data['first_trace_ts_ns'] or not data['last_trace_ts_ns']:
            return
        self.tstart = float(min(data['first_trace_ts_ns'], self.tstart))
        self.tend = float(max(data['last_trace_ts_ns'], self.tstart))
        self.count_system_trace += data['count_system_trace']

        for cut in self.cuts:
            self.__merge_cut(cut, data[cut.upper()])

    def __merge_cut(self, cut, data):
        out = self.get_cut(cut)
        for k, newdata in data.items():
            olddata = out.get(k)
            if not olddata:
                olddata = newdata
                out[k] = olddata
            else:
                olddata['size'] += newdata['size']
                olddata['count'] += newdata['count']
        # Computed values
        for d in out.values():
            self.__compute_data_fields(d)

    def __compute_data_fields(self, data):
        data['throughput'] = data['size'] / self.tinterval_s if self.tinterval_s != 0 else 0
        data['percentage'] = (float(data['size']) / self.total_bytes) * 100 if self.total_bytes != 0 else 0


# Populate some more methods in ProfileData class
for cut in ProfileData.cuts:
    setattr(ProfileData, cut, dict())
    for selector in ProfileData.selectors:
        setattr(ProfileData, cut + '_by_' + selector, property(lambda self: self.get_cut_selector(cut, selector)))


def profile_report_by_cut_and_selector(f, data, cut, selector, top):
    f.write('Report for {0} by {1}\n'.format(cut, selector))
    f.write('ID,{0}\n'.format(ProfileData.selector_titles[selector]))
    top = data.get_cut_selector(cut, selector)[:top]
    for line in top:
        f.write('{0},{1}\n'.format(line[0], line[1][selector]))
    f.write('\n')


def list_cross(list1, list2):
    return ((i, j) for i in list1 for j in list2)


def profile(topic, args):
    logger.info('Profiling topic ###{0}###'.format(topic))

    # Run pager
    cmd = list(chain([args.pager, args.logs_dir, '--statistics', '-t', args.since, args.until],
                     ['--reliable'] if args.reliable else [],
                     ['-l'], TOPICS[topic]["channels"]))
    logger.info('Running pager')
    logger.debug('CMD: {0}'.format(str(cmd)))
    popen = subprocess.Popen(cmd, stdout=subprocess.PIPE, universal_newlines=True)
    stats = json.load(popen.stdout)
    popen.stdout.close()
    logger.info('Pager exit code: {0}'.format(popen.wait()))

    # Analyze the data
    logger.info('Analyzing data')
    data = ProfileData()
    for ch_stats in stats.values():
        data.merge(ch_stats)

    # Output
    outfile = os.path.join(args.output, topic + '-profile.csv')
    logger.info('Generating report `{0}`'.format(outfile))
    with open(outfile, 'w') as f:
        f.write('Report for topic `{0}`\n\n'.format(topic))
        f.write('Time interval analyzed (seconds),{0}\n'.format(data.tinterval_s))
        f.write('Total throughput (B/s),{0}\n'.format(data.throughput))
        f.write('Total data (B),{0}\n'.format(data.total_bytes))
        f.write('\n')
        for cut, selector in list_cross(ProfileData.cuts, ProfileData.selectors):
            profile_report_by_cut_and_selector(f, data, cut, selector, args.top)


def argcomplete_if_possible(parser):
    """ Try initialize argcomplete if available
    """
    from importlib import import_module
    try:
        argcomplete = import_module("argcomplete")
    except:
        pass  # Too bad, but we can live with that
    else:
        argcomplete.autocomplete(parser)

def get_pager_path():
    if BUNDLE_MODE:                          
        return os.path.join(os.path.dirname(os.path.realpath(sys.executable)), 'pager')  # type:
    return os.path.join(os.path.dirname(os.path.realpath(__file__)), 'pager.py')

def init_argparse():
	parser = argparse.ArgumentParser('Pager profiler')
	parser.add_argument(dest='topics', nargs='*', type=str, default=list(TOPICS.keys()), choices=list(TOPICS.keys()))
	parser.add_argument('--since', dest='since', type=valid_datetime, default=valid_datetime('0'))
	parser.add_argument('--until', dest='until', type=valid_datetime, default=valid_datetime('18446744073709551615'))
	parser.add_argument('-o', '--output', dest='output', type=valid_dir, default='.')
	parser.add_argument('--top', dest='top', type=int, default=10)
	parser.add_argument('-l', '--logs_dir', dest='logs_dir', type=valid_dir, default='/var/log/nvmesh/trace_daemon')

	# Hidden
	parser.add_argument('--pager', dest='pager', type=str, default=get_pager_path(), help=argparse.SUPPRESS)
	parser.add_argument('--reliable', dest='reliable', action='store_true', help=argparse.SUPPRESS)
	parser.add_argument('--verbose', dest='verbose', action='store_true', help=argparse.SUPPRESS)
	argcomplete_if_possible(parser)
	return parser


def main():
	global logger
	args = init_argparse().parse_args()
	#print(f"\n\n\n\n{args}\n\n\n\n");
	logger = init_logger(args.verbose)
	for topic in args.topics:
		profile(topic, args)
	return 0

if __name__ == '__main__':
    sys.exit(main())
