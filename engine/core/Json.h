#pragma once
//
// 最小の JSON パーサ（ヘッダオンリー・依存なし）。ステージL-01a で追加。
//
// なぜ自作したか:
//   glTF 2.0 のシーン記述は JSON なので、モデルを読むには JSON パーサが要る。
//   定番の nlohmann/json は 2 万行超のヘッダで、本プロジェクトの「依存を最小に保つ」
//   方針（アロケータ・.pak・AES 以外は自作）に合わない。glTF が使う JSON は
//   オブジェクト／配列／数値／文字列／真偽値／null だけの素直なものなので、
//   再帰下降パーサ 1 本で十分に読める。
//
// 設計方針:
//   ・速度より読みやすさ優先。オブジェクトのキー検索は線形探索（glTF のオブジェクトは
//     メンバ数が少ないので実測上問題にならない）。
//   ・例外を投げない。失敗は戻り値と error 文字列で返す（プロジェクト方針に合わせる）。
//   ・木構造をそのまま値として持つ（Value のコピーは重いので const 参照で回すこと）。
//
#include <cstdint>
#include <cstdlib>
#include <cstring>   // strlen / memcpy
#include <string>
#include <utility>
#include <vector>

namespace json {

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type        type    = Type::Null;
    bool        boolean = false;
    double      number  = 0.0;
    std::string text;                                    // String
    std::vector<Value> items;                            // Array
    std::vector<std::pair<std::string, Value>> members;  // Object

    bool IsNull()   const { return type == Type::Null;   }
    bool IsBool()   const { return type == Type::Bool;   }
    bool IsNumber() const { return type == Type::Number; }
    bool IsString() const { return type == Type::String; }
    bool IsArray()  const { return type == Type::Array;  }
    bool IsObject() const { return type == Type::Object; }

    // 常に有効な参照を返すための「空の値」。見つからなかった場合に返す。
    static const Value& Null() {
        static const Value kNull;
        return kNull;
    }

    // --- オブジェクト用 ---
    // キーで引く。無ければ nullptr（存在判定に使う）。
    const Value* Find(const char* key) const {
        if (type != Type::Object) return nullptr;
        for (const auto& kv : members) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }
    // キーで引く。無ければ空の値（連鎖アクセスに使う: v["a"]["b"] が安全に空になる）。
    const Value& operator[](const char* key) const {
        const Value* v = Find(key);
        return v ? *v : Null();
    }
    bool Has(const char* key) const { return Find(key) != nullptr; }

    // --- 配列用 ---
    size_t Size() const {
        if (type == Type::Array)  return items.size();
        if (type == Type::Object) return members.size();
        return 0;
    }
    const Value& operator[](size_t i) const {
        return (type == Type::Array && i < items.size()) ? items[i] : Null();
    }
    // ★int 版が必要な理由★
    // C++ では整数リテラル 0 は「ヌルポインタ定数」でもあるため、v[0] と書くと
    // operator[](size_t) と operator[](const char*) のどちらとも解釈でき、
    // コンパイルエラー（あいまいな呼び出し）になる。int を完全一致で受ける版を
    // 用意しておくと、そちらが優先されて曖昧さが消える。
    const Value& operator[](int i) const {
        return (i < 0) ? Null() : (*this)[static_cast<size_t>(i)];
    }

    // --- 値の取り出し（型が違えば既定値を返す） ---
    double      Num(double def = 0.0) const { return IsNumber() ? number : def; }
    int         Int(int def = 0) const {
        return IsNumber() ? static_cast<int>(number) : def;
    }
    float       Flt(float def = 0.0f) const {
        return IsNumber() ? static_cast<float>(number) : def;
    }
    bool        Bool(bool def = false) const { return IsBool() ? boolean : def; }
    const std::string& Str() const {
        static const std::string kEmpty;
        return IsString() ? text : kEmpty;
    }

    // --- オブジェクトのメンバをまとめて取り出す糖衣 ---
    int GetInt(const char* key, int def = 0) const {
        const Value* v = Find(key);
        return v ? v->Int(def) : def;
    }
    float GetFlt(const char* key, float def = 0.0f) const {
        const Value* v = Find(key);
        return v ? v->Flt(def) : def;
    }
    std::string GetStr(const char* key, const char* def = "") const {
        const Value* v = Find(key);
        return (v && v->IsString()) ? v->text : std::string(def);
    }
};

namespace detail {

// 再帰下降パーサ本体。
// p を進めながら値を組み立てる。失敗したら err にメッセージを入れて false を返す。
struct Parser {
    const char* p   = nullptr;
    const char* end = nullptr;
    std::string err;
    int depth = 0;

    // 壊れた／悪意ある入力で無限に再帰しないための上限。
    // glTF の入れ子はせいぜい数段なので 64 あれば十分。
    static constexpr int kMaxDepth = 64;

    bool Fail(const char* msg) {
        if (err.empty()) {
            err = msg;
        }
        return false;
    }

