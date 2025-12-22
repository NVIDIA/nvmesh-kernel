#!/usr/bin/env python3

import os
import abc
import ctypes
import typing
import pathlib
import argparse
import datetime
from elftools.elf.elffile import ELFFile
from nvmeib_pet_archive import NvmeibPetArchive


libc = ctypes.cdll.LoadLibrary("libc.so.6")

sprintf_buffer = ctypes.create_string_buffer(8192)


class Message(typing.NamedTuple):
    entity: int
    ns_stamp: int
    dt_stamp: datetime.datetime
    text: str

def pet_variant_get_value_attr_name(self: NvmeibPetArchive.PetVariant) -> str:
    for vname in ('sv1', 'uv1', 'sv2', 'uv2', 'sv4', 'uv4', 'sv8', 'uv8'):
        value = getattr(self, vname, None)
        if value is not None:
            return vname
    raise RuntimeError(f"Failed to extract value from the variant: {self}")

def pet_variant_get_value(self: NvmeibPetArchive.PetVariant) -> int:
    for vname in ('sv1', 'uv1', 'sv2', 'uv2', 'sv4', 'uv4', 'sv8', 'uv8'):
        value = getattr(self, vname, None)
        if value is not None:
            return value
    raise RuntimeError(f"Failed to extract value from the variant: {self}")


NvmeibPetArchive.PetVariant.pet_value = property(pet_variant_get_value) # type: ignore
NvmeibPetArchive.PetVariant.pet_value_attr_name = property(pet_variant_get_value_attr_name) # type: ignore

class Template:
    def __init__(self, offset: int, msg: bytearray):
        self.__offset = offset
        try:
            self.__spec = msg.decode("utf-8").rstrip()
        except UnicodeError:
            self.__spec = ""

    def __bool__(self):
        return bool(self.__spec)

    @property
    def offset(self) -> int:
        return self.__offset

    @property
    def spec(self) -> str:
        return self.__spec

    @property
    def as_line(self):
        return f"{self.offset:#0{4}x} {self.spec}"

    def instantiate(self, msg:NvmeibPetArchive.Message, entity:int, prev_ns_stemp: int) -> Message:
        args: list[int] = []
        for arg in msg.args: # type: ignore
            args.append(arg.pet_value) # type: ignore
        rc = libc.sprintf(sprintf_buffer, self.spec.encode('utf-8'), *args)
        if rc < 0:
            raise RuntimeError(f"Failed to create human message; spec={self}, args={msg}, entity={entity}")
        text = sprintf_buffer.value.decode("utf-8", errors="replace")
        
        timestamp:int = msg.timestamp.pet_value # type: ignore
        if msg.timestamp.pet_value_attr_name != 'uv8': # type: ignore
            timestamp += prev_ns_stemp # type: ignore

        dt_stamp = datetime.datetime.fromtimestamp(timestamp/(10**9)) # type: ignore
        return Message(entity=entity, ns_stamp=timestamp, dt_stamp=dt_stamp, text=text)


class TemplatesLoader:
    def __init__(self, module: pathlib.Path, section_name: str):
        self.module = module
        self.section_name = section_name

    @typing.no_type_check
    def load_templates(self) -> bytes:
        with open(self.module, "rb") as fobj:
            elf = ELFFile(fobj)
            section: Section = elf.get_section_by_name(self.section_name)
            if not section:
                raise ValueError(f"{self.section_name} section was not found in {self.module}")

            offset:int = section['sh_offset']
            size:int = section['sh_size']

            fobj.seek(offset)
            return fobj.read(size)

    def find_template(self, offset: int) -> Template:
        data = self.load_templates()

        bin_msg = bytearray()
        for byte in data[offset:]:
            if byte == 0:
                break
            else:
                bin_msg.append(byte)
        return Template(offset, bin_msg)

    def iter_templates(self) -> typing.Iterator[Template]:
        data = self.load_templates()

        msg_starts_at = 0
        bin_msg = bytearray()
        for idx, byte in enumerate(data):
            if byte == 0:
                msg = Template(msg_starts_at, bin_msg)
                bin_msg.clear()
                msg_starts_at = idx+1
                if msg:
                    yield msg
            else:
                bin_msg.append(byte)


TCommand = typing.TypeVar("TCommand", bound="Command")


class Command(abc.ABC):
    subcommands: typing.ClassVar[list[type["Command"]]] = []

    def __init_subclass__(cls: type[TCommand], **kwargs: typing.Any) -> None:
        super().__init_subclass__(**kwargs)
        cls.subcommands.append(cls)

    @classmethod
    @typing.no_type_check
    def add_common_args(cls, parser) -> None:
        parser.set_defaults(klass=cls)
        parser.add_argument('module', type=pathlib.Path, help='path to the binary file(executable, shared library, kernel module)')
        parser.add_argument('section', type=str, help='the ELF section name, all the PET strings are stored in')

    def __init__(self, args: argparse.Namespace):
        self.extractor = TemplatesLoader(args.module, args.section)

    @abc.abstractmethod
    def __call__(self) -> None:
        pass


