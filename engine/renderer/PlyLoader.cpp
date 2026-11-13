#include "engine/renderer/PlyLoader.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace gs {

namespace {

enum class ScalarType { I8, U8, I16, U16, I32, U32, F32, F64, Unknown };

int TypeSize(ScalarType t) {
    switch (t) {
        case ScalarType::I8:  case ScalarType::U8:  return 1;
        case ScalarType::I16: case ScalarType::U16: return 2;
        case ScalarType::I32: case ScalarType::U32: case ScalarType::F32: return 4;
        case ScalarType::F64: return 8;
        default: return 0;
    }
}

ScalarType ParseType(const std::string& s) {
    if (s == "char"   || s == "int8")    return ScalarType::I8;
    if (s == "uchar"  || s == "uint8")   return ScalarType::U8;
    if (s == "short"  || s == "int16")   return ScalarType::I16;
    if (s == "ushort" || s == "uint16")  return ScalarType::U16;
    if (s == "int"    || s == "int32")   return ScalarType::I32;
    if (s == "uint"   || s == "uint32")  return ScalarType::U32;
    if (s == "float"  || s == "float32") return ScalarType::F32;
    if (s == "double" || s == "float64") return ScalarType::F64;
    return ScalarType::Unknown;
}

struct Property {
    std::string name;
    ScalarType  type = ScalarType::Unknown;
    bool        isList = false;
    ScalarType  listCountType = ScalarType::U8;
    int         offset = 0;   // 固定長レコード内のバイト位置（非リスト時のみ有効）
};

enum class Format { Ascii, BinaryLE, BinaryBE };

double ReadScalarBinary(const uint8_t* p, ScalarType t, bool bigEndian) {
    uint8_t buf[8];
    int n = TypeSize(t);
    std::memcpy(buf, p, static_cast<size_t>(n));
    if (bigEndian && n > 1) std::reverse(buf, buf + n);
    switch (t) {
        case ScalarType::I8:  { int8_t   v; std::memcpy(&v, buf, 1); return v; }
        case ScalarType::U8:  { uint8_t  v; std::memcpy(&v, buf, 1); return v; }
        case ScalarType::I16: { int16_t  v; std::memcpy(&v, buf, 2); return v; }
        case ScalarType::U16: { uint16_t v; std::memcpy(&v, buf, 2); return v; }
        case ScalarType::I32: { int32_t  v; std::memcpy(&v, buf, 4); return v; }
        case ScalarType::U32: { uint32_t v; std::memcpy(&v, buf, 4); return v; }
        case ScalarType::F32: { float    v; std::memcpy(&v, buf, 4); return v; }
        case ScalarType::F64: { double   v; std::memcpy(&v, buf, 8); return v; }
        default: return 0.0;
    }
}

float Sigmoid(float x) { return 1.0f / (1.0f + std::exp(-x)); }

}  // namespace

