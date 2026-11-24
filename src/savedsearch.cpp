// Gespeicherte Suchen: Ort (Ordner mit/ohne Unterordner, alle Sammlungen, Merkliste), Suchtext, Filterpillen und
// Sortierung. Sie stehen in der Seitenleiste unter den Sammlungen. "Suche speichern" erscheint links neben dem
// Sortieren-Knopf, solange ein Suchergebnis angezeigt wird; ist es eine gespeicherte Suche, steht dort "Suche loeschen".
#include "app.h"
#include "core/textutil.h"

namespace peek {

namespace {

// Werte im Filtertext: ; = | % maskieren
std::wstring Esc(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) {
        if (c == L'%') o += L"%25";
        else if (c == L';') o += L"%3B";
        else if (c == L'=') o += L"%3D";
        else if (c == L'|') o += L"%7C";
        else o += c;
    }
    return o;
}

std::wstring Unesc(const std::wstring& s) {
    std::wstring o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'%' && i + 2 < s.size()) {
            wchar_t hex[3] = {s[i + 1], s[i + 2], 0};
            o += static_cast<wchar_t>(wcstol(hex, nullptr, 16));
            i += 2;
        } else {
            o += s[i];
        }
    }
    return o;
}

std::vector<std::wstring> Split(const std::wstring& s, wchar_t sep) {
    std::vector<std::wstring> out;
    size_t a = 0;
    while (a <= s.size()) {
        size_t b = s.find(sep, a);
        if (b == std::wstring::npos) b = s.size();
        if (b > a) out.push_back(s.substr(a, b - a));
        a = b + 1;
    }
    return out;
}

const wchar_t kBookmarksPlace[] = L"::merkliste";   // lang:ok

}  // namespace

bool App::SearchActive() const {
    if (!collectionsView_ && !bookmarksView_ && path_.empty()) return false;
    return !search_.empty() || filters_.Any();
}

// Filterpillen als Text, nur von der Voreinstellung abweichende Werte, immer in derselben Reihenfolge
std::wstring App::FiltersText() const {
    const Filters& f = filters_;
    std::wstring o;
    auto add = [&](const wchar_t* k, const std::wstring& v) {
        if (!o.empty()) o += L';';
        o += k;
        o += L'=';
        o += v;
    };
    auto num = [](int64_t v) { return std::to_wstring(v); };
    if (f.type >= 0) add(L"t", num(f.type));
    if (f.date) {
        add(L"d", num(f.date));
        if (f.date == 7) { add(L"df", num(f.dateFrom)); add(L"dt", num(f.dateTo)); }
    }
    if (f.taken) {
        add(L"k", num(f.taken));
        if (f.taken == 7) { add(L"kf", num(f.takenFrom)); add(L"kt", num(f.takenTo)); }
    }
    if (f.size) add(L"s", num(f.size));
    if (f.res) add(L"r", num(f.res));
    if (f.orient) add(L"o", num(f.orient));
    if (!f.source.empty()) add(L"src", Esc(f.source));
    if (f.templ != UINT32_MAX && f.templ < templDict_.Size()) add(L"tpl", Esc(templDict_.Word(f.templ)));
    if (!f.terms.empty()) {
        std::wstring t;
        for (uint32_t id : f.terms) {
            if (id >= dict_.Size()) continue;
            if (!t.empty()) t += L'|';
            t += Esc(dict_.Word(id));
        }
        if (!t.empty()) add(L"terms", t);
    }
    return o;
}

