#!/bin/sh
# GNU coreutils' own du tests (tests/du/), run with fastdu in du's place.
#
# The tests are GPLv3 and stay in coreutils: this downloads a release, builds
# it once (their harness needs the configured tree), puts fastdu where the
# built du was, and runs `make check` on tests/du. A test fastdu is known to
# fail for a reason of GNU's own making is listed, with the reason, in
# test/gnu-du-known; any other failure fails this script.
#
#   test/gnu-du.sh ./fastdu [WORKDIR]
set -eu
VERSION=9.12
here=$(cd "$(dirname "$0")" && pwd)
fastdu=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
work=${2:-$(mktemp -d)}
mkdir -p "$work"
cd "$work"
jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu)

tree=coreutils-$VERSION
if [ ! -f "$tree/src/du.o" ] && [ ! -f "$tree/src/du-du.o" ]; then
  curl -fsSL "https://ftp.gnu.org/gnu/coreutils/$tree.tar.xz" | tar -xJf -
  (cd "$tree" && ./configure --quiet --disable-nls >/dev/null && make -j"$jobs" >/dev/null)
fi
cd "$tree"

# fastdu in du's place, newer than everything make would rebuild it from.
cp "$fastdu" src/du
touch src/du

tests=$(cd tests/du && ls ./*.sh ./*.pl | sed 's|^\./|tests/du/|' | tr '\n' ' ')
set +e
make check SUBDIRS=. TESTS="$tests" VERBOSE=yes > "$work/check.log" 2>&1
set -e

known=$(grep -v '^#' "$here/gnu-du-known" | cut -d' ' -f1)
failures=0
for test in $tests; do
  log=${test%.*}.log
  result=$(sed -n 's/^\(PASS\|FAIL\|SKIP\|XFAIL\|XPASS\|ERROR\): .*/\1/p' "$log" 2>/dev/null | tail -1)
  [ -n "$result" ] || result=$(grep -Eo '^(PASS|FAIL|SKIP|XFAIL|XPASS|ERROR)' "$log" 2>/dev/null | tail -1 || true)
  [ -n "$result" ] || result=MISSING
  name=$(basename "$test")
  if [ "$result" = FAIL ] || [ "$result" = ERROR ] || [ "$result" = MISSING ]; then
    if printf '%s\n' "$known" | grep -qx "$name"; then
      echo "known  $name"
      continue
    fi
    echo "$result  $name"
    failures=$((failures + 1))
    # The differences, not the harness's trace: each diff, and the Perl tests' mismatches.
    { grep -A14 '^--- ' "$log" || true; grep -E 'mismatch|^-[a-z]' "$log" || true; } |
      grep -v '^++' | head -40 | sed 's/^/    /'
    continue
  fi
  echo "$result  $name"
done
[ "$failures" -eq 0 ] || { echo "$failures of GNU's du tests failed" >&2; exit 1; }
echo "GNU coreutils $VERSION tests/du: all passed or known"
