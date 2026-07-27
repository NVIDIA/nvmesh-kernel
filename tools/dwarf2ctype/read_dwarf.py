#!/usr/bin/env python3
import json
import re
import sys
import ctypes
from ctypes import ArgumentError
from dwarf2py_engine.dumb_text_parser_engine import DumbTextParserDwarf2PyEngine
from abc import abstractmethod, ABCMeta

ANON_GLOBAL_COUNTER = 1

class BasePyCType(object):
    """ Base class used to store information about an unresolved type, until
        we have enough info to fully resolve it.
    """
    __metaclass__ = ABCMeta

    def __init__(self, tid, name):
        """ Type is identified by tid (offset in DWARF header) and name (may
            be empty)
        """
        self.tid = tid
        if name:
            self.name = name
        else:
            global ANON_GLOBAL_COUNTER
            self.name = "anonymous_" + str(ANON_GLOBAL_COUNTER)
            ANON_GLOBAL_COUNTER += 1

        self._ctype = None

    @abstractmethod
    def _gen_ctype(self):
        """ Abstract method, generates and returns a c_type structure, based
            on PyC type info aggregated to this point.
            Private.
        """
        raise NotImplementedError()

    @abstractmethod
    def get_byte_size(self):
        """ Abstract method, each specific type must implement a method that
            resolves its size in bytes.
            Public.
        """
        raise NotImplementedError()

    @property
    def ctype(self):
        """ Cached property used to resolve ctype without regenerating each
            time
        """
        if self._ctype is None:
            self._ctype = self._gen_ctype()
        return self._ctype


# Primitive types
# @TODO: Extend with floating point
uint64 = type("uint64", (ctypes.c_ulong,), {})
int64 = type("int64", (ctypes.c_long,), {})
uint32 = type("uint32", (ctypes.c_uint,), {})
int32 = type("int32", (ctypes.c_int,), {})
uint16 = type("uint16", (ctypes.c_ushort, ), {})
int16 = type("int16", (ctypes.c_short,), {})
uint8 = type("uint8",  (ctypes.c_ubyte,), {})
int8 = type("int8",  (ctypes.c_byte,), {})


def select_primitive(byte_size, signess):
    """ Helper, select a primitive c type based on size and signess
        @TODO: Add support for float/double
    """
    if byte_size == 4:
        base = int32 if signess else uint32
    elif byte_size == 8:
        base = int64 if signess else uint64
    elif byte_size == 1:
        base = int8 if signess else uint8
    elif byte_size == 2:
        base = int16 if signess else uint16
    else:
        raise ArgumentError(
            "Invalid byte_size for primitive: {0}, signess: {1}".format(
                byte_size, signess))
    return base


class PyCTypePrimitiveInt(BasePyCType):
    """ Primitive type - 1 to 8 bytes integer
    """

    def __init__(self, tid, byte_size, signess):
        name = select_primitive(byte_size, signess).__name__
        super(PyCTypePrimitiveInt, self).__init__(tid, name)
        self.byte_size = byte_size
        self.signess = signess

    def _gen_ctype(self):
        return select_primitive(self.byte_size, self.signess)

    def get_byte_size(self):
        return self.size


class PyCTypeTypedef(BasePyCType):
    """ A typedef
    """

    def __init__(self, tid, name, orig):
        super(PyCTypeTypedef, self).__init__(tid, name)
        self.orig = orig

    def _gen_ctype(self):
        orig_ctype = self.orig.ctype
        new_type = None
        if hasattr(orig_ctype, "_length_") and  hasattr(orig_ctype, "_type_"):
            # For arrays there is a small problem - must define length
            new_type = type(self.name, (orig_ctype, ),
                {
                    "_length_": orig_ctype._length_,
                    "_type_": orig_ctype._type_,
                })
            pass
        else: # Else everythin is very simple
            new_type = type(self.name, (orig_ctype, ), {})
        return new_type

    def get_byte_size(self):
        return self.orig.get_byte_size()

