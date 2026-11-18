#include "docimaging.h"
#include "fileinput.h"
#include "fs.h"
#include "imaging.h"
#include "core/textutil.h"

#include <shcore.h>
#include <windows.data.pdf.h>
#include <windows.storage.streams.h>
#include <wrl/event.h>
#include <wrl/wrappers/corewrappers.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PSD
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_FAILURE_STRINGS
#pragma warning(push, 0)
#include <stb_image.h>
#pragma warning(pop)

namespace peek {

using namespace ABI::Windows::Data::Pdf;
using namespace ABI::Windows::Foundation;
using namespace ABI::Windows::Storage::Streams;
using Microsoft::WRL::Callback;
using Microsoft::WRL::FtmBase;
using Microsoft::WRL::Implements;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::ClassicCom;
using Microsoft::WRL::Wrappers::HStringReference;

namespace {

std::wstring ExtOf(const std::wstring& path) {
    size_t dot = path.find_last_of(L'.');
    size_t slash = path.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return {};
    return ToLower(path.substr(dot + 1));
}

// ---------------------------------------------------------------------------
// WinRT: auf eine asynchrone Operation warten (Rueckruf laeuft im Thread-Pool, daher agil)
struct EventHolder {
    HANDLE h = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ~EventHolder() { if (h) CloseHandle(h); }
};

template <typename Op, typename Handler>
bool Await(Op* op, DWORD timeoutMs) {
    auto ev = std::make_shared<EventHolder>();
    auto cb = Callback<Implements<RuntimeClassFlags<ClassicCom>, Handler, FtmBase>>(
        [ev](Op*, AsyncStatus) -> HRESULT {
            SetEvent(ev->h);
            return S_OK;
        });
    if (!cb || FAILED(op->put_Completed(cb.Get()))) return false;
    ComPtr<IAsyncInfo> info;
    op->QueryInterface(IID_PPV_ARGS(&info));
    if (WaitForSingleObject(ev->h, timeoutMs) != WAIT_OBJECT_0) {
        if (info) info->Cancel();
        return false;
    }
    AsyncStatus st = AsyncStatus::Error;
    if (info) info->get_Status(&st);
    return st == AsyncStatus::Completed;
}

bool LoadPdf(const std::wstring& path, ComPtr<IPdfDocument>& doc) {
    // Datenstrom ueber dem Eingabe-Handle (im Dekoder-Prozess gibt es keinen Zugriff ueber den Pfad)
    ComPtr<IStream> in = OpenInputStream(path);
    ComPtr<IRandomAccessStream> file;
    if (!in || FAILED(CreateRandomAccessStreamOverStream(in.Get(), BSOS_DEFAULT, IID_PPV_ARGS(&file))))
        return false;
    ComPtr<IPdfDocumentStatics> statics;
    if (FAILED(RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Data_Pdf_PdfDocument).Get(),
                                      IID_PPV_ARGS(&statics))))
        return false;
    ComPtr<IAsyncOperation<PdfDocument*>> op;
    if (FAILED(statics->LoadFromStreamAsync(file.Get(), &op)) ||
        !Await<IAsyncOperation<PdfDocument*>, IAsyncOperationCompletedHandler<PdfDocument*>>(op.Get(), 15000))
        return false;
    return SUCCEEDED(op->GetResults(&doc)) && doc;
}

bool RenderPdfPage(IPdfDocument* doc, uint32_t edge, Pixels& out) {
    ComPtr<IPdfPage> page;
    if (FAILED(doc->GetPage(0, &page)) || !page) return false;
    ABI::Windows::Foundation::Size sz{};
    page->get_Size(&sz);
    ComPtr<IInspectable> insp;
    ComPtr<IPdfPageRenderOptions> opts;
    if (FAILED(RoActivateInstance(HStringReference(RuntimeClass_Windows_Data_Pdf_PdfPageRenderOptions).Get(), &insp)) ||
        FAILED(insp.As(&opts)))
        return false;
    if (sz.Width >= sz.Height) opts->put_DestinationWidth(edge);
    else opts->put_DestinationHeight(edge);
    ABI::Windows::UI::Color white = {255, 255, 255, 255};
    opts->put_BackgroundColor(white);
    ComPtr<IInspectable> minsp;
    ComPtr<IRandomAccessStream> mem;
    if (FAILED(RoActivateInstance(HStringReference(RuntimeClass_Windows_Storage_Streams_InMemoryRandomAccessStream).Get(),
                                  &minsp)) ||
        FAILED(minsp.As(&mem)))
        return false;
    ComPtr<IAsyncAction> act;
    if (FAILED(page->RenderWithOptionsToStreamAsync(mem.Get(), opts.Get(), &act)) ||
        !Await<IAsyncAction, IAsyncActionCompletedHandler>(act.Get(), 15000))
        return false;
    // gerendertes PNG aus dem Speicherstrom lesen
    ComPtr<IStream> st;
    if (FAILED(CreateStreamOverRandomAccessStream(mem.Get(), IID_PPV_ARGS(&st)))) return false;
    STATSTG stat{};
    if (FAILED(st->Stat(&stat, STATFLAG_NONAME)) || stat.cbSize.QuadPart == 0 || stat.cbSize.QuadPart > 256ull * 1024 * 1024)
        return false;
    LARGE_INTEGER zero{};
    st->Seek(zero, STREAM_SEEK_SET, nullptr);
    std::vector<uint8_t> png(static_cast<size_t>(stat.cbSize.QuadPart));
    ULONG read = 0;
    if (FAILED(st->Read(png.data(), static_cast<ULONG>(png.size()), &read)) || read == 0) return false;
    return DecodeImageMemory(png.data(), read, out);
}

