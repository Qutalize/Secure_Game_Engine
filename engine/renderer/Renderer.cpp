#include "engine/renderer/Renderer.h"
#include "engine/core/EngineFeatures.h"

#include <fstream>
#include <filesystem>
#include <stdexcept>
#include <string>

#include <d3dcompiler.h>   // 実行時 HLSL コンパイル（dxc が無い場合のフォールバック）

#include "engine/core/Common.h"   // DX_CHECK
#include "engine/core/Logger.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
// 小さなヘルパー（d3dx12.h を使わず自前で用意）
// ---------------------------------------------------------------------------
namespace {

D3D12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES p = {};
    p.Type                 = type;
    p.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    p.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    p.CreationNodeMask     = 1;
    p.VisibleNodeMask      = 1;
    return p;
}

D3D12_RESOURCE_DESC BufferDesc(UINT64 bytes) {
    D3D12_RESOURCE_DESC d = {};
    d.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Alignment          = 0;
    d.Width              = bytes;
    d.Height             = 1;
    d.DepthOrArraySize   = 1;
    d.MipLevels          = 1;
    d.Format             = DXGI_FORMAT_UNKNOWN;
    d.SampleDesc.Count   = 1;
    d.SampleDesc.Quality = 0;
    d.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags              = D3D12_RESOURCE_FLAG_NONE;
    return d;
}

// リソースバリア（状態遷移）を 1 件挿入する。
// CLAUDE.md「リソースバリアを必ず書く」に対応。
void Transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* res,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b = {};
    b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    b.Transition.pResource   = res;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter  = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &b);
}

} // namespace

// ---------------------------------------------------------------------------
Renderer::~Renderer() {
    Shutdown();
}

void Renderer::Shutdown() {
    if (!m_initialized) return;
    WaitForGpu(); // GPU が全フレームを描き終えるのを待ってから解放する
    if (m_fenceEvent) {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }
    m_initialized = false;
}

bool Renderer::Initialize(HWND hwnd, int width, int height, bool basicOnly) {
    m_width  = width;
    m_height = height;

    CreateDevice();
#if ENGINE_RAY_TRACING
    m_rayScene = std::make_unique<RayTracingScene>(m_device.Get());
#endif
    CreateCommandObjects();

    // ★フェンスはここで作る（コマンドキュー作成の直後）★
    // 以前はこの下の一連の Create* の後（関数末尾）に置いていたが、L-02 で追加した
    // CreateDummyTextures() が内部で CreateTexture → WaitForGpu() を呼ぶようになった。
    // WaitForGpu() は m_fence が null だと同期をスキップして即座に戻る安全策があるため
    // クラッシュはしないが、その代わり「GPU がまだコピー中のアップロードバッファを
    // CPU 側が解放してしまう」という実際のバグを引き起こす（D3D12 デバッグレイヤーが
    // 検出してプロセスを異常終了させる）。フェンスは最初から作っておき、
    // 以降のどの Create* が GPU 同期を必要としても安全に動くようにする。
    DX_CHECK(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
    m_fenceValues[m_frameIndex] = 1;
    m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent) {
        DX_CHECK(HRESULT_FROM_WIN32(GetLastError()));
    }

    CreateSwapChain(hwnd);
    CreateRenderTargets();
    CreateDepthBuffer();
    CreateRootSignature();
    CreatePipelineState();
    CreateConstantBuffers();
    CreateSrvHeap();               // PROMPT-G-01: テクスチャ SRV ヒープ
#if defined(SELECTED_RUNTIME)
    basicOnly = false;
#if ENGINE_TEXTURE_PIPELINE
    CreateTextureRootSignature();
    CreateTexturePipelineState();
#endif
#if ENGINE_SHADOWS
    CreateShadowMap();
    CreateShadowRootSignature();
    CreateShadowPipelineState();
#else
    m_shadowEnabled = false;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    m_shadowSrvIndex = m_srvCount++;
    m_device->CreateShaderResourceView(nullptr, &srv, m_srvHeap->GetCPUDescriptorHandleForHeapStart());
#endif
#if ENGINE_PBR
    CreateDummyTextures();
    CreatePbrRootSignature();
    CreatePbrPipelineState();
#endif
    LOG_INFO("SELECTED renderer: PBR=%d shadow=%d textures=%d skinning=%d", ENGINE_PBR, ENGINE_SHADOWS, ENGINE_TEXTURES, ENGINE_SKINNING);
#elif defined(GAME_RANGE_ONLY)
    basicOnly = true;
#else
    if (!basicOnly) {
    CreateTextureRootSignature();  // PROMPT-G-01: テクスチャ用ルートシグネチャ
    CreateTexturePipelineState();  // PROMPT-G-01: テクスチャ PSO

    // ステージL-03b: シャドウマップ。
    // ★CreateShadowMap は「SRV ヒープ作成後」かつ「ゲームが CreateTexture を呼ぶ前」に
    //   実行される必要がある（SRV インデックス 0 を確保するため）。順序を変えないこと。
    CreateShadowMap();
    CreateShadowRootSignature();
    CreateShadowPipelineState();
    CreateShadowSkinnedRootSignature();   // L-04b: スキン付きキャラの影
    CreateShadowSkinnedPipelineState();

    // ステージL-02: PBR。ダミーテクスチャを先に作る（マテリアルの欠け埋めに使う）。
    // ※ CreateShadowMap の直後＝ゲームの CreateTexture より前に置くことで、
    //   SRV インデックスの割り当て順が「0=シャドウ / 1=白 / 2=平坦法線 / 3〜=ゲーム」で固定される。
    CreateDummyTextures();
    CreatePbrRootSignature();
    CreatePbrPipelineState();
    CreateSkinnedRootSignature();          // L-04b: GPU スキニング
    CreateSkinnedPipelineState();
    CreateSkinBuffers();
    }
#endif
    if (basicOnly) {
        m_shadowEnabled = false;
        // 影を使わなくてもSimplePSのt1へ有効なnull SRVを束縛する。
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format=DXGI_FORMAT_R32_FLOAT;
        srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels=1;
        m_shadowSrvIndex=m_srvCount++;
        m_device->CreateShaderResourceView(nullptr,&srv,m_srvHeap->GetCPUDescriptorHandleForHeapStart());
        LOG_INFO("Renderer BASIC: Lit+Overlay; shadow/PBR/skinning/textures not initialized");
    }

    // ステージL-03a: 描画リストの領域を先に確保しておく（フレーム中の再確保を避ける）。
    m_drawList.reserve(kMaxDrawsPerFrame);

    // ステージL-00: PSO の組み合わせが揃っているかを検査してログに出す。
    // 「マテリアルを増やしたのに敵用 ALWAYS 版を作り忘れて門番に穴が空く」事故は
    // 描画結果を見ても気付けないので、起動時に必ず声を上げさせる。
#ifdef SELECTED_RUNTIME
    if (!GetPso(MaterialType::Lit, DepthMode::Normal) || !GetPso(MaterialType::Overlay, DepthMode::Disabled))
        throw std::runtime_error("Selected base shaders failed to initialize");
#if ENGINE_PBR
    if (!GetPso(MaterialType::Pbr, DepthMode::Normal)) throw std::runtime_error("Selected PBR failed to initialize");
#endif
#if ENGINE_SHADOWS
    if (!m_shadowPSO) throw std::runtime_error("Selected shadow pipeline failed to initialize");
#endif
#else
    if (!basicOnly) ValidatePsoCache();
#endif

    m_initialized = true;
    LOG_INFO("Renderer initialized (%dx%d, %u frames)", width, height, kFrameCount);
    return true;
}

void Renderer::CreateDevice() {
    UINT factoryFlags = 0;

    // Debug ビルドでのみ検証レイヤーを有効化（CLAUDE.md 必須事項）
#if defined(_DEBUG)
    {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
            debugController->EnableDebugLayer();
            factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
            LOG_INFO("D3D12 debug layer enabled");
        }
    }
#endif

    DX_CHECK(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&m_factory)));

    // ハードウェアアダプタを選んでフィーチャーレベル 12.0 でデバイス作成
    ComPtr<IDXGIAdapter1> adapter;
    bool created = false;
    for (UINT i = 0;
         m_factory->EnumAdapterByGpuPreference(
             i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
             IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND;
         ++i) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue; // WARP は除外

        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0,
                                        IID_PPV_ARGS(&m_device)))) {
            LOG_INFO("D3D12 adapter: %ls (vendor=%04x device=%04x)",desc.Description,desc.VendorId,desc.DeviceId);
            created = true;
            break;
        }
    }
    if (!created) {
        LOG_ERROR("Failed to create D3D12 device at feature level 12.0");
        DX_CHECK(E_FAIL);
    }
}

void Renderer::CreateCommandObjects() {
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type  = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    DX_CHECK(m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_commandQueue)));

    // フレームバッファ数分のコマンドアロケータ
    for (UINT i = 0; i < kFrameCount; ++i) {
        DX_CHECK(m_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&m_commandAllocators[i])));
    }

    // コマンドリストは 1 本。生成直後は記録状態なので Close しておく。
    DX_CHECK(m_device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_commandAllocators[0].Get(), nullptr,
        IID_PPV_ARGS(&m_commandList)));
    DX_CHECK(m_commandList->Close());
}

void Renderer::CreateSwapChain(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC1 scd = {};
    scd.BufferCount      = kFrameCount;             // トリプルバッファ
    scd.Width            = static_cast<UINT>(m_width);
    scd.Height           = static_cast<UINT>(m_height);
    scd.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.SampleDesc.Count = 1;

    ComPtr<IDXGISwapChain1> swap1;
    DX_CHECK(m_factory->CreateSwapChainForHwnd(
        m_commandQueue.Get(), hwnd, &scd, nullptr, nullptr, &swap1));

    // Alt+Enter のフルスクリーン切替は無効化（DX12 では明示制御が無難）
    DX_CHECK(m_factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER));

    DX_CHECK(swap1.As(&m_swapChain));
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
}

void Renderer::CreateRenderTargets() {
    // RTV ヒープ: スワップチェーンのバッファ数ぶん。
    // 根拠 = kFrameCount（バックバッファ 1 つにつき RTV 1 つ）
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
    rtvDesc.NumDescriptors = kFrameCount;
    rtvDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    DX_CHECK(m_device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&m_rtvHeap)));
    m_rtvDescriptorSize =
        m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_CPU_DESCRIPTOR_HANDLE handle =
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kFrameCount; ++i) {
        DX_CHECK(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_renderTargets[i])));
        m_device->CreateRenderTargetView(m_renderTargets[i].Get(), nullptr, handle);
        handle.ptr += m_rtvDescriptorSize;
    }
}

void Renderer::CreateDepthBuffer() {
    // DSV ヒープ: 2 枚ぶん。
    //   index 0 = 本描画の深度バッファ（画面解像度）
    //   index 1 = シャドウマップ（ステージL-03b・2048×2048）
    D3D12_DESCRIPTOR_HEAP_DESC dsvDesc = {};
    dsvDesc.NumDescriptors = 2;
    dsvDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    DX_CHECK(m_device->CreateDescriptorHeap(&dsvDesc, IID_PPV_ARGS(&m_dsvHeap)));

    D3D12_RESOURCE_DESC depthDesc = {};
    depthDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDesc.Width            = static_cast<UINT64>(m_width);
    depthDesc.Height           = static_cast<UINT>(m_height);
    depthDesc.DepthOrArraySize = 1;
    depthDesc.MipLevels        = 1;
    depthDesc.Format           = DXGI_FORMAT_D32_FLOAT;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    depthDesc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clear = {};
    clear.Format               = DXGI_FORMAT_D32_FLOAT;
    clear.DepthStencil.Depth   = 1.0f;
    clear.DepthStencil.Stencil = 0;

    D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    DX_CHECK(m_device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &depthDesc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
        IID_PPV_ARGS(&m_depthBuffer)));

    D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
    dsv.Format        = DXGI_FORMAT_D32_FLOAT;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsv.Flags         = D3D12_DSV_FLAG_NONE;
    m_device->CreateDepthStencilView(
        m_depthBuffer.Get(), &dsv,
        m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
}

