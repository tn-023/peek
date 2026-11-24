#include "core/aimeta.h"

#include <algorithm>
#include <cstdint>
#include <cctype>
#include <cstdlib>
#include <cwctype>
#include <map>
#include <memory>
#include <set>

namespace peek {

namespace {

constexpr size_t kMaxPrompt = 8000;   // Zeichen
constexpr size_t kMaxNames = 32;

// --- kleiner JSON-Leser (nur, was fuer die Metadaten noetig ist; Tiefe und Groesse begrenzt)
struct Json {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::vector<std::pair<std::string, Json>> o;
    const Json* Get(std::string_view k) const {
        if (type != Obj) return nullptr;
        for (auto& [kk, v] : o)
            if (kk == k) return &v;
        return nullptr;
    }
    const std::string* Text(std::string_view k) const {
        const Json* v = Get(k);
        return v && v->type == Str ? &v->s : nullptr;
    }
};

class JsonReader {
public:
    explicit JsonReader(std::string_view t) : t_(t) {}
    bool Parse(Json& out) {
        if (!Value(out, 0)) return false;
        Ws();
        return true;
    }

private:
    std::string_view t_;
    size_t i_ = 0;
    size_t nodes_ = 0;
    void Ws() {
        while (i_ < t_.size() && (t_[i_] == ' ' || t_[i_] == '\n' || t_[i_] == '\r' || t_[i_] == '\t')) ++i_;
    }
    bool Lit(std::string_view w) {
        if (t_.substr(i_, w.size()) != w) return false;
        i_ += w.size();
        return true;
    }
    static void PutUtf8(std::string& s, uint32_t cp) {
        if (cp < 0x80) s += static_cast<char>(cp);
        else if (cp < 0x800) { s += static_cast<char>(0xC0 | (cp >> 6)); s += static_cast<char>(0x80 | (cp & 63)); }
        else if (cp < 0x10000) {
            s += static_cast<char>(0xE0 | (cp >> 12));
            s += static_cast<char>(0x80 | ((cp >> 6) & 63));
            s += static_cast<char>(0x80 | (cp & 63));
        } else {
            s += static_cast<char>(0xF0 | (cp >> 18));
            s += static_cast<char>(0x80 | ((cp >> 12) & 63));
            s += static_cast<char>(0x80 | ((cp >> 6) & 63));
            s += static_cast<char>(0x80 | (cp & 63));
        }
    }
    bool Hex4(uint32_t& v) {
        if (i_ + 4 > t_.size()) return false;
        v = 0;
        for (int k = 0; k < 4; ++k) {
            char c = t_[i_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return false;
        }
        return true;
    }
    bool String(std::string& s) {
        if (i_ >= t_.size() || t_[i_] != '"') return false;
        ++i_;
        while (i_ < t_.size()) {
            char c = t_[i_++];
            if (c == '"') return true;
            if (c != '\\') { s += c; continue; }
            if (i_ >= t_.size()) return false;
            char e = t_[i_++];
            switch (e) {
            case 'n': s += '\n'; break;
            case 't': s += '\t'; break;
            case 'r': s += '\r'; break;
            case 'b': s += '\b'; break;
            case 'f': s += '\f'; break;
            case 'u': {
                uint32_t cp;
                if (!Hex4(cp)) return false;
                if (cp >= 0xD800 && cp < 0xDC00 && i_ + 6 <= t_.size() && t_[i_] == '\\' && t_[i_ + 1] == 'u') {
                    size_t save = i_;
                    i_ += 2;
                    uint32_t lo;
                    if (Hex4(lo) && lo >= 0xDC00 && lo < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    else i_ = save;
                }
                PutUtf8(s, cp);
                break;
            }
            default: s += e; break;   // \" \\ \/
            }
        }
        return false;
    }
    bool Value(Json& v, int depth) {
        if (depth > 64 || ++nodes_ > 2000000) return false;
        Ws();
        if (i_ >= t_.size()) return false;
        char c = t_[i_];
        if (c == '{') {
            v.type = Json::Obj;
            ++i_;
            Ws();
            if (i_ < t_.size() && t_[i_] == '}') { ++i_; return true; }
            for (;;) {
                Ws();
                std::string k;
                if (!String(k)) return false;
                Ws();
                if (i_ >= t_.size() || t_[i_] != ':') return false;
                ++i_;
                v.o.emplace_back(std::move(k), Json());
                if (!Value(v.o.back().second, depth + 1)) return false;
                Ws();
                if (i_ < t_.size() && t_[i_] == ',') { ++i_; continue; }
                if (i_ < t_.size() && t_[i_] == '}') { ++i_; return true; }
                return false;
            }
        }
        if (c == '[') {
            v.type = Json::Arr;
            ++i_;
            Ws();
            if (i_ < t_.size() && t_[i_] == ']') { ++i_; return true; }
            for (;;) {
                v.a.emplace_back();
                if (!Value(v.a.back(), depth + 1)) return false;
                Ws();
                if (i_ < t_.size() && t_[i_] == ',') { ++i_; continue; }
                if (i_ < t_.size() && t_[i_] == ']') { ++i_; return true; }
                return false;
            }
        }
        if (c == '"') { v.type = Json::Str; return String(v.s); }
        if (Lit("true")) { v.type = Json::Bool; v.b = true; return true; }
        if (Lit("false")) { v.type = Json::Bool; return true; }
        if (Lit("null")) return true;
        if (Lit("NaN") || Lit("Infinity") || Lit("-Infinity")) { v.type = Json::Num; return true; }
        size_t s0 = i_;
        while (i_ < t_.size() && (std::isdigit(static_cast<unsigned char>(t_[i_])) || t_[i_] == '-' || t_[i_] == '+' ||
                                  t_[i_] == '.' || t_[i_] == 'e' || t_[i_] == 'E'))
            ++i_;
        if (i_ == s0) return false;
        v.type = Json::Num;
        v.n = std::strtod(std::string(t_.substr(s0, i_ - s0)).c_str(), nullptr);
        return true;
    }
};

std::string Trimmed(std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

void AddName(std::vector<std::wstring>& v, const std::string& utf8) {
    std::wstring n = ModelStem(Utf8ToWideCore(Trimmed(utf8)));
    if (n.empty() || v.size() >= kMaxNames) return;
    if (std::find(v.begin(), v.end(), n) == v.end()) v.push_back(n);
}

void SetPrompt(AiMeta& m, const std::string& utf8) {
    std::wstring p = Utf8ToWideCore(Trimmed(utf8));
    if (p.size() > kMaxPrompt) p.resize(kMaxPrompt);
    m.prompt = p;
}

// --- ComfyUI: Graph im API-Format {"id": {"class_type": ..., "inputs": {...}}}
class Comfy {
public:
    explicit Comfy(const Json& g) : g_(g) {}

    void Run(AiMeta& m) {
        std::vector<std::string> prompts;
        // Startpunkte: Knoten mit "positive" (Sampler, Guider)
        for (auto& [id, node] : g_.o) {
            const Json* in = Inputs(node);
            if (!in || !in->Get("positive")) continue;
            Cond(*in->Get("positive"), prompts, 0);
            if (const Json* mo = in->Get("model")) Model(*mo, m, 0);
        }
        if (prompts.empty()) {   // ohne Sampler: alle Texte von Textkodierern
            for (auto& [id, node] : g_.o) {
                const std::string* ct = node.Text("class_type");
                const Json* in = Inputs(node);
                if (!ct || !in || ct->find("TextEncode") == std::string::npos) continue;
                if (const Json* t = in->Get("text")) {
                    std::string s = TextOf(*t, 0);
                    if (!s.empty()) prompts.push_back(s);
                }
            }
        }
        if (m.models.empty() && m.loras.empty()) {   // ohne erkennbare Kette: alle Lader
            for (auto& [id, node] : g_.o)
                if (const Json* in = Inputs(node)) Loaders(*in, m);
        }
        std::string all;
        std::set<std::string> seen;
        for (auto& p : prompts) {
            std::string t = Trimmed(p);
            if (t.empty() || !seen.insert(t).second) continue;
            if (!all.empty()) all += "\n";
            all += t;
        }
        if (!all.empty()) SetPrompt(m, all);
    }

private:
    const Json& g_;

    static const Json* Inputs(const Json& node) {
        const Json* in = node.Get("inputs");
        return in && in->type == Json::Obj ? in : nullptr;
    }
    // Verweis ["id", Ausgang] -> Knoten
    const Json* Linked(const Json& v) const {
        if (v.type != Json::Arr || v.a.size() != 2) return nullptr;
        std::string id = v.a[0].type == Json::Str ? v.a[0].s : v.a[0].type == Json::Num ? std::to_string(static_cast<long long>(v.a[0].n)) : "";
        const Json* n = g_.Get(id);
        return n && n->type == Json::Obj ? n : nullptr;
    }
    bool BoolOf(const Json& v, int depth) const {
        if (v.type == Json::Bool) return v.b;
        if (v.type == Json::Num) return v.n != 0;
        if (const Json* n = Linked(v); n && depth < 32)
            if (const Json* in = Inputs(*n))
                for (const char* k : {"value", "boolean", "bool"})
                    if (const Json* x = in->Get(k)) return BoolOf(*x, depth + 1);
        return true;
    }
    // Umschalter: der gewaehlte Zweig (sonst nullptr)
    const Json* Switched(const Json& in, int depth) const {
        const Json* sw = in.Get("switch");
        if (!sw) sw = in.Get("boolean");
        const Json* t = in.Get("on_true");
        const Json* f = in.Get("on_false");
        if (!sw || (!t && !f)) return nullptr;
        bool on = BoolOf(*sw, depth + 1);
        return on ? (t ? t : f) : (f ? f : t);
    }
    std::string TextOf(const Json& v, int depth) const {
        if (depth > 48) return {};
        if (v.type == Json::Str) return v.s;
        const Json* n = Linked(v);
        if (!n) return {};
        const Json* in = Inputs(*n);
        if (!in) return {};
        if (const Json* s = Switched(*in, depth)) return TextOf(*s, depth + 1);
        const std::string* ct = n->Text("class_type");
        if (ct && ct->find("TextGenerate") != std::string::npos) return {};   // Ergebnis eines Sprachmodells: unbekannt
        // Verketten: string_a, string_b … (leere Teile entfallen)
        if (in->Get("string_a") || in->Get("text_a")) {
            std::string delim = ", ";
            if (const std::string* d = in->Text("delimiter")) delim = *d;
            std::string out;
            for (auto& [k, x] : in->o) {
                if (k.rfind("string_", 0) != 0 && k.rfind("text_", 0) != 0) continue;
                std::string part = Trimmed(TextOf(x, depth + 1));
                if (part.empty()) continue;
                if (!out.empty()) out += delim;
                out += part;
            }
            return out;
        }
        for (const char* k : {"value", "text", "string", "prompt", "text_g", "wildcard_text", "populated_text", "source", "input", "any"})
            if (const Json* x = in->Get(k)) {
                std::string s = TextOf(*x, depth + 1);
                if (!s.empty()) return s;
            }
        return {};
    }
    void Cond(const Json& v, std::vector<std::string>& out, int depth) const {
        if (depth > 48) return;
        const Json* n = Linked(v);
        if (!n) return;
        const Json* in = Inputs(*n);
        if (!in) return;
        if (const Json* s = Switched(*in, depth)) { Cond(*s, out, depth + 1); return; }
        bool any = false;
        for (const char* k : {"text", "text_g", "prompt"})
            if (const Json* t = in->Get(k)) {
                std::string s = TextOf(*t, depth + 1);
                if (!s.empty()) { out.push_back(s); any = true; }
                break;
            }
        if (any) return;
        for (auto& [k, x] : in->o)
            if (k.find("conditioning") != std::string::npos || k == "positive" || k == "cond") Cond(x, out, depth + 1);
    }
    static void Loaders(const Json& in, AiMeta& m) {
        for (auto& [k, x] : in.o) {
            if (x.type == Json::Str) {
                if (k == "lora_name" || k == "lora") AddName(m.loras, x.s);
                else if (k == "ckpt_name" || k == "unet_name" || k == "model_name" || k == "gguf_name") AddName(m.models, x.s);
            } else if (x.type == Json::Obj) {   // z. B. Power Lora Loader: {"on": true, "lora": "...", "strength": 1}
                const Json* on = x.Get("on");
                if (const std::string* l = x.Text("lora"); l && (!on || on->type != Json::Bool || on->b)) AddName(m.loras, *l);
            }
        }
    }
    void Model(const Json& v, AiMeta& m, int depth) const {
        if (depth > 48) return;
        const Json* n = Linked(v);
        if (!n) return;
        const Json* in = Inputs(*n);
        if (!in) return;
        if (const Json* s = Switched(*in, depth)) { Model(*s, m, depth + 1); return; }
        Loaders(*in, m);
        if (const Json* next = in->Get("model")) Model(*next, m, depth + 1);
    }
};

// --- Automatic1111/Forge: "Prompt\nNegative prompt: …\nSteps: 20, Sampler: …, Model: name, Lora hashes: \"a: 1, b: 2\""
void ParseA1111(std::string_view text, AiMeta& m) {
    std::string t(text);
    size_t neg = t.find("\nNegative prompt:");
    size_t steps = t.rfind("\nSteps: ");
    if (steps == std::string::npos && t.rfind("Steps: ", 0) == 0) steps = 0;
    size_t end = std::min(neg, steps);
    std::string prompt = end == std::string::npos ? t : t.substr(0, end);
    SetPrompt(m, prompt);
    // <lora:name:0.8> im Prompt
    for (size_t p = prompt.find("<lora:"); p != std::string::npos; p = prompt.find("<lora:", p + 1)) {
        size_t a = p + 6, b = prompt.find_first_of(":>", a);
        if (b != std::string::npos) AddName(m.loras, prompt.substr(a, b - a));
    }
    if (steps == std::string::npos) return;
    std::string params = t.substr(steps);
    if (size_t mp = params.find(", Model: "); mp != std::string::npos || (mp = params.find("Model: ")) != std::string::npos) {
        size_t a = params.find("Model: ", mp) + 7, b = params.find(',', a);
        AddName(m.models, params.substr(a, b == std::string::npos ? std::string::npos : b - a));
    }
    if (size_t lh = params.find("Lora hashes: \""); lh != std::string::npos) {
        size_t a = lh + 14, b = params.find('"', a);
        std::string list = params.substr(a, b == std::string::npos ? std::string::npos : b - a);
        size_t s = 0;
        while (s < list.size()) {
            size_t e = list.find(',', s);
            if (e == std::string::npos) e = list.size();
            std::string item = list.substr(s, e - s);
            size_t c = item.find(':');
            AddName(m.loras, item.substr(0, c));
            s = e + 1;
        }
    }
}

void ParseInvoke(const Json& j, AiMeta& m) {
    if (const std::string* p = j.Text("positive_prompt")) SetPrompt(m, *p);
    auto nameOf = [](const Json* x) -> const std::string* {
        if (!x) return nullptr;
        if (x->type == Json::Str) return &x->s;
        if (const std::string* s = x->Text("name")) return s;
        return x->Text("model_name");
    };
    if (const std::string* mn = nameOf(j.Get("model"))) AddName(m.models, *mn);
    if (const Json* ls = j.Get("loras"); ls && ls->type == Json::Arr)
        for (auto& l : ls->a) {
            const Json* inner = l.Get("lora");
            if (!inner) inner = l.Get("model");
            if (const std::string* n = nameOf(inner)) AddName(m.loras, *n);
        }
}

}  // namespace

std::wstring Utf8ToWideCore(std::string_view s) {
    std::wstring out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t cp = 0;
        int extra = 0;
        if (c < 0x80) cp = c;
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
        else { cp = c; }   // kein gueltiges UTF-8: als Latin-1 lesen
        bool ok = i + extra < s.size() + (extra ? 0 : 1);
        for (int k = 1; ok && k <= extra; ++k) {
            unsigned char d = static_cast<unsigned char>(s[i + k]);
            if ((d & 0xC0) != 0x80) ok = false;
            else cp = (cp << 6) | (d & 0x3F);
        }
        if (!ok) { cp = c; extra = 0; }
        i += 1 + extra;
        if (cp >= 0x10000 && sizeof(wchar_t) == 2) {
            cp -= 0x10000;
            out += static_cast<wchar_t>(0xD800 + (cp >> 10));
            out += static_cast<wchar_t>(0xDC00 + (cp & 0x3FF));
        } else {
            out += static_cast<wchar_t>(cp);
        }
    }
    return out;
}

std::wstring ModelStem(std::wstring_view name) {
    std::wstring n(name);
    size_t slash = n.find_last_of(L"\\/");
    if (slash != std::wstring::npos) n.erase(0, slash + 1);
    for (const wchar_t* ext : {L".safetensors", L".ckpt", L".pt", L".pth", L".gguf", L".bin", L".sft"}) {
        size_t el = std::char_traits<wchar_t>::length(ext);
        if (n.size() > el) {
            bool same = true;
            for (size_t k = 0; k < el && same; ++k) same = static_cast<wchar_t>(std::towlower(n[n.size() - el + k])) == ext[k];
            if (same) { n.resize(n.size() - el); break; }
        }
    }
    // Steuerzeichen (Trenner der kompakten Form) entfernen
    n.erase(std::remove_if(n.begin(), n.end(), [](wchar_t c) { return c < 32; }), n.end());
    return n;
}

void ParseAiText(std::string_view key, std::string_view value, AiMeta& m) {
    if (value.size() > (16u << 20)) return;
    if (key == "prompt") {   // ComfyUI
        Json j;
        if (!JsonReader(value).Parse(j) || j.type != Json::Obj) return;
        Comfy(j).Run(m);
    } else if (key == "parameters") {
        ParseA1111(value, m);
    } else if (key == "invokeai_metadata") {
        Json j;
        if (JsonReader(value).Parse(j) && j.type == Json::Obj) ParseInvoke(j, m);
    }
}

std::wstring EncodeAi(const AiMeta& m) {
    if (m.Empty()) return {};
    std::wstring s;
    for (wchar_t c : m.prompt) s += c == 0x1E || c == 0x1F ? L' ' : c;
    s += static_cast<wchar_t>(0x1E);
    for (size_t i = 0; i < m.models.size(); ++i) s += (i ? std::wstring(1, 0x1F) : std::wstring()) + m.models[i];
    s += static_cast<wchar_t>(0x1E);
    for (size_t i = 0; i < m.loras.size(); ++i) s += (i ? std::wstring(1, 0x1F) : std::wstring()) + m.loras[i];
    return s;
}

AiMeta DecodeAi(std::wstring_view s) {
    AiMeta m;
    if (s.empty()) return m;
    size_t a = s.find(static_cast<wchar_t>(0x1E));
    if (a == std::wstring_view::npos) { m.prompt = std::wstring(s); return m; }
    size_t b = s.find(static_cast<wchar_t>(0x1E), a + 1);
    m.prompt = std::wstring(s.substr(0, a));
    auto list = [](std::wstring_view part, std::vector<std::wstring>& out) {
        size_t p = 0;
        while (p < part.size()) {
            size_t e = part.find(static_cast<wchar_t>(0x1F), p);
            if (e == std::wstring_view::npos) e = part.size();
            if (e > p) out.emplace_back(part.substr(p, e - p));
            p = e + 1;
        }
    };
    list(s.substr(a + 1, b == std::wstring_view::npos ? std::wstring_view::npos : b - a - 1), m.models);
    if (b != std::wstring_view::npos) list(s.substr(b + 1), m.loras);
    return m;
}

} // namespace peek
