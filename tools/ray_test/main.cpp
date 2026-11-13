#include "engine/renderer/RayTracing.h"
#include "engine/core/Common.h"
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <fstream>
#include <iostream>
#include <cmath>
#include <filesystem>
using Microsoft::WRL::ComPtr;
using namespace DirectX;
constexpr UINT TestCount=ENGINE_RT_MESH?16:8;
ComPtr<ID3D12Resource> Output(ID3D12Device* device,D3D12_HEAP_TYPE heap,D3D12_RESOURCE_STATES state){
    D3D12_HEAP_PROPERTIES h{};h.Type=heap;
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=TestCount*16;d.Height=1;d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if(heap==D3D12_HEAP_TYPE_DEFAULT)d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> out;DX_CHECK(device->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&out)));return out;
}
int main(int argc,char** argv){try{
    ComPtr<IDXGIFactory6> factory;DX_CHECK(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)));
    ComPtr<ID3D12Device> device;ComPtr<IDXGIAdapter1> adapter;
    for(UINT i=0;factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&adapter))!=DXGI_ERROR_NOT_FOUND;++i){
        DXGI_ADAPTER_DESC1 desc{};adapter->GetDesc1(&desc);if(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE){adapter.Reset();continue;}
        if(SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device))))break;adapter.Reset();
    }
    if(!device)throw std::runtime_error("No hardware D3D12 device");
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};DX_CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,&options,sizeof(options)));
    D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_5};DX_CHECK(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,&model,sizeof(model)));
    DXGI_ADAPTER_DESC1 desc{};adapter->GetDesc1(&desc);char name[256]{};WideCharToMultiByte(CP_UTF8,0,desc.Description,-1,name,sizeof(name),nullptr,nullptr);
    std::cout<<"{\"adapter\":\""<<name<<"\",\"raytracing_tier\":"<<options.RaytracingTier<<",\"shader_model\":"<<model.HighestShaderModel<<"}\n";
    if(argc>1&&std::string(argv[1])=="--capabilities")return 0;
    RayTracingScene scene(device.Get());
    std::vector<RayTracingScene::Box> boxes(2);XMStoreFloat4x4(&boxes[0].world,XMMatrixIdentity());boxes[0].color={.2f,.4f,.6f,1};
    XMStoreFloat4x4(&boxes[1].world,XMMatrixScaling(2,1,1)*XMMatrixRotationY(XM_PIDIV2)*XMMatrixTranslation(3,0,0));boxes[1].color={1,0,0,1};
#if ENGINE_RT_MESH
    geometry::TriangleMesh panels;
    for(float left:{-2.f,.5f})for(int x=0;x<2;++x)for(int y=0;y<2;++y){
        uint32_t start=static_cast<uint32_t>(panels.positions.size());float a=left+x*.75f,b=-1.f+y;
        panels.positions.insert(panels.positions.end(),{{a,b,0},{a+.75f,b,0},{a+.75f,b+1,0},{a,b+1,0}});
        panels.indices.insert(panels.indices.end(),{start,start+1,start+2,start,start+2,start+3});
    }
    auto meshId=scene.RegisterMesh(panels);RayTracingScene::Box instance;
    instance.geometry=meshId;instance.color={.8f,.2f,.4f,1};XMStoreFloat4x4(&instance.world,XMMatrixTranslation(10,0,4));boxes.push_back(instance);
    XMStoreFloat4x4(&instance.world,XMMatrixScaling(2,1,1)*XMMatrixRotationY(XM_PIDIV2)*XMMatrixTranslation(16,0,4));boxes.push_back(instance);
    geometry::TriangleMesh sloped;sloped.positions={{-1,-1,0},{1,-1,0},{0,1,1}};sloped.indices={0,1,2};
    auto slopedId=scene.RegisterMesh(sloped);instance.geometry=slopedId;XMStoreFloat4x4(&instance.world,XMMatrixTranslation(20,0,4));boxes.push_back(instance);
#endif
    scene.Set(boxes);
    ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC q{};DX_CHECK(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator;DX_CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> list;DX_CHECK(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));
    D3D12_ROOT_PARAMETER params[2+ENGINE_RT_HARDWARE+2*ENGINE_RT_MESH]{};params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;params[0].Descriptor.ShaderRegister=1;
    params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;params[1].Descriptor.ShaderRegister=0;
