#include "fs.h"
#include "core/textutil.h"

#include <cwctype>

namespace peek {

std::wstring Utf8ToWide(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string WideToUtf8(std::wstring_view w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

int64_t FileTimeToUnix(const FILETIME& ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    if (u.QuadPart < 116444736000000000ull) return 0;
    return static_cast<int64_t>((u.QuadPart - 116444736000000000ull) / 10000000ull);
}

int64_t NowUnix() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return FileTimeToUnix(ft);
}

Kind ClassifyExt(std::wstring_view e) {
    if (IsViewerFormat(e)) return Kind::Image;
    static const wchar_t* kVideo[] = {L"mp4", L"m4v", L"mov", L"avi", L"mkv", L"wmv", L"webm", L"mpg",
                                      L"mpeg", L"ts", L"mts", L"m2ts", L"3gp", L"3g2", L"flv", L"asf", L"vob"};
    for (auto v : kVideo)
        if (e == v) return Kind::Video;
    return Kind::Other;
}

bool IsViewerFormat(std::wstring_view e) {
    static const wchar_t* kImg[] = {L"jpg", L"jpeg", L"jpe", L"jfif", L"png", L"gif", L"bmp",
                                    L"dib", L"webp", L"avif", L"tif", L"tiff", L"ico", L"svg", L"tga"};
    for (auto v : kImg)
        if (e == v) return true;
    return false;
}

bool IsLocalDrivePath(const std::wstring& path) {
    if (path.size() < 2) return false;
    if (path[0] == L'\\' && path[1] == L'\\') {
        // \\?\C:\... ist lokal erlaubt, \\server\... und \\?\UNC\... nicht
        if (path.size() >= 7 && path.compare(0, 4, L"\\\\?\\") == 0 && path[5] == L':')
            return IsLocalDrivePath(path.substr(4));
        return false;
    }
    if (path[1] != L':') return false;
    wchar_t root[4] = {path[0], L':', L'\\', 0};
    UINT t = GetDriveTypeW(root);
    return t == DRIVE_FIXED || t == DRIVE_REMOVABLE;
}

std::vector<std::wstring> LocalDrives() {
    std::vector<std::wstring> out;
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        wchar_t root[4] = {static_cast<wchar_t>(L'A' + i), L':', L'\\', 0};
        UINT t = GetDriveTypeW(root);
        if (t == DRIVE_FIXED || t == DRIVE_REMOVABLE) out.emplace_back(root);
    }
    return out;
}

std::wstring KnownFolderPath(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &p)) && p) r = p;
    if (p) CoTaskMemFree(p);
    return r;
}

std::wstring ParentPath(const std::wstring& path) {
    std::wstring p = path;
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    size_t pos = p.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return {};
    if (pos == 2 && p[1] == L':') return p.substr(0, 3);  // Laufwerkswurzel
    if (pos < 2) return {};
    return p.substr(0, pos);
}

std::wstring FileNameOf(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

std::wstring JoinPath(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    if (dir.back() == L'\\') return dir + name;
    return dir + L"\\" + name;
}

std::wstring LongPath(const std::wstring& path) {
    if (path.size() < 240 || path.compare(0, 4, L"\\\\?\\") == 0) return path;
    return L"\\\\?\\" + path;
}

bool DirectoryExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(LongPath(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring NormalizeDir(std::wstring p) {
    for (auto& c : p) if (c == L'/') c = L'\\';
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    if (p.size() == 2 && p[1] == L':') p += L'\\';
    if (p.size() >= 1) p[0] = static_cast<wchar_t>(towupper(p[0]));
    return p;
}

FileEntry MakeEntry(const std::wstring& dir, const WIN32_FIND_DATAW& fd) {
    FileEntry e;
    e.name = fd.cFileName;
    e.path = JoinPath(dir, e.name);
    e.attrs = fd.dwFileAttributes;
    e.mtime = FileTimeToUnix(fd.ftLastWriteTime);
    e.ctime = FileTimeToUnix(fd.ftCreationTime);
    e.offline = (fd.dwFileAttributes & (FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN |
                                        FILE_ATTRIBUTE_OFFLINE)) != 0;
    e.nameLower = FoldForMatch(ToLower(e.name));
    e.folderLower = FoldForMatch(ToLower(dir));
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        e.kind = Kind::Folder;
    } else {
        e.size = (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        size_t dot = e.name.find_last_of(L'.');
        if (dot != std::wstring::npos && dot + 1 < e.name.size()) e.ext = ToLower(e.name.substr(dot + 1));
        e.kind = ClassifyExt(e.ext);
    }
    return e;
}

void EnumerateFolder(const std::wstring& root, bool recursive, bool showHidden,
                     const std::function<bool()>& cancel,
                     const std::function<void(std::vector<FileEntry>&&)>& sink, size_t batch) {
    std::vector<std::wstring> stack{root};
    std::vector<FileEntry> out;
    out.reserve(std::min<size_t>(batch, 4096));
    while (!stack.empty()) {
        if (cancel && cancel()) return;
        std::wstring dir = std::move(stack.back());
        stack.pop_back();
        if (!IsLocalDrivePath(dir)) continue;

        WIN32_FIND_DATAW fd;
        std::wstring pattern = LongPath(JoinPath(dir, L"*"));
        HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                    FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) continue;
        std::vector<std::wstring> subdirs;
        do {
            if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == 0 || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0)))
                continue;
            if (!showHidden && (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) continue;
            bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (isDir) {
                if (recursive) {
                    // Verbindungspunkte/Links nicht verfolgen (Schleifen, fremde Ziele)
                    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                        subdirs.push_back(JoinPath(dir, fd.cFileName));
                    continue;
                }
            }
            out.push_back(MakeEntry(dir, fd));
            if (out.size() >= batch) {
                sink(std::move(out));
                out = {};
                out.reserve(batch);
                if (cancel && cancel()) { FindClose(h); return; }
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        // in umgekehrter Reihenfolge auf den Stapel, damit alphabetisch abgearbeitet wird
        for (auto it = subdirs.rbegin(); it != subdirs.rend(); ++it) stack.push_back(std::move(*it));
    }
    if (!out.empty()) sink(std::move(out));
}

std::wstring ExeDir() {
    std::wstring buf(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    buf.resize(n);
    return ParentPath(buf);
}

static std::wstring g_dataDir;

void OverrideDataDirForTest(const std::wstring& dir) { g_dataDir = dir; }

std::wstring DataDir() {
    std::wstring& cached = g_dataDir;
    if (!cached.empty()) return cached;
    std::wstring exe = ExeDir();
    std::wstring marker = JoinPath(exe, L"peek.portable");
    if (GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::wstring d = JoinPath(exe, L"peek-data");
        CreateDirectoryW(d.c_str(), nullptr);
        if (DirectoryExists(d)) {
            // Schreibtest
            std::wstring probe = JoinPath(d, L".probe");
            HANDLE f = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                CloseHandle(f);
                cached = d;
                return cached;
            }
        }
    }
    std::wstring local = KnownFolderPath(FOLDERID_LocalAppData);
    std::wstring d = JoinPath(local, L"peek");
    CreateDirectoryW(d.c_str(), nullptr);
    cached = d;
    return cached;
}

} // namespace peek