    void SkipWs() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
            ++p;
        }
    }

    bool Literal(const char* lit) {
        const size_t n = strlen(lit);
        if (static_cast<size_t>(end - p) < n) return false;
        for (size_t i = 0; i < n; ++i) {
            if (p[i] != lit[i]) return false;
        }
        p += n;
        return true;
    }

    // コードポイントを UTF-8 で追記する（\uXXXX の展開に使う）。
    static void AppendUtf8(std::string& s, uint32_t cp) {
        if (cp < 0x80) {
            s.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool ParseHex4(uint32_t& out) {
        if (end - p < 4) return Fail("\\u の桁が足りません");
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = p[i];
            uint32_t d;
            if (c >= '0' && c <= '9')      d = static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') d = static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = static_cast<uint32_t>(c - 'A' + 10);
            else return Fail("\\u に 16 進数以外の文字があります");
            out = (out << 4) | d;
        }
        p += 4;
        return true;
    }

    bool ParseString(std::string& out) {
        if (p >= end || *p != '"') return Fail("文字列の開始 \" がありません");
        ++p;
        out.clear();
        while (p < end) {
            const char c = *p++;
            if (c == '"') return true;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (p >= end) return Fail("エスケープが途中で終わっています");
            const char e = *p++;
            switch (e) {
                case '"':  out.push_back('"');  break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/');  break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!ParseHex4(cp)) return false;
                    // サロゲートペア（BMP 外の文字）は 2 つの \u で表される。
                    if (cp >= 0xD800 && cp <= 0xDBFF && (end - p) >= 6 &&
                        p[0] == '\\' && p[1] == 'u') {
                        const char* save = p;
                        p += 2;
                        uint32_t lo = 0;
                        if (ParseHex4(lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else {
                            p = save; // 後続が下位サロゲートでなければ戻す
                        }
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default: return Fail("未知のエスケープ文字です");
            }
        }
        return Fail("文字列が閉じられていません");
    }

    bool ParseNumber(Value& out) {
        // 数値トークンを切り出してから strtod に渡す。
        // 入力バッファは null 終端されていないので、直接 strtod に渡せない。
        const char* start = p;
        if (p < end && (*p == '-' || *p == '+')) ++p;
        while (p < end && ((*p >= '0' && *p <= '9') || *p == '.' ||
                           *p == 'e' || *p == 'E' || *p == '+' || *p == '-')) {
            ++p;
        }
        const size_t len = static_cast<size_t>(p - start);
        if (len == 0 || len >= 64) return Fail("数値として解釈できません");

        char buf[64];
        memcpy(buf, start, len);
        buf[len] = '\0';

        char* tail = nullptr;
        const double v = strtod(buf, &tail);
        if (tail == buf) return Fail("数値の変換に失敗しました");

        out.type   = Value::Type::Number;
        out.number = v;
        return true;
    }

    bool ParseValue(Value& out) {
        if (++depth > kMaxDepth) return Fail("JSON の入れ子が深すぎます");
        struct DepthGuard {
            int& d;
            ~DepthGuard() { --d; }
        } guard{ depth };

        SkipWs();
        if (p >= end) return Fail("値がありません");

        switch (*p) {
            case '{': {
                ++p;
                out.type = Value::Type::Object;
                SkipWs();
                if (p < end && *p == '}') { ++p; return true; }
                for (;;) {
                    SkipWs();
                    std::string key;
                    if (!ParseString(key)) return false;
                    SkipWs();
                    if (p >= end || *p != ':') return Fail("オブジェクトに : がありません");
                    ++p;
                    out.members.emplace_back(std::move(key), Value{});
                    if (!ParseValue(out.members.back().second)) return false;
                    SkipWs();
                    if (p < end && *p == ',') { ++p; continue; }
                    if (p < end && *p == '}') { ++p; return true; }
                    return Fail("オブジェクトが閉じられていません");
                }
            }
            case '[': {
                ++p;
                out.type = Value::Type::Array;
                SkipWs();
                if (p < end && *p == ']') { ++p; return true; }
                for (;;) {
                    out.items.emplace_back();
                    if (!ParseValue(out.items.back())) return false;
                    SkipWs();
                    if (p < end && *p == ',') { ++p; continue; }
                    if (p < end && *p == ']') { ++p; return true; }
                    return Fail("配列が閉じられていません");
                }
            }
            case '"':
                out.type = Value::Type::String;
                return ParseString(out.text);
            case 't':
                if (!Literal("true")) return Fail("true の綴りが不正です");
                out.type = Value::Type::Bool; out.boolean = true;  return true;
            case 'f':
                if (!Literal("false")) return Fail("false の綴りが不正です");
                out.type = Value::Type::Bool; out.boolean = false; return true;
            case 'n':
                if (!Literal("null")) return Fail("null の綴りが不正です");
                out.type = Value::Type::Null; return true;
            default:
                return ParseNumber(out);
        }
    }
};

} // namespace detail

// JSON テキストを解析する。失敗したら false を返し error にメッセージを入れる。
inline bool Parse(const char* data, size_t size, Value& out, std::string* error) {
    detail::Parser ps;
    ps.p   = data;
    ps.end = data + size;
    out = Value{};
    if (!ps.ParseValue(out)) {
        if (error) *error = ps.err;
        return false;
    }
    ps.SkipWs();
    // 末尾に余分なデータがあっても、glTF では実害が無いので警告扱いにしない。
    return true;
}

} // namespace json
