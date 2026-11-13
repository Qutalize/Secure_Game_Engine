#include "engine/renderer/ObjLoader.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

#include "engine/core/Logger.h"

using namespace DirectX;
namespace fs = std::filesystem;

namespace {

// 実行ディレクトリの違いを吸収して最初に開けたパスでストリームを返す。
// resolved に実際に開けたパスを格納する（.mtl の相対解決に使う）。
std::ifstream OpenWithFallback(const std::string& path, std::string& resolved) {
    const char* prefixes[] = { "", "../", "../../" };
    for (const char* pre : prefixes) {
        std::string p = std::string(pre) + path;
        std::ifstream f(p);
        if (f) {
            resolved = p;
            return f;
        }
    }
    return std::ifstream(); // 開けない
}

// .mtl を読み、マテリアル名 -> Kd(拡散色) のマップを作る。
void LoadMtl(const std::string& mtlPath,
             std::unordered_map<std::string, XMFLOAT4>& outColors) {
    std::ifstream f(mtlPath);
    if (!f) return;

    std::string line;
    std::string current;
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string tag;
        ss >> tag;
        if (tag == "newmtl") {
            ss >> current;
            outColors[current] = XMFLOAT4{ 0.8f, 0.8f, 0.8f, 1.0f }; // 既定
        } else if (tag == "Kd" && !current.empty()) {
            float r = 0, g = 0, b = 0;
            ss >> r >> g >> b;
            outColors[current] = XMFLOAT4{ r, g, b, 1.0f };
        }
    }
}

// f の 1 頂点参照（"v", "v/vt", "v//vn", "v/vt/vn"）を解析する。
// obj のインデックスは 1 始まり。未指定は 0 のまま。
struct FaceRef { int v = 0; int vt = 0; int vn = 0; };

FaceRef ParseRef(const std::string& tok) {
    FaceRef r;
    size_t s1 = tok.find('/');
    if (s1 == std::string::npos) {
        r.v = std::atoi(tok.c_str());
        return r;
    }
    r.v = std::atoi(tok.substr(0, s1).c_str());

    size_t s2 = tok.find('/', s1 + 1);
    if (s2 == std::string::npos) {
        r.vt = std::atoi(tok.substr(s1 + 1).c_str());
        return r;
    }
    if (s2 > s1 + 1) {
        r.vt = std::atoi(tok.substr(s1 + 1, s2 - s1 - 1).c_str());
    }
    if (s2 + 1 < tok.size()) {
        r.vn = std::atoi(tok.substr(s2 + 1).c_str());
    }
    return r;
}

} // namespace

bool ObjLoader::Load(const std::string& path,
                     std::vector<Vertex>& outVertices,
                     std::vector<uint32_t>& outIndices) {
    std::string resolved;
    std::ifstream f = OpenWithFallback(path, resolved);
    if (!f) {
        return false; // 呼び出し側がフォールバックする
    }
    const fs::path baseDir = fs::path(resolved).parent_path();

    std::vector<XMFLOAT3> positions;
    std::vector<XMFLOAT3> normals;
    std::unordered_map<std::string, XMFLOAT4> matColors;
    XMFLOAT4 curColor{ 0.8f, 0.8f, 0.8f, 1.0f };

    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;

        std::istringstream ss(line);
        std::string tag;
        ss >> tag;

        if (tag == "v") {
            float x = 0, y = 0, z = 0;
            ss >> x >> y >> z;
            positions.push_back(XMFLOAT3{ x, y, z });
        } else if (tag == "vn") {
            float x = 0, y = 0, z = 0;
            ss >> x >> y >> z;
            normals.push_back(XMFLOAT3{ x, y, z });
        } else if (tag == "mtllib") {
            std::string mtl;
            ss >> mtl;
            LoadMtl((baseDir / mtl).string(), matColors);
        } else if (tag == "usemtl") {
            std::string m;
            ss >> m;
            auto it = matColors.find(m);
            if (it != matColors.end()) curColor = it->second;
        } else if (tag == "f") {
            std::vector<FaceRef> refs;
            std::string tok;
            while (ss >> tok) refs.push_back(ParseRef(tok));
            if (refs.size() < 3) continue;

            // 多角形をファン三角形化（v0, vi, vi+1）
            for (size_t i = 1; i + 1 < refs.size(); ++i) {
                const FaceRef tri[3] = { refs[0], refs[i], refs[i + 1] };

                // 頂点インデックスの範囲チェック（壊れた obj で落ちないように）
                bool valid = true;
                for (const FaceRef& r : tri) {
                    if (r.v <= 0 || static_cast<size_t>(r.v) > positions.size()) {
                        valid = false;
                        break;
                    }
                }
                if (!valid) continue;

                // 法線が無い頂点用に面法線を計算しておく
                XMFLOAT3 faceNormal{ 0.0f, 1.0f, 0.0f };
                {
                    XMVECTOR p0 = XMLoadFloat3(&positions[tri[0].v - 1]);
                    XMVECTOR p1 = XMLoadFloat3(&positions[tri[1].v - 1]);
                    XMVECTOR p2 = XMLoadFloat3(&positions[tri[2].v - 1]);
                    XMVECTOR n  = XMVector3Cross(XMVectorSubtract(p1, p0),
                                                 XMVectorSubtract(p2, p0));
                    XMStoreFloat3(&faceNormal, XMVector3Normalize(n));
                }

                for (const FaceRef& r : tri) {
                    Vertex vert;
                    vert.position = positions[r.v - 1];
                    vert.normal   = (r.vn > 0 &&
                                     static_cast<size_t>(r.vn) <= normals.size())
                                        ? normals[r.vn - 1]
                                        : faceNormal;
                    vert.color    = curColor;

                    outIndices.push_back(static_cast<uint32_t>(outVertices.size()));
                    outVertices.push_back(vert);
                }
            }
        }
    }

    if (outVertices.empty()) {
        LOG_WARN("ObjLoader: %s に頂点がありませんでした", resolved.c_str());
        return false;
    }
    LOG_INFO("ObjLoader: %s を読み込み（頂点 %zu / 三角形 %zu）",
             resolved.c_str(), outVertices.size(), outIndices.size() / 3);
    return true;
}