void Renderer::CreateRootSignature() {
    // ルートパラメータ 1 個 = ルート CBV（b0）。
    // デスクリプタヒープを介さずに GPU 仮想アドレスを直接渡せるため、
    // メッシュごとに異なる定数バッファオフセットを指定しやすい。
    // ステージL-03b: param1 にシャドウマップ SRV(t1) のテーブルを追加した。
    // これにより SimplePS が影を判定できる。オーバーレイ（HUD）も同じルートシグネチャを
    // 使うが t1 を参照しないだけで問題ない（バインドは EmitDraw で一律に行う）。
    D3D12_DESCRIPTOR_RANGE shadowRange = {};
    shadowRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    shadowRange.NumDescriptors     = 1;
    shadowRange.BaseShaderRegister = 1; // t1
    shadowRange.RegisterSpace      = 0;
    shadowRange.OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[2 + ENGINE_RAY_TRACING + ENGINE_RT_HARDWARE + 2*ENGINE_RT_MESH] = {};
    params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0; // b0
    params[0].Descriptor.RegisterSpace  = 0;
    params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges   = &shadowRange;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

#if ENGINE_RAY_TRACING
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[2].Descriptor.ShaderRegister = 1;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
#if ENGINE_RT_HARDWARE
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[3].Descriptor.ShaderRegister = 4;
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
#endif
#endif

    // 比較サンプラー s1。通常のサンプラーと違い「読んだ深度と基準値を比較して 0/1 を返す」。
    // ハードウェアが 4 テクセル分の比較結果を線形補間するので、これだけで影の縁が少し滑らかになる。
    //   AddressMode=BORDER + BorderColor=OPAQUE_WHITE
    //     → シャドウマップの範囲外は深度 1.0 扱い＝必ず「光が当たる」と判定される。
    //       影の描画範囲の外側が真っ黒になる事故を防ぐための設定。
#if ENGINE_RT_MESH
    for(UINT i=0;i<2;++i){
        auto& p=params[3+ENGINE_RT_HARDWARE+i];p.ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;
        p.Descriptor.ShaderRegister=5+i;p.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    }
#endif
    D3D12_STATIC_SAMPLER_DESC shadowSampler = {};
    shadowSampler.Filter           = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    shadowSampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    shadowSampler.BorderColor      = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    shadowSampler.MaxLOD           = D3D12_FLOAT32_MAX;
    shadowSampler.ShaderRegister   = 1; // s1
    shadowSampler.RegisterSpace    = 0;
    shadowSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters     = _countof(params);
    rsDesc.pParameters       = params;
    rsDesc.NumStaticSamplers = 1;
    rsDesc.pStaticSamplers   = &shadowSampler;
    rsDesc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> blob, error;
    HRESULT hr = D3D12SerializeRootSignature(
        &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
    if (FAILED(hr) && error) {
        LOG_ERROR("RootSignature serialize error: %s",
                  static_cast<const char*>(error->GetBufferPointer()));
    }
    DX_CHECK(hr);

    DX_CHECK(m_device->CreateRootSignature(
        0, blob->GetBufferPointer(), blob->GetBufferSize(),
        IID_PPV_ARGS(&m_rootSignature)));
}

void Renderer::CreatePipelineState() {
    // dxc 生成の .dxil を優先。無ければ実行時に HLSL を vs_5_1/ps_5_1 でコンパイル。
    std::vector<uint8_t> vs = GetShaderBytecode("SimpleVS.dxil", "SimpleVS.hlsl",
                                                "VSMain", "vs_5_1");
    std::vector<uint8_t> ps = GetShaderBytecode("SimplePS.dxil", "SimplePS.hlsl",
                                                "PSMain", "ps_5_1");
    if (vs.empty() || ps.empty()) {
        LOG_ERROR("シェーダーを取得できませんでした（.dxil も .hlsl も見つからない）。");
        DX_CHECK(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
    }

    // 入力レイアウト: POSITION(float3) + NORMAL(float3) + COLOR(float4)
    // オフセットは Vertex 構造体のメンバ順（position=0, normal=12, color=24）に一致。
    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature        = m_rootSignature.Get();
    pso.VS                    = { vs.data(), vs.size() };
    pso.PS                    = { ps.data(), ps.size() };
    pso.InputLayout           = { inputLayout, _countof(inputLayout) };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets      = 1;
    pso.RTVFormats[0]         = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.DSVFormat             = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count      = 1;
    pso.SampleMask            = UINT_MAX;

    // ラスタライザー: カリングなし（両面描画）
    pso.RasterizerState.FillMode              = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode              = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.FrontCounterClockwise = FALSE;
    pso.RasterizerState.DepthClipEnable       = TRUE;

    // ブレンド: 不透明（デフォルト）
    pso.BlendState.AlphaToCoverageEnable  = FALSE;
    pso.BlendState.IndependentBlendEnable = FALSE;
    pso.BlendState.RenderTarget[0].BlendEnable = FALSE;
    pso.BlendState.RenderTarget[0].LogicOpEnable = FALSE;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask =
        D3D12_COLOR_WRITE_ENABLE_ALL;

    // 深度ステンシル: 深度テスト有効（LESS）。3D 描画で前後関係を正しくする。
    pso.DepthStencilState.DepthEnable    = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS;
    pso.DepthStencilState.StencilEnable  = FALSE;

    DX_CHECK(m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_pipelineState)));

    // ウォールハック用 PSO: 深度テスト・深度書き込みを無効化する。
    // これで描いた物体は遮蔽物の有無を無視して常に描画される（透視）。
    pso.DepthStencilState.DepthEnable    = FALSE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    DX_CHECK(m_device->CreateGraphicsPipelineState(
        &pso, IID_PPV_ARGS(&m_pipelineStateNoDepth)));

    // オーバーレイ（2D HUD）用 PSO: 深度無効のまま、ライティング無しの
    // パススルーシェーダーへ差し替える。ルートシグネチャと入力レイアウトは流用。
    std::vector<uint8_t> ovs = GetShaderBytecode("OverlayVS.dxil", "OverlayVS.hlsl",
                                                 "VSMain", "vs_5_1");
    std::vector<uint8_t> ops = GetShaderBytecode("OverlayPS.dxil", "OverlayPS.hlsl",
                                                 "PSMain", "ps_5_1");
    if (!ovs.empty() && !ops.empty()) {
        pso.VS = { ovs.data(), ovs.size() };
        pso.PS = { ops.data(), ops.size() };
        DX_CHECK(m_device->CreateGraphicsPipelineState(
            &pso, IID_PPV_ARGS(&m_overlayPSO)));
    } else {
        LOG_ERROR("オーバーレイシェーダーを取得できませんでした（HUD は描画されません）。");
    }

    // タイプA ウォールハック（段[5]）の効果を first-party 再現するための敵専用 PSO。
    // typeA_wallhack_explained.md：深度バッファの「数値」ではなく「深度判定基準」を
    // ALWAYS にすり替える手口を、エンジン内の正当な描画機能として再現したもの。
    //   DepthEnable=TRUE  … 深度ステージ自体は有効（検知器が DepthFunc を照合できる）
    //   DepthFunc=ALWAYS  … 壁より奥の敵も破棄されず、壁の上に重ねて描かれる（＝透視）
    //   DepthWriteMask=ZERO … 深度バッファは汚さない（後段の通常描画を壊さない）
    // VS/PS は通常のライティング版に戻す（overlay 用に差し替えた pso.VS/PS を上書き）。
    pso.VS = { vs.data(), vs.size() };
    pso.PS = { ps.data(), ps.size() };
    pso.DepthStencilState.DepthEnable    = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pso.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_ALWAYS;
    pso.DepthStencilState.StencilEnable  = FALSE;
    DX_CHECK(m_device->CreateGraphicsPipelineState(
        &pso, IID_PPV_ARGS(&m_enemyDepthAlwaysPSO)));

    // ステージL-00: 生成した PSO をキャッシュへ登録する。以降の描画は個別メンバではなく
    // GetPso(material, depth) で引く。ここに登録し忘れた組み合わせは ValidatePsoCache が
    // 起動時に警告するので、PSO を足したのに描画経路に繋ぎ忘れる事故を検出できる。
    RegisterPso(MaterialType::Lit,     DepthMode::Normal,   m_pipelineState);
    RegisterPso(MaterialType::Lit,     DepthMode::Disabled, m_pipelineStateNoDepth);
    RegisterPso(MaterialType::Lit,     DepthMode::Always,   m_enemyDepthAlwaysPSO);
    if (m_overlayPSO) {
        // オーバーレイは常に深度無効（HUD は最前面）。Normal/Always 版は存在しない。
        RegisterPso(MaterialType::Overlay, DepthMode::Disabled, m_overlayPSO);
    }
}

// ---- ステージL-00: PSO キャッシュの実装 --------------------------------------

void Renderer::RegisterPso(MaterialType material, DepthMode depth,
                           const Microsoft::WRL::ComPtr<ID3D12PipelineState>& pso,
                           bool skinned, bool blend) {
    if (!pso) return; // シェーダー取得失敗などで作れなかった組み合わせは登録しない
    m_psoCache[MakePsoKey(material, depth, skinned, blend)] = pso;
}

ID3D12PipelineState* Renderer::GetPso(MaterialType material, DepthMode depth,
                                      bool skinned, bool blend) const {
    auto it = m_psoCache.find(MakePsoKey(material, depth, skinned, blend));
    return (it != m_psoCache.end()) ? it->second.Get() : nullptr;
}

void Renderer::ValidatePsoCache() const {
    // 「この組み合わせが無いと描画経路か防御に穴が空く」ものを列挙して検査する。
    // required=true のものが欠けているのは致命的（描けない）。
    // required=false は「あれば防御が効く」もので、機能が無効なら欠けていてよい。
    struct Req { MaterialType material; DepthMode depth; bool required; const char* what; };
    const Req reqs[] = {
        { MaterialType::Lit,      DepthMode::Normal,   true,  "Lit/Normal（通常描画）" },
        { MaterialType::Lit,      DepthMode::Disabled, true,  "Lit/Disabled（ignoreDepth）" },
        { MaterialType::Lit,      DepthMode::Always,   true,  "Lit/Always（段[5]再現＋門番の対象）" },
        { MaterialType::Overlay,  DepthMode::Disabled, false, "Overlay/Disabled（HUD）" },
        { MaterialType::Textured, DepthMode::Normal,   false, "Textured/Normal" },
        { MaterialType::Textured, DepthMode::Disabled, false, "Textured/Disabled" },
        { MaterialType::Textured, DepthMode::Always,   false, "Textured/Always（テクスチャ敵の門番対象）" },
        { MaterialType::Pbr,      DepthMode::Normal,   false, "Pbr/Normal（L-02）" },
        { MaterialType::Pbr,      DepthMode::Disabled, false, "Pbr/Disabled（L-02）" },
        { MaterialType::Pbr,      DepthMode::Always,   false, "Pbr/Always（PBR 敵の門番対象）" },
        { MaterialType::PbrSkinned, DepthMode::Normal,   false, "PbrSkinned/Normal（L-04b）" },
        { MaterialType::PbrSkinned, DepthMode::Disabled, false, "PbrSkinned/Disabled（L-04b）" },
        { MaterialType::PbrSkinned, DepthMode::Always,   false, "PbrSkinned/Always（スキン敵の門番対象）" },
    };
    int missing = 0;
    for (const Req& r : reqs) {
        if (GetPso(r.material, r.depth) != nullptr) continue;
        ++missing;
        if (r.required) {
            LOG_ERROR("PSO キャッシュに必須の組み合わせがありません: %s", r.what);
        } else {
            LOG_WARN("PSO キャッシュに %s がありません（該当機能は無効になります）", r.what);
        }
    }
    LOG_INFO("PSO キャッシュ: %zu 個登録（未登録 %d 個）", m_psoCache.size(), missing);
}

// ---- ステージL-00/J-03: 深度モードの門番（一般化版）--------------------------

DepthMode Renderer::ResolveDepthMode(DrawTag tag, DepthMode requested) {
    security::Measure measure(security::Work::DepthGuard,m_enemyDepthGuard && tag==DrawTag::Enemy);
    if (tag != DrawTag::Enemy) {
        return requested; // 敵以外は素通し（壁や床の描画に門番は関係ない）
    }

    // フックが「要求した」深度（＝拒絶される前の値）を記録する。検知器はこれと
    // GetLastEnemyDepthFunc()（拒絶後＝実際に使った値）を比べて、
    //   要求 ALWAYS かつ 実際 ALWAYS → ウォールハック成立(state1)
    //   要求 ALWAYS かつ 実際 LESS   → 門番が拒絶(state2)
    //   要求 LESS                    → 正常(0)
    // を判別する。
    m_lastEnemyDepthRequested = DepthFuncOf(requested);

    // 防御 ON のときは Normal 以外を一切通さない。
    // ★ J-03 の頃は「Always だけを拒絶」する条件式だったが、L-00 で「Normal 以外は全部拒絶」
    //    に反転させた。この向きにしておくと、後から深度モードを追加したときに
    //    「拒絶リストへの追記漏れ」で防御が抜ける事故が起きない（ホワイトリスト方式）。
    if (m_enemyDepthGuard && requested != DepthMode::Normal) {
        return DepthMode::Normal;
    }
    return requested;
}

void Renderer::CreateConstantBuffers() {
    const UINT64 bufSize =
        static_cast<UINT64>(kCbStride) * kMaxCbSlotsPerFrame;
    D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC desc = BufferDesc(bufSize);

    for (UINT i = 0; i < kFrameCount; ++i) {
        DX_CHECK(m_device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&m_constantBuffers[i])));

        // 永続マップ（Upload ヒープなので問題ない）
        D3D12_RANGE noRead{ 0, 0 };
        void* mapped = nullptr;
        DX_CHECK(m_constantBuffers[i]->Map(0, &noRead, &mapped));
        m_cbMapped[i] = static_cast<uint8_t*>(mapped);
    }
}

// ---------------------------------------------------------------------------
// テクスチャ描画（PROMPT-G-01）
// ---------------------------------------------------------------------------
void Renderer::CreateSrvHeap() {
    // shader-visible な CBV_SRV_UAV ヒープ。テクスチャの SRV をここに並べ、
    // DrawTexturedMesh でインデックスからデスクリプタテーブルを束ねる。
    // 容量の根拠 = kMaxTextures（本プロジェクトのテクスチャ枚数の上限）。
    D3D12_DESCRIPTOR_HEAP_DESC desc = {};
    desc.NumDescriptors = kMaxTextures;
    desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    DX_CHECK(m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_srvHeap)));
    m_srvDescriptorSize =
        m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

