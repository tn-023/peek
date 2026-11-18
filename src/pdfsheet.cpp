#include "pdfsheet.h"
#include "core/lang.h"
#include "fs.h"
#include "imaging.h"
#include "sandbox.h"

#include <fontsub.h>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

namespace peek {

std::wstring SheetPageLabel(int page, int pages) {
    return TF(L"Seite {0} von {1}", {std::to_wstring(page), std::to_wstring(pages)});
}

namespace {

std::vector<UINT32> CodePoints(const std::wstring& s) {
    std::vector<UINT32> cps;
    for (size_t i = 0; i < s.size(); ++i) {
        UINT32 c = s[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            ++i;
        }
        cps.push_back(c);
    }
    return cps;
}

std::wstring FontsDir() {
    PWSTR p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Fonts, 0, nullptr, &p)) && p) dir = p;
    if (p) CoTaskMemFree(p);
    if (dir.empty()) dir = L"C:\\Windows\\Fonts";
    return dir;
}

}  // namespace

bool SheetFont::Load(bool bold) {
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(factory_.GetAddressOf()))))
        return false;
    std::wstring dir = FontsDir();
    const wchar_t* candidates[2] = {bold ? L"segoeuib.ttf" : L"segoeui.ttf", bold ? L"arialbd.ttf" : L"arial.ttf"};
    for (auto c : candidates) {
        std::wstring path = dir + L"\\" + c;
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        ComPtr<IDWriteFontFile> file;
        ComPtr<IDWriteFontFace> face;
        if (FAILED(factory_->CreateFontFileReference(path.c_str(), nullptr, &file)) ||
            FAILED(factory_->CreateFontFace(DWRITE_FONT_FACE_TYPE_TRUETYPE, 1, file.GetAddressOf(), 0,
                                            DWRITE_FONT_SIMULATIONS_NONE, &face)))
            continue;
        // Einbettungserlaubnis (OS/2 fsType): "eingeschraenkt" oder "nur Bitmap" -> naechste Schrift
        const void* data = nullptr;
        UINT32 size = 0;
        void* ctx = nullptr;
        BOOL exists = FALSE;
        bool ok = true;
        if (SUCCEEDED(face->TryGetFontTable(DWRITE_MAKE_OPENTYPE_TAG('O', 'S', '/', '2'), &data, &size, &ctx, &exists)) &&
            exists) {
            if (size >= 10) {
                auto b = static_cast<const uint8_t*>(data);
                uint16_t fs = static_cast<uint16_t>((b[8] << 8) | b[9]);
                if ((fs & 0x000F) == 0x0002 || (fs & 0x0200)) ok = false;
            }
            face->ReleaseFontTable(ctx);
        }
        embeddable_ = ok;
        if (!ok && c != candidates[1]) continue;
        face_ = face;
        file_ = path;
        DWRITE_FONT_METRICS m{};
        face_->GetMetrics(&m);
        upem_ = m.designUnitsPerEm ? m.designUnitsPerEm : 2048;
        ascent_ = static_cast<float>(m.ascent) / upem_;
        line_ = static_cast<float>(m.ascent + m.descent) / upem_;
        return true;
    }
    return false;
}

void SheetFont::Shape(const std::wstring& s, float size, std::vector<UINT16>& glyphs, std::vector<float>& adv) const {
    glyphs.clear();
    adv.clear();
    if (!face_ || s.empty()) return;
    auto cps = CodePoints(s);
    glyphs.resize(cps.size());
    face_->GetGlyphIndices(cps.data(), static_cast<UINT32>(cps.size()), glyphs.data());
    std::vector<DWRITE_GLYPH_METRICS> gm(glyphs.size());
    face_->GetDesignGlyphMetrics(glyphs.data(), static_cast<UINT32>(glyphs.size()), gm.data());
    adv.resize(glyphs.size());
    for (size_t i = 0; i < glyphs.size(); ++i) adv[i] = gm[i].advanceWidth * size / upem_;
}

float SheetFont::Width(const std::wstring& s, float size) const {
    std::vector<UINT16> g;
    std::vector<float> a;
    Shape(s, size, g, a);
    float w = 0;
    for (float x : a) w += x;
    return w;
}

