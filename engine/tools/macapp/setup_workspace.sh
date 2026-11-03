#!/bin/zsh
# setup_workspace.sh PAYLOAD WHEELS WORKSPACE VERSION - the standalone app's first run (and each new
# version): the engine's tree from the bundle (PAYLOAD) copied to WORKSPACE, keeping what earlier
# runs built there (generated C, builds, sessions), and its Python tools (capstone, cmake, ninja)
# installed into WORKSPACE/engine/.venv from the bundled WHEELS, offline, with Apple's python3.
# Needs Apple's Command Line Tools (the compiler); without them it opens their installer, prints
# NEEDTOOLS and exits 3. Progress: "STEP what" lines; details in WORKSPACE/setup.log.
set -eu
payload=$1 wheels=$2 ws=$3 version=$4
mkdir -p $ws; log=$ws/setup.log; : > $log
if ! xcode-select -p >/dev/null 2>&1 || [[ ! -x /usr/bin/python3 ]] || ! /usr/bin/xcrun --find clang >/dev/null 2>&1; then
  print NEEDTOOLS; xcode-select --install >> $log 2>&1 || true; exit 3
fi
if [[ ! -f $ws/.version || $(<$ws/.version) != $version ]]; then
  print "STEP Copying the engine"
  rsync -a $payload/ $ws/ >> $log 2>&1     # no --delete: earlier builds and sessions stay
  rm -f $ws/engine/.venv/.ok
fi
if [[ ! -f $ws/engine/.venv/.ok ]]; then
  print "STEP Installing the build tools"
  rm -rf $ws/engine/.venv
  /usr/bin/python3 -m venv $ws/engine/.venv >> $log 2>&1
  $ws/engine/.venv/bin/pip install --no-index --find-links $wheels capstone cmake ninja >> $log 2>&1
  $ws/engine/.venv/bin/python -c 'import capstone' >> $log 2>&1
  touch $ws/engine/.venv/.ok
fi
mkdir -p ${ws:h}/roms
print -r -- $version > $ws/.version
print READY
