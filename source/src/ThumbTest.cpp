// Test tool: renders thumbnails to PNG, straight through the DLL or through the shell (the path
// Explorer takes, so it also proves the registration).
//   ThumbTest dll <ZMSviewer.dll> <file> <out.png> [size]
//   ThumbTest shell <file> <out.png> [size]
//   ThumbTest batch <ZMSviewer.dll> <list.txt (UTF-8, one path per line)> <outdir> [size]

#define NOMINMAX
#include <windows.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <thumbcache.h>
#include <wincodec.h>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>
#include "Formats.h"

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

static HRESULT SavePng(HBITMAP bitmap, bool alpha, const wchar_t* path)
{
    IWICImagingFactory* factory = nullptr;
    IWICBitmap* source = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = factory->CreateBitmapFromHBITMAP(bitmap, nullptr, alpha ? WICBitmapUsePremultipliedAlpha : WICBitmapIgnoreAlpha, &source);
    if (SUCCEEDED(hr)) hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path, GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, nullptr);
    if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
    if (SUCCEEDED(hr)) hr = frame->WriteSource(source, nullptr);
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();
    if (frame) frame->Release();
    if (encoder) encoder->Release();
    if (stream) stream->Release();
    if (source) source->Release();
    if (factory) factory->Release();
    return hr;
}

static bool FormatOf(const wchar_t* file, Format& format)
{
    const wchar_t* ext = PathFindExtensionW(file);
    for (auto& e : kExtensions)
        if (_wcsicmp(e.extension, ext) == 0) { format = e.format; return true; }
    return false;
}

using GetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);

static HRESULT ThroughDll(GetClassObjectFn getClassObject, const wchar_t* file, UINT size, HBITMAP* bitmap, bool* alpha)
{
    Format format;
    if (!FormatOf(file, format)) return E_INVALIDARG;
    IClassFactory* factory = nullptr;
    IInitializeWithStream* init = nullptr;
    IThumbnailProvider* provider = nullptr;
    IStream* stream = nullptr;
    WTS_ALPHATYPE alphaType = WTSAT_UNKNOWN;
    HRESULT hr = getClassObject(FormatClsid(format), IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = factory->CreateInstance(nullptr, IID_PPV_ARGS(&init));
    if (SUCCEEDED(hr)) hr = SHCreateStreamOnFileEx(file, STGM_READ | STGM_SHARE_DENY_NONE, 0, FALSE, nullptr, &stream);
    if (SUCCEEDED(hr)) hr = init->Initialize(stream, STGM_READ);
    if (SUCCEEDED(hr)) hr = init->QueryInterface(IID_PPV_ARGS(&provider));
    if (SUCCEEDED(hr)) hr = provider->GetThumbnail(size, bitmap, &alphaType);
    *alpha = alphaType == WTSAT_ARGB;
    if (provider) provider->Release();
    if (stream) stream->Release();
    if (init) init->Release();
    if (factory) factory->Release();
    return hr;
}

// Asks the Windows thumbnail cache to extract again (whatever handler Explorer would pick),
// so a stale cached picture does not hide the result.
static HRESULT ThroughShell(const wchar_t* file, UINT size, HBITMAP* bitmap)
{
    IShellItem* item = nullptr;
    IThumbnailCache* cache = nullptr;
    ISharedBitmap* shared = nullptr;
    HRESULT hr = SHCreateItemFromParsingName(file, nullptr, IID_PPV_ARGS(&item));
    if (SUCCEEDED(hr)) hr = CoCreateInstance(CLSID_LocalThumbnailCache, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&cache));
    if (SUCCEEDED(hr)) hr = cache->GetThumbnail(item, size, WTS_EXTRACT | WTS_FORCEEXTRACTION, &shared, nullptr, nullptr);
    if (SUCCEEDED(hr) && shared) hr = shared->Detach(bitmap);
    else if (SUCCEEDED(hr)) hr = E_FAIL;
    if (shared) shared->Release();
    if (cache) cache->Release();
    if (item) item->Release();
    return hr;
}

