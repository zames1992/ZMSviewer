// FictionBook and EPUB covers, Kindle image records and the first page of a ZIP comic.
#include "Common.h"
#include <algorithm>
#include <cctype>
#include <shlwapi.h>
#include <xmllite.h>
#include <wrl/client.h>
#include <array>
#include <string_view>

#pragma comment(lib, "xmllite.lib")
#pragma comment(lib, "user32.lib")

using Microsoft::WRL::ComPtr;

namespace {

// Case-insensitive search for an ASCII needle.
size_t FindNoCase(const std::string& hay, const std::string& needle, size_t from = 0)
{
    auto it = std::search(hay.begin() + std::min(from, hay.size()), hay.end(), needle.begin(), needle.end(),
        [](char a, char b) { return std::tolower(uint8_t(a)) == std::tolower(uint8_t(b)); });
    return it == hay.end() ? std::string::npos : size_t(it - hay.begin());
}

// Value of attribute `name` (any namespace prefix, e.g. l:href / xlink:href) inside one tag.
std::string Attribute(const std::string& tag, const std::string& name)
{
    size_t pos = 0;
    while ((pos = FindNoCase(tag, name, pos)) != std::string::npos)
    {
        bool startOk = pos == 0 || tag[pos - 1] == ':' || std::isspace(uint8_t(tag[pos - 1]));
        size_t p = pos + name.size();
        while (p < tag.size() && std::isspace(uint8_t(tag[p]))) p++;
        if (startOk && p < tag.size() && tag[p] == '=')
        {
            p++;
            while (p < tag.size() && std::isspace(uint8_t(tag[p]))) p++;
            if (p < tag.size() && (tag[p] == '"' || tag[p] == '\''))
            {
                char q = tag[p];
                size_t end = tag.find(q, p + 1);
                if (end != std::string::npos) return tag.substr(p + 1, end - p - 1);
            }
        }
        pos += name.size();
    }
    return {};
}

// The tag (from '<' to '>') starting at pos.
std::string TagAt(const std::string& text, size_t pos)
{
    size_t end = text.find('>', pos);
    return end == std::string::npos ? std::string() : text.substr(pos, end - pos + 1);
}

bool DecodeBase64(std::wstring_view text, std::vector<uint8_t>& bytes)
{
    static const auto table = [] {
        std::array<int8_t, 256> values;
        values.fill(-1);
        const char* abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) values[uint8_t(abc[i])] = int8_t(i);
        return values;
    }();
    uint32_t acc = 0;
    int bits = 0;
    for (wchar_t c : text)
    {
        if (c == L'=') break;
        if (c == L' ' || c == L'\n' || c == L'\r' || c == L'\t') continue;
        if (c >= 256 || table[c] < 0) return false;
        acc = (acc << 6) | uint32_t(table[c]);
        bits += 6;
        if (bits >= 8) { bits -= 8; bytes.push_back(uint8_t(acc >> bits)); }
    }
    return !bytes.empty();
}

bool DecodeCover(const std::vector<uint8_t>& bytes, UINT maxSize, Image& out)
{
    if (!DecodeWithWic(bytes.data(), bytes.size(), out, 48ull << 20)) return false;
    FitImage(out, maxSize);
    return true;
}

