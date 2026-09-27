#!/bin/sh
# macOS: fastdu runs over PATH with THREADS threads, each with its time and
# its folder opens that took over 4 s (bench/slowopen.c) — the App Data stalls.
#
#   bench/stalls.sh THREADS PATH [RUNS]
set -eu
threads=$1
path=$2
runs=${3:-1}
here=$(cd "$(dirname "$0")/.." && pwd)
shim="$here/bench/slowopen.dylib"
[ -f "$shim" ] || cc -O2 -dynamiclib -o "$shim" "$here/bench/slowopen.c"
i=0
while [ "$i" -lt "$runs" ]; do
  i=$((i + 1))
  t0=$(python3 -c 'import time; print(time.time())')
  slow=$(DYLD_INSERT_LIBRARIES="$shim" "$here/fastdu" -j "$threads" -d 0 "$path" 2>&1 >/dev/null \
    | grep "^slow opens:" || echo "slow opens: ?")
  took=$(python3 -c "import time; print(f'{time.time() - $t0:.2f}')")
  line="- fastdu -j $threads on \`$path\`: $took s; $slow"
  echo "$line"
  if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then echo "$line" >> "$GITHUB_STEP_SUMMARY"; fi
done
