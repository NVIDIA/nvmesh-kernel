#!/usr/bin/env python3

import re
import abc
import enum
import pprint
import typing
import pathlib
import argparse
import datetime
import itertools
import dataclasses

from elftools.elf.elffile import ELFFile
from elftools.dwarf.die import DIE 
from elftools.dwarf.descriptions import describe_attr_value

from nvmeib_pet_archive import NvmeibPetArchive, KaitaiStream


#I really don't care about goodies, like pos & flags & width & precision & length & spec(sgGaAeEfFn)
#gGaAeEfF - are used to print floating-point numbers - kernel & pet don't have them
#s is for string - PET will not have them too; the budget is really thin
PRINTF_SPEC_RE = printf_enum_re = re.compile(r"""
	(?P<spec_prefix>0x?)?
	%(?P<pos>\d+\$)?
	(?P<flags>[-+ #0]*)
	(?P<width>\*|\d+)?
	(?P<precision>\.(?:\*|\d+))?
	(?P<length>hh|h|ll|l|j|z|t|L)?
	(?P<spec>[diuoxXcp%])
	(?:<(?P<tag>enum|union|struct)\s(?P<type_name>[A-Za-z_][A-Za-z0-9_]*)>)?
""", re.VERBOSE)

# ---------------------------------------------------------------------------
# Reconstructed message
# ---------------------------------------------------------------------------

class Message(typing.NamedTuple):
	fname: str
	entity: int
	ns_stamp: int
	dt_stamp: datetime.datetime
	text: str

# ---------------------------------------------------------------------------
# Kaitai helpers
# ---------------------------------------------------------------------------

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


# ---------------------------------------------------------------------------
# C types reconstructed from DWARF
# ---------------------------------------------------------------------------


TypeInfo = typing.Union["BaseType", "EnumType", "StructType", "UnionType", "ArrayType"]


@dataclasses.dataclass(frozen=True)
class BaseType: # actually fundamental type, but DWARF uses "base" as terminology
	class Encoding(enum.StrEnum):
		char = 'char'
		boolean = 'boolean' 
		signed = 'signed'
		unsigned = 'unsigned'

	name: str
	size: int
	encoding: Encoding

	def decode(self, view: memoryview) -> int:
		data = view[:self.size].tobytes()
		value = int.from_bytes(data, 'little', signed=(self.encoding == self.Encoding.signed))
		if self.encoding == self.Encoding.boolean:
			return bool(value)
		return value


@dataclasses.dataclass(frozen=True)
class EnumType:
	base_type: BaseType
	enum_info: enum.Enum

	def decode(self, view: memoryview) -> str:
		value = self.base_type.decode(view)
		return self.enum_info(value).name

	def decode_int(self, value:int) -> str:
		return self.enum_info(value).name 

	@property
	def name(self) -> str:
		return self.base_type.name
	
	@property
	def size(self) -> int:
		return self.base_type.size


@dataclasses.dataclass(frozen=True)
class MemberVariable:

	@dataclasses.dataclass(frozen=True)
	class BitLayot:
		offset: int # absolute bit offset from the struct start
		size: int	# size in bits

		@property 
		def start_byte(self) ->int:
			return self.offset // 8
		
		@property
		def end_byte(self) -> int:
			end_bit = self.offset + self.size
			return (end_bit + 7) // 8

		@property
		def mask(self):
			return (1 << self.size) - 1

		def decode(self, composite_view: memoryview) ->int:
			chunk = int.from_bytes(composite_view[self.start_byte:self.end_byte].tobytes(), 'little')
			shift = self.offset - self.start_byte * 8
			return (chunk >> shift) & self.mask

	name: str
	type_info: TypeInfo
	offset: int
	bit_layout: typing.Optional[BitLayot] = None

	@typing.no_type_check
	def decode(self, composite_view: memoryview) -> typing.Any:
		if self.bit_layout:
			value:int = self.bit_layout.decode(composite_view)
			if isinstance(self.type_info, EnumType):
				return self.type_info.decode_int(value)
			else:
				return value
		else:
			start = self.offset
			end = start + self.type_info.size
			return self.type_info.decode(composite_view[start:end])