class FindTemplate(Command):
    @classmethod
    @typing.no_type_check
    def register(cls, subparsers) -> None:
        parser = subparsers.add_parser("find", description="for the given message offset, finds the message format string")
        cls.add_common_args(parser)
        parser.add_argument("offset", type=lambda txt: int(txt, 0), help="the message format offset")

    def __init__(self, args: argparse.Namespace):
        super().__init__(args)
        self.offset = args.offset

    def __call__(self):
        print(self.extractor.find_template(self.offset).as_line)


class ListTemplates(Command):
    @classmethod
    @typing.no_type_check
    def register(cls, subparsers) -> None:
        parser = subparsers.add_parser("list", description="list all messages within a module")
        cls.add_common_args(parser)

    def __init__(self, args: argparse.Namespace):
        super().__init__(args)

    def __call__(self):
        for msg in self.extractor.iter_templates():
            print(msg.as_line)


class SaveTemplates(Command):
    @classmethod
    @typing.no_type_check
    def register(cls, subparsers) -> None:
        parser = subparsers.add_parser("save", description="save all messages within a module to the dedicated file")
        cls.add_common_args(parser)
        parser.add_argument('output', type=pathlib.Path, help='path to the output file')

    def __init__(self, args: argparse.Namespace):
        super().__init__(args)
        self.output = args.output

    def __call__(self):
        with open(self.output, "w+") as fobj:
            for msg in self.extractor.iter_templates():
                fobj.write(f"{msg.offset:#0{4}x}:{msg.spec}{os.linesep}")


class ViewMessages(Command):
    @classmethod
    @typing.no_type_check
    def register(cls, subparsers) -> None:
        parser = subparsers.add_parser("view", description="view all messages")
        cls.add_common_args(parser)
        parser.add_argument('traces', type=pathlib.Path, nargs='+', help='per entity traces files')
        parser.add_argument('--no-sort', action='store_true', dest='no_sort', default=False
                            , help="By default, all traces are sorted; '--no-sort' disables the ordering; usefull to see some entity traces in a single screen")

    def __init__(self, args: argparse.Namespace):
        super().__init__(args)
        self.traces = args.traces
        self.sort = not args.no_sort

    def __load_templates(self) -> dict[int,Template]:
        templates:dict[int,Template] = {}
        for msg in self.extractor.iter_templates():
            templates[msg.offset] = msg
        return templates

    @typing.no_type_check
    def __iter_entities(self) -> typing.Generator[NvmeibPetArchive.Entity, None, None]:
        for fpath in self.traces:
            archive: NvmeibPetArchive = NvmeibPetArchive.from_file(fpath)
            for entity in archive.entities:
                yield entity

    @typing.no_type_check
    def __iter_entity_messages(self, entity: NvmeibPetArchive.Entity) -> typing.Generator[NvmeibPetArchive.Message, None, None]:
        prev_ns_stamp = 0
        for msg in entity.messages:
            if not msg.offset:
                break
            yield msg
    
    @typing.no_type_check
    def __iter_human_messages(self) -> typing.Generator[Message, None, None]:
        templates:dict[int,Template] = self.__load_templates()
        for idx, entity in enumerate(self.__iter_entities()):
            prev_ns_stamp = 0
            for msg in self.__iter_entity_messages(entity):
                try:
                    tmpl = templates[msg.offset - 1]
                except KeyError:
                    raise RuntimeError(f"Unknown PET template offset {msg.offset:#06x} for entity {idx}")
                tmpl = templates[msg.offset-1]
                human_msg = tmpl.instantiate(msg, idx, prev_ns_stamp)
                prev_ns_stamp = human_msg.ns_stamp
                yield human_msg

    def __call__(self):
        human_msgs:typing.Generator[Message, None, None] = self.__iter_human_messages()
        if self.sort:
            human_msgs = sorted(human_msgs, key=lambda hm: hm.ns_stamp) # type: ignore

        for human_msg in human_msgs:
            print(f"{human_msg.dt_stamp} entity={human_msg.entity} {human_msg.text}")
        

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description="extract ASCII strings from a ELF binary .rodata section"
                                     , formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    subparsers = parser.add_subparsers(required=True)
    for subcmd in Command.subcommands:
        if hasattr(subcmd, "register"):
            subcmd.register(subparsers) # type: ignore

    args = parser.parse_args()
    handler = args.klass(args)
    handler()
















