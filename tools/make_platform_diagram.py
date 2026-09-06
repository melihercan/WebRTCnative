#!/usr/bin/env python3
"""Generate the platform-layer figure for the wiki as light and dark SVGs.

GitHub strips <style> blocks and style/class attributes from wiki markdown, so
the figure cannot be expressed as HTML+CSS there. An SVG committed to the wiki
repository and referenced from a <picture> element renders with full fidelity
in both themes.

    python tools/make_platform_diagram.py

Writes wiki/platform-layers-light.svg and wiki/platform-layers-dark.svg.
Fonts are system stacks: an SVG loaded through <img> cannot fetch a webfont.
"""

from __future__ import annotations

import os
from xml.sax.saxutils import escape

SANS = "system-ui,-apple-system,'Segoe UI',Roboto,Helvetica,Arial,sans-serif"
MONO = "ui-monospace,'Cascadia Code',Consolas,'DejaVu Sans Mono',monospace"

THEMES = {
    "light": {
        "ground": "#FAFBFC", "ink": "#101720", "ink2": "#4A5663", "ink3": "#78848F",
        "rule": "#DCE2E8", "sunken": "#F1F4F7", "accent": "#2A5C8A",
        "yes": "#1B7A4B", "yes_bg": "#E4F2EA",
        "partial": "#9A6B10", "partial_bg": "#FBF0DA",
        "host": "#5B4C86", "host_bg": "#EEEAF6",
        "core": "#55606C", "core_bg": "#ECEFF2",
    },
    "dark": {
        "ground": "#0E1319", "ink": "#E6EBF0", "ink2": "#9DAAB6", "ink3": "#74828F",
        "rule": "#27323D", "sunken": "#1A222B", "accent": "#7FB2E0",
        "yes": "#5BBE8A", "yes_bg": "#10281C",
        "partial": "#DDA94A", "partial_bg": "#2B2110",
        "host": "#A797D8", "host_bg": "#1E1A2C",
        "core": "#97A3AE", "core_bg": "#1E262F",
    },
}

# Mac Catalyst rather than macOS: Catalyst is what WebRTCme ships
# (net10.0-maccatalyst) and has its own workflow and binding. The macOS dylib is
# still built but nothing consumes it, so it does not earn a column here.
PLATFORMS = [
    ("Android", "WebRTCnative"),
    ("iOS", "WebRTCnative"),
    ("Mac Catalyst", "WebRTCnative"),
    ("Windows", "WebRTCnative"),
    ("Linux", "WebRTCnative"),
    ("Web", "browser vendor"),
]

# state: core | yes | partial | host | host_partial | none
TIERS = [
    (
        ("TIER 3", "Integration layer", "sdk/ · or the host"),
        [
            ("yes", "●", ["sdk/android", "Java + JNI"]),
            ("yes", "●", ["framework_objc", "Objective-C"]),
            ("yes", "●", ["WebRTC.xcframework", "catalyst: only"]),
            ("partial", "◐", ["WebRtcInterop", "ours, in progress"]),
            ("none", "—", ["none upstream"]),
            ("host", "●", ["the browser", "Chromium //media"]),
        ],
    ),
    (
        ("TIER 2", "Native media I/O", "modules/"),
        [
            ("none", "—", ["handled in tier 3"]),
            ("none", "—", ["handled in tier 3"]),
            ("none", "—", ["handled in tier 3"]),
            ("yes", "●", ["WASAPI · DirectShow", "screen"]),
            ("yes", "●", ["ALSA/Pulse · V4L2", "screen"]),
            ("host_partial", "◐", ["screen capture only", "rest from host"]),
        ],
    ),
    (
        ("TIER 1", "Core engine", "api/ pc/ call/ media/"),
        [("core", "●", ["identical"])] * 6,
    ),
]

LEGEND = [
    ("core", "Core — same source everywhere"),
    ("yes", "Present in the artifact we build"),
    ("host", "Supplied by the browser, not by us"),
    ("partial", "Being built here, incomplete"),
    ("none", "Not used on this platform"),
]

# geometry
PAD = 22
LABEL_W = 196
COL_W = 168
GAP = 8
ROW_H = 86
HEAD_H = 46
LEGEND_WRAP = 3          # captions before the legend wraps to a second row
LEGEND_H = 84
WIDTH = PAD * 2 + LABEL_W + GAP + len(PLATFORMS) * (COL_W + GAP) - GAP
HEIGHT = PAD * 2 + HEAD_H + len(TIERS) * (ROW_H + GAP) - GAP + LEGEND_H


