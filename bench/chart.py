"""
The README's benchmark chart, `bench/benchmarks.svg`: files counted per
second, Linux, macOS and Windows side by side for each tool, from the timings
in the README's tables. Light and dark follow the viewer's colour scheme.

    python3 bench/chart.py
"""

import os

# system: (legend, files walked, bar colour)
SYSTEMS = {
    "linux": ("Linux, a source tree (1.15 M files)", 1_150_000, "#e95420"),
    "macos": ("macOS, Apple Silicon, all of / (3.24 M files)", 3_241_354, "#8250df"),
    "windows": ("Windows, all of C:\\ (1.86 M files)", 1_856_491, "#0078d4"),
}

NO_BUILD = None
NOT_MEASURED = "not measured"

# tool: seconds on (Linux, macOS, Windows); a range's middle.
TIMES = {
    "fastdu": (1.5, 74.12, 13.1),
    "gdu": (2.9, 45.25, 16.2),
    "ncdu": (2.5, 231.99, NO_BUILD),
    "dua": (15.05, 52.25, 17.6),
    "diskus": (5.4, NOT_MEASURED, 61),
    "du": (17, 139.42, NO_BUILD),
    "dust": (34.5, 132.84, 74),
    "pdu": (30.5, NOT_MEASURED, 79.5),
    "Sysinternals du": (NO_BUILD, NO_BUILD, 574),
}

WIDTH = 900
LEFT, RIGHT, TOP = 130, 130, 100
BAR, GAP, GROUP_GAP = 12, 2, 12
SCALE = 800_000
PLOT = WIDTH - LEFT - RIGHT
GROUP = len(SYSTEMS) * BAR + (len(SYSTEMS) - 1) * GAP + GROUP_GAP
HEIGHT = TOP + len(TIMES) * GROUP + 28

STYLE = """
text { font: 12px -apple-system, "Segoe UI", Helvetica, Arial, sans-serif; fill: #1f2328; }
.title { font-size: 15px; font-weight: 600; }
.tick, .none { fill: #59636e; font-size: 11px; }
.grid { stroke: #d1d9e0; }
@media (prefers-color-scheme: dark) {
  text { fill: #f0f6fc; }
  .tick, .none { fill: #9198a1; }
  .grid { stroke: #3d444d; }
}
"""


def main() -> None:
    out = [
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {WIDTH} {HEIGHT}" width="{WIDTH}" height="{HEIGHT}" role="img" aria-label="Files counted per second by fastdu and other disk-usage tools on Linux, macOS and Windows">',
        f"<style>{STYLE}</style>",
        '<text class="title" x="16" y="24">Files counted per second (longer is faster)</text>',
    ]
    for i, (legend, _, colour) in enumerate(SYSTEMS.values()):
        y = 38 + i * 16
        x = LEFT
        out.append(
            f'<rect x="{x}" y="{y}" width="11" height="11" fill="{colour}"/><text x="{x + 16}" y="{y + 10}">{legend}</text>'
        )
    for tick in range(0, SCALE + 1, 100_000):
        x = LEFT + PLOT * tick / SCALE
        out.append(
            f'<line class="grid" x1="{x:.0f}" y1="{TOP - 4}" x2="{x:.0f}" y2="{HEIGHT - 24}"/>'
        )
        out.append(
            f'<text class="tick" x="{x:.0f}" y="{HEIGHT - 10}" text-anchor="middle">{tick // 1000}k</text>'
        )
    middle = (len(SYSTEMS) * BAR + (len(SYSTEMS) - 1) * GAP) / 2
    for i, (name, seconds) in enumerate(TIMES.items()):
        y = TOP + i * GROUP
        weight = ' font-weight="600"' if name == "fastdu" else ""
        out.append(
            f'<text x="{LEFT - 8}" y="{y + middle + 4:.0f}" text-anchor="end"{weight}>{name}</text>'
        )
        for j, (took, (_, files, colour)) in enumerate(zip(seconds, SYSTEMS.values())):
            top = y + j * (BAR + GAP)
            if took is NO_BUILD or took == NOT_MEASURED:
                word = "no build" if took is NO_BUILD else "not measured"
                out.append(
                    f'<text class="none" x="{LEFT + 4}" y="{top + 10}">{word}</text>'
                )
                continue
            rate = files / took
            width = max(PLOT * rate / SCALE, 2)
            out.append(
                f'<rect x="{LEFT}" y="{top}" width="{width:.1f}" height="{BAR}" rx="2" fill="{colour}"/>'
            )
            out.append(
                f'<text x="{LEFT + width + 5:.1f}" y="{top + 10}">{rate / 1000:,.0f}k/s · {took:g} s</text>'
            )
    out.append("</svg>")
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "benchmarks.svg")
    with open(path, "w") as svg:
        svg.write("\n".join(out) + "\n")


main()
