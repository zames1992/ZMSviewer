// Pictures: TIFF (own decoder, Windows Imaging Component as a fallback), TGA, Radiance HDR,
// OpenEXR and DDS.
#include "Common.h"
#include <algorithm>
#include <cmath>

#define STBI_NO_STDIO
#define STBI_ONLY_TGA
#define STBI_ONLY_HDR
#include "../vendor/stb_image.h"
#include "../vendor/tinyexr.h"
#include "../vendor/bcdec.h"

// ---------------------------------------------------------------------------------------------
// TIFF. Windows' own TIFF thumbnails fail on what Photoshop and 3D tools write all the time:
// ZIP compression, floating point, layered files with a predictor. This decoder covers strips and
// tiles, none/LZW/Deflate/PackBits, predictors 2 and 3, 1-32 bit, gray/RGB/palette/CMYK/Lab with
// alpha, classic and BigTIFF, and box-filters straight to thumbnail size.

namespace {

struct TiffReader
{
    const uint8_t* data;
    size_t size;
    bool le;
    bool big;
    uint16_t U16(size_t o) const { return o + 2 <= size ? (le ? Le16(data + o) : Be16(data + o)) : 0; }
    uint32_t U32(size_t o) const { return o + 4 <= size ? (le ? Le32(data + o) : Be32(data + o)) : 0; }
    uint64_t U64(size_t o) const { return o + 8 <= size ? (le ? Le64(data + o) : Be64(data + o)) : 0; }
};

struct TiffField
{
    uint16_t type = 0;
    uint64_t count = 0;
    size_t offset = 0;  // where the values are
};

struct TiffIfd
{
    uint32_t width = 0, height = 0;
    std::vector<uint32_t> bits;
    uint32_t compression = 1, photometric = 1, spp = 1, rowsPerStrip = 0xffffffff, planar = 1, predictor = 1;
    uint32_t tileWidth = 0, tileHeight = 0, subfileType = 0, sampleFormat = 1, inkSet = 1;
    std::vector<uint64_t> offsets, counts;
    std::vector<uint32_t> extra;
    std::vector<uint16_t> colormap;
};

size_t TypeSize(uint16_t type)
{
    switch (type)
    {
    case 1: case 2: case 6: case 7: return 1;
    case 3: case 8: return 2;
    case 4: case 9: case 11: case 13: return 4;
    case 5: case 10: case 12: case 16: case 17: case 18: return 8;
    default: return 0;
    }
}

uint64_t FieldValue(const TiffReader& r, const TiffField& f, uint64_t i)
{
    size_t o = f.offset + size_t(i) * TypeSize(f.type);
    switch (f.type)
    {
    case 1: case 2: case 6: case 7: return o < r.size ? r.data[o] : 0;
    case 3: case 8: return r.U16(o);
    case 4: case 9: case 13: return r.U32(o);
    case 16: case 17: case 18: return r.U64(o);
    default: return 0;
    }
}

bool ReadIfd(const TiffReader& r, uint64_t pos, TiffIfd& ifd, uint64_t& next)
{
    uint64_t n = r.big ? r.U64(size_t(pos)) : r.U16(size_t(pos));
    size_t entry = size_t(pos) + (r.big ? 8 : 2);
    size_t entrySize = r.big ? 20 : 12;
    if (n == 0 || n > 4096 || entry + n * entrySize > r.size) return false;
    for (uint64_t k = 0; k < n; k++, entry += entrySize)
    {
        uint16_t tag = r.U16(entry);
        TiffField f;
        f.type = r.U16(entry + 2);
        f.count = r.big ? r.U64(entry + 4) : r.U32(entry + 4);
        size_t valueBytes = size_t(std::min<uint64_t>(f.count, 1ull << 40)) * TypeSize(f.type);
        size_t inlineSize = r.big ? 8 : 4;
        size_t valuePos = entry + (r.big ? 12 : 8);
        f.offset = valueBytes <= inlineSize ? valuePos : size_t(r.big ? r.U64(valuePos) : r.U32(valuePos));
        if (TypeSize(f.type) == 0 || f.offset + valueBytes > r.size) continue;
        auto one = [&]() { return uint32_t(FieldValue(r, f, 0)); };
        auto list = [&](auto& v) { v.resize(size_t(f.count)); for (uint64_t i = 0; i < f.count; i++) v[size_t(i)] = std::remove_reference_t<decltype(v[0])>(FieldValue(r, f, i)); };
        switch (tag)
        {
        case 254: ifd.subfileType = one(); break;
        case 256: ifd.width = one(); break;
        case 257: ifd.height = one(); break;
        case 258: list(ifd.bits); break;
        case 259: ifd.compression = one(); break;
        case 262: ifd.photometric = one(); break;
        case 273: case 324: list(ifd.offsets); break;
        case 277: ifd.spp = one(); break;
        case 278: ifd.rowsPerStrip = one(); break;
        case 279: case 325: list(ifd.counts); break;
        case 284: ifd.planar = one(); break;
        case 317: ifd.predictor = one(); break;
        case 320: list(ifd.colormap); break;
        case 322: ifd.tileWidth = one(); break;
        case 323: ifd.tileHeight = one(); break;
        case 332: ifd.inkSet = one(); break;
        case 338: list(ifd.extra); break;
        case 339: ifd.sampleFormat = one(); break;
        }
    }
    size_t nextPos = entry;
    next = r.big ? r.U64(nextPos) : r.U32(nextPos);
    return true;
}

bool UnpackBits(const uint8_t* src, size_t n, std::vector<uint8_t>& out, size_t expected)
{
    out.clear();
    out.reserve(expected);
    size_t i = 0;
    while (i < n && out.size() < expected)
    {
        int8_t c = int8_t(src[i++]);
        if (c >= 0)
        {
            size_t count = size_t(c) + 1;
            if (i + count > n) return false;
            out.insert(out.end(), src + i, src + i + count);
            i += count;
        }
        else if (c != -128)
        {
            if (i >= n) return false;
            out.insert(out.end(), size_t(1 - c), src[i++]);
        }
    }
    return out.size() >= expected;
}

// TIFF LZW: MSB-first codes, 9-12 bits, code width grows one code early.
bool UnLzw(const uint8_t* src, size_t n, std::vector<uint8_t>& out, size_t expected)
{
    out.clear();
    out.reserve(expected);
    std::vector<uint32_t> prefix(4096);
    std::vector<uint8_t> suffix(4096), first(4096);
    std::vector<uint16_t> length(4096);
    for (int i = 0; i < 256; i++) { prefix[i] = 0xffff; suffix[i] = uint8_t(i); first[i] = uint8_t(i); length[i] = 1; }
    int next = 258, width = 9;
    int old = -1;
    uint64_t bitPos = 0, totalBits = uint64_t(n) * 8;
    std::vector<uint8_t> stack;
    auto emit = [&](int code) {
        size_t start = out.size();
        out.resize(start + length[code]);
        for (int c = code, k = length[code] - 1; k >= 0; k--, c = prefix[c]) out[start + k] = suffix[c];
    };
    while (bitPos + width <= totalBits && out.size() < expected)
    {
        uint32_t code = 0;
        for (int b = 0; b < width; b++, bitPos++)
            code = (code << 1) | ((src[bitPos >> 3] >> (7 - (bitPos & 7))) & 1);
        if (code == 257) break;
        if (code == 256) { next = 258; width = 9; old = -1; continue; }
        if (old < 0)
        {
            if (code > 255) return false;
            emit(int(code));
            old = int(code);
            continue;
        }
        if (int(code) < next)
        {
            emit(int(code));
            if (next < 4096) { prefix[next] = old; suffix[next] = first[code]; first[next] = first[old]; length[next] = uint16_t(length[old] + 1); next++; }
        }
        else if (int(code) == next && next < 4096)
        {
            prefix[next] = old; suffix[next] = first[old]; first[next] = first[old]; length[next] = uint16_t(length[old] + 1); next++;
            emit(int(code));
        }
        else
            return false;
        old = int(code);
        if (next + 1 >= (1 << width) && width < 12) width++;
    }
    return out.size() >= expected;
}

float HalfToFloat(uint16_t h)
{
    uint32_t sign = uint32_t(h & 0x8000) << 16, exp = (h >> 10) & 0x1f, mant = h & 0x3ff, bits;
    if (exp == 0)
    {
        if (mant == 0) bits = sign;
        else { exp = 127 - 15 + 1; while (!(mant & 0x400)) { mant <<= 1; exp--; } mant &= 0x3ff; bits = sign | (exp << 23) | (mant << 13); }
    }
    else if (exp == 31) bits = sign | 0x7f800000 | (mant << 13);
    else bits = sign | ((exp + 127 - 15) << 23) | (mant << 13);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

bool DecodeTiff(const std::vector<uint8_t>& file, UINT maxSize, Image& out)
{
    if (file.size() < 16) return false;
    TiffReader r{ file.data(), file.size(), file[0] == 'I', false };
    if (!(file[0] == 'I' && file[1] == 'I') && !(file[0] == 'M' && file[1] == 'M')) return false;
    uint16_t magic = r.U16(2);
    uint64_t pos;
    if (magic == 42) pos = r.U32(4);
    else if (magic == 43) { r.big = true; pos = r.U64(8); }
    else return false;

    // The first full-resolution image (some writers put a reduced one first).
    TiffIfd ifd;
    bool found = false;
    for (int i = 0; i < 16 && pos && pos < file.size(); i++)
    {
        TiffIfd candidate;
        uint64_t next = 0;
        if (!ReadIfd(r, pos, candidate, next)) break;
        if (!found || ((ifd.subfileType & 1) && !(candidate.subfileType & 1)))
        {
            ifd = std::move(candidate);
            found = true;
            if (!(ifd.subfileType & 1)) break;
        }
        pos = next;
    }
    if (!found || ifd.width == 0 || ifd.height == 0 || ifd.spp == 0 || ifd.spp > 16) return false;
    if (ifd.compression != 1 && ifd.compression != 5 && ifd.compression != 8 && ifd.compression != 32946
        && ifd.compression != 32773) return false;
    const uint32_t bits = ifd.bits.empty() ? 1 : ifd.bits[0];
    if (bits != 1 && bits != 2 && bits != 4 && bits != 8 && bits != 16 && bits != 32 && bits != 64) return false;
    const bool isFloat = ifd.sampleFormat == 3;
    const bool isSigned = ifd.sampleFormat == 2;
    if (isFloat && bits < 16) return false;

    // How the samples map to color.
    int colorCount;
    switch (ifd.photometric)
    {
    case 0: case 1: colorCount = 1; break;
    case 2: colorCount = 3; break;
    case 3: colorCount = 1; if (ifd.colormap.size() < (size_t(3) << bits)) return false; break;
    case 5: colorCount = ifd.inkSet == 1 ? 4 : int(ifd.spp); if (colorCount != 4) return false; break;
    case 8: case 9: colorCount = 3; break;
    default: return false;  // YCbCr, JPEG and friends go to WIC
    }
    if (int(ifd.spp) < colorCount) return false;
    int alphaIndex = -1;
    if (int(ifd.spp) > colorCount && !ifd.extra.empty() && (ifd.extra[0] == 1 || ifd.extra[0] == 2))
        alphaIndex = colorCount;
    const bool premultiplied = alphaIndex >= 0 && ifd.extra[0] == 1;

    const bool tiled = ifd.tileWidth && ifd.tileHeight;
    const uint32_t blockW = tiled ? ifd.tileWidth : ifd.width;
    const uint32_t blockH = tiled ? ifd.tileHeight : std::min(ifd.rowsPerStrip ? ifd.rowsPerStrip : ifd.height, ifd.height);
    const uint32_t across = (ifd.width + blockW - 1) / blockW;
    const uint32_t down = (ifd.height + blockH - 1) / blockH;
    const uint32_t planes = ifd.planar == 2 ? ifd.spp : 1;
    const uint32_t samplesPerPixelInBlock = ifd.planar == 2 ? 1 : ifd.spp;
    const size_t rowBytes = (size_t(blockW) * samplesPerPixelInBlock * bits + 7) / 8;
    const size_t blockBytes = rowBytes * blockH;
    if (ifd.offsets.size() < size_t(across) * down * planes) return false;
    if (ifd.counts.size() < ifd.offsets.size()) ifd.counts.resize(ifd.offsets.size(), blockBytes);

    // Floating point and 16-bit-half images are scene-linear; integer ones are display values.
    const bool linear = isFloat;
    Downsampler ds(ifd.width, ifd.height, maxSize, 5);  // c0..c3 color, c4 alpha
    std::vector<uint8_t> raw;

    auto sampleAt = [&](const uint8_t* row, uint32_t index) -> float {
        switch (bits)
        {
        case 1: return float((row[index >> 3] >> (7 - (index & 7))) & 1);
        case 2: return float((row[index >> 2] >> (6 - 2 * (index & 3))) & 3) / 3.0f;
        case 4: return float((row[index >> 1] >> (index & 1 ? 0 : 4)) & 15) / 15.0f;
        case 8: return isSigned ? (int8_t(row[index]) + 128) / 255.0f : row[index] / 255.0f;
        case 16:
        {
            uint16_t v = r.le ? Le16(row + 2 * index) : Be16(row + 2 * index);
            if (isFloat) return HalfToFloat(v);
            return isSigned ? (int16_t(v) + 32768) / 65535.0f : v / 65535.0f;
        }
        case 32:
        {
            uint32_t v = r.le ? Le32(row + 4 * index) : Be32(row + 4 * index);
            if (isFloat) { float f; memcpy(&f, &v, 4); return f; }
            return isSigned ? float((double(int32_t(v)) + 2147483648.0) / 4294967295.0) : float(v / 4294967295.0);
        }
        default:
        {
            uint64_t v = r.le ? Le64(row + 8 * index) : Be64(row + 8 * index);
            double d;
            memcpy(&d, &v, 8);
            return isFloat ? float(d) : 0.0f;
        }
        }
    };
    auto palette = [&](float sample, int c) {
        size_t n = size_t(1) << bits;
        size_t index = std::min(n - 1, size_t(std::lround(sample * (n - 1))));
        return ifd.colormap[c * n + index] / 65535.0f;
    };

    for (uint32_t plane = 0; plane < planes; plane++)
    {
        // Planar files: only the planes we show are worth decoding.
        if (planes > 1 && int(plane) >= colorCount && int(plane) != alphaIndex) continue;
        for (uint32_t by = 0; by < down; by++)
            for (uint32_t bx = 0; bx < across; bx++)
            {
                size_t block = size_t(plane) * across * down + size_t(by) * across + bx;
                uint64_t offset = ifd.offsets[block], count = ifd.counts[block];
                if (offset >= file.size()) continue;
                count = std::min<uint64_t>(count, file.size() - offset);
                const uint8_t* src = file.data() + offset;
                const uint8_t* pixels = src;
                size_t have = size_t(count);
                if (ifd.compression == 5)
                {
                    if (!UnLzw(src, have, raw, blockBytes) && raw.empty()) return false;
                    raw.resize(blockBytes, 0); pixels = raw.data(); have = raw.size();
                }
                else if (ifd.compression == 8 || ifd.compression == 32946)
                {
                    if (!Inflate(src, have, true, raw, blockBytes, true)) return false;
                    raw.resize(blockBytes, 0); pixels = raw.data(); have = raw.size();
                }
                else if (ifd.compression == 32773)
                {
                    if (!UnpackBits(src, have, raw, blockBytes) && raw.empty()) return false;
                    raw.resize(blockBytes, 0); pixels = raw.data(); have = raw.size();
                }
                else if (have < blockBytes)
                {
                    raw.assign(src, src + have); raw.resize(blockBytes, 0); pixels = raw.data(); have = raw.size();
                }

                // Undo the predictor in place (needs a writable copy).
                if (ifd.predictor == 2 || ifd.predictor == 3)
                {
                    if (pixels != raw.data()) { raw.assign(pixels, pixels + blockBytes); pixels = raw.data(); }
                    uint8_t* p = raw.data();
                    const uint32_t spp = samplesPerPixelInBlock;
                    for (uint32_t y = 0; y < blockH; y++)
                    {
                        uint8_t* row = p + y * rowBytes;
                        if (ifd.predictor == 3 && isFloat)
                        {
                            // Bytes are split into planes (most significant first) and delta coded.
                            size_t n = size_t(blockW) * spp * (bits / 8);
                            for (size_t i = spp; i < n; i++) row[i] = uint8_t(row[i] + row[i - spp]);
                            std::vector<uint8_t> tmp(row, row + n);
                            size_t count1 = size_t(blockW) * spp, bytes = bits / 8;
                            for (size_t i = 0; i < count1; i++)
                                for (size_t b = 0; b < bytes; b++)
                                    row[i * bytes + b] = tmp[(r.le ? bytes - 1 - b : b) * count1 + i];
                        }
                        else if (bits == 8)
                            for (size_t i = spp; i < size_t(blockW) * spp; i++) row[i] = uint8_t(row[i] + row[i - spp]);
                        else if (bits == 16)
                            for (size_t i = spp; i < size_t(blockW) * spp; i++)
                            {
                                uint16_t a = r.le ? Le16(row + 2 * i) : Be16(row + 2 * i);
                                uint16_t b = r.le ? Le16(row + 2 * (i - spp)) : Be16(row + 2 * (i - spp));
                                uint16_t v = uint16_t(a + b);
                                if (r.le) { row[2 * i] = uint8_t(v); row[2 * i + 1] = uint8_t(v >> 8); }
                                else { row[2 * i] = uint8_t(v >> 8); row[2 * i + 1] = uint8_t(v); }
                            }
                        else if (bits == 32)
                            for (size_t i = spp; i < size_t(blockW) * spp; i++)
                            {
                                uint32_t a = r.le ? Le32(row + 4 * i) : Be32(row + 4 * i);
                                uint32_t b = r.le ? Le32(row + 4 * (i - spp)) : Be32(row + 4 * (i - spp));
                                uint32_t v = a + b;
                                for (int k = 0; k < 4; k++) row[4 * i + k] = uint8_t(r.le ? v >> (8 * k) : v >> (24 - 8 * k));
                            }
                    }
                }

                const uint32_t x0 = bx * blockW, y0 = by * blockH;
                const uint32_t w = std::min(blockW, ifd.width - x0), h = std::min(blockH, ifd.height - y0);
                for (uint32_t y = 0; y < h; y++)
                {
                    const uint8_t* row = pixels + y * rowBytes;
                    for (uint32_t x = 0; x < w; x++)
                    {
                        if (planes > 1)
                        {
                            float v = sampleAt(row, x);
                            int c = int(plane) == alphaIndex ? 4 : int(plane);
                            ds.Add(x0 + x, y0 + y, c, v);
                        }
                        else
                        {
                            uint32_t base = x * ifd.spp;
                            for (int c = 0; c < colorCount; c++) ds.Add(x0 + x, y0 + y, c, sampleAt(row, base + c));
                            if (alphaIndex >= 0) ds.Add(x0 + x, y0 + y, 4, sampleAt(row, base + alphaIndex));
                        }
                    }
                }
            }
    }

    out.width = ds.OutWidth();
    out.height = ds.OutHeight();
    out.alpha = alphaIndex >= 0;
    out.bgra.resize(size_t(out.width) * out.height * 4);
    for (UINT y = 0; y < out.height; y++)
        for (UINT x = 0; x < out.width; x++)
        {
            float v[4] = { ds.Get(x, y, 0), ds.Get(x, y, 1), ds.Get(x, y, 2), ds.Get(x, y, 3) };
            float a = alphaIndex >= 0 ? std::clamp(ds.Get(x, y, 4), 0.0f, 1.0f) : 1.0f;
            if (premultiplied && a > 0) for (int c = 0; c < colorCount; c++) v[c] /= a;
            float rgb[3];
            switch (ifd.photometric)
            {
            case 0: rgb[0] = rgb[1] = rgb[2] = 1.0f - v[0]; break;
            case 1: rgb[0] = rgb[1] = rgb[2] = v[0]; break;
            case 3: for (int c = 0; c < 3; c++) rgb[c] = palette(v[0], c); break;
            case 5: for (int c = 0; c < 3; c++) rgb[c] = (1.0f - v[c]) * (1.0f - v[3]); break;
            case 8: case 9:
            {
                // CIELab: L 0..100, a/b signed bytes (8) or offset (9).
                float L = v[0] * 100.0f, A, B;
                if (ifd.photometric == 8) { A = (v[1] >= 0.5f ? v[1] - 1.0f : v[1]) * 255.0f; B = (v[2] >= 0.5f ? v[2] - 1.0f : v[2]) * 255.0f; }
                else { A = v[1] * 255.0f - 128.0f; B = v[2] * 255.0f - 128.0f; }
                float fy = (L + 16.0f) / 116.0f, fx = fy + A / 500.0f, fz = fy - B / 200.0f;
                auto finv = [](float t) { return t > 6.0f / 29.0f ? t * t * t : 3.0f * 36.0f / 841.0f * (t - 4.0f / 29.0f); };
                float X = 0.9642f * finv(fx), Y = finv(fy), Z = 0.8249f * finv(fz);
                float lin[3] = { 3.1338561f * X - 1.6168667f * Y - 0.4906146f * Z,
                    -0.9787684f * X + 1.9161415f * Y + 0.0334540f * Z, 0.0719453f * X - 0.2289914f * Y + 1.4052427f * Z };
                for (int c = 0; c < 3; c++) rgb[c] = LinearToSrgb(lin[c]);
                break;
            }
            default: rgb[0] = v[0]; rgb[1] = v[1]; rgb[2] = v[2]; break;
            }
            uint8_t* d = out.bgra.data() + (size_t(y) * out.width + x) * 4;
            for (int c = 0; c < 3; c++)
            {
                float value = linear && ifd.photometric != 8 && ifd.photometric != 9 ? LinearToSrgb(rgb[c]) : rgb[c];
                d[2 - c] = uint8_t(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
            }
            d[3] = uint8_t(std::lround(a * 255.0f));
        }
    return true;
}

} // namespace

bool LoadTiff(IStream* stream, UINT maxSize, Image& out)
{
    std::vector<uint8_t> file;
    if (ReadAll(stream, file) && DecodeTiff(file, maxSize, out)) return true;
    file.clear();
    file.shrink_to_fit();
    if (!DecodeWithWic(stream, out)) return false;
    FitImage(out, maxSize);
    return true;
}

// ---------------------------------------------------------------------------------------------
// TGA and Radiance HDR through stb_image.

bool LoadTga(IStream* stream, UINT maxSize, Image& out)
{
    std::vector<uint8_t> file;
    if (!ReadAll(stream, file, 512ull << 20) || file.size() > 0x7fffffff) return false;
    int w, h, n;
    stbi_uc* pixels = stbi_load_from_memory(file.data(), int(file.size()), &w, &h, &n, 4);
    if (!pixels) return false;
    out.width = UINT(w);
    out.height = UINT(h);
    out.bgra.resize(size_t(w) * h * 4);
    bool anyVisible = false, anyTransparent = false;
    for (size_t i = 0; i < size_t(w) * h; i++)
    {
        out.bgra[4 * i] = pixels[4 * i + 2];
        out.bgra[4 * i + 1] = pixels[4 * i + 1];
        out.bgra[4 * i + 2] = pixels[4 * i];
        out.bgra[4 * i + 3] = pixels[4 * i + 3];
        anyVisible = anyVisible || pixels[4 * i + 3] != 0;
        anyTransparent = anyTransparent || pixels[4 * i + 3] != 255;
    }
    stbi_image_free(pixels);
    // Many tools write 32-bit TGAs with an all-zero alpha that means nothing: show those opaque.
    out.alpha = n == 4 && anyVisible && anyTransparent;
    if (!out.alpha)
        for (size_t i = 0; i < size_t(w) * h; i++) out.bgra[4 * i + 3] = 255;
    FitImage(out, maxSize);
    return true;
}

// Data images (displacement, masks, normal-ish maps) are often far outside 0..1; stretch those so
// the thumbnail shows the content instead of a flat black or white square.
static void NormalizeIfData(std::vector<float>& rgba, size_t pixels, bool& linear)
{
    float lo = 1e30f, hi = -1e30f;
    for (size_t i = 0; i < pixels; i++)
        for (int c = 0; c < 3; c++)
        {
            float v = rgba[4 * i + c];
            if (!std::isfinite(v)) continue;
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
    if (!(hi > lo)) return;
    bool negative = lo < -0.001f;
    bool tiny = hi < 0.02f;
    if (!negative && !tiny) return;
    float scale = 1.0f / (hi - lo);
    for (size_t i = 0; i < pixels; i++)
        for (int c = 0; c < 3; c++) rgba[4 * i + c] = (rgba[4 * i + c] - lo) * scale;
    linear = false;
}

bool LoadHdr(IStream* stream, UINT maxSize, Image& out)
{
    std::vector<uint8_t> file;
    if (!ReadAll(stream, file, 1024ull << 20) || file.size() > 0x7fffffff) return false;
    if (!stbi_is_hdr_from_memory(file.data(), int(file.size()))) return false;
    int w, h, n;
    float* pixels = stbi_loadf_from_memory(file.data(), int(file.size()), &w, &h, &n, 4);
    if (!pixels) return false;
    std::vector<float> rgba(pixels, pixels + size_t(w) * h * 4);
    stbi_image_free(pixels);
    bool linear = true;
    NormalizeIfData(rgba, size_t(w) * h, linear);
    FloatToImage(rgba.data(), UINT(w), UINT(h), false, linear, maxSize, out);
    return true;
}

bool LoadExr(IStream* stream, UINT maxSize, Image& out)
{
    std::vector<uint8_t> file;
    if (!ReadAll(stream, file, 1536ull << 20)) return false;
    float* pixels = nullptr;
    int w = 0, h = 0;
    const char* err = nullptr;
    if (LoadEXRFromMemory(&pixels, &w, &h, file.data(), file.size(), &err) != TINYEXR_SUCCESS)
    {
        if (err) FreeEXRErrorMessage(err);
        return false;
    }
    file.clear();
    file.shrink_to_fit();
    size_t count = size_t(w) * h;
    std::vector<float> rgba(pixels, pixels + count * 4);
    free(pixels);
    // EXR stores premultiplied color; FloatToImage expects straight alpha.
    bool alpha = false;
    for (size_t i = 0; i < count; i++)
    {
        float a = rgba[4 * i + 3];
        if (a < 0.999f) alpha = true;
        if (a > 0.0f && a < 1.0f) for (int c = 0; c < 3; c++) rgba[4 * i + c] /= a;
    }
    // Displacement or mask with an empty alpha: show it opaque.
    bool anyVisible = false;
    for (size_t i = 0; i < count && !anyVisible; i++) anyVisible = rgba[4 * i + 3] > 0.0f;
    if (!anyVisible) { alpha = false; for (size_t i = 0; i < count; i++) rgba[4 * i + 3] = 1.0f; }
    bool linear = true;
    NormalizeIfData(rgba, count, linear);
    FloatToImage(rgba.data(), UINT(w), UINT(h), alpha, linear, maxSize, out);
    return true;
}

// ---------------------------------------------------------------------------------------------
// DDS: the top mip of the first surface, block compressed (BC1-BC7) or plain.

namespace {

enum class Dxgi : uint32_t
{
    Unknown = 0, Rgba32F = 2, Rgba16F = 10, Rgba16 = 11, Rgb10A2 = 24, Rg11B10F = 26, Rgba8 = 28, Rgba8Srgb = 29,
    R32F = 41, Rg8 = 49, R16F = 54, R16 = 56, R8 = 61, A8 = 65, Bc1 = 71, Bc1Srgb = 72, Bc2 = 74, Bc2Srgb = 75,
    Bc3 = 77, Bc3Srgb = 78, Bc4 = 80, Bc4S = 81, Bc5 = 83, Bc5S = 84, B5G6R5 = 85, B5G5R5A1 = 86, Bgra8 = 87,
    Bgrx8 = 88, Bgra8Srgb = 91, Bgrx8Srgb = 93, Bc6hU = 95, Bc6hS = 96, Bc7 = 98, Bc7Srgb = 99, Bgra4 = 115,
};

float Unorm(uint32_t v, uint32_t bits) { return bits ? float(v) / float((1ull << bits) - 1) : 0.0f; }

uint32_t MaskShift(uint32_t mask) { uint32_t s = 0; while (mask && !(mask & 1)) { mask >>= 1; s++; } return s; }
uint32_t MaskBits(uint32_t mask) { uint32_t n = 0; mask >>= MaskShift(mask); while (mask & 1) { mask >>= 1; n++; } return n; }

} // namespace

bool LoadDds(IStream* stream, UINT maxSize, Image& out)
{
    std::vector<uint8_t> file;
    if (!ReadAll(stream, file, 1024ull << 20) || file.size() < 128 || memcmp(file.data(), "DDS ", 4) != 0) return false;
    const uint8_t* h = file.data() + 4;
    uint32_t height = Le32(h + 8), width = Le32(h + 12);
    const uint8_t* pf = h + 72;
    uint32_t pfFlags = Le32(pf + 4), fourCC = Le32(pf + 8), bitCount = Le32(pf + 12);
    uint32_t masks[4] = { Le32(pf + 16), Le32(pf + 20), Le32(pf + 24), Le32(pf + 28) };
    size_t dataOffset = 128;
    if (width == 0 || height == 0 || width > 65536 || height > 65536) return false;

    Dxgi format = Dxgi::Unknown;
    bool premultiplied = false;
    auto cc = [](const char* s) { return uint32_t(s[0]) | (uint32_t(s[1]) << 8) | (uint32_t(s[2]) << 16) | (uint32_t(s[3]) << 24); };
    if (pfFlags & 4)
    {
        if (fourCC == cc("DX10"))
        {
            if (file.size() < 148) return false;
            format = Dxgi(Le32(file.data() + 128));
            dataOffset = 148;
        }
        else if (fourCC == cc("DXT1")) format = Dxgi::Bc1;
        else if (fourCC == cc("DXT2")) { format = Dxgi::Bc2; premultiplied = true; }
        else if (fourCC == cc("DXT3")) format = Dxgi::Bc2;
        else if (fourCC == cc("DXT4")) { format = Dxgi::Bc3; premultiplied = true; }
        else if (fourCC == cc("DXT5")) format = Dxgi::Bc3;
        else if (fourCC == cc("ATI1") || fourCC == cc("BC4U")) format = Dxgi::Bc4;
        else if (fourCC == cc("BC4S")) format = Dxgi::Bc4S;
        else if (fourCC == cc("ATI2") || fourCC == cc("BC5U")) format = Dxgi::Bc5;
        else if (fourCC == cc("BC5S")) format = Dxgi::Bc5S;
        else if (fourCC == 36) format = Dxgi::Rgba16;
        else if (fourCC == 111) format = Dxgi::R16F;
        else if (fourCC == 113) format = Dxgi::Rgba16F;
        else if (fourCC == 114) format = Dxgi::R32F;
        else if (fourCC == 116) format = Dxgi::Rgba32F;
        else return false;
    }

    const uint8_t* src = file.data() + dataOffset;
    size_t avail = file.size() - dataOffset;
    std::vector<float> rgba(size_t(width) * height * 4, 1.0f);
    bool alpha = false, linear = false, normalMap = false;

    auto blocks = [&](size_t blockSize, auto decode) -> bool {
        uint32_t bw = (width + 3) / 4, bh = (height + 3) / 4;
        if (size_t(bw) * bh * blockSize > avail) return false;
        for (uint32_t by = 0; by < bh; by++)
            for (uint32_t bx = 0; bx < bw; bx++)
                decode(src + (size_t(by) * bw + bx) * blockSize, bx * 4, by * 4);
        return true;
    };
    auto put = [&](uint32_t x, uint32_t y, float r, float g, float b, float a) {
        if (x >= width || y >= height) return;
        float* p = rgba.data() + (size_t(y) * width + x) * 4;
        p[0] = r; p[1] = g; p[2] = b; p[3] = a;
    };

    bool ok = true;
    switch (format)
    {
    case Dxgi::Bc1: case Dxgi::Bc1Srgb: case Dxgi::Bc2: case Dxgi::Bc2Srgb: case Dxgi::Bc3: case Dxgi::Bc3Srgb:
    case Dxgi::Bc7: case Dxgi::Bc7Srgb:
    {
        bool bc1 = format == Dxgi::Bc1 || format == Dxgi::Bc1Srgb;
        alpha = true;
        ok = blocks(bc1 ? 8 : 16, [&](const uint8_t* b, uint32_t x0, uint32_t y0) {
            uint8_t px[16 * 4];
            switch (format)
            {
            case Dxgi::Bc1: case Dxgi::Bc1Srgb: bcdec_bc1(b, px, 16); break;
            case Dxgi::Bc2: case Dxgi::Bc2Srgb: bcdec_bc2(b, px, 16); break;
            case Dxgi::Bc3: case Dxgi::Bc3Srgb: bcdec_bc3(b, px, 16); break;
            default: bcdec_bc7(b, px, 16); break;
            }
            for (int j = 0; j < 4; j++)
                for (int i = 0; i < 4; i++)
                {
                    const uint8_t* p = px + j * 16 + i * 4;
                    put(x0 + i, y0 + j, p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f);
                }
        });
        break;
    }
    case Dxgi::Bc4: case Dxgi::Bc4S:
        ok = blocks(8, [&](const uint8_t* b, uint32_t x0, uint32_t y0) {
            float px[16];
            bcdec_bc4_float(b, px, 4, format == Dxgi::Bc4S);
            for (int j = 0; j < 4; j++)
                for (int i = 0; i < 4; i++)
                {
                    float v = px[j * 4 + i];
                    if (format == Dxgi::Bc4S) v = v * 0.5f + 0.5f;
                    put(x0 + i, y0 + j, v, v, v, 1.0f);
                }
        });
        break;
    case Dxgi::Bc5: case Dxgi::Bc5S:
        normalMap = true;
        ok = blocks(16, [&](const uint8_t* b, uint32_t x0, uint32_t y0) {
            float px[16 * 2];
            bcdec_bc5_float(b, px, 8, format == Dxgi::Bc5S);
            for (int j = 0; j < 4; j++)
                for (int i = 0; i < 4; i++)
                {
                    float r = px[j * 8 + i * 2], g = px[j * 8 + i * 2 + 1];
                    if (format == Dxgi::Bc5S) { r = r * 0.5f + 0.5f; g = g * 0.5f + 0.5f; }
                    put(x0 + i, y0 + j, r, g, 0.0f, 1.0f);
                }
        });
        break;
    case Dxgi::Bc6hU: case Dxgi::Bc6hS:
        linear = true;
        ok = blocks(16, [&](const uint8_t* b, uint32_t x0, uint32_t y0) {
            float px[16 * 3];
            bcdec_bc6h_float(b, px, 12, format == Dxgi::Bc6hS);
            for (int j = 0; j < 4; j++)
                for (int i = 0; i < 4; i++)
                {
                    const float* p = px + j * 12 + i * 3;
                    put(x0 + i, y0 + j, p[0], p[1], p[2], 1.0f);
                }
        });
        break;
    default:
    {
        // Plain pixels: either a DXGI format or the legacy bit masks.
        size_t bpp;
        switch (format)
        {
        case Dxgi::Rgba32F: bpp = 16; break;
        case Dxgi::Rgba16F: case Dxgi::Rgba16: bpp = 8; break;
        case Dxgi::Rgb10A2: case Dxgi::Rg11B10F: case Dxgi::Rgba8: case Dxgi::Rgba8Srgb: case Dxgi::R32F:
        case Dxgi::Bgra8: case Dxgi::Bgrx8: case Dxgi::Bgra8Srgb: case Dxgi::Bgrx8Srgb: bpp = 4; break;
        case Dxgi::Rg8: case Dxgi::R16F: case Dxgi::R16: case Dxgi::B5G6R5: case Dxgi::B5G5R5A1: case Dxgi::Bgra4: bpp = 2; break;
        case Dxgi::R8: case Dxgi::A8: bpp = 1; break;
        case Dxgi::Unknown:
            if (pfFlags & 4) return false;
            bpp = bitCount / 8;
            if (bpp < 1 || bpp > 4) return false;
            break;
        default: return false;
        }
        if (size_t(width) * height * bpp > avail) return false;
        for (uint32_t y = 0; y < height; y++)
            for (uint32_t x = 0; x < width; x++)
            {
                const uint8_t* p = src + (size_t(y) * width + x) * bpp;
                float c[4] = { 0, 0, 0, 1 };
                switch (format)
                {
                case Dxgi::Rgba32F: for (int k = 0; k < 4; k++) c[k] = LeF32(p + 4 * k); linear = true; alpha = true; break;
                case Dxgi::Rgba16F: for (int k = 0; k < 4; k++) c[k] = HalfToFloat(Le16(p + 2 * k)); linear = true; alpha = true; break;
                case Dxgi::Rgba16: for (int k = 0; k < 4; k++) c[k] = Le16(p + 2 * k) / 65535.0f; alpha = true; break;
                case Dxgi::Rgb10A2: { uint32_t v = Le32(p); c[0] = Unorm(v & 1023, 10); c[1] = Unorm((v >> 10) & 1023, 10); c[2] = Unorm((v >> 20) & 1023, 10); c[3] = Unorm(v >> 30, 2); alpha = true; break; }
                case Dxgi::Rg11B10F:
                {
                    uint32_t v = Le32(p);
                    auto packedFloat = [](uint32_t bits, int mant) { uint32_t e = bits >> mant, m = bits & ((1u << mant) - 1); return e == 0 ? std::ldexp(float(m), -14 - mant) : std::ldexp(1.0f + float(m) / float(1u << mant), int(e) - 15); };
                    c[0] = packedFloat(v & 0x7ff, 6); c[1] = packedFloat((v >> 11) & 0x7ff, 6); c[2] = packedFloat(v >> 22, 5); linear = true; break;
                }
                case Dxgi::Rgba8: case Dxgi::Rgba8Srgb: for (int k = 0; k < 4; k++) c[k] = p[k] / 255.0f; alpha = true; break;
                case Dxgi::Bgra8: case Dxgi::Bgra8Srgb: c[0] = p[2] / 255.0f; c[1] = p[1] / 255.0f; c[2] = p[0] / 255.0f; c[3] = p[3] / 255.0f; alpha = true; break;
                case Dxgi::Bgrx8: case Dxgi::Bgrx8Srgb: c[0] = p[2] / 255.0f; c[1] = p[1] / 255.0f; c[2] = p[0] / 255.0f; break;
                case Dxgi::R32F: c[0] = c[1] = c[2] = LeF32(p); linear = true; break;
                case Dxgi::Rg8: c[0] = p[0] / 255.0f; c[1] = p[1] / 255.0f; c[2] = 0; normalMap = true; break;
                case Dxgi::R16F: c[0] = c[1] = c[2] = HalfToFloat(Le16(p)); linear = true; break;
                case Dxgi::R16: c[0] = c[1] = c[2] = Le16(p) / 65535.0f; break;
                case Dxgi::B5G6R5: { uint16_t v = Le16(p); c[0] = Unorm(v >> 11, 5); c[1] = Unorm((v >> 5) & 63, 6); c[2] = Unorm(v & 31, 5); break; }
                case Dxgi::B5G5R5A1: { uint16_t v = Le16(p); c[0] = Unorm((v >> 10) & 31, 5); c[1] = Unorm((v >> 5) & 31, 5); c[2] = Unorm(v & 31, 5); c[3] = float(v >> 15); alpha = true; break; }
                case Dxgi::Bgra4: { uint16_t v = Le16(p); c[0] = Unorm((v >> 8) & 15, 4); c[1] = Unorm((v >> 4) & 15, 4); c[2] = Unorm(v & 15, 4); c[3] = Unorm(v >> 12, 4); alpha = true; break; }
                case Dxgi::R8: c[0] = c[1] = c[2] = p[0] / 255.0f; break;
                case Dxgi::A8: c[0] = c[1] = c[2] = p[0] / 255.0f; break;  // show the mask itself
                default:
                {
                    uint32_t v = 0;
                    for (size_t k = 0; k < bpp; k++) v |= uint32_t(p[k]) << (8 * k);
                    if (pfFlags & 0x20000)  // luminance
                    {
                        c[0] = c[1] = c[2] = Unorm((v & masks[0]) >> MaskShift(masks[0]), MaskBits(masks[0]));
                        if (pfFlags & 1) { c[3] = Unorm((v & masks[3]) >> MaskShift(masks[3]), MaskBits(masks[3])); alpha = true; }
                    }
                    else if ((pfFlags & 2) && !(pfFlags & 0x40))  // alpha only
                        c[0] = c[1] = c[2] = Unorm((v & masks[3]) >> MaskShift(masks[3]), MaskBits(masks[3]));
                    else
                    {
                        for (int k = 0; k < 3; k++) c[k] = Unorm((v & masks[k]) >> MaskShift(masks[k]), MaskBits(masks[k]));
                        if ((pfFlags & 1) && masks[3]) { c[3] = Unorm((v & masks[3]) >> MaskShift(masks[3]), MaskBits(masks[3])); alpha = true; }
                    }
                    break;
                }
                }
                put(x, y, c[0], c[1], c[2], c[3]);
            }
        break;
    }
    }
    if (!ok) return false;

    size_t count = size_t(width) * height;
    if (normalMap)
        for (size_t i = 0; i < count; i++)
        {
            // Rebuild the blue channel of a two-channel normal map so it looks like one.
            float nx = rgba[4 * i] * 2 - 1, ny = rgba[4 * i + 1] * 2 - 1;
            rgba[4 * i + 2] = std::sqrt(std::max(0.0f, 1.0f - nx * nx - ny * ny)) * 0.5f + 0.5f;
        }
    if (alpha)
    {
        bool anyVisible = false, anyTransparent = false;
        for (size_t i = 0; i < count; i++) { anyVisible = anyVisible || rgba[4 * i + 3] > 0.0f; anyTransparent = anyTransparent || rgba[4 * i + 3] < 1.0f; }
        if (!anyVisible || !anyTransparent) alpha = false;
        if (premultiplied && alpha)
            for (size_t i = 0; i < count; i++)
                if (rgba[4 * i + 3] > 0) for (int c = 0; c < 3; c++) rgba[4 * i + c] /= rgba[4 * i + 3];
    }
    if (linear) NormalizeIfData(rgba, count, linear);
    FloatToImage(rgba.data(), width, height, alpha, linear, maxSize, out);
    return true;
}
