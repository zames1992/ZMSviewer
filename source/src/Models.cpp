// 3D files: loaders that turn each format into a Mesh for the clay renderer, plus the Blender
// thumbnail Blender itself stores inside .blend files.
#include "Mesh.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>

#include "../vendor/ufbx.h"
#include "../vendor/cgltf.h"
#include "../vendor/zstd.h"

namespace {

// ---------------------------------------------------------------------------------------------
// Text helpers: fast number parsing that does not depend on the process locale.

const char* SkipSpaces(const char* p, const char* end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r')) p++;
    return p;
}

bool ParseFloat(const char*& p, const char* end, float& out)
{
    p = SkipSpaces(p, end);
    const char* s = p;
    bool neg = false;
    if (p < end && (*p == '-' || *p == '+')) { neg = *p == '-'; p++; }
    double v = 0;
    bool digits = false;
    while (p < end && *p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; digits = true; }
    if (p < end && *p == '.')
    {
        p++;
        double f = 0.1;
        while (p < end && *p >= '0' && *p <= '9') { v += (*p - '0') * f; f *= 0.1; p++; digits = true; }
    }
    if (!digits)
    {
        // nan / inf and anything else: skip the token, report failure
        p = s;
        while (p < end && !std::isspace(uint8_t(*p))) p++;
        return false;
    }
    if (p < end && (*p == 'e' || *p == 'E'))
    {
        const char* save = p;
        p++;
        bool eneg = false;
        if (p < end && (*p == '-' || *p == '+')) { eneg = *p == '-'; p++; }
        int e = 0;
        bool edigits = false;
        while (p < end && *p >= '0' && *p <= '9') { e = std::min(e * 10 + (*p - '0'), 400); p++; edigits = true; }
        if (edigits) v *= std::pow(10.0, eneg ? -e : e);
        else p = save;
    }
    out = float(neg ? -v : v);
    return true;
}

bool ParseInt(const char*& p, const char* end, long long& out)
{
    p = SkipSpaces(p, end);
    bool neg = false;
    if (p < end && (*p == '-' || *p == '+')) { neg = *p == '-'; p++; }
    long long v = 0;
    bool digits = false;
    while (p < end && *p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; digits = true; }
    out = neg ? -v : v;
    return digits;
}

const char* LineEnd(const char* p, const char* end)
{
    const void* nl = memchr(p, '\n', size_t(end - p));
    return nl ? static_cast<const char*>(nl) : end;
}

bool Finish(Mesh& mesh, const RenderOptions& options, UINT size, Image& out)
{
    if (mesh.indices.empty() && mesh.points.empty()) return false;
    return RenderMesh(mesh, options, size, out);
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Wavefront OBJ: v and f lines are all a thumbnail needs.

bool LoadObj(IStream* stream, UINT maxSize, Image& out)
{
    std::vector<uint8_t> file;
    if (!ReadAll(stream, file)) return false;
    const char* p = reinterpret_cast<const char*>(file.data());
    const char* end = p + file.size();
    Mesh mesh;
    std::vector<uint32_t> poly;
    size_t badLines = 0, goodLines = 0;
    while (p < end)
    {
        const char* lineEnd = LineEnd(p, end);
        const char* q = SkipSpaces(p, lineEnd);
        if (q + 1 < lineEnd && q[0] == 'v' && (q[1] == ' ' || q[1] == '\t'))
        {
            q += 2;
            float x = 0, y = 0, z = 0;
            if (ParseFloat(q, lineEnd, x) && ParseFloat(q, lineEnd, y) && ParseFloat(q, lineEnd, z)) goodLines++;
            else badLines++;
            mesh.AddVertex(x, y, z);
        }
        else if (q + 1 < lineEnd && q[0] == 'f' && (q[1] == ' ' || q[1] == '\t'))
        {
            q += 2;
            poly.clear();
            long long v;
            for (;;)
            {
                q = SkipSpaces(q, lineEnd);
                if (q >= lineEnd || !ParseInt(q, lineEnd, v)) break;
                long long count = long long(mesh.VertexCount());
                long long index = v > 0 ? v - 1 : count + v;
                if (index >= 0 && index < count) poly.push_back(uint32_t(index));
                while (q < lineEnd && !std::isspace(uint8_t(*q))) q++;  // skip /vt/vn
            }
            if (poly.size() >= 3) { mesh.AddPolygon(poly.data(), poly.size()); goodLines++; }
        }
        else if (q + 1 < lineEnd && (q[0] == 'o' || q[0] == 'g') && (q[1] == ' ' || q[1] == '	'))
            mesh.BeginPart();
        else if (q < lineEnd && *q != '#' && *q != 'v' && *q != 'f' && *q != 'o' && *q != 'g' && *q != 's'
            && *q != 'u' && *q != 'm' && *q != 'l' && *q != 'p' && *q != 'c' && *q != '\n')
        {
            // Lots of garbage early on means this is not an OBJ (e.g. a compiler object file).
            if (++badLines > 64 && goodLines == 0) return false;
        }
        p = lineEnd + 1;
    }
    file.clear();
    file.shrink_to_fit();
    if (mesh.indices.empty())
    {
        // A point cloud saved as OBJ.
        mesh.points.swap(mesh.positions);
    }
    return Finish(mesh, RenderOptions{ UpAxis::Y }, maxSize, out);
}

// ---------------------------------------------------------------------------------------------
// STL, binary or ASCII. Usually Z-up (3D printing).

bool LoadStl(IStream* stream, UINT maxSize, Image& out)
{
    std::vector<uint8_t> file;
    if (!ReadAll(stream, file) || file.size() < 15) return false;
    Mesh mesh;
    bool binary = false;
    if (file.size() >= 84)
    {
        uint32_t n = Le32(file.data() + 80);
        binary = uint64_t(84) + uint64_t(n) * 50 == file.size();
        if (!binary && memcmp(file.data(), "solid", 5) != 0 && uint64_t(84) + uint64_t(n) * 50 <= file.size() && n > 0)
            binary = true;  // trailing bytes after a binary STL
        if (binary)
        {
            mesh.positions.reserve(size_t(n) * 9);
            mesh.indices.reserve(size_t(n) * 3);
            for (uint32_t t = 0; t < n; t++)
            {
                const uint8_t* f = file.data() + 84 + size_t(t) * 50 + 12;
                for (int k = 0; k < 3; k++)
                    mesh.indices.push_back(mesh.AddVertex(LeF32(f + 12 * k), LeF32(f + 12 * k + 4), LeF32(f + 12 * k + 8)));
            }
        }
    }
    if (!binary)
    {
        const char* p = reinterpret_cast<const char*>(file.data());
        const char* end = p + file.size();
        uint32_t corner = 0;
        while (p < end)
        {
            const char* lineEnd = LineEnd(p, end);
            const char* q = SkipSpaces(p, lineEnd);
            if (lineEnd - q > 7 && _strnicmp(q, "vertex", 6) == 0)
            {
                q += 6;
                float x, y, z;
                if (ParseFloat(q, lineEnd, x) && ParseFloat(q, lineEnd, y) && ParseFloat(q, lineEnd, z))
                {
                    uint32_t v = mesh.AddVertex(x, y, z);
                    if (++corner == 3) { mesh.indices.push_back(v - 2); mesh.indices.push_back(v - 1); mesh.indices.push_back(v); }
                }
            }
            else if (lineEnd - q >= 8 && _strnicmp(q, "endloop", 7) == 0)
                corner = 0;
            p = lineEnd + 1;
        }
    }
    return Finish(mesh, RenderOptions{ UpAxis::Z, true }, maxSize, out);
}

// ---------------------------------------------------------------------------------------------
// PLY: ASCII or binary, any element layout; uses vertex x/y/z and face vertex index lists.

namespace {

enum class PlyType { None, I8, U8, I16, U16, I32, U32, F32, F64 };

PlyType PlyTypeOf(const std::string& s)
{
    if (s == "char" || s == "int8") return PlyType::I8;
    if (s == "uchar" || s == "uint8") return PlyType::U8;
    if (s == "short" || s == "int16") return PlyType::I16;
    if (s == "ushort" || s == "uint16") return PlyType::U16;
    if (s == "int" || s == "int32") return PlyType::I32;
    if (s == "uint" || s == "uint32") return PlyType::U32;
    if (s == "float" || s == "float32") return PlyType::F32;
    if (s == "double" || s == "float64") return PlyType::F64;
    return PlyType::None;
}

size_t PlySize(PlyType t)
{
    switch (t) { case PlyType::I8: case PlyType::U8: return 1; case PlyType::I16: case PlyType::U16: return 2;
    case PlyType::I32: case PlyType::U32: case PlyType::F32: return 4; case PlyType::F64: return 8; default: return 0; }
}

struct PlyProperty { std::string name; PlyType type = PlyType::None, countType = PlyType::None; bool list = false; };
struct PlyElement { std::string name; uint64_t count = 0; std::vector<PlyProperty> props; };

double PlyBinary(const uint8_t* p, PlyType t, bool big)
{
    uint8_t b[8];
    size_t n = PlySize(t);
    for (size_t i = 0; i < n; i++) b[i] = big ? p[n - 1 - i] : p[i];
    switch (t)
    {
    case PlyType::I8: return int8_t(b[0]);
    case PlyType::U8: return b[0];
    case PlyType::I16: return int16_t(Le16(b));
    case PlyType::U16: return Le16(b);
    case PlyType::I32: return int32_t(Le32(b));
    case PlyType::U32: return Le32(b);
    case PlyType::F32: return LeF32(b);
    case PlyType::F64: { uint64_t v = Le64(b); double d; memcpy(&d, &v, 8); return d; }
    default: return 0;
    }
}

} // namespace

bool LoadPly(IStream* stream, UINT maxSize, Image& out)
{
    std::vector<uint8_t> file;
    if (!ReadAll(stream, file) || file.size() < 10 || memcmp(file.data(), "ply", 3) != 0) return false;
    const char* text = reinterpret_cast<const char*>(file.data());
    const char* end = text + file.size();
    const char* p = text;
    std::vector<PlyElement> elements;
    int format = -1;  // 0 ascii, 1 little, 2 big
    for (;;)
    {
        if (p >= end) return false;
        const char* lineEnd = LineEnd(p, end);
        std::string line(p, lineEnd);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        p = lineEnd + 1;
        std::vector<std::string> w;
        size_t s = 0;
        while (s < line.size())
        {
            size_t e = line.find_first_of(" \t", s);
            if (e == std::string::npos) e = line.size();
            if (e > s) w.push_back(line.substr(s, e - s));
            s = e + 1;
        }
        if (w.empty()) continue;
        if (w[0] == "end_header") break;
        if (w[0] == "format" && w.size() > 1)
            format = w[1] == "ascii" ? 0 : w[1] == "binary_little_endian" ? 1 : w[1] == "binary_big_endian" ? 2 : -1;
        else if (w[0] == "element" && w.size() > 2)
            elements.push_back({ w[1], uint64_t(_strtoui64(w[2].c_str(), nullptr, 10)), {} });
        else if (w[0] == "property" && !elements.empty() && w.size() > 2)
        {
            PlyProperty prop;
            if (w[1] == "list" && w.size() > 4) { prop.list = true; prop.countType = PlyTypeOf(w[2]); prop.type = PlyTypeOf(w[3]); prop.name = w[4]; }
            else { prop.type = PlyTypeOf(w[1]); prop.name = w[2]; }
            if (prop.type == PlyType::None) return false;
            elements.back().props.push_back(prop);
        }
    }
    if (format < 0) return false;
    const bool big = format == 2;
    const uint8_t* bin = reinterpret_cast<const uint8_t*>(p);
    const uint8_t* binEnd = file.data() + file.size();
    Mesh mesh;
    std::vector<uint32_t> poly;
    std::vector<double> values;

    for (auto& el : elements)
    {
        int ix = -1, iy = -1, iz = -1, iface = -1;
        for (size_t k = 0; k < el.props.size(); k++)
        {
            auto& n = el.props[k].name;
            if (n == "x") ix = int(k); else if (n == "y") iy = int(k); else if (n == "z") iz = int(k);
            else if (el.props[k].list && (n == "vertex_indices" || n == "vertex_index")) iface = int(k);
        }
        bool isVertex = el.name == "vertex" && ix >= 0 && iy >= 0 && iz >= 0;
        bool isFace = el.name == "face" && iface >= 0;
        // Fixed-size binary elements we do not need can be skipped in one step.
        bool fixed = true;
        size_t stride = 0;
        for (auto& prop : el.props) { if (prop.list) fixed = false; stride += PlySize(prop.type); }
        if (format != 0 && fixed && !isVertex && !isFace)
        {
            if (uint64_t(binEnd - bin) < el.count * stride) break;
            bin += el.count * stride;
            continue;
        }
        if (isVertex) mesh.positions.reserve(size_t(std::min<uint64_t>(el.count, 1ull << 28)) * 3);
        for (uint64_t i = 0; i < el.count; i++)
        {
            float xyz[3] = { 0, 0, 0 };
            for (size_t k = 0; k < el.props.size(); k++)
            {
                auto& prop = el.props[k];
                if (format == 0)
                {
                    const char* q = p;
                    auto number = [&]() { float f = 0; const char* e = LineEnd(q, end); if (!ParseFloat(q, e, f)) { /* skip */ } return double(f); };
                    if (prop.list)
                    {
                        long long n = 0;
                        if (!ParseInt(q, end, n) || n < 0 || n > 1000000) return false;
                        poly.clear();
                        for (long long j = 0; j < n; j++) { double v = number(); if (int(k) == iface) poly.push_back(uint32_t(v)); }
                        if (int(k) == iface && isFace && poly.size() >= 3) mesh.AddPolygon(poly.data(), poly.size());
                    }
                    else
                    {
                        double v = number();
                        if (int(k) == ix) xyz[0] = float(v); else if (int(k) == iy) xyz[1] = float(v); else if (int(k) == iz) xyz[2] = float(v);
                    }
                    p = q;
                }
                else
                {
                    if (prop.list)
                    {
                        size_t cs = PlySize(prop.countType), is = PlySize(prop.type);
                        if (bin + cs > binEnd) goto done;
                        uint64_t n = uint64_t(PlyBinary(bin, prop.countType, big));
                        bin += cs;
                        if (bin + n * is > binEnd) goto done;
                        if (int(k) == iface && isFace)
                        {
                            poly.clear();
                            for (uint64_t j = 0; j < n; j++) poly.push_back(uint32_t(PlyBinary(bin + j * is, prop.type, big)));
                            if (poly.size() >= 3) mesh.AddPolygon(poly.data(), poly.size());
                        }
                        bin += n * is;
                    }
                    else
                    {
                        size_t sz = PlySize(prop.type);
                        if (bin + sz > binEnd) goto done;
                        if (int(k) == ix || int(k) == iy || int(k) == iz)
                            xyz[int(k) == ix ? 0 : int(k) == iy ? 1 : 2] = float(PlyBinary(bin, prop.type, big));
                        bin += sz;
                    }
                }
            }
            if (format == 0) p = LineEnd(p, end) + 1;
            if (isVertex) mesh.AddVertex(xyz[0], xyz[1], xyz[2]);
        }
    }
done:
    if (mesh.indices.empty()) mesh.points.swap(mesh.positions);
    return Finish(mesh, RenderOptions{ UpAxis::Y }, maxSize, out);
}

// ---------------------------------------------------------------------------------------------
// 3D Studio (.3ds): chunked binary; object vertices are already in world space.

namespace {

void Walk3ds(const uint8_t* p, const uint8_t* end, Mesh& mesh, uint32_t& base)
{
    while (end - p >= 6)
    {
        uint16_t id = Le16(p);
        uint32_t len = Le32(p + 2);
        if (len < 6 || len > size_t(end - p)) return;
        const uint8_t* body = p + 6;
        const uint8_t* next = p + len;
        switch (id)
        {
        case 0x4D4D: case 0x3D3D: case 0x4100:
            Walk3ds(body, next, mesh, base);
            break;
        case 0x4000:  // named object: name, then children
        {
            const uint8_t* q = body;
            while (q < next && *q) q++;
            if (q < next) Walk3ds(q + 1, next, mesh, base);
            break;
        }
        case 0x4110:  // vertices
        {
            if (next - body < 2) break;
            uint16_t n = Le16(body);
            if (size_t(next - body) < 2 + size_t(n) * 12) break;
            base = uint32_t(mesh.VertexCount());
            for (uint16_t i = 0; i < n; i++)
            {
                const uint8_t* v = body + 2 + size_t(i) * 12;
                mesh.AddVertex(LeF32(v), LeF32(v + 4), LeF32(v + 8));
            }
            break;
        }
        case 0x4120:  // faces (a, b, c, flags), followed by sub-chunks we do not need
        {
            if (next - body < 2) break;
            uint16_t n = Le16(body);
            if (size_t(next - body) < 2 + size_t(n) * 8) break;
            mesh.BeginPart();
            for (uint16_t i = 0; i < n; i++)
            {
                const uint8_t* f = body + 2 + size_t(i) * 8;
                mesh.indices.push_back(base + Le16(f));
                mesh.indices.push_back(base + Le16(f + 2));
                mesh.indices.push_back(base + Le16(f + 4));
            }
            break;
        }
        }
        p = next;
    }
}

} // namespace

bool Load3ds(IStream* stream, UINT maxSize, Image& out)
{
    std::vector<uint8_t> file;
    if (!ReadAll(stream, file) || file.size() < 16 || Le16(file.data()) != 0x4D4D) return false;
    Mesh mesh;
    uint32_t base = 0;
    Walk3ds(file.data(), file.data() + file.size(), mesh, base);
    return Finish(mesh, RenderOptions{ UpAxis::Z }, maxSize, out);
}

// ---------------------------------------------------------------------------------------------
// An isolated Explorer stream contains binary GLB or JSON glTF with data URIs.
bool LoadGlb(IStream* stream, UINT maxSize, Image& out)
{
    std::vector<uint8_t> file;
    if (!ReadAll(stream, file)) return false;
    cgltf_options options = {};
    cgltf_data* data = nullptr;
    if (cgltf_parse(&options, file.data(), file.size(), &data) != cgltf_result_success) return false;
    std::unique_ptr<cgltf_data, decltype(&cgltf_free)> owner(data, cgltf_free);
    if (cgltf_validate(data) != cgltf_result_success) return false;
    size_t totalBytes = 0;
    if (data->buffers_count && !data->buffers[0].uri && data->bin && data->bin_size >= data->buffers[0].size)
    {
        data->buffers[0].data = const_cast<void*>(data->bin);
        data->buffers[0].data_free_method = cgltf_data_free_method_none;
    }
    for (cgltf_size i = 0; i < data->buffers_count; i++)
    {
        cgltf_buffer& b = data->buffers[i];
        if (b.size > (512ull << 20) - totalBytes) return false;
        totalBytes += b.size;
        if (b.data || !b.uri || strncmp(b.uri, "data:", 5) != 0) continue;
        const char* comma = strchr(b.uri, ',');
        if (comma && strstr(b.uri, ";base64,") && b.size <= strlen(comma + 1) * 3 / 4)
            if (cgltf_load_buffer_base64(&options, b.size, comma + 1, &b.data) == cgltf_result_success)
                b.data_free_method = cgltf_data_free_method_memory_free;
    }

    Mesh mesh;
    auto readable = [](const cgltf_accessor* a) {
        if (!a || a->is_sparse || !a->count || !a->stride || !a->buffer_view) return false;
        const auto* view = a->buffer_view;
        const auto* buf = view->buffer;
        size_t element = cgltf_calc_size(a->type, a->component_type);
        return element && buf && buf->data && !view->has_meshopt_compression &&
            view->offset <= buf->size && view->size <= buf->size - view->offset &&
            a->offset <= view->size && element <= view->size - a->offset &&
            a->count - 1 <= (view->size - a->offset - element) / a->stride;
    };
    auto addMesh = [&](const cgltf_mesh& m, const float* matrix) {
        for (cgltf_size pi = 0; pi < m.primitives_count; pi++)
        {
            const cgltf_primitive& prim = m.primitives[pi];
            if (prim.type != cgltf_primitive_type_triangles || prim.has_draco_mesh_compression) continue;
            const cgltf_accessor* pos = nullptr;
            for (cgltf_size a = 0; a < prim.attributes_count; a++)
                if (prim.attributes[a].type == cgltf_attribute_type_position) pos = prim.attributes[a].data;
            if (!readable(pos) || pos->type != cgltf_type_vec3 || (prim.indices && !readable(prim.indices))) continue;
            if (pos->count > 16000000 - mesh.VertexCount()) continue;
            mesh.BeginPart();
            uint32_t base = uint32_t(mesh.VertexCount());
            for (cgltf_size v = 0; v < pos->count; v++)
            {
                float p[3] = { 0, 0, 0 };
                cgltf_accessor_read_float(pos, v, p, 3);
                const float* M = matrix;
                mesh.AddVertex(M[0] * p[0] + M[4] * p[1] + M[8] * p[2] + M[12],
                    M[1] * p[0] + M[5] * p[1] + M[9] * p[2] + M[13],
                    M[2] * p[0] + M[6] * p[1] + M[10] * p[2] + M[14]);
            }
            if (prim.indices)
            {
                for (cgltf_size k = 0; k + 2 < prim.indices->count; k += 3)
                {
                    cgltf_size a = cgltf_accessor_read_index(prim.indices, k);
                    cgltf_size b = cgltf_accessor_read_index(prim.indices, k + 1);
                    cgltf_size c = cgltf_accessor_read_index(prim.indices, k + 2);
                    if (a < pos->count && b < pos->count && c < pos->count)
                        mesh.indices.insert(mesh.indices.end(), {base + uint32_t(a), base + uint32_t(b), base + uint32_t(c)});
                }
            }
            else
                for (cgltf_size k = 0; k + 2 < pos->count; k += 3)
                    for (int c = 0; c < 3; c++) mesh.indices.push_back(base + uint32_t(k + c));
        }
    };
    bool anyNode = false;
    for (cgltf_size i = 0; i < data->nodes_count; i++)
    {
        if (!data->nodes[i].mesh) continue;
        float m[16];
        cgltf_node_transform_world(&data->nodes[i], m);
        addMesh(*data->nodes[i].mesh, m);
        anyNode = true;
    }
    if (!anyNode)
    {
        const float identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        for (cgltf_size i = 0; i < data->meshes_count; i++) addMesh(data->meshes[i], identity);
    }
    owner.reset();
    file.clear();
    file.shrink_to_fit();
    return Finish(mesh, RenderOptions{ UpAxis::Y }, maxSize, out);
}

// ---------------------------------------------------------------------------------------------
// FBX through ufbx (binary and ASCII, all versions), converted to Y-up.

bool LoadFbx(IStream* stream, UINT maxSize, Image& out)
{
    std::vector<uint8_t> file;
    if (!ReadAll(stream, file)) return false;
    ufbx_load_opts opts = {};
    opts.target_axes = ufbx_axes_right_handed_y_up;
    opts.ignore_animation = true;
    opts.ignore_embedded = true;
    opts.load_external_files = false;
    opts.file_format = UFBX_FILE_FORMAT_FBX;
    ufbx_error error;
    ufbx_scene* scene = ufbx_load_memory(file.data(), file.size(), &opts, &error);
    file.clear();
    file.shrink_to_fit();
    if (!scene) return false;
    Mesh mesh;
    std::vector<uint32_t> tri;
    for (size_t ni = 0; ni < scene->nodes.count; ni++)
    {
        const ufbx_node* node = scene->nodes.data[ni];
        const ufbx_mesh* m = node->mesh;
        if (!m || !node->visible) continue;
        mesh.BeginPart();
        uint32_t base = uint32_t(mesh.VertexCount());
        for (size_t v = 0; v < m->num_vertices; v++)
        {
            ufbx_vec3 p = ufbx_transform_position(&node->geometry_to_world, m->vertices.data[v]);
            mesh.AddVertex(float(p.x), float(p.y), float(p.z));
        }
        tri.resize(m->max_face_triangles * 3);
        for (size_t f = 0; f < m->num_faces; f++)
        {
            uint32_t n = ufbx_triangulate_face(tri.data(), tri.size(), m, m->faces.data[f]);
            for (uint32_t k = 0; k < n * 3; k++) mesh.indices.push_back(base + m->vertex_indices.data[tri[k]]);
        }
    }
    ufbx_free_scene(scene);
    return Finish(mesh, RenderOptions{ UpAxis::Y }, maxSize, out);
}

// ---------------------------------------------------------------------------------------------
// Maya: transforms shared by the ASCII and binary readers.

namespace {

struct MayaTransform
{
    std::string parent;
    double t[3] = { 0, 0, 0 }, r[3] = { 0, 0, 0 }, s[3] = { 1, 1, 1 };
    double rp[3] = { 0, 0, 0 }, sp[3] = { 0, 0, 0 }, rpt[3] = { 0, 0, 0 }, spt[3] = { 0, 0, 0 };
    int order = 0;  // xyz yzx zxy xzy yxz zyx
    bool visible = true;
};

void ApplyTransform(const MayaTransform& m, double p[3])
{
    for (int i = 0; i < 3; i++) p[i] = (p[i] - m.sp[i]) * m.s[i] + m.sp[i] + m.spt[i] - m.rp[i];
    auto rot = [&](int axis, double a) {
        double c = std::cos(a), s = std::sin(a);
        int i = (axis + 1) % 3, j = (axis + 2) % 3;
        double x = p[i], y = p[j];
        p[i] = x * c - y * s;
        p[j] = x * s + y * c;
    };
    static const int orders[6][3] = { { 0, 1, 2 }, { 1, 2, 0 }, { 2, 0, 1 }, { 0, 2, 1 }, { 1, 0, 2 }, { 2, 1, 0 } };
    const int* o = orders[std::clamp(m.order, 0, 5)];
    for (int k = 0; k < 3; k++) rot(o[k], m.r[o[k]]);
    for (int i = 0; i < 3; i++) p[i] += m.rp[i] + m.rpt[i] + m.t[i];
}

// False when the object or one of its parents is hidden.
bool IsShown(const std::map<std::string, MayaTransform>& xforms, std::string parent)
{
    for (int depth = 0; depth < 256 && !parent.empty(); depth++)
    {
        auto it = xforms.find(parent);
        if (it == xforms.end()) break;
        if (!it->second.visible) return false;
        parent = it->second.parent;
    }
    return true;
}

void ToWorld(const std::map<std::string, MayaTransform>& xforms, std::string parent, double p[3])
{
    for (int depth = 0; depth < 256 && !parent.empty(); depth++)
    {
        auto it = xforms.find(parent);
        if (it == xforms.end()) break;
        ApplyTransform(it->second, p);
        parent = it->second.parent;
    }
}

std::string ShortName(std::string name)
{
    size_t bar = name.rfind('|');
    return bar == std::string::npos ? name : name.substr(bar + 1);
}

} // namespace

// Maya ASCII: createNode / setAttr statements; meshes from .vt, .ed and .fc.

bool LoadMayaAscii(IStream* stream, UINT maxSize, Image& out)
{
    std::vector<uint8_t> file;
    if (!ReadAll(stream, file) || file.size() < 16) return false;
    const char* text = reinterpret_cast<const char*>(file.data());
    const char* end = text + file.size();
    if (memcmp(text, "//Maya", 6) != 0) return false;

    struct MayaMesh
    {
        std::string parent;
        std::vector<float> vt, pt;
        std::vector<std::pair<uint32_t, uint32_t>> edges;
        std::vector<std::vector<long long>> faces;
        bool intermediate = false;
    };
    std::map<std::string, MayaTransform> xforms;
    std::vector<MayaMesh> meshes;
    enum { None, Xform, MeshNode } current = None;
    std::string currentName;

    // Splits the file into statements at ';' outside of quotes, and each into tokens.
    std::vector<std::string> tok;
    const char* p = text;
    while (p < end)
    {
        tok.clear();
        while (p < end)
        {
            while (p < end && std::isspace(uint8_t(*p))) p++;
            if (p >= end) break;
            if (*p == ';') { p++; break; }
            if (*p == '/' && p + 1 < end && p[1] == '/') { p = LineEnd(p, end); continue; }
            if (*p == '"')
            {
                const char* s = ++p;
                while (p < end && *p != '"') { if (*p == '\\') p++; p++; }
                tok.emplace_back(s, std::min(p, end));
                p++;
                continue;
            }
            const char* s = p;
            while (p < end && !std::isspace(uint8_t(*p)) && *p != ';') p++;
            tok.emplace_back(s, p);
        }
        if (tok.empty()) continue;

        if (tok[0] == "createNode" && tok.size() > 1)
        {
            std::string name, parent;
            for (size_t i = 2; i + 1 < tok.size(); i++)
            {
                if (tok[i] == "-n" || tok[i] == "-name") name = tok[++i];
                else if (tok[i] == "-p" || tok[i] == "-parent") parent = ShortName(tok[++i]);
            }
            if (tok[1] == "transform" || tok[1] == "joint")
            {
                current = Xform;
                currentName = name;
                xforms[name].parent = parent;
            }
            else if (tok[1] == "mesh")
            {
                current = MeshNode;
                meshes.push_back({});
                meshes.back().parent = parent;
            }
            else current = None;
            continue;
        }
        if (tok[0] != "setAttr" || current == None) { if (tok[0] != "setAttr") current = (tok[0] == "connectAttr" || tok[0] == "relationship") ? None : current; continue; }

        // setAttr [flags] "attr" [-type "t"] values...
        std::string attr;
        size_t i = 1;
        size_t valuesStart = 0;
        for (; i < tok.size(); i++)
        {
            const std::string& t = tok[i];
            if (t.size() > 1 && t[0] == '-' && std::isalpha(uint8_t(t[1])))
            {
                // Flags take one argument, except -av / -alteredValue.
                if (t != "-av" && t != "-alteredValue" && i + 1 < tok.size()) i++;
                continue;
            }
            if (attr.empty() && !t.empty() && t[0] == '.') { attr = t; continue; }
            if (!attr.empty()) { valuesStart = i; break; }
        }
        if (attr.empty() || valuesStart == 0) continue;
        auto number = [&](size_t k) { return k < tok.size() ? atof(tok[k].c_str()) : 0.0; };
        auto range = [&](const std::string& a, size_t& first) {
            size_t lb = a.find('['); first = lb == std::string::npos ? 0 : size_t(atoll(a.c_str() + lb + 1));
        };

        if (current == Xform)
        {
            MayaTransform& x = xforms[currentName];
            double* target = nullptr;
            if (attr == ".t") target = x.t; else if (attr == ".r") target = x.r; else if (attr == ".s") target = x.s;
            else if (attr == ".rp") target = x.rp; else if (attr == ".sp") target = x.sp;
            else if (attr == ".rpt") target = x.rpt; else if (attr == ".spt") target = x.spt;
            else if (attr == ".ro") x.order = int(number(valuesStart));
            else if (attr == ".v") x.visible = !(tok[valuesStart] == "no" || tok[valuesStart] == "off" || tok[valuesStart] == "false" || tok[valuesStart] == "0");
            if (target) for (int k = 0; k < 3; k++) target[k] = number(valuesStart + k);
            if (target == x.r) for (int k = 0; k < 3; k++) x.r[k] *= 3.14159265358979 / 180.0;
            continue;
        }

        MayaMesh& m = meshes.back();
        if (attr == ".io") { m.intermediate = tok[valuesStart] == "yes" || tok[valuesStart] == "on" || tok[valuesStart] == "1"; continue; }
        // Values are plain numbers here (skip a leading "-type" handled above).
        if (attr.compare(0, 4, ".vt[") == 0 || attr.compare(0, 4, ".pt[") == 0)
        {
            std::vector<float>& dst = attr[1] == 'v' ? m.vt : m.pt;
            size_t first;
            range(attr, first);
            size_t count = (tok.size() - valuesStart) / 3;
            if (dst.size() < (first + count) * 3) dst.resize((first + count) * 3, 0.0f);
            for (size_t k = 0; k < count * 3; k++) dst[first * 3 + k] = float(number(valuesStart + k));
        }
        else if (attr.compare(0, 4, ".ed[") == 0)
        {
            size_t first;
            range(attr, first);
            size_t count = (tok.size() - valuesStart) / 3;
            if (m.edges.size() < first + count) m.edges.resize(first + count);
            for (size_t k = 0; k < count; k++)
                m.edges[first + k] = { uint32_t(number(valuesStart + 3 * k)), uint32_t(number(valuesStart + 3 * k + 1)) };
        }
        else if (attr.compare(0, 4, ".fc[") == 0)
        {
            for (size_t k = valuesStart; k < tok.size();)
            {
                const std::string& t = tok[k];
                if ((t == "f" || t == "h") && k + 1 < tok.size())
                {
                    size_t n = size_t(atoll(tok[k + 1].c_str()));
                    if (t == "f")
                    {
                        std::vector<long long> face;
                        for (size_t j = 0; j < n && k + 2 + j < tok.size(); j++) face.push_back(atoll(tok[k + 2 + j].c_str()));
                        m.faces.push_back(std::move(face));
                    }
                    k += 2 + n;
                }
                else if ((t == "mu" || t == "mc") && k + 2 < tok.size())
                    k += 3 + size_t(atoll(tok[k + 2].c_str()));
                else
                    k++;
            }
        }
    }
    file.clear();
    file.shrink_to_fit();

    // Per transform: the shown shape if it has geometry, else its first original (intermediate)
    // shape, which is where Maya keeps the geometry of deformed objects.
    std::map<std::string, bool> hasShown;
    for (auto& m : meshes) if (!m.intermediate && !m.vt.empty() && !m.faces.empty()) hasShown[m.parent] = true;
    std::map<std::string, bool> used;
    Mesh mesh;
    std::vector<uint32_t> poly;
    for (auto& m : meshes)
    {
        size_t n = m.vt.size() / 3;
        if (n == 0 || m.faces.empty() || !IsShown(xforms, m.parent)) continue;
        if (m.intermediate && (hasShown[m.parent] || used[m.parent])) continue;
        used[m.parent] = true;
        mesh.BeginPart();
        uint32_t base = uint32_t(mesh.VertexCount());
        for (size_t v = 0; v < n; v++)
        {
            double w[3] = { m.vt[3 * v], m.vt[3 * v + 1], m.vt[3 * v + 2] };
            if (m.pt.size() >= 3 * v + 3) for (int k = 0; k < 3; k++) w[k] += m.pt[3 * v + k];
            ToWorld(xforms, m.parent, w);
            mesh.AddVertex(float(w[0]), float(w[1]), float(w[2]));
        }
        for (auto& face : m.faces)
        {
            poly.clear();
            for (long long e : face)
            {
                size_t index = size_t(e >= 0 ? e : -e - 1);
                if (index >= m.edges.size()) { poly.clear(); break; }
                uint32_t v = e >= 0 ? m.edges[index].first : m.edges[index].second;
                if (v >= n) { poly.clear(); break; }
                poly.push_back(base + v);
            }
            if (poly.size() >= 3) mesh.AddPolygon(poly.data(), poly.size());
        }
    }
    return Finish(mesh, RenderOptions{ UpAxis::Y }, maxSize, out);
}

// Maya Binary: an IFF file (FOR4/FOR8 groups, one group per node). A mesh node keeps its
// geometry in a MESH chunk laid out like the ASCII .vt/.ed/.fc data:
//   u32 float count, floats (x y z per vertex)
//   u32 word count, edges as word pairs (v1 with flag bits, v2)
//   u32 word count, face corners as edge indices: bit 31 = edge reversed, bits 29+30 = last edge
// followed by UVs we do not need. All big-endian.

bool LoadMayaBinary(IStream* stream, UINT maxSize, Image& out)
{
    std::vector<uint8_t> file;
    if (!ReadAll(stream, file) || file.size() < 32) return false;
    const uint8_t* d = file.data();
    bool wide;
    if (memcmp(d, "FOR8", 4) == 0) wide = true;
    else if (memcmp(d, "FOR4", 4) == 0) wide = false;
    else return false;
    const size_t header = wide ? 16 : 8, align = wide ? 8 : 4;

    std::map<std::string, MayaTransform> xforms;
    // Tweaks ("pt[a:b]") are per-vertex offsets added to the stored positions.
    struct Tweak { uint32_t first, count; size_t offset; };
    struct MeshChunk { std::string parent; size_t offset, size; bool intermediate; std::vector<Tweak> tweaks; };
    std::vector<MeshChunk> chunks;

    auto isGroup = [](const uint8_t* t) {
        return memcmp(t, "FOR", 3) == 0 || memcmp(t, "LIS", 3) == 0 || memcmp(t, "CAT", 3) == 0;
    };
    struct Frame { size_t pos, end; };
    std::vector<Frame> stack{ { 0, file.size() } };
    std::string nodeType, nodeName, nodeParent;
    bool nodeIntermediate = false;
    size_t nodeChunk = SIZE_MAX;
    std::vector<Tweak> pendingTweaks;
    while (!stack.empty())
    {
        Frame& f = stack.back();
        if (f.pos + header > f.end) { stack.pop_back(); continue; }
        const uint8_t* c = d + f.pos;
        uint64_t size = wide ? Be64(c + 8) : Be32(c + 4);
        size_t body = f.pos + header;
        if (size > f.end - body) { stack.pop_back(); continue; }
        if (isGroup(c))
        {
            f.pos = body + size_t(size);
            if (size >= 4)
            {
                nodeType.assign(reinterpret_cast<const char*>(d + body), 4);
                nodeName.clear();
                nodeParent.clear();
                nodeIntermediate = false;
                nodeChunk = SIZE_MAX;
                pendingTweaks.clear();
                stack.push_back({ body + 4, body + size_t(size) });
            }
            continue;
        }
        f.pos = body + ((size_t(size) + align - 1) / align * align);
        const char* b = reinterpret_cast<const char*>(d + body);
        size_t n = size_t(size);
        if (memcmp(c, "CREA", 4) == 0 && n > 2)
        {
            // flags byte, name, optional parent, then a 16-byte id (Maya 2016 and later)
            size_t q = 1;
            size_t nameEnd = q;
            while (nameEnd < n && b[nameEnd]) nameEnd++;
            nodeName.assign(b + q, nameEnd - q);
            q = nameEnd + 1;
            if (q < n && q + 16 != n)
            {
                size_t parentEnd = q;
                while (parentEnd < n && b[parentEnd]) parentEnd++;
                if (parentEnd + 1 == n || parentEnd + 1 + 16 == n) nodeParent = ShortName(std::string(b + q, parentEnd - q));
            }
            if (nodeType == "XFRM" || nodeType == "JOIN") xforms[nodeName].parent = nodeParent;
            continue;
        }
        size_t nameEnd = 0;
        while (nameEnd < n && b[nameEnd]) nameEnd++;
        if (nameEnd + 2 > n) continue;
        std::string attr(b, nameEnd);
        const uint8_t* value = d + body + nameEnd + 2;  // name, zero, one flags byte
        size_t valueSize = n - nameEnd - 2;
        if ((nodeType == "XFRM" || nodeType == "JOIN") && memcmp(c, "DBL3", 4) == 0 && valueSize >= 24)
        {
            MayaTransform& x = xforms[nodeName];
            double v[3];
            for (int k = 0; k < 3; k++) { uint64_t bits = Be64(value + 8 * k); memcpy(&v[k], &bits, 8); }
            double* target = attr == "t" ? x.t : attr == "r" ? x.r : attr == "s" ? x.s : attr == "rp" ? x.rp
                : attr == "sp" ? x.sp : attr == "rpt" ? x.rpt : attr == "spt" ? x.spt : nullptr;
            if (target) for (int k = 0; k < 3; k++) target[k] = v[k];  // rotation is in radians here
        }
        else if ((nodeType == "XFRM" || nodeType == "JOIN") && memcmp(c, "DBLE", 4) == 0 && attr == "v" && valueSize >= 8)
            xforms[nodeName].visible = Be64(value) != 0;
        else if (nodeType == "DMSH" && memcmp(c, "DBLE", 4) == 0 && attr == "io" && valueSize >= 8)
            nodeIntermediate = Be64(value) != 0;
        else if (nodeType == "DMSH" && memcmp(c, "MESH", 4) == 0 && valueSize >= 12)
        {
            nodeChunk = chunks.size();
            chunks.push_back({ nodeParent, size_t(value - d), valueSize, nodeIntermediate, std::move(pendingTweaks) });
            pendingTweaks.clear();
        }
        else if (nodeType == "DMSH" && memcmp(c, "FLT3", 4) == 0 && attr.compare(0, 3, "pt[") == 0)
        {
            unsigned long a = strtoul(attr.c_str() + 3, nullptr, 10), z = a;
            size_t colon = attr.find(':');
            if (colon != std::string::npos) z = strtoul(attr.c_str() + colon + 1, nullptr, 10);
            if (z >= a && uint64_t(z - a + 1) * 12 <= valueSize)
            {
                Tweak t{ uint32_t(a), uint32_t(z - a + 1), size_t(value - d) };
                if (nodeChunk != SIZE_MAX) chunks[nodeChunk].tweaks.push_back(t);
                else pendingTweaks.push_back(t);
            }
        }
    }

    // The same geometry is often kept several times (the original shape and copies along the
    // deformer chain): draw each distinct vertex buffer once per parent.
    // Per transform: shown shapes with geometry, else the first original (intermediate) shape.
    std::map<std::string, bool> hasShown, used;
    for (auto& ch : chunks) if (!ch.intermediate) hasShown[ch.parent] = true;
    Mesh mesh;
    std::map<std::pair<std::string, uint64_t>, bool> seen;
    std::vector<uint32_t> edges, poly;
    for (auto& ch : chunks)
    {
        if (!IsShown(xforms, ch.parent)) continue;
        if (ch.intermediate && (hasShown[ch.parent] || used[ch.parent])) continue;
        used[ch.parent] = true;
        const uint8_t* p = d + ch.offset;
        const uint8_t* end = p + ch.size;
        uint32_t floats = Be32(p);
        if (floats < 9 || floats % 3 || 4 + uint64_t(floats) * 4 + 8 > ch.size) continue;
        const uint8_t* verts = p + 4;
        uint32_t vcount = floats / 3;
        uint64_t key = (uint64_t(vcount) << 32) ^ Be32(verts) ^ (uint64_t(Be32(verts + 4)) << 7) ^ (uint64_t(Be32(verts + 8)) << 13);
        if (seen[{ ch.parent, key }]) continue;
        seen[{ ch.parent, key }] = true;

        const uint8_t* q = verts + size_t(floats) * 4;
        uint32_t edgeWords = Be32(q);
        q += 4;
        bool topology = edgeWords % 2 == 0 && uint64_t(edgeWords) * 4 + 4 <= uint64_t(end - q);
        const uint8_t* faces = nullptr;
        uint32_t faceWords = 0;
        if (topology)
        {
            edges.resize(edgeWords);
            for (uint32_t i = 0; i < edgeWords; i++) edges[i] = Be32(q + 4 * size_t(i)) & 0x1fffffff;
            q += size_t(edgeWords) * 4;
            faceWords = Be32(q);
            q += 4;
            faces = q;
            topology = uint64_t(faceWords) * 4 <= uint64_t(end - q);
        }

        mesh.BeginPart();
        uint32_t base = uint32_t(mesh.VertexCount());
        auto beFloat = [](const uint8_t* at) { uint32_t bits = Be32(at); float fl; memcpy(&fl, &bits, 4); return fl; };
        std::vector<float> offsets;
        for (const Tweak& t : ch.tweaks)
        {
            if (offsets.empty()) offsets.assign(size_t(vcount) * 3, 0.0f);
            for (uint32_t i = 0; i < t.count && uint64_t(t.first) + i < vcount; i++)
                for (int k = 0; k < 3; k++) offsets[3 * size_t(t.first + i) + k] = beFloat(d + t.offset + 12 * size_t(i) + 4 * k);
        }
        for (uint32_t i = 0; i < vcount; i++)
        {
            const uint8_t* v = verts + size_t(i) * 12;
            double w[3];
            for (int k = 0; k < 3; k++) w[k] = beFloat(v + 4 * k) + (offsets.empty() ? 0.0f : offsets[3 * size_t(i) + k]);
            ToWorld(xforms, ch.parent, w);
            mesh.AddVertex(float(w[0]), float(w[1]), float(w[2]));
        }
        size_t before = mesh.indices.size();
        bool broken = !topology;
        const uint32_t edgeCount = edgeWords / 2;
        poly.clear();
        for (uint32_t i = 0; topology && i < faceWords; i++)
        {
            uint32_t w = Be32(faces + 4 * size_t(i));
            uint32_t e = w & 0x1fffffff;
            if (e >= edgeCount) { broken = true; break; }
            uint32_t v = (w & 0x80000000) ? edges[2 * e + 1] : edges[2 * e];
            if (v >= vcount) { broken = true; break; }
            poly.push_back(base + v);
            if ((w & 0x60000000) == 0x60000000)
            {
                if (poly.size() >= 3) mesh.AddPolygon(poly.data(), poly.size());
                poly.clear();
            }
        }
        if (broken || mesh.indices.size() == before)
        {
            // Unknown layout: show the vertices as points rather than nothing.
            mesh.indices.resize(before);
            for (uint32_t i = 0; i < vcount; i++)
                for (int k = 0; k < 3; k++) mesh.points.push_back(mesh.positions[3 * size_t(base + i) + k]);
        }
    }
    file.clear();
    file.shrink_to_fit();
    return Finish(mesh, RenderOptions{ UpAxis::Y }, maxSize, out);
}

// ---------------------------------------------------------------------------------------------
// Blender: the preview picture Blender saves in the file (TEST block), read without loading
// the whole (often huge) file. Gzip-compressed files from older Blender are inflated first.

// Modern Blender may write a sequence of independent Zstandard frames.
static bool ReadZstdBlend(IStream* stream, std::vector<uint8_t>& result)
{
    LARGE_INTEGER start = {};
    if (FAILED(stream->Seek(start, STREAM_SEEK_SET, nullptr))) return false;
    std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> decoder(ZSTD_createDCtx(), ZSTD_freeDCtx);
    if (!decoder || ZSTD_isError(ZSTD_DCtx_setParameter(decoder.get(), ZSTD_d_windowLogMax, 27))) return false;
    std::vector<uint8_t> inputBytes(ZSTD_DStreamInSize()), outputBytes(ZSTD_DStreamOutSize());
    size_t remaining = 1;
    for (;;)
    {
        ULONG count = 0;
        if (FAILED(stream->Read(inputBytes.data(), ULONG(inputBytes.size()), &count))) return false;
        if (!count) return remaining == 0 && !result.empty();
        ZSTD_inBuffer input = { inputBytes.data(), count, 0 };
        while (input.pos < input.size)
        {
            ZSTD_outBuffer output = { outputBytes.data(), outputBytes.size(), 0 };
            remaining = ZSTD_decompressStream(decoder.get(), &output, &input);
            if (ZSTD_isError(remaining) || result.size() + output.pos > (512ull << 20)) return false;
            result.insert(result.end(), outputBytes.begin(), outputBytes.begin() + output.pos);
        }
    }
}

static bool ReadBlendModel(IStream* stream, UINT maxSize, Image& out, Image& savedPreview)
{
    LARGE_INTEGER zero = {};
    if (FAILED(stream->Seek(zero, STREAM_SEEK_SET, nullptr))) return false;
    uint8_t head[17] = {};
    ULONG got = 0;
    if (FAILED(stream->Read(head, sizeof(head), &got)) || got < 12) return false;

    std::vector<uint8_t> inflated;
    const uint8_t* mem = nullptr;
    size_t memSize = 0;
    if (Le32(head) == ZSTD_MAGICNUMBER)
    {
        if (!ReadZstdBlend(stream, inflated)) return false;
    }
    else if (head[0] == 0x1f && head[1] == 0x8b)
    {
        std::vector<uint8_t> file;
        if (!ReadAll(stream, file, 512ull << 20) || file.size() < 18) return false;
        size_t p = 10;
        uint8_t flags = file[3];
        if (flags & 4) { if (p + 2 > file.size()) return false; p += 2 + Le16(file.data() + p); }
        if (flags & 8) { while (p < file.size() && file[p]) p++; p++; }
        if (flags & 16) { while (p < file.size() && file[p]) p++; p++; }
        if (flags & 2) p += 2;
        if (p >= file.size() || !Inflate(file.data() + p, file.size() - p, false, inflated, size_t(Le32(file.data() + file.size() - 4))))
            return false;
    }
    if (!inflated.empty())
    {
        mem = inflated.data();
        memSize = inflated.size();
        if (memSize < 17) return false;
        memcpy(head, mem, 17);
    }
    if (memcmp(head, "BLENDER", 7) != 0) return false;

    // Old header: BLENDER + '_'/'-' (4/8-byte pointers) + 'v'/'V' (endianness) + version.
    // Blender 5 header: BLENDER17-01v0500 with 64-bit block headers.
    size_t headerSize, bheadSize, ptr;
    bool little, large = false;
    if (head[7] == '1' && head[8] == '7')
    {
        headerSize = 17; large = true; ptr = 8; little = head[12] == 'v'; bheadSize = 32;
    }
    else
    {
        headerSize = 12; ptr = head[7] == '-' ? 8 : 4; little = head[8] == 'v'; bheadSize = 16 + ptr;
    }

    auto readAt = [&](uint64_t pos, void* dst, size_t n) -> bool {
        if (mem) { if (pos > memSize || n > memSize - pos) return false; memcpy(dst, mem + pos, n); return true; }
        LARGE_INTEGER li; li.QuadPart = LONGLONG(pos);
        ULONG r = 0;
        return SUCCEEDED(stream->Seek(li, STREAM_SEEK_SET, nullptr)) && SUCCEEDED(stream->Read(dst, ULONG(n), &r)) && r == n;
    };
    auto u32 = [&](const uint8_t* p) { return little ? Le32(p) : Be32(p); };
    auto u64 = [&](const uint8_t* p) { return little ? Le64(p) : Be64(p); };

    // Walk the block headers once: the preview if there is one, and an index for the meshes.
    struct Block { char code[4]; uint64_t old, pos, len; uint32_t sdna, nr; };
    std::vector<Block> blocks;
    uint64_t pos = headerSize;
    for (size_t guard = 0; guard < 20000000; guard++)
    {
        uint8_t bh[32];
        if (!readAt(pos, bh, bheadSize)) break;
        Block b;
        memcpy(b.code, bh, 4);
        if (large) { b.sdna = u32(bh + 4); b.old = u64(bh + 8); b.len = u64(bh + 16); b.nr = uint32_t(u64(bh + 24)); }
        else { b.len = u32(bh + 4); b.old = ptr == 8 ? u64(bh + 8) : u32(bh + 8); b.sdna = u32(bh + 8 + ptr); b.nr = u32(bh + 12 + ptr); }
        if (pos > UINT64_MAX - bheadSize) return false;
        b.pos = pos + bheadSize;
        if (b.len > UINT64_MAX - b.pos) return false;
        if (memcmp(b.code, "ENDB", 4) == 0) break;
        if (!b.code[0] && !b.code[1] && !b.code[2] && !b.code[3]) break;  // damaged file (zero-filled)
        if (memcmp(b.code, "TEST", 4) == 0)
        {
            uint8_t wh[8];
            if (b.len >= 8 && readAt(b.pos, wh, 8))
            {
                uint32_t w = u32(wh), h = u32(wh + 4);
                if (w && h && w <= 4096 && h <= 4096 && uint64_t(w) * h * 4 + 8 <= b.len)
                {
                    std::vector<uint8_t> rgba(size_t(w) * h * 4);
                    if (readAt(b.pos + 8, rgba.data(), rgba.size()))
                    {
                        out.width = w;
                        out.height = h;
                        out.bgra.resize(rgba.size());
                        size_t visible = 0;
                        bool anyTransparent = false;
                        int darkest = 255, brightest = 0;
                        for (uint32_t y = 0; y < h; y++)  // stored bottom row first
                            for (uint32_t x = 0; x < w; x++)
                            {
                                const uint8_t* s = rgba.data() + (size_t(h - 1 - y) * w + x) * 4;
                                uint8_t* t = out.bgra.data() + (size_t(y) * w + x) * 4;
                                t[0] = s[2]; t[1] = s[1]; t[2] = s[0]; t[3] = s[3];
                                anyTransparent = anyTransparent || s[3] != 255;
                                visible += s[3] > 16;
                                if (s[3] > 16)
                                {
                                    int luma = (s[0] * 3 + s[1] * 6 + s[2]) / 10;
                                    darkest = std::min(darkest, luma);
                                    brightest = std::max(brightest, luma);
                                }
                            }
                        out.alpha = anyTransparent;
                        // An empty preview (nothing drawn) is worse than drawing the meshes.
                        // So is a plain one-color picture (an empty viewport or a blank render).
                        if (visible > size_t(w) * h / 50 && brightest - darkest > 12)
                        {
                            FitImage(out, maxSize);
                            savedPreview = std::move(out);
                        }
                    }
                }
            }
        }
        blocks.push_back(b);
        pos = b.pos + b.len;
    }

    // ---- No preview: read the meshes through the file's own struct description (SDNA). ----
    const Block* dnaBlock = nullptr;
    for (auto& b : blocks) if (memcmp(b.code, "DNA1", 4) == 0) dnaBlock = &b;
    if (!dnaBlock || dnaBlock->len > (64u << 20)) return false;
    std::vector<uint8_t> dna(size_t(dnaBlock->len));
    if (!readAt(dnaBlock->pos, dna.data(), dna.size())) return false;
    auto u16 = [&](const uint8_t* p) { return little ? Le16(p) : Be16(p); };

    std::vector<std::string> names, types;
    std::vector<uint16_t> typeLen;
    struct Field { std::string name; uint16_t type; bool pointer; size_t offset, size, count; };
    struct Struct { uint16_t type; size_t size; std::vector<Field> fields; };
    std::vector<Struct> structs;
    {
        size_t p = 0;
        auto expect = [&](const char* tag) { while (p % 4) p++; if (p + 4 > dna.size() || memcmp(dna.data() + p, tag, 4) != 0) return false; p += 4; return true; };
        auto strings = [&](std::vector<std::string>& list) {
            if (p + 4 > dna.size()) return false;
            uint32_t n = u32(dna.data() + p);
            p += 4;
            for (uint32_t i = 0; i < n; i++)
            {
                size_t e = p;
                while (e < dna.size() && dna[e]) e++;
                if (e >= dna.size()) return false;
                list.emplace_back(reinterpret_cast<const char*>(dna.data() + p), e - p);
                p = e + 1;
            }
            return true;
        };
        if (!expect("SDNA") || !expect("NAME") || !strings(names) || !expect("TYPE") || !strings(types) || !expect("TLEN")) return false;
        for (size_t i = 0; i < types.size(); i++) { if (p + 2 > dna.size()) return false; typeLen.push_back(u16(dna.data() + p)); p += 2; }
        if (!expect("STRC") || p + 4 > dna.size()) return false;
        uint32_t n = u32(dna.data() + p);
        p += 4;
        for (uint32_t i = 0; i < n; i++)
        {
            if (p + 4 > dna.size()) return false;
            Struct st;
            st.type = u16(dna.data() + p);
            if (st.type >= types.size()) return false;
            uint16_t fields = u16(dna.data() + p + 2);
            p += 4;
            size_t offset = 0;
            for (uint16_t k = 0; k < fields; k++)
            {
                if (p + 4 > dna.size()) return false;
                uint16_t t = u16(dna.data() + p), nameIndex = u16(dna.data() + p + 2);
                p += 4;
                if (t >= types.size() || nameIndex >= names.size()) return false;
                const std::string& raw = names[nameIndex];
                Field f;
                f.type = t;
                f.pointer = raw.find('*') != std::string::npos;
                f.count = 1;
                for (size_t b = raw.find('['); b != std::string::npos; b = raw.find('[', b + 1))
                {
                    size_t dim = size_t(std::max(1L, atol(raw.c_str() + b + 1)));
                    if (dim > 65535 / f.count) return false;
                    f.count *= dim;
                }
                for (char c : raw)
                {
                    if (c == '[') break;
                    if (c != '*' && c != '(' && c != ')') f.name += c;
                }
                f.offset = offset;
                f.size = (f.pointer ? ptr : typeLen[t]) * f.count;
                if (offset > 65535 || f.size > 65535 - offset) return false;
                offset += f.size;
                st.fields.push_back(f);
            }
            st.size = typeLen[st.type];
            if (offset > st.size) return false;
            structs.push_back(std::move(st));
        }
    }
    auto findStruct = [&](const char* name) -> const Struct* {
        for (auto& st : structs) if (types[st.type] == name) return &st;
        return nullptr;
    };
    auto field = [&](const Struct* st, std::initializer_list<const char*> candidates, size_t minSize = 1) -> const Field* {
        if (!st) return nullptr;
        for (const char* c : candidates) for (auto& f : st->fields) if (f.name == c && f.size >= minSize) return &f;
        return nullptr;
    };
    const Struct* sObject = findStruct("Object");
    const Struct* sMesh = findStruct("Mesh");
    const Struct* sCustomData = findStruct("CustomData");
    const Struct* sLayer = findStruct("CustomDataLayer");
    const Struct* sMVert = findStruct("MVert");
    const Struct* sMPoly = findStruct("MPoly");
    const Struct* sMLoop = findStruct("MLoop");
    if (!sMesh) return false;

    std::unordered_map<uint64_t, const Block*> byAddress;
    for (auto& b : blocks) if (b.old) byAddress[b.old] = &b;
    auto readBlock = [&](const Block* b, std::vector<uint8_t>& data) {
        if (!b || b->len > (1536ull << 20)) return false;
        data.resize(size_t(b->len));
        return readAt(b->pos, data.data(), data.size());
    };
    auto readPtr = [&](const uint8_t* p) -> uint64_t { return ptr == 8 ? u64(p) : u32(p); };
    auto intAt = [&](const std::vector<uint8_t>& data, const Field* f) -> int64_t {
        if (!f || f->offset + 4 > data.size()) return -1;
        return int32_t(u32(data.data() + f->offset));
    };

    struct LayerRef { int type; std::string name; uint64_t data; };
    auto layersOf = [&](const std::vector<uint8_t>& me, const char* customDataName) {
        std::vector<LayerRef> result;
        const Field* cd = field(sMesh, { customDataName });
        const Field* layers = field(sCustomData, { "layers" });
        const Field* total = field(sCustomData, { "totlayer" });
        const Field* type = field(sLayer, { "type" }, 4);
        const Field* name = field(sLayer, { "name" });
        const Field* data = field(sLayer, { "data" }, ptr);
        if (!cd || !layers || !total || !type || !name || !data || !sLayer) return result;
        if (cd->offset + layers->offset + ptr > me.size() || cd->offset + total->offset + 4 > me.size()) return result;
        uint64_t layersPtr = readPtr(me.data() + cd->offset + layers->offset);
        int32_t count = int32_t(u32(me.data() + cd->offset + total->offset));
        auto it = byAddress.find(layersPtr);
        std::vector<uint8_t> raw;
        if (count <= 0 || count > 4096 || it == byAddress.end() || !readBlock(it->second, raw)) return result;
        for (int32_t i = 0; i < count && size_t(i + 1) * sLayer->size <= raw.size(); i++)
        {
            const uint8_t* l = raw.data() + size_t(i) * sLayer->size;
            LayerRef r;
            r.type = int32_t(u32(l + type->offset));
            const char* n = reinterpret_cast<const char*>(l + name->offset);
            r.name.assign(n, strnlen(n, name->size));
            r.data = readPtr(l + data->offset);
            result.push_back(r);
        }
        return result;
    };
    auto dataBlock = [&](uint64_t address, std::vector<uint8_t>& data) {
        auto it = byAddress.find(address);
        return address && it != byAddress.end() && readBlock(it->second, data);
    };

    // Blender 5 stores mesh attributes in a named array. The payload and its
    // descriptor are separate blocks, so follow serialized pointers via SDNA.
    const Struct* storage = findStruct("AttributeStorage");
    const Struct* attr = findStruct("Attribute");
    const Struct* array = findStruct("AttributeArray");
    auto modernLayers = [&](const std::vector<uint8_t>& me, std::vector<LayerRef>& verts, std::vector<LayerRef>& corners) {
        const Field* embedded = field(sMesh, {"attribute_storage"}, storage ? storage->size : 1);
        const Field* entries = field(storage, {"dna_attributes"}, ptr);
        const Field* countField = field(storage, {"dna_attributes_num"}, 4);
        const Field* nameField = field(attr, {"name"}, ptr);
        const Field* payload = field(attr, {"data"}, ptr);
        const Field* storageType = field(attr, {"storage_type"});
        const Field* samples = field(array, {"data"}, ptr);
        if (!embedded || !entries || !countField || !nameField || !payload || !storageType || !samples || !attr->size) return;
        const uint8_t* base = me.data() + embedded->offset;
        uint32_t count = u32(base + countField->offset);
        std::vector<uint8_t> rows;
        if (!count || count > 4096 || !dataBlock(readPtr(base + entries->offset), rows) || uint64_t(count) * attr->size > rows.size()) return;
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint8_t* row = rows.data() + size_t(i) * attr->size;
            if (row[storageType->offset] != 0) continue; // only dense arrays store mesh topology
            auto found = byAddress.find(readPtr(row + nameField->offset));
            if (found == byAddress.end() || found->second->len > 128) continue;
            std::vector<uint8_t> name, desc;
            if (!readBlock(found->second, name) || name.empty() || name.back() != 0) continue;
            std::string key(reinterpret_cast<const char*>(name.data()));
            if (key != "position" && key != ".corner_vert") continue;
            if (!dataBlock(readPtr(row + payload->offset), desc) || desc.size() < array->size) continue;
            (key == "position" ? verts : corners).push_back({-1, key, readPtr(desc.data() + samples->offset)});
        }
    };

    // One mesh datablock to triangles in local space.
    struct Local { std::vector<float> positions; std::vector<uint32_t> indices; };
    std::unordered_map<uint64_t, Local> meshCache;
    auto loadMesh = [&](const Block* meBlock, Local& local) -> bool {
        std::vector<uint8_t> me;
        if (!readBlock(meBlock, me) || me.size() < sMesh->size) return false;
        int64_t nVerts = intAt(me, field(sMesh, { "totvert", "verts_num" }));
        int64_t nFaces = intAt(me, field(sMesh, { "totpoly", "faces_num", "polys_num" }));
        int64_t nCorners = intAt(me, field(sMesh, { "totloop", "corners_num", "loops_num" }));
        if (nVerts <= 0 || nVerts > 100000000) return false;
        auto vLayers = layersOf(me, "vdata"), lLayers = layersOf(me, "ldata"), pLayers = layersOf(me, "pdata");
        modernLayers(me, vLayers, lLayers);
        std::vector<uint8_t> buf;

        // Positions: "position" float3 layer (3.5+), MVert layer or pointer (older).
        local.positions.clear();
        for (auto& l : vLayers)
            if (l.name == "position" && dataBlock(l.data, buf) && buf.size() >= size_t(nVerts) * 12)
            {
                local.positions.resize(size_t(nVerts) * 3);
                for (size_t i = 0; i < local.positions.size(); i++) { uint32_t b = u32(buf.data() + 4 * i); memcpy(&local.positions[i], &b, 4); }
                break;
            }
        if (local.positions.empty() && sMVert)
        {
            uint64_t address = 0;
            for (auto& l : vLayers) if (l.type == 0) { address = l.data; break; }
            const Field* mvert = field(sMesh, { "mvert" });
            if (!address && mvert && mvert->offset + ptr <= me.size()) address = readPtr(me.data() + mvert->offset);
            const Field* co = field(sMVert, { "co" }, 12);
            if (co && dataBlock(address, buf) && buf.size() >= size_t(nVerts) * sMVert->size)
            {
                local.positions.resize(size_t(nVerts) * 3);
                for (int64_t i = 0; i < nVerts; i++)
                    for (int k = 0; k < 3; k++)
                    {
                        uint32_t b = u32(buf.data() + size_t(i) * sMVert->size + co->offset + 4 * k);
                        memcpy(&local.positions[size_t(i) * 3 + k], &b, 4);
                    }
            }
        }
        if (local.positions.empty()) return false;

        // Corner vertices: ".corner_vert" int layer (3.6+) or MLoop.
        std::vector<uint32_t> corners;
        if (nCorners > 0 && nCorners < 1000000000)
        {
            for (auto& l : lLayers)
                if (l.name == ".corner_vert" && dataBlock(l.data, buf) && buf.size() >= size_t(nCorners) * 4)
                {
                    corners.resize(size_t(nCorners));
                    for (size_t i = 0; i < corners.size(); i++) corners[i] = u32(buf.data() + 4 * i);
                    break;
                }
            if (corners.empty() && sMLoop)
            {
                uint64_t address = 0;
                for (auto& l : lLayers) if (l.type == 26) { address = l.data; break; }
                const Field* mloop = field(sMesh, { "mloop" });
                if (!address && mloop && mloop->offset + ptr <= me.size()) address = readPtr(me.data() + mloop->offset);
                const Field* v = field(sMLoop, { "v" }, 4);
                if (v && dataBlock(address, buf) && buf.size() >= size_t(nCorners) * sMLoop->size)
                {
                    corners.resize(size_t(nCorners));
                    for (size_t i = 0; i < corners.size(); i++) corners[i] = u32(buf.data() + i * sMLoop->size + v->offset);
                }
            }
        }
        // Face ranges: offset array (3.6+) or MPoly (loopstart, totloop).
        std::vector<std::pair<uint32_t, uint32_t>> faces;
        if (!corners.empty() && nFaces > 0)
        {
            const Field* offsets = field(sMesh, { "poly_offset_indices", "face_offset_indices" });
            if (offsets && offsets->offset + ptr <= me.size() && dataBlock(readPtr(me.data() + offsets->offset), buf)
                && buf.size() >= size_t(nFaces + 1) * 4)
            {
                for (int64_t i = 0; i < nFaces; i++)
                {
                    uint32_t a = u32(buf.data() + 4 * size_t(i)), b = u32(buf.data() + 4 * size_t(i + 1));
                    if (b > a) faces.push_back({ a, b - a });
                }
            }
            else if (sMPoly)
            {
                uint64_t address = 0;
                for (auto& l : pLayers) if (l.type == 25) { address = l.data; break; }
                const Field* mpoly = field(sMesh, { "mpoly" });
                if (!address && mpoly && mpoly->offset + ptr <= me.size()) address = readPtr(me.data() + mpoly->offset);
                const Field* start = field(sMPoly, { "loopstart" }, 4);
                const Field* count = field(sMPoly, { "totloop" }, 4);
                if (start && count && dataBlock(address, buf) && buf.size() >= size_t(nFaces) * sMPoly->size)
                    for (int64_t i = 0; i < nFaces; i++)
                    {
                        const uint8_t* f = buf.data() + size_t(i) * sMPoly->size;
                        faces.push_back({ u32(f + start->offset), u32(f + count->offset) });
                    }
            }
        }
        local.indices.clear();
        std::vector<uint32_t> poly;
        for (auto& f : faces)
        {
            if (uint64_t(f.first) + f.second > corners.size() || f.second < 3) continue;
            poly.assign(corners.begin() + f.first, corners.begin() + f.first + f.second);
            bool ok = true;
            for (uint32_t v : poly) ok = ok && v < uint32_t(nVerts);
            if (!ok) continue;
            for (size_t i = 2; i < poly.size(); i++) { local.indices.push_back(poly[0]); local.indices.push_back(poly[i - 1]); local.indices.push_back(poly[i]); }
        }
        return true;
    };

    Mesh mesh;
    auto add = [&](const Local& local, const float* m) {
        mesh.BeginPart();
        uint32_t base = uint32_t(mesh.VertexCount());
        size_t n = local.positions.size() / 3;
        for (size_t i = 0; i < n; i++)
        {
            float x = local.positions[3 * i], y = local.positions[3 * i + 1], z = local.positions[3 * i + 2];
            if (m) mesh.AddVertex(m[0] * x + m[4] * y + m[8] * z + m[12], m[1] * x + m[5] * y + m[9] * z + m[13], m[2] * x + m[6] * y + m[10] * z + m[14]);
            else mesh.AddVertex(x, y, z);
        }
        if (local.indices.empty())
            for (size_t i = 0; i < n; i++) for (int k = 0; k < 3; k++) mesh.points.push_back(mesh.positions[3 * (base + i) + k]);
        for (uint32_t i : local.indices) mesh.indices.push_back(base + i);
    };

    const Field* obType = field(sObject, { "type" }, 2);
    const Field* obData = field(sObject, { "data" }, ptr);
    const Field* obMatrix = field(sObject, { "obmat", "object_to_world" }, 64);
    bool anyObject = false;
    if (obType && obData && obMatrix)
        for (auto& b : blocks)
        {
            if (memcmp(b.code, "OB\0\0", 4) != 0) continue;
            std::vector<uint8_t> ob;
            if (!readBlock(&b, ob) || ob.size() < sObject->size) continue;
            if (u16(ob.data() + obType->offset) != 1) continue;  // OB_MESH
            uint64_t address = readPtr(ob.data() + obData->offset);
            auto it = byAddress.find(address);
            if (it == byAddress.end() || memcmp(it->second->code, "ME\0\0", 4) != 0) continue;
            auto cached = meshCache.find(address);
            if (cached == meshCache.end())
            {
                Local local;
                if (!loadMesh(it->second, local)) local.positions.clear();
                cached = meshCache.emplace(address, std::move(local)).first;
            }
            if (cached->second.positions.empty()) continue;
            float m[16];
            for (int k = 0; k < 16; k++) { uint32_t bits = u32(ob.data() + obMatrix->offset + 4 * k); memcpy(&m[k], &bits, 4); }
            add(cached->second, m);
            anyObject = true;
        }
    if (!anyObject)
        for (auto& b : blocks)
        {
            if (memcmp(b.code, "ME\0\0", 4) != 0) continue;
            Local local;
            if (loadMesh(&b, local)) add(local, nullptr);
        }
    return Finish(mesh, RenderOptions{ UpAxis::Z }, maxSize, out);
}

bool LoadBlend(IStream* stream, UINT maxSize, Image& out)
{
    Image savedPreview;
    if (ReadBlendModel(stream, maxSize, out, savedPreview)) return true;
    if (savedPreview.bgra.empty()) return false;
    out = std::move(savedPreview);
    return true;
}
