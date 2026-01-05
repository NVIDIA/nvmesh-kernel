/*
 * TRACER section collects trace metadata (__tracer input sections).
 * Placed after .rodata to ensure it lands in a non-executable segment,
 * avoiding the "LOAD segment with RWX permissions" linker warning.
 */
SECTIONS {
  TRACER : {
    __tracer_start = .;
    *(__tracer);
    __tracer_end = .;
  }
}

INSERT AFTER .rodata;
