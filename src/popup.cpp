#include "popup.h"
#include "renderer.h"

#include <cmath>

namespace peek {

namespace {

constexpr float kItemH = 32.0f;
constexpr float kSepH = 9.0f;
constexpr float kPad = 4.0f;
constexpr UINT_PTR kTimerSub = 1;
constexpr UINT kSubDelay = 280;   // ms bis ein Untermenue beim Darueberfahren auf- bzw. zuklappt

// Ein Menuefenster; Untermenues sind weitere PopupMenu-Instanzen (child_), die modale Schleife und die
// Mauserfassung gehoeren dem obersten Menue (root).
class PopupMenu {
public:
    UINT Run(HWND owner, const std::vector<MenuItem>& items, POINT pt, const D2D1_COLOR_F& accent);

private:
    static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l);
    LRESULT Handle(UINT m, WPARAM w, LPARAM l);
    bool Open(HWND owner, const std::vector<MenuItem>& items, POINT pt, const D2D1_COLOR_F& accent, const RECT* beside);
    void Close();
    void Paint();
    int ItemAt(float x, float y) const;
    void MoveHover(int dir);
    void EnsureVisible(int i);
    bool Selectable(int i) const {
        return i >= 0 && i < static_cast<int>(items_.size()) && !items_[i].separator && !items_[i].disabled;
    }
    bool HasSub(int i) const { return Selectable(i) && items_[i].submenu != nullptr; }
    PopupMenu* Root() { return root_ ? root_ : this; }
    PopupMenu* Deepest() { PopupMenu* m = this; while (m->child_) m = m->child_.get(); return m; }
    PopupMenu* MenuAtScreen(POINT pt);
    void OpenSub(int i, bool focusFirst);
    void CloseSub();
    void SetHover(int h);
    void Activate(int i);   // Klick/Enter auf einen Eintrag
    void Finish(UINT id) { PopupMenu* r = Root(); r->result_ = id; r->done_ = true; }

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    Renderer r_;
    std::vector<MenuItem> items_;
    std::vector<float> tops_;
    float width_ = 200, height_ = 100, contentH_ = 0, scroll_ = 0;
    int hover_ = -1;
    bool done_ = false;
    UINT result_ = 0;
    D2D1_COLOR_F accent_{};
    std::vector<Microsoft::WRL::ComPtr<ID2D1Bitmap>> icons_;
    PopupMenu* root_ = nullptr;
    PopupMenu* parent_ = nullptr;
    std::unique_ptr<PopupMenu> child_;
    int childOf_ = -1;      // Eintrag, zu dem child_ gehoert
    int pendingSub_ = -2;   // Eintrag, dessen Untermenue nach kSubDelay auf- bzw. das bisherige zuklappen soll
};

bool PopupMenu::Open(HWND owner, const std::vector<MenuItem>& items, POINT pt, const D2D1_COLOR_F& accent,
                     const RECT* beside) {
    items_ = items;
    accent_ = accent;
    owner_ = owner;
    static bool registered = false;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = Proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"peek.popup";
        RegisterClassExW(&wc);
        registered = true;
    }
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP, L"peek.popup", L"",
                            WS_POPUP, pt.x, pt.y, 1, 1, owner, nullptr, inst, this);
    if (!hwnd_) return false;

    // Dunkel, abgerundet, Acrylic-Hintergrund
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    int backdrop = DWMSBT_TRANSIENTWINDOW;
    if (SUCCEEDED(DwmSetWindowAttribute(hwnd_, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop)))) {
        MARGINS m = {-1, -1, -1, -1};
        DwmExtendFrameIntoClientArea(hwnd_, &m);
    }
    if (!r_.Init(hwnd_)) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return false;
    }

    // Groesse bestimmen
    IDWriteTextFormat* f = r_.Text(13);
    IDWriteTextFormat* fr = r_.Text(12);
    float maxText = 0, maxRight = 0;
    bool anySub = false;
    float y = kPad;
    tops_.clear();
    for (auto& it : items_) {
        tops_.push_back(y);
        if (it.separator) { y += kSepH; continue; }
        maxText = std::max(maxText, r_.MeasureText(it.text, f));
        if (!it.right.empty()) maxRight = std::max(maxRight, r_.MeasureText(it.right, fr));
        if (it.submenu) anySub = true;
        y += kItemH;
    }
    tops_.push_back(y);
    contentH_ = y + kPad;
    float rightW = std::max(maxRight > 0 ? 32 + maxRight : 0.0f, anySub ? 40.0f : 0.0f);
    width_ = std::clamp(36 + maxText + rightW + 20, 180.0f, 720.0f);

    float s = r_.scale();
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    RECT wa = mi.rcWork;
    float maxH = (wa.bottom - wa.top) / s * 0.75f;
    height_ = std::min(contentH_, maxH);
    int wpx = static_cast<int>(std::ceil(width_ * s)), hpx = static_cast<int>(std::ceil(height_ * s));
    int x = pt.x, yy = pt.y;
    if (beside) {
        // Untermenue rechts neben dem Eintrag, sonst links davon
        x = beside->right - static_cast<int>(2 * s);
        if (x + wpx > wa.right) x = std::max<int>(wa.left, beside->left - wpx + static_cast<int>(2 * s));
        yy = beside->top - static_cast<int>(kPad * s);
        if (yy + hpx > wa.bottom) yy = std::max<int>(wa.top, wa.bottom - hpx);
    } else {
        if (x + wpx > wa.right) x = std::max<int>(wa.left, wa.right - wpx);
        if (yy + hpx > wa.bottom) yy = std::max<int>(wa.top, pt.y - hpx - 8);   // nach oben aufklappen
    }
    SetWindowPos(hwnd_, HWND_TOPMOST, x, yy, wpx, hpx, SWP_NOACTIVATE);
    r_.Resize();
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    Paint();
    return true;
}