@dataclasses.dataclass
class CompositeType:
	name: str
	size: int
	members: list[MemberVariable]

	@typing.no_type_check
	def decode(self, view: memoryview) -> dict[str, typing.Any]:
		result:dict[str, typing.Any] = {}
		for member in self.members:
			result[member.name] = member.decode(view)
		return result


@dataclasses.dataclass
class StructType(CompositeType):
	pass 


@dataclasses.dataclass
class UnionType(CompositeType):
	pass 


@dataclasses.dataclass(frozen=True)
class ArrayType: # not tested yet
	elem: TypeInfo
	count: int
	size: int

	@typing.no_type_check
	def decode(self, view: memoryview) -> list[typing.Any]:
		elems = []
		stride:int = self.elem.size
		for i in range(self.count):
			start = i * stride
			elems.append(self.elem.decode(view[start:start + stride]))
		return elems
	 

# ---------------------------------------------------------------------------
# DWARF reader / resolver
# ---------------------------------------------------------------------------

class DwarfRuntime:
	@typing.no_type_check
	def __init__(self, elf_path: pathlib.Path):
		self._elf = ELFFile(open(elf_path, 'rb'))
		self._dwarf = self._elf.get_dwarf_info() if self._elf.has_dwarf_info() else None
		self._address_size = self._elf.elfclass // 8
		self._type_cache: dict[int, TypeInfo] = {}
		
	@typing.no_type_check
	def load_types(self, type_names: set[str]) -> dict[str, TypeInfo]:
		name_cache: dict[str, TypeInfo] = {}
		if self._dwarf:
			for type_name in type_names:
				die = self.__find_type_die(type_name)
				desc = self.__build_any_type(die)
				name_cache[type_name] = desc

		return name_cache

	@typing.no_type_check
	def __get_die_name(self, die: DIE) -> str:
		attr = die.attributes.get('DW_AT_name')
		return attr.value.decode() if attr else None

	@typing.no_type_check
	def __get_die_type_name(self, die:DIE) -> str:
		name = self.__get_die_name(die)
		if not name:
			name = f"__type{die.offset}"
		return name

	@typing.no_type_check
	def __resolve_die_type(self, die: DIE) -> DIE:
		return die.get_DIE_from_attribute('DW_AT_type')

	@typing.no_type_check
	def __build_bitfield_layout(self, member_die: DIE, type_info: TypeInfo) -> MemberVariable.BitLayot:
		bit_size_attr = member_die.attributes.get('DW_AT_bit_size')
		if not bit_size_attr:
			return None
		bit_size = bit_size_attr.value
		byte_offset = member_die.attributes.get('DW_AT_data_member_location')
		byte_offset_bits = (byte_offset.value if byte_offset else 0) * 8

		data_bit = member_die.attributes.get('DW_AT_data_bit_offset')
		if data_bit:
			return MemberVariable.BitLayot(data_bit.value, bit_size)

		bit_off_attr = member_die.attributes.get('DW_AT_bit_offset')
		if bit_off_attr is None:
			return MemberVariable.BitLayot(byte_offset_bits, bit_size)

		container_bits = type_info.size * 8 if hasattr(type_info, 'size') else self._address_size * 8
		start = byte_offset_bits + (container_bits - bit_off_attr.value - bit_size)
		return MemberVariable.BitLayot(start, bit_size)

	# DWARF traversal --------------------------------------------------------
	
	@typing.no_type_check
	def __find_type_die(self, name: str) -> DIE:
		for cu in self._dwarf.iter_CUs():
			top = cu.get_top_DIE()
			for die in top.iter_children():
				if die.tag in {'DW_TAG_structure_type', 'DW_TAG_union_type', 'DW_TAG_enumeration_type'}:
					if self.__get_die_name(die) == name:
						return die
		raise KeyError(f"type '{name}' not found in DWARF metadata")

	@typing.no_type_check
	def __build_any_type(self, die: DIE) -> TypeInfo:
		key = die.offset
		if key in self._type_cache:
			return self._type_cache[key]

		tag:str = die.tag
		if tag == 'DW_TAG_base_type':
			desc = self.__build_base(die)
		elif tag == 'DW_TAG_array_type':
			desc = self.__build_array(die)
		elif tag == 'DW_TAG_structure_type':
			desc = self.__build_composite(die)
		elif tag == 'DW_TAG_union_type':
			desc = self.__build_composite(die)
		elif tag in {'DW_TAG_const_type', 'DW_TAG_volatile_type', 'DW_TAG_typedef', 'DW_TAG_restrict_type'}:
			target = die.get_DIE_from_attribute('DW_AT_type')
			desc = self.__build_any_type(target)
		elif tag == "DW_TAG_enumeration_type":
			desc = self.__build_enum(die)
		else:
			raise NotImplementedError(f"Unsupported DIE tag: {tag}")

		self._type_cache[key] = desc
		return desc

	# Parsers ----------------------------------------------------------------
	@typing.no_type_check
	def __build_base(self, die: DIE) -> BaseType:
		name = self.__get_die_type_name(die)
		size = die.attributes['DW_AT_byte_size'].value
		enc_attr = die.attributes.get('DW_AT_encoding')
		encoding = describe_attr_value(enc_attr, die, self._dwarf) if enc_attr else 'DW_ATE_unsigned'
		if 'unsigned' in encoding:
			enc = 'unsigned'
		elif 'boolean' in encoding:
			enc = 'boolean'
		elif 'char' in encoding and 'unsigned' not in encoding:
			enc = 'signed'
		else:
			enc = 'unsigned'
		return BaseType(name=name, size=size, encoding=BaseType.Encoding(enc))

	@typing.no_type_check
	def __strip_longest_prefix(self, enum_values: dict[str,int]) -> dict[str,int]:
		if len(enum_values) < 2:
			return enum_values
		min_name = min(enum_values.keys())
		max_name = max(enum_values.keys())
		prefix_len = sum(1 for a, b in itertools.takewhile(lambda p: p[0] == p[1], zip(min_name, max_name)))
		short_enum_values: dict[str,int] = {}
		for name, value in enum_values.items():
			short_enum_values[name[prefix_len:]] = value
		return short_enum_values

	@typing.no_type_check
	def __build_enum(self, die: DIE) -> EnumType:
		base_type = self.__build_base(die)

		type_name:str = self.__get_die_name(die)

		values = {}
		for child in die.iter_children():
			if child.tag == "DW_TAG_enumerator":
				value_name = child.attributes["DW_AT_name"].value.decode("utf-8")
				value_value = child.attributes["DW_AT_const_value"].value
				values[value_name] = value_value
		
		enum_info = enum.Enum(type_name, self.__strip_longest_prefix(values))
		return EnumType(base_type=base_type, enum_info=enum_info) 

	@typing.no_type_check
	def __build_array(self, die: DIE) -> ArrayType:
		elem_die = die.get_DIE_from_attribute('DW_AT_type')
		elem = self.__build_any_type(elem_die)
		count = 1
		for child in die.iter_children():
			if child.tag != 'DW_TAG_subrange_type':
				continue
			if 'DW_AT_count' in child.attributes:
				count *= child.attributes['DW_AT_count'].value
			elif 'DW_AT_upper_bound' in child.attributes:
				count *= child.attributes['DW_AT_upper_bound'].value + 1
			else:
				raise ValueError('Array subrange without count/upper bound')
		return ArrayType(elem=elem, count=count, size=elem.size * count)

	@typing.no_type_check
	def __build_composite(self, die: DIE) -> CompositeType:
		name = self.__get_die_type_name(die)
		size = die.attributes.get('DW_AT_byte_size')
		if not size:
			raise ValueError(f"struct {name} missing DW_AT_byte_size")
		members = []
		for child in die.iter_children():
			if child.tag != 'DW_TAG_member':
				continue
			mv_name = self.__get_die_name(child) or f'_unnamed{len(members)}'
			mv_type = self.__build_any_type(self.__resolve_die_type(child))
			offset = child.attributes.get('DW_AT_data_member_location')
			byte_offset_val = offset.value if offset else 0
			bit_layout = self.__build_bitfield_layout(child, mv_type)
			members.append(MemberVariable(mv_name, mv_type, byte_offset_val, bit_layout))
		
		if die.tag == 'DW_TAG_structure_type':
			return StructType(name=name, size=size.value, members=members)
		else:
			return UnionType(name=name, size=size.value, members=members)


