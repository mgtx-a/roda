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

# ---------------------------------------------------------------- Phase 3: PDFs, documents, sheets, subtitles
# Needs reportlab, pypdf, python-docx and openpyxl. sample.xls is made from sample.xlsx by Excel itself
# (tests/make_xls.ps1, needs Excel; the file is committed).
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import getSampleStyleSheet
from reportlab.platypus import SimpleDocTemplate, Paragraph, ListFlowable, ListItem, PageBreak, Image as RLImage
from reportlab.pdfgen import canvas
from pypdf import PdfReader, PdfWriter
import docx
from docx.shared import Inches
import openpyxl
import datetime

st = getSampleStyleSheet()
photo(200, 120).save(path("tmp-pic.jpg"), quality=85)
story = [Paragraph("Roda Test Report", st["Title"]), Paragraph("Introduction", st["Heading1"]),
         Paragraph("This is the first paragraph of the document. It is long enough to wrap onto a second line in the PDF, "
                   "so the reader has to join the lines back together into one paragraph.", st["BodyText"]),
         Paragraph("A second paragraph with <b>bold</b> and <i>italic</i> words.", st["BodyText"]),
         ListFlowable([ListItem(Paragraph("First point", st["BodyText"])), ListItem(Paragraph("Second point", st["BodyText"]))], bulletType="bullet"),
         RLImage(path("tmp-pic.jpg"), width=200, height=120), PageBreak(),
         Paragraph("Details", st["Heading2"]), Paragraph("Page two text.", st["BodyText"]), PageBreak(),
         Paragraph("Page three.", st["BodyText"])]
SimpleDocTemplate(path("report.pdf"), pagesize=A4).build(story)
os.remove(path("tmp-pic.jpg"))

# 300 pages (the plan's "long PDF"), kept small: one line each.
c = canvas.Canvas(path("long.pdf"), pagesize=A4)
for i in range(300):
    c.drawString(72, 760, f"Page {i + 1} of 300")
    c.showPage()
c.save()

w = PdfWriter()
w.append(PdfReader(path("report.pdf")))
w.encrypt("secret")
with open(path("password.pdf"), "wb") as f:
    w.write(f)
with open(path("report.pdf"), "rb") as f:
    data = f.read()
