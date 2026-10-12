#!/usr/bin/env python3
"""Draws TubeSearch's icons (an old telly with a play button) as PNGs:
     tools/mkicons.py OUTDIR    -> OUTDIR/tubesearch.png (34x34), sm-tubesearch.png (17x17)
Drawn 8 times bigger and scaled down, so the edges are smooth before
png2sprite turns alpha into a 1-bit mask. Part of TubeSearch. MIT licence."""
import sys
from PIL import Image, ImageDraw


def telly(size):
    s = 8
    W = size * s
    im = Image.new('RGBA', (W, W), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    u = W / 34.0
    # aerials
    d.line([(17 * u, 9 * u), (10 * u, 2 * u)], fill=(60, 60, 70, 255), width=int(2 * u))
    d.line([(17 * u, 9 * u), (24 * u, 2 * u)], fill=(60, 60, 70, 255), width=int(2 * u))
    # cabinet, screen
    d.rounded_rectangle([1 * u, 8 * u, 33 * u, 32 * u], radius=4 * u, fill=(150, 90, 40, 255),
                        outline=(80, 45, 20, 255), width=int(1.2 * u))
    d.rounded_rectangle([4 * u, 11 * u, 26 * u, 29 * u], radius=3 * u, fill=(30, 60, 120, 255),
                        outline=(15, 30, 60, 255), width=int(1 * u))
    # knobs
    d.ellipse([27.5 * u, 13 * u, 31 * u, 16.5 * u], fill=(235, 220, 180, 255))
    d.ellipse([27.5 * u, 19 * u, 31 * u, 22.5 * u], fill=(235, 220, 180, 255))
    # play
    d.polygon([(11.5 * u, 14.5 * u), (11.5 * u, 25.5 * u), (20.5 * u, 20 * u)], fill=(255, 255, 255, 255))
    return im.resize((size, size), Image.LANCZOS)


out = sys.argv[1]
telly(34).save(out + '/tubesearch.png')
telly(17).save(out + '/sm-tubesearch.png')