bool TitleThumbnail(std::wstring title, UINT maxSize, Image& out)
{
    const size_t first = title.find_first_not_of(L" \r\n\t");
    if (first == std::wstring::npos) return false;
    title = title.substr(first, 512);
    const int height = std::max(1u, maxSize), width = std::max(1, height * 3 / 4);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -height;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    out.bgra.resize(size_t(width) * height * 4);
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dc || !bitmap) { if (dc) DeleteDC(dc); if (bitmap) DeleteObject(bitmap); return false; }
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    RECT rect{0, 0, width, height};
    SetDCBrushColor(dc, RGB(248, 245, 232));
    FillRect(dc, &rect, (HBRUSH)GetStockObject(DC_BRUSH));
    RECT binding{0, 0, std::max(1, width / 18), height};
    SetDCBrushColor(dc, RGB(73, 96, 103));
    FillRect(dc, &binding, (HBRUSH)GetStockObject(DC_BRUSH));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(35, 46, 48));
    int fontSize = std::max(1, height / (title.size() > 45 ? 13 : 10));
    HFONT font = CreateFontW(-fontSize, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Georgia");
    HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
    RECT caption{width / 8, height / 5, width - width / 11, height - height / 12};
    DrawTextW(dc, title.c_str(), int(title.size()), &caption, DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL | DT_END_ELLIPSIS);
    GdiFlush();
    memcpy(out.bgra.data(), bits, out.bgra.size());
    if (font) { SelectObject(dc, oldFont); DeleteObject(font); }
    SelectObject(dc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
    out.width = width;
    out.height = height;
    out.alpha = false;
    return true;
}

bool OpenXml(const std::vector<uint8_t>& bytes, ComPtr<IXmlReader>& xml)
{
    if (bytes.empty() || bytes.size() > UINT_MAX) return false;
    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(bytes.data(), UINT(bytes.size())));
    return stream && SUCCEEDED(CreateXmlReader(IID_PPV_ARGS(xml.GetAddressOf()), nullptr))
        && SUCCEEDED(xml->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit))
        && SUCCEEDED(xml->SetInput(stream.Get()));
}

std::wstring XmlAttribute(IXmlReader* xml, const wchar_t* name)
{
    std::wstring value;
    if (xml->MoveToFirstAttribute() != S_OK) return value;
    do
    {
        const wchar_t* local = nullptr;
        if (xml->GetLocalName(&local, nullptr) != S_OK || wcscmp(local, name) != 0) continue;
        const wchar_t* text = nullptr;
        UINT length = 0;
        if (SUCCEEDED(xml->GetValue(&text, &length))) value.assign(text, length);
        break;
    } while (xml->MoveToNextAttribute() == S_OK);
    xml->MoveToElement();
    return value;
}

bool Fb2Cover(const std::vector<uint8_t>& bytes, UINT maxSize, Image& out)
{
    ComPtr<IXmlReader> xml;
    if (!OpenXml(bytes, xml)) return false;
    std::wstring coverId, binaryText, title;
    bool fictionBook = false, inCover = false, inTitle = false, inBinary = false, preferred = false;
    Image fallback;
    XmlNodeType type;
    while (xml->Read(&type) == S_OK)
    {
        if (type == XmlNodeType_Element || type == XmlNodeType_EndElement)
        {
            const wchar_t* name = nullptr;
            if (FAILED(xml->GetLocalName(&name, nullptr))) return false;
            bool start = type == XmlNodeType_Element && !xml->IsEmptyElement();
            if (wcscmp(name, L"FictionBook") == 0) fictionBook = true;
            if (!fictionBook) return false;
            if (wcscmp(name, L"coverpage") == 0) inCover = start;
            else if (inCover && type == XmlNodeType_Element && wcscmp(name, L"image") == 0)
            {
                auto href = XmlAttribute(xml.Get(), L"href");
                if (href.size() > 1 && href[0] == L'#') coverId = href.substr(1);
            }
            else if (wcscmp(name, L"book-title") == 0) inTitle = start && title.empty();
            else if (wcscmp(name, L"binary") == 0)
            {
                if (inBinary && type == XmlNodeType_EndElement)
                {
                    std::vector<uint8_t> data;
                    Image decoded;
                    if (DecodeBase64(binaryText, data) && DecodeCover(data, maxSize, decoded))
                    {
                        if (preferred || coverId.empty()) { out = std::move(decoded); return true; }
                        fallback = std::move(decoded);
                    }
                }
                binaryText.clear();
                preferred = start && !coverId.empty() && XmlAttribute(xml.Get(), L"id") == coverId;
                inBinary = start && (preferred || !fallback.width);
            }
        }
        else if ((type == XmlNodeType_Text || type == XmlNodeType_CDATA || type == XmlNodeType_Whitespace)
            && (inBinary || inTitle))
        {
            const wchar_t* text = nullptr;
            UINT count = 0;
            if (FAILED(xml->GetValue(&text, &count))) return false;
            if (inTitle && title.size() < 512) title.append(text, std::min<size_t>(count, 512 - title.size()));
            if (inBinary && binaryText.size() + count <= (64u << 20)) binaryText.append(text, count);
            else inBinary = false;
        }
    }
    if (fallback.width) { out = std::move(fallback); return true; }
    return fictionBook && TitleThumbnail(title, maxSize, out);
}

