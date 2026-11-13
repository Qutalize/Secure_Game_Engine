#include "engine/renderer/Animation.h"

#include <algorithm>
#include <cmath>

#include "engine/core/Logger.h"

using namespace DirectX;

namespace {

// 文字列の大文字小文字を無視した部分一致。
bool ContainsNoCase(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    auto lower = [](unsigned char c) { return static_cast<char>(::tolower(c)); };
    std::string h = haystack, n = needle;
    std::transform(h.begin(), h.end(), h.begin(), lower);
    std::transform(n.begin(), n.end(), n.begin(), lower);
    return h.find(n) != std::string::npos;
}

// ノード 1 つのローカル変換を作る。
XMMATRIX LocalMatrix(const GltfNode& n,
                     const XMFLOAT3& t, const XMFLOAT4& r, const XMFLOAT3& s) {
    if (n.hasMatrix) {
        // matrix 指定のノードはアニメーションの対象にならない（glTF 仕様）。
        return XMLoadFloat4x4(&n.matrix);
    }
    // glTF の合成順は M = T * R * S（列ベクトル規約）。
    // 行ベクトル規約では順序が反転して S * R * T になる。
    return XMMatrixScaling(s.x, s.y, s.z)
         * XMMatrixRotationQuaternion(XMVectorSet(r.x, r.y, r.z, r.w))
         * XMMatrixTranslation(t.x, t.y, t.z);
}

} // namespace