class PyCTypeStr(BasePyCType):
    """ Pointer to another type
    """

    def __init__(self, tid):
        super(PyCTypeStr, self).__init__(tid, "string")

    def _gen_ctype(self):
        return ctypes.c_char_p

    def get_byte_size(self):
        return 8

class PyCTypeArray(BasePyCType):
    """ An array of given size of another type
    """

    def __init__(self, tid, orig, size):
        super(PyCTypeArray, self).__init__(
            tid, orig.name + "__arr__" + str(size))
        self.orig = orig
        self.size = size

    def _gen_ctype(self):
        return self.size * self.orig.ctype

    def get_byte_size(self):
        return self.size * self.orig.get_byte_size()


class PyCTypePtr(BasePyCType):
    """ Pointer to another type
    """

    def __init__(self, tid, orig):
        super(PyCTypePtr, self).__init__(tid, orig.name + "__ptr")
        self.orig = orig

    def _gen_ctype(self):
        return ctypes.POINTER(self.orig.ctype)

    def get_byte_size(self):
        return 8


class PyCTypeEnum(PyCTypeTypedef):
    """ Enum type - is a typedef of primitive
    """

    def __init__(self, tid, name, byte_size):
        super(PyCTypeEnum, self).__init__(
            tid, name,
            PyCTypePrimitiveInt(tid, byte_size, True))


class PyCTypeComplex(BasePyCType):
    """ Base abstract class for a complex type, may be either struct or union.
        Derived class must specify container (struct/union) and container_base
        (ctypes.Structure/Union)
    """

    def __init__(self, tid, name, byte_size):
        super(PyCTypeComplex, self).__init__(tid, name)
        self.fields = None
        self.byte_size = byte_size
        self._ctype_bak = None

    def set_fields(self, fields):
        if self.fields is not None:
            raise RuntimeError(
                "In type {0}: fields can only be set once".format(self.name))
        self._ctype_bak = self.ctype  # First generate type and save the results
        self.fields = fields  # Now apply fields
        self._ctype = None  # Force regen, now with fields

    def _gen_fields(self, fields):
        _fields = list()
        unnamed_idx = 0
        for f in fields:
            name = f.get("name")
            if not name:
                name = "unnamed_" + str(unnamed_idx)
                unnamed_idx += 1
            if f["bit_size"] != 0:
                _fields.append((name, f["type"].ctype, f["bit_size"]))
            else:
                _fields.append((name, f["type"].ctype))
        return _fields

    def _gen_ctype(self):
        if self.fields is None:
            return type(
                self.name,
                (self.container_base, ), {
                })
        else:
            # Start by declared myself as opaque
            self._ctype = self._ctype_bak
            # From this point, if someone requests my ctype, he will ge a prototype.

            # First ensure all children fields are populated
            self._fields = self._gen_fields(self.fields)

            # This is a hack, but I could not filnd a better way to determine if
            # the struct is packed. First, create an unpacked temporary struct.
            # If its size != known byte_size - create a packed one.
            tmp = type("tmp", (self.container_base, ),
                       {
                       "_pack_": 0,
                       "_fields_": self._fields
                       })
            if ctypes.sizeof(tmp) != self.byte_size:
                print(str(self.name) + " " + str(ctypes.sizeof(tmp)) +
                      "!=" + str(self.byte_size))
                self._ctype._pack_ = 1
            else:
                self._ctype._pack_ = 0

            # Finally, set the fields. From now on it is final.
            self._ctype._fields_ = self._fields
            return self._ctype

    def get_byte_size(self):
        return self.byte_size


class PyCTypeStruct(PyCTypeComplex):
    """ Structure, possibly opaque. Becomes final after set_fields.
    """
    container = "struct"
    container_base = ctypes.Structure

    def __init__(self, tid, name, byte_size):
        super(PyCTypeStruct, self).__init__(tid, name, byte_size)


class PyCTypeUnion(PyCTypeComplex):
    """ Union, possibly opaque. Becomes final after set_fields.
    """

    container = "union"
    container_base = ctypes.Union

    def __init__(self, tid, name, byte_size):
        super(PyCTypeUnion, self).__init__(tid, name, byte_size)

