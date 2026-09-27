"""
Time fastdu and other disk-usage tools on one folder tree: a warm-up run of
each, then interleaved rounds, and each tool's median. Every tool stays on one
file system. Prints a Markdown table, and writes it to the GitHub Actions job
summary when there is one.

    python3 bench/run.py [--rounds N] PATH
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
TOOLS = {
    "fastdu": [FASTDU, "-d", "0"],
    "ncdu": ["ncdu", "-x", "-0", "-o", "/dev/null"],
    "gdu": ["gdu-go" if shutil.which("gdu-go") else "gdu", "-n", "-p", "-s", "-x"],
    "dua": ["dua", "-x", "aggregate"],
    "dust": ["dust", "-x", "-d", "0", "-b", "-P"],
    "du": ["du", "-sxk"],
}


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
    parser.add_argument("path")
    args = parser.parse_args()
    tools = {name: argv for name, argv in TOOLS.items() if shutil.which(argv[0])}
    counted = subprocess.run(
        [FASTDU, "-d", "0", args.path], capture_output=True, text=True
    ).stdout
    files = int(counted.split("\t")[1])
    for name, argv in tools.items():
        print(f"warm-up {name}: {run(argv, args.path):.1f} s", flush=True)
    times: dict[str, list[float]] = {name: [] for name in tools}
    for round_ in range(1, args.rounds + 1):
        for name, argv in tools.items():
            took = run(argv, args.path)
            times[name].append(took)
            print(f"round {round_} {name}: {took:.2f} s", flush=True)

    cpu = subprocess.run(
        ["sysctl", "-n", "machdep.cpu.brand_string"], capture_output=True, text=True
    )
    lines = [
        f"### `{args.path}`, {files:,} files, {os.cpu_count()} logical processors, {cpu.stdout.strip() or sys.platform}",
        "",
        f"Warm caches, {args.rounds} interleaved rounds, median.",
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
    table = "\n".join(lines) + "\n"
    print(table)
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a") as out:
            out.write(table)


main()
