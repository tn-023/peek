#include "core/svgcheck.h"

namespace peek {

namespace {
bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
}

bool SvgEntitiesSafe(std::string_view xml) {
    size_t pos = 0;
    for (;;) {
        size_t p = xml.find("<!ENTITY", pos);
        if (p == std::string_view::npos) return true;
        size_t q = p + 8;
        if (q >= xml.size() || !IsSpace(xml[q])) return false;
        while (q < xml.size() && IsSpace(xml[q])) ++q;
        if (q >= xml.size() || xml[q] == '%') return false;   // Parameter-Entity
        while (q < xml.size() && !IsSpace(xml[q]) && xml[q] != '"' && xml[q] != '\'') ++q;   // Name
        while (q < xml.size() && IsSpace(xml[q])) ++q;
        if (q >= xml.size() || (xml[q] != '"' && xml[q] != '\'')) return false;   // SYSTEM, PUBLIC oder kaputt
        char quote = xml[q];
        size_t end = xml.find(quote, q + 1);
        if (end == std::string_view::npos) return false;
        std::string_view value = xml.substr(q + 1, end - q - 1);
        if (value.find_first_of("<&%") != std::string_view::npos) return false;
        pos = end + 1;
    }
}

} // namespace peek
