#!/usr/bin/env python3

#
# DESCRIPTION:
#

import sys
import re
import pprint
import os
import json
import clang.cindex
import subprocess
from termcolor import colored, cprint
from clang.cindex import CursorKind
from collections import OrderedDict
from threading import Timer

###### CONSTANTS ######

CPP_ARGS = '-Wall -Werror -std=gnu99 -Wno-deprecated -Wdeclaration-after-statement -Wextra -Wshadow -Wno-old-style-declaration -Werror=strict-aliasing -g3 -ggdb3 -m64 -fvisibility=default -D__TRACE_CONV_UTIL_RUN -DBLKDEV_SIMULATOR=1 -DDEBUG_TOPO_CNTRS -DAUTONOMOUS_SYNCS_STATS -DDEBUG_LOCKS_CORRUPTION -DNVMEIBC_SECTOR_SHIFT=12 -DTRACE_CPUID -DDEBUG_LOSER_CONDITIONS -DHTR_HACK_TRIGGER=1 -DVER_TAGID=v2.0.0 -DNVMESH_VERSION=v2.0.0 -DNVMESH_RELEASE=1.1.0-97 -DBUILD_NUMBER=0.0 -I../clnt/ -I../srv/ -I../common/ -I../common_public/ -I../toma/ -I../autogen/common/ -I../autogen/clnt/ -I../autogen/srv/ -I/usr/include'
CPP_ARGS_LIST = CPP_ARGS.split()

MAX_FILES = 500

REFACTORED = '<REFACTORED_PRINT>'

VERBOSE = True

STATISTICS = True

PRINT_WARINGS = True

WRITE = True

CONV_TOKEN = ''

DICTIONARY_JSON = '../tools/dictionary.json'

HANDLE_SIGINT = True

FORCE_KILL_TIMEOUT = 2.0

FILES = sys.stdin

PROBLEMS_ONLY = False

ALL = False

MAX_TRACE_ARGS = 10

EXCLUDE_FILES = []

INITIAL_DICTIONARY_JSON = DICTIONARY_JSON
# INITIAL_DICTIONARY_JSON = '../../dictionary_candidate.json'
# INITIAL_DICTIONARY_JSON = {
#     '@DUMMY_CHAR': {
#         'size': 8,
#         'type': 'char',
#         'fmt': '%c'
#     },
#     '@HTR_STATUS': {
#         'size': 'string',
#         'type': 'string',
#         'fmt': '%s'
#     },
#     '@TR_DEVICE': {
#         'size': 'string',
#         'type': 'string',
#         'fmt': '%s'
#     },
#     '@TR_CHANNEL': {
#         'size': 32,
#         'type': 'int',
#         'fmt': '%d'
#     },
#     '@TR_RAID1': {
#         'size': 32,
#         'type': 'int',
#         'fmt': '%d'
#     },
#     '@TR_SEGMENT': {
#         'size': 32,
#         'type': 'int',
#         'fmt': '%d'
#     },
#     '@TR_RECOV_TYPE': {
#         'size': 32,
#         'type': 'int',
#         'fmt': '%d'
#     },
#     '@HTR_PARAM': {
#         'size': 'string',
#         'type': 'string',
#         'fmt': '%s'
#     },
#     '@FINOUT_PARAM': {
#         'size': 'string',
#         'type': 'string',
#         'fmt': '%s'
#     },
#     '@TS_PD': {
#         'size': 64,
#         'type': 'const void*',
#         'fmt': '%p'
#     },
#     '@TS_DISK_ID': {
#         'size': 'string',
#         'type': 'string',
#         'fmt': '%s'
#     },
#     '@TS_DI': {
#         'size': 64,
#         'type': 'const void*',
#         'fmt': '%p'
#     },
# }

###### END CONSTANTS ######


# This code allows to stop main loop gracefully


terminate = False
killer = Timer(FORCE_KILL_TIMEOUT, exit, [-1])
if HANDLE_SIGINT:
    import signal

    def signal_handler(sig, frame):
        killer.start()
        global terminate
        terminate = True
    signal.signal(signal.SIGINT, signal_handler)


###### PRINT UTILS ######

def dump_node(node, indent=3, lvl=0):
    print(('|' + '-' * indent) * lvl + f'{node.kind}({node.spelling})')
    for c in node.get_children():
        dump_node(c, indent, lvl + 1)


def dump_trace(trace, indent=3):
    pprint.pprint(trace.to_list())


def etext(x):
    return colored(x, 'red', attrs=['bold'])


def perror(x):
    print(etext(x), file=sys.stderr)

###### END PRINT UTILS ######


###### GENERAL UTILS ######

def rreplace(s, old, new, occurrence):
    li = s.rsplit(old, occurrence)
    return new.join(li)


def chomp(x):
    if x.endswith("\r\n"):
        return x[:-2]
    if x.endswith("\n") or x.endswith("\r"):
        return x[:-1]
    return x

