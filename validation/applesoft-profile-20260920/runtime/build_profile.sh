#!/usr/bin/env bash
set -euo pipefail
export PATH="/ucrt64/bin:/usr/bin:$PATH"
if [[ -f ~/.profile ]]; then
    source ~/.profile
else
    source /home/hasse/.profile
fi
export PATH="/ucrt64/bin:$PATH"
cd /c/Users/hasse/source/repos/gssquared
mkdir -p build/profile-tmp
export TMPDIR="$PWD/build/profile-tmp"
export TMP="$(cygpath -w "$TMPDIR")"
export TEMP="$TMP"
command -v gcc g++ cmake mingw32-make
cmake --build build --target GSSquared --parallel 8
