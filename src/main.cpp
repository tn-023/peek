// peek - schneller, portabler Medienbetrachter fuer Windows 11
#include "app.h"
#include "core/atlaslayout.h"
#include "sandbox.h"

#include <mfapi.h>

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    // DLLs nur aus System32 laden: weder aus dem Arbeitsverzeichnis noch aus dem Ordner von peek.exe
    // (portabel z. B. im Download-Ordner, wo eine untergeschobene DLL liegen koennte). Statische Importe
    // schuetzt zusaetzlich die Linker-Option /DEPENDENTLOADFLAG:0x800. Codecs und Shell-Erweiterungen laedt
    // Windows ueber ihren vollen Pfad, sie sind davon nicht betroffen.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    SetDllDirectoryW(L"");
    HeapSetInformation(nullptr, HeapEnableTerminationOnCorruption, nullptr, 0);

    // Abgeschotteter Dekoder-Prozess (von peek selbst gestartet, siehe sandbox.h): peek.exe --decoder-worker <pipe>
    {
        int wc = 0;
        LPWSTR* wv = CommandLineToArgvW(GetCommandLineW(), &wc);
        if (wv && wc >= 3 && wcscmp(wv[1], L"--decoder-worker") == 0) {
            HANDLE pipe = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(_wcstoui64(wv[2], nullptr, 10)));
            LocalFree(wv);
            return peek::RunDecoderWorker(pipe);
        }
        if (wv) LocalFree(wv);
    }

    if (FAILED(OleInitialize(nullptr))) return 1;
    // MFSTARTUP_LITE: Media Foundation ohne Socket-Bibliothek (keine Netzwerkfunktionen)
    MFStartup(MF_VERSION, MFSTARTUP_LITE);

    int code = 0;
    // Pruefmodus fuer die automatischen Tests (ohne Fenster):
    //   peek.exe --kontaktabzug-test <ziel.pdf> <quer 0|1> <spalten> <zeilen> <bild> [<bild> ...]
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc >= 7 && wcscmp(argv[1], L"--kontaktabzug-test") == 0) {
        peek::SheetJob job;
        job.output = argv[2];
        job.options.landscape = wcscmp(argv[3], L"1") == 0;
        job.options.cols = _wtoi(argv[4]);
        job.options.rows = _wtoi(argv[5]);
        job.headline = L"Kontaktabzug Test \u00c4\u00d6\u00dc \u00df 16.11.2026";   // lang:ok
        for (int i = 6; i < argc; ++i) {
            std::wstring pth = argv[i];
            job.images.push_back({pth, pth.substr(pth.find_last_of(L'\\') + 1)});
        }
        std::wstring err;
        code = peek::WriteContactSheet(job, nullptr, err) ? 0 : 2;
        peek::ShutdownDecoderSandbox();
        LocalFree(argv);
        MFShutdown();
        OleUninitialize();
        return code;
    }
    //   peek.exe --index-test <ordner> <bericht.txt>
    if (argv && argc >= 4 && wcscmp(argv[1], L"--index-test") == 0) {
        code = peek::RunIndexTest(argv[2], argv[3]);
        LocalFree(argv);
        MFShutdown();
        OleUninitialize();
        return code;
    }
    //   peek.exe --sandbox-test <ordner> <bericht.txt>
    if (argv && argc >= 4 && wcscmp(argv[1], L"--sandbox-test") == 0) {
        code = peek::RunSandboxTest(argv[2], argv[3]);
        LocalFree(argv);
        MFShutdown();
        OleUninitialize();
        return code;
    }
    //   peek.exe --kacheltextur-test <ordner> <ziel.png> [<spalten>]   (0 = automatisch)
    if (argv && argc >= 4 && wcscmp(argv[1], L"--kacheltextur-test") == 0) {
        std::vector<std::wstring> files;
        peek::EnumerateFolder(argv[2], false, false, [] { return false; }, [&](std::vector<peek::FileEntry>&& part) {
            for (auto& e : part)
                if (e.kind != peek::Kind::Folder && e.ext == L"png") files.push_back(e.path);
        });
        std::sort(files.begin(), files.end(), [](const std::wstring& a, const std::wstring& b) {
            return StrCmpLogicalW(peek::FileNameOf(a).c_str(), peek::FileNameOf(b).c_str()) < 0;
        });
        peek::AtlasInfo info;
        std::wstring err;
        code = 3;
        if (peek::InspectAtlas(files, info, err)) {
            int n = static_cast<int>(files.size());
            int cols = argc >= 5 ? _wtoi(argv[4]) : 0;
            if (cols <= 0) cols = peek::AtlasAutoCols(n, info.w, info.h);
            code = peek::WriteAtlas(files, info, cols, peek::AtlasRows(n, cols), argv[3], nullptr, err) ? 0 : 2;
        }
        if (!err.empty()) {
            if (HANDLE h = CreateFileW((std::wstring(argv[3]) + L".fehler.txt").c_str(), GENERIC_WRITE, 0, nullptr,   // lang:ok
                                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr); h != INVALID_HANDLE_VALUE) {
                int len = WideCharToMultiByte(CP_UTF8, 0, err.c_str(), -1, nullptr, 0, nullptr, nullptr);
                std::string u(len > 0 ? len - 1 : 0, '\0');
                if (len > 1) WideCharToMultiByte(CP_UTF8, 0, err.c_str(), -1, u.data(), len, nullptr, nullptr);
                DWORD wr = 0;
                WriteFile(h, u.data(), static_cast<DWORD>(u.size()), &wr, nullptr);
                CloseHandle(h);
            }
        }
        LocalFree(argv);
        MFShutdown();
        OleUninitialize();
        return code;
    }
    if (argv) LocalFree(argv);
    {
        peek::App app;
        if (app.Init(inst, show)) code = app.Run();
        else code = 1;
    }

    MFShutdown();
    OleUninitialize();
    return code;
}
