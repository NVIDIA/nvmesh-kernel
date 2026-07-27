#!/bin/bash

# This will strip all passed file except kernel modules

file="${@: -1}"
ORIG_STRIP=$(rpm --eval %__strip)
case $file in
  *.ko|*infra_shared.so)
    _STRIP=/usr/bin/true
    ;;
  *)
    _STRIP=$ORIG_STRIP
    ;;
esac
echo "Run ${_STRIP} "${@: 1}""

eval ${_STRIP} "${@: 1}"

