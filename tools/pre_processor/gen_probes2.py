#!/usr/bin/python3
""" This is a refactored and improved version of the original gen_trace_probes
"""

import argparse
import re
import sys
import json
import dictionary_tools
import os
import itertools
from collections import OrderedDict

MAX_ARGS = 23
FIRST_TRACE_ID = 256


def perror(msg):
    print >> sys.stderr, msg


class CSourceWriter:
    """ Utility class used to generate a nice looking C code.
        Keep it simple, no crazy corner cases.
    """

    def __init__(self, indent_symbol='\t'):
        self.indent = 0
        self.indent_symbol = '\t'
        self.lines = []
        self.nl = True

    def skipline(self, count=1):
        self.lines.append('\n'*count)
        self.nl = True

    def write(self, data, ln=False):
        lines = data.splitlines(True)
        for line in lines:
            if line.startswith('}'):
                self.indent -= 1
            if self.nl:
                self.lines.append(self._indent_str())
                self.nl = False
            self.lines.append(line)
            if line.startswith('}'):
                self.indent += (line.count('{') - line.count('}') + 1)
            else:
                self.indent += (line.count('{') - line.count('}'))
            if line[-1] == '\n':
                self.nl = True
        if ln:
            self.lines.append('\n')
            self.nl = True

    def writeln(self, data):
        self.write(data, True)

    def jumbo(self, heading):
        self.skipline()
        self.writeln('/********* {0} *********/'.format(heading))
        self.skipline()

    def _indent_str(self):
        return self.indent_symbol * self.indent

##### CUSTOM EXCEPTIONS #####


class UnknownToken(Exception):
    def __init__(self, token):
        self.token = token
        super(Exception, self).__init__('Unknown token `{0}`'.format(self.token))


class UnknownArgType(Exception):
    def __init__(self, sz, tp):
        self.sz = sz
        self.tp = tp
        super(Exception, self).__init__('Unknown argument type `{0}` of size `{1}`'.format(self.tp, self.sz))


class TraceException(Exception):
    def __init__(self, trace, *args, **kwargs):
        self.trace = trace
        super(Exception, self).__init__(*args, **kwargs)

    def __str__(self):
        if self.trace:
            return 'Binary tracer in {0}:{1} - {2}'.format(self.trace.file, self.trace.line, super(Exception, self).__str__())
        else:
            return super(Exception, self).__str__()


##### CORE CLASSES #####

class Dictionary:
    """ Represents the binary tracer full ditionary
    """

    def __init__(self, path):
        d = dictionary_tools.read_dictionary_and_verify(path)
        self.cksum = d['cksum']
        self.traces = d['traces']
        self.tokens = d['dictionary']