def extract_types(engine, filter=None):
    if type(filter) == str:
        filter = re.compile(filter)
    wipd = dict()
    enums = dict()
    for d in engine.all_tags({"DW_TAG_structure_type", "DW_TAG_typedef", "DW_TAG_enumeration_type"}):
        if not filter or filter.match(d.get("DW_AT_name", "")):
            _expand_wipd(engine, wipd, enums, d)
    return wipd


def _dict_first_value(d):
    values_view = d.values()
    value_iterator = iter(values_view)
    return next(value_iterator)


def _expand_wipd(engine, wipd, enums, d):
    tid = d["tid"]
    tag = d["tag"]
    wip = wipd.get(tid)
    if wip:
        return wip

    if tag == "DW_TAG_base_type":
        wip = PyCTypePrimitiveInt(
            tid,
            d["DW_AT_byte_size"],
            "(signed)" in d["DW_AT_encoding"]
        )
    elif tag == "DW_TAG_typedef":
        orig = _expand_wipd(engine, wipd, enums, engine.get_tid_dict(d["DW_AT_type"]))
        wip = PyCTypeTypedef(
            tid,
            d["DW_AT_name"],
            orig
        )
    elif tag == "DW_TAG_array_type":
        orig = _expand_wipd(engine, wipd, enums, engine.get_tid_dict(d["DW_AT_type"]))
        wip = PyCTypeArray(
            tid,
            orig,
            d["siblings"][0].get("DW_AT_upper_bound", -1) + 1  # Size
        )
    elif tag == "DW_TAG_pointer_type":
        if not d.get("DW_AT_type"):
            # Void ptr
            wip = PyCTypePtr(tid, PyCTypePrimitiveInt(tid, 1, True))
        else:
            orig = _expand_wipd(
                engine, wipd, enums, engine.get_tid_dict(d["DW_AT_type"]))
            if orig.ctype == uint8: # It is a string
                wip = PyCTypeStr(tid)
            else: # It is a pointer
                wip = PyCTypePtr(
                    tid,
                    orig
                )
    elif tag == "DW_TAG_enumeration_type":
        wip = PyCTypeEnum(
            tid,
            d.get("DW_AT_name", ""),  # Turns out unnamed enums exist in nature
            d["DW_AT_byte_size"]
        )
        for enumerator in d.get("siblings", []):
            enums[enumerator["DW_AT_name"]] = enumerator["DW_AT_const_value"]

    elif tag == "DW_TAG_structure_type" or tag == "DW_TAG_union_type":
        ctor = PyCTypeStruct if tag == "DW_TAG_structure_type" else PyCTypeUnion
        if d.get("DW_AT_declaration") == 1:
            # Opaque pointer. Treat as a generic pointer.
            # @TODO: Add opaque pointers resolution.
            wip = PyCTypePrimitiveInt(tid, 1, True)
        else:
            # Non opaque pointer
            wip = ctor(
                tid,
                ctor.container + "__" + d.get("DW_AT_name", ""),
                d["DW_AT_byte_size"]
            )
            wipd[tid] = wip
            fields = list()
            for sib in d["siblings"]:
                wip_sib = _expand_wipd(
                    engine, wipd, enums, engine.get_tid_dict(sib["DW_AT_type"]))
                fields.append({"name": sib.get("DW_AT_name", ""),
                               "type": wip_sib,
                               "bit_size": sib.get("DW_AT_bit_size", 0)})
            wip.set_fields(fields)
    elif tag == "DW_TAG_const_type" or tag == "DW_TAG_volatile_type":
        # Python side does not care for const or volatile - pass through
        if d.get("DW_AT_type"):
            wip = _expand_wipd(
                engine, wipd, enums, engine.get_tid_dict(d["DW_AT_type"]))
        else:
            # This case is for const void *, don't really care what type it is
            wip = PyCTypePrimitiveInt(tid, 1, True)
    elif tag == "DW_TAG_subroutine_type":
        # @TODO: For now func ptr is treated as a byte ptr. Add full support.
        wip = PyCTypePtr(tid, PyCTypePrimitiveInt(tid, 1, True))
    else:
        raise RuntimeError("Unsupported tag: {0}".format(tag))

    wipd[tid] = wip
    return wip


