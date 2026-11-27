// Read a Photoshop composite or a saved thumbnail from the read-only Explorer stream.
#include "Common.h"
#include <algorithm>
#include <cmath>
#include "../vendor/ufbx.h"

namespace {
// ---------------------------------------------------------------------------------------------
// Buffered big-endian reader over the IStream Explorer hands us.

class StreamReader
{
public:
    explicit StreamReader(IStream* stream) : m_stream(stream), m_buffer(1 << 20) {}

    bool Rewind()
    {
        LARGE_INTEGER zero = {};
        ULARGE_INTEGER end = {};
        if (FAILED(m_stream->Seek(zero, STREAM_SEEK_END, &end)) || end.QuadPart > INT64_MAX) return false;
        m_size = end.QuadPart;
        m_start = 0;
        m_pos = m_len = 0;
        return SUCCEEDED(m_stream->Seek(zero, STREAM_SEEK_SET, nullptr));
    }

    uint64_t Tell() const { return m_start + m_pos; }
    uint64_t Size() const { return m_size; }

    bool Seek(uint64_t target)
    {
        if (target > m_size) return false;
        if (target >= m_start && target - m_start <= m_len)
        {
            m_pos = size_t(target - m_start);
            return true;
        }
        LARGE_INTEGER offset;
        offset.QuadPart = static_cast<LONGLONG>(target);
        if (FAILED(m_stream->Seek(offset, STREAM_SEEK_SET, nullptr))) return false;
        m_start = target;
        m_pos = m_len = 0;
        return true;
    }

    bool Read(void* dst, size_t count)
    {
        auto* out = static_cast<uint8_t*>(dst);
        while (count > 0)
        {
            if (m_pos == m_len && !Fill())
                return false;
            size_t take = std::min(count, m_len - m_pos);
            memcpy(out, m_buffer.data() + m_pos, take);
            m_pos += take;
            out += take;
            count -= take;
        }
        return true;
    }

    bool Skip(uint64_t count)
    {
        return count <= m_size - Tell() && Seek(Tell() + count);
    }

    bool U8(uint8_t& v) { return Read(&v, 1); }

    bool U16(uint16_t& v)
    {
        uint8_t b[2];
        if (!Read(b, 2)) return false;
        v = static_cast<uint16_t>((b[0] << 8) | b[1]);
        return true;
    }

    bool U32(uint32_t& v)
    {
        uint8_t b[4];
        if (!Read(b, 4)) return false;
        v = (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3];
        return true;
    }

    bool U64(uint64_t& v)
    {
        uint32_t hi, lo;
        if (!U32(hi) || !U32(lo)) return false;
        v = (uint64_t(hi) << 32) | lo;
        return true;
    }

    // PSD lengths are 4 bytes, PSB uses 8 for the big sections.
    bool Length(bool wide, uint64_t& v)
    {
        if (wide) return U64(v);
        uint32_t v32;
        if (!U32(v32)) return false;
        v = v32;
        return true;
    }

private:
    bool Fill()
    {
        m_start += m_len;
        ULONG got = 0;
        HRESULT hr = m_stream->Read(m_buffer.data(), static_cast<ULONG>(m_buffer.size()), &got);
        m_pos = 0;
        m_len = got;
        return SUCCEEDED(hr) && got > 0;
    }

