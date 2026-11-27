// A triangle soup to draw, and the clay renderer that turns it into a thumbnail.
#pragma once
#include "Common.h"

struct Mesh
{
    std::vector<float> positions;   // x y z per vertex
    std::vector<uint32_t> indices;  // three per triangle
    std::vector<float> points;      // a point cloud instead of (or in addition to) triangles
    std::vector<size_t> parts;      // where each object starts in indices (lets the renderer drop floors)

    void BeginPart() { if (parts.empty() || parts.back() != indices.size()) parts.push_back(indices.size()); }

    size_t VertexCount() const { return positions.size() / 3; }
    uint32_t AddVertex(float x, float y, float z)
    {
        positions.push_back(x); positions.push_back(y); positions.push_back(z);
        return uint32_t(positions.size() / 3 - 1);
    }
    // Fan triangulation of a polygon given by vertex indices.
    void AddPolygon(const uint32_t* v, size_t n)
    {
        for (size_t i = 2; i < n; i++) { indices.push_back(v[0]); indices.push_back(v[i - 1]); indices.push_back(v[i]); }
    }
};

enum class UpAxis { Y, Z };

struct RenderOptions
{
    UpAxis up = UpAxis::Y;
    bool weld = false;  // merge equal positions first (STL stores every triangle separately)
};

// Draws the mesh as a gray clay model from a three-quarter view on a transparent background.
bool RenderMesh(Mesh& mesh, const RenderOptions& options, UINT size, Image& out);
