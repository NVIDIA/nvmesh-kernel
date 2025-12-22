meta:
  id: nvmeib_pet_archive
  file-extension: pet
  endian: le
enums:
  pet_store_type:
    0: pet_store_type_s_byte
    1: pet_store_type_u_byte
    2: pet_store_type_s_short
    3: pet_store_type_u_short
    4: pet_store_type_s_int
    5: pet_store_type_u_int
    6: pet_store_type_s_long_int
    7: pet_store_type_u_long_int
types:
  pet_variant:
    seq:
      - id: type
        type: u1
        enum: pet_store_type
      - id: sv1
        type: s1
        if: type == pet_store_type::pet_store_type_s_byte
      - id: uv1
        type: u1
        if: type == pet_store_type::pet_store_type_u_byte
      - id: sv2
        type: s2
        if: type == pet_store_type::pet_store_type_s_short
      - id: uv2
        type: u2
        if: type == pet_store_type::pet_store_type_u_short
      - id: sv4
        type: s4
        if: type == pet_store_type::pet_store_type_s_int
      - id: uv4
        type: u4
        if: type == pet_store_type::pet_store_type_u_int
      - id: sv8
        type: s8
        if: type == pet_store_type::pet_store_type_s_long_int
      - id: uv8
        type: u8
        if: type == pet_store_type::pet_store_type_u_long_int
  message:
    seq:
      - id: offset
        type: u2
      - id: num_args
        type: u1
      - id: timestamp
        type: pet_variant
      - id: args
        type: pet_variant
        repeat: expr
        repeat-expr: num_args
  entity:
    seq:
      - id: num_messages
        type: u2
      - id: messages
        type: message
        repeat: expr
        repeat-expr: num_messages
instances:
  entities:
    type: entity
    repeat: eos