// ---------------------------------------------------------------------------
// ステージL-03b: シャドウマップ
// ---------------------------------------------------------------------------

#if ENGINE_SHADOWS
void Renderer::CreateShadowMap() {
    // ★重要★ 1 枚のテクスチャを「深度バッファとして書く（DSV）」と
    // 「テクスチャとして読む（SRV）」の両方に使うため、リソース本体の書式は
    // R32_TYPELESS にする。型を決め打ち（D32_FLOAT）で作ると SRV を作れない。
    //   DSV → DXGI_FORMAT_D32_FLOAT（深度として書く）
    //   SRV → DXGI_FORMAT_R32_FLOAT（ただの 1ch 浮動小数テクスチャとして読む）
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width            = kShadowMapSize;
    desc.Height           = kShadowMapSize;
    desc.DepthOrArraySize = 1;
    desc.MipLevels        = 1;
    desc.Format           = DXGI_FORMAT_R32_TYPELESS;
    desc.SampleDesc.Count = 1;
    desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clear = {};
    clear.Format             = DXGI_FORMAT_D32_FLOAT;
    clear.DepthStencil.Depth = 1.0f;

    // 初期状態を PIXEL_SHADER_RESOURCE にしておく理由:
    // シャドウパスは毎フレーム「PSR → DEPTH_WRITE →（描画）→ PSR」と遷移する。
    // 1 フレーム目もこの遷移が成立するよう、開始状態を PSR に合わせておく。
    D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    DX_CHECK(m_device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear,
        IID_PPV_ARGS(&m_shadowMap)));

    // DSV（m_dsvHeap の index 1）
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
    dsv.Format        = DXGI_FORMAT_D32_FLOAT;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsv.Flags         = D3D12_DSV_FLAG_NONE;
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle =
        m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    dsvHandle.ptr += static_cast<SIZE_T>(m_shadowDsvIndex) *
        m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    m_device->CreateDepthStencilView(m_shadowMap.Get(), &dsv, dsvHandle);

    // SRV（m_srvHeap の先頭を確保）。
    // ★この関数は Initialize 内で、ゲームが CreateTexture を呼ぶより前に実行される。
    //   よってシャドウマップの SRV インデックスは必ず 0 になり、以降のテクスチャは 1 番から
    //   割り当てられる。順序に依存しているので、呼び出し位置を動かさないこと。
    m_shadowSrvIndex = m_srvCount++;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format                  = DXGI_FORMAT_R32_FLOAT;
    srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels     = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE srvHandle =
        m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    srvHandle.ptr += static_cast<SIZE_T>(m_shadowSrvIndex) * m_srvDescriptorSize;
    m_device->CreateShaderResourceView(m_shadowMap.Get(), &srv, srvHandle);

    LOG_INFO("シャドウマップ生成: %ux%u (DSV=%u, SRV=%u)",
             kShadowMapSize, kShadowMapSize, m_shadowDsvIndex, m_shadowSrvIndex);
}
#endif


#if ENGINE_SHADOWS
void Renderer::CreateShadowRootSignature() {
    // シャドウパスは色を出さないのでテクスチャもサンプラーも要らない。
    // 必要なのは world / ライト視点の view・proj が入った定数バッファ 1 本だけ。
    // 本描画用のルートシグネチャ（SRV テーブルを要求する）を流用すると、
    // 使いもしないテーブルのバインドを毎回強いられるので、専用に分けている。
    D3D12_ROOT_PARAMETER param = {};
    param.ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    param.Descriptor.ShaderRegister = 0; // b0
    param.ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = 1;
    rsDesc.pParameters   = &param;
    rsDesc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> blob, error;
    HRESULT hr = D3D12SerializeRootSignature(
        &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
    if (FAILED(hr) && error) {
        LOG_ERROR("ShadowRootSignature serialize error: %s",
                  static_cast<const char*>(error->GetBufferPointer()));
    }
    DX_CHECK(hr);
    DX_CHECK(m_device->CreateRootSignature(
        0, blob->GetBufferPointer(), blob->GetBufferSize(),
        IID_PPV_ARGS(&m_shadowRootSig)));
}
#endif


#if ENGINE_SHADOWS
void Renderer::CreateShadowPipelineState() {
    std::vector<uint8_t> vs = GetShaderBytecode("ShadowVS.dxil", "ShadowVS.hlsl",
                                                "VSMain", "vs_5_1");
    if (vs.empty()) {
        LOG_WARN("ShadowVS を取得できませんでした（影は無効になります）。");
        m_shadowEnabled = false;
        return; // 影が無くてもゲームは動く＝致命化しない
    }

    // 入力レイアウトは POSITION だけ。ストライドは頂点バッファビュー側が持つので、
    // Vertex(40B) と TexVertex(32B) の両方をこの 1 つの PSO で描ける（先頭が position で共通）。
    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature        = m_shadowRootSig.Get();
    pso.VS                    = { vs.data(), vs.size() };
    // PS は設定しない（深度専用パス）。色を書かないのでラスタライズ後の処理が軽い。
    pso.InputLayout           = { inputLayout, _countof(inputLayout) };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets      = 0;                      // レンダーターゲット無し
    pso.DSVFormat             = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count      = 1;
    pso.SampleMask            = UINT_MAX;

    pso.RasterizerState.FillMode              = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode              = D3D12_CULL_MODE_NONE; // メッシュが両面描画のため
    pso.RasterizerState.FrontCounterClockwise = FALSE;
    pso.RasterizerState.DepthClipEnable       = TRUE;
    // 傾斜に応じた深度バイアス。シャドウアクネ（自己遮蔽の縞模様）の主対策。
    // ラスタライザ側で傾き分を、シェーダー側（gShadowParams.y）で定数分を吸収する二段構え。
    pso.RasterizerState.SlopeScaledDepthBias  = 2.0f;
    pso.RasterizerState.DepthBias             = 1000;
    pso.RasterizerState.DepthBiasClamp        = 0.0f;

    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = 0; // 色は書かない

    pso.DepthStencilState.DepthEnable    = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS;
    pso.DepthStencilState.StencilEnable  = FALSE;

    DX_CHECK(m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_shadowPSO)));
}
#endif


#if ENGINE_SHADOWS && ENGINE_SKINNING
void Renderer::CreateShadowSkinnedRootSignature() {
    // シャドウパスは色を出さないので、必要なのは
    //   b0 = world/ライト行列（SceneConstants と共通レイアウト）
    //   b1 = ジョイント行列パレット
    // の2つだけ。テクスチャもサンプラーも要らない。
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;

    params[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor.ShaderRegister = 1;
    params[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = _countof(params);
    rsDesc.pParameters   = params;
    rsDesc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> blob, error;
    HRESULT hr = D3D12SerializeRootSignature(
        &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
    if (FAILED(hr)) {
        if (error) {
            LOG_ERROR("ShadowSkinnedRootSignature serialize error: %s",
                      static_cast<const char*>(error->GetBufferPointer()));
        }
        return;
    }
    if (FAILED(m_device->CreateRootSignature(0, blob->GetBufferPointer(),
                                             blob->GetBufferSize(),
                                             IID_PPV_ARGS(&m_shadowSkinnedRootSig)))) {
        LOG_ERROR("シャドウ用スキンルートシグネチャの作成に失敗しました。");
        m_shadowSkinnedRootSig.Reset();
    }
}
#endif


#if ENGINE_SHADOWS && ENGINE_SKINNING
void Renderer::CreateShadowSkinnedPipelineState() {
    if (!m_shadowSkinnedRootSig) return;

    std::vector<uint8_t> vs = GetShaderBytecode("ShadowSkinnedVS.dxil", "ShadowSkinnedVS.hlsl",
                                                "VSMain", "vs_5_1");
    if (vs.empty()) {
        LOG_WARN("ShadowSkinnedVS を取得できませんでした（スキン付きキャラの影は無効）。");
        m_shadowSkinnedRootSig.Reset();
        return;
    }

    // 入力レイアウトは POSITION + BLENDINDICES + BLENDWEIGHT のみ。
    // ストライドは SkinnedPbrVertex（80B）の実サイズをそのまま使う
    // （NORMAL/TANGENT/TEXCOORD は読み飛ばすだけで、ストライドは変えない）。
    D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION",     0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "BLENDINDICES", 0, DXGI_FORMAT_R32G32B32A32_UINT,  0, 40,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "BLENDWEIGHT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 56,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature        = m_shadowSkinnedRootSig.Get();
    pso.VS                    = { vs.data(), vs.size() };
    pso.InputLayout           = { layout, _countof(layout) };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets      = 0;
    pso.DSVFormat             = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count      = 1;
    pso.SampleMask            = UINT_MAX;

    pso.RasterizerState.FillMode              = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode              = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.FrontCounterClockwise = FALSE;
    pso.RasterizerState.DepthClipEnable       = TRUE;
    pso.RasterizerState.SlopeScaledDepthBias  = 2.0f;
    pso.RasterizerState.DepthBias             = 1000;
    pso.RasterizerState.DepthBiasClamp        = 0.0f;

    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;

    pso.DepthStencilState.DepthEnable    = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS;
    pso.DepthStencilState.StencilEnable  = FALSE;

    DX_CHECK(m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_shadowSkinnedPSO)));
}
#endif


#if ENGINE_TEXTURE_PIPELINE
void Renderer::CreateTextureRootSignature() {
    // param0: ルート CBV(b0)（既存と同じ定数バッファ）
    // param1: SRV テーブル(t0)（テクスチャ 1 枚）
    // 静的サンプラー s0（別ヒープ不要。線形フィルタ＋クランプ）
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors     = 1;
    srvRange.BaseShaderRegister = 0; // t0
    srvRange.RegisterSpace      = 0;
    srvRange.OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    // ステージL-03b: param2 にシャドウマップ SRV(t1) のテーブルを追加。
    D3D12_DESCRIPTOR_RANGE shadowRange = {};
    shadowRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    shadowRange.NumDescriptors     = 1;
    shadowRange.BaseShaderRegister = 1; // t1
    shadowRange.RegisterSpace      = 0;
    shadowRange.OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[3] = {};
    params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0; // b0
    params[0].Descriptor.RegisterSpace  = 0;
    params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    params[1].ParameterType   = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges   = &srvRange;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    params[2].ParameterType   = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges   = &shadowRange;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
    samplers[0].Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    // WRAP: 床などでテクスチャをタイル状に繰り返せる。カードは UV 0..1 なので影響なし。
    samplers[0].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
    samplers[0].BorderColor      = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    samplers[0].MaxLOD           = D3D12_FLOAT32_MAX;
    samplers[0].ShaderRegister   = 0; // s0
    samplers[0].RegisterSpace    = 0;
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // シャドウ用の比較サンプラー s1（非テクスチャ側と同じ設定）。
    samplers[1].Filter           = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    samplers[1].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    samplers[1].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    samplers[1].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    samplers[1].ComparisonFunc   = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    samplers[1].BorderColor      = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    samplers[1].MaxLOD           = D3D12_FLOAT32_MAX;
    samplers[1].ShaderRegister   = 1; // s1
    samplers[1].RegisterSpace    = 0;
    samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters     = _countof(params);
    rsDesc.pParameters       = params;
    rsDesc.NumStaticSamplers = _countof(samplers);
    rsDesc.pStaticSamplers   = samplers;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> blob, error;
    HRESULT hr = D3D12SerializeRootSignature(
        &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
    if (FAILED(hr) && error) {
        LOG_ERROR("TexRootSignature serialize error: %s",
                  static_cast<const char*>(error->GetBufferPointer()));
    }
    DX_CHECK(hr);
    DX_CHECK(m_device->CreateRootSignature(
        0, blob->GetBufferPointer(), blob->GetBufferSize(),
        IID_PPV_ARGS(&m_texRootSig)));
}
#endif


#if ENGINE_TEXTURE_PIPELINE
void Renderer::CreateTexturePipelineState() {
    std::vector<uint8_t> vs = GetShaderBytecode("TexturedVS.dxil", "TexturedVS.hlsl",
                                                "VSMain", "vs_5_1");
    std::vector<uint8_t> ps = GetShaderBytecode("TexturedPS.dxil", "TexturedPS.hlsl",
                                                "PSMain", "ps_5_1");
    if (vs.empty() || ps.empty()) {
        LOG_ERROR("テクスチャシェーダーを取得できませんでした（テクスチャ描画は無効）。");
        return; // テクスチャ無しでも他描画は動くように、ここでは致命化しない
    }

    // 入力レイアウト: POSITION(float3)+NORMAL(float3)+TEXCOORD(float2)
    // オフセットは TexVertex のメンバ順（position=0, normal=12, uv=24）に一致。
    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature        = m_texRootSig.Get();
    pso.VS                    = { vs.data(), vs.size() };
    pso.PS                    = { ps.data(), ps.size() };
    pso.InputLayout           = { inputLayout, _countof(inputLayout) };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets      = 1;
    pso.RTVFormats[0]         = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.DSVFormat             = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count      = 1;
    pso.SampleMask            = UINT_MAX;

    pso.RasterizerState.FillMode              = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode              = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.FrontCounterClockwise = FALSE;
    pso.RasterizerState.DepthClipEnable       = TRUE;

    pso.BlendState.RenderTarget[0].RenderTargetWriteMask =
        D3D12_COLOR_WRITE_ENABLE_ALL;

    pso.DepthStencilState.DepthEnable    = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS;
    pso.DepthStencilState.StencilEnable  = FALSE;

    DX_CHECK(m_device->CreateGraphicsPipelineState(
        &pso, IID_PPV_ARGS(&m_texturePSO)));

    // 深度無視版（テクスチャ透視。カード透視チートなどで使う）
    pso.DepthStencilState.DepthEnable    = FALSE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    DX_CHECK(m_device->CreateGraphicsPipelineState(
        &pso, IID_PPV_ARGS(&m_texturePSONoDepth)));

    // ステージL-00: テクスチャ経路の「敵用 ALWAYS 版」。
    // 非テクスチャ側の m_enemyDepthAlwaysPSO と同じ設定（深度ステージは有効のまま
    // 比較関数だけ ALWAYS・深度書き込みはしない）。これを先に用意しておくことで、
    // L-01 以降でテクスチャ付きの敵モデルを描くようになっても
    // 「攻撃側が要求できる状態」と「門番が拒絶すべき状態」が同時に揃う。
    pso.DepthStencilState.DepthEnable    = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pso.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_ALWAYS;
    DX_CHECK(m_device->CreateGraphicsPipelineState(
        &pso, IID_PPV_ARGS(&m_texturePSOAlways)));

    RegisterPso(MaterialType::Textured, DepthMode::Normal,   m_texturePSO);
    RegisterPso(MaterialType::Textured, DepthMode::Disabled, m_texturePSONoDepth);
    RegisterPso(MaterialType::Textured, DepthMode::Always,   m_texturePSOAlways);
}
#endif


#if ENGINE_TEXTURES
Texture Renderer::CreateTexture(UINT width, UINT height, const uint8_t* rgba) {
    Texture tex;
    if (m_srvCount >= kMaxTextures) {
        LOG_ERROR("CreateTexture: SRV ヒープ容量 %u を超過しました。", kMaxTextures);
        return tex;
    }

    // --- 1. DEFAULT ヒープに Texture2D を作る（初期状態 COPY_DEST）---
    D3D12_RESOURCE_DESC td = {};
    td.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width            = width;
    td.Height           = height;
    td.DepthOrArraySize = 1;
    td.MipLevels        = 1;
    td.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags            = D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES defHp = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    DX_CHECK(m_device->CreateCommittedResource(
        &defHp, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&tex.resource)));

    // --- 2. アップロード用フットプリント（行ピッチは 256B アライン）---
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    UINT   numRows   = 0;
    UINT64 rowBytes  = 0;
    UINT64 totalBytes = 0;
    m_device->GetCopyableFootprints(&td, 0, 1, 0, &fp, &numRows, &rowBytes, &totalBytes);

    // --- 3. アップロードバッファへピクセルを行ごとにコピー ---
    ComPtr<ID3D12Resource> upload;
    D3D12_HEAP_PROPERTIES upHp = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC   upDesc = BufferDesc(totalBytes);
    DX_CHECK(m_device->CreateCommittedResource(
        &upHp, D3D12_HEAP_FLAG_NONE, &upDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&upload)));

    uint8_t* mapped = nullptr;
    D3D12_RANGE noRead{ 0, 0 };
    DX_CHECK(upload->Map(0, &noRead, reinterpret_cast<void**>(&mapped)));
    const UINT srcRowBytes = width * 4; // RGBA8
    for (UINT y = 0; y < height; ++y) {
        memcpy(mapped + fp.Offset + static_cast<UINT64>(y) * fp.Footprint.RowPitch,
               rgba + static_cast<UINT64>(y) * srcRowBytes,
               srcRowBytes);
    }
    upload->Unmap(0, nullptr);

    // --- 4. コマンドリストでコピー → PIXEL_SHADER_RESOURCE へ遷移 ---
    // Init 中（フレーム外）に呼ばれる前提。allocator[0] とメインリストを使い、
    // 記録後に GPU 完了まで同期してからアップロードバッファを解放する。
    DX_CHECK(m_commandAllocators[0]->Reset());
    DX_CHECK(m_commandList->Reset(m_commandAllocators[0].Get(), nullptr));

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource        = tex.resource.Get();
    dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource       = upload.Get();
    src.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = fp;
    m_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    Transition(m_commandList.Get(), tex.resource.Get(),
               D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    DX_CHECK(m_commandList->Close());
    ID3D12CommandList* lists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);
    WaitForGpu(); // コピー完了まで待つ（upload はこの後 ComPtr 破棄で安全に解放）

    // --- 5. SRV を発行 ---
    tex.srvIndex = m_srvCount++;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(tex.srvIndex) * m_srvDescriptorSize;
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels     = 1;
    m_device->CreateShaderResourceView(tex.resource.Get(), &srvDesc, cpu);

    tex.valid = true;
    return tex;
}
#endif