###### GENERAL UTILS ######


# Global dictionary from filename to traces listself.
# It is simply simplier to keep it global
traces = {}
suggestions = {}
renaming_histogram = {}

# We dump any important warning during the calculation process here.
# In the end, all those warning are presented to the user (in glowing red)
warnings = []

# We dump whatever statistics related info here
statistics = OrderedDict()


def recstats(key, plus=1):
    if STATISTICS:
        statistics[key] = statistics.get(key, 0) + plus


class Trace:
    """
    Representation of a single tracepoint with all the properties we could get for it
    """

    TOKEN_FULL_RGX = re.compile(
        r'''(?:\\[A-Za-z])? # If starts with an escape character - omit take it
            ([A-Za-z0-9_]* # Prefix
            :?[=\s]? # Separator
            (?:0x)? # Hex prefix
            \% # Format starts here
            [0-9\#\-\.\*]* # Special symbol format prefix - all optional
            [0-9\.]* # Size specifiers 2
            [A-Za-z0-9]+ # Body
            )''',
        re.X)

    TOKEN_FMT_RGX = re.compile(
        r'.*?((?:0x)?\%.*)$')

    HINT_RGX = re.compile(
        r'^\s*//\s*Trace args:\s*\[\s*(.*)\s*\]')

    # Those formats are printk specific. Cannot be used with postprocessing (at this stage).
    # Using them should fail the trace, as it has to be refactored manually
    BAD_FORMATS = ['%ps', '%pb', '%*p',
                   '%pr', '%pm', '%pd', '%pg', '%pv', '%po']

    @staticmethod
    def is_bad_format(fmt):
        for bad_token in Trace.BAD_FORMATS:
            if fmt.lower().startswith(bad_token):
                return True
        return False

    def __init__(self, node, filename, lines, scope):
        self.asterisk = False
        self.node = node
        self.filename = filename
        self.scope = scope[-1]

        # Actual literal is the first child of each of our children
        self.args = [next(c.get_children()) for c in self.node.get_children()]

        # First argument is function reference, not interesting
        self.args.pop(0)
        # Next argument is format - extract and save
        self.fmt = self.args.pop(0)
        # Next argument is dummy, not interesting
        self.args.pop(0)
        # Next argument macrotype
        self.macrotype = self.args.pop(0).spelling.strip('"')
        # Last argument is dummy, not interesting
        self.args.pop(-1)
        # Whatever args left - we are happy

        self._compute_props(lines)

        recstats('traces')
        if self.args:
            recstats('with_args')

    ###### PRIVATE ######

    def _extract_tokens_full(self):
        return self.TOKEN_FULL_RGX.findall(self.strfmt)

    def _extract_tokens(self):
        return [self.TOKEN_FMT_RGX.match(x).group(1) for x in self.tokens_full]

    def _fix_asterisk(self):
        # In printf the '*' requires special handling as it means additional argument will
        # be passed to specify field width. For sake of this script we have to ignore that one
        for idx, val in enumerate(self.tokens):
            if '*' in val:
                recstats('damned_asterisk')
                self.asterisk = True
                warnings.append(f'Asterisk found at {str(self)}). This has to be handled manually.')
                self.args.pop(idx)

    def _extract_argext(self):
        """
        Make the connection between args and tokens, get all extra info needed.
        ASSUMPTION: Token is not split between two lines. Sounds reasonable.
        """

        # IMPORTANT: The search is from end to beginning. This is because
        # nested macros usually push arguments to the beginning of the line
        # so we want to ive priority to real unique arguments, which are more
        # likely to be in the end
        line = len(self.lines) - 1
        char = len(self.lines[line])
        argext = []

        argext = [None] * len(self.tokens)   # First create an empty list

        idx = len(self.tokens)
        for token in reversed(self.tokens):
            idx -= 1
            # Now fill in values list
            lastline = line
            lastchar = char
            # Try to find the token in the actual code
            while True:
                char = self.lines[line].rfind(token, 0, char)
                if (char != -1):
                    # Yay! Found it
                    end = char + len(token)
                    argext[idx] = {
                        'line': line,
                        'start': char,
                        'end': end
                    }
                    break
                else:
                    line -= 1
                    if line < 0:
                        # Could not find token. Leave it empty
                        line = lastline
                        char = lastchar
                        break
                    char = len(self.lines[line])
        return argext

    def _compute_props(self, lines):
        """
        This function extracts all usable info we can calculate ACCURATELY.
        Info that is not precise is (all the suggestions) is not computed
        here.
        """
        # For string values, grab them and remove enclosing quates
        self.strargs = [a.spelling.strip('"') for a in self.args]
        self.strfmt = self.fmt.spelling.strip('"')
        self.strscope = self.scope.spelling.strip('"')
        self.start = self.node.extent.start.line
        self.end = self.node.extent.end.line
        self.lines = lines[self.start - 1:self.end]  # Lines are 1 based

        self.tokens_full = self._extract_tokens_full()  # Extracts tokens from format
        self.tokens = self._extract_tokens()
        self.bad_token = False
        if any(self.is_bad_format(token) for token in self.tokens):
            warnings.append(f'Bad format token at {str(self)}')
            recstats('bad_format_token')
            self.bad_token = True
        self._fix_asterisk()
        assert (len(self.tokens) == len(self.args)), \
            f'Tokens do not match arguments.\nFmt: {self.strfmt}\nTokens: {self.tokens}\nArgs: {self.strargs}'

        self.argext = self._extract_argext()

        # Do we have a hint?
        self.hint = None
        if self.start > 1:
            # Rember: lines are 1 based, arrays are 0 based
            match = Trace.HINT_RGX.match(lines[self.start - 2])
            if match:
                recstats('hints')
                self.hint = [x.strip().upper()
                             for x in match.group(1).split(',')]

    def __str__(self):
        return f'trace at "{self.filename}" lines ({self.start},{self.end})'

    def to_list(self):
        return [
            ('args', self.strargs),
            ('fmt', self.strfmt),
            ('start', self.start),
            ('end', self.end),
            ('lines', self.lines),
            ('tokens', self.tokens),
            ('argext', self.argext),
            ('macrotype', self.macrotype),
        ]


