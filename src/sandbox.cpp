#include "sandbox.h"
#include "docimaging.h"
#include "fileinput.h"
#include "core/lang.h"
#include "fs.h"
#include "imaging.h"
#include "svgimaging.h"

#include <aclapi.h>
#include <cmath>
#include <psapi.h>
#include <mfapi.h>
#include <userenv.h>

#include <condition_variable>
#include <random>
#include <thread>

namespace peek {

namespace {

// ---------------------------------------------------------------------------
// Protokoll (beide Seiten sind dieselbe peek.exe, daher feste Strukturen)
constexpr uint32_t kMagic = 0x6B656570;   // "peek"
constexpr uint32_t kMaxPathChars = 32768;
constexpr uint32_t kMaxCameraChars = 256;
constexpr wchar_t kContainerName[] = L"peek.dekoder";

enum class Op : uint32_t { DecodeImage = 1, SvgRegion, VideoThumb, DocThumb, ImageMeta, VideoMeta, DocMeta, ProbeOpen, TakePixels, EmbeddedThumb, PhotoDetails };

struct Request {
    uint32_t magic = kMagic;
    Op op = Op::DecodeImage;
    uint64_t handle = 0;   // im Worker gueltiges Handle (Datei bzw. gemeinsamer Speicher)
    uint32_t edge = 0, w = 0, h = 0;
    float k = 1, tx = 0, ty = 0;
    uint64_t bytes = 0;    // TakePixels: Groesse des Speichers
    uint32_t pathChars = 0;
};

struct Reply {
    uint32_t magic = kMagic;
    uint32_t ok = 0;
    uint32_t w = 0, h = 0, srcW = 0, srcH = 0;
    uint64_t bytes = 0;
    uint32_t mw = 0, mh = 0, mdur = 0;
    int64_t taken = 0;
    uint32_t cameraChars = 0;
    uint32_t geo = 0;
    double lat = 0, lon = 0;
};

struct Params {
    uint32_t edge = 0, w = 0, h = 0;
    float k = 1, tx = 0, ty = 0;
};

// Fuehrt einen Auftrag im aktuellen Prozess aus (im Worker bzw. als letzte Rueckfallebene)
bool RunLocal(Op op, const std::wstring& path, const Params& p, Pixels& px, MetaRow& meta) {
    switch (op) {
    case Op::DecodeImage: return DecodeImageFile(path, p.edge, px);
    case Op::SvgRegion: return RenderSvgRegion(path, p.k, p.tx, p.ty, p.w, p.h, px);
    case Op::VideoThumb: return VideoThumbnail(path, p.edge, px, &meta);
    case Op::DocThumb: return DecodeDocThumb(path, p.edge, px, &meta);
    case Op::ImageMeta: return ReadImageMeta(path, meta);
    case Op::VideoMeta: return ReadVideoMeta(path, meta);
    case Op::DocMeta: return ReadDocMeta(path, meta);
    case Op::EmbeddedThumb: return DecodeEmbeddedThumb(path, p.edge, px);
    case Op::PhotoDetails: return ReadPhotoDetails(path, meta.camera);   // Text im Kamera-Feld zurueck
    default: return false;
    }
}

bool PipeRead(HANDLE h, void* buf, size_t len) {
    auto* p = static_cast<uint8_t*>(buf);
    while (len) {
        DWORD got = 0;
        if (!ReadFile(h, p, static_cast<DWORD>(std::min<size_t>(len, 1 << 20)), &got, nullptr) || !got) return false;
        p += got;
        len -= got;
    }
    return true;
}

bool PipeWrite(HANDLE h, const void* buf, size_t len) {
    auto* p = static_cast<const uint8_t*>(buf);
    while (len) {
        DWORD put = 0;
        if (!WriteFile(h, p, static_cast<DWORD>(std::min<size_t>(len, 1 << 20)), &put, nullptr) || !put) return false;
        p += put;
        len -= put;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Hauptprozess: Worker und Pool

struct Worker {
    HANDLE proc = nullptr;
    HANDLE pipe = nullptr;   // Server-Ende, ueberlappend
    HANDLE ev = nullptr;
    std::wstring lastPath;
    uint32_t warm = 0;   // Bitmaske der schon genutzten Auftragsarten (geladene Dekoder)
    bool busy = false;

    ~Worker() {
        if (proc) {
            TerminateProcess(proc, 1);
            CloseHandle(proc);
        }
        if (pipe) CloseHandle(pipe);
        if (ev) CloseHandle(ev);
    }

    // Lesen/Schreiben mit Zeitgrenze; bricht ab, wenn der Worker endet
    bool Io(bool write, void* buf, size_t len, ULONGLONG deadline) {
        auto* p = static_cast<uint8_t*>(buf);
        while (len) {
            OVERLAPPED ov = {};
            ov.hEvent = ev;
            ResetEvent(ev);
            DWORD n = static_cast<DWORD>(std::min<size_t>(len, 1 << 20));
            BOOL r = write ? WriteFile(pipe, p, n, nullptr, &ov) : ReadFile(pipe, p, n, nullptr, &ov);
            if (!r && GetLastError() != ERROR_IO_PENDING) return false;
            ULONGLONG now = GetTickCount64();
            DWORD wait = now >= deadline ? 0 : static_cast<DWORD>(std::min<ULONGLONG>(deadline - now, INFINITE - 1));
            HANDLE hs[2] = {ev, proc};
            DWORD res = WaitForMultipleObjects(2, hs, FALSE, wait);
            DWORD done = 0;
            if (res != WAIT_OBJECT_0) {
                CancelIoEx(pipe, &ov);
                GetOverlappedResult(pipe, &ov, &done, TRUE);
                return false;
            }
            if (!GetOverlappedResult(pipe, &ov, &done, FALSE) || !done) return false;
            p += done;
            len -= done;
        }
        return true;
    }
};

struct Pool {
    std::mutex m;
    std::condition_variable cv;
    std::vector<std::unique_ptr<Worker>> workers;
    int maxWorkers = 4;
    int spawning = 0;
    std::atomic<SandboxMode> mode{SandboxMode::Pending};
    bool modeFixed = false;
    HANDLE job = nullptr;
    PSID sid = nullptr;   // AppContainer
    bool exeAclTried = false;
    std::thread prewarm;
    bool shutdown = false;
};

Pool& P() {
    static Pool* p = new Pool();   // bewusst nie freigegeben (Reihenfolge beim Beenden)
    return *p;
}

std::wstring ExePath() {
    std::wstring s(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, s.data(), static_cast<DWORD>(s.size()));
        if (n == 0) return {};
        if (n < s.size()) {
            s.resize(n);
            return s;
        }
        s.resize(s.size() * 2);
    }
}

HANDLE MakeJob(int maxProcs) {
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) return nullptr;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
                                          JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    li.BasicLimitInformation.ActiveProcessLimit = static_cast<DWORD>(maxProcs);
    li.ProcessMemoryLimit = 3ull << 30;   // 3 GB je Worker (volle Aufloesung bis 1 GB plus Zwischenpuffer)
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof(li));
    JOBOBJECT_BASIC_UI_RESTRICTIONS ui = {};
    ui.UIRestrictionsClass = JOB_OBJECT_UILIMIT_DESKTOP | JOB_OBJECT_UILIMIT_DISPLAYSETTINGS | JOB_OBJECT_UILIMIT_EXITWINDOWS |
                             JOB_OBJECT_UILIMIT_GLOBALATOMS | JOB_OBJECT_UILIMIT_HANDLES | JOB_OBJECT_UILIMIT_READCLIPBOARD |
                             JOB_OBJECT_UILIMIT_WRITECLIPBOARD | JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS;
    SetInformationJobObject(job, JobObjectBasicUIRestrictions, &ui, sizeof(ui));
    return job;
}

PSID ContainerSid() {
    PSID sid = nullptr;
    HRESULT hr = CreateAppContainerProfile(kContainerName, L"peek Dekoder",
                                           L"Abgeschotteter Dekoder fuer Bild-, Video- und Dokumentdateien", nullptr, 0, &sid);   // lang:ok
    if (hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) hr = DeriveAppContainerSidFromAppContainerName(kContainerName, &sid);
    return SUCCEEDED(hr) ? sid : nullptr;
}

// Der AppContainer darf peek.exe sonst nicht lesen (z. B. im Download-Ordner): Lese- und Ausfuehrrecht nur fuer
// diesen Container eintragen. Auf FAT/exFAT (USB-Stick) gibt es keine Rechte; dort ist nichts noetig.
bool GrantExeToContainer(PSID sid) {
    std::wstring exe = ExePath();
    PACL oldDacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetNamedSecurityInfoW(exe.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &oldDacl, nullptr,
                              &sd) != ERROR_SUCCESS)
        return false;
    EXPLICIT_ACCESS_W ea = {};
    ea.grfAccessPermissions = GENERIC_READ | GENERIC_EXECUTE;
    ea.grfAccessMode = GRANT_ACCESS;
    ea.grfInheritance = NO_INHERITANCE;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea.Trustee.ptstrName = static_cast<LPWSTR>(sid);
    PACL newDacl = nullptr;
    bool ok = SetEntriesInAclW(1, &ea, oldDacl, &newDacl) == ERROR_SUCCESS &&
              SetNamedSecurityInfoW(exe.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, newDacl,
                                    nullptr) == ERROR_SUCCESS;
    if (newDacl) LocalFree(newDacl);
    if (sd) LocalFree(sd);
    return ok;
}