#if ENGINE_TEXTURE_PIPELINE
Mesh Renderer::CreateTexturedMesh(const std::vector<TexVertex>& vertices,
                                  const std::vector<uint32_t>& indices) {
    Mesh mesh;
    mesh.indexCount = static_cast<UINT>(indices.size());

    const UINT64 vbBytes = vertices.size() * sizeof(TexVertex);
    const UINT64 ibBytes = indices.size()  * sizeof(uint32_t);
    D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_UPLOAD);

    {
        D3D12_RESOURCE_DESC d = BufferDesc(vbBytes);
        DX_CHECK(m_device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &d,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&mesh.vertexBuffer)));
        void* p = nullptr;
        D3D12_RANGE noRead{ 0, 0 };
        DX_CHECK(mesh.vertexBuffer->Map(0, &noRead, &p));
        memcpy(p, vertices.data(), vbBytes);
        mesh.vertexBuffer->Unmap(0, nullptr);
        mesh.vbv.BufferLocation = mesh.vertexBuffer->GetGPUVirtualAddress();
        mesh.vbv.StrideInBytes  = sizeof(TexVertex);
        mesh.vbv.SizeInBytes    = static_cast<UINT>(vbBytes);
    }
    {
        D3D12_RESOURCE_DESC d = BufferDesc(ibBytes);
        DX_CHECK(m_device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &d,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&mesh.indexBuffer)));
        void* p = nullptr;
        D3D12_RANGE noRead{ 0, 0 };
        DX_CHECK(mesh.indexBuffer->Map(0, &noRead, &p));
        memcpy(p, indices.data(), ibBytes);
        mesh.indexBuffer->Unmap(0, nullptr);
        mesh.ibv.BufferLocation = mesh.indexBuffer->GetGPUVirtualAddress();
        mesh.ibv.Format         = DXGI_FORMAT_R32_UINT;
        mesh.ibv.SizeInBytes    = static_cast<UINT>(ibBytes);
    }
    return mesh;
}
#endif


#if ENGINE_TEXTURE_PIPELINE
void Renderer::DrawTexturedMesh(const Mesh& mesh, const Texture& texture,
                                const XMMATRIX& world, bool ignoreDepth) {
    if (!texture.valid) return; // テクスチャが作れていないときは描かない

    // ステージL-03a: 記録するだけ（PSO の解決・存在確認は RecordDraw が行う）。
    RecordDraw(mesh, world, MaterialType::Textured,
               ignoreDepth ? DepthMode::Disabled : DepthMode::Normal,
               &texture, /*isEnemy=*/false);
}
#endif


Mesh Renderer::CreateMesh(const std::vector<Vertex>& vertices,
                          const std::vector<uint32_t>& indices) {
    Mesh mesh;
    mesh.indexCount = static_cast<UINT>(indices.size());

    const UINT64 vbBytes = vertices.size() * sizeof(Vertex);
    const UINT64 ibBytes = indices.size()  * sizeof(uint32_t);

    D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_UPLOAD);

    // --- 頂点バッファ ---
    {
        D3D12_RESOURCE_DESC d = BufferDesc(vbBytes);
        DX_CHECK(m_device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &d,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&mesh.vertexBuffer)));

        void* p = nullptr;
        D3D12_RANGE noRead{ 0, 0 };
        DX_CHECK(mesh.vertexBuffer->Map(0, &noRead, &p));
        memcpy(p, vertices.data(), vbBytes);
        mesh.vertexBuffer->Unmap(0, nullptr);

        mesh.vbv.BufferLocation = mesh.vertexBuffer->GetGPUVirtualAddress();
        mesh.vbv.StrideInBytes  = sizeof(Vertex);
        mesh.vbv.SizeInBytes    = static_cast<UINT>(vbBytes);
    }

    // --- インデックスバッファ ---
    {
        D3D12_RESOURCE_DESC d = BufferDesc(ibBytes);
        DX_CHECK(m_device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &d,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&mesh.indexBuffer)));

        void* p = nullptr;
        D3D12_RANGE noRead{ 0, 0 };
        DX_CHECK(mesh.indexBuffer->Map(0, &noRead, &p));
        memcpy(p, indices.data(), ibBytes);
        mesh.indexBuffer->Unmap(0, nullptr);

        mesh.ibv.BufferLocation = mesh.indexBuffer->GetGPUVirtualAddress();
        mesh.ibv.Format         = DXGI_FORMAT_R32_UINT;
        mesh.ibv.SizeInBytes    = static_cast<UINT>(ibBytes);
    }

    return mesh;
}

Mesh Renderer::CreateCrosshairMesh() {
    // 画面中央の「＋」を NDC（-1..1）で作る。画面は横長なので x をアスペクトで割って正方化。
    const float aspect = (m_height > 0)
        ? static_cast<float>(m_width) / static_cast<float>(m_height)
        : 16.0f / 9.0f;
    const float L = 0.04f;   // 腕の長さ（NDC-y）
    const float T = 0.006f;  // 腕の太さ（NDC-y）
    const float lx = L / aspect, tx = T / aspect; // x はアスペクト補正
    const XMFLOAT4 col{ 0.15f, 1.0f, 0.35f, 1.0f }; // 明るい緑（視認性重視）
    const XMFLOAT3 nrm{ 0.0f, 0.0f, -1.0f };        // オーバーレイでは未使用

    std::vector<Vertex>   v;
    std::vector<uint32_t> idx;
    auto addQuad = [&](float x0, float y0, float x1, float y1) {
        const uint32_t b = static_cast<uint32_t>(v.size());
        v.push_back(Vertex{ { x0, y0, 0.0f }, nrm, col });
        v.push_back(Vertex{ { x1, y0, 0.0f }, nrm, col });
        v.push_back(Vertex{ { x1, y1, 0.0f }, nrm, col });
        v.push_back(Vertex{ { x0, y1, 0.0f }, nrm, col });
        idx.push_back(b + 0); idx.push_back(b + 1); idx.push_back(b + 2);
        idx.push_back(b + 0); idx.push_back(b + 2); idx.push_back(b + 3);
    };
    addQuad(-tx, -L,  tx,  L); // 縦棒
    addQuad(-lx, -T,  lx,  T); // 横棒
    return CreateMesh(v, idx);
}

void Renderer::DrawOverlay(const Mesh& mesh) {
    // ステージL-03a: 記録するだけ。HUD が最前面に来るのは「ゲームが最後に呼ぶ＝
    // 描画リストの末尾に積まれる＝最後に再生される」ことで保証される（深度も無効）。
    // world 行列は使わない（頂点が既に NDC）が、レコード形式を揃えるため単位行列を入れる。
    RecordDraw(mesh, DirectX::XMMatrixIdentity(), MaterialType::Overlay,
               DepthMode::Disabled, nullptr, /*isEnemy=*/false);
}

void Renderer::BeginFrame(const Camera& camera) {
    // 行列を確定（DrawMesh で使う）
    const float aspect = static_cast<float>(m_width) / static_cast<float>(m_height);
    m_viewMatrix = camera.GetViewMatrix();
    m_projMatrix = camera.GetProjMatrix(XMConvertToRadians(70.0f), aspect, 0.1f, 500.0f);
    m_cameraPos  = camera.GetPosition(); // スペキュラ計算用に視点位置を保持

    // J-04: フレーム鍵を更新（xorshift32）。非0 を保証して「マスク==素の値」を避ける。
    if(m_tagMaskOn) {
        security::Measure measure(security::Work::TagMask);
        m_rngState ^= m_rngState << 13;
        m_rngState ^= m_rngState >> 17;
        m_rngState ^= m_rngState << 5;
        m_frameKey = m_rngState | 0x1u;
    }

    // ステージL-03b: ライト視点の行列を更新する（カメラ位置に追従させるため毎フレーム）。
#if ENGINE_SHADOWS
    UpdateLightMatrix();
#endif

    // ステージL-03a: このフレームの描画リストと各種カウンタをリセットする。
    // clear() は capacity を保持するので、フレーム中に再確保は起きない。
    m_drawList.clear();
    m_drawCount = 0;
    m_cbSlot    = 0;
    m_skinSlot  = 0;   // L-04b: ジョイント行列パレットのスロットもフレーム毎にリセット

    // 現フレーム用アロケータをリセットして記録開始
    DX_CHECK(m_commandAllocators[m_frameIndex]->Reset());
    DX_CHECK(m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(),
                                  m_pipelineState.Get()));

    // ステージL-03a: レンダーターゲットへの遷移・クリア・実際の描画命令は
    // すべて EndFrame 側（BeginMainPass / ReplayDrawList）へ移した。
    // こうしておくと、本描画の“前”にシャドウパス（L-03b）を差し込める。
    // ゲーム側から見た BeginFrame → Draw* → EndFrame という使い方は変わらない。
}

