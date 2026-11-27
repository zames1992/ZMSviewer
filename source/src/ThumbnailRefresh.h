// Windows maintains one local thumbnail cache for the current user. An
// association change alone leaves entries for unmodified files in that cache.
#pragma once
#include <windows.h>
#include <emptyvc.h>
#include <wrl/client.h>

namespace thumbnails {

// This is the Windows "Thumbnail Cache" Disk Cleanup category. Never run
// other cleanup categories, reset Explorer preferences or edit source files.
inline constexpr wchar_t categoryKey[] =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VolumeCaches\\Thumbnail Cache";
inline constexpr CLSID thumbnailCleaner =
    { 0x889900c3, 0x59f3, 0x4c2f, { 0xae, 0x21, 0xa4, 0x09, 0xea, 0x01, 0xe6, 0x05 } };

class CleanupSession
{
    HKEY key = nullptr;
    Microsoft::WRL::ComPtr<IEmptyVolumeCache> cleaner;
    bool active = false;
public:
    CleanupSession() = default;
    CleanupSession(const CleanupSession&) = delete;
    CleanupSession& operator=(const CleanupSession&) = delete;
    ~CleanupSession()
    {
        if (active) { DWORD flags = 0; cleaner->Deactivate(&flags); }
        if (key) RegCloseKey(key);
    }

    HRESULT Open(const wchar_t* localAppData)
    {
        wchar_t volume[MAX_PATH] = {};
        if (!GetVolumePathNameW(localAppData, volume, ARRAYSIZE(volume))) return HRESULT_FROM_WIN32(GetLastError());
        LSTATUS err = RegOpenKeyExW(HKEY_LOCAL_MACHINE, categoryKey, 0, KEY_READ, &key);
        if (err != ERROR_SUCCESS) return HRESULT_FROM_WIN32(err);
        HRESULT hr = CoCreateInstance(thumbnailCleaner, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&cleaner));
        if (FAILED(hr)) return hr;
        PWSTR label = nullptr, description = nullptr, button = nullptr;
        Microsoft::WRL::ComPtr<IEmptyVolumeCache2> localized;
        DWORD flags = 0;
        if (SUCCEEDED(cleaner.As(&localized)))
            hr = localized->InitializeEx(key, volume, L"Thumbnail Cache", &label, &description, &button, &flags);
        else
            hr = cleaner->Initialize(key, volume, &label, &description, &flags);
        CoTaskMemFree(label);
        CoTaskMemFree(description);
        CoTaskMemFree(button);
        active = SUCCEEDED(hr);
        return hr; // S_FALSE means Windows found no thumbnails for this volume.
    }

    HRESULT Scan(DWORDLONG& bytes) { return cleaner->GetSpaceUsed(&bytes, nullptr); }
    HRESULT Purge() { return cleaner->Purge(~DWORDLONG(0), nullptr); }
};

inline HRESULT ResetCache(const wchar_t* localAppData)
{
    CleanupSession cleanup;
    HRESULT hr = cleanup.Open(localAppData);
    if (hr == S_FALSE) return S_OK;
    if (FAILED(hr)) return hr;
    DWORDLONG bytes = 0;
    hr = cleanup.Scan(bytes);
    if (FAILED(hr)) return hr;
    // S_FALSE on a size query means the size is unknown. A purge is still
    // required, also for negative entries or empty database allocations.
    return cleanup.Purge();
}
}
