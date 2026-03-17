# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0

# This is a generated file! Please edit source .ksy file and use kaitai-struct-compiler to rebuild
# type: ignore

import kaitaistruct
from kaitaistruct import KaitaiStruct, KaitaiStream, BytesIO
from enum import IntEnum


if getattr(kaitaistruct, 'API_VERSION', (0, 9)) < (0, 11):
    raise Exception("Incompatible Kaitai Struct Python API: 0.11 or later is required, but you have %s" % (kaitaistruct.__version__))

class NvmeibPetArchive(KaitaiStruct):

    class PetStoreType(IntEnum):
        pet_store_type_s_byte = 0
        pet_store_type_u_byte = 1
        pet_store_type_s_short = 2
        pet_store_type_u_short = 3
        pet_store_type_s_int = 4
        pet_store_type_u_int = 5
        pet_store_type_s_long_int = 6
        pet_store_type_u_long_int = 7
    def __init__(self, _io, _parent=None, _root=None):
        super(NvmeibPetArchive, self).__init__(_io)
        self._parent = _parent
        self._root = _root or self
        self._read()

    def _read(self):
        pass


    def _fetch_instances(self):
        pass
        _ = self.entities
        if hasattr(self, '_m_entities'):
            pass
            for i in range(len(self._m_entities)):
                pass
                self._m_entities[i]._fetch_instances()



    class Entity(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            super(NvmeibPetArchive.Entity, self).__init__(_io)
            self._parent = _parent
            self._root = _root
            self._read()

        def _read(self):
            self.commit_id = self._io.read_u8le()
            self.num_messages = self._io.read_u2le()
            self.messages = []
            for i in range(self.num_messages):
                self.messages.append(NvmeibPetArchive.Message(self._io, self, self._root))



        def _fetch_instances(self):
            pass
            for i in range(len(self.messages)):
                pass
                self.messages[i]._fetch_instances()



    class Message(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            super(NvmeibPetArchive.Message, self).__init__(_io)
            self._parent = _parent
            self._root = _root
            self._read()

        def _read(self):
            self.offset = self._io.read_u2le()
            self.num_args = self._io.read_u1()
            self.timestamp = NvmeibPetArchive.PetVariant(self._io, self, self._root)
            self.args = []
            for i in range(self.num_args):
                self.args.append(NvmeibPetArchive.PetVariant(self._io, self, self._root))



        def _fetch_instances(self):
            pass
            self.timestamp._fetch_instances()
            for i in range(len(self.args)):
                pass
                self.args[i]._fetch_instances()



    class PetVariant(KaitaiStruct):
        def __init__(self, _io, _parent=None, _root=None):
            super(NvmeibPetArchive.PetVariant, self).__init__(_io)
            self._parent = _parent
            self._root = _root
            self._read()

        def _read(self):
            self.type = KaitaiStream.resolve_enum(NvmeibPetArchive.PetStoreType, self._io.read_u1())
            if self.type == NvmeibPetArchive.PetStoreType.pet_store_type_s_byte:
                pass
                self.sv1 = self._io.read_s1()

            if self.type == NvmeibPetArchive.PetStoreType.pet_store_type_u_byte:
                pass
                self.uv1 = self._io.read_u1()

            if self.type == NvmeibPetArchive.PetStoreType.pet_store_type_s_short:
                pass
                self.sv2 = self._io.read_s2le()

            if self.type == NvmeibPetArchive.PetStoreType.pet_store_type_u_short:
                pass
                self.uv2 = self._io.read_u2le()

            if self.type == NvmeibPetArchive.PetStoreType.pet_store_type_s_int:
                pass
                self.sv4 = self._io.read_s4le()

            if self.type == NvmeibPetArchive.PetStoreType.pet_store_type_u_int:
                pass
                self.uv4 = self._io.read_u4le()

            if self.type == NvmeibPetArchive.PetStoreType.pet_store_type_s_long_int:
                pass
                self.sv8 = self._io.read_s8le()

            if self.type == NvmeibPetArchive.PetStoreType.pet_store_type_u_long_int:
                pass
                self.uv8 = self._io.read_u8le()



        def _fetch_instances(self):
            pass
            if self.type == NvmeibPetArchive.PetStoreType.pet_store_type_s_byte:
                pass

            if self.type == NvmeibPetArchive.PetStoreType.pet_store_type_u_byte:
                pass

            if self.type == NvmeibPetArchive.PetStoreType.pet_store_type_s_short:
                pass

            if self.type == NvmeibPetArchive.PetStoreType.pet_store_type_u_short:
                pass

            if self.type == NvmeibPetArchive.PetStoreType.pet_store_type_s_int:
                pass

            if self.type == NvmeibPetArchive.PetStoreType.pet_store_type_u_int:
                pass

            if self.type == NvmeibPetArchive.PetStoreType.pet_store_type_s_long_int:
                pass

            if self.type == NvmeibPetArchive.PetStoreType.pet_store_type_u_long_int:
                pass



    @property
    def entities(self):
        if hasattr(self, '_m_entities'):
            return self._m_entities

        self._m_entities = []
        i = 0
        while not self._io.is_eof():
            self._m_entities.append(NvmeibPetArchive.Entity(self._io, self, self._root))
            i += 1

        return getattr(self, '_m_entities', None)


