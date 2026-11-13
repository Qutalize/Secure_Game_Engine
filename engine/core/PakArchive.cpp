#include "engine/core/PakArchive.h"

#include <cstring>
#include <fstream>

#include "engine/core/KeyDerivation.h"
#include "engine/core/Logger.h"

extern "C" {
#include "aes.h"
}

bool PakArchive::Open(const char* pakPath) {
    std::ifstream f(pakPath, std::ios::binary | std::ios::ate);
    if (!f) {
        LOG_ERROR("PakArchive: %s を開けません", pakPath);
        return false;
    }
    std::streamsize size = f.tellg();
    f.seekg(0, std::ios::beg);
    m_fileData.resize(static_cast<size_t>(size));
    if (!f.read(reinterpret_cast<char*>(m_fileData.data()), size)) {
        LOG_ERROR("PakArchive: %s の読み込みに失敗", pakPath);
        return false;
    }

    if (m_fileData.size() < sizeof(PakHeader)) {
        LOG_ERROR("PakArchive: ファイルが小さすぎます");
        return false;
    }

    PakHeader header;
    memcpy(&header, m_fileData.data(), sizeof(PakHeader));
    if (memcmp(header.magic, "GPAK", 4) != 0) {
        LOG_ERROR("PakArchive: マジックナンバーが不正（'GPAK' ではない）");
        return false;
    }
    if (header.version != 1) {
        LOG_WARN("PakArchive: 未知のバージョン %u", header.version);
    }

    const size_t tableBytes = static_cast<size_t>(header.entryCount) * sizeof(PakEntry);
    if (m_fileData.size() < sizeof(PakHeader) + tableBytes) {
        LOG_ERROR("PakArchive: エントリテーブルが切り詰められています");
        return false;
    }

    m_entries.resize(header.entryCount);
    memcpy(m_entries.data(), m_fileData.data() + sizeof(PakHeader), tableBytes);

    m_opened = true;
    LOG_INFO("PakArchive: %s を開きました（%u エントリ）", pakPath, header.entryCount);
    return true;
}

bool PakArchive::Load(uint32_t nameHash, std::vector<uint8_t>& outData) const {
    if (!m_opened) return false;

    const PakEntry* entry = nullptr;
    for (const PakEntry& e : m_entries) {
        if (e.nameHash == nameHash) { entry = &e; break; }
    }
    if (!entry) {
        LOG_WARN("PakArchive: nameHash 0x%08X が見つかりません", nameHash);
        return false;
    }

    if (entry->offset + entry->size > m_fileData.size()) {
        LOG_ERROR("PakArchive: エントリ範囲がファイル外を指しています");
        return false;
    }

    // データブロックをコピー
    outData.assign(
        m_fileData.begin() + static_cast<ptrdiff_t>(entry->offset),
        m_fileData.begin() + static_cast<ptrdiff_t>(entry->offset + entry->size));

    if (entry->flags & PAK_FLAG_ENCRYPTED) {
        if (outData.size() % 16 != 0 || outData.empty()) {
            LOG_ERROR("PakArchive: 暗号化サイズが 16 の倍数ではありません");
            return false;
        }

        uint8_t key[16];
        uint8_t iv[16];
        keyderiv::DeriveKey(key);
        keyderiv::DeriveIv(entry->nameHash, iv);

        AES_ctx ctx;
        AES_init_ctx_iv(&ctx, key, iv);
        AES_CBC_decrypt_buffer(&ctx, outData.data(), outData.size());

        // PKCS#7 パディングを除去
        uint8_t pad = outData.back();
        if (pad == 0 || pad > 16 || pad > outData.size()) {
            LOG_ERROR("PakArchive: PKCS#7 パディングが不正（キー不一致の可能性）");
            return false;
        }
        outData.resize(outData.size() - pad);
    }

    return true;
}

bool PakArchive::LoadByName(const char* name, std::vector<uint8_t>& outData) const {
    return Load(keyderiv::Fnv1a32Str(name), outData);
}
