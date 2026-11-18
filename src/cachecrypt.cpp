#include "cachecrypt.h"
#include "fs.h"

#include <bcrypt.h>
#include <dpapi.h>

#include <unordered_map>

namespace peek {

namespace {

constexpr wchar_t kKeyFile[] = L"index.key";
const char kEntropy[] = "peek-index-v1";
std::atomic<uint32_t> g_nextId{1};

bool Random(uint8_t* p, size_t n) {
    return BCRYPT_SUCCESS(BCryptGenRandom(nullptr, p, static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

void Derive(const uint8_t master[32], const char* label, uint8_t out[32]) {
    BCryptHash(BCRYPT_HMAC_SHA256_ALG_HANDLE, const_cast<PUCHAR>(master), 32,
               reinterpret_cast<PUCHAR>(const_cast<char*>(label)), static_cast<ULONG>(strlen(label)), out, 32);
}

bool ReadAll(const std::wstring& path, std::vector<uint8_t>& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    bool ok = GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart < 65536;
    if (ok) {
        out.resize(static_cast<size_t>(sz.QuadPart));
        DWORD rd = 0;
        ok = ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &rd, nullptr) && rd == out.size();
    }
    CloseHandle(h);
    return ok;
}

bool WriteAll(const std::wstring& path, const uint8_t* p, size_t n) {
    std::wstring tmp = path + L".neu";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    bool ok = WriteFile(h, p, static_cast<DWORD>(n), &wr, nullptr) && wr == n && FlushFileBuffers(h);
    CloseHandle(h);
    return ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::string B64(const uint8_t* p, size_t n) {
    std::string s;
    s.reserve((n * 4 + 2) / 3);
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; ++i) {
        acc = (acc << 8) | p[i];
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            s += kB64[(acc >> bits) & 63];
        }
    }
    if (bits) s += kB64[(acc << (6 - bits)) & 63];
    return s;
}

bool UnB64(const std::string& s, std::vector<uint8_t>& out) {
    static int8_t map[256];
    static std::once_flag once;
    std::call_once(once, [] {
        memset(map, -1, sizeof(map));
        for (int i = 0; i < 64; ++i) map[static_cast<uint8_t>(kB64[i])] = static_cast<int8_t>(i);
    });
    out.clear();
    out.reserve(s.size() * 3 / 4);
    uint32_t acc = 0;
    int bits = 0;
    for (char c : s) {
        int8_t v = map[static_cast<uint8_t>(c)];
        if (v < 0) return false;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
        }
    }
    return true;
}

// AES-ECB-Schluessel je Thread (CNG-Schluessel nicht gleichzeitig aus mehreren Threads benutzen)
struct ThreadKeys {
    uint32_t id = 0;
    BCRYPT_KEY_HANDLE name = nullptr, data = nullptr;
    ~ThreadKeys() {
        if (name) BCryptDestroyKey(name);
        if (data) BCryptDestroyKey(data);
    }
};
thread_local ThreadKeys t_keys;

// Zwischenspeicher fuer Ordner (Kuerzel <-> Klartext), je Thread und Schluessel
struct ThreadCache {
    uint32_t id = 0;
    std::unordered_map<std::wstring, std::string> enc;
    std::unordered_map<std::string, std::wstring> dec;
    void Check(uint32_t key) {
        if (id != key || enc.size() > 8192 || dec.size() > 8192) {
            enc.clear();
            dec.clear();
            id = key;
        }
    }
};
thread_local ThreadCache t_cache;

}  // namespace

bool CacheCrypto::Init(const std::wstring& dir, bool& fresh) {
    fresh = false;
    ready_ = false;
    std::wstring file = JoinPath(dir, kKeyFile);
    uint8_t master[32] = {};
    bool have = false;
    std::vector<uint8_t> blob;
    if (ReadAll(file, blob)) {
        DATA_BLOB in{static_cast<DWORD>(blob.size()), blob.data()};
        DATA_BLOB ent{static_cast<DWORD>(sizeof(kEntropy) - 1), reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy))};
        DATA_BLOB out{};
        if (CryptUnprotectData(&in, nullptr, &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
            if (out.cbData == 32) {
                memcpy(master, out.pbData, 32);
                have = true;
            }
            SecureZeroMemory(out.pbData, out.cbData);
            LocalFree(out.pbData);
        }
    }
    if (!have) {
        // neuer Schluessel (erster Start, anderer PC, anderes Konto oder zurueckgesetztes Kennwort)
        if (!Random(master, 32)) return false;
        DATA_BLOB in{32, master};
        DATA_BLOB ent{static_cast<DWORD>(sizeof(kEntropy) - 1), reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy))};
        DATA_BLOB out{};
        if (!CryptProtectData(&in, L"peek Index", &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
            SecureZeroMemory(master, sizeof(master));
            return false;
        }
        bool ok = WriteAll(file, out.pbData, out.cbData);
        LocalFree(out.pbData);
        if (!ok) {
            SecureZeroMemory(master, sizeof(master));
            return false;
        }
        fresh = true;
    }
    Derive(master, "peek/pfad/aes", kName_);
    Derive(master, "peek/pfad/mac", kNameMac_);
    Derive(master, "peek/daten/aes", kData_);
    Derive(master, "peek/daten/mac", kDataMac_);
    SecureZeroMemory(master, sizeof(master));
    id_ = g_nextId++;
    ready_ = true;
    return true;
}

