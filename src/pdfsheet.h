#pragma once
// Kontaktabzug als PDF: eigener, kleiner PDF-Schreiber (keine Fremdbibliothek).
// Bilder als JPEG (DCTDecode), Segoe UI Regular/Bold als Teilmenge eingebettet (Font Subsetting API).
#include "common.h"
#include "core/sheetlayout.h"

namespace peek {

// Schrift fuer Vorschau und PDF: dieselbe Datei, dieselben Glyphen und Breiten (WYSIWYG)
class SheetFont {
public:
    bool Load(bool bold);                    // Segoe UI (Rueckfall Arial), aus dem Windows-Schriftordner
    bool Ok() const { return face_ != nullptr; }
    IDWriteFontFace* Face() const { return face_.Get(); }
    // Glyphen und Vorschuebe (in Punkt bzw. DIP bei Groesse size)
    void Shape(const std::wstring& s, float size, std::vector<UINT16>& glyphs, std::vector<float>& adv) const;
    float Width(const std::wstring& s, float size) const;
    float Ascent() const { return ascent_; }     // je em
    float LineFactor() const { return line_; }   // (Ascent + Descent) je em
    const std::wstring& File() const { return file_; }
    bool Embeddable() const { return embeddable_; }

private:
    ComPtr<IDWriteFactory> factory_;
    ComPtr<IDWriteFontFace> face_;
    std::wstring file_;
    float ascent_ = 0.92f, line_ = 1.33f;
    UINT16 upem_ = 2048;
    bool embeddable_ = true;
};

struct SheetJob {
    std::wstring output;                 // Zieldatei (.pdf)
    SheetOptions options;
    std::wstring headline;
    struct Image { std::wstring path, caption; };
    std::vector<Image> images;           // in Reihenfolge, nur angehakte
    int quality = 1;                     // 0 klein, 1 mittel, 2 hoch
};

// Erstellt die PDF. progress(0..1) liefert false = abbrechen.
bool WriteContactSheet(const SheetJob& job, const std::function<bool(float)>& progress, std::wstring& error);

// Gemeinsame Texte (Vorschau und PDF)
std::wstring SheetPageLabel(int page, int pages);

} // namespace peek