@dataclasses.dataclass(frozen=True)
class MessageSpec:
	offset:int 
	spec: str


@dataclasses.dataclass
class ArgPrintfSpec:
	spec: str
	tag: str 
	type_name: str
	_hex: bool = dataclasses.field(init=False,repr=False)

	@staticmethod
	def from_re_match(m: re.Match[str]) -> 'ArgPrintfSpec':
		return ArgPrintfSpec(m.group('spec'), m.group('tag'), m.group('type_name'))

	def __post_init__(self):
		self._hex = bool(set('xXpP') & set(self.spec))

	@property
	def hex(self) -> bool:
		return self._hex


class ArgDecoder:
	def __init__(self, user_defined_type:TypeInfo, printf_spec: ArgPrintfSpec):	
		self.__user_defined_type: TypeInfo = user_defined_type
		self.__printf_spec = printf_spec
	
	def __format(self, value: typing.Union[dict,list,tuple,str,int]) -> str:
		if isinstance(value, int):
			if self.__printf_spec.hex and value:
				return '0x{:x}'.format(value)
			else:
				return str(value)
		elif isinstance(value, str):
			return value
		elif isinstance(value, (list, tuple)):
			inner_values: list[str] = []
			for inner_value in value:
				inner_values.append(self.__format(inner_value))
			return '[' + ', '.join(inner_values) + ']'
		else:
			inner_items: list[str] = []
			for inner_key, inner_value in value.items():
				inner_items.append(self.__format(inner_key) + ': ' + self.__format(inner_value))
			return '{' + ', '.join(inner_items) + '}'

	def decode(self, value: int) -> typing.Any:
		try:
			memview = memoryview(value.to_bytes(8, byteorder='little', signed=False)) 
			return self.__user_defined_type.decode(memview)
		except Exception as error:
			return str(error)

	def __call__(self, value: int) -> str:
		if value or isinstance(self.__user_defined_type, EnumType):
			decoded = self.decode(value)
			formatted = self.__format(decoded)
			if self.__printf_spec.hex:
				return '0x{:x}<{}>'.format(value, formatted)
			else:
				return '{}<{}>'.format(value, formatted)
		else:
			return '0'

