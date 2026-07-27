#!/usr/local/bin/bash

BASH_EXE=/usr/local/bin/bash
$BASH_EXE ./build.sh "$@" | $BASH_EXE ./gcc_error_parser.sh
#$BASH_EXE ./build.sh
