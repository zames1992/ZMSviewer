// Software clay renderer for 3D thumbnails: orthographic three-quarter view, z-buffer,
// smooth normals with hard edges kept, key/fill/rim lighting, a little ambient occlusion from the
// depth buffer, supersampled and returned with a transparent background.
#include "Mesh.h"
#include <algorithm>
#include <cmath>
#include <numeric>

#include <cstdlib>

namespace {

struct V3
{
    float x, y, z;
    V3 operator+(const V3& o) const { return { x + o.x, y + o.y, z + o.z }; }
    V3 operator-(const V3& o) const { return { x - o.x, y - o.y, z - o.z }; }
    V3 operator*(float s) const { return { x * s, y * s, z * s }; }
};
float Dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(const V3& a, const V3& b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
V3 Normalize(const V3& v) { float l = std::sqrt(Dot(v, v)); return l > 0 ? v * (1.0f / l) : V3{ 0, 0, 1 }; }

// Merges vertices with identical positions so normals can be shared across UV seams and STL facets.
void Weld(Mesh& mesh)
{
    size_t n = mesh.VertexCount();
    if (n == 0) return;
    std::vector<uint32_t> order(n);
    std::iota(order.begin(), order.end(), 0u);
    const float* p = mesh.positions.data();
    auto key = [&](uint32_t i, int c) { uint32_t b; memcpy(&b, p + 3 * i + c, 4); return b; };
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        for (int c = 0; c < 3; c++) { uint32_t ka = key(a, c), kb = key(b, c); if (ka != kb) return ka < kb; }
        return a < b;
    });
    std::vector<uint32_t> remap(n);
    std::vector<float> positions;
    positions.reserve(mesh.positions.size());
    for (size_t i = 0; i < n; i++)
    {
        uint32_t v = order[i];
        bool same = i > 0 && key(v, 0) == key(order[i - 1], 0) && key(v, 1) == key(order[i - 1], 1) && key(v, 2) == key(order[i - 1], 2);
        if (!same) { positions.insert(positions.end(), p + 3 * v, p + 3 * v + 3); }
        remap[v] = uint32_t(positions.size() / 3 - 1);
    }
    for (auto& i : mesh.indices) i = remap[i];
    mesh.positions.swap(positions);
}