bool CacheCrypto::Hmac(const uint8_t key[32], const uint8_t* p, size_t n, uint8_t out[32]) const {
    return BCRYPT_SUCCESS(BCryptHash(BCRYPT_HMAC_SHA256_ALG_HANDLE, const_cast<PUCHAR>(key), 32, const_cast<PUCHAR>(p),
                                     static_cast<ULONG>(n), out, 32));
}

// AES-256-CTR: Schluesselstrom = AES-ECB(iv + Blocknummer), XOR mit den Daten
bool CacheCrypto::Ctr(const uint8_t key[32], const uint8_t iv[16], uint8_t* data, size_t n) const {
    if (!n) return true;
    ThreadKeys& tk = t_keys;
    if (tk.id != id_) {
        if (tk.name) BCryptDestroyKey(tk.name);
        if (tk.data) BCryptDestroyKey(tk.data);
        tk.name = tk.data = nullptr;
        if (!BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(BCRYPT_AES_ECB_ALG_HANDLE, &tk.name, nullptr, 0,
                                                       const_cast<PUCHAR>(kName_), 32, 0)) ||
            !BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(BCRYPT_AES_ECB_ALG_HANDLE, &tk.data, nullptr, 0,
                                                       const_cast<PUCHAR>(kData_), 32, 0)))
            return false;
        tk.id = id_;
    }
    BCRYPT_KEY_HANDLE h = key == kName_ ? tk.name : tk.data;
    size_t blocks = (n + 15) / 16;
    std::vector<uint8_t> ks(blocks * 16);
    for (size_t b = 0; b < blocks; ++b) {
        uint8_t* c = ks.data() + b * 16;
        memcpy(c, iv, 16);
        uint64_t ctr = 0;
        for (int i = 0; i < 8; ++i) ctr = (ctr << 8) | c[8 + i];
        ctr += b;
        for (int i = 7; i >= 0; --i) {
            c[8 + i] = static_cast<uint8_t>(ctr);
            ctr >>= 8;
        }
    }
    ULONG done = 0;
    if (!BCRYPT_SUCCESS(BCryptEncrypt(h, ks.data(), static_cast<ULONG>(ks.size()), nullptr, nullptr, 0, ks.data(),
                                      static_cast<ULONG>(ks.size()), &done, 0)))
        return false;
    for (size_t i = 0; i < n; ++i) data[i] ^= ks[i];
    return true;
}

// Kuerzel: synthetischer IV (12 Byte HMAC) | AES-CTR(UTF-8, mit 0x80 und Nullen auf 8 Byte aufgefuellt).
// Kompakt, damit der Index nicht unnoetig waechst; die genaue Namenslaenge bleibt verborgen.
constexpr size_t kSiv = 12, kPad = 8;

std::string CacheCrypto::Token(const std::wstring& s) const {
    if (!ready_ || s.empty()) return {};
    std::string u = WideToUtf8(s);
    const auto* raw = reinterpret_cast<const uint8_t*>(u.data());
    size_t len = u.size();
    uint8_t mac[32];
    if (!Hmac(kNameMac_, raw, len, mac)) return {};
    std::vector<uint8_t> buf(kSiv + ((len + 1 + kPad - 1) / kPad) * kPad, 0);
    memcpy(buf.data(), mac, kSiv);
    memcpy(buf.data() + kSiv, raw, len);
    buf[kSiv + len] = 0x80;
    uint8_t iv[16] = {};
    memcpy(iv, mac, kSiv);   // Zaehler in den letzten 4 Byte
    if (!Ctr(kName_, iv, buf.data() + kSiv, buf.size() - kSiv)) return {};
    return B64(buf.data(), buf.size());
}