void Renderer::DrawMesh(const Mesh& mesh, const XMMATRIX& world, bool ignoreDepth) {
    // ステージL-03a: ここでは GPU 命令を積まず、描画リストへ記録するだけ。
    // （上限チェックは RecordDraw に集約した）
    // 実際の命令化は EndFrame の ReplayDrawList で行う。
    RecordDraw(mesh, world, MaterialType::Lit,
               ignoreDepth ? DepthMode::Disabled : DepthMode::Normal,
               nullptr, /*isEnemy=*/false);
}

// 敵専用の描画入口。描画インターセプトの継ぎ目を経由する（ステージJ-01/J-04）。
void Renderer::DrawEnemyTagged(const Mesh& mesh, const XMMATRIX& world) {
    // J-04: 継ぎ目に渡す敵の印は「マスクされ得るトークン」。マスク ON だとフレーム鍵で
    // XOR され、素の値(kEnemyTagPlain)と単純比較するフックは敵を識別できなくなる。
    const uint32_t token = EncodeTag(DrawTag::Enemy);
    if (m_drawInterceptor != nullptr) {
        // フックされている：継ぎ目に委ねる。インターセプタが深度モードを決め、
        // SubmitTaggedDraw を呼んで実描画する（J-02）。
        m_drawInterceptor(*this, mesh, world, token, MaterialType::Lit, nullptr, nullptr, 0,
                          m_drawInterceptorCtx);
    } else {
        // 既定（フック無し）：通常の深度(LESS)で描く＝壁で隠れる。
        SubmitTaggedDraw(mesh, world, token, DepthMode::Normal);
    }
}

// 敵専用の描画入口（スキン付き PBR 版）。DrawEnemyTagged と同じ継ぎ目を通すことで、
// Swat 化した敵にもウォールハック実演・防御・段[5]検知がそのまま効く。
#if ENGINE_SKINNING
void Renderer::DrawEnemyTaggedSkinned(const Mesh& mesh, const PbrMaterial& material,
                                      D3D12_GPU_VIRTUAL_ADDRESS skinPaletteAddr,
                                      const XMMATRIX& world) {
    if (skinPaletteAddr == 0) return;
    // アップロードは呼び出し側（UploadSkinPaletteOnce）が1キャラクターにつき1回済ませている
    // 前提（DrawPbrSkinnedMesh と同じ理由）。ここでは継ぎ目にそのアドレスを渡すだけ。
    const uint32_t token = EncodeTag(DrawTag::Enemy);
    if (m_drawInterceptor != nullptr) {
        m_drawInterceptor(*this, mesh, world, token, MaterialType::PbrSkinned, nullptr,
                          &material, skinPaletteAddr, m_drawInterceptorCtx);
    } else {
        SubmitTaggedDraw(mesh, world, token, DepthMode::Normal,
                         MaterialType::PbrSkinned, nullptr, &material, skinPaletteAddr);
    }
}
#endif


void Renderer::SetDrawInterceptor(DrawInterceptor fn, void* ctx) {
    m_drawInterceptor    = fn;
    m_drawInterceptorCtx = ctx;
}

// タグトークンと深度モードを指定して実際に submit する低レベル描画。インターセプタ（フック）も
// 既定経路も、最終的にここを呼ぶ。トークンは内部で復号（J-04）して DrawTag を得る。
// Enemy タグのときは使った深度比較関数を記録する。
void Renderer::SubmitTaggedDraw(const Mesh& mesh, const XMMATRIX& world,
                                uint32_t tagToken, DepthMode mode,
                                MaterialType material, const Texture* texture,
                                const PbrMaterial* pbr, D3D12_GPU_VIRTUAL_ADDRESS skinPaletteAddr) {
    // J-04: エンジンは鍵を知っているのでトークンを正しく復号できる（自分の描画/防御は正常）。
    const DrawTag tag = DecodeTag(tagToken);

    // ステージL-00: 要求値の記録（検知器用）と門番による拒絶を 1 箇所に集約した。
    // 以前は「Always だけを拒絶」する条件がこの関数に直書きされていたが、深度モードや
    // マテリアルが増えると書き漏らしやすいので ResolveDepthMode へ切り出してある。
    //
    // ★ステージL-03a の重要な設計判断★
    // 描画そのものは遅延させるが、「門番による拒絶」と「段[5]検知用の記録」は
    // ここ（ゲームがドローを要求した瞬間）で確定させる。遅延させると、フックが
    // 深度を要求したタイミングと検知器が読むタイミングがずれて検知が壊れる。
    // つまり「防御と検知は即時、描画は遅延」という非対称な設計にしてある。
    mode = ResolveDepthMode(tag, mode);

    if (tag == DrawTag::Enemy) {
        // 門番の後＝実際にバインドされる値。GetRequestedEnemyDepthFunc() と比べて
        // 「拒絶が起きたか」を検知器が判定する。
        m_lastEnemyDepthFunc = DepthFuncOf(mode);
    }

    RecordDraw(mesh, world, material, mode,
               (material == MaterialType::Textured) ? texture : nullptr,
               /*isEnemy=*/tag == DrawTag::Enemy,
               pbr, skinPaletteAddr);
}

// 後方互換：深度モードを bool で直指定する直接描画（フック非経由）。
void Renderer::DrawEnemyMesh(const Mesh& mesh, const XMMATRIX& world, bool wallhack) {
    SubmitTaggedDraw(mesh, world, EncodeTag(DrawTag::Enemy),
                     wallhack ? DepthMode::Always : DepthMode::Normal);
}

// ---------------------------------------------------------------------------
// ステージL-02: PBR
// ---------------------------------------------------------------------------

#if ENGINE_TEXTURES
UINT Renderer::AllocSrvSlots(UINT count) {
    if (m_srvCount + count > kMaxTextures) {
        LOG_ERROR("SRV ヒープが足りません（要求 %u / 残り %u）。kMaxTextures を増やしてください。",
                  count, kMaxTextures - m_srvCount);
        return UINT_MAX;
    }
    const UINT first = m_srvCount;
    m_srvCount += count;
    return first;
}
#endif


#if ENGINE_TEXTURES
void Renderer::WriteSrvAt(ID3D12Resource* resource, UINT slot) {
    if (resource == nullptr || !m_srvHeap) return;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels     = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(slot) * m_srvDescriptorSize;
    m_device->CreateShaderResourceView(resource, &srv, h);
}
#endif


#if ENGINE_PBR
void Renderer::CreateDummyTextures() {
    // マテリアルの枠が欠けているときに差し込む「何もしない値」のテクスチャ。
    //   白       … baseColor に使えば係数がそのまま出る / ORM に使えば AO=1・粗さ1・金属1
    //   平坦法線 … (0.5, 0.5, 1.0) = 接空間で真上 = 法線マップ無しと同じ
    const uint8_t white[4] = { 255, 255, 255, 255 };
    const uint8_t flatN[4] = { 128, 128, 255, 255 };
    m_dummyWhite  = CreateTexture(1, 1, white);
    m_dummyNormal = CreateTexture(1, 1, flatN);
}
#endif


#if ENGINE_PBR
void Renderer::CreatePbrRootSignature() {
    // param0: CBV(b0)
    // param1: SRV テーブル t0..t2（baseColor / normal / ORM の 3 枚・ヒープ上で連続必須）
    // param2: SRV テーブル t3（シャドウマップ）
    // static sampler s0（線形 WRAP）/ s1（比較サンプラー）
    D3D12_DESCRIPTOR_RANGE matRange = {};
    matRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    matRange.NumDescriptors     = 3;
    matRange.BaseShaderRegister = 0; // t0..t2
    matRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE shadowRange = {};
    shadowRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    shadowRange.NumDescriptors     = 1;
    shadowRange.BaseShaderRegister = 3; // t3
    shadowRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[3 + ENGINE_RAY_TRACING + ENGINE_RT_HARDWARE + 2*ENGINE_RT_MESH] = {};
    params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges   = &matRange;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges   = &shadowRange;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

#if ENGINE_RAY_TRACING
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[3].Descriptor.ShaderRegister = 1;
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
#if ENGINE_RT_HARDWARE
    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[4].Descriptor.ShaderRegister = 4;
    params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
#endif
#endif

#if ENGINE_RT_MESH
    for(UINT i=0;i<2;++i){
        auto& p=params[4+ENGINE_RT_HARDWARE+i];p.ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;
        p.Descriptor.ShaderRegister=5+i;p.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    }
#endif
    D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
    samplers[0].Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplers[0].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
    samplers[0].MaxLOD           = D3D12_FLOAT32_MAX;
    samplers[0].ShaderRegister   = 0;
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    samplers[1].Filter           = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    samplers[1].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    samplers[1].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    samplers[1].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    samplers[1].ComparisonFunc   = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    samplers[1].BorderColor      = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    samplers[1].MaxLOD           = D3D12_FLOAT32_MAX;
    samplers[1].ShaderRegister   = 1;
    samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters     = _countof(params);
    rsDesc.pParameters       = params;
    rsDesc.NumStaticSamplers = _countof(samplers);
    rsDesc.pStaticSamplers   = samplers;
    rsDesc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> blob, error;
    HRESULT hr = D3D12SerializeRootSignature(
        &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
    if (FAILED(hr)) {
        if (error) {
            LOG_ERROR("PbrRootSignature serialize error: %s",
                      static_cast<const char*>(error->GetBufferPointer()));
        }
        return; // PBR 無しでも他は動くので致命化しない
    }
    if (FAILED(m_device->CreateRootSignature(0, blob->GetBufferPointer(),
                                             blob->GetBufferSize(),
                                             IID_PPV_ARGS(&m_pbrRootSig)))) {
        LOG_ERROR("PBR ルートシグネチャの作成に失敗しました。");
        m_pbrRootSig.Reset();
    }
}
#endif


#if ENGINE_PBR
void Renderer::CreatePbrPipelineState() {
    if (!m_pbrRootSig) return;

    std::vector<uint8_t> vs = GetShaderBytecode("PbrVS.dxil", "PbrVS.hlsl", "VSMain", "vs_5_1");
    std::vector<uint8_t> ps = GetShaderBytecode("PbrPS.dxil", "PbrPS.hlsl", "PSMain", "ps_5_1");
    if (vs.empty() || ps.empty()) {
        LOG_WARN("PBR シェーダーを取得できませんでした（PBR 描画は無効・従来経路で描画します）。");
        m_pbrRootSig.Reset();   // IsPbrAvailable() が false になる
        return;
    }

    // 入力レイアウト: POSITION(12) + NORMAL(12) + TANGENT(16) + TEXCOORD(8) = 48B
    // オフセットは PbrVertex のメンバ順と一致させること。
    D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TANGENT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 40,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature        = m_pbrRootSig.Get();
    pso.VS                    = { vs.data(), vs.size() };
    pso.PS                    = { ps.data(), ps.size() };
    pso.InputLayout           = { layout, _countof(layout) };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets      = 1;
    pso.RTVFormats[0]         = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.DSVFormat             = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count      = 1;
    pso.SampleMask            = UINT_MAX;

    pso.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    pso.DepthStencilState.DepthEnable    = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS;
    DX_CHECK(m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_pbrPSO)));

    pso.DepthStencilState.DepthEnable    = FALSE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    DX_CHECK(m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_pbrPSONoDepth)));

    // 敵用 ALWAYS 版。L-00 の方針どおり、マテリアルを増やしたら必ず
    // 「攻撃側が要求できる状態」と「門番が拒絶すべき状態」を同時に用意する。
    pso.DepthStencilState.DepthEnable    = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pso.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_ALWAYS;
    DX_CHECK(m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_pbrPSOAlways)));

    RegisterPso(MaterialType::Pbr, DepthMode::Normal,   m_pbrPSO);
    RegisterPso(MaterialType::Pbr, DepthMode::Disabled, m_pbrPSONoDepth);
    RegisterPso(MaterialType::Pbr, DepthMode::Always,   m_pbrPSOAlways);
}
#endif