// Scene clean-up by object: a floor (a big flat plane under the model) or stray small pieces far
// away (hidden copies, leftovers) would shrink the model to a speck.
void CleanParts(Mesh& mesh)
{
    if (mesh.parts.size() < 2) return;
    struct Box { float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f }; bool any = false; };
    auto grow = [](Box& b, const float* p) { for (int k = 0; k < 3; k++) { b.lo[k] = std::min(b.lo[k], p[k]); b.hi[k] = std::max(b.hi[k], p[k]); } b.any = true; };
    auto merge = [&](Box& into, const Box& b) { if (b.any) { grow(into, b.lo); grow(into, b.hi); } };
    const size_t count = mesh.parts.size();
    auto range = [&](size_t i) { return std::pair<size_t, size_t>(mesh.parts[i], i + 1 < count ? mesh.parts[i + 1] : mesh.indices.size()); };
    std::vector<Box> boxes(count);
    std::vector<size_t> tris(count);
    std::vector<bool> flat(count), drop(count, false);
    size_t total = 0;
    for (size_t i = 0; i < count; i++)
    {
        auto [a, b] = range(i);
        for (size_t k = a; k < b; k++) grow(boxes[i], &mesh.positions[3 * size_t(mesh.indices[k])]);
        tris[i] = (b - a) / 3;
        total += tris[i];
        float ex = boxes[i].hi[0] - boxes[i].lo[0], ey = boxes[i].hi[1] - boxes[i].lo[1], ez = boxes[i].hi[2] - boxes[i].lo[2];
        flat[i] = boxes[i].any && ey <= 0.02f * std::max(ex, ez) && tris[i] <= 20000;
    }
    if (total == 0) return;

    // The main mass: the biggest objects holding 70% of the triangles (floors left out).
    std::vector<size_t> order(count);
    std::iota(order.begin(), order.end(), size_t(0));
    std::sort(order.begin(), order.end(), [&](size_t x, size_t y) { return tris[x] > tris[y]; });
    Box main;
    size_t taken = 0, solid = 0;
    for (size_t i : order) if (!flat[i]) solid += tris[i];
    for (size_t i : order)
    {
        if (flat[i] || !boxes[i].any) continue;
        merge(main, boxes[i]);
        taken += tris[i];
        if (taken >= solid * 7 / 10) break;
    }
    if (!main.any) return;
    float size = 0;
    for (int k = 0; k < 3; k++) size = std::max(size, main.hi[k] - main.lo[k]);
    Box nearBox;
    for (int k = 0; k < 3; k++) { nearBox.lo[k] = main.lo[k] - 0.35f * size; nearBox.hi[k] = main.hi[k] + 0.35f * size; }
    for (size_t i = 0; i < count; i++)
    {
        if (flat[i] || !boxes[i].any || tris[i] * 33 > total) continue;
        bool outside = false;
        for (int k = 0; k < 3; k++) outside = outside || boxes[i].hi[k] < nearBox.lo[k] || boxes[i].lo[k] > nearBox.hi[k];
        drop[i] = outside;
    }

    // Floors: flat, much wider than the rest, at its bottom.
    Box rest;
    for (size_t i = 0; i < count; i++) if (!flat[i] && !drop[i]) merge(rest, boxes[i]);
    if (rest.any)
    {
        float restWidth = std::max(rest.hi[0] - rest.lo[0], rest.hi[2] - rest.lo[2]);
        float restHeight = rest.hi[1] - rest.lo[1];
        for (size_t i = 0; i < count; i++)
        {
            if (!flat[i]) continue;
            float width = std::max(boxes[i].hi[0] - boxes[i].lo[0], boxes[i].hi[2] - boxes[i].lo[2]);
            if (width > 1.5f * restWidth && boxes[i].hi[1] <= rest.lo[1] + 0.25f * restHeight) drop[i] = true;
        }
    }
    if (std::find(drop.begin(), drop.end(), true) == drop.end()) return;
    size_t w = 0;
    for (size_t i = 0; i < count; i++)
    {
        if (drop[i]) continue;
        auto [a, b] = range(i);
        for (size_t k = a; k < b; k++) mesh.indices[w++] = mesh.indices[k];
    }
    if (w == 0) return;  // never drop everything
    mesh.indices.resize(w);
    mesh.parts.clear();
}

} // namespace

