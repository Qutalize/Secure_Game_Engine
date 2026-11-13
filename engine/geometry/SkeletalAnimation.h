#pragma once
#include "engine/geometry/TriangleMesh.h"
#include <array>
#include <span>
#include "engine/core/EngineFeatures.h"
namespace geometry {
// Only linked when model_animation is selected. Four-weight CPU skinning lets
// rasterization and both RT backends consume the same deformed positions.
class SkeletalAnimation {
public:
    static SkeletalAnimation Load(const std::filesystem::path& file,const TriangleMesh& mesh);
    TriangleMesh Sample(double seconds,bool loop=true) const;
#if ENGINE_RAGDOLL
    std::vector<DirectX::XMFLOAT4X4> InitialPalette() const;
    TriangleMesh Skin(std::span<const DirectX::XMFLOAT4X4> palette) const;
#endif
#if ENGINE_DEFORMATION_CCD
    double TimeToNextKey(double seconds) const;
#endif
    float Duration() const { return m_duration; }
private:
    struct SkinVertex { DirectX::XMFLOAT3 bind; uint16_t joints[4]; float weights[4]; };
    std::vector<SkinVertex> m_vertices;
    std::vector<DirectX::XMFLOAT4X4> m_matrices;
    std::vector<uint32_t> m_indices;
    uint32_t m_jointCount=0,m_frameCount=0;
    float m_duration=0;
};
}