#if ENGINE_PBR
Mesh Renderer::CreatePbrMesh(const std::vector<PbrVertex>& vertices,
                             const std::vector<uint32_t>& indices) {
    Mesh mesh;
    if (vertices.empty() || indices.empty()) return mesh;

    const size_t vbBytes = vertices.size() * sizeof(PbrVertex);
    D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC vbDesc = BufferDesc(vbBytes);
    DX_CHECK(m_device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &vbDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&mesh.vertexBuffer)));
    {
        void* p = nullptr;
        D3D12_RANGE noRead{ 0, 0 };
        DX_CHECK(mesh.vertexBuffer->Map(0, &noRead, &p));
        memcpy(p, vertices.data(), vbBytes);
        mesh.vertexBuffer->Unmap(0, nullptr);
    }
    mesh.vbv.BufferLocation = mesh.vertexBuffer->GetGPUVirtualAddress();
    mesh.vbv.StrideInBytes  = sizeof(PbrVertex);
    mesh.vbv.SizeInBytes    = static_cast<UINT>(vbBytes);

    const size_t ibBytes = indices.size() * sizeof(uint32_t);
    D3D12_RESOURCE_DESC ibDesc = BufferDesc(ibBytes);
    DX_CHECK(m_device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &ibDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&mesh.indexBuffer)));
    {
        void* p = nullptr;
        D3D12_RANGE noRead{ 0, 0 };
        DX_CHECK(mesh.indexBuffer->Map(0, &noRead, &p));
        memcpy(p, indices.data(), ibBytes);
        mesh.indexBuffer->Unmap(0, nullptr);
    }
    mesh.ibv.BufferLocation = mesh.indexBuffer->GetGPUVirtualAddress();
    mesh.ibv.Format         = DXGI_FORMAT_R32_UINT;
    mesh.ibv.SizeInBytes    = static_cast<UINT>(ibBytes);
    mesh.indexCount         = static_cast<UINT>(indices.size());
    return mesh;
}
#endif


#if ENGINE_PBR
PbrMaterial Renderer::CreatePbrMaterial(const Texture* baseColor,
                                        const Texture* normalMap,
                                        const Texture* orm) {
    PbrMaterial mat;
    if (!m_srvHeap) return mat;

    // ★連続 3 スロットを確保する★
    // デスクリプタテーブルは「ヒープ上で隣り合った N 個」を指す仕組みなので、
    // 3 枚がバラバラの位置にあると 1 つのテーブルで渡せない。
    // テクスチャ本体（ID3D12Resource）は共有したまま、SRV デスクリプタだけを
    // 連続位置に 3 つ書き直すことで解決する（同じリソースを複数スロットから参照してよい）。
    const UINT first = AllocSrvSlots(3);
    if (first == UINT_MAX) return mat; // ヒープ不足

    auto pick = [&](const Texture* t, const Texture& fallback) -> const Texture& {
        return (t != nullptr && t->valid) ? *t : fallback;
    };
    const Texture& bc = pick(baseColor, m_dummyWhite);
    const Texture& nm = pick(normalMap, m_dummyNormal);
    const Texture& om = pick(orm,       m_dummyWhite);

    WriteSrvAt(bc.resource.Get(), first + 0);
    WriteSrvAt(nm.resource.Get(), first + 1);
    WriteSrvAt(om.resource.Get(), first + 2);

    mat.baseColor     = bc;
    mat.normal        = nm;
    mat.orm           = om;
    mat.srvTableIndex = first;
    // baseColor が本物のときだけ「テクスチャあり」。全部ダミーなら PS は
    // テクスチャサンプルを丸ごとスキップし、係数だけで描く（無駄なサンプルを省く）。
    mat.hasTextures   = (baseColor != nullptr && baseColor->valid);
    mat.valid         = true;
    return mat;
}
#endif


#if ENGINE_PBR
void Renderer::DrawPbrMesh(const Mesh& mesh, const PbrMaterial& material,
                           const XMMATRIX& world, bool ignoreDepth) {
    if (!material.valid) return;
    RecordDraw(mesh, world, MaterialType::Pbr,
               ignoreDepth ? DepthMode::Disabled : DepthMode::Normal,
               nullptr, /*isEnemy=*/false, &material);
}
#endif


// ---------------------------------------------------------------------------
// ステージL-04b: GPU スキニング
// ---------------------------------------------------------------------------

#if ENGINE_SKINNING
void Renderer::CreateSkinBuffers() {
    // フレームごとに 1 本、kMaxSkinnedDrawsPerFrame 個ぶんのジョイント行列パレット領域を
    // 永続 Map したアップロードバッファとして確保する（SceneConstants と同じ考え方）。
    const UINT64 bufSize =
        static_cast<UINT64>(kSkinPaletteStride) * kMaxSkinnedDrawsPerFrame;
    D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC desc = BufferDesc(bufSize);

    for (UINT i = 0; i < kFrameCount; ++i) {
        DX_CHECK(m_device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&m_skinBuffers[i])));
        D3D12_RANGE noRead{ 0, 0 };
        DX_CHECK(m_skinBuffers[i]->Map(0, &noRead,
                                       reinterpret_cast<void**>(&m_skinMapped[i])));
    }
}
#endif


#if ENGINE_SKINNING
D3D12_GPU_VIRTUAL_ADDRESS Renderer::UploadSkinPaletteOnce(
    const std::vector<XMFLOAT4X4>& joints) {
    if (joints.empty() || !m_skinBuffers[m_frameIndex]) return 0;
    if (m_skinSlot >= kMaxSkinnedDrawsPerFrame) {
        LOG_WARN("UploadSkinPaletteOnce: フレームあたりの上限 %u 体を超過"
                 "（呼び出し側がパーツごとに呼んでいないか確認。1 キャラクターにつき 1 回のはず）",
                 kMaxSkinnedDrawsPerFrame);
        return 0;
    }

    const UINT slot   = m_skinSlot++;
    const UINT offset = slot * kSkinPaletteStride;
    uint8_t* dst = m_skinMapped[m_frameIndex] + offset;

    const size_t count = (std::min)(joints.size(), static_cast<size_t>(kMaxJoints));
    for (size_t i = 0; i < count; ++i) {
        // 他の行列と同じく転置してアップロードする（HLSL が mul(vector, matrix) 順のため）。
        XMFLOAT4X4 t;
        XMStoreFloat4x4(&t, XMMatrixTranspose(XMLoadFloat4x4(&joints[i])));
        memcpy(dst + i * 64, &t, 64);
    }
    // 実際のボーン数が 128 未満の場合、残りは単位行列で埋める。
    // シェーダー側は頂点が参照するボーン番号でしかアクセスしないので本来は不要だが、
    // 未初期化メモリを読ませないための安全策。
    if (count < kMaxJoints) {
        XMFLOAT4X4 id;
        XMStoreFloat4x4(&id, XMMatrixIdentity());
        for (size_t i = count; i < kMaxJoints; ++i) {
            memcpy(dst + i * 64, &id, 64);
        }
    }

    return m_skinBuffers[m_frameIndex]->GetGPUVirtualAddress() + offset;
}
#endif


#if ENGINE_SKINNING
void Renderer::CreateSkinnedRootSignature() {
    // PBR ルートシグネチャに「ジョイント行列パレット(b1)」を追加しただけの構成。
    //   param0: CBV(b0) SceneConstants
    //   param1: SRV テーブル t0-t2（マテリアル）
    //   param2: SRV テーブル t3（シャドウ）
    //   param3: CBV(b1) SkinningConstants（ルート CBV・直接バインド）
    D3D12_DESCRIPTOR_RANGE matRange = {};
    matRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    matRange.NumDescriptors     = 3;
    matRange.BaseShaderRegister = 0;
    matRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE shadowRange = {};
    shadowRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    shadowRange.NumDescriptors     = 1;
    shadowRange.BaseShaderRegister = 3;
    shadowRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[4] = {};
    params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges   = &matRange;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges   = &shadowRange;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    params[3].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[3].Descriptor.ShaderRegister = 1; // b1
    params[3].ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
    samplers[0].Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplers[0].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
    samplers[0].MaxLOD           = D3D12_FLOAT32_MAX;
    samplers[0].ShaderRegister   = 0;
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    samplers[1].Filter           = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    samplers[1].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    samplers[1].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    samplers[1].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    samplers[1].ComparisonFunc   = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    samplers[1].BorderColor      = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    samplers[1].MaxLOD           = D3D12_FLOAT32_MAX;
    samplers[1].ShaderRegister   = 1;
    samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters     = _countof(params);
    rsDesc.pParameters       = params;
    rsDesc.NumStaticSamplers = _countof(samplers);
    rsDesc.pStaticSamplers   = samplers;
    rsDesc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> blob, error;
    HRESULT hr = D3D12SerializeRootSignature(
        &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
    if (FAILED(hr)) {
        if (error) {
            LOG_ERROR("SkinnedRootSignature serialize error: %s",
                      static_cast<const char*>(error->GetBufferPointer()));
        }
        return;
    }
    if (FAILED(m_device->CreateRootSignature(0, blob->GetBufferPointer(),
                                             blob->GetBufferSize(),
                                             IID_PPV_ARGS(&m_pbrSkinnedRootSig)))) {
        LOG_ERROR("スキン用ルートシグネチャの作成に失敗しました。");
        m_pbrSkinnedRootSig.Reset();
    }
}
#endif


#if ENGINE_SKINNING
void Renderer::CreateSkinnedPipelineState() {
    if (!m_pbrSkinnedRootSig) return;

    // 頂点シェーダーはスキン専用(PbrSkinnedVS)、ピクセルシェーダーは PBR と共有(PbrPS)。
    // ライティング計算は変形後のワールド法線に対して行うだけなので、
    // スキンの有無で PS 側のロジックは一切変わらない。
    std::vector<uint8_t> vs = GetShaderBytecode("PbrSkinnedVS.dxil", "PbrSkinnedVS.hlsl",
                                                "VSMain", "vs_5_1");
    std::vector<uint8_t> ps = GetShaderBytecode("PbrPS.dxil", "PbrPS.hlsl", "PSMain", "ps_5_1");
    if (vs.empty() || ps.empty()) {
        LOG_WARN("スキン用シェーダーを取得できませんでした（スキンドアニメーションは無効）。");
        m_pbrSkinnedRootSig.Reset();
        return;
    }

    // 入力レイアウト: POSITION(12)+NORMAL(12)+TANGENT(16)+BLENDINDICES(16)+BLENDWEIGHT(16)+TEXCOORD(8) = 80B
    D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION",     0, DXGI_FORMAT_R32G32B32_FLOAT,     0,  0,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",       0, DXGI_FORMAT_R32G32B32_FLOAT,     0, 12,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TANGENT",      0, DXGI_FORMAT_R32G32B32A32_FLOAT,  0, 24,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "BLENDINDICES", 0, DXGI_FORMAT_R32G32B32A32_UINT,   0, 40,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "BLENDWEIGHT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT,  0, 56,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD",     0, DXGI_FORMAT_R32G32_FLOAT,        0, 72,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature        = m_pbrSkinnedRootSig.Get();
    pso.VS                    = { vs.data(), vs.size() };
    pso.PS                    = { ps.data(), ps.size() };
    pso.InputLayout           = { layout, _countof(layout) };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets      = 1;
    pso.RTVFormats[0]         = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.DSVFormat             = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count      = 1;
    pso.SampleMask            = UINT_MAX;

    pso.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    pso.DepthStencilState.DepthEnable    = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS;
    DX_CHECK(m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_pbrSkinnedPSO)));

    pso.DepthStencilState.DepthEnable    = FALSE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    DX_CHECK(m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_pbrSkinnedPSONoDepth)));

    // L-00 の方針どおり、マテリアルを増やしたら敵用 ALWAYS 版も同時に用意する
    // （このマテリアルの門番対象を最初から漏らさないため）。
    pso.DepthStencilState.DepthEnable    = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pso.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_ALWAYS;
    DX_CHECK(m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_pbrSkinnedPSOAlways)));

    RegisterPso(MaterialType::PbrSkinned, DepthMode::Normal,   m_pbrSkinnedPSO);
    RegisterPso(MaterialType::PbrSkinned, DepthMode::Disabled, m_pbrSkinnedPSONoDepth);
    RegisterPso(MaterialType::PbrSkinned, DepthMode::Always,   m_pbrSkinnedPSOAlways);
}
#endif


#if ENGINE_SKINNING
Mesh Renderer::CreateSkinnedPbrMesh(const std::vector<SkinnedPbrVertex>& vertices,
                                    const std::vector<uint32_t>& indices) {
    Mesh mesh;
    if (vertices.empty() || indices.empty()) return mesh;

    const size_t vbBytes = vertices.size() * sizeof(SkinnedPbrVertex);
    D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC vbDesc = BufferDesc(vbBytes);
    DX_CHECK(m_device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &vbDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&mesh.vertexBuffer)));
    {
        void* p = nullptr;
        D3D12_RANGE noRead{ 0, 0 };
        DX_CHECK(mesh.vertexBuffer->Map(0, &noRead, &p));
        memcpy(p, vertices.data(), vbBytes);
        mesh.vertexBuffer->Unmap(0, nullptr);
    }
    mesh.vbv.BufferLocation = mesh.vertexBuffer->GetGPUVirtualAddress();
    mesh.vbv.StrideInBytes  = sizeof(SkinnedPbrVertex);
    mesh.vbv.SizeInBytes    = static_cast<UINT>(vbBytes);

    const size_t ibBytes = indices.size() * sizeof(uint32_t);
    D3D12_RESOURCE_DESC ibDesc = BufferDesc(ibBytes);
    DX_CHECK(m_device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &ibDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&mesh.indexBuffer)));
    {
        void* p = nullptr;
        D3D12_RANGE noRead{ 0, 0 };
        DX_CHECK(mesh.indexBuffer->Map(0, &noRead, &p));
        memcpy(p, indices.data(), ibBytes);
        mesh.indexBuffer->Unmap(0, nullptr);
    }
    mesh.ibv.BufferLocation = mesh.indexBuffer->GetGPUVirtualAddress();
    mesh.ibv.Format         = DXGI_FORMAT_R32_UINT;
    mesh.ibv.SizeInBytes    = static_cast<UINT>(ibBytes);
    mesh.indexCount         = static_cast<UINT>(indices.size());
    return mesh;
}
#endif