std::wstring RandomPipeName() {
    std::random_device rd;
    wchar_t buf[96];
    swprintf_s(buf, L"\\\\.\\pipe\\peek-dekoder-%lu-%08x%08x", GetCurrentProcessId(), rd(), rd());
    return buf;
}

// Startet einen Worker im gewuenschten Modus. Liefert nullptr bei Fehler (lastError: Grund).
std::unique_ptr<Worker> StartWorker(SandboxMode mode, PSID sid, HANDLE job, DWORD& lastError) {
    lastError = 0;
    std::wstring name = RandomPipeName();
    auto w = std::make_unique<Worker>();
    w->pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                               PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 1 << 16,
                               1 << 16, 0, nullptr);
    if (w->pipe == INVALID_HANDLE_VALUE) {
        w->pipe = nullptr;
        lastError = GetLastError();
        return nullptr;
    }
    w->ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE client = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, &sa, OPEN_EXISTING, 0, nullptr);
    if (client == INVALID_HANDLE_VALUE || !w->ev) {
        lastError = GetLastError();
        if (client != INVALID_HANDLE_VALUE) CloseHandle(client);
        return nullptr;
    }
    {
        OVERLAPPED ov = {};
        ov.hEvent = w->ev;
        if (!ConnectNamedPipe(w->pipe, &ov)) {
            DWORD e = GetLastError();
            if (e == ERROR_IO_PENDING) {
                DWORD n = 0;
                GetOverlappedResult(w->pipe, &ov, &n, TRUE);
            } else if (e != ERROR_PIPE_CONNECTED) {
                lastError = e;
                CloseHandle(client);
                return nullptr;
            }
        }
    }

    // Prozessattribute: nur das Pipe-Handle erben, Sandbox, Abschwaechungen, keine Kindprozesse
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 4, 0, &attrSize);
    std::vector<uint8_t> attrBuf(attrSize);
    auto attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    if (!InitializeProcThreadAttributeList(attrs, 4, 0, &attrSize)) {
        lastError = GetLastError();
        CloseHandle(client);
        return nullptr;
    }
    HANDLE inherit[1] = {client};
    UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), nullptr, nullptr);
    SECURITY_CAPABILITIES caps = {};
    if (mode == SandboxMode::AppContainer) {
        caps.AppContainerSid = sid;   // keine Faehigkeiten: kein Netzwerk, keine Bibliotheken des Benutzers
        UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES, &caps, sizeof(caps), nullptr, nullptr);
    }
    DWORD64 mitigation = PROCESS_CREATION_MITIGATION_POLICY_STRICT_HANDLE_CHECKS_ALWAYS_ON |
                         PROCESS_CREATION_MITIGATION_POLICY_EXTENSION_POINT_DISABLE_ALWAYS_ON |
                         PROCESS_CREATION_MITIGATION_POLICY_IMAGE_LOAD_NO_REMOTE_ALWAYS_ON |
                         PROCESS_CREATION_MITIGATION_POLICY_IMAGE_LOAD_NO_LOW_LABEL_ALWAYS_ON |
                         PROCESS_CREATION_MITIGATION_POLICY_HEAP_TERMINATE_ALWAYS_ON |
                         PROCESS_CREATION_MITIGATION_POLICY_BOTTOM_UP_ASLR_ALWAYS_ON |
                         PROCESS_CREATION_MITIGATION_POLICY_HIGH_ENTROPY_ASLR_ALWAYS_ON;
    UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY, &mitigation, sizeof(mitigation), nullptr, nullptr);
    DWORD childPolicy = PROCESS_CREATION_CHILD_PROCESS_RESTRICTED;
    UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY, &childPolicy, sizeof(childPolicy), nullptr,
                              nullptr);

    STARTUPINFOEXW si = {};
    si.StartupInfo.cb = sizeof(si);
    // Ohne dieses Flag zeigt Windows den Cursor "Wird gestartet" (blauer Kreis), bis der neue Prozess ein Fenster
    // oeffnet - der Worker oeffnet nie eines, also stuende der Kreis jedes Mal einige Sekunden am Mauszeiger.
    si.StartupInfo.dwFlags = STARTF_FORCEOFFFEEDBACK;
    si.lpAttributeList = attrs;
    PROCESS_INFORMATION pi = {};
    std::wstring exe = ExePath();
    std::wstring cmd = L"\"" + exe + L"\" --decoder-worker " + std::to_wstring(reinterpret_cast<uintptr_t>(client));
    wchar_t sysDir[MAX_PATH] = {};
    GetSystemDirectoryW(sysDir, MAX_PATH);   // Arbeitsverzeichnis, das jeder Container lesen darf
    const DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_NO_WINDOW;
    BOOL created = FALSE;
    if (mode == SandboxMode::LowIntegrity) {
        HANDLE tok = nullptr, low = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ADJUST_DEFAULT | TOKEN_ASSIGN_PRIMARY,
                             &tok) &&
            DuplicateTokenEx(tok, 0, nullptr, SecurityImpersonation, TokenPrimary, &low)) {
            SID_IDENTIFIER_AUTHORITY auth = SECURITY_MANDATORY_LABEL_AUTHORITY;
            PSID lowSid = nullptr;
            if (AllocateAndInitializeSid(&auth, 1, SECURITY_MANDATORY_LOW_RID, 0, 0, 0, 0, 0, 0, 0, &lowSid)) {
                TOKEN_MANDATORY_LABEL tml = {};
                tml.Label.Attributes = SE_GROUP_INTEGRITY;
                tml.Label.Sid = lowSid;
                if (SetTokenInformation(low, TokenIntegrityLevel, &tml, sizeof(tml) + GetLengthSid(lowSid)))
                    created = CreateProcessAsUserW(low, exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, flags, nullptr, sysDir,
                                                   &si.StartupInfo, &pi);
                FreeSid(lowSid);
            }
        }
        if (!created) lastError = GetLastError();
        if (low) CloseHandle(low);
        if (tok) CloseHandle(tok);
    } else {
        created = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, flags, nullptr, sysDir, &si.StartupInfo, &pi);
        if (!created) lastError = GetLastError();
    }
    DeleteProcThreadAttributeList(attrs);
    CloseHandle(client);   // gehoert jetzt dem Worker
    if (!created) return nullptr;
    if (job && !AssignProcessToJobObject(job, pi.hProcess)) {
        lastError = GetLastError();
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return nullptr;
    }
    w->proc = pi.hProcess;
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    // Begruessung abwarten: der Worker meldet sich, wenn er bereit ist
    Reply hello;
    if (!w->Io(false, &hello, sizeof(hello), GetTickCount64() + 10000) || hello.magic != kMagic || !hello.ok) {
        lastError = ERROR_INVALID_DATA;
        return nullptr;
    }
    return w;
}