class Template:
	def __init__(self, msg_spec:MessageSpec, user_defined_types: dict[str, TypeInfo]):
		self.__user_defined_types = user_defined_types
		self.__c_spec = msg_spec
		self.__py_spec: str = ""
		self.__py_decoders:dict[int,typing.Callable[[int], typing.Any]] = {} 
		self.__process_c_spec() # updates __py_spec and __py_args

	def __escape_msg_part(self, part: str ) -> str:
		return part.replace('{', '{{').replace('}', '}}')

	def __process_c_spec(self) -> None:
		last = 0
		parts: list[str] = []
		spec = self.__c_spec.spec
		for idx, m in enumerate(PRINTF_SPEC_RE.finditer(spec)):
			parts.append(self.__escape_msg_part(spec[last:m.start()]))	# field before separator
			arg_printf_spec:ArgPrintfSpec = ArgPrintfSpec.from_re_match(m)
			if m.group('type_name'):
				udt_found:typing.Optional[TypeInfo] = self.__user_defined_types.get(m.group('type_name'), None)
				if udt_found:
					self.__py_decoders[idx] = ArgDecoder(udt_found, arg_printf_spec)
				parts.append('{}')
			else:
				if arg_printf_spec.hex:
					parts.append('0x{:x}')
				else:
					parts.append('{}')
			last = m.end()
		parts.append(self.__escape_msg_part(spec[last:]))				# trailing field
		self.__py_spec = ''.join(parts)

	def __bool__(self):
		return bool(self.__py_spec)

	@property
	def spec(self) -> MessageSpec:
		return self.__c_spec

	@typing.no_type_check
	def __load_args(self, msg:NvmeibPetArchive.Message) -> list[typing.Any]:
		args: list[int] = []
		for idx, arg in enumerate(msg.args): # type: ignore
			decoder = self.__py_decoders.get(idx, None)
			if decoder:
				args.append(decoder(arg.pet_value))
			else:
				args.append(arg.pet_value)
		return args

	def instantiate(self, msg:NvmeibPetArchive.Message, fname: str, entity:int) -> Message:
		args: list[typing.Any] = self.__load_args(msg)
		text = self.__py_spec.format(*args)
		dt_stamp = datetime.datetime.fromtimestamp(msg.timestamp.uv8/(10**9)) # type: ignore
		return Message(fname=fname, entity=entity, ns_stamp=msg.timestamp.uv8, dt_stamp=dt_stamp, text=text) # type: ignore


