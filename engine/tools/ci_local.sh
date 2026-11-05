#!/bin/zsh
# ci_local.sh [mac|linux] - the CI's ROM-free checks (.github/workflows/ci.yml) on this machine, for the
# private repo where GitHub's runners are not used: the engine with warnings as errors, ctest, the
# analyzer's self-test. mac: here; linux: in an Ubuntu 24.04 Docker container (GCC). No argument: both.
set -eu; setopt pipefail
root=${0:A:h:h:h}; cd $root
want=($@); (( $#want )) || want=(mac linux); fail=0
if (( ${want[(I)mac]} )); then
  print "== mac"
  b=build/ci-mac; [[ -x .venv-ci/bin/python ]] || { python3 -m venv .venv-ci && .venv-ci/bin/pip install -q capstone; }
  { cmake -S engine -B $b -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-Werror >/dev/null && cmake --build $b >/dev/null &&
    ctest --test-dir $b --output-on-failure --no-tests=error | tail -3 && .venv-ci/bin/python engine/tools/recompiler/test_analyze.py | tail -1; } || { print "mac: FAILED"; fail=1; }
fi
if (( ${want[(I)linux]} )); then
  print "== linux (ubuntu-24.04, the committed tree as a fresh checkout)"
  t=$(mktemp -d); trap "rm -rf $t" EXIT
  git archive HEAD engine | tar -C $t -xf -
  sub=engine/ext/Genesis-Plus-GX; mkdir -p $t/$sub
  git -C $sub archive $(git rev-parse HEAD:$sub) | tar -C $t/$sub -xf -      # the recorded commit, unpatched
  docker run --rm -v $t:/src:ro ubuntu:24.04 bash -c '
    set -eo pipefail; export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq >/dev/null; apt-get install -y -qq gcc cmake ninja-build libsdl2-dev zlib1g-dev python3-venv patch git >/dev/null 2>&1
    cp -a /src /w; cd /w
    python3 -m venv /v; /v/bin/pip install -q capstone 2>/dev/null
    cmake -S engine -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-Werror >/dev/null
    cmake --build build >/dev/null
    ctest --test-dir build --output-on-failure --no-tests=error | tail -3
    /v/bin/python engine/tools/recompiler/test_analyze.py | tail -1' || { print "linux: FAILED"; fail=1; }
fi
(( fail )) && exit 1; print "ci_local: OK"