with open(path("broken.pdf"), "wb") as f:
    f.write(data[: len(data) // 2])

d = docx.Document()
d.core_properties.title = "Roda sample"
d.add_heading("Roda sample", 1)
p = d.add_paragraph("Plain, ")
p.add_run("bold").bold = True
p.add_run(" and ")
p.add_run("italic").italic = True
p.add_run(" words.")
d.add_heading("A list", 2)
d.add_paragraph("Bullet one", style="List Bullet")
d.add_paragraph("Bullet two", style="List Bullet")
d.add_paragraph("Step one", style="List Number")
d.add_paragraph("Step two", style="List Number")
t = d.add_table(rows=2, cols=2)
t.cell(0, 0).text, t.cell(0, 1).text, t.cell(1, 0).text, t.cell(1, 1).text = "Name", "Value", "Width", "42"
photo(160, 100).save(path("tmp-pic.png"))
d.add_picture(path("tmp-pic.png"), width=Inches(1.6))
os.remove(path("tmp-pic.png"))
d.save(path("sample.docx"))

with open(path("sample.md"), "w", encoding="utf-8") as f:
    f.write("# Markdown sample\n\nSome *italic*, **bold** and `code`, and a [link](https://example.com).\n\n"
            "## List\n\n- one\n- two\n  - nested\n\n1. first\n2. second\n\n> A quote.\n\n"
            "```\ncode block\n  indented\n```\n\n| A | B |\n|---|---|\n| 1 | 2 |\n\n---\n\nThe end, with ünïcödé.\n")
with open(path("sample.html"), "w", encoding="utf-8") as f:
    f.write("<!DOCTYPE html><html><head><title>HTML sample</title><style>p{color:red}</style><script>var x = '<p>no</p>';</script></head>"
            "<body><h1>HTML sample</h1><p>Text with <b>bold</b>, <i>italic</i> &amp; an <a href=\"https://example.com\">link</a>.</p>"
            "<ul><li>alpha</li><li>beta<ul><li>inner</li></ul></li></ul><ol><li>uno</li><li>dos</li></ol>"
            "<table><tr><th>K</th><th>V</th></tr><tr><td>x</td><td>1</td></tr></table><pre>  pre\n  formatted</pre>"
            "<!-- a comment <b>not</b> shown --><p>Caf&eacute; &#8364;5</p></body></html>")
with open(path("sample.rtf"), "w", encoding="ascii") as f:
    B = chr(92)   # backslashes spelled out: RTF is all backslashes
    f.write("{{{B}rtf1{B}ansi{B}ansicpg1252{B}deff0{{{B}fonttbl{{{B}f0 Arial;}}}}{{{B}*{B}generator Roda test}}{B}f0{B}fs24 "
            "{{{B}pard{B}outlinelevel0{B}b{B}fs36 RTF sample{B}par}}"
            "{B}pard Plain, {{{B}b bold}} and {{{B}i italic}} words, caf{B}'e9 and {B}u8364? euro.{B}par "
            "Second line{B}line same paragraph.{B}par}}".format(B=B))
with open(path("sample.txt"), "w", encoding="utf-8") as f:
    f.write("First paragraph,\nstill the first.\n\nSecond paragraph.\n")

wb = openpyxl.Workbook()
s1 = wb.active
s1.title = "Data"
s1.append(["Name", "Count", "When", "OK", "Note"])
s1.append(["apple", 3, datetime.date(2024, 5, 17), True, 'has "quotes", commas'])
s1.append(["pear", 2.5, datetime.datetime(2024, 5, 17, 14, 30), False, "two\nlines"])
s1["B4"] = "=B2+B3"
s2 = wb.create_sheet("Second")
s2.append(["only", "sheet two"])
wb.save(path("sample.xlsx"))

ods_content = ('<?xml version="1.0" encoding="UTF-8"?><office:document-content xmlns:office="urn:oasis:names:tc:opendocument:xmlns:office:1.0" '
               'xmlns:table="urn:oasis:names:tc:opendocument:xmlns:table:1.0" xmlns:text="urn:oasis:names:tc:opendocument:xmlns:text:1.0" office:version="1.2">'
               '<office:body><office:spreadsheet><table:table table:name="Ods">'
               '<table:table-row><table:table-cell office:value-type="string"><text:p>Name</text:p></table:table-cell>'
               '<table:table-cell office:value-type="string"><text:p>Value</text:p></table:table-cell></table:table-row>'
               '<table:table-row><table:table-cell office:value-type="string"><text:p>x</text:p></table:table-cell>'
               '<table:table-cell office:value-type="float" office:value="12.5"><text:p>12.5</text:p></table:table-cell></table:table-row>'
               '<table:table-row table:number-rows-repeated="1048570"><table:table-cell table:number-columns-repeated="1024"/></table:table-row>'
               '</table:table></office:spreadsheet></office:body></office:document-content>')
with zipfile.ZipFile(path("sample.ods"), "w") as z:
    z.writestr(zipfile.ZipInfo("mimetype"), "application/vnd.oasis.opendocument.spreadsheet")
    z.writestr("content.xml", ods_content)
with open(path("sample.csv"), "w", encoding="utf-8", newline="") as f:
    f.write('Name,Count,Note\r\napple,3,"has ""quotes"", commas"\r\npear,007,"two\nlines"\r\n')
with open(path("semicolon.csv"), "w", encoding="utf-8", newline="") as f:
    f.write("Name;Price\r\nbread;2,50\r\n")
with open(path("sample.tsv"), "w", encoding="utf-8", newline="") as f:
    f.write("A\tB\r\n1\t2\r\n")
with open(path("sample.srt"), "w", encoding="utf-8") as f:
    f.write("1\n00:00:01,000 --> 00:00:03,500\nHello <i>there</i>.\n\n2\n00:00:04,000 --> 00:00:06,000\nTwo lines\nof text.\n")
with open(path("sample.vtt"), "w", encoding="utf-8") as f:
    f.write("WEBVTT\n\nNOTE a comment\n\nintro\n00:01.000 --> 00:03.500 align:start\nHi &amp; welcome.\n\n00:00:04.000 --> 00:00:06.000\nSecond cue.\n")

print("corpus:", sum(os.path.getsize(os.path.join(dp, n)) for dp, _, ns in os.walk(OUT) for n in ns) // 1024, "KB")
