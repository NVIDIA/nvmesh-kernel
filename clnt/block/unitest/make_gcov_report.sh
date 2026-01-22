#!/bin/sh

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0

OUTDIR=report.gcov
OUTFILE=report.html

NOCONFIRM=false

BUILD=false

print_help()
{
  echo -e "Usage:"
  echo -e "\t$0 [--build] [--noconfirm]"
}

user_confirm()
{
  if [ "$NOCONFIRM" = true ]; then
    return 0
  fi
  read -p "$1 (y/n) " CONT
  if [ "$CONT" == "y" ]; then
    return 0
  fi
  return 1
}

for ARG in "$@"; do
  if [ "$ARG" = "--noconfirm" ]; then
    NOCONFIRM=true
  fi
  if [ "$ARG" = "--build" ]; then
    BUILD=true
  fi
  if [ "$ARG" = "-h" ] || [ "$ARG" = "--help" ]; then
    print_help
    exit 1
  fi
done

if ! command -v gcovr 2>&1 >/dev/null; then
  if user_confirm "I need 'gcovr' to run. Do you want me to install it for you?" ; then
    pip install --user gcovr
  else
    exit 1
  fi
fi

if [ "$BUILD" == false ]; then
  # Simply generate report
  
  if user_confirm "I will erase all context of the directory '$OUTDIR'. Continue?" ; then
    rm -rf report.gcov
    mkdir report.gcov
  else
    exit 1
  fi
  
  echo 'Making report, it may take some time...'
  gcovr -r ../../../ --html --html-details -o $OUTDIR/$OUTFILE -j 12 --object-directory .
  xdg-open $OUTDIR/$OUTFILE
  
else
  # Full build => run => report cycle
  if user_confirm "This will rebuild simulator with GCOV, run it and generate report. Continue?" ; then
    rm -rf report.gcov
    mkdir report.gcov
  else
    exit 1
  fi

  ./build_block_testing.sh build GCOV=1
  ./run_block_unitest.sh -n
  ./make_gcov_report.sh --noconfirm
  
fi
