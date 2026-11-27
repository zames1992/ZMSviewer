// ZMSviewer Setup: a wizard installer with the thumbnail DLL inside.
// Installs for the current user, so it needs no administrator rights:
//   <folder>\ZMSviewer-<hash>.dll                         the thumbnail providers
//   <folder>\ZMSviewer.exe                                a copy of this program (change / repair / remove)
//   Start menu\Programs\ZMSviewer.lnk                     opens ZMSviewer.exe
//   HKCU\Software\Classes\CLSID\{...}                     one class per format
//   HKCU\Software\Classes\<ext>\ShellEx\{e357fccd-...}    and SystemFileAssociations\<ext>\...: the handler link
//   HKCU\...\CurrentVersion\Uninstall\ZMSviewer           the Settings -> Apps entry
// The default folder is %LocalAppData%\Programs\ZMSviewer. The DLL gets a content-hashed name, so
// an update works even while Explorer still has the old copy loaded. Thumbnail handlers that
// were set for an extension before are remembered and put back on removal. Older versions
// ("Explorer Thumbnails", "PSD превью для Проводника") are replaced.
//
// Command line: /install [/types=pictures,books,models] [/dir=<folder>], /uninstall, /quiet, /en, /ru.
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <knownfolders.h>
#include <wincodec.h>
#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <functional>
#include <initializer_list>
#include <memory>
#include <iterator>
#include <string>
#include <vector>
#include "Formats.h"
#include "ThumbnailRefresh.h"

