SECTIONS {
  TRACER : {
    __tracer_start = .;
    *(__tracer);
    __tracer_end = .;
  }
}

INSERT AFTER .text;
