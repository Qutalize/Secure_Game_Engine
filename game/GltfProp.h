#pragma once

#include <string>
#include <vector>

#include <DirectXMath.h>

#include "engine/renderer/Renderer.h"

// 静的（非スキン）glTF モデルを読み込み・保持し、任意の world 行列で描画するための
// 軽量ラッパー。World::LoadGltfCharacter と同じ「primitive ごとに PBR/テクスチャ/単色へ
// 振り分ける」ロジックを、キャラクター専用ではない置物（銃・建築モジュール等）向けに
// 切り出したもの。スキン/アニメーションは対象外（静的メッシュのみ）。
//
// 読み込みに失敗しても IsLoaded()==false が返るだけで例外は投げない。呼び出し側は
// 描画をスキップする（Draw() は未読込なら何もしない安全設計）か、代替表現に
// フォールバックすること。
class GltfProp {
public:
    // path の .glb/.gltf を読み込む。成功/失敗はログに出る。
    bool Load(Renderer& renderer, const std::string& path);

    bool IsLoaded() const { return m_loaded; }

    // 読み込んだモデルのバウンディングボックス（モデル空間）。
    // ★配置側はこれを見て「接地」と「中心合わせ」を自動で行う★ アセットの原点は
    //   作者・用途によってばらばら（建築モジュールは角原点、置物は中心、植物は根が
    //   地面下まで伸びていて minY が大きく負、など）。手書きの寸法表で吸収しようとすると
    //   モジュールを増やすたびに間違える。ファイル自身が知っている情報を使うほうが堅い。
    const DirectX::XMFLOAT3& BoundsMin() const { return m_boundsMin; }
    const DirectX::XMFLOAT3& BoundsMax() const { return m_boundsMax; }

    // 読み込み済みの全パーツを指定 world 行列で描く。ignoreDepth=true で深度無視
    // （一人称ビューモデルのように常に手前へ描きたい場合に使う）。
    void Draw(Renderer& renderer, const DirectX::XMMATRIX& world, bool ignoreDepth = false) const;

private:
    struct Part {
        Mesh        mesh;
        Texture     texture;
        PbrMaterial pbrMaterial;
        bool        textured = false;
        bool        usePbr   = false;
    };
    std::vector<Part> m_parts;
    bool m_loaded = false;
    DirectX::XMFLOAT3 m_boundsMin{};
    DirectX::XMFLOAT3 m_boundsMax{};
};
