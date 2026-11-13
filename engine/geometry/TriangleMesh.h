#pragma once
#include <DirectXMath.h>
#include <cstdint>
#include <filesystem>
#include <vector>
namespace geometry {
// Shared local-space, indexed triangles. No rendering or physics dependency.
struct TriangleMesh {
    static constexpr uint32_t MaxVertices=65536, MaxTriangles=65536;
    std::vector<DirectX::XMFLOAT3> positions;
    std::vector<uint32_t> indices;
    void Validate() const;
    static TriangleMesh Load(const std::filesystem::path& file);
};
}
