#!/bin/sh
# fastdu against GNU du: the same arguments on the same tree, at one thread and
# at many, must print the same lines (sorted: du's order within a folder is the
# directory's), exit the same, and report the same unreadable folders.
#
#   test/unix.sh ./fastdu
#
# GNU du is `du` on Linux and `gdu` from Homebrew's coreutils on macOS.
set -eu
fastdu=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")

du=
for candidate in du gdu; do
  if "$candidate" --version 2>/dev/null | grep -q 'GNU coreutils'; then du=$candidate; break; fi
done
[ -n "$du" ] || { echo "FAIL: no GNU du (on macOS: brew install coreutils)" >&2; exit 1; }
"$du" --version | head -1
# GNU du before 9 counted a folder's own st_size under --apparent-size; fastdu
# counts 0, as 9 does. Against an older du those cases are left out.
du_major=$("$du" --version | head -1 | sed 's/.* \([0-9][0-9]*\)\.[0-9.]*$/\1/')

base=$(mktemp -d)
cleanup() { chmod -R u+rwx "$base" 2>/dev/null; rm -rf "$base"; }
trap cleanup EXIT
cd "$base"

# ---- the tree ----
mkdir -p tree/a/b/c tree/empty tree/many tree/hl tree/links tree/node_modules/pkg tree/logs \
  "tree/space dir" outside/dir
i=0
while [ $i -lt 3000 ]; do echo $i > "tree/many/f$i"; i=$((i + 1)); done
head -c 200000 /dev/zero > tree/a/one.bin
head -c 50000 /dev/zero > tree/a/b/c/deep.bin
head -c 7000 /dev/zero > tree/a/b/small.bin
# Hard links in one folder: which name counts cannot depend on thread order.
head -c 300000 /dev/zero > tree/hl/first
ln tree/hl/first tree/hl/second
# Sparse: 10 MiB apparent, next to nothing on disk.
dd if=/dev/zero of=tree/sparse.bin bs=1 count=0 seek=10485760 2>/dev/null
head -c 100 /dev/zero > tree/node_modules/pkg/index.js
echo log > tree/logs/x.log
echo txt > tree/logs/y.txt
echo spaced > "tree/space dir/file name.txt"
# Links point outside the tree, so -L reaches each target once.
head -c 40000 /dev/zero > outside/target.bin
head -c 60000 /dev/zero > outside/dir/inner.bin
ln -s ../../outside/target.bin tree/links/tofile
ln -s ../../outside/dir tree/links/todir
ln -s ../nowhere tree/links/dangling
ln -s tree rootlink
# Fixed times everywhere; access times in the future, so reading never moves them.
find tree outside -exec touch -h -m -t 202401020304.05 {} +
touch -m -t 202502030405.06 tree/a/b/c/deep.bin
# -h: touching through the dangling link would create what it points to.
find tree outside -exec touch -h -a -t 203701010000 {} +
[ ! -e tree/nowhere ] || { echo "FAIL: the dangling link's target exists" >&2; exit 1; }
if [ "$(id -u)" != 0 ]; then
  mkdir tree/locked
  echo hidden > tree/locked/secret
  chmod 000 tree/locked
fi
printf 'node_modules\n*.txt\n' > excludes

failures=0
check() {
  # check LABEL [VAR=value]... -- ARGS...
  label=$1
  shift
  vars=
  while [ "$1" != "--" ]; do
    vars="$vars $1"
    shift
  done
  shift
  for threads in 1 8; do
    set +e
    # shellcheck disable=SC2086 # the assignments split on spaces, as meant
    env $vars "$du" "$@" > du.out 2> du.err
    du_status=$?
    # shellcheck disable=SC2086
    env $vars "$fastdu" -j "$threads" "$@" > fastdu.out 2> fastdu.err
    fastdu_status=$?
    set -e
    tr '\0' '\n' < du.out | LC_ALL=C sort > du.sorted
    tr '\0' '\n' < fastdu.out | LC_ALL=C sort > fastdu.sorted
    sed "s/^$du:/X:/" du.err | grep -v '^Try ' | LC_ALL=C sort > du.errs || true
    sed 's/^fastdu:/X:/' fastdu.err | grep -v '^Try ' | LC_ALL=C sort > fastdu.errs || true
    if ! cmp -s du.sorted fastdu.sorted || [ "$du_status" != "$fastdu_status" ] ||
      ! cmp -s du.errs fastdu.errs; then
      echo "FAIL: $label (-j $threads): exit du $du_status, fastdu $fastdu_status" >&2
      diff du.sorted fastdu.sorted | head -20 >&2 || true
      diff du.errs fastdu.errs | head -5 >&2 || true
      failures=$((failures + 1))
      return
    fi
  done
  echo "ok  $label"
}

