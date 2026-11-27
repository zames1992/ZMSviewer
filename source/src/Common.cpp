#include "Common.h"
#include <shlwapi.h>
#include <wincodec.h>
#include <algorithm>
#include <cmath>

#include "../vendor/ufbx.h"

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "windowscodecs.lib")

bool ReadAll(IStream* stream, std::vector<uint8_t>& data, uint64_t limit)
{
    STATSTG stat = {};
    LARGE_INTEGER zero = {};
    if (FAILED(stream->Seek(zero, STREAM_SEEK_SET, nullptr))) return false;
    uint64_t size = SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME)) ? stat.cbSize.QuadPart : 0;
    if (size > limit) return false;
    data.clear();
    if (size) data.reserve(size_t(size));
    uint8_t buffer[1 << 16];
    for (;;)
    {
        ULONG got = 0;
        HRESULT hr = stream->Read(buffer, sizeof(buffer), &got);
        if (FAILED(hr)) return false;
        if (got == 0) break;
        if (data.size() + got > limit) return false;
        data.insert(data.end(), buffer, buffer + got);
        if (hr == S_FALSE) break;
    }
    return !data.empty();
}

static bool DecodeWicStream(IStream* stream, Image& out, uint64_t maxPixels)
{
    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    bool ok = false;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))
        && SUCCEEDED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder))
        && SUCCEEDED(decoder->GetFrame(0, &frame))
        && SUCCEEDED(factory->CreateFormatConverter(&converter))
        && SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
            nullptr, 0.0, WICBitmapPaletteTypeCustom))
        && SUCCEEDED(converter->GetSize(&out.width, &out.height))
        && out.width > 0 && out.height > 0 && uint64_t(out.width) * out.height <= maxPixels)
    {
        out.alpha = true;
        out.bgra.resize(size_t(out.width) * out.height * 4);
        ok = SUCCEEDED(converter->CopyPixels(nullptr, out.width * 4, static_cast<UINT>(out.bgra.size()), out.bgra.data()));
        if (ok)
        {
            // Opaque pictures are handed over without alpha so Explorer does not draw a frame oddly.
            bool anyTransparent = false;
            for (size_t i = 3; i < out.bgra.size() && !anyTransparent; i += 4) anyTransparent = out.bgra[i] != 255;
            out.alpha = anyTransparent;
        }
    }
    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (factory) factory->Release();
    return ok;
}

bool DecodeWithWic(const uint8_t* data, size_t size, Image& out, uint64_t maxPixels)
{
    if (!data || size == 0 || size > 0x7fffffff) return false;
    IStream* stream = SHCreateMemStream(data, static_cast<UINT>(size));
    if (!stream) return false;
    bool ok = DecodeWicStream(stream, out, maxPixels);
    stream->Release();
    return ok;
}

bool DecodeWithWic(IStream* stream, Image& out)
{
    LARGE_INTEGER zero = {};
    if (FAILED(stream->Seek(zero, STREAM_SEEK_SET, nullptr))) return false;
    return DecodeWicStream(stream, out, 400ull << 20);
}

