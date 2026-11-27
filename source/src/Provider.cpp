// COM side: one thumbnail provider class per format (see Formats.h), handed to Explorer through
// DllGetClassObject. Explorer gives the provider a read-only stream of the file.
#include "Common.h"
#include <shlwapi.h>
#include <thumbcache.h>
#include <new>

namespace {

long g_dllRefs = 0;

using Loader = bool (*)(IStream*, UINT, Image&);

Loader LoaderFor(Format f)
{
    switch (f)
    {
    case Format::Psd: return LoadPsd;
    case Format::Tiff: return LoadTiff;
    case Format::Tga: return LoadTga;
    case Format::Exr: return LoadExr;
    case Format::Hdr: return LoadHdr;
    case Format::Dds: return LoadDds;
    case Format::Fb2: return LoadFb2;
    case Format::Epub: return LoadEpub;
    case Format::Pdf: return LoadPdf;
    case Format::Cbz: return LoadCbz;
    case Format::Mobi: return LoadMobi;
    case Format::Obj: return LoadObj;
    case Format::Stl: return LoadStl;
    case Format::Fbx: return LoadFbx;
    case Format::Max3ds: return Load3ds;
    case Format::Blend: return LoadBlend;
    case Format::MayaAscii: return LoadMayaAscii;
    case Format::MayaBinary: return LoadMayaBinary;
    case Format::Ply: return LoadPly;
    case Format::Glb: return LoadGlb;
    default: return nullptr;
    }
}

HBITMAP ToBitmap(const Image& image)
{
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = static_cast<LONG>(image.width);
    bmi.bmiHeader.biHeight = -static_cast<LONG>(image.height);
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap) return nullptr;
    // Explorer expects premultiplied alpha for WTSAT_ARGB.
    auto* dst = static_cast<uint8_t*>(bits);
    for (size_t i = 0; i < size_t(image.width) * image.height; i++)
    {
        const uint8_t* s = image.bgra.data() + 4 * i;
        uint8_t* d = dst + 4 * i;
        uint32_t a = image.alpha ? s[3] : 255;
        d[0] = uint8_t((s[0] * a + 127) / 255);
        d[1] = uint8_t((s[1] * a + 127) / 255);
        d[2] = uint8_t((s[2] * a + 127) / 255);
        d[3] = uint8_t(a);
    }
    return bitmap;
}

class Provider : public IInitializeWithStream, public IThumbnailProvider
{
public:
    explicit Provider(Format format) : m_format(format) { InterlockedIncrement(&g_dllRefs); }

    ~Provider()
    {
        if (m_stream) m_stream->Release();
        InterlockedDecrement(&g_dllRefs);
    }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv)
    {
        static const QITAB qit[] = {
            QITABENT(Provider, IInitializeWithStream),
            QITABENT(Provider, IThumbnailProvider),
            { 0 },
        };
        return QISearch(this, qit, riid, ppv);
    }

    IFACEMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&m_refs); }

    IFACEMETHODIMP_(ULONG) Release()
    {
        long refs = InterlockedDecrement(&m_refs);
        if (refs == 0) delete this;
        return refs;
    }

    IFACEMETHODIMP Initialize(IStream* stream, DWORD)
    {
        if (!stream) return E_INVALIDARG;
        if (m_stream) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        return stream->QueryInterface(IID_PPV_ARGS(&m_stream));
    }

    IFACEMETHODIMP GetThumbnail(UINT cx, HBITMAP* bitmap, WTS_ALPHATYPE* alphaType)
    {
        if (!bitmap || !alphaType) return E_POINTER;
        *bitmap = nullptr;
        if (!m_stream) return E_UNEXPECTED;
        Loader load = LoaderFor(m_format);
        if (!load) return E_FAIL;
        if (cx == 0 || cx > 2560) cx = 256;
        Image image;
        try
        {
            if (!load(m_stream, cx, image) || image.width == 0 || image.height == 0
                || image.bgra.size() != size_t(image.width) * image.height * 4)
                return E_FAIL;
        }
        catch (const std::bad_alloc&)
        {
            return E_OUTOFMEMORY;
        }
        catch (...)
        {
            return E_FAIL;
        }
        *bitmap = ToBitmap(image);
        *alphaType = image.alpha ? WTSAT_ARGB : WTSAT_RGB;
        return *bitmap ? S_OK : E_OUTOFMEMORY;
    }

private:
    long m_refs = 1;
    Format m_format;
    IStream* m_stream = nullptr;
};

class ClassFactory : public IClassFactory
{
public:
    Format format = Format::Psd;

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv)
    {
        static const QITAB qit[] = { QITABENT(ClassFactory, IClassFactory), { 0 } };
        return QISearch(this, qit, riid, ppv);
    }

    IFACEMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&g_dllRefs); }

    IFACEMETHODIMP_(ULONG) Release() { return InterlockedDecrement(&g_dllRefs); }

    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv)
    {
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        auto* provider = new (std::nothrow) Provider(format);
        if (!provider) return E_OUTOFMEMORY;
        HRESULT hr = provider->QueryInterface(riid, ppv);
        provider->Release();
        return hr;
    }

    IFACEMETHODIMP LockServer(BOOL lock)
    {
        if (lock) InterlockedIncrement(&g_dllRefs);
        else InterlockedDecrement(&g_dllRefs);
        return S_OK;
    }
};

struct Factories
{
    ClassFactory byFormat[int(Format::Count)];
    Factories()
    {
        for (int i = 0; i < int(Format::Count); ++i) byFormat[i].format = Format(i);
    }
};
Factories g_factories;

} // namespace

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    for (int i = 0; i < int(Format::Count); i++)
        if (IsEqualCLSID(clsid, FormatClsid(Format(i))))
        {
            return g_factories.byFormat[i].QueryInterface(riid, ppv);
        }
    return CLASS_E_CLASSNOTAVAILABLE;
}

STDAPI DllCanUnloadNow()
{
    return g_dllRefs == 0 ? S_OK : S_FALSE;
}
