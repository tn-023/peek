#pragma once
// Angaben zur Bilderzeugung (KI) aus PNG-Textbloecken: Prompt, Modelle und LoRAs.
// ComfyUI ("prompt": ausgefuehrter Graph als JSON), Automatic1111/Forge ("parameters": Text),
// InvokeAI ("invokeai_metadata": JSON). Plattformunabhaengig (fuer Tests); laeuft im Dekoder-Prozess.
#include <string>
#include <string_view>
#include <vector>

namespace peek {

struct AiMeta {
    std::wstring prompt;
    std::vector<std::wstring> models, loras;   // Dateinamen ohne Ordner und Endung
    bool Empty() const { return prompt.empty() && models.empty() && loras.empty(); }
};

// Ein Textblock (Schluessel und Wert, UTF-8 bzw. Latin-1 bei tEXt). Unbekannte Schluessel werden ignoriert.
void ParseAiText(std::string_view key, std::string_view value, AiMeta& m);

// Kompakte Form fuer Index und Dekoder-Prozess: Prompt, Modelle, LoRAs (getrennt durch Steuerzeichen)
std::wstring EncodeAi(const AiMeta& m);
AiMeta DecodeAi(std::wstring_view s);

// Dateiname eines Modells ohne Ordner und Endung (.safetensors, .ckpt, .pt, .gguf, .bin, .pth)
std::wstring ModelStem(std::wstring_view name);

std::wstring Utf8ToWideCore(std::string_view s);

} // namespace peek