// Nur fuer die Pruefung der Rueckfallebenen in der CI: PEEK_SANDBOX=low bzw. off. Wirkt ausschliesslich im
// Pruefmodus (--sandbox-test); im normalen Betrieb laesst sich die Sandbox so nicht abschalten.
std::atomic<bool> g_testMode{false};

SandboxMode ForcedMode() {
    if (!g_testMode) return SandboxMode::Pending;
    wchar_t v[16] = {};
    DWORD n = GetEnvironmentVariableW(L"PEEK_SANDBOX", v, 16);
    if (n && n < 16) {
        if (_wcsicmp(v, L"low") == 0) return SandboxMode::LowIntegrity;
        if (_wcsicmp(v, L"off") == 0) return SandboxMode::InProcess;
    }
    return SandboxMode::Pending;
}

// Ersten Worker starten und dabei den Modus bestimmen: AppContainer, sonst Low Integrity, sonst im Prozess
std::unique_ptr<Worker> Spawn() {
    Pool& p = P();
    DWORD err = 0;
    if (!p.job) p.job = MakeJob(p.maxWorkers + 1);
    SandboxMode mode = p.mode.load();
    if (mode == SandboxMode::Pending && ForcedMode() != SandboxMode::Pending) {
        p.mode = ForcedMode();
        if (p.mode == SandboxMode::InProcess) return nullptr;
        mode = p.mode;
    }
    if (mode == SandboxMode::Pending || mode == SandboxMode::AppContainer) {
        if (!p.sid) p.sid = ContainerSid();
        if (p.sid) {
            auto w = StartWorker(SandboxMode::AppContainer, p.sid, p.job, err);
            if (!w && !p.exeAclTried) {
                p.exeAclTried = true;
                if (GrantExeToContainer(p.sid)) w = StartWorker(SandboxMode::AppContainer, p.sid, p.job, err);
            }
            if (w) {
                p.mode = SandboxMode::AppContainer;
                return w;
            }
        }
        if (mode == SandboxMode::AppContainer) return nullptr;   // ging schon einmal: diesmal nur Pech
    }
    if (mode == SandboxMode::Pending || mode == SandboxMode::LowIntegrity) {
        auto w = StartWorker(SandboxMode::LowIntegrity, nullptr, p.job, err);
        if (w) {
            p.mode = SandboxMode::LowIntegrity;
            return w;
        }
        if (mode == SandboxMode::LowIntegrity) return nullptr;
    }
    p.mode = SandboxMode::InProcess;
    return nullptr;
}