@dataclasses.dataclass(frozen=True)
class Dictionaty:
	templates: dict[int, Template]
	user_defined_types: dict[str, TypeInfo] 

class TemplatesLoader:
	def __init__(self, module: pathlib.Path, section_name: str):
		self.module = module
		self.section_name = section_name
		self.__dwarf_runtime = DwarfRuntime(module)

	@typing.no_type_check
	def __load_messages_blob(self) -> bytes:
		with open(self.module, "rb") as fobj:
			elf = ELFFile(fobj)
			section: Section = elf.get_section_by_name(self.section_name)
			if not section:
				raise ValueError(f"{self.section_name} section was not found in {self.module}")

			offset:int = section['sh_offset']
			size:int = section['sh_size']

			fobj.seek(offset)
			return fobj.read(size)

	def __load_messages_spec(self) -> list[MessageSpec]:
		data = self.__load_messages_blob()
		msgs: list[MessageSpec] = []

		msg_starts_at:int = 0
		bin_msg = bytearray()
		for idx, byte in enumerate(data):
			if byte == 0:
				if bin_msg:
					msgs.append(MessageSpec(offset=msg_starts_at, spec=bin_msg.decode("utf-8", 'replace')))
					bin_msg.clear()
				msg_starts_at = idx+1
			else:
				bin_msg.append(byte)
		return msgs 
	
	def __list_user_defined_types(self, msgs:list[MessageSpec]) -> set[str]:
		types: set[str] = set()
		for msg in msgs:
			for m in PRINTF_SPEC_RE.finditer(msg.spec):
				if m.group('tag') and m.group('type_name'):
					types.add(f"{m.group('type_name')}")
		return types

	def load_dictionary(self) -> Dictionaty:
		msgs = self.__load_messages_spec()
		user_defined_type_names: set[str] = self.__list_user_defined_types(msgs)
		user_defined_types = self.__dwarf_runtime.load_types(user_defined_type_names)

		templates: dict[int, Template] = {}
		for msg in msgs:
			templates[msg.offset] = Template(msg, user_defined_types)
		return Dictionaty(templates=templates, user_defined_types=user_defined_types)

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


class ViewDictionary(Command):
	@classmethod
	@typing.no_type_check
	def register(cls, subparsers) -> None:
		parser = subparsers.add_parser("view-dictionary", description="list all messages & user defined types within a module")
		cls.add_common_args(parser)

	def __init__(self, args: argparse.Namespace):
		super().__init__(args)

	def __call__(self):
		dictionary = self.extractor.load_dictionary()
		for msg in dictionary.templates.values():
			pprint.pprint(dataclasses.asdict(msg.spec))
		for udt in dictionary.user_defined_types.values():
			pprint.pprint(dataclasses.asdict(udt))
			 