class RenameHistogramEntry:
    """
    This class holds a set of all possible formats for a given token.
    It can be used to deduce actual variable type for each token,
    come up with most popular format (if there is more then one option),
    and detect possible conflicts in types.
    """

    FMT_STR_RGX = re.compile(
        r'.*(\%[^\s]*)')

    SIGNED = ['d', 'i', 'o']
    UNSIGNED = ['u', 'x', 'X']
    INTS = SIGNED + UNSIGNED
    FLOATS = ['e', 'E', 'f', 'g', 'G']

    def __init__(self, token, original=None):
        self.token = token
        self.error = False
        self.formats = {}
        self.fmt_class = None
        self.split = False
        self.reliable = False  # Used to mark as final in output. For human reviewers only
        self.original = original  # In case we loaded this entry from old dictionary
        if original and original.get('fmt'):
            self.add(original.get('fmt'), True)

    def add(self, fmt, reliable):
        cls = self._deduce_intermediate_type(fmt)['class']
        if not self.fmt_class:
            self.fmt_class = cls
        elif self.fmt_class != cls:
            # Huston we've got a problem. Cannot have same token for two incompatible formats
            warnings.append(f'Format {fmt} is incompatible with {self.formats}')
            recstats('format_collision')
            self.split = True
            return False, self.token + '_' + cls
        if reliable:
            # If we have one reliable source for this format - it is reliable
            self.reliable = True
        e = self.formats.get(fmt, 0)
        self.formats[fmt] = e + 1
        return True, self.token

    def render(self):
        """
        Renders all data accumulated into JSON compatible dictionary.
        Should be used afer all data is in place
        """
        # Retrieve the most popular format and use it
        most_popular = max(self.formats, key=lambda k: self.formats[k])

        # Get its type info
        t = self._deduce_intermediate_type(most_popular)

        # And now start building the final dictionary
        res = OrderedDict([
            ('size', t['type'][0]),
            ('type', t['type'][1]),
            ('fmt', most_popular)
        ])

        if self.reliable:
            res['final'] = True

        # If we have more info - add it too
        if t.get('extra'):
            for (k, v) in t['extra'].items():
                res[k] = v

        # If we have original (from old dictionary) - don't lose any
        # additional key it stores
        if self.original:
            for k in self.original:
                if not k in res:
                    res[k] = self.original[k]

        # Add some useful but not crucial statistics
        if len(self.formats) > 1:
            res['__collision'] = True
        if self.split:
            res['__split'] = True
        if not res.get('__histogram'):
            res['__histogram'] = {}
        for f in self.formats:
            if not res['__histogram'].get(f):
                res['__histogram'][f] = 0
            res['__histogram'][f] += self.formats[f]

        return res

    ##### PRIVATE #####

    def _deduce_intermediate_type(self, fmt):
        """
        Deduce a data structure describing type properties
        from format
        """
        fmt_str = RenameHistogramEntry.FMT_STR_RGX.match(
            fmt).group(1)  # Should never fail

        ft = {}
        if 'pU' in fmt_str:
            # UUID type, treated as 16 bytes array with a special flag
            ft['class'] = 'UUID'
            ft['type'] = (64, 'const void*')
            ft['extra'] = {
                'uuid': True
            }
            return ft

        if 'pf' in fmt_str:
            # FUNC type, treated as 16 bytes array with a special flag
            ft['class'] = 'FUNC'
            ft['type'] = (64, 'const void*')
            ft['extra'] = {
                'func': True
            }
            return ft

        if 'pI6' in fmt_str:
            # IPV6 type, treated as 16 bytes array with a special flag
            ft['class'] = 'IPV6'
            ft['type'] = (64, 'const void*')
            ft['extra'] = {
                'ipv6': True
            }
            return ft

        if 's' in fmt_str:
            # String, not much to do
            ft['class'] = 'STR'
            ft['type'] = ('string', 'string')
            return ft

        if 'p' in fmt_str:
            # Pointer, not much to do
            ft['class'] = 'PTR'
            ft['type'] = (64, 'const void*')
            return ft

        if 'c' in fmt_str:
            # Char, really? Who cares
            ft['class'] = 'CHR'
            ft['type'] = (8, 'char')
            return ft

        if any((c in fmt_str) for c in RenameHistogramEntry.FLOATS):
            # We assume any float is float, not double, which seems like truth
            ft['class'] = 'FLT'
            ft['type'] = (32, 'float')
            return ft

        if any((c in fmt_str) for c in RenameHistogramEntry.INTS):
            if 'll' in fmt_str:
                ft['class'] = 'LLONG'
                ft['type'] = (64, 's64')
                return ft
            elif 'l' in fmt_str:
                ft['class'] = 'LONG'
                ft['type'] = (64, 'long')
                return ft
            else:
                ft['class'] = 'INT'
                ft['type'] = (32, 's32')
                return ft

        # if any((c in fmt_str) for c in RenameHistogramEntry.SIGNED):
        #     # SIGNED int
        #     ft['class'] = 'SINT'
        #     ft['type'] = (64, 's64') if 'l' in fmt_str else (32, 's32')
        #     return ft
        #
        # if any((c in fmt_str) for c in RenameHistogramEntry.UNSIGNED):
        #     # UNSIGNED int
        #     ft['class'] = 'UINT'
        #     ft['type'] = (64, 'u64') if 'l' in fmt_str else (32, 'u32')
        #     return ft

        assert False, f'Could not parse format {fmt} ({fmt_str}). Should never happen'