// Freien Worker waehlen: gleiche Datei (SVG-Zwischenspeicher) vor gleicher Auftragsart (Dekoder schon geladen,
// z. B. Media Foundation oder PDF) vor irgendeinem
Worker* Acquire(const std::wstring& path, uint32_t kind = 0) {
    Pool& p = P();
    std::unique_lock<std::mutex> lock(p.m);
    for (;;) {
        if (p.shutdown || p.mode == SandboxMode::InProcess) return nullptr;
        Worker* any = nullptr;
        int best = -1;
        for (auto& w : p.workers) {
            if (w->busy) continue;
            int score = (!path.empty() && w->lastPath == path) ? 2 : ((w->warm & kind) ? 1 : 0);
            if (score > best) {
                best = score;
                any = w.get();
            }
        }
        if (any) {
            any->busy = true;
            return any;
        }
        bool first = p.mode == SandboxMode::Pending;
        if (static_cast<int>(p.workers.size()) + p.spawning < p.maxWorkers && (!first || p.spawning == 0)) {
            ++p.spawning;
            lock.unlock();
            std::unique_ptr<Worker> w = Spawn();
            lock.lock();
            --p.spawning;
            p.cv.notify_all();
            if (!w) return nullptr;
            w->busy = true;
            p.workers.push_back(std::move(w));
            return p.workers.back().get();
        }
        p.cv.wait(lock);
    }
}