static GetClassObjectFn LoadDll(const wchar_t* dll)
{
    HMODULE module = LoadLibraryW(dll);
    return module ? reinterpret_cast<GetClassObjectFn>(GetProcAddress(module, "DllGetClassObject")) : nullptr;
}

int wmain(int argc, wchar_t** argv)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (argc >= 5 && wcscmp(argv[1], L"batch") == 0)
    {
        GetClassObjectFn get = LoadDll(argv[2]);
        if (!get) { fwprintf(stderr, L"cannot load %s\n", argv[2]); return 1; }
        UINT size = argc > 5 ? UINT(_wtoi(argv[5])) : 256;
        FILE* f = nullptr;
        if (_wfopen_s(&f, argv[3], L"rb") || !f) return 1;
        std::string all;
        char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) all.append(buf, n);
        fclose(f);
        int index = 0, failed = 0;
        size_t pos = 0;
        while (pos < all.size())
        {
            size_t nl = all.find('\n', pos);
            std::string line = all.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            pos = nl == std::string::npos ? all.size() : nl + 1;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.empty()) continue;
            int wl = MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1, nullptr, 0);
            std::wstring path(wl, 0);
            MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1, &path[0], wl);
            path.resize(wl - 1);
            index++;
            wchar_t out[MAX_PATH];
            swprintf(out, MAX_PATH, L"%s\\%03d%s.png", argv[4], index, PathFindExtensionW(path.c_str()));
            HBITMAP bitmap = nullptr;
            bool alpha = false;
            DWORD start = GetTickCount();
            HRESULT hr = ThroughDll(get, path.c_str(), size, &bitmap, &alpha);
            DWORD elapsed = GetTickCount() - start;
            if (SUCCEEDED(hr)) hr = SavePng(bitmap, alpha, out);
            BITMAP info = {};
            if (bitmap) { GetObject(bitmap, sizeof(info), &info); DeleteObject(bitmap); }
            if (FAILED(hr)) failed++;
            wprintf(L"%03d hr=0x%08X %4dx%-4d a=%d %6lums  %s\n", index, unsigned(hr), info.bmWidth, info.bmHeight, alpha ? 1 : 0, elapsed, path.c_str());
            fflush(stdout);
        }
        wprintf(L"done: %d files, %d failed\n", index, failed);
        CoUninitialize();
        return failed ? 1 : 0;
    }

    bool dll = argc >= 5 && wcscmp(argv[1], L"dll") == 0;
    bool shell = argc >= 4 && wcscmp(argv[1], L"shell") == 0;
    if (!dll && !shell)
    {
        fwprintf(stderr, L"usage: ThumbTest dll <dll> <file> <out.png> [size] | shell <file> <out.png> [size] | batch <dll> <list> <outdir> [size]\n");
        return 2;
    }
    int sizeArg = dll ? 5 : 4;
    UINT size = argc > sizeArg ? UINT(_wtoi(argv[sizeArg])) : 256;
    HBITMAP bitmap = nullptr;
    bool alpha = true;
    DWORD start = GetTickCount();
    HRESULT hr;
    if (dll)
    {
        GetClassObjectFn get = LoadDll(argv[2]);
        hr = get ? ThroughDll(get, argv[3], size, &bitmap, &alpha) : HRESULT_FROM_WIN32(GetLastError());
    }
    else
        hr = ThroughShell(argv[2], size, &bitmap);
    DWORD elapsed = GetTickCount() - start;
    if (SUCCEEDED(hr)) hr = SavePng(bitmap, alpha, dll ? argv[4] : argv[3]);
    BITMAP info = {};
    if (bitmap) GetObject(bitmap, sizeof(info), &info);
    wprintf(L"hr=0x%08X %dx%d alpha=%d %lums\n", unsigned(hr), info.bmWidth, info.bmHeight, alpha ? 1 : 0, elapsed);
    if (bitmap) DeleteObject(bitmap);
    CoUninitialize();
    return SUCCEEDED(hr) ? 0 : 1;
}