void PopupMenu::Close() {
    CloseSub();
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

UINT PopupMenu::Run(HWND owner, const std::vector<MenuItem>& items, POINT pt, const D2D1_COLOR_F& accent) {
    if (!Open(owner, items, pt, accent, nullptr)) return 0;
    for (int i = 0; i < static_cast<int>(items_.size()); ++i)
        if (items_[i].checked && Selectable(i)) { hover_ = i; break; }
    if (hover_ >= 0) { EnsureVisible(hover_); Paint(); }
    SetCapture(hwnd_);

    // Modale Schleife (Tastatur geht an das tiefste offene Menue)
    while (!done_) {
        MSG msg;
        BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got <= 0) {
            PostQuitMessage(static_cast<int>(msg.wParam));
            break;
        }
        if (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) {
            PopupMenu* m = Deepest();
            switch (msg.wParam) {
            case VK_UP: m->MoveHover(-1); break;
            case VK_DOWN: m->MoveHover(1); break;
            case VK_HOME: m->hover_ = -1; m->MoveHover(1); break;
            case VK_END: m->hover_ = static_cast<int>(m->items_.size()); m->MoveHover(-1); break;
            case VK_RIGHT:
                if (m->HasSub(m->hover_)) m->OpenSub(m->hover_, true);
                break;
            case VK_RETURN: case VK_SPACE:
                m->Activate(m->hover_);
                break;
            case VK_LEFT: case VK_ESCAPE:
                if (m->parent_) m->parent_->CloseSub();
                else done_ = true;
                break;
            case VK_MENU: case VK_F10: case VK_TAB: done_ = true; break;
            default: break;
            }
            continue;
        }
        if (msg.message == WM_CHAR || msg.message == WM_SYSCHAR || msg.message == WM_KEYUP || msg.message == WM_SYSKEYUP)
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (GetCapture() == hwnd_) ReleaseCapture();
    KillTimer(hwnd_, kTimerSub);
    Close();
    return result_;
}

LRESULT CALLBACK PopupMenu::Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto self = reinterpret_cast<PopupMenu*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        self = static_cast<PopupMenu*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        self->hwnd_ = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self) return self->Handle(m, w, l);
    return DefWindowProcW(h, m, w, l);
}