class EvaluateInt(Command):
	@classmethod
	@typing.no_type_check
	def register(cls, subparsers) -> None:
		parser = subparsers.add_parser("evaluate-int", description="given a user defined type (enum/struct/union) and integer - prints the struct content")
		cls.add_common_args(parser)
		parser.add_argument('user_defined_type', type=str, help='the user defined type ; no need to specify the tag(enum/struct/union)')
		parser.add_argument('value', type=lambda s: int(s, 0), help='the user defined type ; no need to specify the tag(enum/struct/union)')

	def __init__(self, args: argparse.Namespace):
		super().__init__(args)
		self.udt_name = args.user_defined_type
		self.value = args.value

	def __call__(self):
		dictionary = self.extractor.load_dictionary()
		udt = dictionary.user_defined_types.get(self.udt_name, None)
		if udt is None:
			print(f"Failed to find {self.udt_name}.")
			return 
		decoder = ArgDecoder(udt, ArgPrintfSpec('u', '', self.udt_name))
		import pudb; pudb.set_trace()
		print(decoder(self.value))

class SaveDictionary(Command):
	@classmethod
	@typing.no_type_check
	def register(cls, subparsers) -> None:
		parser = subparsers.add_parser("save-dictionary", description="save all messages within a module to the dedicated file")
		cls.add_common_args(parser)
		parser.add_argument('output', type=pathlib.Path, help='path to the output file')

	def __init__(self, args: argparse.Namespace):
		super().__init__(args)
		self.output = args.output

	def __call__(self):
		dictionary = self.extractor.load_dictionary()
		with open(self.output, "w+") as fobj:
			for msg in dictionary.templates.values():
				fobj.write(pprint.pformat(dataclasses.asdict(msg.spec)))
				fobj.write('\n\n')
			for udt in dictionary.user_defined_types.values():
				fobj.write(pprint.pformat(dataclasses.asdict(udt)))
				fobj.write('\n\n')


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

	@typing.no_type_check
	def __iter_entities(self) -> typing.Generator[NvmeibPetArchive.Entity, None, None]:
		for fpath in self.traces:
			with open(fpath, 'rb') as fobj:
				idx = 0
				kstream = KaitaiStream(fobj)
				while not kstream.is_eof():
					entity_start_position = kstream.pos()
					entity = NvmeibPetArchive.Entity(kstream)
					entity.fname = fpath.name
					entity.idx = idx 
					entity.size = kstream.pos() - entity_start_position
					idx += 1
					yield entity
			#The code below loads the whole file into memory - waste of resources 
			#archive: NvmeibPetArchive = NvmeibPetArchive.from_file(fpath)
			#for entity in archive.entities:
				#yield entity

	@typing.no_type_check
	def __iter_entity_messages(self, entity: NvmeibPetArchive.Entity) -> typing.Generator[NvmeibPetArchive.Message, None, None]:
		prev_ns_stamp = 0
		for msg in entity.messages:
			if not msg.offset:
				break

			if msg.timestamp.pet_value_attr_name != 'uv8':
				# it means we have time delta from the previous message
				delta:int = msg.timestamp.pet_value
				setattr(msg.timestamp, msg.timestamp.pet_value_attr_name, None)
				msg.timestamp.uv8 = delta + prev_ns_stamp

			prev_ns_stamp = msg.timestamp.uv8

			yield msg

	@typing.no_type_check
	def __iter_human_messages(self) -> typing.Generator[Message, None, None]:
		dictionary = self.extractor.load_dictionary()
		for entity in self.__iter_entities():
			last_human_msg = None
			for msg in self.__iter_entity_messages(entity):
				try:
					tmpl = dictionary.templates[msg.offset - 1]
				except KeyError:
					raise RuntimeError(f"Unknown PET template offset {msg.offset:#06x} for entity {entity.idx}")
				human_msg = tmpl.instantiate(msg, entity.fname, entity.idx)
				yield human_msg
				last_human_msg = human_msg
			yield human_msg._replace(text=f"entity size={entity.size} bytes")


	def __call__(self):
		human_msgs:typing.Generator[Message, None, None] = self.__iter_human_messages()
		if self.sort:
			human_msgs = sorted(human_msgs, key=lambda hm: hm.ns_stamp) # type: ignore

		for human_msg in human_msgs:
			print(f"{human_msg.dt_stamp} entity={human_msg.fname}[{human_msg.entity}] {human_msg.text}")


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
















