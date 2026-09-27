"""
Time fastdu and other disk-usage tools on one folder tree: a warm-up run of
each, then interleaved rounds, and each tool's median. Every tool stays on one
file system. Each run is written to the GitHub Actions job summary as it ends,
so a job that runs out of time still leaves what it measured; a tool whose run
took longer than --give-up seconds is left out of the rounds after.

    python3 bench/run.py [--rounds N] [--fastdu-threads 3,16,32] [--only gdu,dua] PATH
"""

import argparse
import os
import shutil
import statistics
import subprocess
import sys
import time

FASTDU = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "fastdu")

# name: the command, the path appended. Only those installed are run.
OTHERS = {
    "ncdu": ["ncdu", "-x", "-0", "-o", "/dev/null"],
    "gdu": ["gdu-go" if shutil.which("gdu-go") else "gdu", "-n", "-p", "-s", "-x"],
    "dua": ["dua", "-x", "aggregate"],
    "dust": ["dust", "-x", "-d", "0", "-b", "-P"],
    "du": ["du", "-sxk"],
}


def report(text: str) -> None:
    print(text, flush=True)
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a") as out:
            out.write(text + "\n")


def run(argv: list[str], path: str) -> float:
    started = time.monotonic()
    subprocess.run(
        argv + [path],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        timeout=1800,
    )
    return time.monotonic() - started


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument(
        "--fastdu-threads",
        default="",
        help='comma-separated; "default" is its own; empty: only its own',
    )
    parser.add_argument(
        "--only", default="", help="comma-separated other tools; default: all"
    )
    parser.add_argument("--give-up", type=float, default=300)
    parser.add_argument("path")
    args = parser.parse_args()

    # -x, as every other tool here stays on one file system.
    tools: dict[str, list[str]] = {}
    for threads in filter(None, args.fastdu_threads.split(",")):
        if threads == "default":
            tools["fastdu"] = [FASTDU, "-x", "-s"]
        else:
            tools[f"fastdu -j {threads}"] = [FASTDU, "-x", "-s", "-j", threads]
    if not tools:
        tools["fastdu"] = [FASTDU, "-x", "-s"]
    only = set(filter(None, args.only.split(",")))
    for name, argv in OTHERS.items():
        if (not only or name in only) and shutil.which(argv[0]):
            tools[name] = argv

    counted = subprocess.run(
        [FASTDU, "-x", "-s", "--files", args.path], capture_output=True, text=True
    ).stdout
    files = int(counted.split("\t")[1])
    cpu = subprocess.run(
        ["sysctl", "-n", "machdep.cpu.brand_string"], capture_output=True, text=True
    )
    report(
        f"### `{args.path}`, {files:,} files, {os.cpu_count()} logical processors, "
        f"{cpu.stdout.strip() or sys.platform}\n"
    )

    for name, argv in tools.items():
        report(f"- warm-up {name}: {run(argv, args.path):.1f} s")
    times: dict[str, list[float]] = {name: [] for name in tools}
    for round_ in range(1, args.rounds + 1):
        for name, argv in tools.items():
            if times[name] and times[name][-1] > args.give_up:
                continue
            took = run(argv, args.path)
            times[name].append(took)
            report(f"- round {round_} {name}: {took:.2f} s")

    lines = [
        "",
        f"{args.rounds} interleaved rounds after a warm-up, median.",
        "",
        "| Tool | Median | Files per second | Rounds |",
        "|---|---|---|---|",
    ]
    for name in sorted(times, key=lambda n: statistics.median(times[n])):
        median = statistics.median(times[name])
        rounds = ", ".join(f"{t:.2f}" for t in times[name])
        lines.append(
            f"| {name} | {median:.2f} s | {files / median / 1000:,.0f} k | {rounds} |"
        )
    report("\n".join(lines) + "\n")


main()