int PopupMenu::ItemAt(float x, float y) const {
    if (x < 0 || x >= width_ || y < 0 || y >= height_) return -1;
    float cy = y + scroll_;
    for (size_t i = 0; i < items_.size(); ++i)
        if (cy >= tops_[i] && cy < tops_[i + 1]) return static_cast<int>(i);
    return -1;
}

void PopupMenu::EnsureVisible(int i) {
    if (i < 0) return;
    float top = tops_[i], bottom = tops_[i + 1];
    if (top - kPad < scroll_) scroll_ = std::max(0.0f, top - kPad);
    if (bottom + kPad > scroll_ + height_) scroll_ = bottom + kPad - height_;
    scroll_ = std::clamp(scroll_, 0.0f, std::max(0.0f, contentH_ - height_));
}

void PopupMenu::MoveHover(int dir) {
    int n = static_cast<int>(items_.size());
    int i = hover_;
    for (int k = 0; k < n; ++k) {
        i += dir;
        if (i < 0) i = n - 1;
        if (i >= n) i = 0;
        if (Selectable(i)) {
            hover_ = i;
            EnsureVisible(i);
            Paint();
            return;
        }
    }
}

PopupMenu* PopupMenu::MenuAtScreen(POINT pt) {
    // tiefstes Menue zuerst
    std::vector<PopupMenu*> chain;
    for (PopupMenu* m = this; m; m = m->child_.get()) chain.push_back(m);
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        RECT rc;
        if ((*it)->hwnd_ && GetWindowRect((*it)->hwnd_, &rc) && PtInRect(&rc, pt)) return *it;
    }
    return nullptr;
}

void PopupMenu::CloseSub() {
    if (!child_) return;
    child_->Close();
    child_.reset();
    childOf_ = -1;
    Paint();
}

void PopupMenu::OpenSub(int i, bool focusFirst) {
    if (!HasSub(i)) return;
    if (child_ && childOf_ == i) {
        if (focusFirst && child_->hover_ < 0) child_->MoveHover(1);
        return;
    }
    CloseSub();
    std::vector<MenuItem> sub = items_[i].submenu();
    if (sub.empty()) return;
    // Bildschirmrechteck des Eintrags
    float s = r_.scale();
    RECT wr;
    GetWindowRect(hwnd_, &wr);
    RECT beside = {wr.left, wr.top + static_cast<LONG>((tops_[i] - scroll_) * s), wr.right,
                   wr.top + static_cast<LONG>((tops_[i + 1] - scroll_) * s)};
    auto c = std::make_unique<PopupMenu>();
    c->root_ = Root();
    c->parent_ = this;
    POINT pt = {beside.right, beside.top};
    if (!c->Open(owner_, sub, pt, accent_, &beside)) return;
    child_ = std::move(c);
    childOf_ = i;
    hover_ = i;
    if (focusFirst) child_->MoveHover(1);
    Paint();
}

void PopupMenu::SetHover(int h) {
    if (!Selectable(h)) h = -1;
    if (h != hover_) {
        hover_ = h;
        Paint();
    }
    // Untermenue verzoegert auf- bzw. zuklappen (wie im Explorer)
    int want = HasSub(h) ? h : -1;
    if (want != childOf_ || (want < 0 && child_)) {
        if (h < 0 && child_) return;   // in einer Luecke: offenes Untermenue stehen lassen
        pendingSub_ = want;
        SetTimer(Root()->hwnd_, kTimerSub, kSubDelay, nullptr);
    } else {
        pendingSub_ = -2;
    }
}

void PopupMenu::Activate(int i) {
    if (!Selectable(i)) return;
    if (HasSub(i)) {
        OpenSub(i, true);
        return;
    }
    Finish(items_[i].id);
}