// ---------------------------------------------------------------------------
// Datei-Hilfen
bool ReadHead(const std::wstring& path, size_t maxBytes, std::vector<uint8_t>& data) {
    return ReadInputHead(path, maxBytes, data);
}

uint32_t BE32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint16_t BE16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

void FitTo(Pixels& px, uint32_t edge) {
    if (edge && (px.w > edge || px.h > edge)) {
        Pixels s;
        if (ScalePixels(px, edge, s)) {
            s.srcW = px.srcW ? px.srcW : px.w;
            s.srcH = px.srcH ? px.srcH : px.h;
            px = std::move(s);
        }
    }
}

// ---------------------------------------------------------------------------
// Photoshop
struct PsdHeader { uint32_t w = 0, h = 0; uint16_t version = 0, mode = 0, depth = 0; };

bool ReadPsdHeader(const std::vector<uint8_t>& d, PsdHeader& hd) {
    if (d.size() < 26 || memcmp(d.data(), "8BPS", 4) != 0) return false;
    hd.version = BE16(&d[4]);
    hd.h = BE32(&d[14]);
    hd.w = BE32(&d[18]);
    hd.depth = BE16(&d[22]);
    hd.mode = BE16(&d[24]);
    return (hd.version == 1 || hd.version == 2) && hd.w && hd.h;
}

// eingebettetes JPEG-Vorschaubild (Bildressource 1036 bzw. 1033), etwa 160 px
bool PsdEmbeddedThumb(const std::vector<uint8_t>& d, Pixels& out) {
    if (d.size() < 34) return false;
    size_t p = 26;
    uint32_t cmLen = BE32(&d[p]);
    p += 4 + cmLen;
    if (p + 4 > d.size()) return false;
    uint32_t resLen = BE32(&d[p]);
    p += 4;
    size_t end = std::min(d.size(), p + resLen);
    while (p + 12 <= end) {
        if (memcmp(&d[p], "8BIM", 4) != 0) break;
        uint16_t id = BE16(&d[p + 4]);
        size_t q = p + 6;
        uint8_t nameLen = d[q];
        q += 1 + nameLen;
        if ((1 + nameLen) % 2) ++q;   // auf gerade Laenge aufgefuellt
        if (q + 4 > end) break;
        uint32_t sz = BE32(&d[q]);
        q += 4;
        if (q + sz > end) break;
        if ((id == 1036 || id == 1033) && sz > 28) {
            // 28 Byte Kopf, danach JFIF
            if (DecodeImageMemory(&d[q + 28], sz - 28, out)) {
                if (id == 1033) {   // altes Format: BGR statt RGB
                    for (size_t i = 0; i + 3 < out.data.size(); i += 4) std::swap(out.data[i], out.data[i + 2]);
                }
                return true;
            }
        }
        p = q + sz + (sz % 2);
    }
    return false;
}

struct StbFile { HANDLE h; uint64_t size, pos; };
int StbRead(void* u, char* data, int size) {
    auto f = static_cast<StbFile*>(u);
    if (size <= 0 || f->pos >= f->size) return 0;
    DWORD got = ReadAt(f->h, f->pos, data, static_cast<DWORD>(size));
    f->pos += got;
    return static_cast<int>(got);
}
void StbSkip(void* u, int n) {
    auto f = static_cast<StbFile*>(u);
    int64_t p = static_cast<int64_t>(f->pos) + n;
    f->pos = p < 0 ? 0 : static_cast<uint64_t>(p);
}
int StbEof(void* u) {
    auto f = static_cast<StbFile*>(u);
    return f->pos >= f->size ? 1 : 0;
}