class Ctx:
    """ Represents a full context of probes generation run
    """

    def __init__(self, module, full_dict, userspace, base_includes):
        self.userspace = userspace
        self.module = module
        self.structs = []
        self.traces = OrderedDict()
        self.full_dict = full_dict
        self.tokens = OrderedDict()
        self.argtypes = OrderedDict()
        self.typedefs = OrderedDict()
        self.base_includes = base_includes

    def token(self, token_name):
        """ Lazy getter for processed Token class objects
        """
        return Token.instance(self, token_name)

    def argtype(self, size, tp, is_bitfield, fmt):
        """ Lazy getter for ArgType class objects
        """
        return ArgType.instance(self, size, tp, is_bitfield, fmt)

    def register_trace(self, trace):
        """ Register trace object within the context
        """
        other = self.traces.get(trace.name)
        if other:
            if other.file != trace.file or abs(other.line - trace.line) > 2:
                raise TraceException(trace, 'Duplicate trace, previous defined in {0}:{1}'.format(other.file, other.line))
        else:
            self.traces[trace.name] = trace

    def render(self, caption='GEN_EVENTS_H'):
        """ Render the gen_events file described by given context into and open file descriptor
        """
        writer = CSourceWriter()

        # Header
        writer.writeln('#ifndef {0}\n#define {0}'.format(caption))
        writer.skipline(2)

        self.__render_header(writer)

        # Typedefs
        writer.jumbo('Typedefs')
        for typedef, _ in self.typedefs.items():
            writer.writeln(typedef)

        # Traces
        writer.jumbo('Traces')
        for trace in self.traces.values():
            self.__render_single_trace(trace, writer)

        writer.jumbo('Formats')

        # Footer
        writer.skipline(2)
        writer.writeln('#endif /*{0}*/'.format(caption))
        writer.skipline()

        return writer.lines

    def __render_header(self, writer):
        for line in self.render_traces_ids_h_file():
            writer.write(line)
        writer.jumbo('Header')
        if self.userspace:
            writer.writeln('#define _NVMEIB_TRACE_BACKEND_USER')
        else:
            writer.writeln('#define _NVMEIB_TRACE_BACKEND_KERNEL')
        # Toma requires some more declarations first
        # TODO: It is ugly bullshit. Do something about it.

        if self.base_includes and self.module in ['nvmeibc', 'nvmeibs', 'nvmeibm', 'nvmeibp']:
            writer.writeln('''
#include <kr_incs.h>
#include "nvmeib.h"
#include "{}_trace.h"
            '''.format(self.module))

        if self.module == 'nvmeibt':
            writer.writeln('''
#include <errno.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>
#include <stdio.h>
#include "interfaces/log/nvmeibt_binary_tracing.h"
#include "../tools/nvmeib_trace_userspace.h"
#include "../tools/nvmeib_trace_userspace_poller.h"

typedef uint8_t u8;
typedef int8_t s8;
typedef uint16_t u16;
typedef int16_t s16;
typedef uint32_t u32;
typedef int32_t s32;
typedef long long unsigned int u64;
typedef long long int s64;

''')

        if self.module == 'nvmeshum':
            writer.writeln('''
#include <errno.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>
#include <stdio.h>
#include "nvmeshum_trace.h"

typedef uint8_t u8;
typedef int8_t s8;
typedef uint16_t u16;
typedef int16_t s16;
typedef uint32_t u32;
typedef int32_t s32;
typedef long long unsigned int u64;
typedef long long int s64;

''')

    def __render_single_trace(self, trace, writer):
        # Prototype
        writer.write('static inline void trace_{0}_{1}_{2}('.format(self.module, trace.channel, trace.name))
        if not trace.proto_args:
            writer.write('void')
        else:
            writer.write(', '.join(arg.proto_type + ' ' + arg.name for arg in trace.proto_args))
        if trace.printf_arg:
            writer.write(', ...')
        elif trace.vprintf_arg:
            writer.write(', va_list args')
        writer.writeln(') {')

        # Typedef
        writer.writeln('struct __attribute((packed)) {0} {{'.format(trace.struct))
        writer.writeln('short ____traceid:16;')
        writer.writeln('\n'.join(arg.struct_field_def for arg in trace.fixed_len_args))
        if trace.is_bitfield:
            writer.writeln(trace.render_aligner)
        writer.writeln('};')

        # Log level
        writer.writeln('extern int {0};\nif ({0} >= {1}) {{'.format(trace.scope, trace.lvl))

        # Auto errno
        if trace.auto_errno:
            writer.writeln('const u32 {0} = errno;'.format(trace.auto_errno.name))
        if trace.printf_arg or trace.vprintf_arg:
            writer.writeln('va_list arg_ptr;')

        # Body
        # Va-len args - length calculations
        writer.writeln('char *ptr;')
        if trace.va_len_args:
            writer.write('size_t ')
            writer.write(', '.join(arg.len_func_call for arg in trace.va_len_args if not arg.token.argtype.key.endswith('printf_arg')))
            writer.writeln(';')

        if trace.printf_arg:
            writer.writeln("size_t {} ;".format(trace.printf_arg.len_func_call))
        elif trace.vprintf_arg:
            writer.writeln("size_t {} ;".format(trace.vprintf_arg.len_func_call))

        self.__render_start_write_trace(trace, writer)

        # Write fixed length args
        writer.writeln('*(struct {0}*)ptr = (struct {0}) {{'.format(trace.struct))
        writer.writeln('id_' + trace.name + ',')
        writer.writeln(',\n'.join(arg.struct_assign for arg in trace.fixed_len_args))
        if trace.is_bitfield:
            writer.writeln(trace.render_aligner_init)
        writer.writeln('};')
        writer.writeln('ptr+=sizeof(struct {0});'.format(trace.struct))

        # Write va-length args
        writer.writeln('\n'.join('{0};\nptr+={1};'.format(arg.cpy_func, arg.len_var) for arg in trace.va_len_args))

        self.__render_end_write_trace(trace, writer)

        writer.writeln('}')
        writer.writeln('}')
        writer.writeln('#define ___trace_fmt_{0} "{1}"'.format(trace.name, trace.fmt))

    def _get_channel_var_userspace(self, trace):
        fmt = "{}_trace_{}".format(self.module, trace.channel.lower())
        if self.module == 'nvmeshum':
            return "RTE_PER_LCORE({})".format(fmt)
        return fmt

    def __render_start_write_trace(self, trace, writer):
        if self.userspace:
            writer.writeln('ptr = (char *) nvmeib_start_trace_write({1}, {0}, NVMEIB_DICTIONARY_CKSUM);\nif (!ptr) {{ if (!nvmeib_trace_is_terminated({1}))\n\tfprintf(stderr, "trace {2} too long\\n");\n return; }}'.format(
                trace.size_formula, self._get_channel_var_userspace(trace), trace.name))
        else:
            writer.writeln('ptr = (char *) nvmeib_add_trace({0}, get_cpu_var({1}_trace_{2}_percpu), NVMEIB_DICTIONARY_CKSUM);\nif (!ptr) {{ put_cpu_var({1}_trace_{2}_percpu); return; }}'.format(
                trace.size_formula, self.module, trace.channel.lower()))

    def __render_end_write_trace(self, trace, writer):
        if self.userspace:
            writer.writeln('nvmeib_finish_trace_write({0});'.format(self._get_channel_var_userspace(trace)))
        else:
            writer.writeln('put_cpu_var({0}_trace_{1}_percpu);'.format(self.module, trace.channel.lower()))

    @staticmethod
    def _trace_id_variable(trace):
        return "id_{}".format(trace.name)

    def render_traces_ids_c_file(self):
        writer = CSourceWriter()
        for line in self.render_traces_ids_h_file():
            writer.write(line)
        writer.writeln('unsigned int NVMEIB_DICTIONARY_CKSUM = {0};'.format(self.full_dict.cksum))
        for trace in self.traces.values():
            writer.writeln("short {} = {};".format(self._trace_id_variable(trace), trace.tid))
        return writer.lines

    def render_traces_ids_h_file(self):
        writer = CSourceWriter()
        writer.writeln("extern unsigned int NVMEIB_DICTIONARY_CKSUM;".format(self.full_dict.cksum))
        for trace in self.traces.values():
            writer.writeln("extern short {};".format(self._trace_id_variable(trace)))
        return writer.lines


