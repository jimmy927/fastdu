#!/bin/sh
# fastdu against du on a tree with nesting, an empty folder, many files in one
# folder and a file under two names: sizes to the byte, on one thread and many.
set -eu
fastdu=$1
root=$(mktemp -d)
trap 'rm -rf "$root"' EXIT

mkdir -p "$root/a/b/c" "$root/empty" "$root/many"
i=0
while [ $i -lt 3000 ]; do echo $i > "$root/many/f$i"; i=$((i + 1)); done
head -c 200000 /dev/zero > "$root/a/one.bin"
head -c 50000 /dev/zero > "$root/a/b/c/deep.bin"
head -c 300000 /dev/zero > "$root/a/b/linked.bin"
ln "$root/a/b/linked.bin" "$root/empty/same.bin"

fail() { echo "FAIL: $*" >&2; exit 1; }
size() { du -sx --block-size=1 "$1" | cut -f1; }
line() { printf '%s\n' "$1" | awk -F '\t' -v path="$2" '$3 == path'; }

for threads in 1 8; do
  out=$("$fastdu" -j "$threads" "$root")
  for path in "$root" "$root/many" "$root/a/b/c"; do
    got=$(line "$out" "$path" | cut -f1)
    [ "$got" = "$(size "$path")" ] || fail "$path at $threads threads: $got, du says $(size "$path")"
  done
  # 3000 + one + deep + the linked file once.
  files=$(line "$out" "$root" | cut -f2)
  [ "$files" = 3003 ] || fail "$files files at $threads threads, not 3003"
done

out=$("$fastdu" -m 150K "$root")
[ -n "$(line "$out" "$root/a")" ] || fail "-m left out $root/a"
[ -z "$(line "$out" "$root/a/b/c")" ] || fail "-m kept $root/a/b/c"

[ "$("$fastdu" -d 1 "$root" | wc -l)" = 4 ] || fail "-d 1 did not print the root and its three folders"

if "$fastdu" "$root/missing" 2>/dev/null; then fail "a missing path exited 0"; fi
if "$fastdu" -d x "$root" 2>/dev/null; then fail "a bad depth exited 0"; fi

echo "ok"
