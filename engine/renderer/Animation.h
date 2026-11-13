#pragma once
//
// スケルタルアニメーション（ステージL-04a）。
//
// 何をするものか:
//   glTF のアニメーションは「時刻 t におけるノードの TRS」をキーフレームで持っている。
//   再生とは、
//     ①各チャンネルを時刻 t で補間して、動くノードの TRS を差し替える
//     ②ノード階層を根から辿って各ジョイントの世界変換を求める
//     ③ジョイント行列 = 逆バインド行列 × ジョイント世界変換
//   を毎フレームやること。③の結果（ジョイント行列パレット）を頂点シェーダーへ渡し、
//   頂点ごとに 4 本のボーンの行列を重み付き平均して変形させる（＝スキニング）。
//
// なぜ逆バインド行列が要るのか:
//   メッシュの頂点は「バインドポーズ（たいてい T ポーズ）」の座標で入っている。
//   ボーンを動かす前に、まず頂点を「そのボーンのローカル空間」へ引き戻す必要がある。
//   その引き戻しが逆バインド行列。引き戻してからボーンの現在の世界変換を掛けると、
//   ボーンに追従した位置になる。
//
// ★行列規約★ 本プロジェクトは DirectXMath の行ベクトル規約（v' = v M）で統一している。
//   glTF は列ベクトル規約（v' = M v）なので、ローダーが読み込み時に転置済み。
//   その結果、glTF で「A * B」と書かれる合成は、こちらでは「B * A」になる。
//
#include <cstdint>
#include <string>
#include <vector>

#include <DirectXMath.h>

#include "engine/renderer/GltfLoader.h"

// kMaxJoints は engine/renderer/Renderer.h で定義（GPU 側の上限と共有するため）。

// アニメーションの再生状態。モデル 1 体につき 1 つ持つ。
class AnimationPlayer {
public:
    // モデルを紐付ける。ノード階層とスキンを参照するので、model は
    // このプレイヤーより長く生存していること。
    void Bind(const GltfModel* model, int skinIndex = 0);

    bool IsValid() const { return m_model != nullptr && m_skinIndex >= 0; }

    // クリップ名で選ぶ（部分一致・大文字小文字を無視）。見つからなければ false。
    bool Play(const std::string& clipName, bool loop = true);
    // インデックスで選ぶ。
    bool PlayIndex(int clipIndex, bool loop = true);

    // dt 秒ぶん時間を進めてジョイント行列を更新する。
    void Update(float dt);

    // 現在のジョイント行列パレット（頂点シェーダーへ渡す）。
    // ★アップロード時は転置すること★（HLSL は mul(vector, matrix) 順で使うため、
    //   他の行列と同じく CPU 側で転置してから定数バッファへ書く）。
    const std::vector<DirectX::XMFLOAT4X4>& JointMatrices() const { return m_jointMatrices; }
    size_t JointCount() const { return m_jointMatrices.size(); }

    float CurrentTime() const { return m_time; }
    float Duration() const;
    int   CurrentClip() const { return m_clipIndex; }
    const std::string& CurrentClipName() const;

    // 再生位置を直接指定する（テスト・巻き戻し用）。
    void SetTime(float t) { m_time = t; }

    // 指定クリップの指定時刻でジョイント行列を計算する（再生状態を変えない）。
    // 検証ツールから「t=0.0 と t=0.5 でボーンが動いているか」を確かめるのに使う。
    static bool SampleAt(const GltfModel& model, int skinIndex, int clipIndex, float time,
                         std::vector<DirectX::XMFLOAT4X4>& outJoints);

private:
    const GltfModel* m_model = nullptr;
    int   m_skinIndex = -1;
    int   m_clipIndex = -1;
    float m_time = 0.0f;
    bool  m_loop = true;
    std::vector<DirectX::XMFLOAT4X4> m_jointMatrices;
};

// 内部実装だが、テストからも使えるよう公開している。
namespace anim {

// サンプラーを時刻 t で評価して out（comps 個）へ書く。
void SampleSampler(const GltfAnimSampler& s, float time, float* out);

// クリップを時刻 t で評価し、ノードのローカル TRS を差し替えたうえで
// 全ノードの世界変換を求める。clipIndex < 0 ならバインドポーズ。
void ComputeNodeGlobals(const GltfModel& model, int clipIndex, float time,
                        std::vector<DirectX::XMFLOAT4X4>& outGlobals);

} // namespace anim
