"""
The README's benchmark chart, `bench/benchmarks.svg`: files counted per
second, Linux and Windows side by side for each tool, from the timings in the
README's table. Light and dark follow the viewer's colour scheme.

    python3 bench/chart.py
"""

import os

WINDOWS_FILES = 1_856_491  # all of C:\
LINUX_FILES = 1_150_000  # a source tree

# tool: (Linux seconds, Windows seconds); a range's middle; None: no build.
TIMES = {
    "fastdu": (1.5, 13.1),
    "ncdu": (2.5, None),
    "gdu": (2.9, 16.2),
    "diskus": (5.4, 61),
    "dua": (15.05, 17.6),
    "GNU du": (17, None),
    "pdu": (30.5, 79.5),
    "dust": (34.5, 74),
    "Sysinternals du": (None, 574),
}

WIDTH = 900
LEFT, RIGHT, TOP = 130, 130, 64
BAR, GAP, GROUP_GAP = 14, 2, 12
SCALE = 800_000
PLOT = WIDTH - LEFT - RIGHT
GROUP = 2 * BAR + GAP + GROUP_GAP
HEIGHT = TOP + len(TIMES) * GROUP + 28

STYLE = """
text { font: 12px -apple-system, "Segoe UI", Helvetica, Arial, sans-serif; fill: #1f2328; }
.title { font-size: 15px; font-weight: 600; }
.tick, .none { fill: #59636e; font-size: 11px; }
.grid { stroke: #d1d9e0; }
.linux { fill: #e95420; }
.windows { fill: #0078d4; }
@media (prefers-color-scheme: dark) {
  text { fill: #f0f6fc; }
  .tick, .none { fill: #9198a1; }
  .grid { stroke: #3d444d; }
}
"""


def main() -> None:
    out = [
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {WIDTH} {HEIGHT}" width="{WIDTH}" height="{HEIGHT}" role="img" aria-label="Files counted per second by fastdu and other disk-usage tools on Linux and Windows">',
        f"<style>{STYLE}</style>",
        '<text class="title" x="16" y="24">Files counted per second (longer is faster)</text>',
        f'<rect class="linux" x="{LEFT}" y="36" width="11" height="11"/><text x="{LEFT + 16}" y="46">Linux, a source tree (1.15 M files)</text>',
        f'<rect class="windows" x="{LEFT + 250}" y="36" width="11" height="11"/><text x="{LEFT + 266}" y="46">Windows, all of C:\\ (1.86 M files)</text>',
    ]
    for tick in range(0, SCALE + 1, 100_000):
        x = LEFT + PLOT * tick / SCALE
        out.append(
            f'<line class="grid" x1="{x:.0f}" y1="{TOP - 4}" x2="{x:.0f}" y2="{HEIGHT - 24}"/>'
        )
        out.append(
            f'<text class="tick" x="{x:.0f}" y="{HEIGHT - 10}" text-anchor="middle">{tick // 1000}k</text>'
        )
    for i, (name, seconds) in enumerate(TIMES.items()):
        y = TOP + i * GROUP
        weight = ' font-weight="600"' if name == "fastdu" else ""
        out.append(
            f'<text x="{LEFT - 8}" y="{y + BAR + 5}" text-anchor="end"{weight}>{name}</text>'
        )
        for j, (took, files, kind) in enumerate(
            [(seconds[0], LINUX_FILES, "linux"), (seconds[1], WINDOWS_FILES, "windows")]
        ):
            top = y + j * (BAR + GAP)
            if took is None:
                out.append(
                    f'<text class="none" x="{LEFT + 4}" y="{top + 11}">no build</text>'
                )
                continue
            rate = files / took
            width = max(PLOT * rate / SCALE, 2)
            out.append(
                f'<rect class="{kind}" x="{LEFT}" y="{top}" width="{width:.1f}" height="{BAR}" rx="2"/>'
            )
            out.append(
                f'<text x="{LEFT + width + 5:.1f}" y="{top + 11}">{rate / 1000:,.0f}k/s · {took:g} s</text>'
            )
    out.append("</svg>")
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "benchmarks.svg")
    with open(path, "w") as svg:
        svg.write("\n".join(out) + "\n")


main()
