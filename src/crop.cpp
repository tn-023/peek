// Zuschneiden im Bildbetrachter: Symbol in der oberen Leiste oder direkt mit der mittleren Maustaste aufziehen.
// Ausschnitt mit Griffen anpassen, Speichern ueberschreibt die Datei (Original geht in den Papierkorb).
#include "app.h"
#include "core/textutil.h"

#include <cmath>

namespace peek {

namespace {
constexpr float kGrip = 9.0f;   // Fangbereich der Griffe (DIP)
float Clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

// Formate, die direkt ueberschrieben werden koennen (WIC-Encoder vorhanden)
bool WritableContainer(const std::wstring& ext, GUID& g) {
    if (ext == L"jpg" || ext == L"jpeg" || ext == L"jpe" || ext == L"jfif") { g = GUID_ContainerFormatJpeg; return true; }
    if (ext == L"png") { g = GUID_ContainerFormatPng; return true; }
    if (ext == L"bmp" || ext == L"dib") { g = GUID_ContainerFormatBmp; return true; }
    if (ext == L"tif" || ext == L"tiff") { g = GUID_ContainerFormatTiff; return true; }
    return false;
}
}  // namespace

void App::EnterCrop(bool fit) {
    if (!viewer_.open || viewer_.pos < 0 || viewer_.pos >= static_cast<int>(visible_.size())) return;
    const std::wstring& ext = entries_[visible_[viewer_.pos]].ext;
    if (ext == L"svg" || ext == L"tga") return;   // Vektorgrafik bzw. TGA (kein Encoder in Windows): kein Zuschneiden
    viewer_.crop = true;
    viewer_.cropN = D2D1::RectF(0, 0, 1, 1);
    viewer_.cropDrag = 0;
    viewer_.cropInset = fit;
    if (fit) {   // ueber das Symbol: ganzes Bild etwas kleiner zeigen (weich animiert); mittlere Maustaste: Ansicht behalten
        viewer_.zoom = 0;
        viewer_.pan = {0, 0};
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::CancelCrop() {
    viewer_.crop = false;
    viewer_.cropDrag = 0;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

D2D1_RECT_F App::CropScreenRect() const {
    const D2D1_RECT_F& b = viewer_.imgBox;
    float w = b.right - b.left, h = b.bottom - b.top;
    const D2D1_RECT_F& n = viewer_.cropN;
    return D2D1::RectF(b.left + n.left * w, b.top + n.top * h, b.left + n.right * w, b.top + n.bottom * h);
}

// 3..10: Ecken oben links, oben rechts, unten rechts, unten links; Kanten oben, rechts, unten, links; 2 = innen
int App::CropHandleAt(float x, float y) const {
    D2D1_RECT_F r = CropScreenRect();
    auto nearTo = [](float a, float b) { return std::fabs(a - b) <= kGrip; };
    bool inX = x >= r.left - kGrip && x <= r.right + kGrip, inY = y >= r.top - kGrip && y <= r.bottom + kGrip;
    if (!inX || !inY) return 0;
    bool l = nearTo(x, r.left), rr = nearTo(x, r.right), t = nearTo(y, r.top), b = nearTo(y, r.bottom);
    if (t && l) return 3;
    if (t && rr) return 4;
    if (b && rr) return 5;
    if (b && l) return 6;
    if (t) return 7;
    if (rr) return 8;
    if (b) return 9;
    if (l) return 10;
    if (x > r.left && x < r.right && y > r.top && y < r.bottom) return 2;
    return 0;
}

LPCWSTR App::CropCursor(float x, float y) const {
    int hnd = viewer_.cropDrag ? viewer_.cropDrag : CropHandleAt(x, y);
    switch (hnd) {
    case 2: return IDC_SIZEALL;
    case 3: case 5: return IDC_SIZENWSE;
    case 4: case 6: return IDC_SIZENESW;
    case 7: case 9: return IDC_SIZENS;
    case 8: case 10: return IDC_SIZEWE;
    default: return IDC_CROSS;
    }
}

bool App::CropMouseDown(float x, float y, bool newRect) {
    const D2D1_RECT_F& b = viewer_.imgBox;
    float w = b.right - b.left, h = b.bottom - b.top;
    if (w < 1 || h < 1) return false;
    D2D1_POINT_2F p = D2D1::Point2F(Clamp01((x - b.left) / w), Clamp01((y - b.top) / h));
    int hnd = newRect ? 0 : CropHandleAt(x, y);
    viewer_.cropStartN = viewer_.cropN;
    viewer_.cropFrom = p;
    if (hnd == 0) {
        // neuen Ausschnitt aufziehen
        viewer_.cropDrag = 1;
        viewer_.cropN = D2D1::RectF(p.x, p.y, p.x, p.y);
    } else {
        viewer_.cropDrag = hnd;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

void App::CropMouseMove(float x, float y) {
    if (!viewer_.cropDrag) return;
    const D2D1_RECT_F& b = viewer_.imgBox;
    float w = b.right - b.left, h = b.bottom - b.top;
    if (w < 1 || h < 1) return;
    D2D1_POINT_2F p = D2D1::Point2F(Clamp01((x - b.left) / w), Clamp01((y - b.top) / h));
    D2D1_RECT_F n = viewer_.cropStartN;
    float dx = p.x - viewer_.cropFrom.x, dy = p.y - viewer_.cropFrom.y;
    switch (viewer_.cropDrag) {
    case 1:
        n = D2D1::RectF(std::min(viewer_.cropFrom.x, p.x), std::min(viewer_.cropFrom.y, p.y),
                        std::max(viewer_.cropFrom.x, p.x), std::max(viewer_.cropFrom.y, p.y));
        break;
    case 2: {
        float cw = n.right - n.left, ch = n.bottom - n.top;
        float nl = std::clamp(n.left + dx, 0.0f, 1.0f - cw), nt = std::clamp(n.top + dy, 0.0f, 1.0f - ch);
        n = D2D1::RectF(nl, nt, nl + cw, nt + ch);
        break;
    }
    default: {
        int hnd = viewer_.cropDrag;
        if (hnd == 3 || hnd == 6 || hnd == 10) n.left = p.x;
        if (hnd == 4 || hnd == 5 || hnd == 8) n.right = p.x;
        if (hnd == 3 || hnd == 4 || hnd == 7) n.top = p.y;
        if (hnd == 5 || hnd == 6 || hnd == 9) n.bottom = p.y;
        if (n.left > n.right) std::swap(n.left, n.right);
        if (n.top > n.bottom) std::swap(n.top, n.bottom);
        break;
    }
    }
    viewer_.cropN = n;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::CropMouseUp() {
    if (!viewer_.cropDrag) return;
    viewer_.cropDrag = 0;
    // zu kleiner Ausschnitt (nur geklickt): vorherigen behalten
    const D2D1_RECT_F& b = viewer_.imgBox;
    D2D1_RECT_F n = viewer_.cropN;
    if ((n.right - n.left) * (b.right - b.left) < 6 || (n.bottom - n.top) * (b.bottom - b.top) < 6)
        viewer_.cropN = viewer_.cropStartN.right > viewer_.cropStartN.left ? viewer_.cropStartN : D2D1::RectF(0, 0, 1, 1);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::PaintCrop(ID2D1DeviceContext* dc) {
    const D2D1_RECT_F b = viewer_.imgBox;
    D2D1_RECT_F r = CropScreenRect();
    // ausserhalb abdunkeln
    ID2D1SolidColorBrush* shade = r_.Brush(D2D1::ColorF(0, 0, 0, 0.6f));
    dc->FillRectangle(D2D1::RectF(b.left, b.top, b.right, r.top), shade);
    dc->FillRectangle(D2D1::RectF(b.left, r.bottom, b.right, b.bottom), shade);
    dc->FillRectangle(D2D1::RectF(b.left, r.top, r.left, r.bottom), shade);
    dc->FillRectangle(D2D1::RectF(r.right, r.top, b.right, r.bottom), shade);
    // Drittel-Linien beim Ziehen
    if (viewer_.cropDrag) {
        ID2D1SolidColorBrush* g = r_.Brush(D2D1::ColorF(1, 1, 1, 0.35f));
        for (int i = 1; i < 3; ++i) {
            float gx = r.left + (r.right - r.left) * i / 3, gy = r.top + (r.bottom - r.top) * i / 3;
            dc->DrawLine(D2D1::Point2F(gx, r.top), D2D1::Point2F(gx, r.bottom), g, 1.0f);
            dc->DrawLine(D2D1::Point2F(r.left, gy), D2D1::Point2F(r.right, gy), g, 1.0f);
        }
    }
    ID2D1SolidColorBrush* white = r_.Brush(D2D1::ColorF(1, 1, 1, 1));
    dc->DrawRectangle(r, white, 1.5f);
    // Eckgriffe als kraeftige Winkel, Kantengriffe als kurze Balken
    const float L = std::min(18.0f, std::min(r.right - r.left, r.bottom - r.top) / 2), kT = 4;
    auto bar = [&](float x0, float y0, float x1, float y1) { dc->FillRectangle(D2D1::RectF(x0, y0, x1, y1), white); };
    bar(r.left - kT / 2, r.top - kT / 2, r.left + L, r.top + kT / 2);
    bar(r.left - kT / 2, r.top - kT / 2, r.left + kT / 2, r.top + L);
    bar(r.right - L, r.top - kT / 2, r.right + kT / 2, r.top + kT / 2);
    bar(r.right - kT / 2, r.top - kT / 2, r.right + kT / 2, r.top + L);
    bar(r.right - L, r.bottom - kT / 2, r.right + kT / 2, r.bottom + kT / 2);
    bar(r.right - kT / 2, r.bottom - L, r.right + kT / 2, r.bottom + kT / 2);
    bar(r.left - kT / 2, r.bottom - kT / 2, r.left + L, r.bottom + kT / 2);
    bar(r.left - kT / 2, r.bottom - L, r.left + kT / 2, r.bottom + kT / 2);
    float mx = (r.left + r.right) / 2, my = (r.top + r.bottom) / 2;
    if (r.right - r.left > 3 * L) {
        bar(mx - L / 2, r.top - kT / 2, mx + L / 2, r.top + kT / 2);
        bar(mx - L / 2, r.bottom - kT / 2, mx + L / 2, r.bottom + kT / 2);
    }
    if (r.bottom - r.top > 3 * L) {
        bar(r.left - kT / 2, my - L / 2, r.left + kT / 2, my + L / 2);
        bar(r.right - kT / 2, my - L / 2, r.right + kT / 2, my + L / 2);
    }
    // Groesse in Pixeln
    const FileEntry& e = entries_[visible_[viewer_.pos]];
    uint32_t fw = e.width, fh = e.height;
    auto fit = fullBmps_.find(e.path);
    if (fit != fullBmps_.end() && fit->second.srcW) { fw = fit->second.srcW; fh = fit->second.srcH; }
    if (viewer_.rotation % 2 == 1) std::swap(fw, fh);
    if (fw && fh) {
        const D2D1_RECT_F& n = viewer_.cropN;
        std::wstring s = std::to_wstring(static_cast<long>(std::lround((n.right - n.left) * fw))) + L" × " +
                         std::to_wstring(static_cast<long>(std::lround((n.bottom - n.top) * fh))) + L" px";
        IDWriteTextFormat* f = r_.Text(12, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
        float tw = r_.MeasureText(s, f) + 18;
        float ty = r.bottom + 10;
        if (ty + 26 > H_ - 8) ty = r.bottom - 36;
        D2D1_RECT_F lr = D2D1::RectF(mx - tw / 2, ty, mx + tw / 2, ty + 26);
        dc->FillRoundedRectangle(D2D1::RoundedRect(lr, 5, 5), r_.Brush(D2D1::ColorF(0, 0, 0, 0.7f)));
        DrawText(dc, s, lr, f, D2D1::ColorF(1, 1, 1, 1));
    }
}

void App::SaveCrop() {
    int n = static_cast<int>(visible_.size());
    if (!viewer_.open || !viewer_.crop || viewer_.pos < 0 || viewer_.pos >= n) return;
    const FileEntry e = entries_[visible_[viewer_.pos]];
    D2D1_RECT_F c = viewer_.cropN;
    if (c.left <= 0.0005f && c.top <= 0.0005f && c.right >= 0.9995f && c.bottom >= 0.9995f) {
        CancelCrop();   // nichts zugeschnitten
        return;
    }
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    GUID container;
    std::wstring dir = ParentPath(e.path);
    std::wstring base = e.name.substr(0, e.name.find_last_of(L'.'));
    bool overwrite = WritableContainer(e.ext, container);
    std::wstring target;
    if (overwrite) {
        // Zwischendatei ohne Bildendung, damit sie nie als Bild in der Ansicht auftaucht
        target = dir + L"\\~" + base + L"." + e.ext + L".peek-tmp";
    } else {
        // WebP, AVIF, GIF, ICO: als PNG-Kopie daneben
        container = GUID_ContainerFormatPng;
        for (int i = 1; i < 1000; ++i) {
            target = dir + L"\\" + base + (i == 1 ? T(L" (zugeschnitten)") : T(L" (zugeschnitten ") + std::to_wstring(i) + L")") + L".png";
            if (GetFileAttributesW(LongPath(target).c_str()) == INVALID_FILE_ATTRIBUTES) break;
        }
    }
    uint32_t ow = 0, oh = 0;
    savingCrop_ = true;   // waehrend des Speicherns (Papierkorb-Dialog pumpt Nachrichten) nicht neu einlesen
    struct Done { bool& f; ~Done() { f = false; } } done{savingCrop_};
    bool ok = GuardedCall([&] {
        return SaveCroppedImage(e.path, viewer_.rotation, c.left, c.top, c.right, c.bottom, target, container, &ow, &oh);
    });
    if (!ok) {
        DeleteFileW(LongPath(target).c_str());
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        viewer_.toast = T(L"Zuschnitt konnte nicht gespeichert werden");
        viewer_.toastUntil = GetTickCount() + 2500;
        SetTimer(hwnd_, kTimerToast, 2600, nullptr);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (overwrite) {
        // Original in den Papierkorb (wiederherstellbar), dann die neue Datei an seinen Platz
        if (!RecycleFiles(hwnd_, {e.path}) ||
            !MoveFileExW(LongPath(target).c_str(), LongPath(e.path).c_str(), MOVEFILE_REPLACE_EXISTING)) {
            DeleteFileW(LongPath(target).c_str());
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            viewer_.toast = T(L"Zuschnitt nicht gespeichert – Original unverändert");
            viewer_.toastUntil = GetTickCount() + 2500;
            SetTimer(hwnd_, kTimerToast, 2600, nullptr);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        // Bild neu laden (alte Vorschau verwerfen), Drehung ist im gespeicherten Bild enthalten
        fullBmps_.erase(e.path);
        bmps_.erase(e.path);
        viewer_.rotation = 0;
        viewer_.toast = TF(L"Zugeschnitten auf {0} × {1} px – Original im Papierkorb", {std::to_wstring(ow), std::to_wstring(oh)});
        viewerSwitchTo_ = e.path;
    } else {
        viewer_.toast = T(L"Als Kopie gespeichert: ") + target.substr(dir.size() + 1);
        viewerSwitchTo_ = target;   // zur Kopie wechseln, sobald sie eingelesen ist
    }
    // sofort neu einlesen (die Aenderungsmeldungen des Ordners folgen ohnehin)
    if (!collectionsView_ && !bookmarksView_) {   // in Sammlungen/Merkliste wuerde Neuladen den Betrachter schliessen
        KillTimer(hwnd_, kTimerReload);
        SetTimer(hwnd_, kTimerReload, 150, nullptr);
    } else {
        viewerSwitchTo_.clear();
    }
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    viewer_.crop = false;
    viewer_.cropDrag = 0;
    viewer_.zoom = 0;
    viewer_.pan = {0, 0};
    viewer_.animS = 0;
    viewer_.toastUntil = GetTickCount() + 3000;
    SetTimer(hwnd_, kTimerToast, 3100, nullptr);
    ViewerPrefetch();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

}  // namespace peek
