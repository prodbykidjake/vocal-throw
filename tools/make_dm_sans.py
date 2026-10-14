#!/usr/bin/env python3
"""Builds the static DM Sans TTFs in assets/fonts from Google's variable woff2.

DM Sans is licensed under the SIL Open Font License 1.1 (see assets/fonts/OFL.txt).
JUCE can't load woff2 or variable fonts, so we convert once and pin the weights we use.

  python3 -m venv venv && venv/bin/pip install fonttools brotli
  curl -o DMSans-latin-var.woff2 https://fonts.gstatic.com/s/dmsans/v17/rP2Yp2ywxg089UriI5-g4vlH9VoD8Cmcqbu0-K4.woff2
  venv/bin/python tools/make_dm_sans.py DMSans-latin-var.woff2 assets/fonts

This is the Latin subset Google serves to browsers (ASCII, Latin-1, common punctuation, U+2212 minus).
"""
import sys
from pathlib import Path

from fontTools.ttLib import TTFont
from fontTools.varLib import instancer

WEIGHTS = {400: "Regular", 500: "Medium", 600: "SemiBold", 700: "Bold"}


def main(source: str, out_dir: str) -> None:
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)

    for weight, style in WEIGHTS.items():
        font = TTFont(source)
        font.flavor = None  # woff2 -> plain TTF
        static = instancer.instantiateVariableFont(font, {"wght": weight}, updateFontNames=True)

        # Make the names unambiguous for every weight
        family = "DM Sans"
        full = f"{family} {style}"
        for rec in static["name"].names:
            if rec.nameID == 1:
                rec.string = family if style in ("Regular", "Bold") else f"{family} {style}"
            elif rec.nameID == 2:
                rec.string = "Bold" if style == "Bold" else "Regular"
            elif rec.nameID == 4:
                rec.string = full
            elif rec.nameID == 6:
                rec.string = f"DMSans-{style}"

        target = out / f"DMSans-{style}.ttf"
        static.save(target)
        print(f"wrote {target} ({target.stat().st_size} bytes)")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
