#include "engine/renderer/RayTracing.h"
#include "engine/core/Common.h"
#include "engine/core/Logger.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
using namespace DirectX;
using Microsoft::WRL::ComPtr;
namespace {
ComPtr<ID3D12Resource> Buffer(ID3D12Device* device,UINT64 bytes,D3D12_HEAP_TYPE heap,D3D12_RESOURCE_STATES state,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE){
    D3D12_HEAP_PROPERTIES hp{};hp.Type=heap;hp.CreationNodeMask=hp.VisibleNodeMask=1;
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=(bytes+255)&~UINT64(255);
    desc.Height=1;desc.DepthOrArraySize=1;desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;desc.Flags=flags;
    ComPtr<ID3D12Resource> result;DX_CHECK(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(&result)));return result;
}
void Upload(ID3D12Resource* buffer,const void* data,size_t bytes){void* target=nullptr;D3D12_RANGE read{0,0};DX_CHECK(buffer->Map(0,&read,&target));std::memcpy(target,data,bytes);D3D12_RANGE written{0,bytes};buffer->Unmap(0,&written);}
#if ENGINE_RT_HARDWARE
void UavBarrier(ID3D12GraphicsCommandList* list,ID3D12Resource* resource){D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;b.UAV.pResource=resource;list->ResourceBarrier(1,&b);}
#endif
}
void RayTracingScene::CheckHardware(ID3D12Device* device){
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_5};
    if(FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,&options,sizeof(options))) || options.RaytracingTier<D3D12_RAYTRACING_TIER_1_1 ||
       FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,&model,sizeof(model))) || model.HighestShaderModel<D3D_SHADER_MODEL_6_5)
        throw std::runtime_error("Hardware RT requires DXR Tier 1.1 and Shader Model 6.5; no software fallback was made");
}
#if ENGINE_RT_HARDWARE
RayTracingScene::Geometry RayTracingScene::CreateGeometry(const std::vector<XMFLOAT3>& vertices){
    Geometry g;g.descriptor.Type=D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;g.descriptor.Flags=D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    g.descriptor.Triangles.VertexBuffer.StrideInBytes=sizeof(XMFLOAT3);
    g.descriptor.Triangles.VertexCount=static_cast<UINT>(vertices.size());g.descriptor.Triangles.VertexFormat=DXGI_FORMAT_R32G32B32_FLOAT;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS input{};input.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    input.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;input.NumDescs=1;input.pGeometryDescs=&g.descriptor;input.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};m_device->GetRaytracingAccelerationStructurePrebuildInfo(&input,&info);
    if(!info.ResultDataMaxSizeInBytes)throw std::runtime_error("DXR BLAS prebuild failed");
    for(unsigned frame=0;frame<GeometryFrames;frame++){
        g.vertices[frame]=Buffer(m_device.Get(),vertices.size()*sizeof(XMFLOAT3),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        Upload(g.vertices[frame].Get(),vertices.data(),vertices.size()*sizeof(XMFLOAT3));
        g.blas[frame]=Buffer(m_device.Get(),info.ResultDataMaxSizeInBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        g.scratch[frame]=Buffer(m_device.Get(),info.ScratchDataSizeInBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    }
    return g;
}
#endif
RayTracingScene::RayTracingScene(ID3D12Device* device){
    static_assert(sizeof(PackedInstance)==(ENGINE_RT_MESH?96:80));
    for(auto& buffer:m_constants)buffer=Buffer(device,sizeof(ConstantsData),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
#if ENGINE_RT_MESH
    m_meshDevice=device;
#endif
#if ENGINE_RT_HARDWARE
    CheckHardware(device);DX_CHECK(device->QueryInterface(IID_PPV_ARGS(&m_device)));
    const XMFLOAT3 corners[8]={{-.5f,-.5f,-.5f},{.5f,-.5f,-.5f},{.5f,.5f,-.5f},{-.5f,.5f,-.5f},{-.5f,-.5f,.5f},{.5f,-.5f,.5f},{.5f,.5f,.5f},{-.5f,.5f,.5f}};
    const int faces[6][4]={{0,1,2,3},{5,4,7,6},{4,0,3,7},{1,5,6,2},{3,2,6,7},{4,5,1,0}};
    std::vector<XMFLOAT3> vertices;for(auto& face:faces)for(int i:{0,1,2,0,2,3})vertices.push_back(corners[face[i]]);
    m_geometry.push_back(CreateGeometry(vertices));
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS top{};top.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    top.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;top.NumDescs=Capacity;top.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};m_device->GetRaytracingAccelerationStructurePrebuildInfo(&top,&info);
    if(!info.ResultDataMaxSizeInBytes)throw std::runtime_error("DXR TLAS prebuild failed");
    for(unsigned i=0;i<Frames;++i){
        m_tlas[i]=Buffer(device,info.ResultDataMaxSizeInBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        m_scratch[i]=Buffer(device,info.ScratchDataSizeInBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        m_instances[i]=Buffer(device,Capacity*sizeof(D3D12_RAYTRACING_INSTANCE_DESC),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    }
    LOG_INFO("RAY TRACING backend=dxr-inline tier=1.1 required mesh=%d max-instances=32",ENGINE_RT_MESH);
#else
    LOG_INFO("RAY TRACING backend=software-gpu hardware-rt=0 mesh=%d max-instances=32",ENGINE_RT_MESH);
#endif
}
#if ENGINE_RT_MESH
uint32_t RayTracingScene::RegisterMesh(const geometry::TriangleMesh& mesh){
    if(m_prepared)throw std::runtime_error("RT geometry registration is closed after the first frame");
    if(m_meshes.size()>=31)throw std::runtime_error("RT mesh geometry capacity exceeded");
    auto tree=raymesh::Build(mesh);
    if(m_triangles.size()+tree.triangles.size()>geometry::TriangleMesh::MaxTriangles)throw std::runtime_error("RT scene exceeds 65536 triangles");
    const auto first=static_cast<uint32_t>(m_triangles.size()),root=static_cast<uint32_t>(m_nodes.size());
#if ENGINE_RT_HARDWARE
    // Primitive indices match the reordered BVH triangle array used for normals in HLSL.
    std::vector<XMFLOAT3> vertices;for(auto& t:tree.triangles){vertices.push_back(t.a);vertices.push_back(t.b);vertices.push_back(t.c);}m_geometry.push_back(CreateGeometry(vertices));
#endif
    for(auto& n:tree.nodes)n.first+=n.count?first:root;
    m_triangles.insert(m_triangles.end(),tree.triangles.begin(),tree.triangles.end());m_nodes.insert(m_nodes.end(),tree.nodes.begin(),tree.nodes.end());m_meshes.push_back({first,root,static_cast<uint32_t>(tree.triangles.size()),static_cast<uint32_t>(tree.nodes.size())});
    LOG_INFO("RT MESH registered triangles=%zu nodes=%zu",tree.triangles.size(),tree.nodes.size());return static_cast<uint32_t>(m_meshes.size());
}
#endif
#if ENGINE_RT_MESH && ENGINE_MODEL_ANIMATION
void RayTracingScene::UpdateMesh(uint32_t id,const geometry::TriangleMesh& mesh){
    if(!id||id>m_meshes.size())throw std::runtime_error("Unknown animated RT mesh");
    const auto range=m_meshes[id-1];
    if(mesh.indices.size()!=size_t(range.triangles)*3)throw std::runtime_error("Animated RT triangle count changed");
    auto tree=raymesh::Build(mesh);
    if(tree.nodes.size()!=range.nodes)throw std::runtime_error("Animated RT BVH size changed");
    for(auto& node:tree.nodes)node.first+=node.count?range.firstTriangle:range.rootNode;
    std::copy(tree.triangles.begin(),tree.triangles.end(),m_triangles.begin()+range.firstTriangle);
    std::copy(tree.nodes.begin(),tree.nodes.end(),m_nodes.begin()+range.rootNode);
}
#endif
void RayTracingScene::Set(const std::vector<Instance>& instances){
    if(instances.empty()||instances.size()>Capacity)throw std::runtime_error("RT scene needs 1..32 instances");
    for(auto& instance:instances){
        for(auto& row:instance.world.m)for(float v:row)if(!std::isfinite(v))throw std::runtime_error("RT transform must be finite");
        float determinant=XMVectorGetX(XMMatrixDeterminant(XMLoadFloat4x4(&instance.world)));
        if(!std::isfinite(determinant)||std::abs(determinant)<1e-8f)throw std::runtime_error("RT transform must be invertible");
        if(std::abs(instance.world._14)>1e-6f||std::abs(instance.world._24)>1e-6f||std::abs(instance.world._34)>1e-6f||std::abs(instance.world._44-1)>1e-6f)
            throw std::runtime_error("RT transform must be affine");
#if ENGINE_RT_MESH
        if(instance.geometry>m_meshes.size())throw std::runtime_error("Unknown RT mesh geometry");
#endif
    }
    m_instancesCpu=instances;
}
void RayTracingScene::Prepare(ID3D12GraphicsCommandList* list,unsigned frame){
    if(frame>=Frames||m_instancesCpu.empty())throw std::runtime_error("Invalid RT frame/scene");
#if ENGINE_RT_MESH
    m_meshFrame=DynamicGeometry?frame:0;
    if(!m_prepared){
        // Allocate one immutable set, or a frame-fenced ring for animated geometry.
        if(m_triangles.empty())m_triangles.resize(1);if(m_nodes.empty())m_nodes.resize(1);
        for(unsigned slot=0;slot<GeometryFrames;slot++){
            m_triangleBuffers[slot]=Buffer(m_meshDevice.Get(),m_triangles.size()*sizeof(raymesh::Triangle),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
            m_nodeBuffers[slot]=Buffer(m_meshDevice.Get(),m_nodes.size()*sizeof(raymesh::Node),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        }
    }
    if(!m_prepared||DynamicGeometry){
        Upload(m_triangleBuffers[m_meshFrame].Get(),m_triangles.data(),m_triangles.size()*sizeof(raymesh::Triangle));
        Upload(m_nodeBuffers[m_meshFrame].Get(),m_nodes.data(),m_nodes.size()*sizeof(raymesh::Node));
    }
#endif
    ConstantsData data{};data.count=static_cast<uint32_t>(m_instancesCpu.size());
    for(size_t i=0;i<m_instancesCpu.size();++i){
        XMStoreFloat4x4(&data.instances[i].inverse,XMMatrixInverse(nullptr,XMLoadFloat4x4(&m_instancesCpu[i].world)));data.instances[i].color=m_instancesCpu[i].color;
#if ENGINE_RT_MESH
        auto id=m_instancesCpu[i].geometry;if(id){auto range=m_meshes[id-1];data.instances[i].geometry={1,range.firstTriangle,range.rootNode,0};}
#endif
    }
    Upload(m_constants[frame].Get(),&data,sizeof(data));
#if ENGINE_RT_HARDWARE
    ComPtr<ID3D12GraphicsCommandList4> rt;DX_CHECK(list->QueryInterface(IID_PPV_ARGS(&rt)));
    const unsigned geometryFrame=DynamicGeometry?frame:0;
#if ENGINE_RT_MESH && ENGINE_MODEL_ANIMATION
    for(size_t i=0;i<m_meshes.size();i++){
        auto range=m_meshes[i];std::vector<XMFLOAT3> vertices;vertices.reserve(size_t(range.triangles)*3);
        for(uint32_t j=0;j<range.triangles;j++){const auto& t=m_triangles[range.firstTriangle+j];vertices.push_back(t.a);vertices.push_back(t.b);vertices.push_back(t.c);}
        Upload(m_geometry[i+1].vertices[geometryFrame].Get(),vertices.data(),vertices.size()*sizeof(XMFLOAT3));
    }
#endif
    if(!m_prepared||DynamicGeometry)for(auto& g:m_geometry){
        auto descriptor=g.descriptor;descriptor.Triangles.VertexBuffer.StartAddress=g.vertices[geometryFrame]->GetGPUVirtualAddress();
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};build.Inputs.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        build.Inputs.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;build.Inputs.NumDescs=1;build.Inputs.pGeometryDescs=&descriptor;
        build.Inputs.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        build.DestAccelerationStructureData=g.blas[geometryFrame]->GetGPUVirtualAddress();build.ScratchAccelerationStructureData=g.scratch[geometryFrame]->GetGPUVirtualAddress();
        rt->BuildRaytracingAccelerationStructure(&build,0,nullptr);UavBarrier(list,g.blas[geometryFrame].Get());
    }
    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instances(m_instancesCpu.size());
    for(size_t i=0;i<instances.size();++i){
        XMFLOAT4X4 transposed;XMStoreFloat4x4(&transposed,XMMatrixTranspose(XMLoadFloat4x4(&m_instancesCpu[i].world)));
        std::memcpy(instances[i].Transform,&transposed,sizeof(instances[i].Transform));instances[i].InstanceID=static_cast<UINT>(i);instances[i].InstanceMask=255;
        instances[i].Flags=D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;uint32_t geometry=0;
#if ENGINE_RT_MESH
        geometry=m_instancesCpu[i].geometry;
#endif
        instances[i].AccelerationStructure=m_geometry[geometry].blas[geometryFrame]->GetGPUVirtualAddress();
    }
    Upload(m_instances[frame].Get(),instances.data(),instances.size()*sizeof(instances[0]));
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};build.Inputs.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    build.Inputs.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;build.Inputs.NumDescs=static_cast<UINT>(instances.size());build.Inputs.InstanceDescs=m_instances[frame]->GetGPUVirtualAddress();
    build.Inputs.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    build.DestAccelerationStructureData=m_tlas[frame]->GetGPUVirtualAddress();build.ScratchAccelerationStructureData=m_scratch[frame]->GetGPUVirtualAddress();
    rt->BuildRaytracingAccelerationStructure(&build,0,nullptr);UavBarrier(list,m_tlas[frame].Get());
#endif
    m_prepared=true;
}
D3D12_GPU_VIRTUAL_ADDRESS RayTracingScene::Acceleration(unsigned frame)const{
#if ENGINE_RT_HARDWARE
    return m_tlas.at(frame)->GetGPUVirtualAddress();
#else
    return 0;
#endif
}