// ZIP containers used by EPUB, CBZ and compressed FictionBook.

struct ZipEntry
{
    std::string name;
    uint32_t method = 0;
    uint64_t compressed = 0, size = 0, localOffset = 0;
};

bool ReadZip(const std::vector<uint8_t>& z, std::vector<ZipEntry>& entries)
{
    if (z.size() < 22) return false;
    size_t eocd = z.size() - 22;
    while (Le32(z.data() + eocd) != 0x06054b50 || Le16(z.data() + eocd + 20) != z.size() - eocd - 22)
    {
        if (!eocd || z.size() - eocd >= 65557) return false;
        --eocd;
    }
    uint64_t count = Le16(z.data() + eocd + 10), offset = Le32(z.data() + eocd + 16);
    if (count == 0xffff || offset == 0xffffffff)
    {
        if (eocd < 20 || Le32(z.data() + eocd - 20) != 0x07064b50) return false;
        uint64_t rec = Le64(z.data() + eocd - 12);
        if (z.size() < 56 || rec > z.size() - 56 || Le32(z.data() + rec) != 0x06064b50) return false;
        count = Le64(z.data() + rec + 32);
        offset = Le64(z.data() + rec + 48);
    }
    if (offset > eocd || count > 100000 || count > (eocd - offset) / 46) return false;
    size_t p = size_t(offset);
    for (size_t i = 0; i < count; ++i)
    {
        if (eocd - p < 46 || Le32(z.data() + p) != 0x02014b50) return false;
        ZipEntry e;
        e.method = Le16(z.data() + p + 10);
        e.compressed = Le32(z.data() + p + 20);
        e.size = Le32(z.data() + p + 24);
        uint16_t nameLen = Le16(z.data() + p + 28), extraLen = Le16(z.data() + p + 30), commentLen = Le16(z.data() + p + 32);
        e.localOffset = Le32(z.data() + p + 42);
        if (46ull + nameLen + extraLen + commentLen > eocd - p) return false;
        e.name.assign(reinterpret_cast<const char*>(z.data() + p + 46), nameLen);
        size_t x = p + 46 + nameLen, xEnd = x + extraLen;
        while (x + 4 <= xEnd)
        {
            uint16_t id = Le16(z.data() + x), len = Le16(z.data() + x + 2);
            x += 4;
            if (len > xEnd - x) return false;
            if (id == 1)
            {
                size_t q = x;
                for (uint64_t* field : {&e.size, &e.compressed, &e.localOffset})
                {
                    if (*field != 0xffffffff) continue;
                    if (x + len - q < 8) return false;
                    *field = Le64(z.data() + q);
                    q += 8;
                }
            }
            x += len;
        }
        if (!(Le16(z.data() + p + 8) & 1)) entries.push_back(std::move(e));
        p += 46 + nameLen + extraLen + commentLen;
    }
    return !entries.empty();
}

bool Extract(const std::vector<uint8_t>& z, const ZipEntry& e, std::vector<uint8_t>& out)
{
    if (z.size() < 30 || e.localOffset > z.size() - 30 || !e.size || e.size > (128u << 20)) return false;
    size_t p = size_t(e.localOffset);
    if (Le32(z.data() + p) != 0x04034b50) return false;
    size_t data = p + 30 + Le16(z.data() + p + 26) + Le16(z.data() + p + 28);
    if (data > z.size() || e.compressed > z.size() - data) return false;
    if (e.method == 0 && e.compressed == e.size) { out.assign(z.begin() + data, z.begin() + data + size_t(e.size)); return true; }
    return e.method == 8 && Inflate(z.data() + data, size_t(e.compressed), false, out, size_t(e.size));
}

std::string UrlDecode(const std::string& s)
{
    std::string r;
    for (size_t i = 0; i < s.size(); i++)
    {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(uint8_t(s[i + 1])) && std::isxdigit(uint8_t(s[i + 2])))
        {
            r += char(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        }
        else r += s[i];
    }
    return r;
}

