#pragma once
#include "engine/core/EngineFeatures.h"
#if ENGINE_MODEL_CONVEX || ENGINE_MODEL_STATIC || ENGINE_MODEL_DEFORMING
#include "engine/geometry/TriangleMesh.h"
#endif
#include <DirectXMath.h>
#include <memory>
#include <cstdint>
#include <span>
#include <vector>
namespace physics {
struct RagdollBone;
struct Pose {
    DirectX::XMFLOAT3 position, size;
    DirectX::XMFLOAT4 rotation;
    DirectX::XMFLOAT3 velocity, angularVelocity;
};
// Jolt is hidden behind this optional module. IDs are local to this world.
class RigidBodyWorld {
public:
    RigidBodyWorld();
    ~RigidBodyWorld();
    RigidBodyWorld(const RigidBodyWorld&)=delete;
    RigidBodyWorld& operator=(const RigidBodyWorld&)=delete;
    uint32_t Box(DirectX::XMFLOAT3 center, DirectX::XMFLOAT3 size, float mass=0,
                 float restitution=.2f,float friction=.6f);
#if ENGINE_MODEL_CONVEX
    uint32_t ConvexHull(const geometry::TriangleMesh& mesh,DirectX::XMFLOAT3 position,float mass=1,
                        float restitution=.2f,float friction=.6f);
#endif
#if ENGINE_MODEL_STATIC
    uint32_t StaticMesh(const geometry::TriangleMesh& mesh,DirectX::XMFLOAT3 position,float friction=.6f);
#endif
#if ENGINE_MODEL_DEFORMING
    // Animation controls a static triangle collider; it is not a dynamic ragdoll.
    uint32_t DeformingMesh(const geometry::TriangleMesh& mesh,DirectX::XMFLOAT3 position,
                          DirectX::XMFLOAT4 rotation={0,0,0,1},float friction=.6f);
    void UpdateDeformingMesh(uint32_t id,const geometry::TriangleMesh& mesh,DirectX::XMFLOAT3 position,
                             DirectX::XMFLOAT4 rotation={0,0,0,1});
    bool RayDeformingMesh(uint32_t id,DirectX::XMFLOAT3 origin,DirectX::XMFLOAT3 direction,
                          float maxDistance,float& distance) const;
    DirectX::XMFLOAT3 MoveAgainstDeformingMesh(uint32_t id,DirectX::XMFLOAT3 center,
                                             DirectX::XMFLOAT3 halfExtent,DirectX::XMFLOAT3 delta) const;
    bool IsActive(uint32_t id) const;
#endif
#if ENGINE_RAGDOLL
    void ConnectRagdoll(std::span<const uint32_t> bodies,std::span<const RagdollBone> bones,DirectX::XMFLOAT3 origin);
#endif
#if ENGINE_DEFORMATION_CCD
    geometry::TriangleMesh ConstrainDeformation(uint32_t id,const geometry::TriangleMesh& desired,
        DirectX::XMFLOAT3 position,DirectX::XMFLOAT4 rotation,float seconds,
        DirectX::XMFLOAT3 playerCenter,DirectX::XMFLOAT3 playerHalfExtent) const;
#endif
    void Step(float seconds);
    void Impulse(uint32_t id,DirectX::XMFLOAT3 impulse,DirectX::XMFLOAT3 worldPoint);
    Pose Get(uint32_t id) const;
private:
    struct Impl;std::unique_ptr<Impl> m;
};
void CheckRigidBodies();
#if ENGINE_MODEL_CONVEX || ENGINE_MODEL_STATIC
void CheckModelBodies();
#endif
}