namespace {

// ---------------------------------------------------------------------------
// PDF-Datei
class Pdf {
public:
    bool Open(const std::wstring& path) {
        f_ = _wfopen(path.c_str(), L"wb");
        if (!f_) return false;
        Raw("%PDF-1.7\n%\xE2\xE3\xCF\xD3\n");
        return true;
    }
    ~Pdf() { if (f_) fclose(f_); }
    int Reserve() { offsets_.push_back(0); return static_cast<int>(offsets_.size()); }
    void Begin(int id) {
        offsets_[id - 1] = static_cast<long long>(_ftelli64(f_));
        Raw(std::to_string(id) + " 0 obj\n");
    }
    void End() { Raw("\nendobj\n"); }
    void Raw(const std::string& s) { fwrite(s.data(), 1, s.size(), f_); }
    void Bytes(const void* p, size_t n) { fwrite(p, 1, n, f_); }
    void Stream(int id, const std::string& dict, const void* data, size_t n) {
        Begin(id);
        Raw("<< " + dict + " /Length " + std::to_string(n) + " >>\nstream\n");
        Bytes(data, n);
        Raw("\nendstream");
        End();
    }
    bool Finish(int root) {
        long long xref = _ftelli64(f_);
        Raw("xref\n0 " + std::to_string(offsets_.size() + 1) + "\n0000000000 65535 f \n");
        char buf[32];
        for (auto o : offsets_) {
            snprintf(buf, sizeof(buf), "%010lld 00000 n \n", o);
            Raw(buf);
        }
        Raw("trailer\n<< /Size " + std::to_string(offsets_.size() + 1) + " /Root " + std::to_string(root) +
            " 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n");
        bool ok = !ferror(f_);
        ok = fclose(f_) == 0 && ok;
        f_ = nullptr;
        return ok;
    }

private:
    FILE* f_ = nullptr;
    std::vector<long long> offsets_;
};

std::string Num(float v) {
    char b[32];
    snprintf(b, sizeof(b), "%.3f", v);
    std::string s = b;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s.empty() || s == "-0" ? "0" : s;
}

// Eingebettete Schrift (Type0 / CIDFontType2, Identity-H, Glyph-IDs als CIDs)
struct PdfFont {
    const SheetFont* font = nullptr;
    int type0 = 0;
    std::map<UINT16, UINT32> used;   // Glyph -> Unicode (fuer ToUnicode)
    std::map<UINT16, int> widths;    // Glyph -> Breite in 1000/em

    // Textoperator fuer eine Zeile; zeichnet ab (x, baseline) in PDF-Koordinaten
    std::string Show(const std::wstring& s, float size, float x, float baselineY, const char* name) {
        std::vector<UINT16> g;
        std::vector<float> a;
        font->Shape(s, size, g, a);
        auto cps = CodePoints(s);
        std::string hex;
        char b[8];
        for (size_t i = 0; i < g.size(); ++i) {
            snprintf(b, sizeof(b), "%04X", g[i]);
            hex += b;
            if (!used.count(g[i])) used[g[i]] = i < cps.size() ? cps[i] : 0xFFFD;
            widths[g[i]] = static_cast<int>(std::lround(a[i] / size * 1000));
        }
        return "BT /" + std::string(name) + " " + Num(size) + " Tf 1 0 0 1 " + Num(x) + " " + Num(baselineY) + " Tm <" + hex +
               "> Tj ET\n";
    }