#if ENGINE_RT_HARDWARE
    params[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;params[2].Descriptor.ShaderRegister=4;
#endif
#if ENGINE_RT_MESH
    for(UINT i=0;i<2;++i){params[2+ENGINE_RT_HARDWARE+i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;params[2+ENGINE_RT_HARDWARE+i].Descriptor.ShaderRegister=5+i;}
#endif
    D3D12_ROOT_SIGNATURE_DESC rs{};rs.NumParameters=_countof(params);rs.pParameters=params;
    ComPtr<ID3DBlob> blob,error;DX_CHECK(D3D12SerializeRootSignature(&rs,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error));
    ComPtr<ID3D12RootSignature> root;DX_CHECK(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)));
    wchar_t exe[32768]{};GetModuleFileNameW(nullptr,exe,32768);auto path=std::filesystem::path(exe).parent_path().parent_path()/"shaders"/"RayTestCS.dxil";
    std::ifstream file(path,std::ios::binary);std::vector<char> code((std::istreambuf_iterator<char>(file)),{});if(code.empty())throw std::runtime_error("Missing test compute shader");
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};psoDesc.pRootSignature=root.Get();psoDesc.CS={code.data(),code.size()};
    ComPtr<ID3D12PipelineState> pso;DX_CHECK(device->CreateComputePipelineState(&psoDesc,IID_PPV_ARGS(&pso)));
    auto output=Output(device.Get(),D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto readback=Output(device.Get(),D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    constexpr unsigned passes=RayTracingScene::DynamicGeometry?6:1;
    for(unsigned pass=0;pass<passes;pass++){
        if(pass){DX_CHECK(allocator->Reset());DX_CHECK(list->Reset(allocator.Get(),nullptr));
            D3D12_RESOURCE_BARRIER reset{};reset.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            reset.Transition={output.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS};list->ResourceBarrier(1,&reset);
        }
#if ENGINE_RT_MESH && ENGINE_MODEL_ANIMATION
        auto changed=panels;for(auto& p:changed.positions)p.z=pass%2?2.f:0.f;
        scene.UpdateMesh(meshId,changed);
        auto second=sloped;for(auto& p:second.positions)p.z+=float(pass%3)*.25f;
        scene.UpdateMesh(slopedId,second); // Independent animated geometry must keep its own BVH range.
#endif
        const unsigned slot=pass%RayTracingScene::Frames;scene.Prepare(list.Get(),slot);
    list->SetComputeRootSignature(root.Get());list->SetPipelineState(pso.Get());list->SetComputeRootConstantBufferView(0,scene.Constants(slot));list->SetComputeRootUnorderedAccessView(1,output->GetGPUVirtualAddress());
#if ENGINE_RT_HARDWARE
    list->SetComputeRootShaderResourceView(2,scene.Acceleration(slot));
#endif
#if ENGINE_RT_MESH
    list->SetComputeRootShaderResourceView(2+ENGINE_RT_HARDWARE,scene.Triangles());
    list->SetComputeRootShaderResourceView(3+ENGINE_RT_HARDWARE,scene.Nodes());
    bool closed=false;try{scene.RegisterMesh(panels);}catch(const std::runtime_error&){closed=true;}
    if(!closed)throw std::runtime_error("RT registration changed in-flight geometry");
#endif
    list->Dispatch(TestCount/8,1,1);
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition={output.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE};list->ResourceBarrier(1,&barrier);
    list->CopyResource(readback.Get(),output.Get());DX_CHECK(list->Close());ID3D12CommandList* lists[]={list.Get()};queue->ExecuteCommandLists(1,lists);
    ComPtr<ID3D12Fence> fence;DX_CHECK(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));DX_CHECK(queue->Signal(fence.Get(),1));
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)throw std::runtime_error("Fence event failed");
    HRESULT hr=fence->SetEventOnCompletion(1,event);DWORD wait=FAILED(hr)?WAIT_FAILED:WaitForSingleObject(event,30000);CloseHandle(event);DX_CHECK(hr);if(wait!=WAIT_OBJECT_0)throw std::runtime_error("GPU readback timeout");
    XMFLOAT4* values=nullptr;D3D12_RANGE range{0,TestCount*16};DX_CHECK(readback->Map(0,&range,reinterpret_cast<void**>(&values)));
    XMFLOAT4 expected[TestCount]={{2.5f,0,0,-1},{-1,0,0,0},{.5f,1,0,0},{2,0,0,-1},{-1,0,0,0},{0,0,0,0},{.25f,.5f,.75f,1},{1,1,1,1}
#if ENGINE_RT_MESH
        ,{4,0,0,-1},{-1,0,0,0},{4,0,0,1},{4,-1,0,0},{-1,0,0,0},{1,.25f,.5f,1},{1,1,1,1},{4.5f,0,.4472136f,-.8944272f}
#endif
    };
#if ENGINE_RT_MESH && ENGINE_MODEL_ANIMATION
    if(pass%2){expected[8].x=6;expected[10].x=2;expected[11].x=6;}
    expected[15].x+=float(pass%3)*.25f;
#endif
    std::cout<<"frame="<<pass<<" slot="<<slot<<"\n";
    bool good=true;for(UINT i=0;i<TestCount;++i){std::cout<<i<<": "<<values[i].x<<","<<values[i].y<<","<<values[i].z<<","<<values[i].w<<"\n";for(int j=0;j<4;++j){float actual=(&values[i].x)[j];if(!std::isfinite(actual)||std::abs(actual-(&expected[i].x)[j])>.002f)good=false;}}
    D3D12_RANGE none{0,0};readback->Unmap(0,&none);if(!good)throw std::runtime_error("GPU intersection/shadow/reflection mismatch");
    }
    std::cout<<"GPU RAY CHECK OK: hit/miss/inside/rotation/scale/distance/shadow/reflection\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 2;}}
