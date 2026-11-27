// The formats this pack handles: COM class id per format and the file extensions that map to it.
// Shared by the thumbnail DLL, the installer and the test tool.
#pragma once
#include <windows.h>

enum class Format : int
{
    Psd, Tiff, Tga, Exr, Hdr, Dds, Fb2, Epub, Obj, Stl, Fbx, Max3ds, Blend, MayaAscii, MayaBinary, Ply, Glb,
    Pdf, Cbz, Mobi, Count
};

// One COM class per format, so the provider knows the format from the registration and never
// has to guess it from the bytes (TGA, OBJ and STL have no reliable signature).
// 1.1 uses fresh classes so previously loaded 1.0 handlers cannot serve the new registration.
inline CLSID FormatClsid(Format f)
{
    CLSID id = { 0x6e1f3a21, 0x8c4b, 0x4d7e, { 0xb5, 0xa2, 0x1c, 0x9d, 0x0e, 0x4f, 0x7a, 0x00 } };
    id.Data4[7] = static_cast<unsigned char>(f);
    return id;
}

struct FormatExtension
{
    const wchar_t* extension;
    Format format;
};

inline const FormatExtension kExtensions[] = {
    { L".psd", Format::Psd }, { L".psb", Format::Psd },
    { L".tif", Format::Tiff }, { L".tiff", Format::Tiff },
    { L".tga", Format::Tga },
    { L".exr", Format::Exr },
    { L".hdr", Format::Hdr },
    { L".dds", Format::Dds },
    { L".fb2", Format::Fb2 }, { L".fbz", Format::Fb2 },
    { L".epub", Format::Epub },
    { L".pdf", Format::Pdf }, { L".cbz", Format::Cbz },
    { L".mobi", Format::Mobi }, { L".azw", Format::Mobi }, { L".azw3", Format::Mobi },
    { L".obj", Format::Obj },
    { L".stl", Format::Stl },
    { L".fbx", Format::Fbx },
    { L".3ds", Format::Max3ds },
    { L".blend", Format::Blend },
    { L".ma", Format::MayaAscii },
    { L".mb", Format::MayaBinary },
    { L".ply", Format::Ply },
    { L".glb", Format::Glb }, { L".gltf", Format::Glb },
};

inline const wchar_t* kFormatNames[] = {
    L"Photoshop", L"TIFF", L"Targa", L"OpenEXR", L"Radiance HDR", L"DirectDraw Surface", L"FictionBook", L"EPUB",
    L"Wavefront OBJ", L"STL", L"FBX", L"3D Studio", L"Blender", L"Maya ASCII", L"Maya Binary", L"PLY", L"glTF",
    L"PDF", L"Comic Book ZIP", L"Mobipocket / Kindle",
};
static_assert(sizeof(kFormatNames) / sizeof(kFormatNames[0]) == int(Format::Count));
