#pragma once

#include <cstdint>
#include <string>
#include <vector>

// .pak アーカイブの読み込み。
// フォーマット（PROMPT-07 仕様）:
//   [Header 32B] magic="GPAK", version, entryCount, reserved[4]
//   [Entry Table: entryCount x 24B] nameHash, flags, offset, size
//   [Data Blocks] 各エントリ（flags bit0 が立っていれば AES-128-CBC 暗号化）

// ファイル先頭ヘッダ（32 バイト）
#pragma pack(push, 1)
struct PakHeader {
    char     magic[4];     // "GPAK"
    uint32_t version;      // 1
    uint32_t entryCount;
    uint32_t reserved[4];
};

struct PakEntry {
    uint32_t nameHash;     // FNV-1a 32bit
    uint32_t flags;        // bit0 = AES 暗号化済み
    uint64_t offset;       // ファイル先頭からのバイトオフセット
    uint64_t size;         // 暗号化後のバイト数（パディング込み）
};
#pragma pack(pop)

constexpr uint32_t PAK_FLAG_ENCRYPTED = 0x1u;

class PakArchive {
public:
    bool Open(const char* pakPath);

    // nameHash を指定してエントリを取り出す（暗号化されていれば復号する）
    bool Load(uint32_t nameHash, std::vector<uint8_t>& outData) const;

    // ファイル名（例 "assets/test.txt"）から FNV-1a を計算して Load する
    bool LoadByName(const char* name, std::vector<uint8_t>& outData) const;

    size_t EntryCount() const { return m_entries.size(); }
    bool   IsOpen()     const { return m_opened; }

private:
    std::vector<uint8_t> m_fileData; // .pak 全体を読み込んで保持
    std::vector<PakEntry> m_entries;
    bool m_opened = false;
};
