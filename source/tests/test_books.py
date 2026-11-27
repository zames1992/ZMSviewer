"""Generate synthetic files; test each through ThumbTest's DLL entry point."""
import argparse
import base64
import io
from pathlib import Path
import struct
import tempfile
import subprocess
import zipfile
from PIL import Image, ImageStat

parser = argparse.ArgumentParser()
parser.add_argument("--bin", type=Path, required=True, help="Directory containing ZMSviewer.dll and ThumbTest.exe")
args = parser.parse_args()
scratch = tempfile.TemporaryDirectory(prefix="ZMSviewer-books-")
root = Path(scratch.name)
files = root / "inputs"
results = root / "results"
files.mkdir(exist_ok=True)
results.mkdir(exist_ok=True)
RED, BLUE = (235, 24, 42), (25, 76, 208)


def png(color):
    out = io.BytesIO()
    Image.new("RGB", (48, 72), color).save(out, "PNG")
    return out.getvalue()


red, blue = png(RED), png(BLUE)
def zipfiles(path, entries):
    with zipfile.ZipFile(files / path, "w", zipfile.ZIP_DEFLATED) as f:
        for name, data in entries.items():
            f.writestr(name, data)


def fb2(name, cover, encoding="utf-8"):
    data = f"""<?xml version="1.0" encoding="{encoding}"?>
<fb:FictionBook xmlns:fb="http://www.gribuser.ru/xml/fictionbook/2.0" xmlns:l="http://www.w3.org/1999/xlink">
<fb:description><fb:title-info><fb:book-title>Синяя книга</fb:book-title>{cover}</fb:title-info></fb:description>
<fb:body><fb:section><fb:p>Проверка</fb:p></fb:section></fb:body>"""
    if cover:
        for key, binary in [("ad", red), ("main", blue)]:
            data += '<fb:binary id="%s" content-type="image/png">%s</fb:binary>' % (key, base64.b64encode(binary).decode())
    data += "</fb:FictionBook>"
    (files / name).write_bytes(data.encode(encoding))

cover = '<fb:coverpage><fb:image l:href="#main"/></fb:coverpage>'
fb2("cover.fb2", cover, "utf-16")
fb2("title.fb2", "", "windows-1251")
fb2("missing-cover.fb2", cover.replace("#main", "#missing"))
zipfiles("compressed.fbz", {"nested/test.fb2": (files / "cover.fb2").read_bytes()})

container = '<container><rootfiles><rootfile full-path="OPS/real.opf"/></rootfiles></container>'
meta = '<opf:metadata><dc:title xmlns:dc="urn:test">Настоящее название</dc:title></opf:metadata>'
package = '<opf:package xmlns:opf="urn:test">' + meta + '<opf:manifest>%s</opf:manifest></opf:package>'
items = '<opf:item id="one" media-type="image/png" href="pages/1.png"/><opf:item id="the-right-picture" properties="cover-image" media-type="image/png" href="pages/blue%20%26%20sky.png"/>'
zipfiles("cover.epub", {"OPS/wrong.opf":package % "", "META-INF/container.xml":container,
                        "OPS/real.opf":package % items, "OPS/pages/1.png":red, "OPS/pages/blue & sky.png":blue})
zipfiles("text.epub", {"META-INF/container.xml":container, "OPS/real.opf":(package % "").encode("utf-16")})
zipfiles("pages.cbz", {"__MACOSX/b.png":red, "pages/10.png":red, "pages/2.png":blue})

def make_mobi(name, version):
    header = bytearray(16 + 232)
    header[16:20] = b"MOBI"
    struct.pack_into(">H", header, 0, 1)
    for pos, value in [(20, 232),(28, 65001), (36, version), (108, 1), (128, 64)]:
        struct.pack_into(">I", header, pos, value)
    header += struct.pack(">4sIIIII", b"EXTH",24,1,201,12,1)
    palm = bytearray(78 + 3 * 8 + 2)
    palm[60:68] = b"BOOKMOBI"
    struct.pack_into(">H", palm, 76, 3)
    offsets = [len(palm), len(palm) + len(header), len(palm) + len(header) + len(red)]
    for i, off in enumerate(offsets):
        struct.pack_into(">I",palm,78 + 8*i,off)
    (files / name).write_bytes(palm+header+red+blue)

for name, version in [("cover.mobi", 6), ("cover.azw", 6), ("cover.azw3", 8)]:
    make_mobi(name, version)

def make_pdf():
    # Landscape first page, blue on left and red on right. Second page is solid green.
    def content(blob):
        return f"<< /Length {len(blob)} >>\nstream\n{blob}\nendstream"
    objects = ["<< /Type /Catalog /Pages 2 0 R >>",
               "<< /Type /Pages /Kids [3 0 R 5 0 R] /Count 2 >>",
               "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 200] /Resources <<>> /Contents 4 0 R >>",
               content("0 0 1 rg 0 0 200 200 re f 1 0 0 rg 200 0 200 200 re f"),
               "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] /Resources <<>> /Contents 6 0 R >>",
               content("0 1 0 rg 0 0 200 200 re f")]
    data, offsets = b"%PDF-1.4\n", [0]
    for idx, obj in enumerate(objects, 1):
        offsets.append(len(data))
        data += f"{idx} 0 obj\n{obj}\nendobj\n".encode()
    x = len(data)
    data += f"xref\n0 {len(offsets)}\n0000000000 65535 f \n".encode()
    for off in offsets[1:]:
        data += f"{off:010} 00000 n \n".encode()
    data += f"trailer\n<< /Size {len(offsets)} /Root 1 0 R >>\nstartxref\n{x}\n%%EOF\n".encode()
    (files / "pages.pdf").write_bytes(data)
make_pdf()

for ext in ["pdf", "mobi", "epub", "cbz", "fbz"]:
    (files / ("damaged." + ext)).write_bytes(b"invalid input\x00" * 10)

failed = []
for f in sorted(files.iterdir()):
    out = results / (f.name + ".png")
    out.unlink(missing_ok=True)
    try:
        test = subprocess.run([str((args.bin/"ThumbTest.exe").resolve()),"dll", str((args.bin/"ZMSviewer.dll").resolve()), str(f),str(out),"160"], capture_output=True, timeout=25)
        if f.stem == "damaged":
            assert test.returncode and not out.exists(), "damaged sample reported as success"
        else:
            assert test.returncode == 0 and out.exists(), test.stdout
            img = Image.open(out).convert("RGB")
            w,h = img.size
            if f.suffix == ".pdf":
                assert (w,h) == (160,80) and img.getpixel((30,40))[2] > 240 and img.getpixel((130,40))[0] > 240, img.size
            elif f.stem in ("title","text"):
                assert w/h == 0.75 and max(ImageStat.Stat(img).stddev) > 25 and min(img.crop((w//8,h//6,w*9//10,h*4//5)).getextrema()[0]) < 120, img.size
            else:
                color = RED if f.stem == "missing-cover" else BLUE
                assert (w,h) == (48,72) and img.getpixel((24,36)) == color, (img.size,img.getpixel((w//2,h//2)))
        print("PASS", f.name)
    except Exception as e:
        failed.append(f.name)
        print("FAIL", f.name, str(e))
if failed:
    raise SystemExit(1)