// Joins an href to the directory of the file it came from, resolving ./ and ../.
std::string ResolvePath(const std::string& baseFile, std::string href)
{
    size_t hash = href.find('#');
    if (hash != std::string::npos) href.resize(hash);
    href = UrlDecode(href);
    std::string full = href.size() && href[0] == '/' ? href.substr(1) : baseFile.substr(0, baseFile.rfind('/') + 1) + href;
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= full.size())
    {
        size_t slash = full.find('/', start);
        std::string part = full.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (part == "..") { if (!parts.empty()) parts.pop_back(); }
        else if (!part.empty() && part != ".") parts.push_back(part);
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    std::string r;
    for (auto& part : parts) { if (!r.empty()) r += '/'; r += part; }
    return r;
}

const ZipEntry* FindEntry(const std::vector<ZipEntry>& entries, const std::string& name)
{
    for (auto& e : entries) if (e.name == name) return &e;
    for (auto& e : entries)
        if (e.name.size() == name.size() && FindNoCase(e.name, name) == 0) return &e;
    return nullptr;
}

bool IsImageName(const std::string& name)
{
    std::string lower = name;
    for (auto& c : lower) c = char(std::tolower(uint8_t(c)));
    for (const char* ext : { ".jpg", ".jpeg", ".png", ".gif", ".bmp", ".webp" })
        if (lower.size() > strlen(ext) && lower.compare(lower.size() - strlen(ext), std::string::npos, ext) == 0) return true;
    return false;
}