void Release(Worker* w, bool broken) {
    Pool& p = P();
    std::lock_guard<std::mutex> lock(p.m);
    if (broken) {
        for (auto it = p.workers.begin(); it != p.workers.end(); ++it)
            if (it->get() == w) {
                p.workers.erase(it);   // beendet den Prozess
                break;
            }
    } else {
        w->busy = false;
    }
    p.cv.notify_all();
}

DWORD TimeoutFor(Op op, uint32_t edge) {
    switch (op) {
    case Op::ImageMeta: case Op::VideoMeta: case Op::DocMeta: case Op::ProbeOpen: case Op::EmbeddedThumb: case Op::PhotoDetails: return 15000;
    case Op::DecodeImage: return edge >= 4096 || edge == 0 ? 60000 : 20000;
    case Op::VideoThumb: return 30000;
    case Op::DocThumb: return 40000;
    default: return 30000;
    }
}

// Ein Auftrag ueber einen Worker. Bei jedem Protokollfehler wird der Worker verworfen.
bool Remote(Worker* w, Op op, const std::wstring& path, const Params& prm, Pixels* px, MetaRow* meta, bool& broken) {
    broken = true;
    ULONGLONG deadline = GetTickCount64() + TimeoutFor(op, prm.edge);
    Request rq;
    rq.op = op;
    rq.edge = prm.edge;
    rq.w = prm.w;
    rq.h = prm.h;
    rq.k = prm.k;
    rq.tx = prm.tx;
    rq.ty = prm.ty;
    rq.pathChars = static_cast<uint32_t>(std::min<size_t>(path.size(), kMaxPathChars));
    if (op != Op::ProbeOpen) {
        // Die Datei oeffnet der Hauptprozess; der Worker erhaelt nur ein Lese-Handle
        if (!IsLocalDrivePath(path)) return broken = false, false;
        HANDLE f = CreateFileW(LongPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return broken = false, false;
        HANDLE remote = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), f, w->proc, &remote, 0, FALSE, DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE))
            return false;
        rq.handle = reinterpret_cast<uint64_t>(remote);
    }
    if (!w->Io(true, &rq, sizeof(rq), deadline) ||
        !w->Io(true, const_cast<wchar_t*>(path.data()), rq.pathChars * sizeof(wchar_t), deadline))
        return false;
    Reply rp;
    if (!w->Io(false, &rp, sizeof(rp), deadline) || rp.magic != kMagic || rp.cameraChars > kMaxCameraChars) return false;
    std::wstring camera(rp.cameraChars, L'\0');
    if (rp.cameraChars && !w->Io(false, camera.data(), rp.cameraChars * sizeof(wchar_t), deadline)) return false;
    if (meta) {
        meta->width = rp.mw;
        meta->height = rp.mh;
        meta->durationMs = rp.mdur;
        meta->taken = rp.taken;
        meta->camera = std::move(camera);
        // Worker nicht blind vertrauen: nur gueltige Koordinaten
        meta->hasGeo = rp.geo == 1 && std::isfinite(rp.lat) && std::isfinite(rp.lon) && std::fabs(rp.lat) <= 90 &&
                       std::fabs(rp.lon) <= 180;
        meta->lat = meta->hasGeo ? rp.lat : 0;
        meta->lon = meta->hasGeo ? rp.lon : 0;
    }
    if (rp.ok && rp.bytes) {
        // Antwort des Workers nicht blind vertrauen: Groessen pruefen
        if (!px || !rp.w || !rp.h || static_cast<uint64_t>(rp.w) * rp.h > kMaxDecodePixels ||
            rp.bytes != static_cast<uint64_t>(rp.w) * rp.h * 4)
            return false;
        HANDLE sec = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, static_cast<DWORD>(rp.bytes >> 32),
                                        static_cast<DWORD>(rp.bytes), nullptr);
        if (!sec) return false;
        HANDLE remoteSec = nullptr;
        bool ok = DuplicateHandle(GetCurrentProcess(), sec, w->proc, &remoteSec, FILE_MAP_WRITE, FALSE, 0);
        Request take;
        take.op = Op::TakePixels;
        take.handle = reinterpret_cast<uint64_t>(remoteSec);
        take.bytes = rp.bytes;
        Reply ack;
        ok = ok && w->Io(true, &take, sizeof(take), deadline) && w->Io(false, &ack, sizeof(ack), deadline) &&
             ack.magic == kMagic && ack.ok;
        if (ok) {
            const void* view = MapViewOfFile(sec, FILE_MAP_READ, 0, 0, static_cast<SIZE_T>(rp.bytes));
            ok = view != nullptr;
            if (ok) {
                px->w = rp.w;
                px->h = rp.h;
                px->srcW = rp.srcW;
                px->srcH = rp.srcH;
                px->data.resize(static_cast<size_t>(rp.bytes));
                memcpy(px->data.data(), view, static_cast<size_t>(rp.bytes));
                UnmapViewOfFile(view);
            }
        }
        CloseHandle(sec);
        if (!ok) return false;
    } else if (rp.ok && px && op != Op::ImageMeta && op != Op::VideoMeta && op != Op::DocMeta && op != Op::ProbeOpen) {
        return false;   // Bild erwartet, aber keine Pixel
    }
    broken = false;
    return rp.ok != 0;
}