class Suggestion:
    """
    This class is an encapsulator for suggestion on how to refactor a piece of code
    """

    def __str__(self):
        return f'suggestion for {str(self.trace)}'

    # Precompile useful regex
    PREFIX_RGX = re.compile(
        r'(^[A-Za-z0-9_]+)\s*=\s*')
    MEMBER_RGX = re.compile(
        r'.*?([A-Za-z0-9_]+)[A-Za-z0-9_\[\]\(\)]*\s*(\.|\-\>)\s*([A-Za-z0-9_]+)\s*$')
    PLAIN_RGX = re.compile(
        r'^\s*\&?(?:\+\+)?([A-Za-z0-9_]+)(?:\+\+)?\s*$')
    FUNC_RGX = re.compile(
        r'^\s*([A-Za-z0-9_]+)\s*(\(.*\))\s*$')
    GETTER_RGX = re.compile(
        r'^.*get_([A-Za-z0-9_]+)\s*(\(.*\))\s*$')
    NVMEIB_FUNC_RGX = re.compile(
        r'^\s*nvmeib._([A-Za-z0-9_]+)\s*(\(.*\))\s*$')
    TRINAR_RGX = re.compile(
        r'^(.*)\?(.*)\:(.*)')

    NAMESPACE_RGX = re.compile(
        r'^(.*?)/?(nvmeib._)?(block_)?_*([A-Za-z0-9_]+)\..+?')

    MACRO_REGEX = re.compile(
        r'^(\s*)([A-Za-z0-9_]+)(\s*)(.*)')

    RESERVED_NAMES = {
        'u[0]': 'UUID_0', 'u[1]': 'UUID_1',
        '(__func__)': 'FUNCTION'
    }

    # If we have an arg named parent->special_arg we will rename it to
    # parent_special_arg
    SPECIAL_ARGS = {'NAME', 'VERSION', 'FULL_NAME',
                    'UUID', 'UID', 'TYPE', 'UNIQUEID'}

    SPECIAL_RENAMES = {
        'RV': 'RV', 'O': 'OPERATION', 'T': 'TOPOLOGY',
    }

    # Those macro - we do not convert at all
    IGNORED_MACROS = {'BUF_ADD_AND_LOG'}

    # The map is from macro name to number of hidden arguments it gets
    KNOWN_NESTED_MACROS = {
        '_Ts': 3, '_Ds': 3, '_Es': 3, '_Ws': 3,
        '_Tj': 3, '_Dj': 3, '_Ej': 3, '_Wj': 3,
        '_Tn': 3, '_Dn': 3, '_En': 3, '_Wn': 3,
        '_TR': 4, '_DR': 4, '_ER': 4,
        '_TRR': 6, '_DRR': 6,
        '__EXC_2433_if_happened_goto': 4, '_T_IR': 4,
        '_TSO': 1,
        'schedule_on_main_wq': 1,
        '_Dtbuf': 3,
        'FINS': 1, 'FOUTS': 1,
        '__FIN': 2, '__FOUT': 2,
        '__FIND': 2, '__FOUTD': 2,
        '_Th': 1, '_Dh': 1, '_Wh': 1, '_Eh': 1, '_Ih': 1, 'HTR_STS': 1
    }

    MACRO_TRANSLATIONS = {
        '_F': '_NF', '_D': '_ND', '_T': '_NT', '_I': '_NI', '_W': '_NW', '_E': '_NE',
        '_Ts': '_NTs', '_Ds': '_NDs', '_Es': '_NEs', '_Ws': '_NWs',
        '_Tj': '_NTj', '_Dj': '_NDj', '_Ej': '_NEj', '_Wj': '_NWj',
        '_Tn': '_NTn', '_Dn': '_NDn', '_En': '_NEn', '_Wn': '_NWn',
        '_TR': '_NTR', '_DR': '_NDR', '_ER': '_NER',
        '_TRR': '_NTRR', '_DRR': '_NDRR',
        '__EXC_2433_if_happened_goto': '__NEXC_2433_if_happened_goto', '_T_IR': '_NT_IR',
        '_TSO': '_NTSO',
        'flog': 'nflog',
        'schedule_on_main_wq': 'Nschedule_on_main_wq',
        '_Dtbuf': '_NDtbuf',
        'FIN': 'NFIN', 'FOUT': 'NFOUT', '__FIN': '__NFIN', '__FIND': '__NFIND', '__FOUT': '__NFOUT', '__FOUTD': '__NFOUTD', 'FINS': 'NFINS', 'FOUTS': 'NFOUTS',
        '_Th': '_NTh', '_Dh': '_NDh', '_Wh': '_NWh', '_Eh': '_NEh', '_Ih': '_NIh', 'HTR_STS': 'NHTR_STS'
    }

    # Global counter for number of prints inside a given scope
    _scopeidxmap = {}

    def __init__(self, trace):

        # No error to begin with. If we do an encounter some error at some stage,
        # we set this. If error is set, applying the change will not generate
        # a new code, instead it will generate a comment with best effort suggestion
        self.error = False

        self.trace = trace

        self.macro = self._get_macro_name()

        # Renaming patterns for each trace token, ex: %d => @LEN=%d
        self.renaming = self._gen_renaming()

        # LTTNG unique title for trace
        self.title = self._gen_title()

        # Precompute fixed lines
        self.fixed_lines = self._gen_fixed_lines()

        if not self.error:
            recstats('good_guess')
            if self.trace.args:
                recstats('good_guess_with_args')
        recstats('total_args_to_parse', len(self.trace.args))

    ###### PRIVATE ######

    _unique_idx = 0

    def _uidx(self):
        Suggestion._unique_idx += 1
        return Suggestion._unique_idx

    @property
    def ignored_macro(self):
        return self.macro in Suggestion.IGNORED_MACROS

    @property
    def _errlevel(self):
        """
        Deduce this trace error level, based on either macro type or format
        """
        if self.trace.macrotype == '_E':
            return 'error'
        if self.trace.macrotype == '_W':
            return 'warn'
        if self.trace.macrotype == 'flog':
            return 'flog'
        if 'error' in self.trace.strfmt.lower():
            return 'error'
        if 'waring' in self.trace.strfmt.lower():
            return 'warn'
        if self.macro and 'FIN' in self.macro:
            return 'fin'
        if self.macro and 'FOUT' in self.macro:
            return 'fout'
        return 'trace'

    @property
    def _scopeidx(self):
        """
        Return a unique index (zero based) for this trace's scope and with respect to error level
        """
        s = Suggestion._scopeidxmap.get(self.trace.scope.spelling)
        if not s:
            s = {'error': 0, 'warn': 0, 'trace': 0, 'fin': 0, 'fout': 0, 'flog': 0}
            Suggestion._scopeidxmap[self.trace.scope.spelling] = s
        errlevel = self._errlevel
        idx = s[errlevel]
        s[errlevel] = idx + 1
        return idx

    @property
    def _scopeidx_str(self):
        idx = self._scopeidx
        if idx:
            return f'{self._errlevel}_{idx}'
        return self._errlevel

    @property
    def _namespace(self):
        match = Suggestion.NAMESPACE_RGX.match(self.trace.filename)
        if match:
            return match.group(4)
        return 'stub'

    def _gen_title(self):
        stripped_scope = self.trace.scope.spelling.strip('_')
        return f'{self._scopeidx_str}_{self._namespace}_{stripped_scope}'

    def _gen_renaming_token(self, token, token_full, arg):

        match = Suggestion.PREFIX_RGX.match(token_full)
        rename = None
        if match:
            recstats('prefixmatch')
            # If we have token format prefix=%yyy it is easy
            rename = match.group(1).upper()

        if not rename or len(rename) < 3:
            # There are some (very few) reserved expressions that we always convert to preset value
            match = Suggestion.RESERVED_NAMES.get(arg)
            if match:
                recstats('reservedmatch')
                rename = match

        if not rename or len(rename) < 3:
            # If rename is not good enougth
            match = Suggestion.PLAIN_RGX.match(arg)
            if match:
                recstats('plainmatch')
                # If we have plain format like some_name or some_func(some_args) it is also OK
                rename = match.group(1).upper()

        if not rename or len(rename) < 3:
            # If rename is still not good enougth
            match = Suggestion.MEMBER_RGX.match(arg)
            if match:
                recstats('membermatch')
                # If we have a member acces like struct->member it can get tricky
                parent = match.group(1).upper()
                child = match.group(3).upper()
                if child in Suggestion.SPECIAL_ARGS:
                    rename = parent + "_" + child
                else:
                    rename = child

        if not rename or len(rename) < 3:
            # If rename is still not good enougth
            match = Suggestion.GETTER_RGX.match(arg)
            if match:
                rename = match.group(1).upper()
                recstats('gettermatch')

        if not rename or len(rename) < 3:
            # If rename is still not good enougth
            match = Suggestion.NVMEIB_FUNC_RGX.match(arg)
            if match:
                rename = match.group(1).upper()
                recstats('nvmeib_funcmatch')

        if not rename or len(rename) < 3:
            # If rename is still not good enougth
            match = Suggestion.FUNC_RGX.match(arg)
            if match:
                rename = match.group(1).upper()
                recstats('funcmatch')

        if not rename or len(rename) < 3:
            # If rename is still not good enougth
            match = Suggestion.TRINAR_RGX.match(arg)
            if match:
                # C Trinary expression - open it and rerun on smaller case
                recstats('trinarmatch')
                return self._gen_renaming_token(token, token_full, match.group(2))

        if rename and Suggestion.SPECIAL_RENAMES.get(rename):
            return Suggestion.SPECIAL_RENAMES[rename]

        if not rename or len(rename) < 2:
            if not rename:
                warnings.append(f'nomatch for arg "{arg}" token "{token}" {str(self)}')
                recstats('nomatch')
            else:
                warnings.append(f'tooshort: "{rename}" for arg "{arg}" token "{token}" {str(self)}')
                recstats('tooshort')

            # Here we did not find a suitable or it is one letter name - we fail to give
            # any good suggestion
            self.error = True
            return None
        return rename

    def _gen_renaming(self):

        if self.ignored_macro:
            return []

        renaming = []
        # Broken means the macro is of some non standard format, and we have to fix it manually
        # We still do the best effort to deduce tags from it
        # Initial value is asterisk, because using asterisk in format is always a problem
        broken = self.trace.asterisk or self.trace.bad_token
        if (len(self.trace.args) > MAX_TRACE_ARGS):
            warnings.append(f'Too many args for {str(self)}')
            recstats('too_many_args')
            broken = True

        first_arg = Suggestion.KNOWN_NESTED_MACROS.get(self.macro, 0)
        deduced_from_hint = False

        for idx, arg in enumerate(self.trace.args[first_arg:], first_arg):
            if not self.trace.argext[idx]:
                broken = True

            if self.trace.hint:  # If we have a hint - use it
                rnm = self.trace.hint[idx]
                deduced_from_hint = True
            else:  # If not - guess it
                rnm = self._gen_renaming_token(
                    self.trace.tokens[idx], self.trace.tokens_full[idx], self.trace.strargs[idx])

            if rnm:
                # Maintain renaming histogram
                stop = False
                while not stop:
                    rhe = renaming_histogram.get(rnm)
                    if not rhe:
                        rhe = RenameHistogramEntry(rnm)
                        renaming_histogram[rnm] = rhe
                    stop, rnm = rhe.add(
                        self.trace.tokens[idx], deduced_from_hint)

            renaming.append(rnm)

        if broken:
            self.error = True
            warnings.append(f'Broken macro at {str(self)}')
            recstats('broken')

        return renaming

    def _gen_reg_tokens(self):
        return []

    def _gen_fixed_lines_no_error(self):
        """
        Get the final lines as they should be after the suggested change is applied
        not including error handling
        """
        newlines = []
        # Parsing pointers
        charptr = 0
        lineptr = 0
        # String we are working on
        line = ''
        first_arg = Suggestion.KNOWN_NESTED_MACROS.get(self.macro, 0)
        for idx, rename in enumerate(self.renaming):
            argext = self.trace.argext[idx + first_arg]
            # If we had an error, just skip this arg. We are just making best effort.
            if rename and argext:
                while lineptr != argext['line']:
                    line += self.trace.lines[lineptr][charptr:]
                    newlines.append(line)
                    line = ''
                    lineptr += 1
                    charptr = 0
                line += self.trace.lines[lineptr][charptr:argext['start']]
                line += f'@{rename}'
                charptr = argext['end']

        # So, we are done with renaming. Now just append what we have left
        while lineptr < len(self.trace.lines):
            line += self.trace.lines[lineptr][charptr:]
            newlines.append(line)
            line = ''
            lineptr += 1
            charptr = 0

        return newlines

    def _get_macro_name(self):
        match = Suggestion.MACRO_REGEX.match(self.trace.lines[0])
        if match:
            return match.group(2)

    def _gen_renaming_macro(self, lines):
        """
        Input are semi-refactored lines. Refactor the macro call and add macro level title
        ASSUMPTION: Macro call is always on line 0
        """
        match = Suggestion.MACRO_REGEX.match(lines[0])
        if match:
            indent = match.group(1)
            macro = match.group(2)
            spacer = match.group(3)
            args = match.group(4)
            suggested_macro = Suggestion.MACRO_TRANSLATIONS.get(macro)
            if not suggested_macro:
                # Some unknown keyword
                warnings.append(f'Unknown macro name {macro} in {str(self)}')
                self.error = True
                recstats('unknown_macro')
            else:
                macro = suggested_macro
            if args.startswith('('):
                # We have args, add title and a ','
                args = args[0] + self.title + ', ' + args[1:]
            else:
                # No args, (could be FIN for example), just add title
                args = f'({self.title}){args}'

            lines[0] = indent + macro + spacer + args
            return lines

        # Some really ugly formatted print, should be here very rarely
        self.error = True
        # We could not parse the lines. So we just add a comment before
        lines.insert(0, f'Title: {self.title}')

        warnings.append(f'Could not parse macro name at {str(self)}')
        recstats('ugly_lines')

        return lines

    def _gen_remove_trailing_newline(self, lines):
        """
        Takes semi - refactored lines and removes the trailing newline in the format
        """
        for i in reversed(range(len(lines))):
            replaced = rreplace(lines[i], '\\n', '', 1)
            if replaced != lines[i]:
                lines[i] = replaced
                break
        return lines

    def _gen_fixed_lines(self):
        """
        Get the final lines as they should be after the suggested change is applied
        including error handling
        """

        if self.ignored_macro:
            return self.trace.lines

        lines = self._gen_fixed_lines_no_error()
        lines = self._gen_remove_trailing_newline(lines)
        lines = self._gen_renaming_macro(lines)

        if not self.error:
            return lines
        else:
            # In error mode add comment instead of actually fixing
            newlist = ['// __MARKER__: This comment is generated by an atomated script',
                       '// because it could not reliably parse the following trace.',
                       '// Suggested refactoring for the trace is:']
            for line in lines:
                # C does not like having \\ in the end of comment line
                newlist.append('//' + line.strip("\\"))
            newlist.extend(self.trace.lines)
            return newlist

    ###### PUBLIC ######

    def visualize(self):
        """
        Print the suggested fix in a colorful human readable form
        """
        print()
        cprint(f'{self.trace.filename}: line {self.trace.start}, error={self.error}', 'white', attrs=['bold'])
        cprint('============', 'white')
        for line in self.trace.lines:
            cprint('-' + line.replace('\t', '    '), 'red')
        cprint('============', 'white')

        for line in self.fixed_lines:
            if line.startswith('//'):
                cprint('+' + line.replace('\t', '    '), 'yellow')
            else:
                cprint('+' + line.replace('\t', '    '), 'green')
        cprint('============', 'white')
        print()