    IStream* m_stream;
    std::vector<uint8_t> m_buffer;
    uint64_t m_start = 0;
    uint64_t m_size = 0;
    size_t m_pos = 0;
    size_t m_len = 0;
};

// ---------------------------------------------------------------------------------------------
// What the parser learns about a document before the merged image.

enum ColorMode : uint16_t
{
    ModeBitmap = 0, ModeGray = 1, ModeIndexed = 2, ModeRgb = 3, ModeCmyk = 4,
    ModeMultichannel = 7, ModeDuotone = 8, ModeLab = 9,
};

struct PsdInfo
{
    bool psb = false;
    uint16_t channels = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint16_t depth = 0;
    uint16_t mode = 0;
    std::vector<uint8_t> palette;      // indexed mode: 256 R, 256 G, 256 B
    std::vector<uint8_t> preview;      // JPEG or RGB thumbnail resource (including its header)
    bool previewBgr = false;           // resource 1033 has Photoshop 4's channel order
    bool hasRealMergedData = true;     // resource 1057
    bool mergedAlpha = false;          // negative layer count: first extra channel is transparency
    uint64_t layerStart = 0, layerEnd = 0;
};


static bool ReadHeader(StreamReader& r, PsdInfo& info)
{
    uint8_t sig[4];
    uint16_t version;
    if (!r.Read(sig, 4) || memcmp(sig, "8BPS", 4) != 0) return false;
    if (!r.U16(version) || (version != 1 && version != 2)) return false;
    info.psb = version == 2;
    return r.Skip(6) && r.U16(info.channels) && r.U32(info.height) && r.U32(info.width)
        && r.U16(info.depth) && r.U16(info.mode)
        && info.width > 0 && info.height > 0 && info.channels > 0 && info.channels <= 56
        && info.width <= (info.psb ? 300000u : 30000u) && info.height <= (info.psb ? 300000u : 30000u);
}

static bool ReadColorModeData(StreamReader& r, PsdInfo& info)
{
    uint32_t length;
    if (!r.U32(length)) return false;
    if (info.mode == ModeIndexed && length >= 768)
    {
        info.palette.resize(768);
        return r.Read(info.palette.data(), 768) && r.Skip(length - 768);
    }
    return r.Skip(length);
}

static bool ReadImageResources(StreamReader& r, PsdInfo& info)
{
    uint32_t length;
    if (!r.U32(length)) return false;
    if (length > r.Size() - r.Tell()) return false;
    uint64_t end = r.Tell() + length;
    while (r.Tell() + 12 <= end)
    {
        uint8_t sig[4];
        uint16_t id;
        uint8_t nameLength;
        uint32_t size;
        if (!r.Read(sig, 4) || !r.U16(id) || !r.U8(nameLength)) return false;
        // The Pascal name with its length byte is padded to an even size.
        if (!r.Skip(nameLength + ((nameLength + 1) & 1))) return false;
        if (!r.U32(size)) return false;
        uint64_t padded = uint64_t(size) + (size & 1);
        if (r.Tell() > end || padded > end - r.Tell()) return false;

        if ((id == 1036 || (id == 1033 && info.preview.empty())) && size > 28 && size <= (16u << 20))
        {
            info.preview.resize(size);
            info.previewBgr = id == 1033;
            if (!r.Read(info.preview.data(), size) || !r.Skip(padded - size)) return false;
        }
        else if (id == 1057 && size >= 5)
        {
            uint32_t version;
            uint8_t merged;
            if (!r.U32(version) || !r.U8(merged)) return false;
            info.hasRealMergedData = merged != 0;
            if (!r.Skip(padded - 5)) return false;
        }
        else if (!r.Skip(padded))
            return false;
    }
    return r.Tell() <= end && r.Skip(end - r.Tell());
}

// Only one fact matters here: whether the merged image carries transparency in its first extra
// channel. 8-bit documents say so with a negative layer count; 16/32-bit ones keep their layers
// in an Lr16/Lr32 tagged block after the global mask and flag it with an empty Mt16/Mt32 block.
static bool ReadLayerSection(StreamReader& r, PsdInfo& info)
{
    uint64_t length;
    if (!r.Length(info.psb, length)) return false;
    if (length > r.Size() - r.Tell()) return false;
    uint64_t end = r.Tell() + length;
    if (length == 0) return true;

    uint64_t layerInfoLength;
    if (!r.Length(info.psb, layerInfoLength) || r.Tell() > end || layerInfoLength > end - r.Tell()) return false;
    if (layerInfoLength >= 2)
    {
        info.layerStart = r.Tell();
        info.layerEnd = info.layerStart + layerInfoLength;
        uint16_t count;
        if (!r.U16(count)) return false;
        info.mergedAlpha = info.mergedAlpha || static_cast<int16_t>(count) < 0;
    }
    else if (end - r.Tell() >= 4)
    {
        uint32_t maskLength;
        if (!r.Skip(layerInfoLength) || !r.U32(maskLength) || !r.Skip(maskLength)) return false;
        while (r.Tell() + 12 <= end)
        {
            uint8_t sig[4], key[4];
            if (!r.Read(sig, 4) || !r.Read(key, 4)) return false;
            if (memcmp(sig, "8BIM", 4) != 0 && memcmp(sig, "8B64", 4) != 0) break;
            if (memcmp(key, "Mtrn", 4) == 0 || memcmp(key, "Mt16", 4) == 0 || memcmp(key, "Mt32", 4) == 0)
                info.mergedAlpha = true;
            bool layers = memcmp(key, "Lr16", 4) == 0 || memcmp(key, "Lr32", 4) == 0
                || memcmp(key, "Layr", 4) == 0;
            static const char* wideKeys[] = { "LMsk", "Lr16", "Lr32", "Layr", "Mt16", "Mt32",
                "Mtrn", "Alph", "FMsk", "lnk2", "FEid", "FXid", "PxSD" };
            bool wide = false;
            if (info.psb)
                for (const char* k : wideKeys)
                    wide = wide || memcmp(key, k, 4) == 0;
            uint64_t blockLength;
            if (!r.Length(wide, blockLength)) return false;
            if (layers && blockLength >= 2)
            {
                if (r.Tell() > end || blockLength > end - r.Tell()) return false;
                info.layerStart = r.Tell();
                info.layerEnd = info.layerStart + blockLength;
                uint16_t count;
                if (!r.U16(count)) return false;
                info.mergedAlpha = info.mergedAlpha || static_cast<int16_t>(count) < 0;
                break;
            }
            if (!r.Skip(blockLength)) return false;
        }
    }
    return r.Tell() <= end && r.Skip(end - r.Tell());
}

// ---------------------------------------------------------------------------------------------
// Planar channels are filtered down to the requested thumbnail size.

static bool UnpackBits(const uint8_t* src, size_t srcLength, uint8_t* dst, size_t dstLength)
{
    size_t i = 0, o = 0;
    while (i < srcLength && o < dstLength)
    {
        int8_t n = static_cast<int8_t>(src[i++]);
        if (n >= 0)
        {
            size_t count = size_t(n) + 1;
            if (i + count > srcLength || o + count > dstLength) return false;
            memcpy(dst + o, src + i, count);
            i += count;
            o += count;
        }
        else if (n != -128)
        {
            size_t count = size_t(1 - n);
            if (i >= srcLength || o + count > dstLength) return false;
            memset(dst + o, src[i++], count);
            o += count;
        }
    }
    return o == dstLength;
}

// A composite stores every plane in a single coded section. A layer stores one
// such section per plane. All reads below are confined to the declared section.
class ChannelRows
{
public:
    explicit ChannelRows(StreamReader& reader) : r(reader) {}

