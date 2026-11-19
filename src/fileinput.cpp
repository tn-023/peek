#include "fileinput.h"
#include "fs.h"

#include <algorithm>
#include <cstring>

namespace peek {

namespace {

std::atomic<bool> g_sandboxed{false};

struct Injected {
    const std::wstring* path = nullptr;
    HANDLE h = nullptr;
};
thread_local Injected t_input;

// Nur lesbarer IStream ueber einem Datei-Handle mit eigener Position (liest per ReadAt). Frei-threaded (agil):
// WinRT (Windows.Data.Pdf) und Media Foundation rufen ihn aus eigenen Threads auf; ohne Agilitaet wuerde der Aufruf
// in ein blockiertes Single-Thread-Apartment umgeleitet.
class InputStream final : public IStream, public IAgileObject {
public:
    InputStream(HANDLE h, uint64_t size) : h_(h), size_(size) {
        CoCreateFreeThreadedMarshaler(static_cast<IStream*>(this), &ftm_);
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ISequentialStream) || riid == __uuidof(IStream)) {
            *ppv = static_cast<IStream*>(this);
            AddRef();
            return S_OK;
        }
        if (riid == __uuidof(IAgileObject)) {
            *ppv = static_cast<IAgileObject*>(this);
            AddRef();
            return S_OK;
        }
        if (riid == __uuidof(IMarshal) && ftm_) return ftm_->QueryInterface(riid, ppv);
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {   // IStream und IAgileObject teilen sich den Zaehler
        ULONG r = --refs_;
        if (r == 0) delete this;
        return r;
    }

    // ISequentialStream
    // Dekoder lesen Dateikoepfe in vielen kleinen Stuecken. Lokal ist das egal, ueber ein Netzlaufwerk kostet jedes
    // Stueck eine Rundreise zum Server. Deshalb: kleine Lesezugriffe aus Bloecken zu 256 KB, die je in einem Zugriff
    // geholt werden (wenige Bloecke werden behalten); grosse Lesezugriffe direkt.
    HRESULT STDMETHODCALLTYPE Read(void* pv, ULONG cb, ULONG* pcbRead) override {
        if (!pv) return STG_E_INVALIDPOINTER;
        std::lock_guard<std::mutex> lock(mutex_);
        ULONG total = 0;
        auto* dst = static_cast<uint8_t*>(pv);
        if (cb >= kDirect) {
            while (total < cb && pos_ < size_) {
                DWORD want = static_cast<DWORD>(std::min<uint64_t>({cb - total, size_ - pos_, 1u << 24}));
                DWORD got = ReadAt(h_, pos_, dst + total, want);
                if (got == 0) break;
                total += got;
                pos_ += got;
            }
        } else {
            while (total < cb && pos_ < size_) {
                const Block* b = GetBlock(pos_ / kBlock);
                uint64_t off = pos_ % kBlock;
                if (!b || off >= b->data.size()) break;
                ULONG n = static_cast<ULONG>(std::min<uint64_t>(cb - total, b->data.size() - off));
                memcpy(dst + total, b->data.data() + off, n);
                total += n;
                pos_ += n;
            }
        }
        if (pcbRead) *pcbRead = total;
        return total < cb ? S_FALSE : S_OK;
    }
    HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG* pcbWritten) override {
        if (pcbWritten) *pcbWritten = 0;
        return STG_E_ACCESSDENIED;
    }

    // IStream
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* newPos) override {
        std::lock_guard<std::mutex> lock(mutex_);
        int64_t base = origin == STREAM_SEEK_SET ? 0 : origin == STREAM_SEEK_CUR ? static_cast<int64_t>(pos_)
                     : origin == STREAM_SEEK_END ? static_cast<int64_t>(size_) : -1;
        if (base < 0) return STG_E_INVALIDFUNCTION;
        int64_t p = base + move.QuadPart;
        if (p < 0) return STG_E_INVALIDFUNCTION;
        pos_ = static_cast<uint64_t>(p);
        if (newPos) newPos->QuadPart = pos_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override { return STG_E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream* dst, ULARGE_INTEGER cb, ULARGE_INTEGER* pcbRead,
                                     ULARGE_INTEGER* pcbWritten) override {
        if (!dst) return STG_E_INVALIDPOINTER;
        uint64_t left = cb.QuadPart, rd = 0, wr = 0;
        std::vector<uint8_t> buf(1 << 16);
        while (left > 0) {
            ULONG got = 0;
            Read(buf.data(), static_cast<ULONG>(std::min<uint64_t>(left, buf.size())), &got);
            if (!got) break;
            rd += got;
            ULONG put = 0;
            HRESULT hr = dst->Write(buf.data(), got, &put);
            wr += put;
            if (FAILED(hr)) return hr;
            left -= got;
        }
        if (pcbRead) pcbRead->QuadPart = rd;
        if (pcbWritten) pcbWritten->QuadPart = wr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Revert() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* st, DWORD) override {
        if (!st) return STG_E_INVALIDPOINTER;
        ZeroMemory(st, sizeof(*st));
        st->type = STGTY_STREAM;
        st->cbSize.QuadPart = size_;
        st->grfMode = STGM_READ | STGM_SHARE_DENY_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clone(IStream** out) override {
        if (!out) return STG_E_INVALIDPOINTER;
        HANDLE dup = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), h_, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS))
            return E_FAIL;
        auto* c = new InputStream(dup, size_);
        std::lock_guard<std::mutex> lock(mutex_);
        c->pos_ = pos_;
        *out = c;
        return S_OK;
    }