// Gesamtbild (bei "Kompatibilitaet maximieren"), ohne die Ebenen in den Speicher zu lesen
bool PsdComposite(const std::wstring& path, const PsdHeader& hd, uint32_t edge, Pixels& out) {
    if (hd.version != 1 || (hd.mode != 3 && hd.mode != 1)) return false;   // nur PSD, RGB/Graustufen
    if (uint64_t(hd.w) * hd.h > 120ull * 1000 * 1000) return false;      // sehr grosse Bilder: eingebettetes Vorschaubild
    StbFile f{OpenInput(path), 0, 0};
    if (f.h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    GetFileSizeEx(f.h, &sz);
    f.size = static_cast<uint64_t>(sz.QuadPart);
    stbi_io_callbacks cb = {StbRead, StbSkip, StbEof};
    int w = 0, h = 0, comp = 0;
    stbi_uc* rgba = stbi_load_from_callbacks(&cb, &f, &w, &h, &comp, 4);
    CloseHandle(f.h);
    if (!rgba) return false;
    Pixels px;
    px.w = static_cast<uint32_t>(w);
    px.h = static_cast<uint32_t>(h);
    px.srcW = px.w;
    px.srcH = px.h;
    px.data.resize(size_t(w) * h * 4);
    for (size_t i = 0; i < px.data.size(); i += 4) {
        uint8_t r = rgba[i], g = rgba[i + 1], b = rgba[i + 2], a = rgba[i + 3];
        px.data[i] = static_cast<uint8_t>(b * a / 255);
        px.data[i + 1] = static_cast<uint8_t>(g * a / 255);
        px.data[i + 2] = static_cast<uint8_t>(r * a / 255);
        px.data[i + 3] = a;
    }
    stbi_image_free(rgba);
    FitTo(px, edge);
    out = std::move(px);
    return true;
}

// ---------------------------------------------------------------------------
// Illustrator: XMP-Vorschaubild (<xmpGImg:image>Base64-JPEG</xmpGImg:image>)
bool AiXmpThumb(const std::wstring& path, Pixels& out) {
    std::vector<uint8_t> d;
    if (!ReadHead(path, 8u << 20, d)) return false;
    static const char kTag[] = "xmpGImg:image>";
    auto it = std::search(d.begin(), d.end(), kTag, kTag + sizeof(kTag) - 1);
    if (it == d.end()) return false;
    size_t p = static_cast<size_t>(it - d.begin()) + sizeof(kTag) - 1;
    static int8_t map[256];
    static bool init = false;
    if (!init) {
        memset(map, -1, sizeof(map));
        const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) map[static_cast<uint8_t>(a[i])] = static_cast<int8_t>(i);
        init = true;
    }
    std::vector<uint8_t> jpg;
    uint32_t acc = 0;
    int bits = 0;
    for (; p < d.size() && d[p] != '<'; ++p) {
        uint8_t c = d[p];
        if (c == '&') {   // Zeilenumbruch als Entitaet "&#xA;"
            while (p < d.size() && d[p] != ';') ++p;
            continue;
        }
        int8_t v = map[c];
        if (v < 0) continue;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            jpg.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
        }
    }
    return jpg.size() > 100 && DecodeImageMemory(jpg.data(), jpg.size(), out);
}

}  // namespace

bool DecodeDocThumb(const std::wstring& path, uint32_t edge, Pixels& out, MetaRow* meta) {
    std::wstring ext = ExtOf(path);
    if (ext == L"pdf" || ext == L"ai") {
        ComPtr<IPdfDocument> doc;
        if (LoadPdf(path, doc)) {
            UINT32 n = 0;
            doc->get_PageCount(&n);
            if (meta) meta->durationMs = n;   // Seitenzahl (siehe FileEntry::pages)
            if (n && RenderPdfPage(doc.Get(), edge, out)) return true;
        }
        if (ext == L"ai") return AiXmpThumb(path, out);
        return false;
    }
    if (ext == L"psd" || ext == L"psb") {
        std::vector<uint8_t> head;
        if (!ReadHead(path, 4u << 20, head)) return false;
        PsdHeader hd;
        if (!ReadPsdHeader(head, hd)) return false;
        if (meta) { meta->width = hd.w; meta->height = hd.h; }
        if (PsdComposite(path, hd, edge, out)) return true;
        return PsdEmbeddedThumb(head, out);
    }
    return false;
}

bool ReadDocMeta(const std::wstring& path, MetaRow& m) {
    std::wstring ext = ExtOf(path);
    if (ext == L"pdf" || ext == L"ai") {
        ComPtr<IPdfDocument> doc;
        if (!LoadPdf(path, doc)) return ext == L"ai";   // AI ohne PDF-Teil: trotzdem "gelesen"
        UINT32 n = 0;
        doc->get_PageCount(&n);
        m.durationMs = n;
        return true;
    }
    if (ext == L"psd" || ext == L"psb") {
        std::vector<uint8_t> head;
        PsdHeader hd;
        if (!ReadHead(path, 64, head) || !ReadPsdHeader(head, hd)) return false;
        m.width = hd.w;
        m.height = hd.h;
        return true;
    }
    return false;
}

}  // namespace peek