    bool Open(bool psb, uint32_t width, uint32_t height, uint16_t depth,
        uint16_t channels, uint64_t end)
    {
        if (end > r.Size() || r.Tell() > end || end - r.Tell() < 2 || !r.U16(compression)) return false;
        bits = depth;
        limit = end;
        const uint64_t rowCount = uint64_t(height) * channels;
        const size_t rowBytes = (size_t(width) * depth + 7) / 8;
        const uint64_t total = rowBytes * rowCount;
        if (rowBytes == 0 || rowCount == 0) return false;
        row.resize(rowBytes);
        if (compression == 0) return total <= end - r.Tell();
        if (compression == 1)
        {
            if (rowCount > (end - r.Tell()) / (psb ? 4 : 2) || rowCount > (16u << 20)) return false;
            lengths.resize(size_t(rowCount));
            for (auto& length : lengths)
            {
                if (psb) { if (!r.U32(length)) return false; }
                else { uint16_t shortLength; if (!r.U16(shortLength)) return false; length = shortLength; }
            }
            return true;
        }
        if (compression > 3 || (compression == 3 && depth == 1) || total > (192ull << 20)) return false;
        const size_t compressedBytes = size_t(end - r.Tell());
        if (compressedBytes == 0) return false;
        inflated.resize(size_t(total));
        std::vector<uint8_t> inputBuffer(64 * 1024);
        ufbx_inflate_input input = {};
        input.total_size = compressedBytes;
        input.read_user = &r;
        input.read_fn = [](void* user, void* data, size_t count) -> size_t {
            return static_cast<StreamReader*>(user)->Read(data, count) ? count : SIZE_MAX;
        };
        input.buffer = inputBuffer.data();
        input.buffer_size = inputBuffer.size();
        ufbx_inflate_retain retain;
        retain.initialized = false;
        return ufbx_inflate(inflated.data(), inflated.size(), &input, &retain) == ptrdiff_t(total);
    }

