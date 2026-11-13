#include "engine/renderer/GltfLoader.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "engine/core/Json.h"
#include "engine/core/Logger.h"
#include "engine/renderer/PngLoader.h"   // ステージL-01b

using namespace DirectX;

namespace {

// ---------------------------------------------------------------------------
// ファイル読み込み
// ---------------------------------------------------------------------------
bool ReadWholeFile(const std::string& path, std::vector<uint8_t>& out) {
    FILE* fp = nullptr;
    if (fopen_s(&fp, path.c_str(), "rb") != 0 || fp == nullptr) return false;
    fseek(fp, 0, SEEK_END);
    const long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size < 0) { fclose(fp); return false; }
    out.resize(static_cast<size_t>(size));
    const size_t got = (size > 0) ? fread(out.data(), 1, out.size(), fp) : 0;
    fclose(fp);
    return got == out.size();
}

// パスからディレクトリ部分を取り出す（外部 .bin を相対解決するため）。
std::string DirectoryOf(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return (slash == std::string::npos) ? std::string() : path.substr(0, slash + 1);
}

// ---------------------------------------------------------------------------
// base64 デコード（data URI 埋め込みバッファ用）
// ---------------------------------------------------------------------------
int Base64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1; // '=' や改行など
}

std::vector<uint8_t> Base64Decode(const char* s, size_t len) {
    std::vector<uint8_t> out;
    out.reserve(len * 3 / 4);
    uint32_t acc  = 0;
    int      bits = 0;
    for (size_t i = 0; i < len; ++i) {
        const int v = Base64Value(s[i]);
        if (v < 0) continue; // パディングや空白は読み飛ばす
        acc  = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// glTF のアクセサを読むためのコンテキスト
// ---------------------------------------------------------------------------
struct GltfContext {
    const json::Value*                root = nullptr;
    std::vector<std::vector<uint8_t>> buffers;  // buffers[i] の実体
    std::string                       error;

    bool Fail(const std::string& msg) {
        if (error.empty()) error = msg;
        return false;
    }

    // 1 要素あたりの成分数（"VEC3" → 3）
    static int ComponentsOf(const std::string& type) {
        if (type == "SCALAR") return 1;
        if (type == "VEC2")   return 2;
        if (type == "VEC3")   return 3;
        if (type == "VEC4")   return 4;
        if (type == "MAT2")   return 4;
        if (type == "MAT3")   return 9;
        if (type == "MAT4")   return 16;
        return 0;
    }

    // componentType（glTF の定数）→ 1 成分あたりのバイト数
    static int ByteSizeOf(int componentType) {
        switch (componentType) {
            case 5120: return 1; // BYTE
            case 5121: return 1; // UNSIGNED_BYTE
            case 5122: return 2; // SHORT
            case 5123: return 2; // UNSIGNED_SHORT
            case 5125: return 4; // UNSIGNED_INT
            case 5126: return 4; // FLOAT
            default:   return 0;
        }
    }

    // アクセサの生データ位置とレイアウトを解決する。
    // glTF は「accessor →(どのbufferViewの何番目から) bufferView →(どのbufferの何バイト目から) buffer」
    // という 3 段の間接参照になっている。ここでそれを 1 回で辿る。
    struct AccessorView {
        const uint8_t* base       = nullptr; // 先頭要素の位置
        size_t         stride     = 0;       // 次の要素までのバイト数
        int            count      = 0;       // 要素数
        int            components = 0;       // 1 要素の成分数
        int            compType   = 0;       // componentType
        bool           normalized = false;
        size_t         remaining  = 0;       // base から先に読めるバイト数（境界チェック用）
    };

    bool ResolveAccessor(int accessorIndex, AccessorView& out) {
        const json::Value& accessors = (*root)["accessors"];
        if (accessorIndex < 0 || static_cast<size_t>(accessorIndex) >= accessors.Size()) {
            return Fail("accessor のインデックスが範囲外です");
        }
        const json::Value& acc = accessors[static_cast<size_t>(accessorIndex)];

        out.count      = acc.GetInt("count", 0);
        out.compType   = acc.GetInt("componentType", 0);
        out.components = ComponentsOf(acc.GetStr("type"));
        out.normalized = acc["normalized"].Bool(false);

        const int compBytes = ByteSizeOf(out.compType);
        if (compBytes == 0 || out.components == 0) {
            return Fail("accessor の componentType / type が未対応です");
        }
        if (acc.Has("sparse")) {
            // スパースアクセサは「大部分が既定値で一部だけ上書き」という表現。
            // 使っているアセットは稀なので未対応。読み飛ばすと形が崩れるので明示的に失敗させる。
            return Fail("スパースアクセサには未対応です");
        }

        // bufferView が無いアクセサは「全要素ゼロ」を意味する（glTF 仕様）。
        const json::Value* bvIndex = acc.Find("bufferView");
        if (bvIndex == nullptr) {
            out.base      = nullptr;
            out.stride    = 0;
            out.remaining = 0;
            return true;
        }

        const json::Value& views = (*root)["bufferViews"];
        const int bvi = bvIndex->Int(-1);
        if (bvi < 0 || static_cast<size_t>(bvi) >= views.Size()) {
            return Fail("bufferView のインデックスが範囲外です");
        }
        const json::Value& bv = views[static_cast<size_t>(bvi)];

        const int bufIndex = bv.GetInt("buffer", -1);
        if (bufIndex < 0 || static_cast<size_t>(bufIndex) >= buffers.size()) {
            return Fail("buffer のインデックスが範囲外です");
        }
        const std::vector<uint8_t>& buf = buffers[static_cast<size_t>(bufIndex)];

        const size_t bvOffset = static_cast<size_t>(bv.GetInt("byteOffset", 0));
        const size_t bvLength = static_cast<size_t>(bv.GetInt("byteLength", 0));
        const size_t accOffset = static_cast<size_t>(acc.GetInt("byteOffset", 0));

        if (bvOffset + bvLength > buf.size()) {
            return Fail("bufferView が buffer の範囲を超えています");
        }

        // byteStride は「インターリーブ配置」のときに入る。無ければ密詰め。
        const int declaredStride = bv.GetInt("byteStride", 0);
        out.stride = (declaredStride > 0)
            ? static_cast<size_t>(declaredStride)
            : static_cast<size_t>(compBytes * out.components);

        if (accOffset > bvLength) {
            return Fail("accessor の byteOffset が bufferView を超えています");
        }
        out.base      = buf.data() + bvOffset + accOffset;
        out.remaining = bvLength - accOffset;

        // 最後の要素まで読めるかを確認しておく（壊れたファイルでの範囲外読みを防ぐ）。
        if (out.count > 0) {
            const size_t need = out.stride * static_cast<size_t>(out.count - 1)
                              + static_cast<size_t>(compBytes) * static_cast<size_t>(out.components);
            if (need > out.remaining) {
                return Fail("accessor が bufferView の範囲を超えています");
            }
        }
        return true;
    }

    // 1 成分を float として読む（整数型は normalized 指定に従って正規化する）。
    static float ReadComponentAsFloat(const uint8_t* p, int compType, bool normalized) {
        switch (compType) {
            case 5126: { float v; memcpy(&v, p, 4); return v; }
            case 5125: { uint32_t v; memcpy(&v, p, 4); return static_cast<float>(v); }
            case 5123: { uint16_t v; memcpy(&v, p, 2);
                         return normalized ? static_cast<float>(v) / 65535.0f
                                           : static_cast<float>(v); }
            case 5122: { int16_t v; memcpy(&v, p, 2);
                         return normalized ? std::max(static_cast<float>(v) / 32767.0f, -1.0f)
                                           : static_cast<float>(v); }
            case 5121: { uint8_t v = *p;
                         return normalized ? static_cast<float>(v) / 255.0f
                                           : static_cast<float>(v); }
            case 5120: { int8_t v; memcpy(&v, p, 1);
                         return normalized ? std::max(static_cast<float>(v) / 127.0f, -1.0f)
                                           : static_cast<float>(v); }
            default: return 0.0f;
        }
    }

    // アクセサを float 配列（count × components）として読み出す。
    bool ReadFloats(int accessorIndex, std::vector<float>& out, int& components, int& count) {
        AccessorView av;
        if (!ResolveAccessor(accessorIndex, av)) return false;
        components = av.components;
        count      = av.count;
        out.assign(static_cast<size_t>(count) * static_cast<size_t>(components), 0.0f);
        if (av.base == nullptr) return true; // bufferView 無し＝全ゼロ

        const int compBytes = ByteSizeOf(av.compType);
        for (int i = 0; i < count; ++i) {
            const uint8_t* elem = av.base + av.stride * static_cast<size_t>(i);
            for (int c = 0; c < components; ++c) {
                out[static_cast<size_t>(i) * components + c] =
                    ReadComponentAsFloat(elem + compBytes * c, av.compType, av.normalized);
            }
        }
        return true;
    }

    // インデックスアクセサを uint32 配列として読み出す。
    bool ReadIndices(int accessorIndex, std::vector<uint32_t>& out) {
        AccessorView av;
        if (!ResolveAccessor(accessorIndex, av)) return false;
        if (av.components != 1) return Fail("インデックスアクセサが SCALAR ではありません");
        out.assign(static_cast<size_t>(av.count), 0u);
        if (av.base == nullptr) return true;

        for (int i = 0; i < av.count; ++i) {
            const uint8_t* p = av.base + av.stride * static_cast<size_t>(i);
            switch (av.compType) {
                case 5125: { uint32_t v; memcpy(&v, p, 4); out[static_cast<size_t>(i)] = v; break; }
                case 5123: { uint16_t v; memcpy(&v, p, 2); out[static_cast<size_t>(i)] = v; break; }
                case 5121: { out[static_cast<size_t>(i)] = *p; break; }
                default: return Fail("インデックスの componentType が未対応です");
            }
        }
        return true;
    }
};

// ---------------------------------------------------------------------------
// ノードのローカル変換行列を求める
// ---------------------------------------------------------------------------
XMMATRIX NodeLocalMatrix(const json::Value& node) {
    // glTF のノードは "matrix"（列優先 16 要素）か "translation/rotation/scale" の
    // どちらかで変換を持つ。両方が同時に指定されることはない（仕様）。
    if (const json::Value* m = node.Find("matrix")) {
        if (m->Size() == 16) {
            float v[16];
            for (size_t i = 0; i < 16; ++i) v[i] = (*m)[i].Flt(0.0f);
            // ★行列規約の話（ここを間違えるとモデルが明後日の向きになる）★
            // glTF は「列優先の配列」で「列ベクトル規約（v' = M v）」の行列を表す。
            //   → v[0..3] は M の第 0 列。
            // DirectXMath は「行ベクトル規約（v' = v M）」なので、必要なのは M の転置。
            //   → M^T の第 0 行 = M の第 0 列 = v[0..3]。
            // つまり配列を順番どおり XMMatrixSet（行優先で受け取る）へ流し込めば、
            // ちょうど M^T が得られる。転置処理を書き足す必要はない。
            return XMMatrixSet(v[0], v[1], v[2], v[3],
                               v[4], v[5], v[6], v[7],
                               v[8], v[9], v[10], v[11],
                               v[12], v[13], v[14], v[15]);
        }
    }

    XMVECTOR scale = XMVectorSet(1.0f, 1.0f, 1.0f, 0.0f);
    if (const json::Value* s = node.Find("scale")) {
        if (s->Size() == 3) {
            scale = XMVectorSet((*s)[0].Flt(1.0f), (*s)[1].Flt(1.0f), (*s)[2].Flt(1.0f), 0.0f);
        }
    }
    XMVECTOR rot = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f); // クォータニオン (x,y,z,w)
    if (const json::Value* r = node.Find("rotation")) {
        if (r->Size() == 4) {
            rot = XMVectorSet((*r)[0].Flt(0.0f), (*r)[1].Flt(0.0f),
                              (*r)[2].Flt(0.0f), (*r)[3].Flt(1.0f));
        }
    }
    XMVECTOR trans = XMVectorSet(0.0f, 0.0f, 0.0f, 0.0f);
    if (const json::Value* t = node.Find("translation")) {
        if (t->Size() == 3) {
            trans = XMVectorSet((*t)[0].Flt(0.0f), (*t)[1].Flt(0.0f), (*t)[2].Flt(0.0f), 0.0f);
        }
    }

    // glTF の合成順は M = T * R * S（列ベクトル規約）。
    // DirectXMath は行ベクトル規約なので、掛ける順序が逆になり S * R * T になる。
    return XMMatrixScalingFromVector(scale)
         * XMMatrixRotationQuaternion(rot)
         * XMMatrixTranslationFromVector(trans);
}

// ---------------------------------------------------------------------------
// プリミティブ 1 つを読み出す
// ---------------------------------------------------------------------------
// glTF の 16 要素配列（列優先・列ベクトル規約）を DirectXMath の行列へ。
// ★L-01a と同じ理屈★: 必要なのは転置 M^T で、M^T の第 r 行 = M の第 r 列 = 配列の 4r..4r+3。
// つまり配列を順番どおり XMMatrixSet（行優先で受け取る）へ流せばそれが M^T になる。
XMMATRIX MatrixFromGltfArray(const float* v) {
    return XMMatrixSet(v[0],  v[1],  v[2],  v[3],
                       v[4],  v[5],  v[6],  v[7],
                       v[8],  v[9],  v[10], v[11],
                       v[12], v[13], v[14], v[15]);
}

bool LoadPrimitive(GltfContext& ctx, const json::Value& prim,
                   const XMMATRIX& world, GltfModel& model, int skinIndex) {
    const json::Value& attrs = prim["attributes"];
    const json::Value* posAttr = attrs.Find("POSITION");
    if (posAttr == nullptr) {
        LOG_WARN("glTF: POSITION の無い primitive を読み飛ばしました");
        return true; // 致命的ではないので続行
    }

    // mode 4 = TRIANGLES。既定値も 4。点・線・ストリップは未対応。
    const int mode = prim.GetInt("mode", 4);
    if (mode != 4) {
        LOG_WARN("glTF: primitive.mode=%d（三角形以外）を読み飛ばしました", mode);
        return true;
    }

    std::vector<float> pos, nrm, uv;
    int posComp = 0, posCount = 0;
    if (!ctx.ReadFloats(posAttr->Int(-1), pos, posComp, posCount)) return false;
    if (posComp != 3) return ctx.Fail("POSITION が VEC3 ではありません");

    int nrmComp = 0, nrmCount = 0;
    const bool hasNormal = (attrs.Find("NORMAL") != nullptr) &&
        ctx.ReadFloats(attrs["NORMAL"].Int(-1), nrm, nrmComp, nrmCount) &&
        nrmComp == 3 && nrmCount == posCount;

    int uvComp = 0, uvCount = 0;
    const bool hasUv = (attrs.Find("TEXCOORD_0") != nullptr) &&
        ctx.ReadFloats(attrs["TEXCOORD_0"].Int(-1), uv, uvComp, uvCount) &&
        uvComp == 2 && uvCount == posCount;

    GltfPrimitive out;
    out.hasUv     = hasUv;
    out.skinIndex = skinIndex;
    out.skinned   = (skinIndex >= 0);
    out.vertices.resize(static_cast<size_t>(posCount));

    // 法線変換用の行列。回転＋一様スケールを想定（既存 Renderer と同じ前提）。
    const XMMATRIX normalMat = world;

    for (int i = 0; i < posCount; ++i) {
        const size_t k = static_cast<size_t>(i);
        XMVECTOR p = XMVectorSet(pos[k * 3 + 0], pos[k * 3 + 1], pos[k * 3 + 2], 1.0f);
        p = XMVector3TransformCoord(p, world);
        XMStoreFloat3(&out.vertices[k].position, p);

        if (hasNormal) {
            XMVECTOR n = XMVectorSet(nrm[k * 3 + 0], nrm[k * 3 + 1], nrm[k * 3 + 2], 0.0f);
            n = XMVector3Normalize(XMVector3TransformNormal(n, normalMat));
            XMStoreFloat3(&out.vertices[k].normal, n);
        } else {
            out.vertices[k].normal = XMFLOAT3{ 0.0f, 0.0f, 0.0f }; // 後で面法線から作る
        }

        out.vertices[k].uv = hasUv ? XMFLOAT2{ uv[k * 2 + 0], uv[k * 2 + 1] }
                                   : XMFLOAT2{ 0.0f, 0.0f };
    }

    // インデックス。省略時は 0,1,2,... の連番（glTF 仕様）。
    if (const json::Value* idx = prim.Find("indices")) {
        if (!ctx.ReadIndices(idx->Int(-1), out.indices)) return false;
    } else {
        out.indices.resize(static_cast<size_t>(posCount));
        for (int i = 0; i < posCount; ++i) out.indices[static_cast<size_t>(i)] =
            static_cast<uint32_t>(i);
    }

    // 法線が無ければ面法線を頂点へ積んで正規化する（.obj ローダーと同じ手法）。
    if (!hasNormal) {
        for (size_t i = 0; i + 2 < out.indices.size(); i += 3) {
            const uint32_t i0 = out.indices[i], i1 = out.indices[i + 1], i2 = out.indices[i + 2];
            if (i0 >= out.vertices.size() || i1 >= out.vertices.size() ||
                i2 >= out.vertices.size()) {
                continue;
            }
            XMVECTOR v0 = XMLoadFloat3(&out.vertices[i0].position);
            XMVECTOR v1 = XMLoadFloat3(&out.vertices[i1].position);
            XMVECTOR v2 = XMLoadFloat3(&out.vertices[i2].position);
            XMVECTOR fn = XMVector3Cross(XMVectorSubtract(v1, v0), XMVectorSubtract(v2, v0));
            for (uint32_t vi : { i0, i1, i2 }) {
                XMVECTOR acc = XMLoadFloat3(&out.vertices[vi].normal);
                XMStoreFloat3(&out.vertices[vi].normal, XMVectorAdd(acc, fn));
            }
        }
        for (TexVertex& v : out.vertices) {
            XMVECTOR n = XMLoadFloat3(&v.normal);
            if (XMVectorGetX(XMVector3LengthSq(n)) < 1e-12f) {
                n = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f); // 縮退三角形の保険
            }
            XMStoreFloat3(&v.normal, XMVector3Normalize(n));
        }
    }

    // ---- ステージL-04a: スキニング属性（JOINTS_0 / WEIGHTS_0）----
    // JOINTS_0 は「この頂点に影響するボーン 4 本の番号」、WEIGHTS_0 はその重み。
    // 番号は skin.joints[] のインデックス（ノード番号ではない）ことに注意。
    if (skinIndex >= 0) {
        std::vector<float> jf, wf;
        int jc = 0, jn = 0, wc = 0, wn = 0;
        const bool hasJoints = (attrs.Find("JOINTS_0") != nullptr) &&
            ctx.ReadFloats(attrs["JOINTS_0"].Int(-1), jf, jc, jn) && jc == 4 && jn == posCount;
        const bool hasWeights = (attrs.Find("WEIGHTS_0") != nullptr) &&
            ctx.ReadFloats(attrs["WEIGHTS_0"].Int(-1), wf, wc, wn) && wc == 4 && wn == posCount;

        if (hasJoints && hasWeights) {
            out.joints.resize(static_cast<size_t>(posCount));
            out.weights.resize(static_cast<size_t>(posCount));
            for (int i = 0; i < posCount; ++i) {
                const size_t k = static_cast<size_t>(i);
                // JOINTS_0 は UNSIGNED_BYTE / UNSIGNED_SHORT だが normalized ではないので、
                // ReadFloats がそのままの整数値を float で返している。四捨五入して戻す。
                out.joints[k] = XMUINT4{
                    static_cast<uint32_t>(jf[k * 4 + 0] + 0.5f),
                    static_cast<uint32_t>(jf[k * 4 + 1] + 0.5f),
                    static_cast<uint32_t>(jf[k * 4 + 2] + 0.5f),
                    static_cast<uint32_t>(jf[k * 4 + 3] + 0.5f) };
                // 重みの合計は 1 であるべきだが、量子化で誤差が出るので正規化しておく。
                // ここを怠るとメッシュが微妙に膨らんだり縮んだりする。
                float sum = wf[k * 4 + 0] + wf[k * 4 + 1] + wf[k * 4 + 2] + wf[k * 4 + 3];
                if (sum < 1e-6f) { sum = 1.0f; wf[k * 4 + 0] = 1.0f; }
                out.weights[k] = XMFLOAT4{ wf[k * 4 + 0] / sum, wf[k * 4 + 1] / sum,
                                           wf[k * 4 + 2] / sum, wf[k * 4 + 3] / sum };
            }
        } else {
            LOG_WARN("glTF: skin 参照だが JOINTS_0/WEIGHTS_0 が読めません（静止メッシュ扱い）");
            out.skinned   = false;
            out.skinIndex = -1;
        }
    }

    // ---- ステージL-02: 接線 ----
    // 法線マップは「接空間（面の横方向 = UV の u 方向）」を基準に法線の傾きを記録している。
    // それをワールド空間へ戻すには接線が要る。glTF が TANGENT を持っていればそれを使い、
    // 無ければ三角形ごとに UV の変化量から求めて頂点に積む（平均して滑らかにする）。
    {
        std::vector<float> tan;
        int tanComp = 0, tanCount = 0;
        const bool hasTangent = (attrs.Find("TANGENT") != nullptr) &&
            ctx.ReadFloats(attrs["TANGENT"].Int(-1), tan, tanComp, tanCount) &&
            tanComp == 4 && tanCount == posCount;

        out.tangents.assign(static_cast<size_t>(posCount), XMFLOAT4{ 1.0f, 0.0f, 0.0f, 1.0f });

        if (hasTangent) {
            for (int i = 0; i < posCount; ++i) {
                const size_t k = static_cast<size_t>(i);
                XMVECTOR t = XMVectorSet(tan[k * 4 + 0], tan[k * 4 + 1], tan[k * 4 + 2], 0.0f);
                t = XMVector3Normalize(XMVector3TransformNormal(t, world));
                XMFLOAT3 t3; XMStoreFloat3(&t3, t);
                out.tangents[k] = XMFLOAT4{ t3.x, t3.y, t3.z, tan[k * 4 + 3] };
            }
        } else if (hasUv) {
            // UV から接線を生成する。三角形の 2 辺のワールド差分と UV 差分の関係を解くと、
            // 「UV の u が増える向き」に対応するワールド方向（＝接線）が求まる。
            std::vector<XMFLOAT3> acc(static_cast<size_t>(posCount), XMFLOAT3{ 0, 0, 0 });
            for (size_t i = 0; i + 2 < out.indices.size(); i += 3) {
                const uint32_t a = out.indices[i], b = out.indices[i + 1], c = out.indices[i + 2];
                if (a >= out.vertices.size() || b >= out.vertices.size() ||
                    c >= out.vertices.size()) {
                    continue;
                }
                const XMFLOAT3& p0 = out.vertices[a].position;
                const XMFLOAT3& p1 = out.vertices[b].position;
                const XMFLOAT3& p2 = out.vertices[c].position;
                const XMFLOAT2& w0 = out.vertices[a].uv;
                const XMFLOAT2& w1 = out.vertices[b].uv;
                const XMFLOAT2& w2 = out.vertices[c].uv;

                const float x1 = p1.x - p0.x, x2 = p2.x - p0.x;
                const float y1 = p1.y - p0.y, y2 = p2.y - p0.y;
                const float z1 = p1.z - p0.z, z2 = p2.z - p0.z;
                const float s1 = w1.x - w0.x, s2 = w2.x - w0.x;
                const float t1 = w1.y - w0.y, t2 = w2.y - w0.y;

                const float det = s1 * t2 - s2 * t1;
                if (fabsf(det) < 1e-12f) continue; // UV が潰れている三角形は寄与させない
                const float r = 1.0f / det;
                const XMFLOAT3 tdir{ (t2 * x1 - t1 * x2) * r,
                                     (t2 * y1 - t1 * y2) * r,
                                     (t2 * z1 - t1 * z2) * r };
                for (uint32_t vi : { a, b, c }) {
                    acc[vi].x += tdir.x; acc[vi].y += tdir.y; acc[vi].z += tdir.z;
                }
            }
            for (int i = 0; i < posCount; ++i) {
                const size_t k = static_cast<size_t>(i);
                XMVECTOR n = XMLoadFloat3(&out.vertices[k].normal);
                XMVECTOR t = XMLoadFloat3(&acc[k]);
                if (XMVectorGetX(XMVector3LengthSq(t)) < 1e-12f) {
                    // 接線が求まらなかった頂点は、法線と直交する適当な軸を使う。
                    XMVECTOR up = (fabsf(out.vertices[k].normal.y) > 0.99f)
                        ? XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f)
                        : XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
                    t = XMVector3Cross(up, n);
                }
                // グラム・シュミット直交化（接線を法線に対して直交させる）
                t = XMVector3Normalize(XMVectorSubtract(t, XMVectorScale(n, XMVectorGetX(XMVector3Dot(n, t)))));
                XMFLOAT3 t3; XMStoreFloat3(&t3, t);
                out.tangents[k] = XMFLOAT4{ t3.x, t3.y, t3.z, 1.0f };
            }
        }
    }

    // マテリアル（baseColorFactor / 参照画像 / PBR 係数）。
    out.materialIndex = prim.GetInt("material", -1);
    if (out.materialIndex >= 0) {
        const json::Value& mats = (*ctx.root)["materials"];
        if (static_cast<size_t>(out.materialIndex) < mats.Size()) {
            const json::Value& pbr =
                mats[static_cast<size_t>(out.materialIndex)]["pbrMetallicRoughness"];
            if (const json::Value* bcf = pbr.Find("baseColorFactor")) {
                if (bcf->Size() == 4) {
                    out.baseColor = XMFLOAT4{ (*bcf)[0].Flt(1.0f), (*bcf)[1].Flt(1.0f),
                                              (*bcf)[2].Flt(1.0f), (*bcf)[3].Flt(1.0f) };
                }
            }
            // texture のインデックス → textures[i].source → images[j] を辿る共通処理。
            const json::Value& textures = (*ctx.root)["textures"];
            auto imageOf = [&](int texIndex) -> int {
                if (texIndex < 0 || static_cast<size_t>(texIndex) >= textures.Size()) return -1;
                return textures[static_cast<size_t>(texIndex)].GetInt("source", -1);
            };

            out.baseColorImage = imageOf(pbr["baseColorTexture"].GetInt("index", -1));

            // ---- ステージL-02: PBR の追加情報 ----
            out.metallicFactor  = pbr.GetFlt("metallicFactor", 1.0f);
            out.roughnessFactor = pbr.GetFlt("roughnessFactor", 1.0f);
            // glTF の metallicRoughnessTexture は G=粗さ / B=金属度。R（AO）は
            // 本来 occlusionTexture 側だが、実務上は同じ画像に詰められていることが多いので
            // そのまま ORM テクスチャとして扱う。
            out.ormImage = imageOf(pbr["metallicRoughnessTexture"].GetInt("index", -1));

            const json::Value& mat = mats[static_cast<size_t>(out.materialIndex)];
            out.normalImage = imageOf(mat["normalTexture"].GetInt("index", -1));
            out.normalScale = mat["normalTexture"].GetFlt("scale", 1.0f);
            if (out.ormImage < 0) {
                out.ormImage = imageOf(mat["occlusionTexture"].GetInt("index", -1));
            }
        }
    }

    model.totalVertices += out.vertices.size();
    model.totalIndices  += out.indices.size();
    model.primitives.push_back(std::move(out));
    return true;
}

