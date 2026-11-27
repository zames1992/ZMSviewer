"""Exercise the release DLL and compare its PSD/PSB thumbnails with known pixels.

Usage: python tests/test_psd.py --bin bin --output ../qa-psd
Requires Pillow for the PNG checks. Fixtures are generated locally.
"""
import argparse
import io
import struct
import subprocess
import zlib
from pathlib import Path

from PIL import Image


def pack(fmt, *args):
    return struct.pack(">" + fmt, *args)


def resource(number, data):
    return b"8BIM" + pack("H", number) + b"\0\0" + pack("I", len(data)) + data + b"\0" * (len(data) % 2)


def document(version, size, depth, body, resources=b"", layers=b"", channels=3, mode=3):
    length_code = "Q" if version == 2 else "I"
    header = b"8BPS" + pack("H6xHIIHH", version, channels, size[1], size[0], depth, mode)
    return (header + pack("II", 0, len(resources)) + resources
            + pack(length_code, len(layers)) + layers + body)


def serialize_channel(image, channel, depth, prediction=False):
    rows = []
    for y in range(image.height):
        values = [image.getpixel((x, y))[channel] for x in range(image.width)]
        if depth == 8:
            row = bytes(values)
        elif depth == 16:
            values = [v * 257 for v in values]
            if prediction:
                values = [values[0]] + [(v - p) % 65536 for p, v in zip(values, values[1:])]
            row = b"".join(pack("H", v) for v in values)
        else:
            row = b"".join(pack("f", v / 255) for v in values)
            if prediction:
                row = b"".join(row[b::4] for b in range(4))
        if prediction and depth != 16:
            row = row[:1] + bytes((v - p) % 256 for p, v in zip(row, row[1:]))
        rows.append(row)
    return rows


def compressed(rows, code, version):
    if code == 1:
        packed = [bytes([len(row) - 1]) + row for row in rows]
        lengths = b"".join(pack("I" if version == 2 else "H", len(row)) for row in packed)
        data = lengths + b"".join(packed)
    else:
        raw = b"".join(rows)
        data = zlib.compress(raw) if code > 1 else raw
    return pack("H", code) + data


def layer_record(image, position, opacity, hidden, code, version):
    length_code = "Q" if version == 2 else "I"
    planes = []
    for index, band in enumerate(image.getbands()):
        rows = serialize_channel(image, index, 8, prediction=code == 3)
        planes.append((-1 if band == "A" else index, compressed(rows, code, version)))
    x, y = position
    record = pack("iiiiH", y, x, y + image.height, x + image.width, len(planes))
    record += b"".join(pack("h" + length_code, chan, len(body)) for chan, body in planes)
    extra = pack("II", 0, 0) + b"\x03Art"
    for key, value in [(b"clbl", 1 << 24), (b"infx", 0), (b"knko", 0)]:
        extra += b"8BIM" + key + pack("II", 4, value)
    record += b"8BIMnorm" + pack("BBBBI", opacity, 0, 2 if hidden else 0, 0, len(extra)) + extra
    return record, b"".join(body for _, body in planes)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    passed = 0

    def check(name, data, expected):
        nonlocal passed
        path = args.output / name
        path.write_bytes(data)
        dst = path.with_suffix(".png")
        result = subprocess.run([args.bin / "ThumbTest.exe", "dll", (args.bin / "ZMSviewer.dll").resolve(),
                                 path, dst, "256"], capture_output=True, timeout=10)
        if expected is None:
            assert result.returncode == 1, name
        else:
            assert result.returncode == 0, (name, result.stdout)
            with Image.open(dst) as png:
                actual = png.convert("RGB")
                assert actual.size == expected.size, (name, actual.size)
                delta = max(abs(a - e) for a, e in zip(actual.tobytes(), expected.convert("RGB").tobytes()))
                assert delta <= 2, (name, delta)
        passed += 1

    image = Image.new("RGB", (11, 7))
    for y in range(image.height):
        for x in range(image.width):
            image.putpixel((x, y), ((79 * x + y) % 256, (17 * x + 13 * y) % 256, (31 * x + 47 * y) % 256))
    for version, ext in [(1, "psd"), (2, "psb")]:
        for depth in [8, 16, 32]:
            for code in range(4):
                rows = []
                for channel in range(3):
                    rows += serialize_channel(image, channel, depth, code == 3)
                body = compressed(rows, code, version)
                check(f"merged-{depth}-{code}.{ext}", document(version, image.size, depth, body), image)

        background = Image.new("RGB", image.size, (32, 17, 198))
        foreground = Image.new("RGBA", (6, 4), (200, 61, 42, 190))
        hidden = Image.new("RGB", image.size, (18, 207, 6))
        records = [layer_record(background, (0, 0), 255, False, 1, version),
                   layer_record(foreground, (-2, 1), 170, False, 3, version),
                   layer_record(hidden, (0, 0), 255, True, 0, version)]
        layer_data = pack("h", 3) + b"".join(r for r, _ in records) + b"".join(d for _, d in records)
        layer_data += b"\0" * (len(layer_data) % 2)
        layers = pack("Q" if version == 2 else "I", len(layer_data)) + layer_data + pack("I", 0)
        no_composite = resource(1057, pack("IB", 1, 0))
        expected = background.copy()
        expected.paste(foreground.convert("RGB"), (-2, 1),
                       foreground.getchannel("A").point(lambda a: round(a * 170 / 255)))
        check(f"raster-layers.{ext}", document(version, image.size, 8, b"", no_composite, layers), expected)

    for legacy in (False, True):
        stored = Image.merge("RGB", image.split()[::-1]) if legacy else image
        stride = (3 * stored.width + 3) & ~3
        for jpeg in (False, True):
            if jpeg:
                temp = io.BytesIO()
                stored.save(temp, format="JPEG", quality=95, subsampling=0)
                data = temp.getvalue()
                with Image.open(io.BytesIO(data)) as loaded:
                    expected = loaded.convert("RGB")
                if legacy:
                    expected = Image.merge("RGB", expected.split()[::-1])
            else:
                data = b"".join(stored.crop((0, y, stored.width, y + 1)).tobytes()
                                + b"\0" * (stride - 3 * stored.width) for y in range(stored.height))
                expected = image
            header = pack("IIIIIIHH", int(jpeg), stored.width, stored.height, stride,
                          stride * stored.height, len(data), 24, 1)
            resources = no_composite + resource(1033 if legacy else 1036, header + data)
            check(f"embedded-{int(legacy)}-{int(jpeg)}.psd", document(1, image.size, 8, b"", resources), expected)

    bad = document(1, (1, 1), 8, pack("H", 2) + zlib.compress(b"x" * 4096))
    check("invalid-zip-size.psd", bad, None)
    masked = layers.replace(b"8BIMclbl", b"8BIMvmsk", 1)
    check("unsupported-vector-mask.psb", document(2, image.size, 8, b"", no_composite, masked), None)
    check("invalid-indexed-depth.psd", document(1, (16, 1), 1, b"\0" * 32, mode=2), None)
    print(f"Passed {passed} DLL thumbnail checks.")


if __name__ == "__main__":
    main()