namespace {

const wchar_t kVersion[] = L"1.1.0";
const wchar_t kProduct[] = L"ZMSviewer";
const wchar_t kPublisher[] = L"ZMS";
const wchar_t kThumbnailHandler[] = L"{e357fccd-a995-4576-b01f-234630154e96}";
const wchar_t kClasses[] = L"Software\\Classes";
const wchar_t kUninstallKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\ZMSviewer";
const wchar_t kStateKey[] = L"Software\\ZMSviewer";
const wchar_t kPreviousKey[] = L"Software\\ZMSviewer\\PreviousHandlers";
// Earlier versions of this tool.
const wchar_t kLegacyUninstallKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\ExplorerThumbnails";
const wchar_t kLegacyStateKey[] = L"Software\\ExplorerThumbnails";
const wchar_t kLegacyPreviousKey[] = L"Software\\ExplorerThumbnails\\PreviousHandlers";
const wchar_t kOldClsid[] = L"{76638739-7E1C-4DE3-A007-703CAC5401E6}";
const wchar_t kOldUninstallKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\PsdThumbnailer";

enum Group { kPictures, kBooks, kModels, kGroupCount };
const DWORD kAllGroups = 7;
Group GroupOf(Format f)
{
    if (f <= Format::Dds) return kPictures;
    if (f >= Format::Obj && f <= Format::Glb) return kModels;
    return kBooks;
}

// ---------------------------------------------------------------- texts
struct Texts
{
    const wchar_t *setupTitle, *tagline, *versionFmt;
    const wchar_t *welcomeTitle, *welcomeBody;
    const wchar_t *typesTitle, *typesBody, *groups[3];
    const wchar_t *folderTitle, *folderBody, *browse, *folderNote;
    const wchar_t *maintainTitle, *maintainBody, *modify, *modifyNote, *repair, *repairNote, *remove, *removeNote;
    const wchar_t *removeTitle, *removeBody;
    const wchar_t *installingTitle, *removingTitle;
    const wchar_t *doneTitle, *doneBody, *changedBody, *removedTitle, *removedBody, *failedTitle;
    const wchar_t *back, *next, *install, *apply, *removeButton, *cancel, *finish, *close;
    const wchar_t *confirmExit, *pickOne, *stepPrepare, *stepOld, *stepCopy, *stepRegister, *stepUnregister, *stepShortcut,
        *stepApps, *stepRefresh, *stepFiles, *stepDone;
};

const Texts kEnglish = {
    L"ZMSviewer Setup", L"Previews for Windows Explorer", L"Version %s",
    L"Welcome to ZMSviewer",
    L"ZMSviewer shows real previews in File Explorer and on the desktop for files Windows leaves as blank icons: "
    L"Photoshop documents, HDR images, game textures, e-books and 3D models.\n\n"
    L"Setup refreshes this account's thumbnail cache, including thumbnails of other file types. Windows rebuilds them as folders are opened. Your files are only read.\n\n"
    L"Click Next to continue.",
    L"File types", L"Choose which files should get previews. You can change this later from the Start menu.",
    { L"Pictures", L"PDF and e-books", L"3D models (clay render)" },
    L"Install location", L"ZMSviewer will be installed in this folder:", L"Browse...",
    L"Click Install to update previews.",
    L"ZMSviewer is installed", L"What would you like to do?",
    L"Change file types", L"Turn previews on or off for pictures, e-books or 3D models",
    L"Repair", L"Install this version again",
    L"Remove", L"Uninstall ZMSviewer and restore previous thumbnail handlers",
    L"Remove ZMSviewer", L"ZMSviewer will be removed from your computer. Previews you had before it was installed come back.\n\nClick Remove to continue.",
    L"Installing ZMSviewer", L"Removing ZMSviewer",
    L"ZMSviewer is ready",
    L"Open a folder with these files and choose View → Large icons.\n\n"
    L"Windows rebuilds thumbnails as folders are viewed. Large models can take a moment to appear.\n\nTo change file types or remove ZMSviewer later, "
    L"open ZMSviewer from the Start menu or use Settings → Apps.",
    L"Your changes are applied. Explorer rebuilds thumbnails as you view your folders.",
    L"ZMSviewer has been removed", L"Explorer will use the thumbnail handlers you had before installing ZMSviewer.",
    L"Setup could not finish",
    L"< Back", L"Next >", L"Install", L"Apply", L"Remove", L"Cancel", L"Finish", L"Close",
    L"Exit ZMSviewer Setup?", L"Choose at least one file type.",
    L"Preparing...", L"Removing older versions...", L"Copying files...", L"Registering %s", L"Unregistering %s",
    L"Updating the Start menu...", L"Adding to Settings → Apps...", L"Refreshing Explorer...", L"Removing files...", L"Done",
};

const Texts kRussian = {
    L"Установка ZMSviewer", L"Превью для Проводника Windows", L"Версия %s",
    L"Добро пожаловать в ZMSviewer",
    L"ZMSviewer показывает настоящие превью в Проводнике и на рабочем столе для файлов, у которых Windows рисует пустые значки: "
    L"документы Photoshop, HDR-картинки, игровые текстуры, электронные книги и 3D-модели.\n\n"
    L"Установщик обновит кэш эскизов этой учётной записи, включая эскизы других форматов. Windows создаст их заново при просмотре папок. Исходные файлы только читаются.\n\n"
    L"Нажмите «Далее».",
    L"Типы файлов", L"Выберите, для каких файлов показывать превью. Потом это можно изменить через меню «Пуск».",
    { L"Картинки", L"PDF и электронные книги", L"3D-модели («глиняный» рендер)" },
    L"Папка установки", L"ZMSviewer будет установлен в эту папку:", L"Обзор...",
    L"Нажмите «Установить», чтобы обновить превью.",
    L"ZMSviewer уже установлен", L"Что сделать?",
    L"Изменить типы файлов", L"Включить или выключить превью для картинок, книг или 3D-моделей",
    L"Восстановить", L"Установить эту версию заново",
    L"Удалить", L"Удалить ZMSviewer и вернуть прежние обработчики эскизов",
    L"Удаление ZMSviewer", L"ZMSviewer будет удалён с компьютера. Превью, которые были до установки, вернутся.\n\nНажмите «Удалить».",
    L"Установка ZMSviewer", L"Удаление ZMSviewer",
    L"ZMSviewer готов к работе",
    L"Откройте папку с такими файлами и выберите Вид → Крупные значки.\n\n"
    L"Проводник заново создаёт эскизы при просмотре папок. Для больших моделей это может занять некоторое время.\n\nЧтобы потом изменить типы файлов или "
    L"удалить ZMSviewer, откройте ZMSviewer в меню «Пуск» или через Параметры → Приложения.",
    L"Изменения применены. Проводник заново создаёт эскизы при просмотре папок.",
    L"ZMSviewer удалён", L"Проводник будет использовать обработчики эскизов, которые действовали до установки ZMSviewer.",
    L"Установка не завершена",
    L"< Назад", L"Далее >", L"Установить", L"Применить", L"Удалить", L"Отмена", L"Готово", L"Закрыть",
    L"Выйти из установки ZMSviewer?", L"Выберите хотя бы один тип файлов.",
    L"Подготовка...", L"Удаление старых версий...", L"Копирование файлов...", L"Регистрация %s", L"Отключение %s",
    L"Обновление меню «Пуск»...", L"Добавление в Параметры → Приложения...", L"Обновление Проводника...", L"Удаление файлов...", L"Готово",
};

const Texts* T = &kEnglish;

// ---------------------------------------------------------------- helpers
struct Failure { std::wstring what; };

void Check(LSTATUS status, const std::wstring& what)
{
    if (status == ERROR_SUCCESS) return;
    wchar_t* message = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   DWORD(status), 0, reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    std::wstring text = what + L"\n" + (message ? message : L"");
    if (message) LocalFree(message);
    throw Failure{ text };
}
void Fail(const std::wstring& what) { DWORD e = GetLastError(); Check(LSTATUS(e ? e : ERROR_GEN_FAILURE), what); }

std::wstring Format1(const wchar_t* fmt, const std::wstring& a)
{
    wchar_t buffer[512];
    swprintf_s(buffer, fmt, a.c_str());
    return buffer;
}

std::wstring ClsidString(Format f, bool version10 = false)
{
    wchar_t text[64] = {};
    CLSID id = FormatClsid(f);
    if (version10) id.Data1 = 0x6e1f3a20;
    StringFromGUID2(id, text, 64);
    return text;
}

bool IsOurClsid(const std::wstring& value)
{
    for (int f = 0; f < int(Format::Count); f++)
    {
        if (_wcsicmp(value.c_str(), ClsidString(Format(f)).c_str()) == 0 ||
            (f <= int(Format::Glb) && _wcsicmp(value.c_str(), ClsidString(Format(f), true).c_str()) == 0)) return true;
    }
    return _wcsicmp(value.c_str(), kOldClsid) == 0;
}

void SetString(const std::wstring& key, const wchar_t* name, const std::wstring& value)
{
    Check(RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), name, REG_SZ, value.c_str(), DWORD((value.size() + 1) * sizeof(wchar_t))),
          L"Registry: " + key);
}
void SetDword(const std::wstring& key, const wchar_t* name, DWORD value)
{
    Check(RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), name, REG_DWORD, &value, sizeof(value)), L"Registry: " + key);
}
bool GetString(const std::wstring& key, const wchar_t* name, std::wstring& value)
{
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), name, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS) return false;
    std::wstring buffer(bytes / sizeof(wchar_t) + 1, L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), name, RRF_RT_REG_SZ, nullptr, buffer.data(), &bytes) != ERROR_SUCCESS) return false;
    buffer.resize(wcslen(buffer.c_str()));
    value = buffer;
    return true;
}
bool GetDword(const std::wstring& key, const wchar_t* name, DWORD& value)
{
    DWORD bytes = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, key.c_str(), name, RRF_RT_REG_DWORD, nullptr, &value, &bytes) == ERROR_SUCCESS;
}
bool KeyExists(const std::wstring& key)
{
    HKEY h;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS) return false;
    RegCloseKey(h);
    return true;
}
void DeleteTree(const std::wstring& key) { RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str()); RegDeleteKeyW(HKEY_CURRENT_USER, key.c_str()); }
void DeleteIfEmpty(const std::wstring& key)  // only when nothing else is left in it
{
    HKEY h;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS) return;
    DWORD subkeys = 1, values = 1;
    RegQueryInfoKeyW(h, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr, &values, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(h);
    if (subkeys == 0 && values == 0) RegDeleteKeyW(HKEY_CURRENT_USER, key.c_str());
}

// Where the handler goes for one extension. Windows looks at <ext>\ShellEx first, then the file
// type's ProgID, then SystemFileAssociations\<ext>; other programs (Windows' own 3D viewer,
// Blender) register at the first level, so we write both. Per-user values that were there
// before are kept under PreviousHandlers ("<ext>" and "sfa:<ext>") and put back on removal;
// machine-wide handlers simply show through again once our per-user key is gone.
struct HandlerPlace { std::wstring base, key, saved; };
std::vector<HandlerPlace> HandlerPlaces(const wchar_t* extension)
{
    std::wstring ext = std::wstring(kClasses) + L"\\" + extension;
    std::wstring sfa = std::wstring(kClasses) + L"\\SystemFileAssociations\\" + extension;
    return {
        { ext, ext + L"\\ShellEx\\" + kThumbnailHandler, extension },
        { sfa, sfa + L"\\ShellEx\\" + kThumbnailHandler, std::wstring(L"sfa:") + extension },
    };
}

void RegisterExtension(const FormatExtension& e)
{
    for (const HandlerPlace& place : HandlerPlaces(e.extension))
    {
        std::wstring previous;
        if (GetString(place.key, nullptr, previous) && !IsOurClsid(previous))
            SetString(kPreviousKey, place.saved.c_str(), previous);
        SetString(place.key, nullptr, ClsidString(e.format));
    }
}