namespace anim {

void SampleSampler(const GltfAnimSampler& s, float time, float* out) {
    const int c = s.comps;
    if (s.times.empty() || s.values.empty()) {
        for (int i = 0; i < c; ++i) out[i] = 0.0f;
        return;
    }

    // CUBICSPLINE は 1 キーにつき「入力接線 / 値 / 出力接線」の 3 組が入る。
    const int stride = (s.interp == GltfAnimSampler::Interp::CubicSpline) ? c * 3 : c;
    const size_t keyCount = s.times.size();

    // 範囲外はクランプ（glTF 仕様）。
    if (time <= s.times.front() || keyCount == 1) {
        const int off = (s.interp == GltfAnimSampler::Interp::CubicSpline) ? c : 0;
        for (int i = 0; i < c; ++i) out[i] = s.values[static_cast<size_t>(off + i)];
        return;
    }
    if (time >= s.times.back()) {
        const size_t base = (keyCount - 1) * static_cast<size_t>(stride);
        const int off = (s.interp == GltfAnimSampler::Interp::CubicSpline) ? c : 0;
        for (int i = 0; i < c; ++i) out[i] = s.values[base + static_cast<size_t>(off + i)];
        return;
    }

    // time を挟むキー区間を二分探索で見つける。
    const size_t hi = static_cast<size_t>(
        std::upper_bound(s.times.begin(), s.times.end(), time) - s.times.begin());
    const size_t k1 = (hi < keyCount) ? hi : keyCount - 1;
    const size_t k0 = (k1 > 0) ? k1 - 1 : 0;

    const float t0 = s.times[k0];
    const float t1 = s.times[k1];
    const float dt = t1 - t0;
    const float u  = (dt > 1e-9f) ? (time - t0) / dt : 0.0f;

    if (s.interp == GltfAnimSampler::Interp::Step) {
        const size_t base = k0 * static_cast<size_t>(stride);
        for (int i = 0; i < c; ++i) out[i] = s.values[base + static_cast<size_t>(i)];
        return;
    }

    if (s.interp == GltfAnimSampler::Interp::CubicSpline) {
        // エルミート補間。v=値 / a=入力接線 / b=出力接線 の並びは [a0 v0 b0][a1 v1 b1]。
        const size_t b0 = k0 * static_cast<size_t>(stride);
        const size_t b1 = k1 * static_cast<size_t>(stride);
        const float u2 = u * u, u3 = u2 * u;
        const float h00 =  2.0f * u3 - 3.0f * u2 + 1.0f;
        const float h10 =         u3 - 2.0f * u2 + u;
        const float h01 = -2.0f * u3 + 3.0f * u2;
        const float h11 =         u3 -        u2;
        for (int i = 0; i < c; ++i) {
            const float v0 = s.values[b0 + static_cast<size_t>(c + i)];
            const float m0 = s.values[b0 + static_cast<size_t>(c * 2 + i)] * dt;
            const float v1 = s.values[b1 + static_cast<size_t>(c + i)];
            const float m1 = s.values[b1 + static_cast<size_t>(i)] * dt;
            out[i] = h00 * v0 + h10 * m0 + h01 * v1 + h11 * m1;
        }
        return;
    }

    // LINEAR
    const size_t b0 = k0 * static_cast<size_t>(stride);
    const size_t b1 = k1 * static_cast<size_t>(stride);
    if (c == 4) {
        // ★回転は線形補間ではなく球面線形補間（slerp）★
        // クォータニオンを成分ごとに lerp すると長さが 1 でなくなり、
        // 回転が途中で縮んで見える（ボーンが痩せる/ねじれる）。
        XMVECTOR q0 = XMVectorSet(s.values[b0 + 0], s.values[b0 + 1],
                                  s.values[b0 + 2], s.values[b0 + 3]);
        XMVECTOR q1 = XMVectorSet(s.values[b1 + 0], s.values[b1 + 1],
                                  s.values[b1 + 2], s.values[b1 + 3]);
        // 符号が反転していると長い方の弧を回ってしまうので、内積が負なら反転させる。
        if (XMVectorGetX(XMVector4Dot(q0, q1)) < 0.0f) q1 = XMVectorNegate(q1);
        XMVECTOR q = XMQuaternionNormalize(XMQuaternionSlerp(q0, q1, u));
        XMFLOAT4 r; XMStoreFloat4(&r, q);
        out[0] = r.x; out[1] = r.y; out[2] = r.z; out[3] = r.w;
        return;
    }
    for (int i = 0; i < c; ++i) {
        const float v0 = s.values[b0 + static_cast<size_t>(i)];
        const float v1 = s.values[b1 + static_cast<size_t>(i)];
        out[i] = v0 + (v1 - v0) * u;
    }
}

void ComputeNodeGlobals(const GltfModel& model, int clipIndex, float time,
                        std::vector<XMFLOAT4X4>& outGlobals) {
    const size_t n = model.nodes.size();
    outGlobals.assign(n, XMFLOAT4X4{});
    if (n == 0) return;

    // ①バインドポーズの TRS から始め、アニメーションのチャンネルで上書きする。
    std::vector<XMFLOAT3> trans(n);
    std::vector<XMFLOAT4> rot(n);
    std::vector<XMFLOAT3> scale(n);
    for (size_t i = 0; i < n; ++i) {
        trans[i] = model.nodes[i].translation;
        rot[i]   = model.nodes[i].rotation;
        scale[i] = model.nodes[i].scale;
    }

    if (clipIndex >= 0 && static_cast<size_t>(clipIndex) < model.animations.size()) {
        const GltfAnimation& clip = model.animations[static_cast<size_t>(clipIndex)];
        for (const GltfAnimChannel& ch : clip.channels) {
            if (ch.node < 0 || static_cast<size_t>(ch.node) >= n) continue;
            if (ch.sampler < 0 || static_cast<size_t>(ch.sampler) >= clip.samplers.size()) continue;
            const GltfAnimSampler& s = clip.samplers[static_cast<size_t>(ch.sampler)];
            if (s.times.empty()) continue;

            float v[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            SampleSampler(s, time, v);
            const size_t i = static_cast<size_t>(ch.node);
            switch (ch.path) {
                case GltfAnimChannel::Path::Translation:
                    trans[i] = XMFLOAT3{ v[0], v[1], v[2] };
                    break;
                case GltfAnimChannel::Path::Rotation:
                    rot[i] = XMFLOAT4{ v[0], v[1], v[2], v[3] };
                    break;
                case GltfAnimChannel::Path::Scale:
                    scale[i] = XMFLOAT3{ v[0], v[1], v[2] };
                    break;
                default:
                    break;
            }
        }
    }

    // ②根から辿って世界変換を合成する。
    //   行ベクトル規約なので child_world = child_local * parent_world。
    std::vector<int> stack;
    std::vector<bool> done(n, false);
    for (int root : model.rootNodes) {
        if (root < 0 || static_cast<size_t>(root) >= n) continue;
        stack.push_back(root);
        XMStoreFloat4x4(&outGlobals[static_cast<size_t>(root)],
                        LocalMatrix(model.nodes[static_cast<size_t>(root)],
                                    trans[static_cast<size_t>(root)],
                                    rot[static_cast<size_t>(root)],
                                    scale[static_cast<size_t>(root)]));
        done[static_cast<size_t>(root)] = true;

        while (!stack.empty()) {
            const int cur = stack.back();
            stack.pop_back();
            const XMMATRIX parentWorld =
                XMLoadFloat4x4(&outGlobals[static_cast<size_t>(cur)]);
            for (int c : model.nodes[static_cast<size_t>(cur)].children) {
                if (c < 0 || static_cast<size_t>(c) >= n || done[static_cast<size_t>(c)]) {
                    continue;   // 循環参照への保険
                }
                const size_t ci = static_cast<size_t>(c);
                const XMMATRIX local =
                    LocalMatrix(model.nodes[ci], trans[ci], rot[ci], scale[ci]);
                XMStoreFloat4x4(&outGlobals[ci], local * parentWorld);
                done[ci] = true;
                stack.push_back(c);
            }
        }
    }

    // ルートから辿れなかったノード（シーンに繋がっていない）は単位行列にしておく。
    for (size_t i = 0; i < n; ++i) {
        if (!done[i]) XMStoreFloat4x4(&outGlobals[i], XMMatrixIdentity());
    }
}

} // namespace anim

// ---------------------------------------------------------------------------

bool AnimationPlayer::SampleAt(const GltfModel& model, int skinIndex, int clipIndex,
                               float time, std::vector<XMFLOAT4X4>& outJoints) {
    outJoints.clear();
    if (skinIndex < 0 || static_cast<size_t>(skinIndex) >= model.skins.size()) return false;
    const GltfSkin& skin = model.skins[static_cast<size_t>(skinIndex)];

    std::vector<XMFLOAT4X4> globals;
    anim::ComputeNodeGlobals(model, clipIndex, time, globals);

    outJoints.resize(skin.jointNodes.size());
    for (size_t j = 0; j < skin.jointNodes.size(); ++j) {
        const int node = skin.jointNodes[j];
        XMMATRIX g = (node >= 0 && static_cast<size_t>(node) < globals.size())
            ? XMLoadFloat4x4(&globals[static_cast<size_t>(node)])
            : XMMatrixIdentity();
        const XMMATRIX ib = XMLoadFloat4x4(&skin.inverseBind[j]);

        // ★合成順に注意★
        // glTF（列ベクトル規約）: skin = globalJoint * inverseBind
        // 転置すると        : skin^T = inverseBind^T * globalJoint^T
        // 本プロジェクトの行列は全部転置済みなので、掛ける順序は「逆バインド × 世界変換」。
        XMStoreFloat4x4(&outJoints[j], ib * g);
    }
    return true;
}

void AnimationPlayer::Bind(const GltfModel* model, int skinIndex) {
    m_model = model;
    m_skinIndex = -1;
    m_clipIndex = -1;
    m_time = 0.0f;
    m_jointMatrices.clear();

    if (model == nullptr || model->skins.empty()) return;
    if (skinIndex < 0 || static_cast<size_t>(skinIndex) >= model->skins.size()) skinIndex = 0;
    m_skinIndex = skinIndex;

    const size_t joints = model->skins[static_cast<size_t>(skinIndex)].jointNodes.size();
    if (joints > kMaxJoints) {
        LOG_WARN("スキンのボーン数 %zu が上限 %u を超えています（超過分は無視されます）",
                 joints, kMaxJoints);
    }
    // バインドポーズで初期化しておく（クリップ未選択でも描けるように）。
    SampleAt(*model, m_skinIndex, -1, 0.0f, m_jointMatrices);
}

bool AnimationPlayer::PlayIndex(int clipIndex, bool loop) {
    if (m_model == nullptr) return false;
    if (clipIndex < 0 || static_cast<size_t>(clipIndex) >= m_model->animations.size()) {
        return false;
    }
    if (m_clipIndex != clipIndex) m_time = 0.0f;   // 別クリップへ切り替えたら頭出し
    m_clipIndex = clipIndex;
    m_loop = loop;
    return true;
}

bool AnimationPlayer::Play(const std::string& clipName, bool loop) {
    if (m_model == nullptr) return false;
    // 完全一致を優先し、無ければ部分一致。
    // Blender 経由の glTF はクリップ名が "CharacterArmature|Idle_Gun" のように
    // 接頭辞付きになることがあるため、部分一致を許している。
    for (size_t i = 0; i < m_model->animations.size(); ++i) {
        if (m_model->animations[i].name == clipName) return PlayIndex(static_cast<int>(i), loop);
    }
    for (size_t i = 0; i < m_model->animations.size(); ++i) {
        if (ContainsNoCase(m_model->animations[i].name, clipName)) {
            return PlayIndex(static_cast<int>(i), loop);
        }
    }
    return false;
}

float AnimationPlayer::Duration() const {
    if (m_model == nullptr || m_clipIndex < 0) return 0.0f;
    return m_model->animations[static_cast<size_t>(m_clipIndex)].duration;
}

const std::string& AnimationPlayer::CurrentClipName() const {
    static const std::string kEmpty;
    if (m_model == nullptr || m_clipIndex < 0) return kEmpty;
    return m_model->animations[static_cast<size_t>(m_clipIndex)].name;
}

void AnimationPlayer::Update(float dt) {
    if (!IsValid()) return;

    if (m_clipIndex >= 0) {
        const float dur = Duration();
        m_time += dt;
        if (dur > 1e-6f) {
            if (m_loop) {
                m_time = std::fmod(m_time, dur);
                if (m_time < 0.0f) m_time += dur;
            } else if (m_time > dur) {
                m_time = dur;   // 末尾で停止（Death などループさせたくないクリップ用）
            }
        }
    }
    SampleAt(*m_model, m_skinIndex, m_clipIndex, m_time, m_jointMatrices);
}
