// Implementations of the single-file libraries (see vendor\ and THIRD_PARTY.txt).
#define NOMINMAX
#pragma warning(push, 0)

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_TGA
#define STBI_ONLY_HDR
#define STBI_SUPPORT_ZLIB
#include "../vendor/stb_image.h"

#define TINYEXR_IMPLEMENTATION
#define TINYEXR_USE_MINIZ 0
#define TINYEXR_USE_STB_ZLIB 1
#define TINYEXR_USE_THREAD 0
#define TINYEXR_USE_OPENMP 0
#include "../vendor/tinyexr.h"
// tinyexr's writer wants a zlib compressor; we only read EXR files.
extern "C" unsigned char* stbi_zlib_compress(unsigned char*, int, int* out_len, int)
{
    *out_len = 0;
    return nullptr;
}

#define BCDEC_IMPLEMENTATION
#include "../vendor/bcdec.h"

#define CGLTF_IMPLEMENTATION
#include "../vendor/cgltf.h"

#pragma warning(pop)
