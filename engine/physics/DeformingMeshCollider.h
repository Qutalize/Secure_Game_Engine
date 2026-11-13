#pragma once
// Internal Jolt bridge, compiled only for ENGINE_MODEL_DEFORMING.
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Physics/Body/BodyID.h>
#include "engine/geometry/TriangleMesh.h"
namespace JPH { class PhysicsSystem; }
namespace physics::deforming {
JPH::RefConst<JPH::Shape> Cook(const geometry::TriangleMesh& mesh);
void ValidatePose(DirectX::XMFLOAT3 p,DirectX::XMFLOAT4 q);
void Replace(JPH::PhysicsSystem& world,JPH::BodyID id,const JPH::Shape* shape,
             DirectX::XMFLOAT3 p,DirectX::XMFLOAT4 q);
bool Ray(const JPH::PhysicsSystem& world,JPH::BodyID id,DirectX::XMFLOAT3 origin,
         DirectX::XMFLOAT3 direction,float maxDistance,float& distance);
DirectX::XMFLOAT3 Move(const JPH::PhysicsSystem& world,JPH::BodyID id,DirectX::XMFLOAT3 center,
                      DirectX::XMFLOAT3 halfExtent,DirectX::XMFLOAT3 delta);
}