uint32_t KindBit(Op op, const std::wstring& path) {
    switch (op) {
    case Op::VideoThumb: case Op::VideoMeta: return 1;   // Media Foundation
    case Op::DocThumb: case Op::DocMeta: return 2;       // PDF / Photoshop
    case Op::SvgRegion: return 4;
    default: return IsSvgPath(path) ? 4 : 8;            // resvg bzw. WIC
    }
}

bool Run(Op op, const std::wstring& path, const Params& prm, Pixels* px, MetaRow* meta) {
    uint32_t kind = KindBit(op, path);
    Worker* w = Acquire(path, kind);
    if (!w) {
        if (P().mode != SandboxMode::InProcess || P().shutdown) return false;
        // Rueckfallebene: im eigenen Prozess (wie vor der Sandbox)
        Pixels lp;
        MetaRow lm;
        bool ok = GuardedCall([&] { return RunLocal(op, path, prm, lp, lm); });
        if (px) *px = std::move(lp);
        if (meta) *meta = std::move(lm);
        return ok;
    }
    bool broken = true;
    bool ok = Remote(w, op, path, prm, px, meta, broken);
    if (!broken) {
        w->lastPath = path;
        w->warm |= kind;
    }
    Release(w, broken);
    return ok;
}

}  // namespace

// ---------------------------------------------------------------------------
// Oeffentliche Schnittstelle (Hauptprozess)

void InitDecoderSandbox(int maxWorkers) { P().maxWorkers = std::clamp(maxWorkers, 1, 12); }

void PrewarmDecoderSandbox() {
    Pool& p = P();
    if (p.prewarm.joinable()) return;
    p.prewarm = std::thread([] {
        if (Worker* w = Acquire(std::wstring())) Release(w, false);
    });
}

void ShutdownDecoderSandbox() {
    Pool& p = P();
    if (p.prewarm.joinable()) p.prewarm.join();
    SandboxMode mode;
    {
        std::lock_guard<std::mutex> lock(p.m);
        p.shutdown = true;
        p.workers.clear();   // beendet alle Worker
        mode = p.mode;
    }
    p.cv.notify_all();
    if (p.job) {
        CloseHandle(p.job);
        p.job = nullptr;
    }
    if (p.sid) {
        FreeSid(p.sid);
        p.sid = nullptr;
    }
    // Profil (Registry-Eintrag und leerer Ordner unter %LOCALAPPDATA%\Packages) wieder entfernen
    if (mode == SandboxMode::AppContainer) DeleteAppContainerProfile(kContainerName);
}

SandboxMode DecoderSandboxMode() { return P().mode; }

const wchar_t* DecoderSandboxText() {
    switch (P().mode.load()) {
    case SandboxMode::AppContainer: return T(L"Dekoder abgeschottet (AppContainer, ohne Netzwerk- und Dateizugriff)");
    case SandboxMode::LowIntegrity: return T(L"Dekoder abgeschottet (niedrige Integritätsstufe)");
    case SandboxMode::InProcess: return T(L"Dekoder ohne Abschottung (Sandbox auf diesem PC nicht verfügbar)");
    default: return T(L"Dekoder-Sandbox wird gestartet …");
    }
}

bool SbDecodeImage(const std::wstring& path, uint32_t maxEdge, Pixels& out) {
    Params p;
    p.edge = maxEdge;
    return Run(Op::DecodeImage, path, p, &out, nullptr);
}

bool SbPhotoDetails(const std::wstring& path, std::wstring& out) {
    MetaRow m;
    bool ok = Run(Op::PhotoDetails, path, Params(), nullptr, &m);
    out = ok ? std::move(m.camera) : std::wstring();
    return ok;
}

bool SbEmbeddedThumb(const std::wstring& path, uint32_t maxEdge, Pixels& out) {
    Params p;
    p.edge = maxEdge;
    return Run(Op::EmbeddedThumb, path, p, &out, nullptr);
}

bool SbSvgRegion(const std::wstring& path, float k, float tx, float ty, uint32_t w, uint32_t h, Pixels& out) {
    Params p;
    p.k = k;
    p.tx = tx;
    p.ty = ty;
    p.w = w;
    p.h = h;
    return Run(Op::SvgRegion, path, p, &out, nullptr);
}

bool SbVideoThumbnail(const std::wstring& path, uint32_t maxEdge, Pixels& out, MetaRow* meta) {
    Params p;
    p.edge = maxEdge;
    MetaRow m;
    bool ok = Run(Op::VideoThumb, path, p, &out, &m);
    if (meta) *meta = m;
    return ok;
}

bool SbDocThumb(const std::wstring& path, uint32_t edge, Pixels& out, MetaRow* meta) {
    Params p;
    p.edge = edge;
    MetaRow m;
    bool ok = Run(Op::DocThumb, path, p, &out, &m);
    if (meta) *meta = m;
    return ok;
}

bool SbImageMeta(const std::wstring& path, MetaRow& m) { return Run(Op::ImageMeta, path, Params(), nullptr, &m); }
bool SbVideoMeta(const std::wstring& path, MetaRow& m) { return Run(Op::VideoMeta, path, Params(), nullptr, &m); }
bool SbDocMeta(const std::wstring& path, MetaRow& m) { return Run(Op::DocMeta, path, Params(), nullptr, &m); }

