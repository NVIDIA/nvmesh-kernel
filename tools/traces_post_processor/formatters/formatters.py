#!/usr/bin/python3

import argparse
import ctypes
import os
from subprocess import PIPE, Popen

from IPython import embed
from IPython.terminal.prompts import Prompts, Token
from traitlets.config import Config

BUF_SIZE = 4096
BUNDLE_MODE = hasattr(sys, '_MEIPASS')


def loaddll(dllname: str) -> ctypes.CDLL:
    out = Popen(
        args=f"nm {dllname}",
        shell=True,
        stdout=PIPE
    ).communicate()[0].decode("utf-8")
    attrs = [
        i.split(" ")[-1].replace("\r", "")
        for i in out.split("\n") if " T " in i
    ]
    dll = ctypes.CDLL(dllname)
    return dll, [i for i in attrs if hasattr(dll, i)]


def invoke_formatter(fmtso: str, func: str, val) -> str:
    outbuf = ctypes.create_string_buffer(BUF_SIZE)
    outbuf_ptr = ctypes.POINTER(ctypes.c_int8)(outbuf)
    length = ctypes.c_int(BUF_SIZE)
    if type(val) == int:
        datalen = ctypes.c_int(4)
        arg = ctypes.c_long(val)
    elif type(val) == str:
        bytesval = bytearray.fromhex(val)
        chararr = ctypes.c_char * len(bytesval)
        datalen = ctypes.c_int(len(bytesval))
        arg = chararr.from_buffer(bytesval)
    getattr(fmtso, func)(outbuf_ptr, length, arg, datalen)
    return outbuf.value.decode("utf-8")


class Formatter:
    def __init__(self, fmtso, exports):
        self.fmtso = fmtso
        self.exports = exports
        for func in exports:
            setattr(self, func, lambda val, func=func: invoke_formatter(
                self.fmtso, func, val))


def get_fmtlib_default_location():
    _lib_name = "libfmtrs.so"
    if BUNDLE_MODE:
        return os.path.join(sys._MEIPASS, _lib_name)
    return os.path.join(os.path.dirname(os.path.realpath(__file__)), _lib_name)

def init_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        "Formatters", formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    parser.add_argument("--fmtlib", dest="fmtlib", action="store", type=str,
                        default=get_fmtlib_default_location(),
                        help="Formatters dynamic library full path")

    return parser


def embed_shell(f: Formatter):
    class MyPrompt(Prompts):
        def in_prompt_tokens(self, cli=None):
            return [(Token.Prompt, ">>> ")]

    print(f"""
Imported {f.exports}
Use the object `f` to invoke formatter functions
Format functions receive 1 argument, either ineger or string.
Integer argument is treated as is.
String argument is treated as hexadecimal representation of bytes buffer.
EXAMPLE:
    >>> f.fmt_nvmeib_block_io_op(2)
    Out[1]: "NVMEIB_BLOCK_IO_OP_WRITE"

""")

    cfg = Config()
    cfg.InteractiveShell.colors = "linux"
    cfg.TerminalInteractiveShell.prompts_class = MyPrompt
    cfg.InteractiveShell.confirm_exit = False
    cfg.TerminalIPythonApp.display_banner = False

    embed(using=False, config=cfg)


def main():
    parser = init_parser()
    args = parser.parse_args()
    f = Formatter(*loaddll(args.fmtlib))

    embed_shell(f)


if __name__ == "__main__":
    main()
