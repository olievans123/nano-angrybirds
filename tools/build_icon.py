#!/usr/bin/env python3
"""Frame the game's original red-bird sprite using the NanoApps icon builder."""
import argparse
import plistlib
import sys
from pathlib import Path

from PIL import Image

from convert_assets import load_pvr, read_sheet


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--sdk', type=Path, required=True, help='NanoApps checkout')
    ap.add_argument('--source', type=Path, required=True, help='AngryBirds.app/data')
    ap.add_argument('--out', type=Path, required=True, help='output icon.png (112 by 112)')
    args = ap.parse_args()
    sys.path.insert(0, str(args.sdk.resolve() / 'tools'))
    import build_apps

    image_name, sprites = read_sheet(args.source / 'INGAME.dat')
    sprite = next(s for s in sprites if s['name'] == 'BIRD_RED')
    path = args.source / image_name
    sheet = load_pvr(str(path)) if path.suffix.lower() == '.pvr' else Image.open(path).convert('RGBA')
    x, y, w, h = (sprite[k] for k in ('x', 'y', 'w', 'h'))
    if not (0 <= x < x + w <= sheet.width and 0 <= y < y + h <= sheet.height):
        raise ValueError('red-bird sprite is outside its texture')
    art = sheet.crop((x, y, x + w, y + h))
    # Game textures store premultiplied colors; Pillow's icon compositor expects
    # straight alpha. This preserves the original art, including its soft edges.
    art = Image.frombytes('RGBa', art.size, art.tobytes()).convert('RGBA')
    metadata = Path(__file__).resolve().parents[1] / 'port/nano/app/Info.plist'
    with metadata.open('rb') as stream:
        color = plistlib.load(stream)['HBIconColor'].removeprefix('#')
    rgb = tuple(int(color[i:i + 2], 16) for i in (0, 2, 4))
    args.out.parent.mkdir(parents=True, exist_ok=True)
    build_apps.make_png(None, rgb, args.out, art=art)
    print('Wrote', args.out)


if __name__ == '__main__':
    main()