// Gegenstueck zu FiltersText; Muster und Begriffe ueber die Woerterbuecher (unbekannte entfallen)
Filters App::ParseFiltersText(const std::wstring& text) {
    Filters f;
    for (auto& part : Split(text, L';')) {
        size_t eq = part.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring k = part.substr(0, eq), v = part.substr(eq + 1);
        auto n = [&] { return _wtoi64(v.c_str()); };
        if (k == L"t") f.type = static_cast<int>(n());
        else if (k == L"d") f.date = static_cast<int>(n());
        else if (k == L"df") f.dateFrom = n();
        else if (k == L"dt") f.dateTo = n();
        else if (k == L"k") f.taken = static_cast<int>(n());
        else if (k == L"kf") f.takenFrom = n();
        else if (k == L"kt") f.takenTo = n();
        else if (k == L"s") f.size = static_cast<int>(n());
        else if (k == L"r") f.res = static_cast<int>(n());
        else if (k == L"o") f.orient = static_cast<int>(n());
        else if (k == L"src") f.source = Unesc(v);
        else if (k == L"tpl") {
            std::wstring w = Unesc(v);
            for (uint32_t id = 0; id < templDict_.Size(); ++id)
                if (templDict_.Word(id) == w) { f.templ = id; break; }
        } else if (k == L"terms") {
            for (auto& w0 : Split(v, L'|')) {
                std::wstring w = Unesc(w0);
                for (uint32_t id = 0; id < dict_.Size(); ++id)
                    if (dict_.Word(id) == w) { f.terms.push_back(id); break; }
            }
        }
    }
    return f;
}

Settings::SavedSearch App::CurrentSearch() const {
    Settings::SavedSearch s;
    if (bookmarksView_) s.place = kBookmarksPlace;
    else if (!collectionsView_) {
        s.place = path_;
        s.recursive = settings_.recursive;
    }
    s.query = search_;
    s.filters = FiltersText();
    s.sort = settings_.sortKey | (settings_.sortDesc ? 16 : 0) | (settings_.group ? 32 : 0);
    return s;
}

int App::MatchingSavedSearch() const {
    if (!SearchActive()) return -1;
    const auto cur = CurrentSearch();
    for (size_t i = 0; i < settings_.searches.size(); ++i) {
        const auto& s = settings_.searches[i];
        if (_wcsicmp(s.place.c_str(), cur.place.c_str()) == 0 && s.recursive == cur.recursive && s.query == cur.query &&
            s.filters == cur.filters && s.sort == cur.sort)
            return static_cast<int>(i);
    }
    return -1;
}

// Automatischer Name: Suchtext (sonst die aktiven Filter) und der Ort, z. B. "hund · Reise"
std::wstring App::SavedSearchName(const Settings::SavedSearch& s) const {
    std::wstring what = s.query;
    while (!what.empty() && what.front() == L' ') what.erase(0, 1);
    while (!what.empty() && what.back() == L' ') what.pop_back();
    if (what.empty()) {
        std::vector<std::wstring> parts;
        if (filters_.type >= 0) parts.push_back(ChipLabel(ChipId::Type));
        if (filters_.date) parts.push_back(ChipLabel(ChipId::Date));
        if (filters_.taken) parts.push_back(ChipLabel(ChipId::Taken));
        if (filters_.size) parts.push_back(ChipLabel(ChipId::Size));
        if (filters_.res) parts.push_back(ChipLabel(ChipId::Resolution));
        if (filters_.orient) parts.push_back(ChipLabel(ChipId::Orientation));
        if (!filters_.source.empty()) parts.push_back(ChipLabel(ChipId::Source));
        if (filters_.templ != UINT32_MAX) parts.push_back(ChipLabel(ChipId::Pattern));
        for (uint32_t id : filters_.terms)
            if (id < dict_.Size()) parts.push_back(dict_.Word(id));
        for (auto& p : parts) {
            if (!what.empty()) what += L", ";
            what += p;
        }
    }
    std::wstring place;
    if (s.place.empty()) place = T(L"Alle Sammlungen");
    else if (s.place == kBookmarksPlace) place = T(L"Merkliste");
    else {
        place = FileNameOf(s.place);
        if (place.empty()) place = s.place;
    }
    std::wstring base = what.empty() ? place : what + L" · " + place;
    std::wstring name = base;
    for (int n = 2; n < 1000; ++n) {
        bool taken = false;
        for (auto& o : settings_.searches) taken = taken || _wcsicmp(o.name.c_str(), name.c_str()) == 0;
        if (!taken) break;
        name = base + L" (" + std::to_wstring(n) + L")";
    }
    return name;
}

