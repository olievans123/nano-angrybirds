#!/usr/bin/env python3
"""Convert Angry Birds 1.0's data folder into what the nano port loads.

The game logic (Lua bytecode), levels, text and font tables are copied as they are. Sprite
sheets are rebuilt: every sprite is cut out of its sheet, scaled by --scale (the nano
draws the 320-pixel-high game on a 240-pixel-high panel, so 0.75 gives one texel per
pixel), and packed into power-of-two 16-bit pages the nano's GL driver accepts (no PNG or
PVRTC decoding on the device, no thin textures).

Output (little-endian):
  <SHEET>.sht   index:  'ABS1', u16 nsprites, u16 npages, u16 image_w, u16 image_h, f32 scale
                pages:  npages x { u16 w, u16 h, u8 fmt (0 = RGB565, 1 = RGBA4444), u8 repeat, u16 0 }
                sprites: nsprites x { char name[40]; i16 x, y (position in the original sheet);
                                      u16 w, h; i16 pivot_x, pivot_y; u16 page; u16 tx, ty, tw, th }
  <SHEET>.pN    page N: raw u16 pixels, w*h of them
"""
import argparse, os, shutil, struct, sys
import numpy as np
from PIL import Image

try:
    import texture2ddecoder
except ImportError:
    texture2ddecoder = None

GUTTER = 1


def load_pvr(path):
    d = open(path, 'rb').read()
    hlen, h, w, mips, flags, size, bpp, rm, gm, bm, am, magic, nsurf = struct.unpack_from('<13I', d, 0)
    assert magic == 0x21525650, 'not a PVR v2 file: ' + path
    fmt = flags & 0xFF
    body = d[hlen:]
    if fmt == 0x10:                                   # OGL_RGBA_4444
        px = np.frombuffer(body[:w * h * 2], dtype='<u2').reshape(h, w)
        out = np.zeros((h, w, 4), np.uint8)
        for i, sh in enumerate((12, 8, 4, 0)):
            out[..., i] = ((px >> sh) & 15) * 17
        return Image.fromarray(out, 'RGBA')
    if fmt == 0x19:                                   # OGL_PVRTC4 (first mip level only)
        if texture2ddecoder is None:
            sys.exit('PVRTC texture found: pip install texture2ddecoder')
        raw = texture2ddecoder.decode_pvrtc(body[:w * h // 2], w, h, 0)
        return Image.frombytes('RGBA', (w, h), raw, 'raw', 'BGRA')
    sys.exit('unsupported PVR pixel format 0x%x in %s' % (fmt, path))


def read_sheet(path):
    d = open(path, 'rb').read()
    pos = 0

    def u16():
        nonlocal pos
        v = struct.unpack_from('>H', d, pos)[0]
        pos += 2
        return v

    def s16():
        nonlocal pos
        v = struct.unpack_from('>h', d, pos)[0]
        pos += 2
        return v

    def string():
        nonlocal pos
        n = u16()
        v = d[pos:pos + n].decode('latin1')
        pos += n
        return v

    image = string()
    sprites = []
    for _ in range(u16()):
        name = string()
        x, y, w, h = u16(), u16(), u16(), u16()
        px, py = s16(), s16()
        sprites.append(dict(name=name, x=x, y=y, w=w, h=h, px=px, py=py))
    assert pos == len(d), path
    return image, sprites


def read_font(path):
    """Bitmap font .dat: image name, leading, tracking, then glyphs (code, rect, baseline).
    Glyphs become sprites named by their decimal character code, pivot (0, baseline)."""
    d = open(path, 'rb').read()
    n = struct.unpack_from('>H', d, 0)[0]
    image = d[2:2 + n].decode('latin1')
    pos = 2 + n
    leading, tracking, count = struct.unpack_from('>hhh', d, pos)
    pos += 6
    sprites = []
    for _ in range(count):
        code, x, y, w, h, base = struct.unpack_from('>Hhhhhh', d, pos)
        pos += 12
        sprites.append(dict(name='%u' % code, x=x, y=y, w=w, h=h, px=0, py=base))
    assert pos == len(d), path
    return image, sprites, leading, tracking


def scale_rgba(im, w, h):
    """Resize. The game's images are stored with premultiplied alpha (it blends with
    GL_ONE, GL_ONE_MINUS_SRC_ALPHA), so the channels can be filtered as they are."""
    if im.size == (w, h):
        return im
    a = np.asarray(im, np.float32) / 255.0
    chans = [Image.fromarray(a[..., i]).resize((w, h), Image.LANCZOS) for i in range(4)]
    out = np.clip(np.stack([np.asarray(c) for c in chans], axis=-1), 0, 1)
    out[..., :3] = np.minimum(out[..., :3], out[..., 3:4])     # stay premultiplied after ringing
    return Image.fromarray((out * 255 + 0.5).astype(np.uint8), 'RGBA')


BAYER = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]], np.float32) / 16.0 - 0.5


