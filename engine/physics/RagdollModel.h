#pragma once
#include "engine/geometry/SkeletalAnimation.h"
#include <filesystem>
#include <vector>
namespace physics {
class RigidBodyWorld;
struct RagdollBone {int32_t parent;DirectX::XMFLOAT3 center,size,anchor;};
class RagdollModel {
public:
    static std::vector<RagdollBone> Load(const std::filesystem::path& file,uint32_t paletteCount);
    RagdollModel(RigidBodyWorld& world,const geometry::SkeletalAnimation& animation,
                 const std::vector<RagdollBone>& bones,DirectX::XMFLOAT3 position);
    geometry::TriangleMesh Pose() const;
    const std::vector<uint32_t>& Bodies() const{return m_bodies;}
    float MaxJointError() const;
private:
    RigidBodyWorld& m_world;const geometry::SkeletalAnimation& m_animation;
    std::vector<RagdollBone> m_bones;std::vector<uint32_t> m_bodies;
    std::vector<DirectX::XMFLOAT4X4> m_initial;
};
}