#if ENGINE_SKINNING
void Renderer::DrawPbrSkinnedMesh(const Mesh& mesh, const PbrMaterial& material,
                                  D3D12_GPU_VIRTUAL_ADDRESS skinPaletteAddr,
                                  const XMMATRIX& world, bool ignoreDepth) {
    if (!material.valid || !IsSkinningAvailable() || skinPaletteAddr == 0) return;

    // アップロードは呼び出し側（UploadSkinPaletteOnce）が済ませている。ここで再アップロード
    // しないのは、1 キャラクターが複数パーツに分かれるとき全パーツで同じアドレスを
    // 使い回すため（パーツごとに再アップロードすると 1 フレームの上限をキャラ1体で消費する）。
    // シャドウパスと本描画パスも同じ DrawRecord（＝同じ GPU アドレス）を再生する。
    RecordDraw(mesh, world, MaterialType::PbrSkinned,
               ignoreDepth ? DepthMode::Disabled : DepthMode::Normal,
               nullptr, /*isEnemy=*/false, &material, skinPaletteAddr);
}
#endif


// ---- ステージL-03a: 描画のディファード化 -------------------------------------

void Renderer::RecordDraw(const Mesh& mesh, const XMMATRIX& world,
                          MaterialType material, DepthMode depth,
                          const Texture* texture, bool isEnemy,
                          const PbrMaterial* pbr,
                          D3D12_GPU_VIRTUAL_ADDRESS skinPaletteAddr) {
    if (m_drawCount >= kMaxDrawsPerFrame) {
        LOG_WARN("RecordDraw: フレームあたりの最大描画数 %u を超過", kMaxDrawsPerFrame);
        return;
    }
    // PSO の存在確認は「記録時」に行う。再生時まで遅らせると、どのゲームコードが
    // 出した描画で失敗したのかログから追えなくなるため。
    if (GetPso(material, depth) == nullptr) {
        LOG_WARN("RecordDraw: PSO 未登録（material=%d depth=%d）＝描画をスキップ",
                 static_cast<int>(material), static_cast<int>(depth));
        return;
    }
    if (material == MaterialType::PbrSkinned && skinPaletteAddr == 0) {
        LOG_WARN("RecordDraw: PbrSkinned だがジョイントパレットが空＝描画をスキップ");
        return;
    }

    DrawRecord rec;
    rec.mesh     = &mesh;
    XMStoreFloat4x4(&rec.world, world);
    rec.material = material;
    rec.depth    = depth;
    rec.texture  = texture;
    rec.pbr      = pbr;
    rec.skinPaletteAddr = skinPaletteAddr;
    rec.isEnemy  = isEnemy;
    m_drawList.push_back(rec);
    ++m_drawCount;
}

#if ENGINE_SHADOWS
void Renderer::UpdateLightMatrix() {
    // 平行光源には「位置」が無い（無限遠から一定方向に降る光）。
    // だがシャドウマップを描くには視点と投影範囲が要るので、
    // 「カメラの足元を中心に、光の来る方向へ後退した仮想の視点」を毎フレーム作る。
    //
    //        光の進む向き gLightDir
    //          ＼
    //           ＼  eye（仮想のライト視点）
    //            ＼ ↓
    //   ──────────●────────── center（カメラの足元）
    //
    // カメラに追従させることで、広いマップでも常にプレイヤー周辺に影が出る。
    // 代償として視点が動くとシャドウマップのテクセル境界がずれ、影の縁が
    // ちらつく（シャドウシマリング）。テクセル単位のスナップで軽減できるが、
    // 現状は未実装（L-06 以降で必要になれば対応する）。
    XMVECTOR dir = XMVector3Normalize(
        XMVectorSet(m_lightDir.x, m_lightDir.y, m_lightDir.z, 0.0f));

    // カメラの足元（Y=0 の地面上）を影の中心にする。
    XMVECTOR center = XMVectorSet(m_cameraPos.x, 0.0f, m_cameraPos.z, 1.0f);

    // 光源方向へ後退した位置に仮想の視点を置く。
    const float distance = 80.0f;
    XMVECTOR eye = XMVectorSubtract(center, XMVectorScale(dir, distance));

    // 上方向ベクトル。光がほぼ真下向きだと (0,1,0) と平行になって
    // ビュー行列が作れなくなるので、その場合だけ別の軸に逃がす。
    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    if (fabsf(XMVectorGetX(XMVector3Dot(dir, up))) > 0.99f) {
        up = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
    }

    XMMATRIX lightView = XMMatrixLookAtLH(eye, center, up);
    // 平行光源なので透視投影ではなく正射影（遠近感の無い平行な投影）を使う。
    XMMATRIX lightProj = XMMatrixOrthographicLH(
        m_shadowHalfExtent * 2.0f, m_shadowHalfExtent * 2.0f,
        1.0f, distance * 2.0f);

    XMStoreFloat4x4(&m_lightViewProj, lightView * lightProj);
}
#endif


