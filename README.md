# fastdu

Folder sizes, fast, on Windows, Linux and macOS: a `du` that uses every core
and asks the file system as little as it can. Two small C files, one for
Windows and one for Linux and macOS, no dependencies.

All of `C:\` (1.86 million files) takes 13 seconds, where gdu takes 16 and dua
18. A 1.15-million-file source tree on Linux takes 1.5 seconds, where ncdu takes
2.5 and gdu 2.9 ([benchmarks](#benchmarks)).

## Install

Download a binary from [Releases](https://github.com/jimmy927/fastdu/releases/latest):

| System | File |
|---|---|
| Windows, x64 | `fastdu-windows-x64.exe` |
| Linux, x64 | `fastdu-linux-x64` (static) |
| Linux, arm64 | `fastdu-linux-arm64` (static) |
| macOS 11 or later, Intel and Apple Silicon | `fastdu-macos` |

```sh
curl -Lo fastdu https://github.com/jimmy927/fastdu/releases/latest/download/fastdu-linux-x64
chmod +x fastdu
```

`SHA256SUMS` beside them lists their checksums. Or [build it](#build).

The macOS binary is not notarised: fetched with `curl` it runs as it is, but
one downloaded in a browser is stopped by Gatekeeper until
`xattr -d com.apple.quarantine fastdu-macos`.

## Use

```
fastdu [-d DEPTH] [-m SIZE] [-j THREADS] [-h] [-p] [PATH...]
```

It prints each folder's size, file count and path, tab-separated, for every
folder under each `PATH` (default: the current folder):

```
$ fastdu -h -d 1 -m 1G 'C:\Program Files'
43G     151993  C:\Program Files
6.7G    11893   C:\Program Files\JetBrains
```

| Option | |
|---|---|
| `-d DEPTH` | only folders at most `DEPTH` below a `PATH` (`0`: the `PATH` alone) |
| `-m SIZE` | only folders of at least `SIZE` bytes; `K`, `M`, `G`, `T` are powers of 1024 |
| `-j THREADS` | threads to walk with (default: one per logical processor) |
| `-h` | sizes as `4.0K`, `12M`, `1.5G` |
| `-p` | `progress<TAB>folders<TAB>files<TAB>bytes` on standard error each second |

Folders come out in no particular order; `sort -n` (or `sort -h` with `-h`)
orders them. Without `-h` the output is meant for programs: bytes as a whole
number, one folder per line, paths in UTF-8. A path that cannot be read is
reported on standard error and makes the exit status 1; folders below it that
cannot be read count as empty.

What is counted differs between the systems, as each one's own tools do:

| | Windows | Linux and macOS |
|---|---|---|
| Size | file lengths, as Explorer shows them | allocated blocks, as `du` shows them |
| A file with several names | counted at each | counted once, like `du` |
| Other file systems mounted inside | — | not counted, like `du -x` |
| Links | junctions and symbolic links not followed | symbolic links not followed, except a `PATH` given as one |

On macOS, `/Users` and `/System/Volumes/Data/Users` are the same folders, both
on the same volume (a firmlink), so walking `/` counts them twice, as `du -x /`
does. A hard-linked file is still counted once; macOS's `du` counts it again
when it is met under more paths than it has links, so on `/` it can come out
higher than fastdu (by 100 MB of hard-linked `uv` caches on one Mac).

On Windows, `C:` means the drive's root; `\\?\` paths and `\\server\share`
paths work too.

## How it is fast

Opening a folder and listing it is cheap; asking the file system about every
file in it is what costs. Everything else is arranged around that.

### Windows

Windows' directory listing already carries each file's size, so a whole drive is
summed from the listings alone: no file is opened, and nothing is asked per
file.

- Each folder is opened relative to its parent's open handle
  (`NtOpenFile` with a `RootDirectory`), so the kernel looks up one name
  rather than parsing a whole path component by component.
- Folders are listed 256 KB at a time with `NtQueryDirectoryFile`, where
  `FindNextFile` returns small batches. Entries are read in place from that
  buffer; only folder names are copied, into per-thread memory arenas that are
  never freed before exit.
- A thread walks its own subtree depth-first, keeping its parents' handles
  open, and hands folders to a shared stack only while another thread is idle.

It is bound by the kernel: with 16 threads on 8 cores Windows sits at 100 % CPU,
75–97 % of it in the kernel (NTFS and Defender's file-system filter), and 16
threads are 5.2 times as fast as one. Opening a folder costs about 99 µs cold
and 21 µs warm.

### Linux

A Linux directory listing has names, inode numbers and types, but no sizes, so
files need a `stat`. Those stats cost 4.3 µs each here, 70 % of a one-thread
walk, so there are as few as possible:

- Folders are listed with `getdents64` into 1 MB buffers, and each file is
  stat-ed with `fstatat` relative to its folder's descriptor.
- A folder is never stat-ed from its parent: its own blocks and file system
  come from `fstat` on the descriptor it is listed through.
- A file whose inode number (which the listing gives for free) was already seen
  is another name for a file already counted, and is skipped without a stat. A
  source tree with `node_modules` hard-linked between checkouts had 2.49 million
  names for 1.15 million files.
- A listing that leaves the buffer part-empty was the whole folder, so the
  call that would only say "end" is skipped.

Those three took one thread from 8.45 s to 5.37 s on that tree. Work is shared
between threads as on Windows.

### macOS

macOS's `getattrlistbulk` lists a folder and returns, with each name, whatever
attributes are asked for: here its kind, file ID, link count and allocated size.
So, as on Windows, nothing is stat-ed per file, and only a file with more than
one link is looked up in the set of files already counted. Folders are walked
and shared between threads as on Linux (the same source, `src/unix/fastdu.c`).

It starts with one thread per logical processor and adds more while they mostly
wait on the disk: every 100 ms, if every thread is walking and together they
used under half their processor time, a processor's worth more start, up to 8
per processor. On a 3-processor Apple Silicon runner that took all of `/` from
43.6 s (3 threads) to about 30 s; on a Mac whose disk answers from memory the
threads stay busy and none are added.

### What did not help

- **Ending a Windows listing at a short batch.** NTFS returns batches that leave
  the buffer part-empty in the middle of a folder: stopping at one counted
  1,171,304 files on `C:\` where there were 1,856,491. Only "no more files" ends
  a listing.
- **Opening Windows folders by file ID** (`FILE_OPEN_BY_FILE_ID`, from the
  listing's IDs): 0.63 s against 0.34 s on `C:\Program Files`.
- **Handing work out more or less eagerly**: no difference.
- **`statx` instead of `fstatat`**: no difference.
- **Stat-ing a folder's files in inode order**: slower.

## Benchmarks

![Files counted per second by each tool on Linux, macOS and Windows: fastdu 767k/s on Linux, 109k/s on macOS and 142k/s on Windows, ahead of every other tool measured on each](bench/benchmarks.svg)

Where a tool's time varied between rounds, the chart shows the middle of its
range. `python3 bench/chart.py` redraws it.

**Linux and Windows**, on one laptop, 2026-09-27: AMD Ryzen 9 5900HS (8 cores,
16 threads), Windows 11 (build 26200) with Defender's real-time protection on,
and Linux in WSL 2 (kernel 6.18) on ext4. Five interleaved rounds with warm
caches, medians; the machine was busy with other work (load average 14–20), and
fastdu was fastest in every round. On a quiet machine it reads `C:\` in about
10.5 s.

**macOS**, on GitHub's Apple Silicon runner (Apple M1, virtual, 3 logical
processors), all of `/` (3.24 M files), 2026-09-27: a warm-up and three
interleaved rounds, medians ([`benchmark-macos`](.github/workflows/benchmark-macos.yml),
started by hand). Its disk is slow and does not all fit in memory, so these
are mostly waits on the disk; fastdu was ahead in two of the three rounds.

The tools: [gdu](https://github.com/dundee/gdu) 5,
[dua](https://github.com/Byron/dua-cli) 2.45 (which missed ~110 GiB of `C:\` to
permission errors), [ncdu](https://dev.yorhel.nl/ncdu) 2.9 with `-t 16`,
[diskus](https://github.com/sharkdp/diskus) 0.9,
[dust](https://github.com/bootandy/dust) 1.2,
[pdu](https://github.com/KSXGitHub/parallel-disk-usage) 0.24,
[Sysinternals du](https://learn.microsoft.com/sysinternals/downloads/du), and
the systems' own `du`.

Tools that read NTFS's master file table directly, such as WizTree and
Everything, are faster still on Windows, but need administrator rights and are
not command-line tools. fastdu needs no rights beyond reading the folders.

## Build

Linux and macOS, with any C compiler (on macOS, `xcode-select --install`):

```sh
make            # ./fastdu
make test       # compares it with du: to the byte on Linux, the KiB on macOS
sudo make install
```

Windows, with Visual Studio's C compiler (the free Build Tools will do), from an
"x64 Native Tools Command Prompt":

```bat
build.bat
powershell -ExecutionPolicy Bypass -File test\windows.ps1 -Fastdu .\fastdu.exe
```

Each tagged version `vX.Y.Z` is built, tested and released by
[GitHub Actions](.github/workflows/build.yml).

## Licence

[MIT](LICENSE)