void UnregisterExtension(const FormatExtension& e)
{
    for (const HandlerPlace& place : HandlerPlaces(e.extension))
    {
        std::wstring value, previous;
        if (!GetString(place.key, nullptr, value) || !IsOurClsid(value)) continue;  // someone else's handler: leave it
        if (GetString(kPreviousKey, place.saved.c_str(), previous) && !IsOurClsid(previous))
        {
            SetString(place.key, nullptr, previous);
            RegDeleteKeyValueW(HKEY_CURRENT_USER, kPreviousKey, place.saved.c_str());
        }
        else
        {
            Check(RegDeleteKeyValueW(HKEY_CURRENT_USER, place.key.c_str(), nullptr), L"Registry: " + place.key);
            DeleteIfEmpty(place.key);
            DeleteIfEmpty(place.base + L"\\ShellEx");
            DeleteIfEmpty(place.base);
        }
    }
}

std::wstring KnownFolder(REFKNOWNFOLDERID id)
{
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &path))) throw Failure{ L"A system folder was not found." };
    std::wstring s = path;
    CoTaskMemFree(path);
    return s;
}
std::wstring DefaultDir() { return KnownFolder(FOLDERID_LocalAppData) + L"\\Programs\\ZMSviewer"; }
std::wstring ShortcutPath() { return KnownFolder(FOLDERID_Programs) + L"\\ZMSviewer.lnk"; }

std::wstring ThisExe()
{
    std::wstring path(MAX_PATH, L'\0');
    for (;;)
    {
        DWORD n = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
        if (n < path.size()) { path.resize(n); return path; }
        path.resize(path.size() * 2);
    }
}
std::wstring DirOf(const std::wstring& path) { return path.substr(0, path.find_last_of(L'\\')); }
bool SamePath(const std::wstring& a, const std::wstring& b)
{
    wchar_t fa[MAX_PATH * 2] = {}, fb[MAX_PATH * 2] = {};
    GetFullPathNameW(a.c_str(), DWORD(std::size(fa)), fa, nullptr);
    GetFullPathNameW(b.c_str(), DWORD(std::size(fb)), fb, nullptr);
    size_t la = wcslen(fa), lb = wcslen(fb);
    while (la > 3 && fa[la - 1] == L'\\') fa[--la] = 0;
    while (lb > 3 && fb[lb - 1] == L'\\') fb[--lb] = 0;
    return _wcsicmp(fa, fb) == 0;
}

void WriteFileBytes(const std::wstring& path, const void* data, DWORD size)
{
    std::wstring temp = path + L".new";
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) Fail(L"Cannot write " + temp);
    DWORD written = 0;
    BOOL ok = WriteFile(file, data, size, &written, nullptr) && written == size;
    CloseHandle(file);
    if (!ok) { DeleteFileW(temp.c_str()); Fail(L"Cannot write " + temp); }
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) { DeleteFileW(temp.c_str()); Fail(L"Cannot write " + path); }
}

bool FileMatches(const std::wstring& path, const void* data, DWORD size)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER length{};
    bool equal = GetFileSizeEx(file, &length) && length.QuadPart == size;
    BYTE buffer[16384];
    for (DWORD offset = 0; equal && offset < size;)
    {
        DWORD count = std::min<DWORD>(sizeof(buffer), size - offset), read = 0;
        equal = ReadFile(file, buffer, count, &read, nullptr) && read == count &&
                memcmp(buffer, static_cast<const BYTE*>(data) + offset, count) == 0;
        offset += count;
    }
    CloseHandle(file);
    return equal;
}

// A running setup or loaded DLL may be renamed before it can be deleted. Only
// retry a unique retired name: a later repair can immediately reuse the original
// name. The child receives paths as data, never as shell source or wildcards.
void RemoveOwnedFile(const std::wstring& path)
{
    if (DeleteFileW(path.c_str())) return;
    GUID token = {};
    wchar_t guid[40] = {}, windows[MAX_PATH] = {}, system[MAX_PATH] = {};
    if (FAILED(CoCreateGuid(&token)) || !StringFromGUID2(token, guid, ARRAYSIZE(guid)) ||
        !GetWindowsDirectoryW(windows, ARRAYSIZE(windows)) || !GetSystemDirectoryW(system, ARRAYSIZE(system))) return;
    std::wstring retired = DirOf(path) + L"\\.ZMSviewer-removed-" + guid + L".tmp";
    if (!MoveFileExW(path.c_str(), retired.c_str(), 0)) return;
    if (DeleteFileW(retired.c_str())) return;
    std::wstring powershell = std::wstring(system) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
    std::wstring command = L"\"" + powershell +
        L"\" -NoLogo -NoProfile -NonInteractive -Command \""
        L"$p=$env:ZMSVIEWER_RETIRED; for($i=0;$i -lt 90;$i++){"
        L"try{[IO.File]::Delete($p); break}catch{Start-Sleep -Seconds 1}};"
        L"try{[IO.Directory]::Delete([IO.Path]::GetDirectoryName($p),$false)}catch{}\"";
    std::wstring env = std::wstring(L"SystemRoot=") + windows;
    env.push_back(L'\0');
    env += L"ZMSVIEWER_RETIRED=" + retired;
    env.push_back(L'\0');
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(powershell.c_str(), command.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, env.data(), system, &si, &pi))
    {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

bool HashedName(const std::wstring& name, const std::wstring& prefix, const wchar_t* suffix)
{
    if (name.size() != prefix.size() + 8 + wcslen(suffix) ||
        _wcsnicmp(name.c_str(), prefix.c_str(), prefix.size()) ||
        _wcsicmp(name.c_str() + prefix.size() + 8, suffix)) return false;
    for (size_t i = prefix.size(); i < prefix.size() + 8; ++i)
        if (!iswxdigit(name[i])) return false;
    return true;
}

// Never remove an arbitrary ZMSviewer*.exe (e.g. the user's distributor),
// Setup*.exe, or *.ps1, even if someone chose a nonempty install directory.
void RemoveProductFiles(const std::wstring& dir, const std::wstring& dllBase, const std::wstring& exeBase,
                        const std::wstring& keepDll = {}, const std::wstring& keepExe = {})
{
    if (dir.empty()) return;
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return;
    do
    {
        if (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        const std::wstring name = data.cFileName, path = dir + L"\\" + name;
        const bool dll = HashedName(name, dllBase + L"-", L".dll");
        const bool exe = !exeBase.empty() && (!_wcsicmp(name.c_str(), (exeBase + L".exe").c_str()) ||
                                              HashedName(name, exeBase + L"-", L".exe"));
        if ((dll && (keepDll.empty() || !SamePath(path, keepDll))) ||
            (exe && (keepExe.empty() || !SamePath(path, keepExe)))) RemoveOwnedFile(path);
    } while (FindNextFileW(find, &data));
    FindClose(find);
    if (keepDll.empty() && keepExe.empty()) RemoveDirectoryW(dir.c_str()); // only succeeds if empty
}

void RefreshThumbnails()
{
    // Flush ownership changes first so an open Explorer view cannot refill the
    // cache with the previous handler. The second broadcast refreshes views.
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSH, nullptr, nullptr);
    HRESULT hr = thumbnails::ResetCache(KnownFolder(FOLDERID_LocalAppData).c_str());
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSH, nullptr, nullptr);
    PIDLIST_ABSOLUTE desktop = nullptr;
    if (SUCCEEDED(SHGetSpecialFolderLocation(nullptr, CSIDL_DESKTOP, &desktop)))
    {
        SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, desktop, nullptr);
        CoTaskMemFree(desktop);
    }
    if (FAILED(hr))
    {
        wchar_t code[16]; swprintf_s(code, L"0x%08X", unsigned(hr));
        throw Failure{ std::wstring(T == &kRussian ?
            L"Windows не удалось обновить кэш эскизов. Код ошибки " :
            L"Windows could not refresh the thumbnail cache. Error ") + code };
    }
}

