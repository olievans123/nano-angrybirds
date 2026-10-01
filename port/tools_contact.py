import sys, glob
from PIL import Image
d = sys.argv[1]
fs = sorted(f for f in glob.glob(d + '/*.png') if not f.endswith('contact.png'))
ims = [Image.open(f) for f in fs]
w, h = ims[0].size
cols = int(sys.argv[2]) if len(sys.argv) > 2 else 3
sheet = Image.new('RGB', (w * cols + 10 * (cols - 1), h * ((len(ims) + cols - 1) // cols) + 10 * ((len(ims) - 1) // cols)), (40, 40, 40))
for i, im in enumerate(ims):
    sheet.paste(im, ((i % cols) * (w + 10), (i // cols) * (h + 10)))
sheet.save(d + '/contact.png')
print(' '.join(f.split('/')[-1] for f in fs))
