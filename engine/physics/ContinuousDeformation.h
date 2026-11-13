#pragma once
#include "engine/geometry/TriangleMesh.h"
#include <span>
namespace physics {
struct SweptSphere { DirectX::XMFLOAT3 start,end;float radius; };
// Conservative advancement over linearly moving triangle vertices and enclosing spheres.
// Returns a safe fraction of the deformation. A budget-limited query stops early, never skips ahead.
float LimitDeformation(const geometry::TriangleMesh& before,const geometry::TriangleMesh& after,
                       std::span<const SweptSphere> spheres);
}