std::wstring InstalledDir()
{
    std::wstring dir;
    if (GetString(kStateKey, L"InstallDir", dir) && !dir.empty()) return dir;
    return DefaultDir();
}
DWORD InstalledGroups()
{
    DWORD g = kAllGroups;
    GetDword(kStateKey, L"Groups", g);
    return g & kAllGroups ? g & kAllGroups : kAllGroups;
}
bool IsInstalled() { return KeyExists(kUninstallKey); }

// ---------------------------------------------------------------- the work, as a list of steps
// The wizard runs one step per timer tick, so the progress bar moves and the window stays alive.
struct Step { std::wstring label; std::function<void()> run; };

struct InstallState
{
    std::wstring dir, dll, exe, previousDir;
    const void* dllData = nullptr;
    DWORD dllSize = 0;
    wchar_t hash[16] = {};
    HANDLE mutex = nullptr;
    ~InstallState() { if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); } }
    void Begin()
    {
        HANDLE lock = CreateMutexW(nullptr, FALSE, L"Local\\ZMSviewer.Setup.1");
        if (!lock) Fail(L"ZMSviewer setup");
        DWORD result = WaitForSingleObject(lock, 0);
        if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) { mutex = lock; return; }
        CloseHandle(lock);
        throw Failure{ T == &kRussian ? L"Уже запущена другая установка ZMSviewer." : L"Another ZMSviewer setup is running." };
    }
};