#if ENGINE_SHADOWS
void Renderer::RenderShadowPass() {
    if (!IsShadowEnabled()) return;

    // シャドウマップを「読める状態」から「深度として書ける状態」へ遷移。
    Transition(m_commandList.Get(), m_shadowMap.Get(),
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_DEPTH_WRITE);

    // ビューポートはシャドウマップの解像度（画面解像度とは無関係）。
    D3D12_VIEWPORT vp{ 0.0f, 0.0f,
                       static_cast<float>(kShadowMapSize),
                       static_cast<float>(kShadowMapSize), 0.0f, 1.0f };
    D3D12_RECT scissor{ 0, 0,
                        static_cast<LONG>(kShadowMapSize),
                        static_cast<LONG>(kShadowMapSize) };
    m_commandList->RSSetViewports(1, &vp);
    m_commandList->RSSetScissorRects(1, &scissor);

    D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    dsv.ptr += static_cast<SIZE_T>(m_shadowDsvIndex) *
        m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    // レンダーターゲット無し・深度だけ。色を一切書かないので高速。
    m_commandList->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
    m_commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // ライト視点用の定数を組み立てる土台（world だけ各レコードで差し替える）。
    // ShadowVS/ShadowSkinnedVS は mul(mul(wp, gView), gProj) を計算するので、
    // gView にライトのビュー×射影を、gProj に単位行列を入れれば同じ結果になる。
    // （ライト行列を 2 つに分けて持つ必要がなく、定数バッファの構成も変えずに済む）
    SceneConstants cb{};
    XMStoreFloat4x4(&cb.view, XMMatrixTranspose(XMLoadFloat4x4(&m_lightViewProj)));
    XMStoreFloat4x4(&cb.proj, XMMatrixTranspose(XMMatrixIdentity()));

    // ルートシグネチャ／PSO はレコードが変わるたびに切り替わり得るので、直前の状態を
    // 覚えておいて同じなら SetGraphicsRootSignature 等を呼び直さない（無駄な状態変更を避ける）。
    bool skinnedBound = false, plainBound = false;

    for (const DrawRecord& rec : m_drawList) {
        // 影を落とさないものを除外する:
        //   Overlay  … HUD（2D なので影の概念が無い）
        //   Disabled … 空・照準・エフェクト系（深度を無視して描く＝実体が無いもの）
        // 敵は深度モードが Always（ウォールハック成立中）でも影は普通に落とさせる。
        // 影だけ消えると「透視していることが画面から分かってしまう」ため、
        // チートの見え方を素直に保つ意味でもこの扱いが正しい。
        if (rec.material == MaterialType::Overlay) continue;
        if (rec.depth    == DepthMode::Disabled)   continue;
        if (rec.mesh == nullptr) continue;

        const bool isSkinned = (rec.material == MaterialType::PbrSkinned);
        if (isSkinned && (!m_shadowSkinnedPSO || rec.skinPaletteAddr == 0)) {
            continue; // スキン用シャドウが使えない環境／パレット無し＝このレコードは影を落とさない
        }

        if (isSkinned) {
            if (!skinnedBound) {
                m_commandList->SetGraphicsRootSignature(m_shadowSkinnedRootSig.Get());
                m_commandList->SetPipelineState(m_shadowSkinnedPSO.Get());
                skinnedBound = true; plainBound = false;
            }
        } else {
            if (!plainBound) {
                m_commandList->SetGraphicsRootSignature(m_shadowRootSig.Get());
                m_commandList->SetPipelineState(m_shadowPSO.Get());
                plainBound = true; skinnedBound = false;
            }
        }

        if (m_cbSlot >= kMaxCbSlotsPerFrame) {
            LOG_WARN("RenderShadowPass: 定数バッファスロット上限 %u を超過", kMaxCbSlotsPerFrame);
            break;
        }
        XMStoreFloat4x4(&cb.world, XMMatrixTranspose(XMLoadFloat4x4(&rec.world)));

        const UINT slot   = m_cbSlot++;
        const UINT offset = slot * kCbStride;
        memcpy(m_cbMapped[m_frameIndex] + offset, &cb, sizeof(SceneConstants));
        m_commandList->SetGraphicsRootConstantBufferView(
            0, m_constantBuffers[m_frameIndex]->GetGPUVirtualAddress() + offset);

        // ★このアドレスは本描画パスと共通★（RecordDraw 時に 1 回だけアップロード済み）。
        if (isSkinned) {
            m_commandList->SetGraphicsRootConstantBufferView(1, rec.skinPaletteAddr);
        }

        m_commandList->IASetVertexBuffers(0, 1, &rec.mesh->vbv);
        m_commandList->IASetIndexBuffer(&rec.mesh->ibv);
        m_commandList->DrawIndexedInstanced(rec.mesh->indexCount, 1, 0, 0, 0);
    }

    // 書き終えたので「ピクセルシェーダーから読める状態」へ戻す。
    // このバリアを忘れると本描画パスで未定義の内容を読むことになる。
    Transition(m_commandList.Get(), m_shadowMap.Get(),
               D3D12_RESOURCE_STATE_DEPTH_WRITE,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}
#endif


void Renderer::BeginMainPass() {
    // ビューポート / シザー
    D3D12_VIEWPORT vp{ 0.0f, 0.0f,
                       static_cast<float>(m_width), static_cast<float>(m_height),
                       0.0f, 1.0f };
    D3D12_RECT scissor{ 0, 0, m_width, m_height };
    m_commandList->RSSetViewports(1, &vp);
    m_commandList->RSSetScissorRects(1, &scissor);

    // バックバッファを PRESENT → RENDER_TARGET に遷移
    Transition(m_commandList.Get(), m_renderTargets[m_frameIndex].Get(),
               D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv =
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(m_frameIndex) * m_rtvDescriptorSize;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv =
        m_dsvHeap->GetCPUDescriptorHandleForHeapStart();

    m_commandList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

    // クリア（紺色）＋深度クリア
    const float clearColor[] = { 0.1f, 0.1f, 0.2f, 1.0f };
    m_commandList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
    m_commandList->ClearDepthStencilView(
        dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    // テクスチャ SRV ヒープを束ねる（テクスチャ描画で使う。非テクスチャ描画にも無害）。
    // shader-visible ヒープは 1 種類につき 1 つだけ束ねられる（CBV_SRV_UAV = m_srvHeap）。
    if (m_srvHeap) {
        ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
        m_commandList->SetDescriptorHeaps(1, heaps);
    }

    m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void Renderer::ReplayDrawList() {
    // 記録順にそのまま再生する。ゲームが呼んだ順序＝描画順序なので、
    // 従来（即時描画）と結果は完全に一致する（HUD が最後、など）。
    for (const DrawRecord& rec : m_drawList) {
        EmitDraw(rec);
    }
}

void Renderer::EmitDraw(const DrawRecord& rec) {
    ID3D12PipelineState* pso = GetPso(rec.material, rec.depth);
    if (!pso || rec.mesh == nullptr) return; // 記録時に検査済みだが念のため

    // マテリアルに応じたルートシグネチャを毎回明示する。
    // （テクスチャ描画と非テクスチャ描画が交互に来ても壊れないようにするため）
    const bool textured = (rec.material == MaterialType::Textured);
    const bool isSkinned = (rec.material == MaterialType::PbrSkinned);
    const bool isPbr    = (rec.material == MaterialType::Pbr) || isSkinned;

    ID3D12RootSignature* rootSig = m_rootSignature.Get();
    if (isSkinned)          rootSig = m_pbrSkinnedRootSig.Get();
    else if (rec.material == MaterialType::Pbr) rootSig = m_pbrRootSig.Get();
    else if (textured)      rootSig = m_texRootSig.Get();
    if (rootSig == nullptr) return;

    m_commandList->SetGraphicsRootSignature(rootSig);
    m_commandList->SetPipelineState(pso);

    auto srvGpu = [&](UINT index) {
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += static_cast<UINT64>(index) * m_srvDescriptorSize;
        return gpu;
    };

    // ステージL-02/L-04b: PBR（スキン有無問わず）はマテリアルの 3 枚組を param1 に束ねる。
    // PBR と PbrSkinned はルートシグネチャの param0-2 が同じレイアウトなので共通に書ける。
    if (isPbr && m_srvHeap && rec.pbr != nullptr) {
        m_commandList->SetGraphicsRootDescriptorTable(1, srvGpu(rec.pbr->srvTableIndex));
    }

    // ステージL-03b: シャドウマップ SRV を束ねる。
    // ルートシグネチャがテーブルを宣言している以上、そのドローがシャドウを読まなくても
    // （＝オーバーレイでも）バインドしておく。未バインドのまま描くとデバッグレイヤーが警告する。
    // テーブルの位置はルートシグネチャごとに違う:
    //   非テクスチャ = param1 / テクスチャ = param2 / PBR・PbrSkinned = param2
    if (m_srvHeap) {
        const UINT shadowParam = (textured || isPbr) ? 2u : 1u;
        m_commandList->SetGraphicsRootDescriptorTable(shadowParam, srvGpu(m_shadowSrvIndex));
    }

    // ステージL-04b: ジョイント行列パレット（param3・ルート CBV）。
    // RecordDraw 時点で存在確認済みなので、ここでは 0 でないことだけ前提にできる。
    if (isSkinned) {
        m_commandList->SetGraphicsRootConstantBufferView(3, rec.skinPaletteAddr);
    }

#if ENGINE_RAY_TRACING
    const UINT rayParameter = isPbr ? 3u : 2u;
    m_commandList->SetGraphicsRootConstantBufferView(rayParameter, m_rayScene->Constants(m_frameIndex));
#if ENGINE_RT_HARDWARE
    m_commandList->SetGraphicsRootShaderResourceView(rayParameter+1, m_rayScene->Acceleration(m_frameIndex));
#endif
#endif
#if ENGINE_RT_MESH
    m_commandList->SetGraphicsRootShaderResourceView(rayParameter+1+ENGINE_RT_HARDWARE,m_rayScene->Triangles());
    m_commandList->SetGraphicsRootShaderResourceView(rayParameter+2+ENGINE_RT_HARDWARE,m_rayScene->Nodes());
#endif
    WriteCbAndDraw(*rec.mesh, XMLoadFloat4x4(&rec.world), rec.texture, rec.pbr);
}

void Renderer::WriteCbAndDraw(const Mesh& mesh, const XMMATRIX& world,
                              const Texture* texture, const PbrMaterial* pbr) {
    // 定数バッファへ書き込み（HLSL は column-major なので転置してアップロード）
    SceneConstants cb;
    XMStoreFloat4x4(&cb.view,  XMMatrixTranspose(m_viewMatrix));
    XMStoreFloat4x4(&cb.proj,  XMMatrixTranspose(m_projMatrix));
    XMStoreFloat4x4(&cb.world, XMMatrixTranspose(world));

    // ライティング情報（毎ドロー同じ値だが、定数バッファは 1 スロットに全部入れる）
    XMFLOAT4 normalizedDir;
    XMStoreFloat4(&normalizedDir,
                  XMVector3Normalize(XMVectorSet(m_lightDir.x, m_lightDir.y, m_lightDir.z, 0.0f)));
    cb.lightDir   = normalizedDir;
    cb.lightColor = m_lightColor;
    cb.ambient    = m_ambient;
    // cameraPos.w にフォグ有効フラグ（1=フォグ / 0=フォグ無し）を載せる（PS が距離減衰に使う）。
    cb.cameraPos  = XMFLOAT4{ m_cameraPos.x, m_cameraPos.y, m_cameraPos.z,
                              m_fogEnabled ? 1.0f : 0.0f };

    // ステージL-03b: 影の判定に必要な情報。
    //   shadowParams.x = 1/解像度（PCF で隣のテクセルへ 1 歩動く量）
    //   shadowParams.y = 深度バイアス
    //   shadowParams.z = 有効フラグ。0 のとき PS は影の計算を丸ごとスキップする＝
    //     シャドウ用シェーダーが読めなかった環境でも従来どおりの絵になる（安全側に倒す）。
    // ※ 他の行列と同じく転置してアップロードすること（HLSL 側が mul(vector, matrix) 順のため）。
    //   ここを転置し忘れると影だけが明後日の方向に出る、という追いにくいバグになる。
    XMStoreFloat4x4(&cb.lightViewProj,
                    XMMatrixTranspose(XMLoadFloat4x4(&m_lightViewProj)));
    cb.shadowParams  = XMFLOAT4{ 1.0f / static_cast<float>(kShadowMapSize),
                                 m_shadowBias,
                                 IsShadowEnabled() ? 1.0f : 0.0f,
                                 0.0f };

    // ステージL-02: PBR マテリアルの係数。PBR 以外の描画では使われないが、
    // 定数バッファのレイアウトは共通なので常に妥当な値を入れておく。
    if (pbr != nullptr) {
        cb.baseColorFactor = pbr->baseColorFactor;
        cb.materialParams  = XMFLOAT4{ pbr->metallicFactor, pbr->roughnessFactor,
                                       pbr->normalScale,
                                       pbr->hasTextures ? 1.0f : 0.0f };
    } else {
        cb.baseColorFactor = XMFLOAT4{ 1.0f, 1.0f, 1.0f, 1.0f };
        cb.materialParams  = XMFLOAT4{ 0.0f, 1.0f, 1.0f, 0.0f };
    }

    // ステージL-03a: 定数バッファのスロットは「描画件数」ではなく専用カウンタで数える。
    // 同じ描画レコードがシャドウパスと本描画パスで 2 回再生されると、行列が異なる
    // 2 つの定数が要る＝2 スロット消費するため、件数と一致しなくなる。
    if (m_cbSlot >= kMaxCbSlotsPerFrame) {
        LOG_WARN("WriteCbAndDraw: 定数バッファスロット上限 %u を超過", kMaxCbSlotsPerFrame);
        return;
    }
    const UINT slot   = m_cbSlot++;
    const UINT offset = slot * kCbStride;
    memcpy(m_cbMapped[m_frameIndex] + offset, &cb, sizeof(SceneConstants));

    D3D12_GPU_VIRTUAL_ADDRESS cbAddr =
        m_constantBuffers[m_frameIndex]->GetGPUVirtualAddress() + offset;
    m_commandList->SetGraphicsRootConstantBufferView(0, cbAddr);

    // ステージL-00: テクスチャ経路のときは SRV をデスクリプタテーブル(param1)へ束ねる。
    // ※ このバインドはテクスチャ用ルートシグネチャ(m_texRootSig)が設定済みであることが前提。
    //   呼び出し側（DrawTexturedMesh / SubmitTaggedDraw）が必ず先に設定している。
    if (texture != nullptr && texture->valid && m_srvHeap) {
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += static_cast<UINT64>(texture->srvIndex) * m_srvDescriptorSize;
        m_commandList->SetGraphicsRootDescriptorTable(1, gpu);
    }

    m_commandList->IASetVertexBuffers(0, 1, &mesh.vbv);
    m_commandList->IASetIndexBuffer(&mesh.ibv);
    m_commandList->DrawIndexedInstanced(mesh.indexCount, 1, 0, 0, 0);
}

void Renderer::EndFrame() {
#if ENGINE_RAY_TRACING
    m_rayScene->Prepare(m_commandList.Get(), m_frameIndex);
#endif
    // ステージL-03a/L-03b: ここでフレーム分の描画をまとめて GPU 命令に変換する。
    // 同じ描画リストを 2 回再生する:
    //   1 回目 = ライト視点・深度だけ → シャドウマップ
    //   2 回目 = カメラ視点・通常描画 → バックバッファ（シャドウマップを参照して影を付ける）
#if ENGINE_SHADOWS
    RenderShadowPass();
#endif
    BeginMainPass();
    ReplayDrawList();

    // RENDER_TARGET → PRESENT に戻す
    Transition(m_commandList.Get(), m_renderTargets[m_frameIndex].Get(),
               D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);

    DX_CHECK(m_commandList->Close());
    ID3D12CommandList* lists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);

    DX_CHECK(m_swapChain->Present(1, 0)); // VSync あり
    MoveToNextFrame();
}

void Renderer::SetDirectionalLight(const XMFLOAT3& dir, const XMFLOAT3& color,
                                   float intensity, const XMFLOAT3& ambient) {
    m_lightDir   = XMFLOAT4{ dir.x, dir.y, dir.z, 0.0f };       // 正規化は DrawMesh で行う
    m_lightColor = XMFLOAT4{ color.x, color.y, color.z, intensity };
    m_ambient    = XMFLOAT4{ ambient.x, ambient.y, ambient.z, 0.0f };
}

void Renderer::MoveToNextFrame() {
    // 現フレームの完了マーカーをキューに積む
    const UINT64 currentValue = m_fenceValues[m_frameIndex];
    DX_CHECK(m_commandQueue->Signal(m_fence.Get(), currentValue));

    // 次フレームへ
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    // 次フレームのバックバッファがまだ GPU 使用中なら待つ
    if (m_fence->GetCompletedValue() < m_fenceValues[m_frameIndex]) {
        DX_CHECK(m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex], m_fenceEvent));
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }

    m_fenceValues[m_frameIndex] = currentValue + 1;
}

void Renderer::WaitForGpu() {
    if (!m_commandQueue || !m_fence) return;
    const UINT64 value = m_fenceValues[m_frameIndex];
    DX_CHECK(m_commandQueue->Signal(m_fence.Get(), value));
    DX_CHECK(m_fence->SetEventOnCompletion(value, m_fenceEvent));
    WaitForSingleObject(m_fenceEvent, INFINITE);
    m_fenceValues[m_frameIndex] = value + 1;
}

std::vector<uint8_t> Renderer::LoadShaderBlob(const char* fileName) {
    // 事前コンパイル済み .dxil の候補パスを順に試す。
    // 配布先を最優先し、開発用パスはフォールバックにする。
    wchar_t exePath[32768]{};
    GetModuleFileNameW(nullptr,exePath,32768);
    const std::filesystem::path candidates[] = {
        std::filesystem::path(exePath).parent_path()/"shaders",
#ifndef PROTECTED_DISTRIBUTION
#ifdef SHADER_OUTPUT_DIR
        SHADER_OUTPUT_DIR,
#endif
        "shaders/",
        "../shaders/",
        "",
#endif
    };

    for (const auto& dir : candidates) {
        auto path = dir / fileName;
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f) continue;
        std::streamsize size = f.tellg();
        f.seekg(0, std::ios::beg);
        std::vector<uint8_t> blob(static_cast<size_t>(size));
        if (f.read(reinterpret_cast<char*>(blob.data()), size)) {
            LOG_INFO("Loaded precompiled shader %s (%lld bytes)", path.string().c_str(),
                     static_cast<long long>(size));
            return blob;
        }
    }
    return {}; // 見つからなければ呼び出し側が実行時コンパイルへフォールバックする
}

Microsoft::WRL::ComPtr<ID3DBlob> Renderer::CompileHlsl(
    const char* hlslName, const char* entry, const char* target) {
    // HLSL ソースの候補パス
    const char* dirs[] = {
#ifdef SHADER_SRC_DIR
        SHADER_SRC_DIR "/",
#endif
        "assets/shaders/",
        "../assets/shaders/",
        "shaders/",
        "",
    };

    for (const char* dir : dirs) {
        std::string path = std::string(dir) + hlslName;
        // 存在確認
        std::ifstream probe(path, std::ios::binary);
        if (!probe) continue;
        probe.close();

        // narrow パス -> wide（UTF-8 として変換）
        int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
        std::wstring wpath(static_cast<size_t>(wlen), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), wlen);

        UINT flags = 0;
#if defined(_DEBUG)
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
        ComPtr<ID3DBlob> code, error;
        HRESULT hr = D3DCompileFromFile(
            wpath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
            entry, target, flags, 0, &code, &error);
        if (FAILED(hr)) {
            if (error) {
                LOG_ERROR("HLSL コンパイルエラー (%s): %s", hlslName,
                          static_cast<const char*>(error->GetBufferPointer()));
            } else {
                LOG_ERROR("HLSL コンパイル失敗 (%s): 0x%08X", hlslName,
                          static_cast<unsigned>(hr));
            }
            return nullptr;
        }
        LOG_INFO("Compiled %s at runtime (%s, %zu bytes)", path.c_str(), target,
                 code->GetBufferSize());
        return code;
    }

    LOG_ERROR("HLSL ソース %s が見つかりません。", hlslName);
    return nullptr;
}

std::vector<uint8_t> Renderer::GetShaderBytecode(
    const char* dxilName, const char* hlslName,
    const char* entry, const char* target) {
    // まず dxc 生成の .dxil を探す
    std::vector<uint8_t> blob = LoadShaderBlob(dxilName);
    if (!blob.empty()) return blob;
#ifdef PROTECTED_DISTRIBUTION
    throw std::runtime_error(std::string("Required packaged shader missing: ")+dxilName);
#else
    // 無ければ実行時コンパイル（DXBC）にフォールバック
    LOG_WARN("%s が探索先に無いため %s を実行時コンパイルします。", dxilName, hlslName);
    ComPtr<ID3DBlob> compiled = CompileHlsl(hlslName, entry, target);
    if (compiled) {
        const uint8_t* p = static_cast<const uint8_t*>(compiled->GetBufferPointer());
        blob.assign(p, p + compiled->GetBufferSize());
    }
    return blob;
#endif
}