void App::SaveCurrentSearch() {
    if (!SearchActive() || MatchingSavedSearch() >= 0) return;
    auto s = CurrentSearch();
    s.name = SavedSearchName(s);
    settings_.searches.push_back(s);
    settings_.Save();
    BuildSidebar();
    SetStatus(TF(L"Suche „{0}“ gespeichert.", {s.name}));
}

void App::DeleteSavedSearch(int i) {
    if (i < 0 || i >= static_cast<int>(settings_.searches.size())) return;
    std::wstring name = settings_.searches[static_cast<size_t>(i)].name;
    settings_.searches.erase(settings_.searches.begin() + i);
    settings_.Save();
    BuildSidebar();
    SetStatus(TF(L"Suche „{0}“ gelöscht.", {name}));
}

void App::OpenSavedSearch(int i) {
    if (i < 0 || i >= static_cast<int>(settings_.searches.size())) return;
    const auto s = settings_.searches[static_cast<size_t>(i)];
    if (!s.place.empty() && s.place != kBookmarksPlace && !IsNetworkPath(s.place) && !DirectoryExists(s.place)) {
        SetStatus(T(L"Ordner nicht gefunden: ") + s.place);
        return;
    }
    if (renameHost_) FinishRename(true);
    searchFocus_ = false;
    showHistory_ = false;
    EditSearch(s.query, s.query.size());
    filters_ = ParseFiltersText(s.filters);   // Muster/Begriffe ggf. erst nach dem Einlesen bekannt
    pendingSaved_ = i;
    if (s.place.empty()) ShowAllCollections();
    else if (s.place == kBookmarksPlace) ShowBookmarks();
    else {
        forceRecursive_ = s.recursive ? 1 : 0;
        Navigate(s.place);
        forceRecursive_ = -1;
    }
    pendingSavedGen_ = gen_;
    // Sortierung der Suche (die gespeicherte Sortierung des Ordners bleibt unberuehrt)
    settings_.sortKey = std::clamp(s.sort & 15, 0, 5);
    settings_.sortDesc = (s.sort & 16) != 0;
    settings_.group = (s.sort & 32) != 0;
    if (!loading_) ApplySavedPending();   // Merkliste: sofort eingelesen
    else Refilter();
}

// nach dem Einlesen: Filter vollstaendig setzen (Ordnerwechsel leert Quelle, Muster und Begriffe)
void App::ApplySavedPending() {
    if (pendingSaved_ < 0) return;
    const int i = pendingSaved_;
    pendingSaved_ = -1;
    if (pendingSavedGen_ != gen_ || i >= static_cast<int>(settings_.searches.size())) return;
    filters_ = ParseFiltersText(settings_.searches[static_cast<size_t>(i)].filters);
    Refilter();
}

// Umbenennen in der Seitenleiste: Eingabefeld ueber dem Namen der Suche
void App::BeginRenameSearch(int sidebarIndex) {
    if (renameHost_) FinishRename(true);
    if (sidebarIndex < 0 || sidebarIndex >= static_cast<int>(sidebar_.size())) return;
    const SidebarItem& it = sidebar_[static_cast<size_t>(sidebarIndex)];
    if (it.kind != SidebarItem::SavedSearch || it.idx < 0 || it.idx >= static_cast<int>(settings_.searches.size())) return;
    if (!settings_.sidebar) {
        settings_.sidebar = true;
        sidebarAnim_ = 1;
    }
    D2D1_RECT_F rc{};
    bool found = false;
    for (auto& h : hits_)
        if (h.type == HitType::SidebarItem && h.a == sidebarIndex) { rc = h.rc; found = true; break; }
    if (!found) return;
    const float fieldH = 26;
    float top = (rc.top + rc.bottom) / 2 - fieldH / 2;
    renameSearch_ = it.idx;
    const std::wstring& name = settings_.searches[static_cast<size_t>(it.idx)].name;
    OpenRenameField(D2D1::RectF(rc.left + 40, top, rc.right - 2, top + fieldH), fieldH, name, name.size());
}

}  // namespace peek