###### CLANG BINDINGS PARSING ######


def is_myfile(node, filename):
    return not node.location or not node.location.file or node.location.file.name == filename


def is_marker(node):
    return node.spelling == '__magic__marker__' and node.kind == CursorKind.CALL_EXPR


def is_func_def(node):
    return node.kind == CursorKind.FUNCTION_DECL


def parse_marker_node(node, filename, lines, scope):
    if VERBOSE:
        print(f'Found marker at span ({node.extent.start}, {node.extent.end})')

    if VERBOSE:
        dump_node(node)

    trace = Trace(node, filename, lines, scope)
    if REFACTORED in trace.strfmt:
        recstats('already_refactored')
        if VERBOSE:
            print('Already refactored, skipping')
        return

    # In nested macros, there be two separate traces for one line
    # We dont really care for the second one in that case, so keep just
    # the first one. Fix for HTR_STS
    if traces[filename] and traces[filename][-1].start == trace.start:
        warnings.append(f'Two traces at one line {str(trace)}). Not interesting.')
        recstats('two_traces_one_line')
        return

    traces[filename].append(trace)

    if VERBOSE:
        dump_trace(trace)


def parse_node(node, filename, lines, scope):
    if not is_myfile(node, filename):
        # Assumption: we don't care for h files. For now. Maybe later
        return

    if is_marker(node):
        parse_marker_node(node, filename, lines, scope)
        return

    if is_func_def(node):
        scope.append(node)

    for c in node.get_children():
        parse_node(c, filename, lines, scope)

    if is_func_def(node):
        scope.pop()