bool CacheCrypto::FromToken(const std::string& t, std::wstring& out) const {
    out.clear();
    if (!ready_) return false;
    if (t.empty()) return true;
    std::vector<uint8_t> buf;
    if (!UnB64(t, buf) || buf.size() < kSiv + kPad || (buf.size() - kSiv) % kPad) return false;
    uint8_t iv[16] = {};
    memcpy(iv, buf.data(), kSiv);
    if (!Ctr(kName_, iv, buf.data() + kSiv, buf.size() - kSiv)) return false;
    size_t end = buf.size();
    while (end > kSiv && buf[end - 1] == 0) --end;
    if (end <= kSiv || buf[end - 1] != 0x80) return false;
    size_t len = end - 1 - kSiv;
    uint8_t mac[32];
    if (!Hmac(kNameMac_, buf.data() + kSiv, len, mac) || memcmp(mac, buf.data(), kSiv) != 0) return false;
    out = Utf8ToWide(std::string_view(reinterpret_cast<const char*>(buf.data() + kSiv), len));
    return true;
}

std::string CacheCrypto::PathKey(const std::wstring& path) const {
    if (!ready_) return {};
    std::wstring p = path;
    while (p.size() > 1 && p.back() == L'\\') p.pop_back();
    size_t slash = p.find_last_of(L'\\');
    if (slash == std::wstring::npos) return Token(p);
    std::wstring folder = p.substr(0, slash);
    ThreadCache& c = t_cache;
    c.Check(id_);
    std::string fk;
    auto it = c.enc.find(folder);
    if (it != c.enc.end()) {
        fk = it->second;
    } else {
        fk = PathKey(folder);
        c.enc.emplace(folder, fk);
    }
    return fk + "\\" + Token(p.substr(slash + 1));
}

bool CacheCrypto::PathFromKey(const std::string& key, std::wstring& out) const {
    out.clear();
    size_t slash = key.find_last_of('\\');
    if (slash == std::string::npos) return FromToken(key, out);
    std::string fk = key.substr(0, slash);
    ThreadCache& c = t_cache;
    c.Check(id_);
    std::wstring folder;
    auto it = c.dec.find(fk);
    if (it != c.dec.end()) {
        folder = it->second;
    } else {
        if (!PathFromKey(fk, folder)) return false;
        c.dec.emplace(fk, folder);
    }
    std::wstring name;
    if (!FromToken(key.substr(slash + 1), name)) return false;
    out = folder + L"\\" + name;
    return true;
}

std::vector<uint8_t> CacheCrypto::Seal(const uint8_t* p, size_t n) const {
    if (!ready_) return {};
    std::vector<uint8_t> out(32 + n);   // IV (16) | MAC (16) | Daten
    if (!Random(out.data(), 16)) return {};
    memcpy(out.data() + 32, p, n);
    if (!Ctr(kData_, out.data(), out.data() + 32, n)) return {};
    std::vector<uint8_t> macIn(16 + n);
    memcpy(macIn.data(), out.data(), 16);
    memcpy(macIn.data() + 16, out.data() + 32, n);
    uint8_t mac[32];
    if (!Hmac(kDataMac_, macIn.data(), macIn.size(), mac)) return {};
    memcpy(out.data() + 16, mac, 16);
    return out;
}

bool CacheCrypto::Open(const uint8_t* p, size_t n, std::vector<uint8_t>& out) const {
    if (!ready_ || n < 32) return false;
    std::vector<uint8_t> macIn(16 + n - 32);
    memcpy(macIn.data(), p, 16);
    memcpy(macIn.data() + 16, p + 32, n - 32);
    uint8_t mac[32];
    if (!Hmac(kDataMac_, macIn.data(), macIn.size(), mac)) return false;
    // Vergleich ohne fruehen Abbruch
    uint8_t diff = 0;
    for (int i = 0; i < 16; ++i) diff |= static_cast<uint8_t>(mac[i] ^ p[16 + i]);
    if (diff) return false;
    out.assign(p + 32, p + n);
    return Ctr(kData_, p, out.data(), out.size());
}

} // namespace peek