LRESULT PopupMenu::Handle(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd_, &ps);
        EndPaint(hwnd_, &ps);
        Paint();
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_TIMER:
        if (w == kTimerSub) {
            KillTimer(hwnd_, kTimerSub);
            // das Menue mit ausstehender Aenderung finden
            for (PopupMenu* mm = this; mm; mm = mm->child_.get()) {
                if (mm->pendingSub_ == -2) continue;
                int p = mm->pendingSub_;
                mm->pendingSub_ = -2;
                if (p >= 0) mm->OpenSub(p, false);
                else mm->CloseSub();
                break;
            }
        }
        return 0;
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MOUSEWHEEL: {
        // Die Maus ist am obersten Menue erfasst: an das Menue unter dem Zeiger weiterleiten
        POINT pt;
        if (m == WM_MOUSEWHEEL) pt = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        else {
            pt = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            ClientToScreen(hwnd_, &pt);
        }
        PopupMenu* target = MenuAtScreen(pt);
        if (!target) {
            if (m == WM_LBUTTONDOWN || m == WM_RBUTTONDOWN || m == WM_MBUTTONDOWN) done_ = true;   // Klick ausserhalb
            return 0;
        }
        POINT lp = pt;
        ScreenToClient(target->hwnd_, &lp);
        float s = target->r_.scale() > 0 ? target->r_.scale() : 1.0f;
        float x = lp.x / s, y = lp.y / s;
        int h = target->ItemAt(x, y);
        if (m == WM_MOUSEMOVE) {
            target->SetHover(h);
            // im Untermenue: Elternmenue haelt den Eintrag markiert
            if (target->parent_) target->parent_->pendingSub_ = -2;
        } else if (m == WM_LBUTTONDOWN && target->HasSub(h)) {
            target->OpenSub(h, false);
        } else if (m == WM_LBUTTONUP || m == WM_RBUTTONUP) {
            if (target->Selectable(h) && !target->HasSub(h)) target->Finish(target->items_[h].id);
        } else if (m == WM_MOUSEWHEEL) {
            target->scroll_ = std::clamp(target->scroll_ - GET_WHEEL_DELTA_WPARAM(w) / 120.0f * kItemH * 3, 0.0f,
                                         std::max(0.0f, target->contentH_ - target->height_));
            target->Paint();
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (!root_ && reinterpret_cast<HWND>(l) != hwnd_) done_ = true;
        return 0;
    }
    return DefWindowProcW(hwnd_, m, w, l);
}

