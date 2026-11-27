// Runs the actual installer steps with a private application hive, temporary
// known folders, and a test cache sink. No changes to desktop associations,
// thumbnail caches, the Start menu or installed applications.
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <string>
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include "../src/ThumbnailRefresh.h"

namespace fs = std::filesystem;
fs::path root;
unsigned resets = 0, notifications = 0;
HRESULT resetResult = S_OK;
HRESULT TestKnownFolder(REFKNOWNFOLDERID folder, DWORD, HANDLE, PWSTR* result)
{
    fs::path path;
    if (folder == FOLDERID_LocalAppData) path = root / L"Local";
    else if (folder == FOLDERID_Programs) path = root / L"Menu";
    else return E_INVALIDARG;
    fs::create_directories(path);
    return SHStrDupW(path.c_str(), result);
}
namespace thumbnails {
HRESULT TestResetCache(const wchar_t*) { ++resets; return resetResult; }
}
void TestChangeNotify(LONG, UINT, const void*, const void*) { ++notifications; }
#define SHGetKnownFolderPath TestKnownFolder
#define ResetCache TestResetCache
#define SHChangeNotify TestChangeNotify
#include "../src/Setup.cpp"
#undef SHGetKnownFolderPath
#undef SHChangeNotify
#undef ResetCache