// ---------------------------------------------------------------------------
// Worker-Prozess

int RunDecoderWorker(HANDLE pipe) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    SetInputSandboxed(true);
    ComScope com(COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    Reply hello;
    hello.ok = 1;
    if (!PipeWrite(pipe, &hello, sizeof(hello))) return 1;
    for (;;) {
        Request rq;
        if (!PipeRead(pipe, &rq, sizeof(rq)) || rq.magic != kMagic || rq.pathChars > kMaxPathChars) break;
        std::wstring path(rq.pathChars, L'\0');
        if (rq.pathChars && !PipeRead(pipe, path.data(), rq.pathChars * sizeof(wchar_t))) break;
        HANDLE file = reinterpret_cast<HANDLE>(rq.handle);
        Reply rp;
        Pixels px;
        MetaRow meta;
        if (rq.op == Op::ProbeOpen) {
            // Pruefung der Sandbox: ein Oeffnen ueber den Pfad muss scheitern
            HANDLE h = CreateFileW(LongPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, 0, nullptr);
            rp.ok = h != INVALID_HANDLE_VALUE;
            if (rp.ok) CloseHandle(h);
        } else {
            Params prm;
            prm.edge = rq.edge;
            prm.w = rq.w;
            prm.h = rq.h;
            prm.k = rq.k;
            prm.tx = rq.tx;
            prm.ty = rq.ty;
            {
                ScopedInput in(path, file);
                rp.ok = GuardedCall([&] { return RunLocal(rq.op, path, prm, px, meta); }) ? 1 : 0;
            }
            if (file) CloseHandle(file);
        }
        if (rp.ok && px.Valid()) {
            rp.w = px.w;
            rp.h = px.h;
            rp.srcW = px.srcW;
            rp.srcH = px.srcH;
            rp.bytes = static_cast<uint64_t>(px.w) * px.h * 4;
        }
        rp.mw = meta.width;
        rp.mh = meta.height;
        rp.mdur = meta.durationMs;
        rp.taken = meta.taken;
        rp.geo = meta.hasGeo ? 1 : 0;
        rp.lat = meta.lat;
        rp.lon = meta.lon;
        if (meta.camera.size() > kMaxCameraChars) meta.camera.resize(kMaxCameraChars);
        rp.cameraChars = static_cast<uint32_t>(meta.camera.size());
        if (!PipeWrite(pipe, &rp, sizeof(rp)) ||
            (rp.cameraChars && !PipeWrite(pipe, meta.camera.data(), rp.cameraChars * sizeof(wchar_t))))
            break;
        if (rp.bytes) {
            Request take;
            if (!PipeRead(pipe, &take, sizeof(take)) || take.magic != kMagic || take.op != Op::TakePixels ||
                take.bytes != rp.bytes)
                break;
            HANDLE sec = reinterpret_cast<HANDLE>(take.handle);
            Reply ack;
            if (void* view = MapViewOfFile(sec, FILE_MAP_WRITE, 0, 0, static_cast<SIZE_T>(rp.bytes))) {
                memcpy(view, px.data.data(), static_cast<size_t>(rp.bytes));
                UnmapViewOfFile(view);
                ack.ok = 1;
            }
            CloseHandle(sec);
            if (!PipeWrite(pipe, &ack, sizeof(ack))) break;
        }
    }
    MFShutdown();
    return 0;
}

// ---------------------------------------------------------------------------
// Pruefmodus (CI)