std::string Utf8(const std::wstring& s)
{
    if (s.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring ToWide(const char* s, size_t length, UINT codePage)
{
    if (length == 0 || length > INT_MAX) return {};
    int count = MultiByteToWideChar(codePage, 0, s, int(length), nullptr, 0);
    std::wstring result(count, 0);
    MultiByteToWideChar(codePage, 0, s, int(length), result.data(), count);
    return result;
}

bool HasSuffix(const std::string& name, const char* suffix)
{
    size_t n = strlen(suffix);
    return name.size() >= n && FindNoCase(name, suffix, name.size() - n) == name.size() - n;
}

std::string ContainerPackage(const std::vector<uint8_t>& bytes)
{
    ComPtr<IXmlReader> xml;
    if (!OpenXml(bytes, xml)) return {};
    XmlNodeType type;
    while (xml->Read(&type) == S_OK)
    {
        const wchar_t* name = nullptr;
        if (type == XmlNodeType_Element && SUCCEEDED(xml->GetLocalName(&name, nullptr)) && !wcscmp(name, L"rootfile"))
            return Utf8(XmlAttribute(xml.Get(), L"full-path"));
    }
    return {};
}

struct ManifestItem { std::string id, href, type, props; };
struct Package
{
    std::wstring title;
    std::string coverId, guideCover, firstSpine;
    std::vector<ManifestItem> items;

    const ManifestItem* Find(const std::string& id) const
    {
        if (!id.empty()) for (auto& item : items) if (item.id == id) return &item;
        return nullptr;
    }

    void Read(const std::vector<uint8_t>& bytes)
    {
        ComPtr<IXmlReader> xml;
        if (!OpenXml(bytes, xml)) return;
        XmlNodeType type;
        bool inTitle = false;
        auto attr = [&](const wchar_t* key) { return Utf8(XmlAttribute(xml.Get(), key)); };
        while (xml->Read(&type) == S_OK && items.size() < 100000)
        {
            const wchar_t* name = nullptr;
            if (type == XmlNodeType_Element && SUCCEEDED(xml->GetLocalName(&name, nullptr)))
            {
                if (!wcscmp(name, L"title")) inTitle = title.empty() && !xml->IsEmptyElement();
                else if (!wcscmp(name, L"item"))
                    items.push_back({attr(L"id"), attr(L"href"), attr(L"media-type"), attr(L"properties")});
                else if (!wcscmp(name, L"itemref") && firstSpine.empty()) firstSpine = attr(L"idref");
                else if (!wcscmp(name, L"meta") && attr(L"name") == "cover") coverId = attr(L"content");
                else if (!wcscmp(name, L"reference") && attr(L"type") == "cover") guideCover = attr(L"href");
            }
            if (inTitle && (type == XmlNodeType_Text || type == XmlNodeType_CDATA))
            {
                const wchar_t* text = nullptr;
                UINT count = 0;
                if (SUCCEEDED(xml->GetValue(&text, &count))) title.append(text, std::min<size_t>(count, 512 - title.size()));
            }
            if (type == XmlNodeType_EndElement) inTitle = false;
        }
    }
};
} // namespace

bool LoadFb2(IStream* stream, UINT maxSize, Image& out)
{
    std::vector<uint8_t> file;
    if (!ReadAll(stream, file, 512ull << 20)) return false;
    if (file.size() >= 4 && Le32(file.data()) == 0x04034b50)
    {
        std::vector<ZipEntry> entries;
        if (!ReadZip(file, entries)) return false;
        std::vector<uint8_t> xml;
        for (const auto& e : entries)
            if (HasSuffix(e.name, ".fb2") && Extract(file, e, xml)) return Fb2Cover(xml, maxSize, out);
        return false;
    }
    return Fb2Cover(file, maxSize, out);
}

bool LoadEpub(IStream* stream, UINT maxSize, Image& out)
{
    std::vector<uint8_t> zip, bytes;
    std::vector<ZipEntry> entries;
    if (!ReadAll(stream, zip, 1024ull << 20) || !ReadZip(zip, entries)) return false;
    std::string opfPath;
    if (auto container = FindEntry(entries, "META-INF/container.xml"))
        if (container->size <= (4u << 20) && Extract(zip, *container, bytes)) opfPath = ContainerPackage(bytes);
    const ZipEntry* opf = FindEntry(entries, opfPath);
    if (!opf) for (auto& e : entries) if (HasSuffix(e.name, ".opf")) { opf = &e; break; }

    size_t attempts = 0;
    auto tryImage = [&](const ZipEntry* e) {
        return e && attempts++ < 32 && Extract(zip, *e, bytes) && DecodeCover(bytes, maxSize, out);
    };
    Package package;
    if (opf && opf->size <= (16u << 20) && Extract(zip, *opf, bytes))
    {
        package.Read(bytes);
        size_t lookups = 0;
        auto itemImage = [&](const std::string& href) {
            return !href.empty() && lookups++ < 128 && attempts < 32
                && tryImage(FindEntry(entries, ResolvePath(opf->name, href)));
        };
        for (auto& i : package.items)
            if (FindNoCase(i.props, "cover-image") != std::string::npos && itemImage(i.href)) return true;
        auto cover = package.Find(package.coverId);
        if (itemImage(cover ? cover->href : package.coverId)) return true;
        for (auto& i : package.items)
            if (i.type.compare(0, 6, "image/") == 0 && (FindNoCase(i.id, "cover") != std::string::npos
                || FindNoCase(i.href, "cover") != std::string::npos) && itemImage(i.href)) return true;
        auto first = package.Find(package.firstSpine);
        for (auto& href : {package.guideCover, cover ? cover->href : std::string(), first ? first->href : std::string()})
        {
            if (href.empty()) continue;
            auto page = FindEntry(entries, ResolvePath(opf->name, href));
            if (!page || page->size > (8u << 20) || !Extract(zip, *page, bytes)) continue;
            if (IsImageName(page->name) && DecodeCover(bytes, maxSize, out)) return true;
            std::string html(bytes.begin(), bytes.end());
            for (const char* tagName : { "<img", "<image" })
            {
                size_t pos = FindNoCase(html, tagName);
                if (pos == std::string::npos) continue;
                std::string tag = TagAt(html, pos);
                std::string src = Attribute(tag, "src");
                if (src.empty()) src = Attribute(tag, "href");
                if (!src.empty() && tryImage(FindEntry(entries, ResolvePath(page->name, src)))) return true;
            }
        }
        for (auto& i : package.items)
            if (i.type.compare(0, 6, "image/") == 0 && itemImage(i.href)) return true;
    }
    const ZipEntry* largest = nullptr;
    for (const auto& e : entries)
    {
        if (!IsImageName(e.name)) continue;
        if (FindNoCase(e.name, "cover") != std::string::npos && tryImage(&e)) return true;
        if (!largest || e.size > largest->size) largest = &e;
    }
    return tryImage(largest) || TitleThumbnail(package.title, maxSize, out);
}

bool LoadCbz(IStream* stream, UINT maxSize, Image& out)
{
    std::vector<uint8_t> zip, bytes;
    std::vector<ZipEntry> entries;
    if (!ReadAll(stream, zip, 1024ull << 20) || !ReadZip(zip, entries)) return false;
    struct Page { std::wstring name; const ZipEntry* entry; };
    std::vector<Page> pages;
    for (auto& e : entries)
    {
        if (e.name.find("__MACOSX/") != std::string::npos || !IsImageName(e.name)) continue;
        pages.push_back({ToWide(e.name.data(), e.name.size(), CP_UTF8), &e});
    }
    std::stable_sort(pages.begin(), pages.end(), [](auto& a, auto& b) { return StrCmpLogicalW(a.name.c_str(), b.name.c_str()) < 0; });
    for (size_t i = 0; i < std::min<size_t>(32, pages.size()); ++i)
        if (Extract(zip, *pages[i].entry, bytes) && DecodeCover(bytes, maxSize, out)) return true;
    return false;
}

bool LoadMobi(IStream* stream, UINT maxSize, Image& out)
{
    std::vector<uint8_t> file;
    if (!ReadAll(stream, file, 512ull << 20) || file.size() < 86 || memcmp(file.data() + 60, "BOOKMOBI", 8)) return false;
    size_t count = Be16(file.data() + 76);
    if (!count || count > (file.size() - 78) / 8) return false;
    std::vector<size_t> offsets(count + 1, file.size());
    size_t lower = 78 + count * 8;
    for (size_t i = 0; i < count; ++i)
    {
        offsets[i] = Be32(file.data() + 78 + i * 8);
        if (offsets[i] < lower || offsets[i] > file.size()) return false;
        lower = offsets[i];
    }
    const uint8_t* header = file.data() + offsets[0];
    size_t size = offsets[1] - offsets[0];
    if (size < 112 || memcmp(header + 16, "MOBI", 4) || Be16(header + 12)) return false;
    size_t headerLength = Be32(header + 20);
    if (headerLength < 96 || headerLength > size - 16) return false;
    size_t firstImage = Be32(header + 108);
    auto recordImage = [&](uint64_t i) {
        if (i == 0 || i >= count) return false;
        size_t length = offsets[i + 1] - offsets[i];
        if (!length || length > (64u << 20) || !DecodeWithWic(file.data() + offsets[i], length, out, 48ull << 20)) return false;
        FitImage(out, maxSize);
        return true;
    };
    size_t exth = 16 + headerLength;
    if (size - exth >= 12 && memcmp(header + exth, "EXTH", 4) == 0)
    {
        size_t block = Be32(header + exth + 4), records = Be32(header + exth + 8);
        if (block >= 12 && block <= size - exth)
        {
            size_t p = exth + 12, end = exth + block;
            for (size_t i = 0; i < records && end - p >= 8; ++i)
            {
                size_t id = Be32(header + p), len = Be32(header + p + 4);
                if (len < 8 || len > end - p) break;
                if (id == 201 && len >= 12 && recordImage(uint64_t(firstImage) + Be32(header + p + 8))) return true;
                p += len;
            }
        }
    }
    if (firstImage < count)
        for (size_t i = firstImage; i < std::min(count, firstImage + 32); ++i) if (recordImage(i)) return true;
    size_t titleOff = Be32(header + 84), titleLen = Be32(header + 88);
    if (titleOff > size || !titleLen || titleLen > size - titleOff || titleLen > 4096) return false;
    UINT codepage = Be32(header + 28);
    return TitleThumbnail(ToWide(reinterpret_cast<const char*>(header + titleOff), titleLen,
        codepage == CP_UTF8 ? CP_UTF8 : 1252), maxSize, out);
}