private:
    static constexpr uint64_t kBlock = 256 * 1024;
    static constexpr ULONG kDirect = 1024 * 1024;
    static constexpr size_t kMaxBlocks = 6;
    struct Block {
        uint64_t index = 0;
        std::vector<uint8_t> data;
    };

    // Block aus dem Zwischenspeicher oder in einem Zugriff von der Datei; zuletzt benutzte Bloecke bleiben
    const Block* GetBlock(uint64_t index) {
        for (size_t i = 0; i < blocks_.size(); ++i) {
            if (blocks_[i].index != index) continue;
            if (i + 1 != blocks_.size()) std::rotate(blocks_.begin() + i, blocks_.begin() + i + 1, blocks_.end());
            return &blocks_.back();
        }
        uint64_t start = index * kBlock;
        if (start >= size_) return nullptr;
        Block b;
        b.index = index;
        b.data.resize(static_cast<size_t>(std::min<uint64_t>(kBlock, size_ - start)));
        size_t got = 0;
        while (got < b.data.size()) {
            DWORD n = ReadAt(h_, start + got, b.data.data() + got, static_cast<DWORD>(b.data.size() - got));
            if (n == 0) break;
            got += n;
        }
        if (got == 0) return nullptr;
        b.data.resize(got);
        if (blocks_.size() >= kMaxBlocks) blocks_.erase(blocks_.begin());
        blocks_.push_back(std::move(b));
        return &blocks_.back();
    }

    ~InputStream() { if (h_ && h_ != INVALID_HANDLE_VALUE) CloseHandle(h_); }
    std::atomic<ULONG> refs_{1};
    ComPtr<IUnknown> ftm_;   // Free-Threaded Marshaler
    std::mutex mutex_;       // Position und Lesen gegen gleichzeitige Aufrufe schuetzen
    HANDLE h_;
    uint64_t size_;
    uint64_t pos_ = 0;
    std::vector<Block> blocks_;
};

}  // namespace

void SetInputSandboxed(bool on) { g_sandboxed = on; }
bool InputSandboxed() { return g_sandboxed; }

ScopedInput::ScopedInput(const std::wstring& path, HANDLE h) {
    t_input.path = &path;
    t_input.h = h;
}
ScopedInput::~ScopedInput() { t_input = {}; }

HANDLE OpenInput(const std::wstring& path) {
    if (t_input.path && t_input.h && _wcsicmp(t_input.path->c_str(), path.c_str()) == 0) {
        HANDLE dup = nullptr;
        if (DuplicateHandle(GetCurrentProcess(), t_input.h, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS))
            return dup;
        return INVALID_HANDLE_VALUE;
    }
    if (g_sandboxed) return INVALID_HANDLE_VALUE;   // Dekoder-Prozess: keine Zugriffe ueber Pfade
    return CreateFileW(LongPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}

DWORD ReadAt(HANDLE h, uint64_t offset, void* buf, DWORD len) {
    OVERLAPPED ov = {};
    ov.Offset = static_cast<DWORD>(offset);
    ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD got = 0;
    if (!ReadFile(h, buf, len, &got, &ov)) return 0;
    return got;
}

bool ReadInputHead(const std::wstring& path, size_t maxBytes, std::vector<uint8_t>& out) {
    HANDLE h = OpenInput(path);
    if (h == INVALID_HANDLE_VALUE) return false;
    out.resize(maxBytes);
    size_t total = 0;
    while (total < maxBytes) {
        DWORD got = ReadAt(h, total, out.data() + total, static_cast<DWORD>(std::min<size_t>(maxBytes - total, 1 << 20)));
        if (!got) break;
        total += got;
    }
    CloseHandle(h);
    out.resize(total);
    return total > 0;
}

bool ReadInputAll(const std::wstring& path, size_t maxBytes, std::vector<uint8_t>& out, int64_t* mtime) {
    HANDLE h = OpenInput(path);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    bool ok = GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && static_cast<uint64_t>(sz.QuadPart) <= maxBytes;
    if (ok) {
        if (mtime) {
            FILETIME ft{};
            GetFileTime(h, nullptr, nullptr, &ft);
            *mtime = FileTimeToUnix(ft);
        }
        out.resize(static_cast<size_t>(sz.QuadPart));
        size_t total = 0;
        while (total < out.size()) {
            DWORD got = ReadAt(h, total, out.data() + total, static_cast<DWORD>(std::min<size_t>(out.size() - total, 1 << 24)));
            if (!got) break;
            total += got;
        }
        ok = total == out.size();
    }
    CloseHandle(h);
    return ok;
}

ComPtr<IStream> OpenInputStream(const std::wstring& path) {
    HANDLE h = OpenInput(path);
    if (h == INVALID_HANDLE_VALUE) return nullptr;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0) {
        CloseHandle(h);
        return nullptr;
    }
    ComPtr<IStream> s;
    s.Attach(new InputStream(h, static_cast<uint64_t>(sz.QuadPart)));
    return s;
}

} // namespace peek
