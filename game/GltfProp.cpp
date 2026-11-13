#include "game/GltfProp.h"

#include "engine/core/Logger.h"
#include "engine/renderer/GltfLoader.h"

using namespace DirectX;

bool GltfProp::Load(Renderer& renderer, const std::string& path) {
    m_parts.clear();
    m_loaded = false;

    GltfModel model;
    std::string err;
    if (!LoadGltf(path, model, &err)) {
        LOG_WARN("GltfProp: %s を読み込めません（%s）", path.c_str(), err.c_str());
        return false;
    }

    // 画像インデックス → GPU テクスチャ（World::LoadGltfCharacter と同じキャッシュ方式）。
    std::vector<Texture> gpuImages(model.images.size());
    std::vector<bool>    gpuImageMade(model.images.size(), false);
    auto gpuTexture = [&](int imgIndex) -> const Texture* {
        if (imgIndex < 0 || static_cast<size_t>(imgIndex) >= model.images.size()) return nullptr;
        const size_t ii = static_cast<size_t>(imgIndex);
        if (!model.images[ii].valid) return nullptr;
        if (!gpuImageMade[ii]) {
            const GltfImage& img = model.images[ii];
            gpuImages[ii] = renderer.CreateTexture(img.width, img.height, img.rgba.data());
            gpuImageMade[ii] = true;
        }
        return gpuImages[ii].valid ? &gpuImages[ii] : nullptr;
    };

    const bool pbrOk = renderer.IsPbrAvailable();
    m_parts.reserve(model.primitives.size());
    for (const GltfPrimitive& prim : model.primitives) {
        Part part;
        const Texture* baseTex = prim.hasUv ? gpuTexture(prim.baseColorImage) : nullptr;

        if (pbrOk) {
            const Texture* nrmTex = prim.hasUv ? gpuTexture(prim.normalImage) : nullptr;
            const Texture* ormTex = prim.hasUv ? gpuTexture(prim.ormImage)    : nullptr;
            part.pbrMaterial = renderer.CreatePbrMaterial(baseTex, nrmTex, ormTex);
            if (part.pbrMaterial.valid) {
                part.pbrMaterial.baseColorFactor = prim.baseColor;
                part.pbrMaterial.metallicFactor  = prim.metallicFactor;
                part.pbrMaterial.roughnessFactor = prim.roughnessFactor;
                part.pbrMaterial.normalScale     = prim.normalScale;
                part.mesh   = renderer.CreatePbrMesh(MakePbrVertices(prim), prim.indices);
                part.usePbr = true;
            }
        }
        if (!part.usePbr) {
            // フォールバック: PBR 不可環境ではテクスチャ／単色経路。
            if (baseTex != nullptr) {
                part.texture  = *baseTex;
                part.textured = true;
                part.mesh = renderer.CreateTexturedMesh(prim.vertices, prim.indices);
            } else {
                part.mesh = renderer.CreateMesh(MakeLitVertices(prim), prim.indices);
            }
        }
        m_parts.push_back(std::move(part));
    }

    m_boundsMin = model.boundsMin;
    m_boundsMax = model.boundsMax;
    m_loaded = !m_parts.empty();
    if (m_loaded) {
        LOG_INFO("GltfProp: %s を読み込みました（primitive %zu / 頂点 %zu / 画像 %zu）",
                 path.c_str(), model.primitives.size(), model.totalVertices, model.images.size());
    } else {
        LOG_WARN("GltfProp: %s に primitive がありません", path.c_str());
    }
    return m_loaded;
}

void GltfProp::Draw(Renderer& renderer, const XMMATRIX& world, bool ignoreDepth) const {
    if (!m_loaded) return;
    for (const Part& part : m_parts) {
        if (part.usePbr) {
            renderer.DrawPbrMesh(part.mesh, part.pbrMaterial, world, ignoreDepth);
        } else if (part.textured) {
            renderer.DrawTexturedMesh(part.mesh, part.texture, world, ignoreDepth);
        } else {
            renderer.DrawMesh(part.mesh, world, ignoreDepth);
        }
    }
}
