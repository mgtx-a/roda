# Makes tests/corpus: small sample files of every image format Roda reads, plus awkward ones.
# Needs Pillow (dev machine only; the corpus itself is committed, so CI doesn't run this).
#   python tests/make_corpus.py
import os
import zipfile
from PIL import Image, ImageDraw, features

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "corpus")
os.makedirs(OUT, exist_ok=True)


def photo(w=640, h=480):
    """Something photo-like: gradients and shapes, so lossy formats have work to do."""
    im = Image.new("RGB", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            px[x, y] = (x * 255 // w, y * 255 // h, (x + y) * 255 // (w + h))
    d = ImageDraw.Draw(im)
    d.ellipse((w // 4, h // 4, w * 3 // 4, h * 3 // 4), fill=(240, 200, 40))
    d.rectangle((20, 20, 120, 90), fill=(20, 60, 200))
    return im


def drawing(w=300, h=200):
    """A transparent drawing with soft edges."""
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.ellipse((10, 10, w - 10, h - 10), fill=(31, 122, 116, 255))
    d.ellipse((60, 40, w - 60, h - 40), fill=(255, 255, 255, 128))
    return im


def path(name):
    return os.path.join(OUT, name)


photo().save(path("photo.jpg"), quality=90)
photo(320, 240).save(path("photo.png"))
drawing().save(path("transparent.png"))
photo(200, 150).save(path("photo.bmp"))
photo(200, 150).save(path("photo.tiff"))
drawing().save(path("icon.ico"), sizes=[(16, 16), (32, 32), (48, 48), (128, 128)])
assert features.check("webp"), "Pillow without WebP"
photo(320, 240).save(path("photo.webp"), quality=80)
drawing().save(path("lossless.webp"), lossless=True)

# Animated GIF: Roda takes the first frame.
frames = [Image.new("P", (120, 80), i * 40) for i in range(4)]
frames[0].save(path("animated.gif"), save_all=True, append_images=frames[1:], duration=100, loop=0)

# CMYK JPEG (print files): WIC must turn it into RGB.
photo(200, 150).convert("CMYK").save(path("cmyk.jpg"), quality=90)

# 16-bit greyscale PNG.
Image.linear_gradient("L").resize((256, 64)).convert("I").point(lambda v: v * 257).convert("I;16").save(path("gray16.png"))

# A photo stored sideways with EXIF orientation 6 (rotate 90° clockwise to view): 200x100 stored, 100x200 shown.
side = photo(200, 100)
exif = Image.Exif()
exif[0x0112] = 6
side.save(path("rotated-exif.jpg"), exif=exif.tobytes(), quality=90)

# SVG with no pixel size of its own (only a viewBox) and one with a size.
with open(path("drawing.svg"), "w", encoding="utf-8") as f:
    f.write('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 50">'
            '<rect width="100" height="50" rx="8" fill="#1f7a74"/><circle cx="25" cy="25" r="15" fill="#fff"/></svg>')
with open(path("sized.svg"), "w", encoding="utf-8") as f:
    f.write('<svg xmlns="http://www.w3.org/2000/svg" width="800" height="600">'
            '<rect width="800" height="600" fill="#e8eef0"/><path d="M100 500 L400 100 L700 500 Z" fill="#b3261e"/></svg>')

# Awkward names and contents.
photo(64, 48).save(path("测试 файл.png"))
photo(64, 48).save(path("really-a-png.jpg"), format="PNG")   # wrong extension: the bytes decide
open(path("empty.png"), "wb").close()                          # zero bytes
with open(path("photo.png"), "rb") as f:
    data = f.read()
with open(path("truncated.png"), "wb") as f:
    f.write(data[: len(data) // 3])
with open(path("garbage.jpg"), "wb") as f:
    f.write(bytes((i * 37 + 11) & 255 for i in range(5000)))

# Archives to extract.
os.makedirs(path("folder/sub dir"), exist_ok=True)
with open(path("folder/notes.txt"), "w", encoding="utf-8") as f:
    f.write("Roda test file\n" * 20)
with open(path("folder/sub dir/über.txt"), "w", encoding="utf-8") as f:
    f.write("non-ASCII name\n")
with zipfile.ZipFile(path("sample.zip"), "w", zipfile.ZIP_DEFLATED) as z:
    z.write(path("folder/notes.txt"), "folder/notes.txt")
    z.write(path("folder/sub dir/über.txt"), "folder/sub dir/über.txt")
with zipfile.ZipFile(path("evil.zip"), "w") as z:   # tries to write outside its folder
    z.writestr("../escaped.txt", "should never be written outside")
    z.writestr("ok.txt", "fine")

print("corpus:", sum(os.path.getsize(os.path.join(dp, n)) for dp, _, ns in os.walk(OUT) for n in ns) // 1024, "KB")
