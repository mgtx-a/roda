# Draws res/app.ico: a teal ring of eight segments around a light hub. Run: python tools/make_icon.py
import math
from PIL import Image, ImageDraw

def draw(size):
    k = 8  # supersample
    S = size * k
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    c = S / 2
    r_out, r_in, hub = S * 0.48, S * 0.22, S * 0.16
    cols = [(31, 122, 116), (46, 158, 150)]
    for i in range(8):
        a0 = -90 + i * 45 - 22.5 + 3.5
        a1 = a0 + 45 - 7
        d.pieslice([c - r_out, c - r_out, c + r_out, c + r_out], a0, a1, fill=cols[i % 2] + (255,))
    d.ellipse([c - r_in, c - r_in, c + r_in, c + r_in], fill=(0, 0, 0, 0))
    d.ellipse([c - hub, c - hub, c + hub, c + hub], fill=(236, 242, 243, 255))
    return img.resize((size, size), Image.LANCZOS)

sizes = [16, 20, 24, 32, 40, 48, 64, 256]
big = draw(256)
big.save("res/app.ico", sizes=[(s, s) for s in sizes], append_images=[draw(s) for s in sizes[:-1]])
print("res/app.ico written")