std::vector<Step> InstallSteps(std::wstring dir, DWORD groups, bool removing)
{
    auto st = std::make_shared<InstallState>();
    std::vector<Step> steps;
    steps.push_back({ T->stepPrepare, [st] { st->Begin(); } });
    if (removing)
    {
        if (!IsInstalled() && !KeyExists(kStateKey)) return steps;
        for (const auto& e : kExtensions)
            steps.push_back({ Format1(T->stepUnregister, e.extension), [e] { UnregisterExtension(e); } });
        steps.push_back({ T->stepRefresh, RefreshThumbnails });
        steps.push_back({ T->stepShortcut, [] { DeleteFileW(ShortcutPath().c_str()); } });
        steps.push_back({ T->stepApps, [] {
            for (int f = 0; f < int(Format::Count); f++) DeleteTree(std::wstring(kClasses) + L"\\CLSID\\" + ClsidString(Format(f)));
            for (int f = 0; f <= int(Format::Glb); f++) DeleteTree(std::wstring(kClasses) + L"\\CLSID\\" + ClsidString(Format(f), true));
            DeleteTree(kUninstallKey);
        } });
        steps.push_back({ T->stepFiles, [] {
            std::wstring d = InstalledDir();
            DeleteTree(kStateKey);
            RemoveProductFiles(d, L"ZMSviewer", L"ZMSviewer");
        } });
        return steps;
    }

    steps.push_back({ T->stepPrepare, [st, dir] {
        HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
        HGLOBAL loaded = res ? LoadResource(nullptr, res) : nullptr;
        st->dllData = loaded ? LockResource(loaded) : nullptr;
        st->dllSize = res ? SizeofResource(nullptr, res) : 0;
        if (!st->dllData || st->dllSize < 1024) throw Failure{ L"The installer is damaged (no DLL inside). Download it again." };
        uint32_t h = 2166136261u;  // FNV-1a names the installed copy
        for (DWORD i = 0; i < st->dllSize; i++) h = (h ^ static_cast<const uint8_t*>(st->dllData)[i]) * 16777619u;
        swprintf_s(st->hash, L"%08x", h);
        st->dir = dir;
        if (IsInstalled()) st->previousDir = InstalledDir();
        SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
        if (GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) Fail(L"Cannot create the folder " + dir);
    } });

    steps.push_back({ T->stepOld, [] {
        // "Explorer Thumbnails": keep the handlers it remembered, then remove it.
        HKEY h;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kLegacyPreviousKey, 0, KEY_READ, &h) == ERROR_SUCCESS)
        {
            for (DWORD i = 0;; i++)
            {
                wchar_t name[256]; DWORD nameLen = 256;
                wchar_t data[256]; DWORD dataLen = sizeof(data), type = 0;
                if (RegEnumValueW(h, i, name, &nameLen, nullptr, &type, reinterpret_cast<BYTE*>(data), &dataLen) != ERROR_SUCCESS) break;
                std::wstring existing;
                if (type == REG_SZ && !GetString(kPreviousKey, name, existing)) SetString(kPreviousKey, name, data);
            }
            RegCloseKey(h);
        }
        bool hadLegacy = KeyExists(kLegacyUninstallKey) || KeyExists(kLegacyStateKey);
        DeleteTree(kLegacyUninstallKey);
        DeleteTree(kLegacyStateKey);
        std::wstring legacy = KnownFolder(FOLDERID_LocalAppData) + L"\\Programs\\ExplorerThumbnails";
        if (hadLegacy) RemoveProductFiles(legacy, L"ExplorerThumbnails", L"Setup");
        // "PSD превью для Проводника".
        DeleteTree(std::wstring(kClasses) + L"\\CLSID\\" + kOldClsid);
        if (KeyExists(kOldUninstallKey))
        {
            DeleteTree(kOldUninstallKey);
            RemoveProductFiles(KnownFolder(FOLDERID_LocalAppData) + L"\\Programs\\PsdThumbnailer", L"PsdThumbnailer", L"");
        }
    } });

    steps.push_back({ T->stepCopy, [st] {
        st->dll = st->dir + L"\\ZMSviewer-" + st->hash + L".dll";
        if (!FileMatches(st->dll, st->dllData, st->dllSize)) WriteFileBytes(st->dll, st->dllData, st->dllSize);
        std::wstring self = ThisExe();
        st->exe = st->dir + L"\\ZMSviewer.exe";
        if (!SamePath(self, st->exe) && !CopyFileW(self.c_str(), st->exe.c_str(), FALSE))
        {
            st->exe = st->dir + L"\\ZMSviewer-" + st->hash + L".exe";  // the old copy is running (Start menu -> Repair)
            if (!SamePath(self, st->exe) && !CopyFileW(self.c_str(), st->exe.c_str(), FALSE)) Fail(L"Cannot copy files to " + st->dir);
        }
        DeleteFileW((st->exe + L":Zone.Identifier").c_str());
        for (int f = 0; f < int(Format::Count); f++)
        {
            std::wstring key = std::wstring(kClasses) + L"\\CLSID\\" + ClsidString(Format(f));
            SetString(key, nullptr, std::wstring(kProduct) + L" - " + kFormatNames[f]);
            SetString(key + L"\\InprocServer32", nullptr, st->dll);
            SetString(key + L"\\InprocServer32", L"ThreadingModel", L"Apartment");
        }
    } });

    for (const auto& e : kExtensions)
    {
        bool on = (groups >> GroupOf(e.format)) & 1;
        steps.push_back({ Format1(on ? T->stepRegister : T->stepUnregister, e.extension), [e, on] { on ? RegisterExtension(e) : UnregisterExtension(e); } });
    }

    steps.push_back({ T->stepShortcut, [st] {
        IShellLinkW* link = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))))
        {
            link->SetPath(st->exe.c_str());
            link->SetDescription(T->tagline);
            link->SetIconLocation(st->exe.c_str(), 0);
            link->SetWorkingDirectory(st->dir.c_str());
            IPersistFile* file = nullptr;
            if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file)))) { file->Save(ShortcutPath().c_str(), TRUE); file->Release(); }
            link->Release();
        }
    } });

    steps.push_back({ T->stepApps, [st, groups] {
        DWORD sizeKb = (st->dllSize + 1023) / 1024;
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (GetFileAttributesExW(st->exe.c_str(), GetFileExInfoStandard, &fa)) sizeKb += (fa.nFileSizeLow + 1023) / 1024;
        SetString(kUninstallKey, L"DisplayName", kProduct);
        SetString(kUninstallKey, L"DisplayVersion", kVersion);
        SetString(kUninstallKey, L"Publisher", kPublisher);
        SetString(kUninstallKey, L"DisplayIcon", L"\"" + st->exe + L"\",0");
        SetString(kUninstallKey, L"InstallLocation", st->dir);
        SetString(kUninstallKey, L"UninstallString", L"\"" + st->exe + L"\" /uninstall");
        SetString(kUninstallKey, L"QuietUninstallString", L"\"" + st->exe + L"\" /uninstall /quiet");
        SetString(kUninstallKey, L"ModifyPath", L"\"" + st->exe + L"\"");
        SetDword(kUninstallKey, L"NoRepair", 1);
        SetDword(kUninstallKey, L"EstimatedSize", sizeKb);
        SetString(kStateKey, L"Dll", st->dll);
        SetString(kStateKey, L"InstallDir", st->dir);
        SetString(kStateKey, L"Version", kVersion);
        SetDword(kStateKey, L"Groups", groups);
        for (int f = 0; f <= int(Format::Glb); f++) DeleteTree(std::wstring(kClasses) + L"\\CLSID\\" + ClsidString(Format(f), true));
        RemoveProductFiles(st->dir, L"ZMSviewer", L"ZMSviewer", st->dll, st->exe);
        if (!st->previousDir.empty() && !SamePath(st->previousDir, st->dir))
            RemoveProductFiles(st->previousDir, L"ZMSviewer", L"ZMSviewer");
    } });

    steps.push_back({ T->stepRefresh, RefreshThumbnails });
    return steps;
}

// ---------------------------------------------------------------- the wizard window
enum Page { kWelcome, kTypes, kFolder, kMaintain, kConfirmRemove, kProgress, kFinish };
enum Mode { kFresh, kModify, kRepair, kRemove };
enum Id { kIdBack = 100, kIdBrowse, kIdEdit, kIdCheck0, kIdCheck1, kIdCheck2, kIdRadio0, kIdRadio1, kIdRadio2 };

struct Wizard
{
    HWND hwnd = nullptr;
    UINT dpi = 96;
    HFONT font = nullptr, bold = nullptr, title = nullptr, panelTitle = nullptr, smallFont = nullptr;
    HBITMAP logo = nullptr;
    int logoSize = 0;
    HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
    HWND heading, body, back, next, cancel, browse, edit, progress, status;
    HWND checks[3], checkNotes[3], radios[3], radioNotes[3];
    Page page = kWelcome;
    Mode mode = kFresh;
    bool installed = false, startRemove = false, ok = false, running = false;
    std::wstring error;
    std::vector<Step> steps;
    size_t stepIndex = 0;

    int S(int v) const { return MulDiv(v, int(dpi), 96); }
};
Wizard g;

HFONT MakeFont(int points, int weight)
{
    LOGFONTW lf = {};
    lf.lfHeight = -MulDiv(points, int(g.dpi), 72);
    lf.lfWeight = weight;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    return CreateFontIndirectW(&lf);
}

// The logo (PNG in the resources), scaled with WIC to the size we draw it at.
HBITMAP LoadLogo(int size)
{
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(2), MAKEINTRESOURCEW(10));
    HGLOBAL loaded = res ? LoadResource(nullptr, res) : nullptr;
    if (!loaded) return nullptr;
    IStream* stream = SHCreateMemStream(static_cast<const BYTE*>(LockResource(loaded)), SizeofResource(nullptr, res));
    IWICImagingFactory* wic = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapScaler* scaler = nullptr;
    IWICFormatConverter* converter = nullptr;
    HBITMAP bitmap = nullptr;
    if (stream && SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))
        && SUCCEEDED(wic->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder))
        && SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(wic->CreateBitmapScaler(&scaler))
        && SUCCEEDED(scaler->Initialize(frame, size, size, WICBitmapInterpolationModeFant))
        && SUCCEEDED(wic->CreateFormatConverter(&converter))
        && SUCCEEDED(converter->Initialize(scaler, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
    {
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = size;
        bmi.bmiHeader.biHeight = -size;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        bitmap = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bitmap && FAILED(converter->CopyPixels(nullptr, size * 4, size * size * 4, static_cast<BYTE*>(bits)))) { DeleteObject(bitmap); bitmap = nullptr; }
    }
    if (converter) converter->Release();
    if (scaler) scaler->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (wic) wic->Release();
    if (stream) stream->Release();
    return bitmap;
}