void PopupMenu::Paint() {
    if (!hwnd_ || !r_.BeginDraw()) return;
    ID2D1DeviceContext* dc = r_.dc();
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    D2D1_RECT_F all = D2D1::RectF(0, 0, width_, height_);
    dc->FillRectangle(all, r_.Brush(D2D1::ColorF(0.13f, 0.13f, 0.13f, 0.72f)));
    dc->PushAxisAlignedClip(all, D2D1_ANTIALIAS_MODE_ALIASED);
    IDWriteTextFormat* f = r_.Text(13);
    IDWriteTextFormat* fr = r_.Text(12, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
    for (size_t i = 0; i < items_.size(); ++i) {
        const MenuItem& it = items_[i];
        float top = tops_[i] - scroll_, bottom = tops_[i + 1] - scroll_;
        if (bottom < 0 || top > height_) continue;
        if (it.separator) {
            float my = std::round((top + bottom) / 2 * r_.scale()) / r_.scale() + 0.5f;
            dc->DrawLine(D2D1::Point2F(8, my), D2D1::Point2F(width_ - 8, my), r_.Brush(D2D1::ColorF(1, 1, 1, 0.08f)), 1.0f);
            continue;
        }
        D2D1_RECT_F rc = D2D1::RectF(kPad, top, width_ - kPad, bottom);
        if (static_cast<int>(i) == hover_ || static_cast<int>(i) == childOf_)
            dc->FillRoundedRectangle(D2D1::RoundedRect(rc, 4, 4), r_.Brush(D2D1::ColorF(1, 1, 1, 0.08f)));
        D2D1_COLOR_F fg = it.disabled ? D2D1::ColorF(1, 1, 1, 0.36f) : D2D1::ColorF(1, 1, 1, 0.92f);
        if (!it.iconPx.empty() && !it.checked) {
            if (icons_.size() != items_.size()) icons_.resize(items_.size());
            if (!icons_[i]) {
                D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
                dc->CreateBitmap(D2D1::SizeU(it.iconW, it.iconH), it.iconPx.data(), it.iconW * 4, bp, &icons_[i]);
            }
            if (icons_[i]) {
                float cy = (rc.top + rc.bottom) / 2;
                dc->DrawBitmap(icons_[i].Get(), D2D1::RectF(rc.left + 11, cy - 8, rc.left + 27, cy + 8), it.disabled ? 0.4f : 1.0f,
                               D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            }
        } else if (it.glyph && !it.checked) {
            wchar_t g[2] = {it.glyph, 0};
            dc->DrawTextW(g, 1, r_.Icon(13), D2D1::RectF(rc.left + 8, rc.top, rc.left + 30, rc.bottom), r_.Brush(fg));
        }
        if (it.checked) {
            D2D1_RECT_F bar = D2D1::RectF(rc.left, rc.top + 9, rc.left + 3, rc.bottom - 9);
            dc->FillRoundedRectangle(D2D1::RoundedRect(bar, 1.5f, 1.5f), r_.Brush(accent_));
            wchar_t g[2] = {0xE73E, 0};
            dc->DrawTextW(g, 1, r_.Icon(12), D2D1::RectF(rc.left + 8, rc.top, rc.left + 30, rc.bottom), r_.Brush(accent_));
        }
        dc->DrawTextW(it.text.c_str(), static_cast<UINT32>(it.text.size()), f,
                      D2D1::RectF(rc.left + 32, rc.top, rc.right - 12, rc.bottom), r_.Brush(fg));
        if (it.submenu) {
            // gut sichtbarer Pfeil zum Untermenue
            wchar_t g[2] = {0xE76C, 0};
            dc->DrawTextW(g, 1, r_.Icon(14), D2D1::RectF(rc.right - 34, rc.top, rc.right - 8, rc.bottom), r_.Brush(fg));
        } else if (!it.right.empty()) {
            dc->DrawTextW(it.right.c_str(), static_cast<UINT32>(it.right.size()), fr,
                          D2D1::RectF(rc.left + 32, rc.top, rc.right - 12, rc.bottom),
                          r_.Brush(D2D1::ColorF(1, 1, 1, it.disabled ? 0.25f : 0.5f)));
        }
    }
    dc->PopAxisAlignedClip();
    r_.EndDraw();
}

// ---------------------------------------------------------------------------
// Popup mit eigenem Inhalt (gleiches Fenster-Design wie die Menues)
// ---------------------------------------------------------------------------
class PanelHost {
public:
    void Run(HWND owner, PopupPanel& panel, POINT pt, const D2D1_COLOR_F& accent);

private:
    static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l);
    LRESULT Handle(UINT m, WPARAM w, LPARAM l);
    void Place();
    void Paint();

    HWND hwnd_ = nullptr;
    Renderer r_;
    PopupPanel* panel_ = nullptr;
    POINT pt_{};
    float width_ = 100, height_ = 100;
};

void PanelHost::Run(HWND owner, PopupPanel& panel, POINT pt, const D2D1_COLOR_F&) {
    panel_ = &panel;
    pt_ = pt;
    static bool registered = false;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = Proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"peek.panel";
        RegisterClassExW(&wc);
        registered = true;
    }
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP, L"peek.panel", L"", WS_POPUP,
                            pt.x, pt.y, 1, 1, owner, nullptr, inst, this);
    if (!hwnd_) return;
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    int backdrop = DWMSBT_TRANSIENTWINDOW;
    if (SUCCEEDED(DwmSetWindowAttribute(hwnd_, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop)))) {
        MARGINS m = {-1, -1, -1, -1};
        DwmExtendFrameIntoClientArea(hwnd_, &m);
    }
    if (!r_.Init(hwnd_)) {
        DestroyWindow(hwnd_);
        return;
    }
    Place();
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    Paint();
    SetCapture(hwnd_);
    while (!panel.close) {
        MSG msg;
        BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got <= 0) {
            PostQuitMessage(static_cast<int>(msg.wParam));
            break;
        }
        if (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) {
            if (msg.wParam == VK_ESCAPE || msg.wParam == VK_MENU || msg.wParam == VK_F10 || msg.wParam == VK_TAB)
                panel.close = true;
            else
                panel.OnKey(msg.wParam);
        } else if (msg.message != WM_CHAR && msg.message != WM_SYSCHAR && msg.message != WM_KEYUP &&
                   msg.message != WM_SYSKEYUP) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (panel.dirty && !panel.close) {
            Place();
            Paint();
        }
    }
    if (GetCapture() == hwnd_) ReleaseCapture();
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
}