    bool Next()
    {
        if (compression == 0)
            return row.size() <= limit - r.Tell() && r.Read(row.data(), row.size());
        if (compression == 1)
        {
            if (index >= lengths.size()) return false;
            const size_t size = lengths[index++];
            if (size > row.size() * 2 + 1024 || size > limit - r.Tell()) return false;
            packed.resize(size);
            return r.Read(packed.data(), size) && UnpackBits(packed.data(), size, row.data(), row.size());
        }
        if (index >= inflated.size() / row.size()) return false;
        uint8_t* src = inflated.data() + index++ * row.size();
        if (compression == 2)
            memcpy(row.data(), src, row.size());
        else if (bits == 16)
        {
            uint16_t previous = 0;
            for (size_t i = 0; i < row.size(); i += 2)
            {
                previous = uint16_t(previous + Be16(src + i));
                row[i] = uint8_t(previous >> 8);
                row[i + 1] = uint8_t(previous);
            }
        }
        else
        {
            for (size_t i = 1; i < row.size(); i++) src[i] += src[i - 1];
            if (bits == 8) memcpy(row.data(), src, row.size());
            else
            {
                // Float samples are delta-coded after splitting the row into byte planes.
                const size_t width = row.size() / 4;
                for (size_t x = 0; x < width; x++)
                    for (size_t b = 0; b < 4; b++) row[4 * x + b] = src[b * width + x];
            }
        }
        return true;
    }