const int kWidth = 720, kHeight = 500, kPanel = 230, kBar = 58;

HWND Child(const wchar_t* cls, const wchar_t* text, DWORD style, int id, HFONT font)
{
    HWND h = CreateWindowExW(0, cls, text, WS_CHILD | style, 0, 0, 10, 10, g.hwnd, reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(h, WM_SETFONT, WPARAM(font), TRUE);
    return h;
}

void Place(HWND h, int x, int y, int w, int hh) { SetWindowPos(h, nullptr, g.S(x), g.S(y), g.S(w), g.S(hh), SWP_NOZORDER | SWP_NOACTIVATE); }
void Show(HWND h, bool on) { ShowWindow(h, on ? SW_SHOW : SW_HIDE); }

DWORD CheckedGroups()
{
    DWORD m = 0;
    for (int i = 0; i < 3; i++) if (SendMessageW(g.checks[i], BM_GETCHECK, 0, 0) == BST_CHECKED) m |= 1u << i;
    return m;
}

std::wstring GroupExtensions(int group)
{
    std::wstring s;
    for (const auto& e : kExtensions) if (GroupOf(e.format) == group) s += (s.empty() ? L"" : L"  ") + std::wstring(e.extension);
    return s;
}

void ShowPage(Page p)
{
    g.page = p;
    const int x = kPanel + 30, w = kWidth - x - 30;
    const wchar_t* head = L"";
    std::wstring text;
    switch (p)
    {
    case kWelcome: head = T->welcomeTitle; text = T->welcomeBody; break;
    case kTypes: head = T->typesTitle; text = T->typesBody; break;
    case kFolder: head = T->folderTitle; text = T->folderBody; break;
    case kMaintain: head = T->maintainTitle; text = T->maintainBody; break;
    case kConfirmRemove: head = T->removeTitle; text = T->removeBody; break;
    case kProgress: head = g.mode == kRemove ? T->removingTitle : T->installingTitle; break;
    case kFinish:
        if (!g.ok) { head = T->failedTitle; text = g.error; }
        else if (g.mode == kRemove) { head = T->removedTitle; text = T->removedBody; }
        else if (g.mode == kModify) { head = T->doneTitle; text = T->changedBody; }
        else { head = T->doneTitle; text = T->doneBody; }
        break;
    }
    SetWindowTextW(g.heading, head);
    SetWindowTextW(g.body, text.c_str());
    Place(g.heading, x, 28, w, 34);
    Place(g.body, x, 72, w, p == kWelcome || p == kFinish || p == kConfirmRemove ? 300 : 44);

    for (int i = 0; i < 3; i++)
    {
        Show(g.checks[i], p == kTypes); Show(g.checkNotes[i], p == kTypes);
        Place(g.checks[i], x, 132 + i * 74, w, 24); Place(g.checkNotes[i], x + 22, 158 + i * 74, w - 22, 36);
        Show(g.radios[i], p == kMaintain); Show(g.radioNotes[i], p == kMaintain);
        Place(g.radios[i], x, 132 + i * 74, w, 24); Place(g.radioNotes[i], x + 22, 158 + i * 74, w - 22, 36);
    }
    Show(g.edit, p == kFolder); Show(g.browse, p == kFolder);
    Place(g.edit, x, 124, w - 100, 26); Place(g.browse, x + w - 90, 123, 90, 28);
    Show(g.status, p == kFolder || p == kProgress);
    if (p == kFolder) { SetWindowTextW(g.status, T->folderNote); Place(g.status, x, 168, w, 40); }
    Show(g.progress, p == kProgress);
    Place(g.progress, x, 150, w, 20); if (p == kProgress) Place(g.status, x, 120, w, 24);

    const wchar_t* nextText = T->next;
    if (p == kFolder) nextText = T->install;
    if (p == kTypes && g.mode == kModify) nextText = T->apply;
    if (p == kConfirmRemove) nextText = T->removeButton;
    if (p == kFinish) nextText = g.ok ? T->finish : T->close;
    SetWindowTextW(g.next, nextText);
    Show(g.back, p == kTypes || p == kFolder || (p == kConfirmRemove && !g.startRemove));
    EnableWindow(g.next, p != kProgress);
    EnableWindow(g.cancel, p != kProgress && p != kFinish);
    Show(g.cancel, p != kFinish);
    InvalidateRect(g.hwnd, nullptr, TRUE);
    SetFocus(g.next);
}

void StartWork()
{
    DWORD groups = g.mode == kRepair ? InstalledGroups() : CheckedGroups();
    std::wstring dir = g.mode == kFresh ? std::wstring() : InstalledDir();
    if (g.mode == kFresh)
    {
        wchar_t buffer[MAX_PATH * 2] = {};
        GetWindowTextW(g.edit, buffer, int(std::size(buffer)));
        dir = buffer;
        while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L' ')) dir.pop_back();
        // Our files always go in a folder of their own.
        if (dir.size() < 9 || _wcsicmp(dir.c_str() + dir.size() - 9, L"ZMSviewer") != 0) dir += L"\\ZMSviewer";
    }
    g.steps = InstallSteps(dir, groups, g.mode == kRemove);
    g.stepIndex = 0;
    g.ok = true;
    g.error.clear();
    SendMessageW(g.progress, PBM_SETRANGE32, 0, LPARAM(g.steps.size()));
    SendMessageW(g.progress, PBM_SETPOS, 0, 0);
    ShowPage(kProgress);
    g.running = true;
    SetTimer(g.hwnd, 1, 70, nullptr);
}

void Tick()
{
    // Shell notifications and COM calls can pump messages. Never run the next
    // installer step (or show success) while the current one is still active.
    KillTimer(g.hwnd, 1);
    if (g.stepIndex >= g.steps.size())
    {
        KillTimer(g.hwnd, 1);
        g.steps.clear();
        g.running = false;
        SetWindowTextW(g.status, T->stepDone);
        ShowPage(kFinish);
        return;
    }
    Step& s = g.steps[g.stepIndex++];
    SetWindowTextW(g.status, s.label.c_str());
    try { s.run(); }
    catch (const Failure& f)
    {
        KillTimer(g.hwnd, 1);
        g.running = false;
        g.ok = false;
        g.error = f.what;
        g.steps.clear();
        ShowPage(kFinish);
        return;
    }
    SendMessageW(g.progress, PBM_SETPOS, WPARAM(g.stepIndex), 0);
    SetTimer(g.hwnd, 1, 70, nullptr);
}

