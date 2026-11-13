// pak_builder — アセットを単一の暗号化アーカイブ (.pak) にまとめる CLI ツール。
// 使い方: pak_builder.exe -i <inputDir> -o <output.pak>
//   例:    pak_builder.exe -i assets/ -o game.pak
//
// 各ファイルを AES-128-CBC + PKCS#7 で暗号化し、ヘッダ・エントリテーブル・
// データブロックの順に書き出す。キーは KeyDerivation.h の DeriveKey() で
// 動的生成する（文字列リテラルのハードコードは禁止）。

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "engine/core/ConsoleUtf8.h"
#include "engine/core/PakArchive.h"     // PakHeader / PakEntry / PAK_FLAG_ENCRYPTED
#include "engine/core/KeyDerivation.h"

extern "C" {
#include "aes.h"
}

namespace fs = std::filesystem;

namespace {

struct BuildEntry {
    std::string          name;       // 例 "assets/test.txt"
    uint32_t             nameHash;
    std::vector<uint8_t> cipherData; // 暗号化済みデータ（パディング込み）
};

std::vector<uint8_t> ReadFileBytes(const fs::path& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    std::vector<uint8_t> data;
    if (!f) return data;
    std::streamsize size = f.tellg();
    f.seekg(0, std::ios::beg);
    data.resize(static_cast<size_t>(size));
    f.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}

// PKCS#7 パディングを付与して 16 バイト境界に揃える
void ApplyPkcs7(std::vector<uint8_t>& data) {
    size_t pad = 16 - (data.size() % 16); // 0 にはならない（揃っていても 16 を足す）
    data.insert(data.end(), pad, static_cast<uint8_t>(pad));
}

} // namespace

int main(int argc, char** argv) {
    EnableConsoleUtf8();

    std::string inputDir;
    std::string outputPath;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            inputDir = argv[++i];
        } else if (std::strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            outputPath = argv[++i];
        }
    }

    if (inputDir.empty() || outputPath.empty()) {
        printf("usage: pak_builder -i <inputDir> -o <output.pak>\n");
        return 1;
    }

    fs::path root(inputDir);
    if (!fs::exists(root) || !fs::is_directory(root)) {
        printf("error: 入力ディレクトリが存在しません: %s\n", inputDir.c_str());
        return 1;
    }

    // 入力ディレクトリの最終要素名（例 "assets"）。
    // ストア名を「assets/相対パス」に正規化し、-i の渡し方に依存しないようにする。
    fs::path canonicalRoot = fs::weakly_canonical(root);
    std::string baseName   = canonicalRoot.filename().string();

    // 暗号鍵を生成
    uint8_t key[16];
    keyderiv::DeriveKey(key);

    std::vector<BuildEntry> entries;

    for (const auto& dirEntry : fs::recursive_directory_iterator(root)) {
        if (!dirEntry.is_regular_file()) continue;

        // root からの相対パスを求めて "assets/..." 形式に正規化
        fs::path rel = fs::relative(dirEntry.path(), canonicalRoot);
        std::string name = baseName + "/" + rel.generic_string();

        BuildEntry be;
        be.name     = name;
        be.nameHash = keyderiv::Fnv1a32Str(name.c_str());
        be.cipherData = ReadFileBytes(dirEntry.path());

        // 暗号化（PKCS#7 → AES-128-CBC、IV は nameHash から導出）
        ApplyPkcs7(be.cipherData);
        uint8_t iv[16];
        keyderiv::DeriveIv(be.nameHash, iv);
        AES_ctx ctx;
        AES_init_ctx_iv(&ctx, key, iv);
        AES_CBC_encrypt_buffer(&ctx, be.cipherData.data(), be.cipherData.size());

        printf("  packed: %-40s (hash=0x%08X, %zu bytes)\n",
               name.c_str(), be.nameHash, be.cipherData.size());
        entries.push_back(std::move(be));
    }

    if (entries.empty()) {
        printf("warning: パック対象のファイルが見つかりませんでした。\n");
    }

    // --- ヘッダとエントリテーブルを構築 ---
    PakHeader header = {};
    memcpy(header.magic, "GPAK", 4);
    header.version    = 1;
    header.entryCount = static_cast<uint32_t>(entries.size());

    const uint64_t tableStart = sizeof(PakHeader);
    const uint64_t dataStart  = tableStart + entries.size() * sizeof(PakEntry);

    std::vector<PakEntry> table(entries.size());
    uint64_t cursor = dataStart;
    for (size_t i = 0; i < entries.size(); ++i) {
        table[i].nameHash = entries[i].nameHash;
        table[i].flags    = PAK_FLAG_ENCRYPTED;
        table[i].offset   = cursor;
        table[i].size     = entries[i].cipherData.size();
        cursor += entries[i].cipherData.size();
    }

    // --- 書き出し ---
    std::ofstream out(outputPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        printf("error: 出力ファイルを開けません: %s\n", outputPath.c_str());
        return 1;
    }
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    out.write(reinterpret_cast<const char*>(table.data()),
              static_cast<std::streamsize>(table.size() * sizeof(PakEntry)));
    for (const BuildEntry& be : entries) {
        out.write(reinterpret_cast<const char*>(be.cipherData.data()),
                  static_cast<std::streamsize>(be.cipherData.size()));
    }
    out.close();

    printf("done: %s に %zu エントリを書き出しました（先頭マジック: GPAK）\n",
           outputPath.c_str(), entries.size());
    return 0;
}