def create_new_lines(filename, lines):
    # Start by sorting all the suggestions by line order. Probably already sorted actually
    # but whatever. Performance not an issue for now
    fsuggestions = sorted(suggestions[filename], key=lambda s: s.trace.start)
    newlines = []
    traceid = 0
    for idx, line in enumerate(lines):
        # If writing problems only - skip all the resolved suggestions
        while PROBLEMS_ONLY and len(fsuggestions) > traceid and not fsuggestions[traceid].error:
            # Roll forward to next trace
            traceid += 1

        # Dont forget line numbers are 1 based and arrays are 0 based
        if len(fsuggestions) <= traceid or idx + 1 < fsuggestions[traceid].trace.start:
            # Before new code
            newlines.append(line)
            continue

        if idx + 1 == fsuggestions[traceid].trace.start:
            # Insert new code
            newlines.extend(fsuggestions[traceid].fixed_lines)

        if idx + 1 < fsuggestions[traceid].trace.end:
            # Skip to the line after new code
            continue

        # After new code
        while len(fsuggestions) > traceid and idx + 1 >= fsuggestions[traceid].trace.start:
            # Roll forward to next trace
            traceid += 1

    return newlines


def parse_file(filename):
    """
    Parse one specific single file
    """
    cprint(f'With {filename}...', 'cyan', attrs=['bold'])

    print (f'Compiling...')
    index = clang.cindex.Index.create()
    tu = index.parse(filename, CPP_ARGS_LIST)
    scope = [tu.cursor]

    print (f'Parsing...')
    lines = []
    with open(filename, 'r') as fp:
        lines = fp.read().split("\n")
    traces[filename] = []
    parse_node(tu.cursor, filename, lines, scope)

    print (f'Compute refactoring suggestions...')
    suggestions[filename] = []
    for trace in traces[filename]:
        suggestions[filename].append(Suggestion(trace))
        if VERBOSE:
            suggestions[filename][-1].visualize()

    print(f'Building new file...')
    newlines = create_new_lines(filename, lines)

    if WRITE:
        n, e = os.path.splitext(filename)
        print(f'Writing output to "{n + CONV_TOKEN + e}"...')
        with open(n + CONV_TOKEN + e, 'w') as outfile:
            outfile.write('\n'.join(newlines))