def quantize(rgba, fmt):
    """8-bit RGBA -> u16 array, ordered-dithered so gradients don't band."""
    h, w = rgba.shape[:2]
    d = np.tile(BAYER, (h // 4 + 1, w // 4 + 1))[:h, :w]
    f = rgba.astype(np.float32)

    def q(c, bits, dither=True):
        step = 255.0 / ((1 << bits) - 1)
        v = f[..., c] / step + (d if dither else 0)
        return np.clip(np.floor(v + 0.5), 0, (1 << bits) - 1).astype(np.uint16)

    if fmt == 0:
        return (q(0, 5) << 11) | (q(1, 6) << 5) | q(2, 5)
    # keep fully transparent / fully opaque texels exact
    a = q(3, 4)
    a = np.where(rgba[..., 3] == 0, 0, np.where(rgba[..., 3] == 255, 15, a)).astype(np.uint16)
    r, g, b = (np.minimum(q(c, 4), a) for c in range(3))       # premultiplied: colour <= alpha
    return (r << 12) | (g << 8) | (b << 4) | a


def pot(n):
    p = 1
    while p < n:
        p *= 2
    return p


def pack(rects, max_size):
    """Pack (w, h) rects (gutter included) into power-of-two pages, trying several page
    shapes and keeping the layout with the least texture memory."""
    best = None
    size = max_size
    shapes = []
    while size >= 128:
        shapes += [(size, size), (size, size // 2)]
        size //= 2
    for mw, mh in shapes:
        if any(w > mw or h > mh for w, h in rects):
            continue
        placed, sizes = pack_shelves(rects, mw, mh)
        # texture memory, plus a charge per page (each page is a file read and a draw-batch break)
        cost = sum(w * h for w, h in sizes) + 65536 * len(sizes)
        if best is None or cost < best[0]:
            best = (cost, placed, sizes)
    assert best, 'sprite larger than a page'
    return best[1], best[2]


def pack_shelves(rects, max_w, max_h):
    """Shelf-pack into pages of at most max_w x max_h. Returns [(page, x, y)], [page (w, h)]."""
    order = sorted(range(len(rects)), key=lambda i: (-rects[i][1], -rects[i][0]))
    pages, out = [], [None] * len(rects)
    for i in order:
        w, h = rects[i]
        for pi, pg in enumerate(pages):
            placed = False
            for sh in pg['shelves']:
                if h <= sh['h'] and sh['x'] + w <= max_w:
                    out[i] = (pi, sh['x'], sh['y'])
                    sh['x'] += w
                    placed = True
                    break
            if not placed and pg['y'] + h <= max_h:
                pg['shelves'].append(dict(x=w, y=pg['y'], h=h))
                out[i] = (pi, 0, pg['y'])
                pg['y'] += h
                placed = True
            if placed:
                break
        else:
            pages.append(dict(shelves=[dict(x=w, y=0, h=h)], y=h))
            out[i] = (len(pages) - 1, 0, 0)
    sizes = []
    for pi, pg in enumerate(pages):
        used_w = max(sh['x'] for sh in pg['shelves'])
        pw, ph = pot(used_w), pot(pg['y'])
        # the nano's GL driver dislikes very thin textures: keep the aspect within 4:1
        while pw > ph * 4:
            ph *= 2
        while ph > pw * 4:
            pw *= 2
        sizes.append((pw, ph))
    return out, sizes


def convert_sheet(src_dir, dat, out_dir, scale, max_page, repeat_sprites, stats, font=False):
    if font:
        image_name, sprites, leading, tracking = read_font(os.path.join(src_dir, dat))
        open(os.path.join(out_dir, dat[:-4] + '.fnt'), 'wb').write(struct.pack('<hh', leading, tracking))
    else:
        image_name, sprites = read_sheet(os.path.join(src_dir, dat))
    path = os.path.join(src_dir, image_name)
    im = load_pvr(path) if image_name.lower().endswith('.pvr') else Image.open(path).convert('RGBA')
    base = dat[:-4]
    cut, rects = [], []
    parent = {}
    for i, s in enumerate(sprites):
        for j, o in enumerate(sprites):
            if j == i or j in parent or o['name'] in repeat_sprites or s['name'] in repeat_sprites:
                continue
            inside = (o['x'] <= s['x'] and o['y'] <= s['y'] and s['x'] + s['w'] <= o['x'] + o['w']
                      and s['y'] + s['h'] <= o['y'] + o['h'])
            bigger = o['w'] * o['h'] > s['w'] * s['h'] or (o['w'] * o['h'] == s['w'] * s['h'] and j < i)
            if inside and bigger:
                parent[i] = j
                break
    for i, s in enumerate(sprites):
        if i in parent:
            cut.append(None)
            rects.append('child')
            continue
        crop = im.crop((s['x'], s['y'], s['x'] + s['w'], s['y'] + s['h']))
        if s['name'] in repeat_sprites:
            # tiled with GL_REPEAT: needs a page of its own, power-of-two, no gutter
            tw, th = pot(max(1, round(s['w'] * scale))), pot(max(1, round(s['h'] * scale)))
            if tw > s['w']: tw //= 2
            if th > s['h']: th //= 2
            cut.append(scale_rgba(crop, tw, th))
            rects.append(None)
            continue
        tw, th = max(1, round(s['w'] * scale)), max(1, round(s['h'] * scale))
        cut.append(scale_rgba(crop, tw, th))
        rects.append((tw + 2 * GUTTER, th + 2 * GUTTER))

    packed_idx = [i for i, r in enumerate(rects) if r is not None and r != 'child']
    placed, sizes = pack([rects[i] for i in packed_idx], max_page) if packed_idx else ([], [])
    pages = [np.zeros((h, w, 4), np.uint8) for (w, h) in sizes]
    repeat_flags = [0] * len(pages)
    entries = [None] * len(sprites)
    for k, i in enumerate(packed_idx):
        pi, x, y = placed[k]
        a = np.asarray(cut[i])
        th, tw = a.shape[:2]
        # edge-extend into the gutter so bilinear filtering never picks up a neighbour
        ext = np.pad(a, ((GUTTER, GUTTER), (GUTTER, GUTTER), (0, 0)), mode='edge')
        pages[pi][y:y + th + 2 * GUTTER, x:x + tw + 2 * GUTTER] = ext
        entries[i] = (pi, x + GUTTER, y + GUTTER, tw, th)
    for i, r in enumerate(rects):
        if r is None:
            a = np.asarray(cut[i])
            pages.append(a.copy())
            repeat_flags.append(1)
            entries[i] = (len(pages) - 1, 0, 0, a.shape[1], a.shape[0])

    for i, j in parent.items():
        s, o = sprites[i], sprites[j]
        pi, tx, ty, tw, th = entries[j]
        fx, fy = tw / o['w'], th / o['h']
        x0, y0 = tx + round((s['x'] - o['x']) * fx), ty + round((s['y'] - o['y']) * fy)
        entries[i] = (pi, x0, y0, max(1, round(s['w'] * fx)), max(1, round(s['h'] * fy)))

    out = bytearray(struct.pack('<4sHHHHf', b'ABS1', len(sprites), len(pages), im.size[0], im.size[1], scale))
    for pi, pg in enumerate(pages):
        fmt = 1 if (pg[..., 3] < 255).any() else 0
        # a page that is all gutter-transparent except opaque sprites still needs alpha
        px = quantize(pg, fmt)
        open(os.path.join(out_dir, '%s.p%d' % (base, pi)), 'wb').write(px.astype('<u2').tobytes())
        out += struct.pack('<HHBBH', pg.shape[1], pg.shape[0], fmt, repeat_flags[pi], 0)
        stats['bytes'] += pg.shape[0] * pg.shape[1] * 2
        stats['pages'] += 1
    for s, e in zip(sprites, entries):
        out += struct.pack('<40shhHHhhHHHHH', s['name'].encode('latin1'), s['x'], s['y'], s['w'], s['h'],
                           s['px'], s['py'], e[0], e[1], e[2], e[3], e[4])
    open(os.path.join(out_dir, base + '.sht'), 'wb').write(out)
    return base, len(sprites), [(p.shape[1], p.shape[0]) for p in pages]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src', help="the app's data folder (AngryBirds.app/data)")
    ap.add_argument('out')
    ap.add_argument('--scale', type=float, default=0.75)
    ap.add_argument('--max-page', type=int, default=1024)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    repeat = {'THEME_GROUND_TEXTURE_1', 'THEME_GROUND_TEXTURE_2', 'THEME_GROUND_TEXTURE_3'}
    stats = dict(bytes=0, pages=0)
    for name in sorted(os.listdir(args.src)):
        p = os.path.join(args.src, name)
        if name.endswith('.lua'):
            shutil.copy(p, os.path.join(args.out, name))
        elif name.endswith('.dat'):
            head = open(p, 'rb').read(64)
            n = struct.unpack_from('>H', head, 0)[0]
            img = head[2:2 + n]
            if name.startswith('TEXTS_'):
                shutil.copy(p, os.path.join(args.out, name))
                continue
            if name.startswith('FONT_'):
                base, count, pages = convert_sheet(args.src, name, args.out, args.scale, args.max_page, repeat, stats, font=True)
                print('%-16s %3d glyphs  -> %s' % (base, count, ' '.join('%dx%d' % p for p in pages)))
                continue
            if img.endswith((b'.pvr', b'.png')):
                base, count, pages = convert_sheet(args.src, name, args.out, args.scale, args.max_page, repeat, stats)
                print('%-16s %3d sprites -> %s' % (base, count, ' '.join('%dx%d' % p for p in pages)))
    print('%d pages, %.2f MB of texture at scale %g' % (stats['pages'], stats['bytes'] / 1048576.0, args.scale))


if __name__ == '__main__':
    main()
