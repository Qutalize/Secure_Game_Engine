#pragma once
#include "engine/geometry/TriangleMesh.h"
namespace raymesh {
// Explicitly padded to match StructuredBuffer records in RayTracing.hlsli.
struct Triangle {
    DirectX::XMFLOAT3 a;uint32_t pad0=0;
    DirectX::XMFLOAT3 b;uint32_t pad1=0;
    DirectX::XMFLOAT3 c;uint32_t pad2=0;
    DirectX::XMFLOAT3 normal;uint32_t pad3=0;
};
struct Node {DirectX::XMFLOAT3 minimum;uint32_t first=0;DirectX::XMFLOAT3 maximum;uint32_t count=0;};
static_assert(sizeof(Triangle)==64 && sizeof(Node)==32);
struct Acceleration {std::vector<Triangle> triangles;std::vector<Node> nodes;};
Acceleration Build(const geometry::TriangleMesh& mesh);
}