bool LoadGaussianPly(const std::string& path,
                     const PlyLoadOptions& options,
                     GaussianCloud& out,
                     std::string* error) {
    auto fail = [&](const std::string& m) {
        if (error) *error = m;
        return false;
    };

    out = GaussianCloud{};

    std::ifstream f(path, std::ios::binary);
    if (!f) return fail("ファイルを開けません: " + path);
    std::string blob((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (blob.size() < 16) return fail(".ply が小さすぎます");

    // ---- ヘッダ終端を探す ----
    const std::string kEnd = "end_header";
    size_t endPos = blob.find(kEnd);
    if (endPos == std::string::npos) return fail("end_header が見つかりません（PLY ではない？）");
    size_t dataStart = blob.find('\n', endPos);
    if (dataStart == std::string::npos) return fail("ヘッダが壊れています");
    dataStart += 1;

    std::string header = blob.substr(0, endPos);
    std::istringstream hs(header);
    std::string line;

    Format format = Format::Ascii;
    long long vertexCount = -1;
    std::vector<Property> vprops;
    std::string currentElement;
    // element ごとの件数（vertex 以外は binary のときにスキップ幅計算へ）。
    std::vector<std::pair<std::string, long long>> elements;

    if (!std::getline(hs, line) || line.substr(0, 3) != "ply")
        return fail("先頭が 'ply' ではありません");

    while (std::getline(hs, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream ls(line);
        std::string tok;
        ls >> tok;
        if (tok == "format") {
            std::string fmt; ls >> fmt;
            if      (fmt == "ascii")                 format = Format::Ascii;
            else if (fmt == "binary_little_endian")  format = Format::BinaryLE;
            else if (fmt == "binary_big_endian")     format = Format::BinaryBE;
            else return fail("未知の format: " + fmt);
        } else if (tok == "comment") {
            // 無視
        } else if (tok == "element") {
            std::string ename; long long ecount = 0;
            ls >> ename >> ecount;
            currentElement = ename;
            elements.emplace_back(ename, ecount);
            if (ename == "vertex") vertexCount = ecount;
        } else if (tok == "property") {
            std::string t1; ls >> t1;
            Property p;
            if (t1 == "list") {
                std::string countT, itemT, name;
                ls >> countT >> itemT >> name;
                p.isList = true;
                p.listCountType = ParseType(countT);
                p.type = ParseType(itemT);
                p.name = name;
            } else {
                std::string name; ls >> name;
                p.type = ParseType(t1);
                p.name = name;
            }
            if (currentElement == "vertex") vprops.push_back(p);
        }
        // "end_header" はここには来ない（substr で切っているため）
    }

    if (vertexCount < 0) return fail("element vertex がありません");
    if (vprops.empty()) return fail("vertex の property がありません");

    // ---- 固定長レコードのオフセットを確定 ----
    int recordStride = 0;
    for (auto& p : vprops) {
        if (p.isList) return fail("vertex に list property があります（3DGS .ply では非対応）");
        if (p.type == ScalarType::Unknown) return fail("未知の property 型: " + p.name);
        p.offset = recordStride;
        recordStride += TypeSize(p.type);
    }

    // ---- 名前 → property index ----
    std::unordered_map<std::string, int> idx;
    for (int i = 0; i < static_cast<int>(vprops.size()); ++i) idx[vprops[i].name] = i;

    auto has = [&](const char* n) { return idx.count(n) != 0; };
    for (const char* n : { "x", "y", "z", "opacity",
                           "scale_0", "scale_1", "scale_2",
                           "rot_0", "rot_1", "rot_2", "rot_3",
                           "f_dc_0", "f_dc_1", "f_dc_2" }) {
        if (!has(n)) return fail(std::string("3DGS 必須プロパティが不足: ") + n);
    }

    // f_rest_* を数える
    int restCount = 0;
    while (has(("f_rest_" + std::to_string(restCount)).c_str())) ++restCount;
    int restPerChannel = restCount / 3;
    int shDegree = restPerChannel == 0 ? 0
                 : restPerChannel == 3 ? 1
                 : restPerChannel == 8 ? 2
                 : restPerChannel == 15 ? 3 : 0;

    // ---- 頂点データを走査 ----
    const int stride = std::max(1, options.stride);
    const float ps = options.positionScale;

    out.splats.reserve(static_cast<size_t>(vertexCount) / static_cast<size_t>(stride) + 1);
    out.sh.restCoeffsPerChannel = restPerChannel;
    if (restPerChannel > 0)
        out.sh.rest.reserve(out.splats.capacity() * static_cast<size_t>(restPerChannel) * 3);
    out.shDegree = shDegree;

    std::vector<double> asciiRow;  // ascii 用の 1 行バッファ

    if (format == Format::Ascii) {
        // ヘッダの後ろ全体を空白トークンとして読む
        std::istringstream ds(blob.substr(dataStart));
        auto readRow = [&](std::vector<double>& row) -> bool {
            row.resize(vprops.size());
            for (size_t i = 0; i < vprops.size(); ++i)
                if (!(ds >> row[i])) return false;
            return true;
        };
        auto get = [&](const std::vector<double>& row, const char* n) {
            return static_cast<float>(row[static_cast<size_t>(idx[n])]);
        };
        for (long long v = 0; v < vertexCount; ++v) {
            if (!readRow(asciiRow)) return fail("ascii 頂点データが途中で終わっています");
            if (v % stride != 0) continue;
            GaussianSplat g;
            g.position[0] = get(asciiRow, "x") * ps;
            g.position[1] = get(asciiRow, "y") * ps;
            g.position[2] = get(asciiRow, "z") * ps;
            g.scale[0] = std::exp(get(asciiRow, "scale_0")) * ps;
            g.scale[1] = std::exp(get(asciiRow, "scale_1")) * ps;
            g.scale[2] = std::exp(get(asciiRow, "scale_2")) * ps;
            float w = get(asciiRow, "rot_0"), x = get(asciiRow, "rot_1"),
                  y = get(asciiRow, "rot_2"), z = get(asciiRow, "rot_3");
            float len = std::sqrt(w*w + x*x + y*y + z*z);
            if (len < 1e-8f) { w = 1; x = y = z = 0; len = 1; }
            g.rotation[0] = x/len; g.rotation[1] = y/len; g.rotation[2] = z/len; g.rotation[3] = w/len;
            g.opacity = Sigmoid(get(asciiRow, "opacity"));
            for (int c = 0; c < 3; ++c) {
                float dc = get(asciiRow, ("f_dc_" + std::to_string(c)).c_str());
                g.color[c] = std::min(1.0f, std::max(0.0f, 0.5f + kSH_C0 * dc));
            }
            out.splats.push_back(g);
            for (int r = 0; r < restCount; ++r)
                out.sh.rest.push_back(get(asciiRow, ("f_rest_" + std::to_string(r)).c_str()));
        }
    } else {
        const bool be = (format == Format::BinaryBE);
        // vertex より前の element（通常無いが念のため）は 3DGS .ply では発生しない前提。
        size_t need = dataStart + static_cast<size_t>(vertexCount) * static_cast<size_t>(recordStride);
        if (blob.size() < need)
            return fail("binary 本体が短すぎます（期待 " + std::to_string(need) +
                        " / 実際 " + std::to_string(blob.size()) + "）");
        const uint8_t* base = reinterpret_cast<const uint8_t*>(blob.data()) + dataStart;

        auto get = [&](const uint8_t* rec, const char* n) {
            const Property& p = vprops[static_cast<size_t>(idx[n])];
            return static_cast<float>(ReadScalarBinary(rec + p.offset, p.type, be));
        };
        for (long long v = 0; v < vertexCount; ++v) {
            if (v % stride != 0) continue;
            const uint8_t* rec = base + static_cast<size_t>(v) * static_cast<size_t>(recordStride);
            GaussianSplat g;
            g.position[0] = get(rec, "x") * ps;
            g.position[1] = get(rec, "y") * ps;
            g.position[2] = get(rec, "z") * ps;
            g.scale[0] = std::exp(get(rec, "scale_0")) * ps;
            g.scale[1] = std::exp(get(rec, "scale_1")) * ps;
            g.scale[2] = std::exp(get(rec, "scale_2")) * ps;
            float w = get(rec, "rot_0"), x = get(rec, "rot_1"),
                  y = get(rec, "rot_2"), z = get(rec, "rot_3");
            float len = std::sqrt(w*w + x*x + y*y + z*z);
            if (len < 1e-8f) { w = 1; x = y = z = 0; len = 1; }
            g.rotation[0] = x/len; g.rotation[1] = y/len; g.rotation[2] = z/len; g.rotation[3] = w/len;
            g.opacity = Sigmoid(get(rec, "opacity"));
            for (int c = 0; c < 3; ++c) {
                float dc = get(rec, ("f_dc_" + std::to_string(c)).c_str());
                g.color[c] = std::min(1.0f, std::max(0.0f, 0.5f + kSH_C0 * dc));
            }
            out.splats.push_back(g);
            for (int r = 0; r < restCount; ++r)
                out.sh.rest.push_back(get(rec, ("f_rest_" + std::to_string(r)).c_str()));
        }
    }

    if (out.splats.empty()) return fail("有効なガウシアンが 0 個です");

    out.RecomputeBounds();
    out.valid = true;
    return true;
}

}  // namespace gs
