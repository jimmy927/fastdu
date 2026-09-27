"""
The README's benchmark charts, one per system (`bench/benchmarks-<system>.svg`):
files counted per second, fastest first, from the timings in the README. Each
is drawn at the width of GitHub's README column, so its text is never scaled
down, and follows the viewer's light or dark colour scheme.

    python3 bench/chart.py
"""

import os

# Seconds; a range's middle. Only tools that ran on that system.
SYSTEMS = {
    "linux": {
        "title": "Linux: a source tree, 1.15 M files",
        "files": 1_150_000,
        "colour": "#e95420",
        "times": {
            "fastdu": 1.5,
            "ncdu": 2.5,
            "gdu": 2.9,
            "diskus": 5.4,
            "dua": 15.05,
            "GNU du": 17,
            "pdu": 30.5,
            "dust": 34.5,
        },
    },
    "macos": {
        "title": "macOS on Apple Silicon: all of /, 3.24 M files",
        "files": 3_241_660,
        "colour": "#8250df",
        "times": {"fastdu": 29.73, "dua": 30.13, "gdu": 34.92},
    },
    "windows": {
        "title": "Windows: all of C:\\, 1.86 M files",
        "files": 1_856_491,
        "colour": "#0078d4",
        "times": {
            "fastdu": 13.1,
            "gdu": 16.2,
            "dua": 17.6,
            "diskus": 61,
            "dust": 74,
            "pdu": 79.5,
            "Sysinternals du": 574,
        },
    },
}

WIDTH = 830
LEFT = 150  # tool names
RIGHT = 200  # the number after the longest bar
TOP = 48
BAR, ROW = 26, 36
PLOT = WIDTH - LEFT - RIGHT

STYLE = """
text { font-family: -apple-system, "Segoe UI", Helvetica, Arial, sans-serif; fill: #1f2328; }
.title { font-size: 18px; font-weight: 600; }
.name { font-size: 16px; }
.value { font-size: 15px; }
.other { fill: #8c959f; }
@media (prefers-color-scheme: dark) {
  text { fill: #f0f6fc; }
  .other { fill: #656c76; }
}
"""


def chart(system: dict) -> str:
    times = system["times"]
    fastest = system["files"] / min(times.values())
    height = TOP + len(times) * ROW + 8
    out = [
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {WIDTH} {height}" width="{WIDTH}" height="{height}" role="img" aria-label="Files counted per second, {system["title"]}">',
        f"<style>{STYLE}</style>",
        f'<text class="title" x="0" y="24">{system["title"]}</text>',
    ]
    for i, (name, took) in enumerate(sorted(times.items(), key=lambda kv: kv[1])):
        y = TOP + i * ROW
        rate = system["files"] / took
        width = max(PLOT * rate / fastest, 3)
        ours = name == "fastdu"
        fill = f'fill="{system["colour"]}"' if ours else 'class="other"'
        weight = ' font-weight="600"' if ours else ""
        out.append(
            f'<text class="name" x="{LEFT - 12}" y="{y + BAR / 2 + 6}" text-anchor="end"{weight}>{name}</text>'
        )
        out.append(
            f'<rect x="{LEFT}" y="{y}" width="{width:.1f}" height="{BAR}" rx="3" {fill}/>'
        )
        out.append(
            f'<text class="value" x="{LEFT + width + 8:.1f}" y="{y + BAR / 2 + 5}"{weight}>{rate / 1000:,.0f}k files/s · {took:.3g} s</text>'
        )
    out.append("</svg>")
    return "\n".join(out) + "\n"


def main() -> None:
    here = os.path.dirname(os.path.abspath(__file__))
    for key, system in SYSTEMS.items():
        with open(os.path.join(here, f"benchmarks-{key}.svg"), "w") as svg:
            svg.write(chart(system))


main()
