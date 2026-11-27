// Shared pieces of the thumbnail providers: the picture type, stream helpers, resizing and
// the list of formats with their COM class ids.
#pragma once

#define NOMINMAX
#include <windows.h>
#include <objidl.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "Formats.h"

// The finished picture: top-down BGRA, straight (not premultiplied) alpha.
struct Image
{
    UINT width = 0;
    UINT height = 0;
    bool alpha = false;
    std::vector<uint8_t> bgra;
};

// Whole stream into memory, refusing files bigger than limit.
bool ReadAll(IStream* stream, std::vector<uint8_t>& data, uint64_t limit = 1536ull << 20);

// Decodes any picture Windows Imaging Component understands (JPEG, PNG, GIF, BMP, TIFF, WebP...).
bool DecodeWithWic(const uint8_t* data, size_t size, Image& out, uint64_t maxPixels = 400ull << 20);
bool DecodeWithWic(IStream* stream, Image& out);

// Shrinks the picture so its longer side is at most maxSize (box filter); never enlarges.
void FitImage(Image& image, UINT maxSize);

// Float RGBA (straight alpha) to an Image, shrinking on the way. linear = values are scene-linear
// (EXR, HDR) and get converted to sRGB; otherwise they are already display values 0..1.
void FloatToImage(const float* rgba, UINT width, UINT height, bool alpha, bool linear, UINT maxSize, Image& out);

float LinearToSrgb(float v);

// Streaming box filter: feed full-resolution pixels row by row, get a thumbnail-sized picture,
// so a huge image never has to sit in memory whole.
class Downsampler
{
public:
    Downsampler(uint32_t width, uint32_t height, UINT maxSize, int channels);
    UINT OutWidth() const { return m_dw; }
    UINT OutHeight() const { return m_dh; }
    void Add(uint32_t x, uint32_t y, int channel, float value)
    {
        m_acc[(size_t(m_ymap[y]) * m_dw + m_xmap[x]) * m_channels + channel] += value;
    }
    // Averaged value of a thumbnail pixel.
    float Get(UINT x, UINT y, int channel) const
    {
        return m_acc[(size_t(y) * m_dw + x) * m_channels + channel] / (m_colWeight[x] * m_rowWeight[y]);
    }
private:
    UINT m_dw, m_dh;
    int m_channels;
    std::vector<UINT> m_xmap, m_ymap;
    std::vector<float> m_colWeight, m_rowWeight, m_acc;
};

// Little helpers for parsing bytes in memory.
inline uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
inline uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
inline uint64_t Le64(const uint8_t* p) { return Le32(p) | (uint64_t(Le32(p + 4)) << 32); }
inline uint16_t Be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
inline uint32_t Be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
inline uint64_t Be64(const uint8_t* p) { return (uint64_t(Be32(p)) << 32) | Be32(p + 4); }
inline float LeF32(const uint8_t* p) { uint32_t b = Le32(p); float f; memcpy(&f, &b, 4); return f; }

// Inflate (zlib-wrapped or raw deflate); false on damaged data or when the result would pass limit.
bool Inflate(const uint8_t* src, size_t size, bool zlibHeader, std::vector<uint8_t>& out, size_t expected, bool allowShort = false);

// Per-format loaders. Each fills out with a picture no bigger than maxSize where it can.
bool LoadPsd(IStream* stream, UINT maxSize, Image& out);
bool LoadTiff(IStream* stream, UINT maxSize, Image& out);
bool LoadTga(IStream* stream, UINT maxSize, Image& out);
bool LoadExr(IStream* stream, UINT maxSize, Image& out);
bool LoadHdr(IStream* stream, UINT maxSize, Image& out);
bool LoadDds(IStream* stream, UINT maxSize, Image& out);
bool LoadFb2(IStream* stream, UINT maxSize, Image& out);
bool LoadEpub(IStream* stream, UINT maxSize, Image& out);
bool LoadPdf(IStream* stream, UINT maxSize, Image& out);
bool LoadCbz(IStream* stream, UINT maxSize, Image& out);
bool LoadMobi(IStream* stream, UINT maxSize, Image& out);
bool LoadObj(IStream* stream, UINT maxSize, Image& out);
bool LoadStl(IStream* stream, UINT maxSize, Image& out);
bool LoadFbx(IStream* stream, UINT maxSize, Image& out);
bool Load3ds(IStream* stream, UINT maxSize, Image& out);
bool LoadBlend(IStream* stream, UINT maxSize, Image& out);
bool LoadMayaAscii(IStream* stream, UINT maxSize, Image& out);
bool LoadMayaBinary(IStream* stream, UINT maxSize, Image& out);
bool LoadPly(IStream* stream, UINT maxSize, Image& out);
bool LoadGlb(IStream* stream, UINT maxSize, Image& out);