run() { check "$@"; }
apparent() {
  if [ "$du_major" -ge 9 ]; then check "$@"; else echo "--  $1 (skipped: $du $du_major)"; fi
}

run "defaults" -- tree
run "-a" -- -a tree
run "-s" -- -s tree
run "-d 0" -- -d 0 tree
run "-d 1" -- -d 1 tree
run "--max-depth=2" -- --max-depth=2 tree
run "-a -d 2" -- -a -d 2 tree
run "-c" -- -c tree/a tree/many
run "-sc" -- -sc tree/a tree/hl tree/many
apparent "-b" -- -b tree
apparent "--apparent-size" -- --apparent-size tree
apparent "-ab" -- -ab tree
run "-k" -- -k tree
run "-m" -- -m tree
run "-B1" -- -B1 tree
run "-BM" -- -BM tree
run "-B 1K" -- -B 1K tree
run "-BKB" -- -BKB tree
run "--block-size=MiB" -- --block-size=MiB tree
run "-h" -- -h tree
run "-ah" -- -ah tree
run "-hs" -- -hs tree
run "--si" -- --si -a tree
apparent "-bh" -- -bh -a tree
run "-S" -- -S tree
run "-Sa" -- -Sa tree
run "-Sc" -- -Sc tree
run "-l" -- -l tree
run "-la" -- -la tree
run "-L" -- -L tree
run "-La" -- -La tree
run "-P" -- -P -a tree
run "symlink root" -- rootlink
run "-D symlink root" -- -D rootlink
run "-H symlink root" -- -H -s rootlink
run "-x" -- -x tree
run "-t 100K" -- -t 100K tree
run "-t -100K" -- -t -100K tree
run "-a -t 1M" -- -a --threshold=1M tree
run "--inodes" -- --inodes tree
run "--inodes -a" -- --inodes -a tree
run "--inodes -S" -- --inodes -S tree
run "--inodes -h" -- --inodes -h tree
run "--time" -- --time tree
run "--time -a" -- --time -a tree
run "--time=ctime" -- --time=ctime tree
run "--time=atime" -- --time=atime -a tree
run "--time-style=full-iso" -- --time --time-style=full-iso -a tree
run "--time-style=iso" -- --time --time-style=iso tree
run "--time-style=+FORMAT" -- --time '--time-style=+%Y/%m/%d_%H:%M:%S.%N' tree
run "TIME_STYLE" TIME_STYLE=full-iso -- --time -s tree
run "--exclude=node_modules" -- --exclude=node_modules tree
run "--exclude='*.log' -a" -- '--exclude=*.log' -a tree
run "--exclude=a/b" -- --exclude=a/b tree
run "-X" -- -X excludes -a tree
run "-0" -- -0 tree
run "-a0" -- -a0 tree
run "several roots" -- tree/a tree/hl tree/many
run "trailing slash" -- tree/a/
run "file root" -- tree/a/one.bin tree/sparse.bin
run "missing root" -- tree/missing tree/a
run "BLOCK_SIZE" BLOCK_SIZE=M -- tree
run "DU_BLOCK_SIZE" DU_BLOCK_SIZE=1K BLOCK_SIZE=M -- -a tree
run "POSIXLY_CORRECT" POSIXLY_CORRECT=1 -- -s tree
run "abbreviated --max-depth" -- --max=1 tree
run "-s with --max-depth" -- --summ --max-depth=1 tree
run "-s with -a" -- -s -a tree
run "bad option" -- --no-such-option tree

# --files0-from: the names from standard input.
printf 'tree/a\0tree/many\0' > names
check "--files0-from" -- --files0-from=names -s

# The extra column: size, file count, path; the count is the subtree's files.
out=$("$fastdu" --files -s tree/many)
[ "$(printf '%s' "$out" | cut -f2)" = 3000 ] || { echo "FAIL: --files: $out" >&2; failures=$((failures + 1)); }

if [ "$failures" -ne 0 ]; then
  echo "$failures failed" >&2
  exit 1
fi
echo "all passed"