def sorted_dictionary(d):
    def extract_key(entry):
        OMIT = ['N_', 'START_', 'END_', 'NEW_']
        key = entry[0][1:]
        for w in OMIT:
            if key.startswith(w):
                key = key[len(w):]
        if entry[1].get('final'):
            key = '!' + key
        return key

    return OrderedDict(sorted([(k, v) for (k, v) in d.items()], key=extract_key))


def load_initial_dictionary():
    if isinstance(INITIAL_DICTIONARY_JSON, dict):
        return INITIAL_DICTIONARY_JSON
    with open(INITIAL_DICTIONARY_JSON, 'r') as infile:
        d = json.load(infile)
        return d


def load_initial_renaming_histogram(d):
    for (k, v) in d.items():
        # First character in key is @, we dont need it now
        renaming_histogram[k[1:]] = RenameHistogramEntry(k[1:], v)


def gen_dictionary_json():
    djn = {}
    for renaming, rhe in renaming_histogram.items():
        djn['@' + renaming] = rhe.render()
    with open(DICTIONARY_JSON, 'w') as outfile:
        json.dump(sorted_dictionary(djn), outfile, indent=4)


def main():
    """
    Main entry point
    """
    global terminate

    if ALL:
        global FILES
        FILES = [os.path.abspath(file) for file in subprocess.getoutput(
            'make print_src_list').split(' ') if '/common' in os.path.abspath(file)]

    # Init renaming dictionary
    djn = load_initial_dictionary()
    load_initial_renaming_histogram(djn)

    for filename in FILES:
        if os.path.basename(filename) in EXCLUDE_FILES:
            continue
        if terminate:
            break
        parse_file(chomp(filename))
        if len(traces) > MAX_FILES:
            break

    if WRITE:
        print(f'Generating {DICTIONARY_JSON}...')
        gen_dictionary_json()

    if PRINT_WARINGS:
        warnings.sort()
        for w in warnings:
            perror(w)

    if STATISTICS:
        statistics['(ABC)Algorithm Badassness Coefficient'] = 100 * \
            statistics.get('good_guess', 0) / (statistics.get('traces',
                                                              1) - statistics.get('already_refactored', 0))
        cprint('Statistics', 'yellow')
        cprint(json.dumps(statistics, indent=3), 'yellow')

    killer.cancel()

    return 0


if __name__ == '__main__':
    argrgx = re.compile(r'(.*)=(.*)')
    thismodule = sys.modules[__name__]
    for arg in sys.argv[1:]:
        match = argrgx.match(arg)
        if match:
            import ast
            setattr(thismodule, match.group(1),
                    ast.literal_eval(match.group(2)))
    exit(main())