void Browse()
{
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return;
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    if (SUCCEEDED(dialog->Show(g.hwnd)))
    {
        IShellItem* item = nullptr;
        PWSTR path = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        {
            std::wstring dir = path;
            if (dir.size() < 9 || _wcsicmp(dir.c_str() + dir.size() - 9, L"ZMSviewer") != 0) dir += (dir.back() == L'\\' ? L"" : L"\\") + std::wstring(L"ZMSviewer");
            SetWindowTextW(g.edit, dir.c_str());
            CoTaskMemFree(path);
        }
        if (item) item->Release();
    }
    dialog->Release();
}

void OnNext()
{
    switch (g.page)
    {
    case kWelcome: ShowPage(kTypes); break;
    case kTypes:
        if (!CheckedGroups()) { MessageBoxW(g.hwnd, T->pickOne, T->setupTitle, MB_ICONINFORMATION); break; }
        if (g.mode == kModify) StartWork(); else ShowPage(kFolder);
        break;
    case kFolder: StartWork(); break;
    case kMaintain:
    {
        int choice = 0;
        for (int i = 0; i < 3; i++) if (SendMessageW(g.radios[i], BM_GETCHECK, 0, 0) == BST_CHECKED) choice = i;
        if (choice == 0)
        {
            g.mode = kModify;
            DWORD groups = InstalledGroups();
            for (int i = 0; i < 3; i++) SendMessageW(g.checks[i], BM_SETCHECK, (groups >> i) & 1 ? BST_CHECKED : BST_UNCHECKED, 0);
            ShowPage(kTypes);
        }
        else if (choice == 1) { g.mode = kRepair; StartWork(); }
        else { g.mode = kRemove; ShowPage(kConfirmRemove); }
        break;
    }
    case kConfirmRemove: g.mode = kRemove; StartWork(); break;
    case kFinish: DestroyWindow(g.hwnd); break;
    default: break;
    }
}

void OnBack()
{
    if (g.page == kFolder) ShowPage(kTypes);
    else if (g.page == kTypes) { if (g.installed) { g.mode = kFresh; ShowPage(kMaintain); } else ShowPage(kWelcome); }
    else if (g.page == kConfirmRemove) ShowPage(kMaintain);
}

