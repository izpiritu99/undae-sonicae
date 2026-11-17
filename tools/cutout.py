#!/usr/bin/env python3
"""Cut artwork off a dark background into a transparent PNG (Pillow only).

usage: cutout.py in.jpg out.png left,top,right,bottom [--square] [--size N] [--height N]

The solid artwork stays opaque; the glow around it becomes partial transparency so it blends on any backdrop.
"""
import sys
from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageMath

src, dst, box = sys.argv[1], sys.argv[2], [int(v) for v in sys.argv[3].split(',')]
opt = sys.argv[4:]
num = lambda flag: int(opt[opt.index(flag) + 1]) if flag in opt else 0

im = Image.open(src).convert('RGB').crop(box)
r, g, b = im.split()
lum = ImageChops.lighter(ImageChops.lighter(r, g), b)                      # brightest channel = how much light a pixel carries

# 1. solid shape: everything the dark backdrop cannot reach from the edges
warm = ImageChops.lighter(r, g)                                           # the haze around the art is pure blue, the art is not
solid = warm.filter(ImageFilter.GaussianBlur(2)).point(lambda v: 255 if v > 62 else 0)
solid = solid.filter(ImageFilter.MaxFilter(41)).filter(ImageFilter.MinFilter(41))   # close hairline gaps in the outline
pad = Image.new('L', (solid.width + 2, solid.height + 2), 0); pad.paste(solid, (1, 1))
ImageDraw.floodfill(pad, (0, 0), 128)
inside = pad.crop((1, 1, solid.width + 1, solid.height + 1)).point(lambda v: 0 if v == 128 else 255)
inside = inside.filter(ImageFilter.MinFilter(9)).filter(ImageFilter.MaxFilter(9))   # drop stars and specks
core = inside.filter(ImageFilter.MinFilter(3)).filter(ImageFilter.GaussianBlur(1.2))

# 2. glow: light near the shape, keyed by brightness
near = inside.filter(ImageFilter.MaxFilter(17)).filter(ImageFilter.GaussianBlur(12))
glow = ImageChops.multiply(lum.point(lambda v: max(0, min(255, int((v - 64) * 255 / 150)))), near)
alpha = ImageChops.lighter(core, glow)

# 3. remove the black the glow was blended with, so edges do not carry a dark fringe
un = lambda c: ImageMath.lambda_eval(lambda a: a['convert'](a['min'](a['c'] * 255.0 / a['max'](a['a'], 1), 255), 'L'), c=c.convert('F'), a=alpha.convert('F'))
out = Image.merge('RGBA', (un(r), un(g), un(b), alpha))

bb = alpha.point(lambda v: 255 if v > 8 else 0).getbbox()
out = out.crop(bb)
if '--square' in opt:
    s = max(out.size); sq = Image.new('RGBA', (s, s), (0, 0, 0, 0)); sq.paste(out, ((s - out.width) // 2, (s - out.height) // 2)); out = sq
if num('--size'): out = out.resize((num('--size'),) * 2, Image.LANCZOS)
if num('--height'): h = num('--height'); out = out.resize((round(out.width * h / out.height), h), Image.LANCZOS)
out.save(dst, optimize=True)
print(dst, out.size)