void PanelHost::Place() {
    D2D1_SIZE_F sz = panel_->Measure(r_);
    width_ = sz.width;
    height_ = sz.height;
    float s = r_.scale();
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(pt_, MONITOR_DEFAULTTONEAREST), &mi);
    RECT wa = mi.rcWork;
    int wpx = static_cast<int>(std::ceil(width_ * s)), hpx = static_cast<int>(std::ceil(height_ * s));
    int x = pt_.x, y = pt_.y;
    if (x + wpx > wa.right) x = std::max<int>(wa.left, wa.right - wpx);
    if (y + hpx > wa.bottom) y = std::max<int>(wa.top, wa.bottom - hpx);
    RECT cur;
    GetWindowRect(hwnd_, &cur);
    if (cur.left != x || cur.top != y || cur.right - cur.left != wpx || cur.bottom - cur.top != hpx) {
        SetWindowPos(hwnd_, HWND_TOPMOST, x, y, wpx, hpx, SWP_NOACTIVATE);
        r_.Resize();
    }
}

LRESULT CALLBACK PanelHost::Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto self = reinterpret_cast<PanelHost*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        self = static_cast<PanelHost*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        self->hwnd_ = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self) return self->Handle(m, w, l);
    return DefWindowProcW(h, m, w, l);
}

LRESULT PanelHost::Handle(UINT m, WPARAM w, LPARAM l) {
    float s = r_.scale() > 0 ? r_.scale() : 1.0f;
    float x = GET_X_LPARAM(l) / s, y = GET_Y_LPARAM(l) / s;
    bool inside = x >= 0 && y >= 0 && x < width_ && y < height_;
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd_, &ps);
        EndPaint(hwnd_, &ps);
        Paint();
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_MOUSEMOVE:
        panel_->OnMouse(WM_MOUSEMOVE, x, y);
        return 0;
    case WM_LBUTTONDOWN:
        if (!inside) panel_->close = true;   // Klick ausserhalb: schliessen
        else panel_->OnMouse(WM_LBUTTONDOWN, x, y);
        return 0;
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        if (!inside) panel_->close = true;
        return 0;
    case WM_LBUTTONUP:
        panel_->OnMouse(WM_LBUTTONUP, x, y);
        return 0;
    case WM_CAPTURECHANGED:
        if (reinterpret_cast<HWND>(l) != hwnd_) panel_->close = true;
        return 0;
    }
    (void)w;
    return DefWindowProcW(hwnd_, m, w, l);
}

void PanelHost::Paint() {
    if (!hwnd_ || !r_.BeginDraw()) return;
    ID2D1DeviceContext* dc = r_.dc();
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    D2D1_RECT_F all = D2D1::RectF(0, 0, width_, height_);
    dc->FillRectangle(all, r_.Brush(D2D1::ColorF(0.13f, 0.13f, 0.13f, 0.72f)));
    dc->PushAxisAlignedClip(all, D2D1_ANTIALIAS_MODE_ALIASED);
    panel_->Paint(r_, width_, height_);
    dc->PopAxisAlignedClip();
    r_.EndDraw();
    panel_->dirty = false;
}

} // namespace

UINT ShowPopupMenu(HWND owner, const std::vector<MenuItem>& items, POINT screen, const D2D1_COLOR_F& accent) {
    if (items.empty()) return 0;
    PopupMenu menu;
    return menu.Run(owner, items, screen, accent);
}

void ShowPopupPanel(HWND owner, PopupPanel& panel, POINT screen, const D2D1_COLOR_F& accent) {
    PanelHost host;
    host.Run(owner, panel, screen, accent);
}

} // namespace peek