    void Write(Pdf& pdf, const std::string& tag) {
        int cid = pdf.Reserve(), desc = pdf.Reserve(), file = pdf.Reserve(), tu = pdf.Reserve();
        // Schriftdatei, auf die benutzten Glyphen reduziert (Glyph-IDs bleiben erhalten)
        std::vector<uint8_t> ttf;
        {
            HANDLE h = CreateFileW(font->File().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                LARGE_INTEGER sz;
                GetFileSizeEx(h, &sz);
                ttf.resize(static_cast<size_t>(sz.QuadPart));
                DWORD rd = 0;
                ReadFile(h, ttf.data(), static_cast<DWORD>(ttf.size()), &rd, nullptr);
                ttf.resize(rd);
                CloseHandle(h);
            }
        }
        std::vector<unsigned short> keep = {0};
        for (auto& [g, u] : used) keep.push_back(g);
        unsigned char* sub = nullptr;
        unsigned long subSize = 0, written = 0;
        bool subset = !ttf.empty() &&
                      CreateFontPackage(ttf.data(), static_cast<unsigned long>(ttf.size()), &sub, &subSize, &written,
                                        TTFCFP_FLAGS_SUBSET | TTFCFP_FLAGS_GLYPHLIST, 0, TTFCFP_SUBSET, 0,
                                        TTFCFP_MS_PLATFORMID, TTFCFP_DONT_CARE, keep.data(),
                                        static_cast<unsigned short>(keep.size()), malloc, realloc, free, nullptr) == 0;
        const void* fdata = subset ? static_cast<const void*>(sub) : ttf.data();
        size_t flen = subset ? written : ttf.size();
        pdf.Stream(file, "/Length1 " + std::to_string(flen), fdata, flen);
        if (sub) free(sub);

        std::string base = tag + "+" + (font->File().find(L"arial") != std::wstring::npos ? "Arial" : "SegoeUI");
        // Metriken
        DWRITE_FONT_METRICS m{};
        font->Face()->GetMetrics(&m);
        float k = 1000.0f / (m.designUnitsPerEm ? m.designUnitsPerEm : 2048);
        pdf.Begin(desc);
        pdf.Raw("<< /Type /FontDescriptor /FontName /" + base + " /Flags 32 /FontBBox [-500 " +
                Num(-m.descent * k) + " 2000 " + Num(m.ascent * k) + "] /ItalicAngle 0 /Ascent " + Num(m.ascent * k) +
                " /Descent " + Num(-m.descent * k) + " /CapHeight " + Num(m.capHeight * k) + " /StemV 80 /FontFile2 " +
                std::to_string(file) + " 0 R >>");
        pdf.End();
        std::string w = "[";
        for (auto& [g, wd] : widths) w += " " + std::to_string(g) + " [" + std::to_string(wd) + "]";
        w += " ]";
        pdf.Begin(cid);
        pdf.Raw("<< /Type /Font /Subtype /CIDFontType2 /BaseFont /" + base +
                " /CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> /FontDescriptor " +
                std::to_string(desc) + " 0 R /CIDToGIDMap /Identity /DW 1000 /W " + w + " >>");
        pdf.End();
        // ToUnicode: Text bleibt durchsuchbar und kopierbar
        std::string cmap =
            "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n/CIDSystemInfo << /Registry (Adobe) "
            "/Ordering (UCS) /Supplement 0 >> def\n/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n"
            "1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n";
        std::vector<std::pair<UINT16, UINT32>> list(used.begin(), used.end());
        for (size_t i = 0; i < list.size(); i += 100) {
            size_t n = std::min<size_t>(100, list.size() - i);
            cmap += std::to_string(n) + " beginbfchar\n";
            char b[48];
            for (size_t j = i; j < i + n; ++j) {
                UINT32 u = list[j].second;
                if (u >= 0x10000) {
                    UINT32 v = u - 0x10000;
                    snprintf(b, sizeof(b), "<%04X> <%04X%04X>\n", list[j].first, 0xD800 + (v >> 10), 0xDC00 + (v & 0x3FF));
                } else {
                    snprintf(b, sizeof(b), "<%04X> <%04X>\n", list[j].first, u);
                }
                cmap += b;
            }
            cmap += "endbfchar\n";
        }
        cmap += "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n";
        pdf.Stream(tu, "", cmap.data(), cmap.size());
        pdf.Begin(type0);
        pdf.Raw("<< /Type /Font /Subtype /Type0 /BaseFont /" + base + " /Encoding /Identity-H /DescendantFonts [" +
                std::to_string(cid) + " 0 R] /ToUnicode " + std::to_string(tu) + " 0 R >>");
        pdf.End();
    }
};

}  // namespace