class PyCFunc(object):
    """ Base class for callable C functions with additional dbg info
    """
    def __init__(self, name, arglist, restype):
        self.name = name
        self.arglist = arglist
        self.restype = restype
        self.restypestr = None

    def __call__(self, *args):
        raise NotImplemented("Trying to call base PyCFunc class")

    @staticmethod
    def gen_doc(restypestr, restype, name, arglist):
        return "{restype} {name} ({arglist})".format(
                restype = restypestr if restypestr else restype.name if restype else "<undef>",
                name = name if name else "<anon>",
                arglist = ", ".join(
                    "{1} {0}".format(arg["name"], arg["type"].name
                    ) for arg in arglist) if arglist is not None else "<undef>",
        )
    @property
    def __doc__(self):
        return PyCFunc.gen_doc(self.restypestr, self.restype, self.name, self.arglist)

class PyCFuncNative(PyCFunc):
    def __init__(self, call, name, arglist = None, restype = None):
        super(PyCFuncNative, self).__init__(name, arglist, restype)
        self.call = call
        if restype is not None:
            call.restype = restype.ctype
        if arglist is not None:
            call.argtypes = [arg["type"].ctype for arg in arglist]

    @property
    def __doc__(self):
        return super(PyCFuncNative, self).__doc__

    def __call__(self, *args):
        return self.call(*args)

def _expand_funcs(engine, types, enums, funcs, d, dll):
    name = d["DW_AT_name"]
    call = None
    try:
        call = getattr(dll, name) if dll else None
    except AttributeError as e:
        # To avoid spamming with error messages, assume static functions end with _ or begin with __
        if not name.endswith('_') and not name.startswith('__'):
            raise
        return None

    restid = d.get("DW_AT_type")
    restype = _expand_wipd(engine, types, enums, engine.get_tid_dict(restid)) if restid else None
    arglist = [{
        "name": arg["DW_AT_name"],
        "type": _expand_wipd(engine, types, enums, engine.get_tid_dict(arg["DW_AT_type"]))
        }
        for arg in d.get("siblings", [])]
    funcs[name] = PyCFuncNative(call, name, arglist, restype)
    return funcs[name]

def extract_funcs_first_full_interface(engine, types = None, enums = None, dll=None, filter=None):
    if not types:
        types = dict()
    if not enums:
        enums = dict()
    if not dll:
        dll = engine.get_dll()
    if type(filter) == str:
        filter = re.compile(filter)
    funcs = dict()
    for d in engine.all_tags("DW_TAG_subprogram"):
        if not filter or filter.match(d.get("DW_AT_name", "")):
            _expand_funcs(engine, types, enums, funcs, d, dll)
    return funcs, types, enums, dll

class Bunch:
    def __init__(self, **kwds):
        self.as_dict = dict()
        self.as_dict.update(kwds)
        self.__dict__.update(kwds)

    def _bunch_update(self, **kwds):
        self.as_dict.update(kwds)
        self.__dict__.update(kwds)

    def _bunch_get(self, val):
        self.__dict__.get(val)

    def __str__(self):
        return str({k: str(vars(self)[k]) for k in vars(self)})

    def __repr__(self):
        return str(self)


def extract_funcs_first_full_interface_as_bunch(engine, types = None, enums = None, dll=None, filter=None):
    funcs, types, enums, dll = extract_funcs_first_full_interface(engine, types, enums, dll, filter)
    return Bunch(**funcs), Bunch(**{t.name:t for t in types.values()}), Bunch(**enums), dll

def main():
    import IPython

    engine = DumbTextParserDwarf2PyEngine("/home/yuranu/projects/nvmesh/core_unitest/libcorecomm.so")

    funcs, types, enums, dll = extract_funcs_first_full_interface_as_bunch(engine)

    IPython.embed()

    return 0


if __name__ == "__main__":
    sys.exit(main())