float LinearToSrgb(float v)
{
    if (!(v > 0.0f)) return 0.0f;
    if (v >= 1.0f) return 1.0f;
    return v <= 0.0031308f ? 12.92f * v : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

Downsampler::Downsampler(uint32_t width, uint32_t height, UINT maxSize, int channels) : m_channels(channels)
{
    double scale = std::min(1.0, double(maxSize) / std::max(width, height));
    m_dw = std::max<UINT>(1, UINT(std::lround(width * scale)));
    m_dh = std::max<UINT>(1, UINT(std::lround(height * scale)));
    m_xmap.resize(width);
    m_ymap.resize(height);
    m_colWeight.assign(m_dw, 0.0f);
    m_rowWeight.assign(m_dh, 0.0f);
    for (uint32_t x = 0; x < width; x++) { m_xmap[x] = UINT(uint64_t(x) * m_dw / width); m_colWeight[m_xmap[x]] += 1; }
    for (uint32_t y = 0; y < height; y++) { m_ymap[y] = UINT(uint64_t(y) * m_dh / height); m_rowWeight[m_ymap[y]] += 1; }
    m_acc.assign(size_t(m_dw) * m_dh * channels, 0.0f);
}

void FitImage(Image& image, UINT maxSize)
{
    if (image.width <= maxSize && image.height <= maxSize) return;
    Downsampler ds(image.width, image.height, maxSize, 4);
    for (UINT y = 0; y < image.height; y++)
    {
        const uint8_t* row = image.bgra.data() + size_t(y) * image.width * 4;
        for (UINT x = 0; x < image.width; x++)
        {
            // Weight color by alpha so transparent pixels do not bleed their (often black) color.
            float a = row[4 * x + 3] / 255.0f;
            ds.Add(x, y, 0, row[4 * x] * a);
            ds.Add(x, y, 1, row[4 * x + 1] * a);
            ds.Add(x, y, 2, row[4 * x + 2] * a);
            ds.Add(x, y, 3, a);
        }
    }
    Image reduced;
    reduced.width = ds.OutWidth();
    reduced.height = ds.OutHeight();
    reduced.alpha = image.alpha;
    reduced.bgra.resize(size_t(reduced.width) * reduced.height * 4);
    for (UINT y = 0; y < reduced.height; y++)
        for (UINT x = 0; x < reduced.width; x++)
        {
            float a = ds.Get(x, y, 3);
            uint8_t* d = reduced.bgra.data() + (size_t(y) * reduced.width + x) * 4;
            for (int c = 0; c < 3; c++)
                d[c] = a > 0 ? uint8_t(std::clamp(std::lround(ds.Get(x, y, c) / a), 0L, 255L)) : 0;
            d[3] = uint8_t(std::clamp(std::lround(a * 255.0f), 0L, 255L));
        }
    image = std::move(reduced);
}

void FloatToImage(const float* rgba, UINT width, UINT height, bool alpha, bool linear, UINT maxSize, Image& out)
{
    Downsampler ds(width, height, maxSize, 4);
    for (UINT y = 0; y < height; y++)
        for (UINT x = 0; x < width; x++)
        {
            const float* p = rgba + (size_t(y) * width + x) * 4;
            float a = alpha ? std::clamp(std::isfinite(p[3]) ? p[3] : 0.0f, 0.0f, 1.0f) : 1.0f;
            for (int c = 0; c < 3; c++)
            {
                float v = std::isfinite(p[c]) ? p[c] : 0.0f;
                // Clamp HDR values so one hot pixel cannot dominate the box average.
                ds.Add(x, y, c, std::clamp(v, 0.0f, linear ? 4.0f : 1.0f) * a);
            }
            ds.Add(x, y, 3, a);
        }
    out.width = ds.OutWidth();
    out.height = ds.OutHeight();
    out.alpha = alpha;
    out.bgra.resize(size_t(out.width) * out.height * 4);
    for (UINT y = 0; y < out.height; y++)
        for (UINT x = 0; x < out.width; x++)
        {
            float a = ds.Get(x, y, 3);
            uint8_t* d = out.bgra.data() + (size_t(y) * out.width + x) * 4;
            for (int c = 0; c < 3; c++)
            {
                float v = a > 0 ? ds.Get(x, y, c) / a : 0.0f;
                if (linear) v = LinearToSrgb(v);
                d[2 - c] = uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
            }
            d[3] = uint8_t(std::lround(std::clamp(a, 0.0f, 1.0f) * 255.0f));
        }
}

bool Inflate(const uint8_t* src, size_t size, bool zlibHeader, std::vector<uint8_t>& out, size_t expected, bool allowShort)
{
    if (!src || !size || !expected || expected > (512ull << 20)) return false;
    ufbx_inflate_input input = {};
    input.data = src;
    input.data_size = input.total_size = size;
    input.no_header = input.no_checksum = !zlibHeader;
    ufbx_inflate_retain state = {};
    out.resize(expected);
    ptrdiff_t length = ufbx_inflate(out.data(), out.size(), &input, &state);
    if (length < 0 || (!allowShort && size_t(length) != expected)) { out.clear(); return false; }
    out.resize(size_t(length));
    return true;
}
