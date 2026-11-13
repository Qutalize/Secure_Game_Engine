#pragma once
#include "engine/core/EngineFeatures.h"
#include <d3d12.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <array>
#include <vector>
#if ENGINE_RT_MESH
#include "engine/renderer/TriangleAcceleration.h"
#endif
// All instances use exactly the same local geometry and world transform as rasterization.
class RayTracingScene {
public:
    static constexpr unsigned Capacity=32, Frames=3;
    static constexpr bool DynamicGeometry=ENGINE_MODEL_ANIMATION && ENGINE_RT_MESH;
    static constexpr unsigned GeometryFrames=DynamicGeometry?Frames:1;
    struct Instance {
        DirectX::XMFLOAT4X4 world;
        DirectX::XMFLOAT4 color;
#if ENGINE_RT_MESH
        uint32_t geometry=0; // 0 = unit box, 1.. = registered triangle mesh.
#endif
    };
    using Box=Instance; // Retain the existing box-only API.
    explicit RayTracingScene(ID3D12Device* device);
    void Set(const std::vector<Instance>& instances);
    void Prepare(ID3D12GraphicsCommandList* list,unsigned frame);
    D3D12_GPU_VIRTUAL_ADDRESS Constants(unsigned frame)const{return m_constants.at(frame)->GetGPUVirtualAddress();}
    D3D12_GPU_VIRTUAL_ADDRESS Acceleration(unsigned frame)const;
    static void CheckHardware(ID3D12Device* device);
#if ENGINE_RT_MESH
    uint32_t RegisterMesh(const geometry::TriangleMesh& mesh); // Register before the first Prepare.
#if ENGINE_MODEL_ANIMATION
    void UpdateMesh(uint32_t id,const geometry::TriangleMesh& mesh);
#endif
    D3D12_GPU_VIRTUAL_ADDRESS Triangles()const{return m_triangleBuffers[m_meshFrame]->GetGPUVirtualAddress();}
    D3D12_GPU_VIRTUAL_ADDRESS Nodes()const{return m_nodeBuffers[m_meshFrame]->GetGPUVirtualAddress();}
#endif
private:
    struct PackedInstance {
        DirectX::XMFLOAT4X4 inverse;DirectX::XMFLOAT4 color;
#if ENGINE_RT_MESH
        DirectX::XMUINT4 geometry{}; // kind, first triangle, root node, unused
#endif
    };
    struct ConstantsData {uint32_t count=0,padding[3]{};PackedInstance instances[Capacity]{};};
    std::vector<Instance> m_instancesCpu;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>,Frames> m_constants;
    bool m_prepared=false;
#if ENGINE_RT_MESH
    Microsoft::WRL::ComPtr<ID3D12Device> m_meshDevice;
    struct MeshRange {uint32_t firstTriangle,rootNode,triangles,nodes;};
    std::vector<MeshRange> m_meshes;
    std::vector<raymesh::Triangle> m_triangles;
    std::vector<raymesh::Node> m_nodes;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>,GeometryFrames> m_triangleBuffers,m_nodeBuffers;
    unsigned m_meshFrame=0;
#endif
#if ENGINE_RT_HARDWARE
    struct Geometry {
        std::array<Microsoft::WRL::ComPtr<ID3D12Resource>,GeometryFrames> vertices,blas,scratch;
        D3D12_RAYTRACING_GEOMETRY_DESC descriptor{};
    };
    Geometry CreateGeometry(const std::vector<DirectX::XMFLOAT3>& vertices);
    Microsoft::WRL::ComPtr<ID3D12Device5> m_device;
    std::vector<Geometry> m_geometry;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>,Frames> m_instances,m_tlas,m_scratch;
#endif
};