void expect(bool ok, const char* what) { if (!ok) { fprintf(stderr, "FAIL %s\n", what); exit(1); } }
bool valueIs(const std::wstring& key, const wchar_t* name, const std::wstring& wanted)
{
    std::wstring value;
    return GetString(key, name, value) && value == wanted;
}
void touch(const fs::path& path)
{
    static const char data[] = "leave me here";
    WriteFileBytes(path, data, sizeof(data));
}
int wmain(int argc, wchar_t** argv)
{
    expect(argc == 3, "usage: test_setup.exe <release setup.exe> <release provider.dll>");
    HMODULE installer = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    expect(installer != nullptr, "open released installer as a resource image");
    HRSRC resource = FindResourceW(installer, MAKEINTRESOURCEW(1), RT_RCDATA);
    HGLOBAL loaded = resource ? LoadResource(installer, resource) : nullptr;
    const void* payload = loaded ? LockResource(loaded) : nullptr;
    DWORD bytes = resource ? SizeofResource(installer, resource) : 0;
    expect(payload && bytes > sizeof(IMAGE_DOS_HEADER) && FileMatches(argv[2], payload, bytes),
           "installer RCDATA 1 matches the built provider DLL byte-for-byte");

    // The batch wrapper creates a new temporary directory for this test process.
    GUID id;
    wchar_t tag[40];
    expect(SUCCEEDED(CoCreateGuid(&id)) && StringFromGUID2(id, tag, ARRAYSIZE(tag)), "generate fixture directory name");
    auto parent = fs::canonical(fs::temp_directory_path());
    root = parent / (std::wstring(L"ZMSviewer-Fixtures-") + tag);
    expect(fs::create_directory(root), "create new fixture directory");
    expect(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)), "COM");
    HKEY hive = nullptr;
    expect(RegLoadAppKeyW((root / L"private.hiv").c_str(), &hive, KEY_ALL_ACCESS, REG_PROCESS_APPKEY, 0) == ERROR_SUCCESS,
           "create private app hive");
    expect(RegOverridePredefKey(HKEY_CURRENT_USER, hive) == ERROR_SUCCESS, "isolate HKCU");
    auto dir = fs::path(DefaultDir()); fs::create_directories(dir);
    auto dist = dir / L"ZMSviewer-Setup.exe", dllUser = dir / L"ZMSviewer-myscene.dll";
    touch(dist); touch(dllUser);
    const std::wstring foreign = L"{11111111-0000-4000-9000-000000000001}", other = L"{22222222-0000-4000-9000-000000000002}";
    auto obj = HandlerPlaces(L".obj"), psd = HandlerPlaces(L".psd"), pdf = HandlerPlaces(L".pdf"), fbx = HandlerPlaces(L".fbx");
    for (const HandlerPlace& p : obj) SetString(p.key, nullptr, foreign);
    for (const HandlerPlace& p : pdf) SetString(p.key, nullptr, other);
    expect(RunQuiet(false, kAllGroups, dir) == 0, "fresh install");
    expect(fs::is_regular_file(ShortcutPath()), "Start menu shortcut");
    expect(fs::is_regular_file(dir / L"ZMSviewer.exe"), "maintenance exe");
    std::wstring dll; expect(GetString(kStateKey, L"Dll", dll) && fs::is_regular_file(dll), "payload extracted");
    expect(FileMatches(dll, payload, bytes), "extracted file is the released setup's actual DLL");
    HMODULE provider = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    expect(provider != nullptr, "load extracted provider");
    using GetClass = HRESULT (STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
    FARPROC entry = GetProcAddress(provider, "DllGetClassObject");
    GetClass getClass = nullptr;
    static_assert(sizeof(entry) == sizeof(getClass));
    memcpy(&getClass, &entry, sizeof(entry));
    expect(getClass != nullptr, "provider exports DllGetClassObject");
    IClassFactory* factory = nullptr;
    expect(SUCCEEDED(getClass(FormatClsid(Format::Pdf), IID_PPV_ARGS(&factory))) && factory,
           "extracted DLL provides the registered PDF COM class");
    factory->Release();
    FreeLibrary(provider);
    expect(valueIs(kUninstallKey, L"DisplayVersion", L"1.1.0"), "app version");
    for (const auto& e : kExtensions) {
        for (const auto& p : HandlerPlaces(e.extension))
            expect(valueIs(p.key, nullptr, ClsidString(e.format)), "handler registered");
        expect(valueIs(std::wstring(kClasses) + L"\\CLSID\\" + ClsidString(e.format) + L"\\InprocServer32", nullptr, dll),
               "handler points to extracted payload");
    }
    expect(resets == 1 && notifications >= 2, "cache refresh on fresh install");
    // Recreate the relevant 1.0 state left by the previous installer.
    for (const auto& p : obj) SetString(p.key, nullptr, ClsidString(Format::Obj, true));
    for (const auto& p : psd) SetString(p.key, nullptr, ClsidString(Format::Psd, true));
    auto oldClass = std::wstring(kClasses) + L"\\CLSID\\" + ClsidString(Format::Obj, true);
    SetString(oldClass + L"\\InprocServer32", nullptr, (dir / L"ZMSviewer-1234abcd.dll").wstring());
    SetString(kStateKey, L"Version", L"1.0.0");
    const DWORD booksModels = (1u << kBooks) | (1u << kModels);
    SetDword(kStateKey, L"Groups", booksModels);
    expect(RunQuiet(false, InstalledGroups(), L"") == 0, "upgrade from 1.0");
    expect(!KeyExists(oldClass) && InstalledGroups() == booksModels, "retired class removed and groups preserved");
    for (const auto& p : psd) expect(!valueIs(p.key, nullptr, ClsidString(Format::Psd, true)), "disabled old handler removed");
    for (const auto& p : obj) expect(valueIs(p.key, nullptr, ClsidString(Format::Obj)), "updated model handler");
    for (const auto& p : pdf) expect(valueIs(p.key, nullptr, ClsidString(Format::Pdf)), "PDF in books group");
    expect(resets == 2, "cache refresh on upgrade");
    touch(dll); expect(RunQuiet(false, InstalledGroups(), L"") == 0 && FileMatches(dll, payload, bytes), "repair corrupt payload");
    for (const auto& p : fbx) SetString(p.key, nullptr, other);
    expect(RunQuiet(false, (1u << kBooks), L"") == 0, "change file types");
    for (const auto& p : obj) expect(valueIs(p.key, nullptr, foreign), "prior model handler restored");
    for (const auto& p : fbx) expect(valueIs(p.key, nullptr, other), "later third-party change preserved");
    expect(resets == 4, "repair and modify refresh the cache");
    resetResult = E_ACCESSDENIED;
    expect(RunQuiet(true, kAllGroups, L"") != 0 && IsInstalled(), "failure to refresh is surfaced; uninstall can retry");
    resetResult = S_OK;
    expect(RunQuiet(true, kAllGroups, L"") == 0 && !IsInstalled() && !KeyExists(kStateKey), "successful uninstall");
    for (const auto& p : pdf) expect(valueIs(p.key, nullptr, other), "previous PDF handler restored");
    expect(!fs::exists(dll) && !fs::exists(dir / L"ZMSviewer.exe") && !fs::exists(ShortcutPath()), "owned files removed");
    expect(fs::exists(dist) && fs::exists(dllUser), "unrelated files preserved");
    expect(resets == 6, "uninstall refreshes cache");
    expect(RunQuiet(true, kAllGroups, L"") == 0 && resets == 6, "already absent is a no-op");
    RegOverridePredefKey(HKEY_CURRENT_USER, nullptr); RegCloseKey(hive);
    CoUninitialize();
    FreeLibrary(installer);
    expect(fs::canonical(root).parent_path() == parent && root.filename() == (std::wstring(L"ZMSviewer-Fixtures-") + tag),
           "cleanup confined to the generated temporary directory");
    std::error_code cleanupError;
    fs::remove_all(root, cleanupError);
    expect(!cleanupError, "delete temporary fixtures");
    printf("PASS released installer resource, extracted provider, isolated fresh install, 1.0 upgrade, group filtering, repair, cache failure/retry, uninstall, ownership and restoration\n");
}