def fill_stroke(state: str, t: dict) -> tuple[str, str, str, bool]:
    """Return (fill, stroke, text colour, dashed) for a cell state."""
    return {
        "core":         (t["core_bg"], t["rule"], t["core"], False),
        "yes":          (t["yes_bg"], t["yes"], t["yes"], False),
        "host":         (t["host_bg"], t["host"], t["host"], False),
        "host_partial": (t["host_bg"], t["host"], t["host"], True),
        "partial":      (t["partial_bg"], t["partial"], t["partial"], True),
        "none":         ("none", t["rule"], t["ink3"], True),
    }[state]


def text(x, y, s, *, fill, size, family=SANS, weight="400", anchor="start", spacing=None):
    extra = f' letter-spacing="{spacing}"' if spacing else ""
    return (
        f'<text x="{x}" y="{y}" fill="{fill}" font-family="{family}" '
        f'font-size="{size}" font-weight="{weight}" text-anchor="{anchor}"{extra}>'
        f"{escape(s)}</text>"
    )


def build(theme_name: str) -> str:
    t = THEMES[theme_name]
    o: list[str] = []
    o.append(
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{WIDTH}" height="{HEIGHT}" '
        f'viewBox="0 0 {WIDTH} {HEIGHT}" role="img" '
        f'aria-label="Which WebRTC layers each platform receives">'
    )
    o.append("<title>Which WebRTC layers each platform receives</title>")
    o.append(f'<rect width="{WIDTH}" height="{HEIGHT}" fill="{t["ground"]}"/>')

    col_x = [PAD + LABEL_W + GAP + i * (COL_W + GAP) for i in range(len(PLATFORMS))]

    # column headers
    for i, (name, by) in enumerate(PLATFORMS):
        cx = col_x[i] + COL_W / 2
        colour = t["host"] if name == "Web" else t["ink2"]
        sub = t["host"] if name == "Web" else t["ink3"]
        o.append(text(cx, PAD + 20, name, fill=colour, size=14, family=MONO,
                      weight="600", anchor="middle"))
        o.append(text(cx, PAD + 36, by, fill=sub, size=10.5, family=MONO, anchor="middle"))
    o.append(text(PAD, PAD + 20, "Layer", fill=t["ink2"], size=14, family=MONO, weight="600"))

    # tier rows
    y = PAD + HEAD_H
    for (tier_no, tier_name, tier_path), cells in TIERS:
        o.append(
            f'<rect x="{PAD}" y="{y}" width="{LABEL_W}" height="{ROW_H}" rx="3" '
            f'fill="{t["sunken"]}" stroke="{t["rule"]}"/>'
        )
        o.append(text(PAD + 15, y + 26, tier_no, fill=t["ink3"], size=10.5,
                      family=MONO, spacing="1.1"))
        o.append(text(PAD + 15, y + 47, tier_name, fill=t["ink"], size=14.5, weight="600"))
        o.append(text(PAD + 15, y + 66, tier_path, fill=t["accent"], size=11, family=MONO))

        for i, (state, mark, lines) in enumerate(cells):
            fill, stroke, ink, dashed = fill_stroke(state, t)
            dash = ' stroke-dasharray="4 3"' if dashed else ""
            o.append(
                f'<rect x="{col_x[i]}" y="{y}" width="{COL_W}" height="{ROW_H}" rx="3" '
                f'fill="{fill}" stroke="{stroke}"{dash}/>'
            )
            cx = col_x[i] + COL_W / 2
            n = len(lines)
            mark_y = y + (30 if n > 1 else 38)
            o.append(text(cx, mark_y, mark, fill=ink, size=15, weight="700", anchor="middle"))
            for j, line in enumerate(lines):
                o.append(text(cx, mark_y + 18 + j * 13, line, fill=ink, size=10,
                              family=MONO, anchor="middle"))
        y += ROW_H + GAP

    # Legend, wrapped onto two rows. Five captions on one line measure close
    # enough to the canvas width that a wider font stack would clip the last one.
    ly = y + 20
    lx = PAD
    for index, (state, caption) in enumerate(LEGEND):
        if index == LEGEND_WRAP:
            lx = PAD
            ly += 22
        fill, stroke, _, dashed = fill_stroke(state, t)
        dash = ' stroke-dasharray="3 2"' if dashed else ""
        o.append(
            f'<rect x="{lx}" y="{ly - 11}" width="14" height="14" rx="3" '
            f'fill="{fill}" stroke="{stroke}"{dash}/>'
        )
        o.append(text(lx + 21, ly, caption, fill=t["ink2"], size=12.5))
        lx += 30 + len(caption) * 7.0

    o.append("</svg>")
    return "\n".join(o)


def main() -> None:
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_dir = os.path.join(here, "wiki")
    for name in THEMES:
        path = os.path.join(out_dir, f"platform-layers-{name}.svg")
        with open(path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(build(name) + "\n")
        print(f"wrote {path}")


if __name__ == "__main__":
    main()