// ノードを再帰的に辿り、メッシュを持つノードのプリミティブを読み出す。
bool LoadNode(GltfContext& ctx, int nodeIndex, const XMMATRIX& parent,
              GltfModel& model, int depth) {
    if (depth > 64) return ctx.Fail("ノード階層が深すぎます（循環参照の疑い）");

    const json::Value& nodes = (*ctx.root)["nodes"];
    if (nodeIndex < 0 || static_cast<size_t>(nodeIndex) >= nodes.Size()) return true;
    const json::Value& node = nodes[static_cast<size_t>(nodeIndex)];

    const XMMATRIX world = NodeLocalMatrix(node) * parent;

    if (const json::Value* meshRef = node.Find("mesh")) {
        const json::Value& meshes = (*ctx.root)["meshes"];
        const int mi = meshRef->Int(-1);
        if (mi >= 0 && static_cast<size_t>(mi) < meshes.Size()) {
            const int skinIndex = node.GetInt("skin", -1);
            // ★glTF 仕様の重要な決まり★
            // スキン付きメッシュでは「そのメッシュを持つノードの変換は無視する」。
            // 頂点はスキン空間にあり、ジョイント行列だけで世界へ運ばれるためで、
            // ここでノード変換を焼き込むと二重に掛かってモデルが吹き飛ぶ。
            const XMMATRIX meshWorld = (skinIndex >= 0) ? XMMatrixIdentity() : world;
            const json::Value& prims = meshes[static_cast<size_t>(mi)]["primitives"];
            for (size_t i = 0; i < prims.Size(); ++i) {
                if (!LoadPrimitive(ctx, prims[i], meshWorld, model, skinIndex)) return false;
            }
        }
    }

    if (const json::Value* children = node.Find("children")) {
        for (size_t i = 0; i < children->Size(); ++i) {
            if (!LoadNode(ctx, (*children)[i].Int(-1), world, model, depth + 1)) return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// GLB コンテナの分解
// ---------------------------------------------------------------------------
// GLB は次の並び:
//   [12B ヘッダ] magic='glTF'(0x46546C67) / version=2 / 全体長
//   [チャンク...] 各チャンク = 長さ(4B) + 種別(4B) + データ（4 バイト境界パディング込み）
//     種別 0x4E4F534A = 'JSON' … シーン記述
//     種別 0x004E4942 = 'BIN\0' … 頂点や画像の実体
bool SplitGlb(const std::vector<uint8_t>& data,
              std::string& jsonOut, std::vector<uint8_t>& binOut, std::string& error) {
    if (data.size() < 12) { error = "GLB が短すぎます"; return false; }

    uint32_t magic = 0, version = 0, length = 0;
    memcpy(&magic,   data.data() + 0, 4);
    memcpy(&version, data.data() + 4, 4);
    memcpy(&length,  data.data() + 8, 4);

    if (magic != 0x46546C67u) { error = "GLB のマジックが 'glTF' ではありません"; return false; }
    if (version != 2)         { error = "GLB のバージョンが 2 ではありません"; return false; }
    if (length > data.size()) { error = "GLB のヘッダ長がファイルサイズを超えています"; return false; }

    size_t offset = 12;
    while (offset + 8 <= length) {
        uint32_t chunkLen = 0, chunkType = 0;
        memcpy(&chunkLen,  data.data() + offset + 0, 4);
        memcpy(&chunkType, data.data() + offset + 4, 4);
        offset += 8;
        if (offset + chunkLen > length) { error = "GLB チャンクが範囲を超えています"; return false; }

        const uint8_t* p = data.data() + offset;
        if (chunkType == 0x4E4F534Au) {          // 'JSON'
            jsonOut.assign(reinterpret_cast<const char*>(p), chunkLen);
        } else if (chunkType == 0x004E4942u) {   // 'BIN\0'
            binOut.assign(p, p + chunkLen);
        }
        // 各チャンクは 4 バイト境界に揃えられている。
        offset += (chunkLen + 3u) & ~3u;
    }

    if (jsonOut.empty()) { error = "GLB に JSON チャンクがありません"; return false; }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// 公開 API
// ---------------------------------------------------------------------------
bool LoadGltf(const std::string& path, GltfModel& out, std::string* error) {
    out = GltfModel{};
    std::string err;

    std::vector<uint8_t> raw;
    if (!ReadWholeFile(path, raw)) {
        if (error) *error = "ファイルを開けません: " + path;
        return false;
    }

    // 拡張子ではなくマジックで判定する（.gltf という名前の GLB もあり得るため）。
    const bool isGlb = (raw.size() >= 4) && (memcmp(raw.data(), "glTF", 4) == 0);

    std::string jsonText;
    std::vector<uint8_t> glbBin;
    if (isGlb) {
        if (!SplitGlb(raw, jsonText, glbBin, err)) {
            if (error) *error = err;
            return false;
        }
    } else {
        jsonText.assign(reinterpret_cast<const char*>(raw.data()), raw.size());
    }

    json::Value root;
    if (!json::Parse(jsonText.data(), jsonText.size(), root, &err)) {
        if (error) *error = "JSON 解析に失敗: " + err;
        return false;
    }
    if (!root.IsObject()) {
        if (error) *error = "glTF のルートがオブジェクトではありません";
        return false;
    }

    GltfContext ctx;
    ctx.root = &root;

    // buffers[] の実体を用意する。3 通りある:
    //   ①uri 無し            … GLB の BIN チャンク
    //   ②data URI (base64)   … その場でデコード
    //   ③外部ファイル        … .gltf からの相対パスで読む
    const json::Value& buffers = root["buffers"];
    const std::string  baseDir = DirectoryOf(path);
    ctx.buffers.resize(buffers.Size());
    for (size_t i = 0; i < buffers.Size(); ++i) {
        const json::Value& b = buffers[i];
        const json::Value* uri = b.Find("uri");
        if (uri == nullptr || !uri->IsString()) {
            if (i == 0 && !glbBin.empty()) {
                ctx.buffers[i] = glbBin;
            } else {
                if (error) *error = "uri の無い buffer に対応する BIN チャンクがありません";
                return false;
            }
            continue;
        }
        const std::string& u = uri->Str();
        const std::string kDataPrefix = "data:";
        if (u.rfind(kDataPrefix, 0) == 0) {
            const size_t comma = u.find(',');
            if (comma == std::string::npos) {
                if (error) *error = "data URI の書式が不正です";
                return false;
            }
            // base64 以外（平文の data URI）は事実上使われないので未対応。
            if (u.find("base64", 0) == std::string::npos || u.find("base64", 0) > comma) {
                if (error) *error = "base64 以外の data URI には未対応です";
                return false;
            }
            ctx.buffers[i] = Base64Decode(u.data() + comma + 1, u.size() - comma - 1);
        } else {
            if (!ReadWholeFile(baseDir + u, ctx.buffers[i])) {
                if (error) *error = "外部バッファを開けません: " + baseDir + u;
                return false;
            }
        }
    }

    // --- ステージL-01b: images[] をデコードする ---
    // glTF の画像の在り処は 3 通り:
    //   ①bufferView 参照   … GLB の BIN チャンクに埋め込み（.glb の標準的なやり方）
    //   ②data URI (base64) … JSON の中に直接埋め込み
    //   ③外部ファイル      … .gltf からの相対パス
    // どれも最終的に「PNG のバイト列」になるので、そこから先は共通で DecodePng に渡す。
    // 1 枚デコードに失敗してもモデル自体は単色で描けるので、致命化せず警告に留める。
    const json::Value& images = root["images"];
    out.images.resize(images.Size());
    for (size_t i = 0; i < images.Size(); ++i) {
        const json::Value& img = images[i];
        std::vector<uint8_t> bytes;
        const uint8_t* p = nullptr;
        size_t          n = 0;

        if (const json::Value* bvRef = img.Find("bufferView")) {
            const json::Value& views = root["bufferViews"];
            const int bvi = bvRef->Int(-1);
            if (bvi >= 0 && static_cast<size_t>(bvi) < views.Size()) {
                const json::Value& bv = views[static_cast<size_t>(bvi)];
                const int bufIndex = bv.GetInt("buffer", -1);
                if (bufIndex >= 0 && static_cast<size_t>(bufIndex) < ctx.buffers.size()) {
                    const std::vector<uint8_t>& buf = ctx.buffers[static_cast<size_t>(bufIndex)];
                    const size_t off = static_cast<size_t>(bv.GetInt("byteOffset", 0));
                    const size_t len = static_cast<size_t>(bv.GetInt("byteLength", 0));
                    if (off + len <= buf.size()) { p = buf.data() + off; n = len; }
                }
            }
        } else if (const json::Value* uriRef = img.Find("uri")) {
            const std::string& u = uriRef->Str();
            if (u.rfind("data:", 0) == 0) {
                const size_t comma = u.find(',');
                if (comma != std::string::npos && u.find("base64") < comma) {
                    bytes = Base64Decode(u.data() + comma + 1, u.size() - comma - 1);
                }
            } else {
                ReadWholeFile(baseDir + u, bytes);
            }
            p = bytes.data();
            n = bytes.size();
        }

        if (p == nullptr || n == 0) {
            LOG_WARN("glTF: images[%zu] のデータを取得できませんでした", i);
            continue;
        }

        PngImage png;
        std::string imgErr;
        if (DecodePng(p, n, png, &imgErr)) {
            out.images[i].width  = png.width;
            out.images[i].height = png.height;
            out.images[i].rgba   = std::move(png.rgba);
            out.images[i].valid  = true;
        } else {
            // JPEG のときはここに来る。PNG へ変換してもらう必要があるので理由を明記する。
            LOG_WARN("glTF: images[%zu] のデコードに失敗（%s）。単色で描画します。",
                     i, imgErr.c_str());
        }
    }

    // --- ステージL-04a: ノード木を保持する ---
    // アニメーションは「ノードの TRS を時間で書き換える」ものなので、
    // 再生のたびに階層を合成し直す必要がある＝木を読み込み時に残しておく。
    {
        const json::Value& nodes = root["nodes"];
        out.nodes.resize(nodes.Size());
        for (size_t i = 0; i < nodes.Size(); ++i) {
            const json::Value& n = nodes[i];
            GltfNode& dst = out.nodes[i];
            dst.name = n.GetStr("name");
            dst.mesh = n.GetInt("mesh", -1);
            dst.skin = n.GetInt("skin", -1);

            if (const json::Value* m = n.Find("matrix")) {
                if (m->Size() == 16) {
                    float v[16];
                    for (size_t k = 0; k < 16; ++k) v[k] = (*m)[k].Flt(0.0f);
                    XMStoreFloat4x4(&dst.matrix, MatrixFromGltfArray(v));
                    dst.hasMatrix = true;
                }
            }
            if (const json::Value* t = n.Find("translation")) {
                if (t->Size() == 3) {
                    dst.translation = XMFLOAT3{ (*t)[0].Flt(), (*t)[1].Flt(), (*t)[2].Flt() };
                }
            }
            if (const json::Value* r = n.Find("rotation")) {
                if (r->Size() == 4) {
                    dst.rotation = XMFLOAT4{ (*r)[0].Flt(), (*r)[1].Flt(),
                                             (*r)[2].Flt(), (*r)[3].Flt(1.0f) };
                }
            }
            if (const json::Value* s = n.Find("scale")) {
                if (s->Size() == 3) {
                    dst.scale = XMFLOAT3{ (*s)[0].Flt(1.0f), (*s)[1].Flt(1.0f), (*s)[2].Flt(1.0f) };
                }
            }
            if (const json::Value* c = n.Find("children")) {
                for (size_t k = 0; k < c->Size(); ++k) {
                    const int ci = (*c)[k].Int(-1);
                    if (ci >= 0) dst.children.push_back(ci);
                }
            }
        }
        // 親を逆引きで埋める（glTF は子リストしか持たない）
        for (size_t i = 0; i < out.nodes.size(); ++i) {
            for (int c : out.nodes[i].children) {
                if (c >= 0 && static_cast<size_t>(c) < out.nodes.size()) {
                    out.nodes[static_cast<size_t>(c)].parent = static_cast<int>(i);
                }
            }
        }
    }

    // --- ステージL-04a: スキン ---
    {
        const json::Value& skins = root["skins"];
        out.skins.resize(skins.Size());
        for (size_t i = 0; i < skins.Size(); ++i) {
            const json::Value& s = skins[i];
            GltfSkin& dst = out.skins[i];
            dst.name = s.GetStr("name");

            const json::Value& joints = s["joints"];
            dst.jointNodes.reserve(joints.Size());
            for (size_t k = 0; k < joints.Size(); ++k) {
                dst.jointNodes.push_back(joints[k].Int(-1));
            }

            // 逆バインド行列（省略時は単位行列）。
            dst.inverseBind.assign(dst.jointNodes.size(), XMFLOAT4X4{});
            for (auto& m : dst.inverseBind) XMStoreFloat4x4(&m, XMMatrixIdentity());

            const int ibmAcc = s.GetInt("inverseBindMatrices", -1);
            if (ibmAcc >= 0) {
                std::vector<float> mats;
                int comps = 0, count = 0;
                if (ctx.ReadFloats(ibmAcc, mats, comps, count) && comps == 16) {
                    const size_t n = (std::min)(static_cast<size_t>(count),
                                                dst.inverseBind.size());
                    for (size_t k = 0; k < n; ++k) {
                        XMStoreFloat4x4(&dst.inverseBind[k],
                                        MatrixFromGltfArray(&mats[k * 16]));
                    }
                } else {
                    LOG_WARN("glTF: skins[%zu] の inverseBindMatrices を読めません", i);
                }
            }
        }
    }

    // --- ステージL-04a: アニメーション ---
    {
        const json::Value& anims = root["animations"];
        out.animations.reserve(anims.Size());
        for (size_t i = 0; i < anims.Size(); ++i) {
            const json::Value& a = anims[i];
            GltfAnimation clip;
            clip.name = a.GetStr("name");
            if (clip.name.empty()) clip.name = "animation_" + std::to_string(i);

            const json::Value& samplers = a["samplers"];
            clip.samplers.resize(samplers.Size());
            for (size_t k = 0; k < samplers.Size(); ++k) {
                const json::Value& s = samplers[k];
                GltfAnimSampler& dst = clip.samplers[k];

                int comps = 0, count = 0;
                if (!ctx.ReadFloats(s.GetInt("input", -1), dst.times, comps, count) ||
                    comps != 1) {
                    LOG_WARN("glTF: animations[%zu].samplers[%zu] の時刻を読めません", i, k);
                    dst.times.clear();
                    continue;
                }
                if (!ctx.ReadFloats(s.GetInt("output", -1), dst.values, dst.comps, count)) {
                    LOG_WARN("glTF: animations[%zu].samplers[%zu] の値を読めません", i, k);
                    dst.times.clear();
                    continue;
                }
                const std::string interp = s.GetStr("interpolation", "LINEAR");
                if (interp == "STEP")             dst.interp = GltfAnimSampler::Interp::Step;
                else if (interp == "CUBICSPLINE") dst.interp = GltfAnimSampler::Interp::CubicSpline;
                else                              dst.interp = GltfAnimSampler::Interp::Linear;

                if (!dst.times.empty()) {
                    clip.duration = (std::max)(clip.duration, dst.times.back());
                }
            }

            const json::Value& channels = a["channels"];
            clip.channels.reserve(channels.Size());
            for (size_t k = 0; k < channels.Size(); ++k) {
                const json::Value& c = channels[k];
                GltfAnimChannel ch;
                ch.sampler = c.GetInt("sampler", -1);
                ch.node    = c["target"].GetInt("node", -1);
                const std::string path = c["target"].GetStr("path");
                if (path == "translation")   ch.path = GltfAnimChannel::Path::Translation;
                else if (path == "rotation") ch.path = GltfAnimChannel::Path::Rotation;
                else if (path == "scale")    ch.path = GltfAnimChannel::Path::Scale;
                else                         ch.path = GltfAnimChannel::Path::Unsupported;
                // weights（モーフターゲット）は未対応。読み飛ばす。
                if (ch.node >= 0 && ch.sampler >= 0 &&
                    ch.path != GltfAnimChannel::Path::Unsupported) {
                    clip.channels.push_back(ch);
                }
            }
            out.animations.push_back(std::move(clip));
        }
    }

    // シーンのルートノードから辿る。scene が無ければ scenes[0]、それも無ければ全ノード。
    const json::Value& scenes = root["scenes"];
    const int sceneIndex = root.GetInt("scene", 0);
    bool traversed = false;
    if (sceneIndex >= 0 && static_cast<size_t>(sceneIndex) < scenes.Size()) {
        const json::Value& nodes = scenes[static_cast<size_t>(sceneIndex)]["nodes"];
        for (size_t i = 0; i < nodes.Size(); ++i) {
            if (!LoadNode(ctx, nodes[i].Int(-1), XMMatrixIdentity(), out, 0)) {
                if (error) *error = ctx.error;
                return false;
            }
            out.rootNodes.push_back(nodes[i].Int(-1));   // L-04a: 再生時の起点
            traversed = true;
        }
    }
    if (!traversed) {
        // シーン定義が無い変則的なファイルへの保険。全ノードを親無しで読む。
        const json::Value& nodes = root["nodes"];
        for (size_t i = 0; i < nodes.Size(); ++i) {
            if (!LoadNode(ctx, static_cast<int>(i), XMMatrixIdentity(), out, 0)) {
                if (error) *error = ctx.error;
                return false;
            }
        }
        // 親を持たないノードをルートとみなす。
        for (size_t i = 0; i < out.nodes.size(); ++i) {
            if (out.nodes[i].parent < 0) out.rootNodes.push_back(static_cast<int>(i));
        }
    }

    if (out.primitives.empty()) {
        if (error) *error = "描画できる primitive が 1 つもありませんでした";
        return false;
    }

    // バウンディングボックス（配置スケールを決めるのに使う）。
    XMFLOAT3 lo{ 1e30f, 1e30f, 1e30f }, hi{ -1e30f, -1e30f, -1e30f };
    for (const GltfPrimitive& prim : out.primitives) {
        for (const TexVertex& v : prim.vertices) {
            lo.x = std::min(lo.x, v.position.x); hi.x = std::max(hi.x, v.position.x);
            lo.y = std::min(lo.y, v.position.y); hi.y = std::max(hi.y, v.position.y);
            lo.z = std::min(lo.z, v.position.z); hi.z = std::max(hi.z, v.position.z);
        }
    }
    out.boundsMin = lo;
    out.boundsMax = hi;
    out.valid     = true;
    return true;
}

std::vector<PbrVertex> MakePbrVertices(const GltfPrimitive& prim) {
    std::vector<PbrVertex> out(prim.vertices.size());
    const bool hasTangents = (prim.tangents.size() == prim.vertices.size());
    for (size_t i = 0; i < prim.vertices.size(); ++i) {
        out[i].position = prim.vertices[i].position;
        out[i].normal   = prim.vertices[i].normal;
        out[i].uv       = prim.vertices[i].uv;
        out[i].tangent  = hasTangents ? prim.tangents[i]
                                      : XMFLOAT4{ 1.0f, 0.0f, 0.0f, 1.0f };
    }
    return out;
}

std::vector<SkinnedPbrVertex> MakeSkinnedPbrVertices(const GltfPrimitive& prim) {
    std::vector<SkinnedPbrVertex> out(prim.vertices.size());
    const bool hasTangents = (prim.tangents.size() == prim.vertices.size());
    const bool hasSkin = (prim.joints.size() == prim.vertices.size()) &&
                         (prim.weights.size() == prim.vertices.size());
    for (size_t i = 0; i < prim.vertices.size(); ++i) {
        out[i].position = prim.vertices[i].position;
        out[i].normal   = prim.vertices[i].normal;
        out[i].uv       = prim.vertices[i].uv;
        out[i].tangent  = hasTangents ? prim.tangents[i]
                                      : XMFLOAT4{ 1.0f, 0.0f, 0.0f, 1.0f };
        out[i].joints   = hasSkin ? prim.joints[i]  : XMUINT4{ 0, 0, 0, 0 };
        out[i].weights  = hasSkin ? prim.weights[i] : XMFLOAT4{ 1.0f, 0.0f, 0.0f, 0.0f };
    }
    return out;
}

std::vector<Vertex> MakeLitVertices(const GltfPrimitive& prim) {
    std::vector<Vertex> out(prim.vertices.size());
    for (size_t i = 0; i < prim.vertices.size(); ++i) {
        out[i].position = prim.vertices[i].position;
        out[i].normal   = prim.vertices[i].normal;
        out[i].color    = prim.baseColor;
    }
    return out;
}
