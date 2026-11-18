#pragma once
// Pruefungen fuer SVG-Dateien vor dem Parsen (plattformunabhaengig, getestet in tests/test_core.cpp)
#include <string_view>

namespace peek {

// true, wenn die DTD nur einfache interne Entities enthaelt (z. B. Namespace-Abkuerzungen aus Illustrator).
// Abgelehnt werden Entities, deren Wert Markup ('<') oder Referenzen ('&', '%') enthaelt, Parameter-Entities
// und externe Entities (SYSTEM/PUBLIC). Sonst koennte eine Entity ein <image href="\\server\..."> erzeugen,
// das die Pruefung der href-Verweise im Rohtext nicht sieht.
bool SvgEntitiesSafe(std::string_view xml);

} // namespace peek