void Paint(HDC dc)
{
    RECT rc; GetClientRect(g.hwnd, &rc);
    // Left panel: the logo on a dark blue-teal gradient.
    TRIVERTEX v[2] = { { 0, 0, 0x1000, 0x1f00, 0x3c00, 0 }, { LONG(g.S(kPanel)), rc.bottom, 0x0c00, 0x4a00, 0x5a00, 0 } };
    GRADIENT_RECT gr = { 0, 1 };
    GradientFill(dc, v, 2, &gr, 1, GRADIENT_FILL_RECT_V);
    if (g.logo)
    {
        HDC mem = CreateCompatibleDC(dc);
        HGDIOBJ old = SelectObject(mem, g.logo);
        BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        AlphaBlend(dc, (g.S(kPanel) - g.logoSize) / 2, g.S(48), g.logoSize, g.logoSize, mem, 0, 0, g.logoSize, g.logoSize, bf);
        SelectObject(mem, old);
        DeleteDC(mem);
    }
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(240, 246, 250));
    HGDIOBJ oldFont = SelectObject(dc, g.panelTitle);
    RECT t = { 0, g.S(48) + g.logoSize + g.S(14), g.S(kPanel), g.S(48) + g.logoSize + g.S(50) };
    DrawTextW(dc, kProduct, -1, &t, DT_CENTER | DT_SINGLELINE);
    SelectObject(dc, g.smallFont);
    SetTextColor(dc, RGB(160, 200, 210));
    wchar_t version[64]; swprintf_s(version, T->versionFmt, kVersion);
    t.top = t.bottom; t.bottom = t.top + g.S(22);
    DrawTextW(dc, version, -1, &t, DT_CENTER | DT_SINGLELINE);
    RECT tag = { 0, rc.bottom - g.S(50), g.S(kPanel), rc.bottom - g.S(20) };
    DrawTextW(dc, T->tagline, -1, &tag, DT_CENTER | DT_SINGLELINE);
    SelectObject(dc, oldFont);
    // Right side and the button bar.
    RECT right = { g.S(kPanel), 0, rc.right, rc.bottom - g.S(kBar) };
    FillRect(dc, &right, g.white);
    RECT bar = { g.S(kPanel), rc.bottom - g.S(kBar), rc.right, rc.bottom };
    HBRUSH barBrush = CreateSolidBrush(RGB(243, 244, 246));
    FillRect(dc, &bar, barBrush);
    DeleteObject(barBrush);
    RECT line = { g.S(kPanel), bar.top, rc.right, bar.top + 1 };
    HBRUSH lineBrush = CreateSolidBrush(RGB(222, 224, 228));
    FillRect(dc, &line, lineBrush);
    DeleteObject(lineBrush);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        Paint(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    {
        HDC dc = HDC(wp);
        HWND ctl = HWND(lp);
        RECT r; GetWindowRect(ctl, &r);
        POINT pt = { r.left, r.top }; ScreenToClient(hwnd, &pt);
        RECT rc; GetClientRect(hwnd, &rc);
        bool inBar = pt.y >= rc.bottom - g.S(kBar);
        SetBkColor(dc, inBar ? RGB(243, 244, 246) : RGB(255, 255, 255));
        SetTextColor(dc, ctl == g.heading ? RGB(18, 42, 78) : (ctl == g.checkNotes[0] || ctl == g.checkNotes[1] || ctl == g.checkNotes[2]
                                                               || ctl == g.radioNotes[0] || ctl == g.radioNotes[1] || ctl == g.radioNotes[2])
                                                                  ? RGB(110, 116, 128) : RGB(30, 30, 30));
        static HBRUSH barBrush = CreateSolidBrush(RGB(243, 244, 246));
        return LRESULT(inBar ? barBrush : g.white);
    }
    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case IDOK: OnNext(); break;
        case kIdBack: OnBack(); break;
        case kIdBrowse: Browse(); break;
        case IDCANCEL:
            if (g.running) break;
            if (g.page == kFinish || MessageBoxW(hwnd, T->confirmExit, T->setupTitle, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES)
                DestroyWindow(hwnd);
            break;
        }
        return 0;
    case WM_TIMER: Tick(); return 0;
    case WM_CLOSE: SendMessageW(hwnd, WM_COMMAND, IDCANCEL, 0); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int RunWizard(bool startRemove)
{
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = HICON(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    wc.lpszClassName = L"ZMSviewerSetup";
    RegisterClassExW(&wc);

    HDC screen = GetDC(nullptr);
    g.dpi = UINT(GetDeviceCaps(screen, LOGPIXELSX));
    ReleaseDC(nullptr, screen);
    g.font = MakeFont(10, FW_NORMAL);
    g.bold = MakeFont(10, FW_SEMIBOLD);
    g.title = MakeFont(15, FW_SEMIBOLD);
    g.panelTitle = MakeFont(16, FW_BOLD);
    g.smallFont = MakeFont(9, FW_NORMAL);
    g.logoSize = g.S(170);
    g.logo = LoadLogo(g.logoSize);
    g.installed = IsInstalled();
    g.startRemove = startRemove;

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT r = { 0, 0, g.S(kWidth), g.S(kHeight) };
    AdjustWindowRectEx(&r, style, FALSE, 0);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    RECT work; SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    g.hwnd = CreateWindowExW(0, wc.lpszClassName, T->setupTitle, style, work.left + (work.right - work.left - ww) / 2,
                             work.top + (work.bottom - work.top - wh) / 2, ww, wh, nullptr, nullptr, inst, nullptr);

    g.heading = Child(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, -1, g.title);
    g.body = Child(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, -1, g.font);
    for (int i = 0; i < 3; i++)
    {
        g.checks[i] = Child(L"BUTTON", T->groups[i], BS_AUTOCHECKBOX | WS_TABSTOP, kIdCheck0 + i, g.bold);
        SendMessageW(g.checks[i], BM_SETCHECK, BST_CHECKED, 0);
        g.checkNotes[i] = Child(L"STATIC", GroupExtensions(i).c_str(), SS_LEFT | SS_NOPREFIX, -1, g.font);
    }
    std::wstring priorVersion;
    bool upgrading = g.installed && GetString(kStateKey, L"Version", priorVersion) && priorVersion == L"1.0.0";
    const wchar_t* update = T == &kRussian ? L"Обновить до версии 1.1" : L"Update to version 1.1";
    const wchar_t* updateNote = T == &kRussian ? L"Сохранить выбранные типы файлов и обновить кэш эскизов" :
                                                L"Keep your selected file types and refresh the thumbnail cache";
    const wchar_t* radioText[3] = { T->modify, upgrading ? update : T->repair, T->remove };
    const wchar_t* radioNote[3] = { T->modifyNote, upgrading ? updateNote : T->repairNote, T->removeNote };
    for (int i = 0; i < 3; i++)
    {
        g.radios[i] = Child(L"BUTTON", radioText[i], BS_AUTORADIOBUTTON | WS_TABSTOP | (i == 0 ? WS_GROUP : 0), kIdRadio0 + i, g.bold);
        g.radioNotes[i] = Child(L"STATIC", radioNote[i], SS_LEFT | SS_NOPREFIX, -1, g.font);
    }
    SendMessageW(g.radios[upgrading ? 1 : 0], BM_SETCHECK, BST_CHECKED, 0);
    g.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", DefaultDir().c_str(), WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 10, 10, g.hwnd,
                             reinterpret_cast<HMENU>(INT_PTR(kIdEdit)), inst, nullptr);
    SendMessageW(g.edit, WM_SETFONT, WPARAM(g.font), TRUE);
    g.browse = Child(L"BUTTON", T->browse, BS_PUSHBUTTON | WS_TABSTOP, kIdBrowse, g.font);
    g.status = Child(L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, -1, g.font);
    g.progress = Child(PROGRESS_CLASSW, L"", PBS_SMOOTH, -1, g.font);
    g.back = Child(L"BUTTON", T->back, BS_PUSHBUTTON | WS_TABSTOP | WS_VISIBLE, kIdBack, g.font);
    g.next = Child(L"BUTTON", T->next, BS_DEFPUSHBUTTON | WS_TABSTOP | WS_VISIBLE, IDOK, g.font);
    g.cancel = Child(L"BUTTON", T->cancel, BS_PUSHBUTTON | WS_TABSTOP | WS_VISIBLE, IDCANCEL, g.font);
    Show(g.heading, true); Show(g.body, true);
    int by = kHeight - kBar + 15;
    Place(g.back, kWidth - 330, by, 96, 30);
    Place(g.next, kWidth - 226, by, 96, 30);
    Place(g.cancel, kWidth - 116, by, 96, 30);

    if (startRemove) { g.mode = kRemove; ShowPage(kConfirmRemove); }
    else if (g.installed) ShowPage(kMaintain);
    else ShowPage(kWelcome);
    ShowWindow(g.hwnd, SW_SHOW);
    SetForegroundWindow(g.hwnd);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0)
    {
        if (IsDialogMessageW(g.hwnd, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return g.ok ? 0 : 1;
}

int RunQuiet(bool remove, DWORD groups, std::wstring dir)
{
    try
    {
        if (dir.empty()) dir = IsInstalled() ? InstalledDir() : DefaultDir();
        for (Step& s : InstallSteps(dir, groups, remove)) s.run();
        return 0;
    }
    catch (const Failure&) { return 2; }
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    LANGID lang = PRIMARYLANGID(GetUserDefaultUILanguage());
    if (lang == LANG_RUSSIAN || lang == LANG_UKRAINIAN || lang == LANG_BELARUSIAN || lang == LANG_KAZAK) T = &kRussian;

    bool install = false, uninstall = false, quiet = false;
    DWORD groups = 0;
    std::wstring dir;
    int argc = 0;
    PWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; i++)
    {
        std::wstring a = argv[i];
        if (!a.empty() && (a[0] == L'/' || a[0] == L'-')) a.erase(0, 1);
        std::wstring lower = a;
        for (auto& ch : lower) ch = wchar_t(towlower(ch));
        if (lower == L"install") install = true;
        else if (lower == L"uninstall") uninstall = true;
        else if (lower == L"quiet" || lower == L"q" || lower == L"silent") quiet = true;
        else if (lower == L"en") T = &kEnglish;
        else if (lower == L"ru") T = &kRussian;
        else if (lower.rfind(L"dir=", 0) == 0) dir = a.substr(4);
        else if (lower.rfind(L"types=", 0) == 0)
        {
            if (lower.find(L"pictures") != std::wstring::npos) groups |= 1u << kPictures;
            if (lower.find(L"books") != std::wstring::npos) groups |= 1u << kBooks;
            if (lower.find(L"models") != std::wstring::npos) groups |= 1u << kModels;
        }
    }
    if (argv) LocalFree(argv);

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 3;
    int result;
    if (quiet && (install || uninstall)) result = RunQuiet(uninstall, groups ? groups : (IsInstalled() ? InstalledGroups() : kAllGroups), dir);
    else result = RunWizard(uninstall);
    CoUninitialize();
    return result;
}