    std::vector<uint8_t> row;
private:
    StreamReader& r;
    uint16_t compression = 0, bits = 0;
    uint64_t limit = 0;
    size_t index = 0;
    std::vector<uint32_t> lengths;
    std::vector<uint8_t> packed, inflated;
};

static float SampleAt(const uint8_t* row, uint32_t x, uint16_t depth)
{
    switch (depth)
    {
    case 1: return (row[x >> 3] & (0x80 >> (x & 7))) ? 0.0f : 1.0f;
    case 8: return row[x] / 255.0f;
    case 16: return ((row[2 * x] << 8) | row[2 * x + 1]) / 65535.0f;
    default:
    {
        const uint8_t* p = row + 4 * x;
        uint32_t bits = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
        float v;
        memcpy(&v, &bits, 4);
        return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f;
    }
    }
}

static int BaseChannels(uint16_t mode)
{
    switch (mode)
    {
    case ModeRgb: case ModeLab: return 3;
    case ModeCmyk: return 4;
    default: return 1;
    }
}

static void LabToRgb(float l, float a, float b, float rgb[3])
{
    float fy = (l + 16.0f) / 116.0f, fx = fy + a / 500.0f, fz = fy - b / 200.0f;
    auto finv = [](float t) { return t > 6.0f / 29.0f ? t * t * t : 3.0f * 36.0f / 841.0f * (t - 4.0f / 29.0f); };
    float x = 0.9642f * finv(fx), y = finv(fy), z = 0.8249f * finv(fz);  // D50 white
    float lin[3] = {
        3.1338561f * x - 1.6168667f * y - 0.4906146f * z,
        -0.9787684f * x + 1.9161415f * y + 0.0334540f * z,
        0.0719453f * x - 0.2289914f * y + 1.4052427f * z,
    };
    for (int i = 0; i < 3; i++)
        rgb[i] = LinearToSrgb(std::clamp(lin[i], 0.0f, 1.0f));
}

static bool DecodeMergedImage(StreamReader& r, const PsdInfo& info, UINT maxSize, Image& out)
{
    if (info.depth != 1 && info.depth != 8 && info.depth != 16 && info.depth != 32) return false;
    if (info.mode == ModeIndexed && (info.depth != 8 || info.palette.size() != 768)) return false;

    const uint32_t w = info.width, h = info.height;
    const int base = BaseChannels(info.mode);
    if (info.channels < base || uint64_t(w) * h > (400ull << 20)) return false;
    ChannelRows rows(r);
    if (!rows.Open(info.psb, w, h, info.depth, info.channels, r.Size())) return false;
    const bool alpha = info.mergedAlpha && info.channels > base;
    const int sourceChannels = base + (alpha ? 1 : 0);
    // Indexed colors are looked up before averaging, so they fill three accumulators.
    const int accChannels = (info.mode == ModeIndexed ? 3 : base) + (alpha ? 1 : 0);

    double scale = std::min(1.0, double(maxSize) / std::max(w, h));
    UINT dw = std::max<UINT>(1, UINT(std::lround(w * scale)));
    UINT dh = std::max<UINT>(1, UINT(std::lround(h * scale)));
    std::vector<UINT> xmap(w), ymap(h);
    std::vector<float> columnWeight(dw, 0.0f), rowWeight(dh, 0.0f);
    for (uint32_t x = 0; x < w; x++) { xmap[x] = UINT(uint64_t(x) * dw / w); columnWeight[xmap[x]] += 1; }
    for (uint32_t y = 0; y < h; y++) { ymap[y] = UINT(uint64_t(y) * dh / h); rowWeight[ymap[y]] += 1; }
    std::vector<float> acc(size_t(dw) * dh * accChannels, 0.0f);

    for (int c = 0; c < sourceChannels; c++)
    {
        // The transparency channel follows the color channels directly.
        int accBase = (info.mode == ModeIndexed && c == 0) ? 0 : (c == base ? accChannels - 1 : c);
        for (uint32_t y = 0; y < h; y++)
        {
            if (!rows.Next()) return false;
            const auto& row = rows.row;

            float* line = acc.data() + size_t(ymap[y]) * dw * accChannels;
            if (info.mode == ModeIndexed && c == 0)
            {
                for (uint32_t x = 0; x < w; x++)
                {
                    uint8_t index = row[x];
                    float* px = line + size_t(xmap[x]) * accChannels;
                    px[0] += info.palette[index] / 255.0f;
                    px[1] += info.palette[256 + index] / 255.0f;
                    px[2] += info.palette[512 + index] / 255.0f;
                }
            }
            else
            {
                for (uint32_t x = 0; x < w; x++)
                    line[size_t(xmap[x]) * accChannels + accBase] += SampleAt(row.data(), x, info.depth);
            }
        }
    }

    out.width = dw;
    out.height = dh;
    out.alpha = alpha;
    out.bgra.assign(size_t(dw) * dh * 4, 255);
    for (UINT y = 0; y < dh; y++)
    {
        for (UINT x = 0; x < dw; x++)
        {
            float weight = columnWeight[x] * rowWeight[y];
            const float* px = acc.data() + (size_t(y) * dw + x) * accChannels;
            float v[5];
            for (int c = 0; c < accChannels; c++) v[c] = px[c] / weight;
            float a = alpha ? v[accChannels - 1] : 1.0f;
            // The merged image is matted against white where it is transparent; undo that in
            // the document's own color space (white is 0.5 on Lab's a and b).
            if (alpha && a > 0.0f)
            {
                int colorChannels = accChannels - 1;
                for (int c = 0; c < colorChannels; c++)
                {
                    float white = (info.mode == ModeLab && c > 0) ? 128.0f / 255.0f : 1.0f;
                    v[c] = std::clamp((v[c] - (1.0f - a) * white) / a, 0.0f, 1.0f);
                }
            }

            float rgb[3];
            switch (info.mode)
            {
            case ModeRgb: case ModeIndexed:
                rgb[0] = v[0]; rgb[1] = v[1]; rgb[2] = v[2];
                break;
            case ModeCmyk:  // stored inverted: 1 means no ink
                rgb[0] = v[0] * v[3]; rgb[1] = v[1] * v[3]; rgb[2] = v[2] * v[3];
                break;
            case ModeLab:
                LabToRgb(v[0] * 100.0f, v[1] * 255.0f - 128.0f, v[2] * 255.0f - 128.0f, rgb);
                break;
            default:
                rgb[0] = rgb[1] = rgb[2] = v[0];
                break;
            }

            uint8_t* dst = out.bgra.data() + (size_t(y) * dw + x) * 4;
            dst[0] = uint8_t(std::lround(std::clamp(rgb[2], 0.0f, 1.0f) * 255));
            dst[1] = uint8_t(std::lround(std::clamp(rgb[1], 0.0f, 1.0f) * 255));
            dst[2] = uint8_t(std::lround(std::clamp(rgb[0], 0.0f, 1.0f) * 255));
            dst[3] = uint8_t(std::lround(std::clamp(a, 0.0f, 1.0f) * 255));
        }
    }
    return true;
}

static bool DecodePreview(const PsdInfo& info, UINT maxSize, Image& out)
{
    if (info.preview.size() < 28) return false;
    const uint8_t* header = info.preview.data();
    uint32_t format = Be32(header), width = Be32(header + 4), height = Be32(header + 8);
    uint32_t stride = Be32(header + 12);
    const uint8_t* data = header + 28;
    const size_t size = info.preview.size() - 28;
    if (format == 1)
    {
        if (size < 2 || data[0] != 0xff || data[1] != 0xd8 ||
            !DecodeWithWic(data, size, out, 4u << 20)) return false;
    }
    else
    {
        if (format != 0 || !width || !height || uint64_t(width) * height > (4u << 20)
            || Be16(header + 24) != 24 || Be16(header + 26) != 1
            || stride < uint64_t(width) * 3 || uint64_t(stride) * height > size) return false;
        out.width = width;
        out.height = height;
        out.bgra.assign(size_t(width) * height * 4, 255);
        for (uint32_t y = 0; y < height; y++)
            for (uint32_t x = 0; x < width; x++)
            {
                const auto* rgb = data + size_t(stride) * y + size_t(x) * 3;
                auto* dst = out.bgra.data() + (size_t(y) * width + x) * 4;
                dst[0] = rgb[2]; dst[1] = rgb[1]; dst[2] = rgb[0];
            }
    }
    if (info.previewBgr)
        for (size_t i = 0; i < out.bgra.size(); i += 4) std::swap(out.bgra[i], out.bgra[i + 2]);
    out.alpha = false;
    FitImage(out, maxSize);
    return true;
}

struct LayerPlane
{
    int16_t id;
    uint64_t length;
};

struct RasterLayer
{
    int32_t top, left, bottom, right;
    bool visible;
    uint8_t opacity;
    std::vector<LayerPlane> planes;
};

// Reconstruct ordinary RGB/gray pixel layers if the writer omitted both composite
// and preview. A document using masks, groups or unsupported blending needs its
// author's saved composite: displaying the underlying unmasked pixels is incorrect.
static bool ReadRasterLayer(StreamReader& r, bool psb, uint64_t end, RasterLayer& layer)
{
    uint8_t rect[16], mode[12];
    uint16_t channels;
    if (!r.Read(rect, sizeof(rect)) || !r.U16(channels) || channels > 56) return false;
    layer.top = int32_t(Be32(rect)); layer.left = int32_t(Be32(rect + 4));
    layer.bottom = int32_t(Be32(rect + 8)); layer.right = int32_t(Be32(rect + 12));
    for (uint16_t i = 0; i < channels; i++)
    {
        uint16_t id;
        uint64_t length;
        if (!r.U16(id) || !r.Length(psb, length)) return false;
        layer.planes.push_back({int16_t(id), length});
    }
    uint32_t extra;
    if (!r.Read(mode, sizeof(mode)) || memcmp(mode, "8BIM", 4) != 0 || !r.U32(extra)
        || r.Tell() > end || extra > end - r.Tell()) return false;
    layer.opacity = mode[8];
    layer.visible = (mode[10] & 2) == 0 && layer.opacity > 0;
    if (memcmp(mode + 4, "norm", 4) != 0 || mode[9] != 0 || (mode[10] & 0x10)) return false;
    uint64_t extraEnd = r.Tell() + extra;
    uint32_t maskBytes, ranges;
    uint8_t nameLength;
    if (extra < 12 || !r.U32(maskBytes) || maskBytes != 0 || !r.U32(ranges)
        || ranges > extraEnd - r.Tell() || (ranges & 3)) return false;
    for (uint32_t i = 0; i < ranges; i += 4)
    {
        uint32_t range;
        if (!r.U32(range) || range != 0x0000ffff) return false;
    }
    if (!r.U8(nameLength) || !r.Skip(((size_t(nameLength) + 4) & ~size_t(3)) - 1)) return false;
    while (r.Tell() < extraEnd)
    {
        uint8_t tag[8];
        uint32_t length;
        if (extraEnd - r.Tell() < 12 || !r.Read(tag, 8) || memcmp(tag, "8BIM", 4)
            || !r.U32(length)) return false;
        // Photoshop also saves editable type information beside the rasterized text.
        const char* metadata[] = {"luni", "lyid", "lclr", "lyvr", "lspf", "shmd", "fxrp", "lnsr", "TySh"};
        bool safe = false;
        for (const char* key : metadata) if (memcmp(tag + 4, key, 4) == 0) safe = true;
        if (length == 4 && (memcmp(tag + 4, "clbl", 4) == 0 || memcmp(tag + 4, "infx", 4) == 0
                || memcmp(tag + 4, "knko", 4) == 0))
        {
            uint32_t value;
            uint32_t usual = memcmp(tag + 4, "clbl", 4) == 0 ? 0x01000000 : 0;
            if (!r.U32(value) || value != usual) return false;
            continue;
        }
        if (!safe || !r.Skip(uint64_t(length) + (length & 1))) return false;
    }
    return r.Tell() == extraEnd;
}

static bool RenderRasterLayers(StreamReader& r, const PsdInfo& info, UINT maxSize, Image& out)
{
    if (!info.layerStart || (info.mode != ModeRgb && info.mode != ModeGray)
        || (info.depth != 8 && info.depth != 16 && info.depth != 32) || !r.Seek(info.layerStart)) return false;
    uint16_t count;
    if (!r.U16(count)) return false;
    int numLayers = std::abs(int(int16_t(count)));
    if (numLayers == 0 || numLayers > 2048) return false;
    std::vector<RasterLayer> layers(numLayers);
    for (auto& layer : layers)
        if (!ReadRasterLayer(r, info.psb, info.layerEnd, layer)) return false;
    const double scale = std::min(1.0, double(maxSize) / std::max(info.width, info.height));
    out.width = std::max<UINT>(1, UINT(std::lround(info.width * scale)));
    out.height = std::max<UINT>(1, UINT(std::lround(info.height * scale)));
    out.bgra.assign(size_t(out.width) * out.height * 4, 0);
    std::vector<uint32_t> xs(out.width), ys(out.height);
    for (UINT x = 0; x < out.width; x++) xs[x] = uint32_t((2ull * x + 1) * info.width / (2 * out.width));
    for (UINT y = 0; y < out.height; y++) ys[y] = uint32_t((2ull * y + 1) * info.height / (2 * out.height));
    uint64_t decodedSamples = 0;
    bool drawn = false;
    for (const auto& layer : layers)
    {
        const int64_t w = int64_t(layer.right) - layer.left, h = int64_t(layer.bottom) - layer.top;
        if (w < 0 || h < 0 || w > 300000 || h > 300000) return false;
        const auto firstX = std::lower_bound(xs.begin(), xs.end(), uint32_t(std::max(0, layer.left)));
        const auto lastX = std::lower_bound(xs.begin(), xs.end(), uint32_t(std::max(0, layer.right)));
        const auto firstY = std::lower_bound(ys.begin(), ys.end(), uint32_t(std::max(0, layer.top)));
        const auto lastY = std::lower_bound(ys.begin(), ys.end(), uint32_t(std::max(0, layer.bottom)));
        const size_t tw = size_t(lastX - firstX), th = size_t(lastY - firstY);
        std::vector<uint8_t> tile;
        const bool visible = layer.visible && tw && th;
        if (visible) tile.assign(tw * th * 4, 255);
        int seen = 0;
        for (const auto& plane : layer.planes)
        {
            if (r.Tell() > info.layerEnd || plane.length > info.layerEnd - r.Tell()) return false;
            uint64_t end = r.Tell() + plane.length;
            if (visible)
            {
                if (plane.id < -1 || plane.id >= BaseChannels(info.mode)) return false;
                const int channel = plane.id == -1 ? 3 : (info.mode == ModeGray ? 0 : 2 - plane.id);
                if ((seen & (1 << channel)) != 0) return false;
                seen |= 1 << channel;
                decodedSamples += uint64_t(w) * h;
                if (decodedSamples > (500ull << 20)) return false;
                ChannelRows rows(r);
                if (!rows.Open(info.psb, uint32_t(w), uint32_t(h), info.depth, 1, end)) return false;
                auto outputY = firstY;
                for (int64_t y = 0; y < h && outputY != lastY; y++)
                {
                    if (!rows.Next()) return false;
                    if (y + layer.top != *outputY) continue;
                    uint8_t* dst = tile.data() + size_t(outputY - firstY) * tw * 4 + channel;
                    for (auto it = firstX; it != lastX; ++it, dst += 4)
                        *dst = uint8_t(std::lround(255 * SampleAt(rows.row.data(), uint32_t(int64_t(*it) - layer.left), info.depth)));
                    ++outputY;
                }
            }
            if (!r.Seek(end)) return false;
        }
        if (!visible) continue;
        int required = info.mode == ModeGray ? 1 : 7;
        if ((seen & required) != required) return false;
        drawn = true;
        for (size_t y = 0; y < th; y++)
            for (size_t x = 0; x < tw; x++)
            {
                const auto* src = tile.data() + (y * tw + x) * 4;
                auto* dst = out.bgra.data() + ((size_t(firstY - ys.begin()) + y) * out.width
                    + size_t(firstX - xs.begin()) + x) * 4;
                const unsigned a = (src[3] * layer.opacity + 127) / 255;
                const unsigned back = dst[3] * (255 - a), sum = a * 255 + back;
                if (!sum) continue;
                for (int c = 0; c < 3; c++)
                    dst[c] = uint8_t((src[info.mode == ModeGray ? 0 : c] * a * 255 + dst[c] * back + sum / 2) / sum);
                dst[3] = uint8_t((sum + 127) / 255);
            }
    }
    out.alpha = true;
    return drawn;
}

} // namespace

bool LoadPsd(IStream* stream, UINT maxSize, Image& out)
{
    StreamReader r(stream);
    PsdInfo info;
    if (!r.Rewind() || !ReadHeader(r, info) || !ReadColorModeData(r, info) || !ReadImageResources(r, info))
        return false;
    // Large canvases can carry a preview already big enough for Explorer.
    if (info.preview.size() >= 28 && (uint64_t(info.width) * info.height > (32ull << 20)
        || std::max(Be32(info.preview.data() + 4), Be32(info.preview.data() + 8)) >= maxSize)
        && DecodePreview(info, maxSize, out)) return true;
    const bool layersOk = ReadLayerSection(r, info);
    if (layersOk && info.hasRealMergedData && DecodeMergedImage(r, info, maxSize, out))
        return true;
    if (DecodePreview(info, maxSize, out)) return true;
    return layersOk && RenderRasterLayers(r, info, maxSize, out);
}