bool WriteContactSheet(const SheetJob& job, const std::function<bool(float)>& progress, std::wstring& error) {
    SheetFont regular, bold;
    if (!regular.Load(false) || !bold.Load(true)) {
        error = T(L"Schrift Segoe UI nicht gefunden.");
        return false;
    }
    SheetOptions o = job.options;
    o.lineFactor = regular.LineFactor();
    SheetGeometry g = ComputeSheet(o);
    const int per = g.PerPage();
    const int pages = SheetPageCount(static_cast<int>(job.images.size()), per);
    static const float kDpi[3] = {150, 220, 300}, kQ[3] = {0.80f, 0.85f, 0.92f};
    const float dpi = kDpi[std::clamp(job.quality, 0, 2)], q = kQ[std::clamp(job.quality, 0, 2)];

    std::wstring tmp = job.output + L".peek-tmp";
    Pdf pdf;
    if (!pdf.Open(tmp)) {
        error = T(L"Datei kann nicht geschrieben werden.");
        return false;
    }
    int catalog = pdf.Reserve(), pagesObj = pdf.Reserve();
    PdfFont f1, f2;
    f1.font = &regular;
    f2.font = &bold;
    f1.type0 = pdf.Reserve();
    f2.type0 = pdf.Reserve();
    std::vector<int> kids;
    const float H = g.pageH;
    auto Y = [&](float top) { return H - top; };   // obere Kante (Punkt) -> PDF-Koordinate

    // Ueberschrift: einzeilig, am Ende gekuerzt, zentriert
    auto wBold = [&](const std::wstring& s) { return bold.Width(s, kSheetHeadSize); };
    std::wstring head = EndEllipsis(job.headline, g.header.w, wBold);
    bool cancelled = false;
    for (int p = 0; p < pages && !cancelled; ++p) {
        int pageObj = pdf.Reserve(), content = pdf.Reserve();
        kids.push_back(pageObj);
        std::string cs = "0 g\n";
        std::string xobj;
        if (!head.empty()) {
            float w = bold.Width(head, kSheetHeadSize);
            cs += f2.Show(head, kSheetHeadSize, g.header.x + (g.header.w - w) / 2, Y(g.header.y + bold.Ascent() * kSheetHeadSize), "F2");
        }
        std::wstring label = SheetPageLabel(p + 1, pages);
        float lw = regular.Width(label, kSheetFootSize);
        cs += f1.Show(label, kSheetFootSize, g.footer.x + (g.footer.w - lw) / 2, Y(g.footer.y + regular.Ascent() * kSheetFootSize), "F1");
        for (int k = 0; k < per; ++k) {
            size_t idx = static_cast<size_t>(p) * per + k;
            if (idx >= job.images.size()) break;
            if (progress && !progress(static_cast<float>(idx) / std::max<size_t>(1, job.images.size()))) {
                cancelled = true;
                break;
            }
            const auto& im = job.images[idx];
            const SheetRect& cell = g.cells[k];
            // Bild in der benoetigten Aufloesung dekodieren, auf Weiss als JPEG einbetten
            uint32_t need = static_cast<uint32_t>(std::ceil(std::max(cell.w, cell.h) / 72.0f * dpi));
            Pixels px;
            if (GuardedCall([&] { return SbDecodeImage(im.path, need, px); })) {
                SheetRect r = FitInto(cell, static_cast<float>(px.w), static_cast<float>(px.h), o.names ? 1.0f : 0.5f);
                uint32_t longEdge = static_cast<uint32_t>(std::ceil(std::max(r.w, r.h) / 72.0f * dpi));
                if (std::max(px.w, px.h) > longEdge) {
                    Pixels s;
                    if (ScalePixels(px, longEdge, s)) px = std::move(s);
                }
                std::vector<uint8_t> jpg;
                if (EncodeJpegOnWhite(px, q, jpg)) {
                    int img = pdf.Reserve();
                    pdf.Stream(img,
                               "/Type /XObject /Subtype /Image /Width " + std::to_string(px.w) + " /Height " +
                                   std::to_string(px.h) + " /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /DCTDecode",
                               jpg.data(), jpg.size());
                    std::string nm = "I" + std::to_string(k);
                    xobj += "/" + nm + " " + std::to_string(img) + " 0 R ";
                    cs += "q " + Num(r.w) + " 0 0 " + Num(r.h) + " " + Num(r.x) + " " + Num(Y(r.y + r.h)) + " cm /" + nm + " Do Q\n";
                }
            }
            if (o.names && k < static_cast<int>(g.captions.size()) && !im.caption.empty()) {
                const SheetRect& cr = g.captions[k];
                auto wReg = [&](const std::wstring& s) { return regular.Width(s, kSheetCaptionSize); };
                std::wstring cap = MiddleEllipsis(im.caption, cr.w, wReg);
                float cw = regular.Width(cap, kSheetCaptionSize);
                cs += f1.Show(cap, kSheetCaptionSize, cr.x + (cr.w - cw) / 2, Y(cr.y + regular.Ascent() * kSheetCaptionSize), "F1");
            }
        }
        pdf.Stream(content, "", cs.data(), cs.size());
        pdf.Begin(pageObj);
        pdf.Raw("<< /Type /Page /Parent " + std::to_string(pagesObj) + " 0 R /MediaBox [0 0 " + Num(g.pageW) + " " +
                Num(g.pageH) + "] /Resources << /Font << /F1 " + std::to_string(f1.type0) + " 0 R /F2 " +
                std::to_string(f2.type0) + " 0 R >> /XObject << " + xobj + ">> >> /Contents " + std::to_string(content) +
                " 0 R >>");
        pdf.End();
    }
    if (cancelled) {
        pdf.Finish(catalog);
        DeleteFileW(tmp.c_str());
        error = T(L"Abgebrochen.");
        return false;
    }
    f1.Write(pdf, "PEEKRG");
    f2.Write(pdf, "PEEKBD");
    std::string k = "[";
    for (int id : kids) k += " " + std::to_string(id) + " 0 R";
    k += " ]";
    pdf.Begin(pagesObj);
    pdf.Raw("<< /Type /Pages /Kids " + k + " /Count " + std::to_string(kids.size()) + " >>");
    pdf.End();
    pdf.Begin(catalog);
    pdf.Raw("<< /Type /Catalog /Pages " + std::to_string(pagesObj) + " 0 R >>");
    pdf.End();
    if (!pdf.Finish(catalog) || !MoveFileExW(tmp.c_str(), job.output.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        error = T(L"PDF konnte nicht gespeichert werden.");
        return false;
    }
    if (progress) progress(1.0f);
    return true;
}

} // namespace peek