int RunSandboxTest(const std::wstring& folder, const std::wstring& report) {
    std::wstring out;
    int failures = 0;
    g_testMode = true;
    InitDecoderSandbox(2);
    std::vector<FileEntry> files;
    EnumerateFolder(folder, false, false, [] { return false; }, [&](std::vector<FileEntry>&& part) {
        for (auto& e : part)
            if (e.kind != Kind::Folder) files.push_back(std::move(e));
    });
    std::sort(files.begin(), files.end(), [](const FileEntry& a, const FileEntry& b) { return a.name < b.name; });

    // 1) Sandbox starten und Modus pruefen
    if (Worker* w = Acquire(std::wstring())) Release(w, false);
    SandboxMode mode = DecoderSandboxMode();
    out += std::wstring(L"Modus: ") + DecoderSandboxText() + L"\n";
    SandboxMode expected = ForcedMode() == SandboxMode::Pending ? SandboxMode::AppContainer : ForcedMode();
    if (mode != expected) {
        out += L"FEHLER: anderer Modus erwartet\n";   // lang:ok
        ++failures;
    }

    // 2) Der Worker darf keine Datei ueber ihren Pfad oeffnen (Benutzerordner und die Testdatei selbst)
    if (!files.empty()) {
        Worker* w = Acquire(files[0].path);
        if (w) {
            bool broken = true;
            bool opened = Remote(w, Op::ProbeOpen, files[0].path, Params(), nullptr, nullptr, broken);
            Release(w, broken);
            // Low Integrity darf lesen (nur nichts veraendern); im AppContainer muss es scheitern
            bool bad = opened && mode == SandboxMode::AppContainer;
            out += std::wstring(L"Pfadzugriff im Worker: ") +
                   (opened ? (bad ? L"M\u00d6GLICH (Fehler)" : L"m\u00f6glich (Low Integrity darf lesen)") : L"verweigert") + L"\n";   // lang:ok
            if (bad) ++failures;
        }
    }

    double sumA = 0, sumB = 0, sumCold = 0;
    // 3) Jede Datei abgeschottet und im Prozess dekodieren und vergleichen (im Prozess wie im Betrieb in einem
    //    eigenen Hintergrund-Thread, nicht im Hauptthread)
    for (auto& e : files) {
        if (e.kind != Kind::Video && e.kind != Kind::Image && !e.Doc()) continue;
        Pixels a, b;
        MetaRow ma, mb;
        bool okA = false, okB = false;
        double t0 = NowMs();
        if (e.kind == Kind::Video) okA = SbVideoThumbnail(e.path, 320, a, &ma);
        else if (e.Doc()) okA = SbDocThumb(e.path, 320, a, &ma);
        else okA = SbDecodeImage(e.path, 320, a) && SbImageMeta(e.path, ma);
        double t1 = NowMs();
        double tB = 0;
        std::thread([&] {
            ComScope com;
            MFStartup(MF_VERSION, MFSTARTUP_LITE);
            // einmal zum Aufwaermen (Codecs laden), dann messen - wie der Worker, der schon laeuft
            {
                Pixels w;
                MetaRow wm;
                if (e.kind == Kind::Video) GuardedCall([&] { return VideoThumbnail(e.path, 320, w, &wm); });
                else if (e.Doc()) GuardedCall([&] { return DecodeDocThumb(e.path, 320, w, &wm); });
                else GuardedCall([&] { return DecodeImageFile(e.path, 320, w); });
            }
            double s0 = NowMs();
            if (e.kind == Kind::Video)
                okB = GuardedCall([&] { return VideoThumbnail(e.path, 320, b, &mb); });
            else if (e.Doc())
                okB = GuardedCall([&] { return DecodeDocThumb(e.path, 320, b, &mb); });
            else
                okB = GuardedCall([&] { return DecodeImageFile(e.path, 320, b) && ReadImageMeta(e.path, mb); });
            tB = NowMs() - s0;
            MFShutdown();
        }).join();
        // zweiter Durchlauf abgeschottet (Worker warm, gleiche Datei geht an denselben Worker)
        double tCold = t1 - t0;
        double r0 = NowMs();
        {
            Pixels a2;
            MetaRow m2;
            if (e.kind == Kind::Video) SbVideoThumbnail(e.path, 320, a2, &m2);
            else if (e.Doc()) SbDocThumb(e.path, 320, a2, &m2);
            else { SbDecodeImage(e.path, 320, a2); SbImageMeta(e.path, m2); }
        }
        double tA = NowMs() - r0;
        sumA += tA;
        sumCold += tCold;
        sumB += tB;
        bool same = okA == okB && (!okA || (a.w == b.w && a.h == b.h && ma.width == mb.width && ma.height == mb.height &&
                                            ma.taken == mb.taken && ma.camera == mb.camera && ma.durationMs == mb.durationMs));
        wchar_t line[512];
        swprintf_s(line, L"%-8s %-26s Sandbox %-4s %4ux%-4u kalt %7.1f ms warm %6.1f ms   im Prozess %-4s %4ux%-4u warm %6.1f ms\n",
                   same ? L"OK" : L"FEHLER", e.name.c_str(), okA ? L"ja" : L"nein", a.w, a.h, tCold, tA,   // lang:ok
                   okB ? L"ja" : L"nein", b.w, b.h, tB);
        out += line;
        if (!same) ++failures;
    }
 {
        wchar_t line[256];
        swprintf_s(line, L"Summe warm: Sandbox %.1f ms, im Prozess %.1f ms (Sandbox kalt %.1f ms)\n", sumA, sumB, sumCold);
        out += line;
        // Speicher der Worker
        Pool& p = P();
        std::lock_guard<std::mutex> lock(p.m);
        for (auto& w : p.workers) {
            PROCESS_MEMORY_COUNTERS_EX pmc = {};
            pmc.cb = sizeof(pmc);
            if (GetProcessMemoryInfo(w->proc, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
                swprintf_s(line, L"Worker: Arbeitsspeicher %.1f MB, privat %.1f MB\n", pmc.WorkingSetSize / 1048576.0,
                           pmc.PrivateUsage / 1048576.0);
                out += line;
            }
        }
    }
    out += failures ? L"Ergebnis: " + std::to_wstring(failures) + L" Fehler\n" : L"Ergebnis: alles in Ordnung\n";   // lang:ok
    ShutdownDecoderSandbox();

    std::string u = WideToUtf8(out);
    HANDLE h = CreateFileW(report.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        WriteFile(h, u.data(), static_cast<DWORD>(u.size()), &wr, nullptr);
        CloseHandle(h);
    }
    return failures ? 2 : 0;
}

} // namespace peek