bool RenderMesh(Mesh& mesh, const RenderOptions& options, UINT size, Image& out)
{
    if (size == 0) return false;
    // Drop triangles that point outside the vertex list or at broken coordinates.
    {
        size_t n = mesh.VertexCount();
        std::vector<uint8_t> bad(n, 0);
        for (size_t i = 0; i < n; i++)
            for (int c = 0; c < 3; c++)
            {
                float v = mesh.positions[3 * i + c];
                if (!std::isfinite(v) || std::fabs(v) > 1e15f) bad[i] = 1;
            }
        size_t w = 0, part = 0;
        for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3)
        {
            // Keep object boundaries in step with the compaction.
            while (part < mesh.parts.size() && mesh.parts[part] <= t) mesh.parts[part++] = w;
            uint32_t a = mesh.indices[t], b = mesh.indices[t + 1], c = mesh.indices[t + 2];
            if (a >= n || b >= n || c >= n || bad[a] || bad[b] || bad[c] || a == b || b == c || a == c) continue;
            mesh.indices[w++] = a; mesh.indices[w++] = b; mesh.indices[w++] = c;
        }
        while (part < mesh.parts.size()) mesh.parts[part++] = w;
        mesh.indices.resize(w);
        size_t pw = 0;
        for (size_t i = 0; i + 2 < mesh.points.size(); i += 3)
        {
            float x = mesh.points[i], y = mesh.points[i + 1], z = mesh.points[i + 2];
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) continue;
            mesh.points[pw++] = x; mesh.points[pw++] = y; mesh.points[pw++] = z;
        }
        mesh.points.resize(pw);
    }
    if (mesh.indices.empty() && mesh.points.empty()) return false;
    if (!mesh.indices.empty()) Weld(mesh);

    // Everything to Y-up.
    if (options.up == UpAxis::Z)
    {
        auto swap = [](std::vector<float>& v) { for (size_t i = 0; i + 2 < v.size(); i += 3) { float y = v[i + 1]; v[i + 1] = v[i + 2]; v[i + 2] = -y; } };
        swap(mesh.positions);
        swap(mesh.points);
    }
    CleanParts(mesh);

    // Camera: in front, a little to the right and above.
    const float yaw = 0.60f, pitch = 0.36f;
    const V3 d = { std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch) };
    const V3 r = Normalize(Cross(d * -1.0f, { 0, 1, 0 }));
    const V3 u = Cross(r, d * -1.0f);

    const UINT ss = size <= 160 ? 4 : size <= 400 ? 3 : 2;
    const UINT S = std::min<UINT>(size * ss, 3072);
    const size_t vcount = mesh.VertexCount();
    const size_t pcount = mesh.points.size() / 3;

    // Project, and frame what is actually drawn.
    std::vector<float> sx(vcount), sy(vcount), sz(vcount);
    std::vector<uint8_t> used(vcount, 0);
    for (uint32_t i : mesh.indices) used[i] = 1;
    float minX = 1e30f, maxX = -1e30f, minY = 1e30f, maxY = -1e30f;
    for (size_t i = 0; i < vcount; i++)
    {
        V3 p = { mesh.positions[3 * i], mesh.positions[3 * i + 1], mesh.positions[3 * i + 2] };
        sx[i] = Dot(p, r); sy[i] = Dot(p, u); sz[i] = Dot(p, d);
        if (!used[i]) continue;
        minX = std::min(minX, sx[i]); maxX = std::max(maxX, sx[i]);
        minY = std::min(minY, sy[i]); maxY = std::max(maxY, sy[i]);
    }
    std::vector<float> px(pcount), py(pcount), pz(pcount);
    for (size_t i = 0; i < pcount; i++)
    {
        V3 p = { mesh.points[3 * i], mesh.points[3 * i + 1], mesh.points[3 * i + 2] };
        px[i] = Dot(p, r); py[i] = Dot(p, u); pz[i] = Dot(p, d);
        minX = std::min(minX, px[i]); maxX = std::max(maxX, px[i]);
        minY = std::min(minY, py[i]); maxY = std::max(maxY, py[i]);
    }
    float extent = std::max(maxX - minX, maxY - minY);
    if (!(extent > 0)) return false;
    const float margin = 0.05f;
    const float scale = S * (1.0f - 2 * margin) / extent;
    const float cx = (minX + maxX) / 2, cy = (minY + maxY) / 2;
    auto toX = [&](float x) { return (x - cx) * scale + S / 2.0f; };
    auto toY = [&](float y) { return S / 2.0f - (y - cy) * scale; };

    const size_t pixels = size_t(S) * S;
    std::vector<float> depth(pixels, -std::numeric_limits<float>::infinity());
    std::vector<V3> normal(pixels, V3{ 0, 0, 0 });  // camera space; zero = take it from the depth

    // Smooth vertex normals (area weighted), in camera space.
    std::vector<V3> vn(vcount, V3{ 0, 0, 0 });
    std::vector<V3> fn(mesh.indices.size() / 3);
    for (size_t t = 0; t < fn.size(); t++)
    {
        uint32_t a = mesh.indices[3 * t], b = mesh.indices[3 * t + 1], c = mesh.indices[3 * t + 2];
        V3 A = { sx[a], sy[a], sz[a] }, B = { sx[b], sy[b], sz[b] }, C = { sx[c], sy[c], sz[c] };
        V3 n = Cross(B - A, C - A);
        fn[t] = Normalize(n);
        vn[a] = vn[a] + n; vn[b] = vn[b] + n; vn[c] = vn[c] + n;
    }
    for (auto& n : vn) n = Normalize(n);

    for (size_t t = 0; t < fn.size(); t++)
    {
        uint32_t idx[3] = { mesh.indices[3 * t], mesh.indices[3 * t + 1], mesh.indices[3 * t + 2] };
        float X[3], Y[3], Z[3];
        V3 N[3];
        for (int k = 0; k < 3; k++)
        {
            X[k] = toX(sx[idx[k]]); Y[k] = toY(sy[idx[k]]); Z[k] = sz[idx[k]];
            // Keep creases sharp: a vertex normal far from the face normal is not used.
            N[k] = Dot(vn[idx[k]], fn[t]) > 0.6f ? vn[idx[k]] : fn[t];
        }
        float area = (X[1] - X[0]) * (Y[2] - Y[0]) - (X[2] - X[0]) * (Y[1] - Y[0]);
        if (std::fabs(area) < 1e-12f) continue;
        int x0 = std::max(0, int(std::floor(std::min({ X[0], X[1], X[2] }))));
        int x1 = std::min(int(S) - 1, int(std::ceil(std::max({ X[0], X[1], X[2] }))));
        int y0 = std::max(0, int(std::floor(std::min({ Y[0], Y[1], Y[2] }))));
        int y1 = std::min(int(S) - 1, int(std::ceil(std::max({ Y[0], Y[1], Y[2] }))));
        if (x0 > x1 || y0 > y1) continue;
        float inv = 1.0f / area;
        for (int y = y0; y <= y1; y++)
        {
            float fy = y + 0.5f;
            for (int x = x0; x <= x1; x++)
            {
                float fx = x + 0.5f;
                float w0 = ((X[1] - fx) * (Y[2] - fy) - (X[2] - fx) * (Y[1] - fy)) * inv;
                float w1 = ((X[2] - fx) * (Y[0] - fy) - (X[0] - fx) * (Y[2] - fy)) * inv;
                float w2 = 1.0f - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                float z = w0 * Z[0] + w1 * Z[1] + w2 * Z[2];
                size_t o = size_t(y) * S + x;
                if (z <= depth[o]) continue;
                depth[o] = z;
                V3 n = N[0] * w0 + N[1] * w1 + N[2] * w2;
                if (n.x == 0 && n.y == 0 && n.z == 0) n = fn[t];
                normal[o] = n;
            }
        }
    }

    // Point cloud: round splats sized to the point spacing; normals come from the depth later.
    if (pcount)
    {
        float coverage = float(S) * S * (1.0f - 2 * margin) * (1.0f - 2 * margin) * 0.35f;
        float radius = std::clamp(std::sqrt(coverage / (pcount * 0.5f)) * 0.75f, 1.0f, S / 40.0f);
        int ri = int(std::ceil(radius));
        for (size_t i = 0; i < pcount; i++)
        {
            float X = toX(px[i]), Y = toY(py[i]);
            int xc = int(X), yc = int(Y);
            for (int dy = -ri; dy <= ri; dy++)
                for (int dx = -ri; dx <= ri; dx++)
                {
                    int x = xc + dx, y = yc + dy;
                    if (x < 0 || y < 0 || x >= int(S) || y >= int(S)) continue;
                    float ex = x + 0.5f - X, ey = y + 0.5f - Y;
                    float d2 = (ex * ex + ey * ey) / (radius * radius);
                    if (d2 > 1.0f) continue;
                    // A sphere-shaped bump keeps neighbouring splats blending into a surface.
                    float z = pz[i] + std::sqrt(1.0f - d2) * radius / scale;
                    size_t o = size_t(y) * S + x;
                    if (z > depth[o]) { depth[o] = z; normal[o] = V3{ 0, 0, 0 }; }
                }
        }
        // Normals from the (lightly smoothed) depth for splat pixels.
        std::vector<float> smooth(depth);
        for (int y = 1; y + 1 < int(S); y++)
            for (int x = 1; x + 1 < int(S); x++)
            {
                size_t o = size_t(y) * S + x;
                if (!std::isfinite(depth[o]) || normal[o].z != 0 || normal[o].x != 0) continue;
                float sum = 0; int n = 0;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++)
                    {
                        float z = depth[o + dy * int(S) + dx];
                        if (std::isfinite(z)) { sum += z; n++; }
                    }
                smooth[o] = sum / n;
            }
        const float step = 1.0f / scale;
        for (int y = 0; y < int(S); y++)
            for (int x = 0; x < int(S); x++)
            {
                size_t o = size_t(y) * S + x;
                if (!std::isfinite(depth[o]) || normal[o].z != 0 || normal[o].x != 0 || normal[o].y != 0) continue;
                auto at = [&](int xx, int yy) { if (xx < 0 || yy < 0 || xx >= int(S) || yy >= int(S)) return -std::numeric_limits<float>::infinity(); return smooth[size_t(yy) * S + xx]; };
                float zl = at(x - 1, y), zr = at(x + 1, y), zu = at(x, y - 1), zd = at(x, y + 1), zc = smooth[o];
                float dzdx = std::isfinite(zl) && std::isfinite(zr) ? (zr - zl) / (2 * step) : std::isfinite(zr) ? (zr - zc) / step : std::isfinite(zl) ? (zc - zl) / step : 0;
                float dzdy = std::isfinite(zu) && std::isfinite(zd) ? (zu - zd) / (2 * step) : std::isfinite(zu) ? (zu - zc) / step : std::isfinite(zd) ? (zc - zd) / step : 0;
                normal[o] = Normalize({ -dzdx, -dzdy, 1.0f });
            }
    }

    // Screen-space ambient occlusion: neighbours standing in front of a pixel darken it.
    std::vector<float> ao(pixels, 1.0f);
    {
        const int dirs = 8;
        const float radiusPx = S * 0.025f;
        const float worldRadius = radiusPx / scale;
        for (int y = 0; y < int(S); y++)
            for (int x = 0; x < int(S); x++)
            {
                size_t o = size_t(y) * S + x;
                float z = depth[o];
                if (!std::isfinite(z)) continue;
                float occ = 0;
                int samples = 0;
                for (int k = 0; k < dirs; k++)
                {
                    float a = (k + 0.5f * ((x ^ y) & 1)) * 6.2831853f / dirs;
                    for (float rr : { 0.35f, 1.0f })
                    {
                        int xx = x + int(std::cos(a) * radiusPx * rr), yy = y + int(std::sin(a) * radiusPx * rr);
                        if (xx < 0 || yy < 0 || xx >= int(S) || yy >= int(S)) { samples++; continue; }
                        float zz = depth[size_t(yy) * S + xx];
                        samples++;
                        if (!std::isfinite(zz)) continue;
                        float diff = (zz - z) / (worldRadius * rr);
                        if (diff > 0.1f && diff < 3.0f) occ += std::min(1.0f, diff) * (1.0f - diff / 3.0f);
                    }
                }
                ao[o] = 1.0f - 0.55f * std::min(1.0f, occ / samples * 1.6f);
            }
    }

    // Shade.
    const V3 key = Normalize({ -0.55f, 0.65f, 0.55f });
    const V3 fill = Normalize({ 0.8f, 0.05f, 0.45f });
    const V3 half = Normalize(key + V3{ 0, 0, 1 });
    const V3 albedo = { 0.50f, 0.485f, 0.46f };  // warm clay, linear
    std::vector<float> color(pixels * 3, 0.0f);
    for (size_t o = 0; o < pixels; o++)
    {
        if (!std::isfinite(depth[o])) continue;
        V3 n = Normalize(normal[o]);
        if (n.z < 0) n = n * -1.0f;
        float diffuse = std::max(0.0f, Dot(n, key)) * 0.95f + std::max(0.0f, Dot(n, fill)) * 0.28f;
        float ambient = (0.20f + 0.12f * (n.y * 0.5f + 0.5f)) * ao[o];
        float rim = std::pow(1.0f - std::clamp(n.z, 0.0f, 1.0f), 3.0f) * 0.35f * (0.6f + 0.4f * n.y);
        float spec = std::pow(std::max(0.0f, Dot(n, half)), 36.0f) * 0.18f;
        float light = diffuse * (0.55f + 0.45f * ao[o]) + ambient;
        color[3 * o] = albedo.x * light + rim + spec;
        color[3 * o + 1] = albedo.y * light + rim + spec;
        color[3 * o + 2] = albedo.z * light + rim * 1.05f + spec;
    }

    // Supersampled pixels down to the thumbnail; crop to the square the model fills.
    out.width = out.height = (S + ss - 1) / ss;
    out.alpha = true;
    out.bgra.assign(size_t(out.width) * out.height * 4, 0);
    for (UINT y = 0; y < out.height; y++)
        for (UINT x = 0; x < out.width; x++)
        {
            float acc[3] = { 0, 0, 0 };
            int covered = 0, total = 0;
            for (UINT j = 0; j < ss; j++)
                for (UINT i = 0; i < ss; i++)
                {
                    UINT X = x * ss + i, Y = y * ss + j;
                    if (X >= S || Y >= S) continue;
                    total++;
                    size_t o = size_t(Y) * S + X;
                    if (!std::isfinite(depth[o])) continue;
                    covered++;
                    for (int c = 0; c < 3; c++) acc[c] += LinearToSrgb(color[3 * o + c]);
                }
            if (!covered) continue;
            uint8_t* dst = out.bgra.data() + (size_t(y) * out.width + x) * 4;
            for (int c = 0; c < 3; c++) dst[2 - c] = uint8_t(std::lround(std::clamp(acc[c] / covered, 0.0f, 1.0f) * 255));
            dst[3] = uint8_t(std::lround(255.0f * covered / std::max(total, 1)));
        }
    return true;
}