class ArgType:
    """ Represent a type of a single argument in the generated C code
    """

    def is_type_str(self):
        return (isinstance(self.key, str) and self.key.startswith('string'))

    def __init__(self, key, fmt):
        self.typedef = None  # No typedef is needed unless otherwise specified
        self.key = key
        if self.key in {'symbol', 'stack_trace', 'printf_arg', 'vprintf_arg'} or self.is_type_str():
            self.is_va_len = True  # Symbols and strings length is known only at runtime. Others known in compile time
        else:
            self.is_va_len = False

        if self.is_type_str():  # Va length strings length is defined by strlen function, and they are copied using memcpy
            self.proto_type = 'const char*'
            match = re.match(r'\%-?.(\d+)s', fmt)
            if match:
                limit = match.group(1)
                self.len_func = lambda arg, limit=limit: '{0}?(strnlen({0}, {1}) + 1):sizeof("(null)")'.format(arg, limit)
                self.cpy_func = lambda dst, arg, l: 'if ({1}) {{ memcpy({0}, {1}, {2} - 1); {0}[{2} - 1] = 0;}} \nelse memcpy({0}, "(null)", {2});'.format(dst, arg, l)
            else:
                self.len_func = lambda arg: '{0}?(strlen({0}) + 1):sizeof("(null)")'.format(arg)
                self.cpy_func = lambda dst, arg, l: 'if ({1}) memcpy({0}, {1}, {2});\nelse memcpy({0}, "(null)", {2});'.format(dst, arg, l)

        elif self.key == 'symbol':  # Symbols are almost like strings, but with a little different length and translation functions
            self.proto_type = 'void*'
            self.len_func = lambda arg: '{0}?((size_t)nvmeib_symbol_length({0}) + 1):sizeof("(null)")'.format(arg)
            self.cpy_func = lambda dst, arg, l: 'if ({1}) nvmeib_symbol_strcpy({0}, {1}, {2});\nelse memcpy({0}, "(null)", {2});'.format(dst, arg, l)

        elif self.key == 'stack_trace':  # Calltraces are mostly like symbols. TODO: merge the two
            self.proto_type = 'void*'
            self.len_func = lambda arg: '{0}?((size_t)nvmeib_stack_trace_length({0}) + 1):sizeof("(null)")'.format(arg)
            self.cpy_func = lambda dst, arg, l: 'if ({1}) nvmeib_stack_trace_strcpy({0}, {1}, {2});\nelse memcpy({0}, "(null)", {2});'.format(dst, arg, l)

        elif self.key == 'printf_arg':
            self.proto_type = 'void*'
            self.len_func = lambda arg: ' 0; va_start(arg_ptr, {0}); {0}__len = vsnprintf(NULL, 0, {0}, arg_ptr) + 1; va_end(arg_ptr);'.format(arg)
            self.cpy_func = lambda dst, arg, l: 'va_start(arg_ptr, {1}); vsnprintf({0}, {2}, {1}, arg_ptr); va_end(arg_ptr);'.format(dst, arg, l)

        elif self.key == 'vprintf_arg':
            self.proto_type = 'const char*'
            self.len_func = lambda arg: ' 0; va_copy(arg_ptr, args); {0}__len = vsnprintf(NULL, 0, {0}, args) + 1;'.format(arg)
            self.cpy_func = lambda dst, arg, l: 'vsnprintf({0}, {2}, {1}, arg_ptr); va_end(arg_ptr);'.format(dst, arg, l)

        elif self.key in {'u8', 'u16', 'u32', 'u64', 'void*', 'float', 'double'}:  # Simple flat types
            self.proto_type = self.struct_type = self.key
            if self.proto_type == 'void*':
                self.proto_type = self.struct_type = 'const void*'
            self.cast = lambda arg: str(arg)  # No casting needed for flat types

        else:  # Compile time known size memory buffers
            self.proto_type = 'const void*'
            self.struct_type = 'struct __binary_tracer_const_buf_{0}'.format(self.key)
            self.typedef = '{0} {{ char _[{1}]; }};'.format(self.struct_type, self.key)
            # Translates to: (x ? (*( (struct X*)x )) : ( (struct X) {{0}} ))
            self.cast = lambda arg: '({1}?(*(({0}*) {1})):(({0}) {{{{0}}}}))'.format(self.struct_type, arg)

    @staticmethod
    def instance(ctx, size, tp, is_bitfield, fmt):
        if tp == 'string' or tp == 'symbol' or tp == 'stack_trace' or tp=='printf_arg' or tp == 'vprintf_arg':
            if tp == 'string':
                key = "{}_{}".format(tp, fmt)
            else:
                key = tp    # Va-len data
        elif size == 64 and '*' in tp:
            key = 'void*'  # Flat pointer
        elif tp == 'double' or tp == 'float':
            key = 'double'
        elif type(size) == int:  # Known length data
            if tp in {'uuid', 'uuid_le', 'ipv6', 'bitmap', 'hex', 'string_n'}:
                key = ((size + 7) // 8)  # Key is the size of the buffer in bytes
            elif is_bitfield:  # For bitfields use 64 bits *always, as it causes compilation errors if not*
                key = 'u64'
            elif size <= 8:
                key = 'u8'
            elif size <= 16:
                key = 'u16'
            elif size <= 32:
                key = 'u32'
            elif size <= 64:
                key = 'u64'
            else:
                raise Exception('Cannot parse type {0} size {1}'.format(tp, size))
        else:
            raise UnknownArgType(size, tp)
        argtype = ctx.argtypes.get(key)
        if not argtype:
            argtype = ArgType(key, fmt)
            if argtype.typedef:
                ctx.typedefs[argtype.typedef] = 0
            ctx.argtypes[key] = argtype
        return argtype


def _extract_tokens_from_fmt(fmt):
    """ Given fmt string, extract a list of tokens from it
    """
    return (token for token in re.findall(r"(\\@|@[A-Z0-9_]+)", fmt) if token != "\\@")


class Token:
    """ Represents a single token in the dictionary
    """

    def __init__(self, ctx, token_name, token_dict):
        self.name = token_name
        if not token_dict.get('composite'):  # A simple token
            self.size = token_dict['size']
            self.is_bitfield = token_dict.get('is_bitfield', False)
            self.is_primitive = type(self.size) == int and self.size <= 64
            self.fmt = token_dict.get('dmesg_fmt', token_dict['fmt'])
            self.argtype = ctx.argtype(self.size, token_dict['type'], self.is_bitfield, self.fmt)
            self.expanded = [self]
        else:  # A composite token
            self.fmt = token_dict['fmt']
            self.subtokens = (self.name + '.' + sub[1:] for sub in _extract_tokens_from_fmt(self.fmt))
            self.expanded = [Token.instance(ctx, sub) for sub in self.subtokens]

    @staticmethod
    def instance(ctx, token_name):
        token = ctx.tokens.get(token_name)
        if not token:
            token_dict = ctx.full_dict.tokens.get(token_name)
            if not token_dict:
                raise UnknownToken(token_name)
            token = Token(ctx, token_name, token_dict)
            ctx.tokens[token_name] = token
        return token


class Arg:
    """ Connecting link between trace and token
    """

    def __init__(self, name, trace, token):
        self.name = name
        self.trace = trace
        self.token = token

    @property
    def proto_type(self):
        if self.token.name != '@AUTO_ERRNO':
            return self.token.argtype.proto_type

    @property
    def len_var(self):
        if self.token.argtype.is_va_len:
            return self.name + '__len'
        else:
            return 'sizeof({0})'.format(self.name)

    @property
    def len_func_call(self):
        return self.len_var + ' = ' + self.token.argtype.len_func(self.name)

    @property
    def cpy_func(self):
        return self.token.argtype.cpy_func('ptr', self.name, self.len_var)

    @property
    def struct_field_def(self):
        return '{0} {1}{2};'.format(self.token.argtype.struct_type, self.name, self.bitlen)

    @property
    def struct_assign(self):
        return self.token.argtype.cast(self.name)

    @property
    def bitlen(self):
        if self.trace.is_bitfield:
            return ': ' + str(self.token.size)
        else:
            return ''


class Trace:
    """ Represents a single trace
    """

    LOG_LEVEL_HELPER = {
        '_E': 1,
        '_W': 2,
        '_IMf': 2,
        '_I': 3,
        '_If': 3,
        '_T': 4,
        '_DBG': 5,
        '_F': 6,
        '_Ef': 1,
        '_ETf': 1,
        '_Wf': 2,
        '_WTf': 2,
        '_Df': 5,
        '_Tf': 4,
        '_CTf': 4,
    }

    def __init__(self, ctx, tid, trace):
        self.ctx = ctx
        self.tid = tid
        self.file = trace['file']
        self.line = trace['line']

        try:
            self.name = trace['name']
            self.channel = trace['type']
            self.fmt = repr(trace['fmt'].encode('ascii'))[2:-1].replace(r'"', r'\"')	# Skip the b' prefix

            self.macro = trace.get('macro')
            if not self.macro:
                self.macro = '_Tf' if ctx.module == 'nvmeibt' else '_T'
            self.lvl = Trace.LOG_LEVEL_HELPER[self.macro] if not trace.get('forced', False) else 0

            self.scope = trace.get('scope')
            if not self.scope:
                self.scope = 'tracer_' + ctx.module
            self.scope += '_debug_level'

            self.struct = '__tracedata_' + self.name + '__struct'

            self.__create_args()
        except ValueError:
            raise
        except Exception as e:
            raise TraceException(self, str(type(e)) + ' ' + str(e))

    def __get_trace_tokens(self):
        token_names = _extract_tokens_from_fmt(self.fmt)
        tokens = itertools.chain.from_iterable(self.ctx.token(token).expanded for token in token_names)
        return tokens

    def __create_args(self):
        tokens = self.__get_trace_tokens()
        self.args = [Arg(self.__argname(token.name, num), self, token) for num, token in enumerate(tokens)]

        if len(self.args) > MAX_ARGS:
            raise Exception("{2}: Too many arguments, maximum allowed {0}, got {1}-- {3}".format(
                MAX_ARGS, len(self.args), self.name, str([a.name for a in self.args])))

        self.proto_args = [arg for arg in self.args if arg.proto_type]

        self.va_len_args = [arg for arg in self.args if arg.token.argtype.is_va_len]
        self.fixed_len_args = [arg for arg in self.args if not arg.token.argtype.is_va_len]

        self.size_formula = '4 + ' + 'sizeof(struct {0})'.format(self.struct)
        va_len_formula = ' + '.join(arg.len_var for arg in self.va_len_args)
        if va_len_formula:
            self.size_formula += ' + ' + va_len_formula

        self.auto_errno = next((arg for arg in self.args if arg.token.name == '@AUTO_ERRNO'), False)
        self.printf_arg = next((arg for arg in self.args if arg.token.argtype.key == 'printf_arg'), None)
        self.vprintf_arg = next((arg for arg in self.args if arg.token.argtype.key == 'vprintf_arg'), None)

        self.is_bitfield = any(arg.token.is_bitfield for arg in self.args)
        if self.is_bitfield and not all(arg.token.is_bitfield or arg.token.is_primitive for arg in self.args):
            raise TraceException(self, 'Cannot mix bitfields with non complex types')
        if self.is_bitfield:
            self.trace_bit_size = sum(arg.token.size for arg in self.args) + 16  # 16 for short trace_id
            self.trace_byte_size = (self.trace_bit_size + 7) // 8
            self.aligner_size = self.trace_byte_size * 8 - self.trace_bit_size
            self.render_aligner = 'u64 ____aligner:{0};'.format(self.aligner_size) if self.aligner_size else ''
            self.render_aligner_init = ',0' if self.aligner_size else ''

        for arg in self.args:
            self.fmt = self.fmt.replace(arg.token.name, arg.token.fmt, 1)

    @staticmethod
    def __argname(name, num):
        return name[1:].lower().replace('.', '__') + '_' + str(num)


def init_argparser():
    parser = argparse.ArgumentParser('gen_probes2')

    parser.add_argument(dest='compiled_dict_path', action='store', type=str, help="The full path to *.trace.json file")

    parser.add_argument(dest='module', action='store', type=str, help="Name of the module processed")

    parser.add_argument('-o', '--output', dest='output_file_name', action='store',
                        type=str, default='./gen_events.h', help="Output file name")

    parser.add_argument('-u', '--userspace', dest='is_userspace', action='store_true')

    parser.add_argument('-v', '--verbose', dest='verbose', action='store_true')
    parser.add_argument('--only_trace_ids', default=False, dest='only_trace_ids', action='store_true')
    parser.add_argument('--no_base_includes', default=False, dest='no_base_includes', action='store_true')

    return parser


def main():
    args = init_argparser().parse_args()
    d = Dictionary(args.compiled_dict_path)
    ctx = Ctx(args.module, d, args.is_userspace, not args.no_base_includes)
    for idx, trace in enumerate(ctx.full_dict.traces):
        trace = Trace(ctx, FIRST_TRACE_ID + idx, trace)
        ctx.register_trace(trace)

    if not args.only_trace_ids:
        with open(args.output_file_name, 'w') as fd:
            fd.writelines(ctx.render())

    with open(os.path.join(os.path.dirname(args.output_file_name), "traces_ids.c"), 'w') as fd:
        fd.writelines(ctx.render_traces_ids_c_file())

    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (TraceException, ValueError) as e:
        perror("ERROR: " + str(e))
        sys.exit(1)

