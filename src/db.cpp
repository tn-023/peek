#include "db.h"
#include "core/textutil.h"

#include <cmath>
#include <cstdio>

#include <unordered_set>

#include <sqlite3.h>

namespace peek {

namespace {

struct Stmt {
    sqlite3_stmt* s = nullptr;
    Stmt(sqlite3* db, const char* sql) {
        if (db) sqlite3_prepare_v2(db, sql, -1, &s, nullptr);
    }
    ~Stmt() { if (s) sqlite3_finalize(s); }
    explicit operator bool() const { return s != nullptr; }
    void Text(int i, const std::wstring& w) {
        sqlite3_bind_text16(s, i, w.c_str(), static_cast<int>(w.size() * sizeof(wchar_t)), SQLITE_TRANSIENT);
    }
    void Int(int i, int64_t v) { sqlite3_bind_int64(s, i, v); }
    void TextA(int i, const std::string& a) {
        sqlite3_bind_text(s, i, a.c_str(), static_cast<int>(a.size()), SQLITE_TRANSIENT);
    }
    void Blob(int i, const std::vector<uint8_t>& b) {
        sqlite3_bind_blob(s, i, b.data(), static_cast<int>(b.size()), SQLITE_TRANSIENT);
    }
    int Step() { return sqlite3_step(s); }
    void Reset() { sqlite3_reset(s); sqlite3_clear_bindings(s); }
    int64_t ColInt(int i) { return sqlite3_column_int64(s, i); }
    std::string ColTextA(int i) {
        auto p = reinterpret_cast<const char*>(sqlite3_column_text(s, i));
        if (!p) return {};
        return std::string(p, static_cast<size_t>(sqlite3_column_bytes(s, i)));
    }
    std::wstring ColText(int i) {
        auto p = static_cast<const wchar_t*>(sqlite3_column_text16(s, i));
        if (!p) return {};
        return std::wstring(p, static_cast<size_t>(sqlite3_column_bytes16(s, i)) / sizeof(wchar_t));
    }
};

void Exec(sqlite3* db, const char* sql) {
    if (db) sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
}

sqlite3* OpenDb(const std::wstring& file) {
    sqlite3* db = nullptr;
    std::string u = WideToUtf8(file);
    if (sqlite3_open_v2(u.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return nullptr;
    }
    sqlite3_busy_timeout(db, 2000);
    Exec(db, "PRAGMA journal_mode=WAL;");
    Exec(db, "PRAGMA synchronous=NORMAL;");
    Exec(db, "PRAGMA temp_store=MEMORY;");
    Exec(db, "PRAGMA mmap_size=268435456;");
    Exec(db, "PRAGMA cache_size=-32768;");
    Exec(db, "PRAGMA secure_delete=ON;");   // Geloeschtes ueberschreiben statt nur freigeben
    return db;
}

// Bereich aller Schluessel unterhalb eines Ordners. Die Kuerzel enthalten weder '\\' noch ']'
// (']' folgt im Zeichensatz direkt auf '\\'), daher erfasst [lo, hi) genau die Pfade "ordner\\...".
void RangeKeys(const std::string& base, std::string& lo, std::string& hi) {
    lo = base + "\\";
    hi = base + "]";
}

std::string SingleValue(sqlite3* db, const char* sql) {
    Stmt st(db, sql);
    if (!st || st.Step() != SQLITE_ROW) return {};
    return st.ColTextA(0);
}

// Aufnahmeort verschluesselt wie das Kameramodell (verraet sonst, wo jemand wohnt): "Breite,Laenge" als Text
std::wstring GeoText(bool has, double lat, double lon) {
    if (!has) return {};
    wchar_t b[64];
    swprintf_s(b, L"%.7f,%.7f", lat, lon);
    return b;
}

void GeoParse(const std::wstring& s, bool& has, double& lat, double& lon) {
    has = false;
    lat = lon = 0;
    double a = 0, b = 0;
    if (s.empty() || swscanf_s(s.c_str(), L"%lf,%lf", &a, &b) != 2) return;
    if (!(std::fabs(a) <= 90 && std::fabs(b) <= 180)) return;
    has = true;
    lat = a;
    lon = b;
}

bool HasTable(sqlite3* db, const char* name) {
    Stmt st(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1;");
    if (!st) return false;
    sqlite3_bind_text(st.s, 1, name, -1, SQLITE_STATIC);
    return st.Step() == SQLITE_ROW;
}

void DeleteDbFiles(const std::wstring& dir) {
    for (const wchar_t* f : {L"index.db", L"index.db-wal", L"index.db-shm", L"thumbs.db", L"thumbs.db-wal", L"thumbs.db-shm"})
        DeleteFileW(JoinPath(dir, f).c_str());
}

constexpr const char* kSchema = "3-verschluesselt";

} // namespace

Database::~Database() { Close(); }

bool Database::Open(const std::wstring& dir) {
    // Schluessel laden bzw. erzeugen; ein neuer Schluessel heisst: alter Index ist nicht lesbar -> verwerfen
    bool fresh = false;
    if (!crypto_.Init(dir, fresh)) return false;
    if (fresh) DeleteDbFiles(dir);
    const std::string check = crypto_.Token(L"peek-index");
    auto valid = [&](sqlite3* db) {
        if (!HasTable(db, "info")) return !HasTable(db, "files") && !HasTable(db, "thumbs");   // leer = neu
        return SingleValue(db, "SELECT value FROM info WHERE key='schema';") == kSchema &&
               SingleValue(db, "SELECT value FROM info WHERE key='check';") == check;
    };
    for (int attempt = 0; attempt < 2; ++attempt) {
        meta_ = OpenDb(JoinPath(dir, L"index.db"));
        thumbs_ = OpenDb(JoinPath(dir, L"thumbs.db"));
        if (meta_ && thumbs_ && valid(meta_) && valid(thumbs_)) break;
        // alte unverschluesselte Version, anderer Schluessel oder beschaedigt: Dateien loeschen, neu anlegen
        Close();
        DeleteDbFiles(dir);
    }
    if (!meta_ || !thumbs_) return false;
    // Pfade, Kameramodell und Vorschaubilder sind verschluesselt (cachecrypt.h); Zahlen (Groesse, Zeiten,
    // Abmessungen) bleiben lesbar, damit die Abfragen schnell bleiben.
    Exec(meta_,
         "CREATE TABLE IF NOT EXISTS files("
         " path TEXT PRIMARY KEY, size INTEGER, mtime INTEGER, ctime INTEGER,"
         " kind INTEGER, attrs INTEGER, meta_ok INTEGER DEFAULT 0, width INTEGER DEFAULT 0,"
         " height INTEGER DEFAULT 0, dur INTEGER DEFAULT 0, taken INTEGER DEFAULT 0, camera TEXT,"
         " seen INTEGER DEFAULT 0);");
    Exec(thumbs_,
         "CREATE TABLE IF NOT EXISTS thumbs(path TEXT PRIMARY KEY, size INTEGER, mtime INTEGER, data BLOB);");
    {
        // Spalte fuer den Aufnahmeort (seit 1.6.8): nachtraeglich anlegen und die Metadaten aller Bilder neu
        // lesen lassen; Vorschaubilder und uebrige Eintraege bleiben erhalten
        bool hasGeo = false;
        {
            Stmt probe(meta_, "SELECT geo FROM files LIMIT 0;");
            hasGeo = static_cast<bool>(probe);
        }
        if (!hasGeo) {
            Exec(meta_, "ALTER TABLE files ADD COLUMN geo TEXT;");
            Exec(meta_, "UPDATE files SET meta_ok=0 WHERE kind=1;");
        }
        // Fingerabdruck des Vorschaubilds fuer die Duplikatsuche (seit 1.6.9; verschluesselt wie alles andere)
        bool hasHash = false;
        {
            Stmt probe(thumbs_, "SELECT phash FROM thumbs LIMIT 0;");
            hasHash = static_cast<bool>(probe);
        }
        if (!hasHash) Exec(thumbs_, "ALTER TABLE thumbs ADD COLUMN phash TEXT;");
    }
    for (sqlite3* db : {meta_, thumbs_}) {
        Exec(db, "CREATE TABLE IF NOT EXISTS info(key TEXT PRIMARY KEY, value TEXT);");
        Stmt st(db, "INSERT OR REPLACE INTO info(key, value) VALUES('schema', ?1), ('check', ?2);");
        if (st) {
            st.TextA(1, kSchema);
            st.TextA(2, check);
            st.Step();
        }
    }
    return true;
}

void Database::Close() {
    // WAL-Datei leeren: sie enthaelt sonst noch alte Seitenstaende
    if (meta_) { Exec(meta_, "PRAGMA wal_checkpoint(TRUNCATE);"); sqlite3_close_v2(meta_); meta_ = nullptr; }
    if (thumbs_) { Exec(thumbs_, "PRAGMA wal_checkpoint(TRUNCATE);"); sqlite3_close_v2(thumbs_); thumbs_ = nullptr; }
}

bool Database::GetMeta(const std::wstring& path, uint64_t size, int64_t mtime, MetaRow& out) {
    std::lock_guard<std::mutex> lock(metaMutex_);
    Stmt st(meta_, "SELECT width, height, dur, taken, camera, geo FROM files "
                   "WHERE path=?1 AND size=?2 AND mtime=?3 AND meta_ok=1;");
    if (!st) return false;
    st.TextA(1, crypto_.PathKey(path));
    st.Int(2, static_cast<int64_t>(size));
    st.Int(3, mtime);
    if (st.Step() != SQLITE_ROW) return false;
    out.width = static_cast<uint32_t>(st.ColInt(0));
    out.height = static_cast<uint32_t>(st.ColInt(1));
    out.durationMs = static_cast<uint32_t>(st.ColInt(2));
    out.taken = st.ColInt(3);
    crypto_.FromToken(st.ColTextA(4), out.camera);
    std::wstring geo;
    crypto_.FromToken(st.ColTextA(5), geo);
    GeoParse(geo, out.hasGeo, out.lat, out.lon);
    return true;
}

void Database::PutFile(const FileEntry& e, const MetaRow* m, int64_t seen) {
    std::lock_guard<std::mutex> lock(metaMutex_);
    PutFileLocked(e, m, seen);
}

void Database::PutMetaBatch(const std::vector<std::pair<FileEntry, MetaRow>>& rows, int64_t seen) {
    if (rows.empty()) return;
    std::lock_guard<std::mutex> lock(metaMutex_);
    Exec(meta_, "BEGIN;");
    for (auto& r : rows) PutFileLocked(r.first, &r.second, seen);
    Exec(meta_, "COMMIT;");
}

void Database::PutFileLocked(const FileEntry& e, const MetaRow* m, int64_t seen) {
    Stmt st(meta_,
            "INSERT INTO files(path, size, mtime, ctime, kind, attrs, meta_ok, width, height, dur, taken,"
            " camera, seen, geo) VALUES(?1,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15)"
            " ON CONFLICT(path) DO UPDATE SET size=excluded.size, mtime=excluded.mtime,"
            " ctime=excluded.ctime, kind=excluded.kind, attrs=excluded.attrs, meta_ok=excluded.meta_ok,"
            " width=excluded.width, height=excluded.height, dur=excluded.dur, taken=excluded.taken,"
            " camera=excluded.camera, seen=MAX(files.seen, excluded.seen), geo=excluded.geo;");
    if (!st) return;
    st.TextA(1, crypto_.PathKey(e.path));
    st.Int(3, static_cast<int64_t>(e.size));
    st.Int(4, e.mtime);
    st.Int(5, e.ctime);
    st.Int(6, static_cast<int64_t>(e.kind));
    st.Int(7, e.attrs);
    st.Int(8, m ? 1 : 0);
    st.Int(9, m ? m->width : 0);
    st.Int(10, m ? m->height : 0);
    st.Int(11, m ? m->durationMs : 0);
    st.Int(12, m ? m->taken : 0);
    st.TextA(13, m ? crypto_.Token(m->camera) : std::string());
    st.Int(14, seen);
    st.TextA(15, m && m->hasGeo ? crypto_.Token(GeoText(true, m->lat, m->lon)) : std::string());
    st.Step();
}

void Database::PutFiles(const std::vector<FileEntry>& all, int64_t seen) {
    // in kleinen Transaktionen, damit die Datenbank fuer die Ansicht nie lange gesperrt ist
    const size_t kChunk = 400;
    for (size_t from = 0; from < all.size(); from += kChunk) {
        std::vector<FileEntry> part(all.begin() + from, all.begin() + std::min(all.size(), from + kChunk));
        PutFilesChunk(part, seen);
    }
}

void Database::PutFilesChunk(const std::vector<FileEntry>& entries, int64_t seen) {
    std::lock_guard<std::mutex> lock(metaMutex_);
    Exec(meta_, "BEGIN;");
    {
        Stmt st(meta_,
                "INSERT INTO files(path, size, mtime, ctime, kind, attrs, seen) VALUES(?1,?3,?4,?5,?6,?7,?8)"
                " ON CONFLICT(path) DO UPDATE SET"
                " meta_ok = CASE WHEN files.size=excluded.size AND files.mtime=excluded.mtime THEN files.meta_ok ELSE 0 END,"
                " size=excluded.size, mtime=excluded.mtime, ctime=excluded.ctime,"
                " kind=excluded.kind, attrs=excluded.attrs, seen=excluded.seen;");
        if (st) {
            for (auto& e : entries) {
                if (e.kind == Kind::Folder) continue;
                st.TextA(1, crypto_.PathKey(e.path));
                st.Int(3, static_cast<int64_t>(e.size));
                st.Int(4, e.mtime);
                st.Int(5, e.ctime);
                st.Int(6, static_cast<int64_t>(e.kind));
                st.Int(7, e.attrs);
                st.Int(8, seen);
                st.Step();
                st.Reset();
            }
        }
    }
    Exec(meta_, "COMMIT;");
}

void Database::DeleteUnseenUnder(const std::wstring& root, int64_t seen) {
    std::string lo, hi;
    RangeKeys(crypto_.PathKey(root), lo, hi);
    std::vector<std::string> gone;
    {
        std::lock_guard<std::mutex> lock(metaMutex_);
        Stmt q(meta_, "SELECT path FROM files WHERE path >= ?1 AND path < ?2 AND seen < ?3;");
        if (!q) return;
        q.TextA(1, lo);
        q.TextA(2, hi);
        q.Int(3, seen);
        while (q.Step() == SQLITE_ROW) gone.push_back(q.ColTextA(0));
        Stmt st(meta_, "DELETE FROM files WHERE path >= ?1 AND path < ?2 AND seen < ?3;");
        if (st) {
            st.TextA(1, lo);
            st.TextA(2, hi);
            st.Int(3, seen);
            st.Step();
        }
    }
    // Vorschaubilder der nicht mehr vorhandenen Dateien ebenfalls entfernen
    DeleteThumbKeys(gone);
}

void Database::DeleteThumbKeys(const std::vector<std::string>& keys) {
    if (keys.empty()) return;
    std::lock_guard<std::mutex> lock(thumbMutex_);
    Exec(thumbs_, "BEGIN;");
    {
        Stmt st(thumbs_, "DELETE FROM thumbs WHERE path=?1;");
        if (st) {
            for (auto& k : keys) {
                st.TextA(1, k);
                st.Step();
                st.Reset();
            }
        }
    }
    Exec(thumbs_, "COMMIT;");
}

void Database::DeleteUnder(const std::wstring& root) {
    std::string lo, hi;
    RangeKeys(crypto_.PathKey(root), lo, hi);
    {
        std::lock_guard<std::mutex> lock(metaMutex_);
        Stmt st(meta_, "DELETE FROM files WHERE path >= ?1 AND path < ?2;");
        if (st) { st.TextA(1, lo); st.TextA(2, hi); st.Step(); }
    }
    {
        std::lock_guard<std::mutex> lock(thumbMutex_);
        Stmt st(thumbs_, "DELETE FROM thumbs WHERE path >= ?1 AND path < ?2;");
        if (st) { st.TextA(1, lo); st.TextA(2, hi); st.Step(); }
    }
}

void Database::Forget(const std::vector<std::wstring>& paths) {
    // Datei oder Ordner: der Eintrag selbst und alles darunter
    for (auto& p : paths) {
        std::string key = crypto_.PathKey(p);
        if (key.empty()) continue;
        std::string lo, hi;
        RangeKeys(key, lo, hi);
        {
            std::lock_guard<std::mutex> lock(metaMutex_);
            Stmt st(meta_, "DELETE FROM files WHERE path=?1 OR (path >= ?2 AND path < ?3);");
            if (st) { st.TextA(1, key); st.TextA(2, lo); st.TextA(3, hi); st.Step(); }
        }
        {
            std::lock_guard<std::mutex> lock(thumbMutex_);
            Stmt st(thumbs_, "DELETE FROM thumbs WHERE path=?1 OR (path >= ?2 AND path < ?3);");
            if (st) { st.TextA(1, key); st.TextA(2, lo); st.TextA(3, hi); st.Step(); }
        }
    }
}

void Database::PruneFolder(const std::wstring& folder, const std::vector<std::wstring>& existing) {
    std::string base = crypto_.PathKey(folder);
    if (base.empty()) return;
    std::unordered_set<std::string> keep;
    keep.reserve(existing.size() * 2);
    for (auto& p : existing) keep.insert(crypto_.Token(FileNameOf(p)));
    std::string lo, hi;
    RangeKeys(base, lo, hi);
    // nur direkte Eintraege des Ordners (Unterordner werden beim Oeffnen dort geprueft)
    auto orphans = [&](sqlite3* db, std::mutex& m, const char* table) {
        std::vector<std::string> out;
        std::lock_guard<std::mutex> lock(m);
        std::string sql = std::string("SELECT path FROM ") + table + " WHERE path >= ?1 AND path < ?2;";
        Stmt q(db, sql.c_str());
        if (!q) return out;
        q.TextA(1, lo);
        q.TextA(2, hi);
        while (q.Step() == SQLITE_ROW) {
            std::string k = q.ColTextA(0);
            std::string rest = k.substr(lo.size());
            if (rest.find('\\') == std::string::npos && !keep.count(rest)) out.push_back(std::move(k));
        }
        return out;
    };
    std::vector<std::string> goneMeta = orphans(meta_, metaMutex_, "files");
    std::vector<std::string> goneThumbs = orphans(thumbs_, thumbMutex_, "thumbs");
    if (!goneMeta.empty()) {
        std::lock_guard<std::mutex> lock(metaMutex_);
        Exec(meta_, "BEGIN;");
        {
            Stmt st(meta_, "DELETE FROM files WHERE path=?1;");
            if (st) {
                for (auto& k : goneMeta) {
                    st.TextA(1, k);
                    st.Step();
                    st.Reset();
                }
            }
        }
        Exec(meta_, "COMMIT;");
    }
    DeleteThumbKeys(goneThumbs);
}

std::vector<FileEntry> Database::LoadUnder(const std::wstring& root) {
    std::vector<FileEntry> out;
    std::string lo, hi;
    RangeKeys(crypto_.PathKey(root), lo, hi);
    std::lock_guard<std::mutex> lock(metaMutex_);
    Stmt st(meta_, "SELECT path, size, mtime, ctime, kind, attrs, meta_ok, width, height, dur, taken, camera, geo"
                   " FROM files WHERE path >= ?1 AND path < ?2;");
    if (!st) return out;
    st.TextA(1, lo);
    st.TextA(2, hi);
    while (st.Step() == SQLITE_ROW) {
        FileEntry e;
        if (!crypto_.PathFromKey(st.ColTextA(0), e.path)) continue;
        e.name = FileNameOf(e.path);
        e.size = static_cast<uint64_t>(st.ColInt(1));
        e.mtime = st.ColInt(2);
        e.ctime = st.ColInt(3);
        e.kind = static_cast<Kind>(st.ColInt(4));
        e.attrs = static_cast<uint32_t>(st.ColInt(5));
        e.metaDone = st.ColInt(6) != 0;
        e.width = static_cast<uint32_t>(st.ColInt(7));
        e.height = static_cast<uint32_t>(st.ColInt(8));
        e.durationMs = static_cast<uint32_t>(st.ColInt(9));
        e.taken = st.ColInt(10);
        crypto_.FromToken(st.ColTextA(11), e.camera);
        {
            std::wstring geo;
            crypto_.FromToken(st.ColTextA(12), geo);
            GeoParse(geo, e.hasGeo, e.lat, e.lon);
        }
        e.cameraLower = FoldForMatch(ToLower(e.camera));
        e.nameLower = FoldForMatch(ToLower(e.name));
        e.folderLower = FoldForMatch(ToLower(ParentPath(e.path)));
        size_t dot = e.name.find_last_of(L'.');
        if (dot != std::wstring::npos && dot + 1 < e.name.size()) e.ext = ToLower(e.name.substr(dot + 1));
        e.offline = (e.attrs & (FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN |
                                FILE_ATTRIBUTE_OFFLINE)) != 0;
        out.push_back(std::move(e));
    }
    return out;
}

uint64_t Database::CountUnder(const std::wstring& root) {
    std::string lo, hi;
    RangeKeys(crypto_.PathKey(root), lo, hi);
    std::lock_guard<std::mutex> lock(metaMutex_);
    Stmt st(meta_, "SELECT COUNT(*) FROM files WHERE path >= ?1 AND path < ?2;");
    if (!st) return 0;
    st.TextA(1, lo);
    st.TextA(2, hi);
    return st.Step() == SQLITE_ROW ? static_cast<uint64_t>(st.ColInt(0)) : 0;
}

bool Database::GetThumb(const std::wstring& path, uint64_t size, int64_t mtime, std::vector<uint8_t>& jpeg) {
    std::lock_guard<std::mutex> lock(thumbMutex_);
    Stmt st(thumbs_, "SELECT data FROM thumbs WHERE path=?1 AND size=?2 AND mtime=?3;");
    if (!st) return false;
    st.TextA(1, crypto_.PathKey(path));
    st.Int(2, static_cast<int64_t>(size));
    st.Int(3, mtime);
    if (st.Step() != SQLITE_ROW) return false;
    auto p = static_cast<const uint8_t*>(sqlite3_column_blob(st.s, 0));
    int n = sqlite3_column_bytes(st.s, 0);
    if (!p || n <= 0) return false;
    return crypto_.Open(p, static_cast<size_t>(n), jpeg);
}

bool Database::GetHash(const std::wstring& path, uint64_t size, int64_t mtime, uint64_t& hash) {
    std::lock_guard<std::mutex> lock(thumbMutex_);
    Stmt st(thumbs_, "SELECT phash FROM thumbs WHERE path=?1 AND size=?2 AND mtime=?3;");
    if (!st) return false;
    st.TextA(1, crypto_.PathKey(path));
    st.Int(2, static_cast<int64_t>(size));
    st.Int(3, mtime);
    if (st.Step() != SQLITE_ROW) return false;
    std::wstring text;
    if (!crypto_.FromToken(st.ColTextA(0), text) || text.size() != 16) return false;
    hash = wcstoull(text.c_str(), nullptr, 16);
    return true;
}

void Database::PutHash(const std::wstring& path, uint64_t size, int64_t mtime, uint64_t hash) {
    std::lock_guard<std::mutex> lock(thumbMutex_);
    wchar_t b[24];
    swprintf_s(b, L"%016llx", static_cast<unsigned long long>(hash));
    Stmt st(thumbs_, "UPDATE thumbs SET phash=?4 WHERE path=?1 AND size=?2 AND mtime=?3;");
    if (!st) return;
    st.TextA(1, crypto_.PathKey(path));
    st.Int(2, static_cast<int64_t>(size));
    st.Int(3, mtime);
    st.TextA(4, crypto_.Token(b));
    st.Step();
}

bool Database::HasThumb(const std::wstring& path, uint64_t size, int64_t mtime) {
    std::lock_guard<std::mutex> lock(thumbMutex_);
    Stmt st(thumbs_, "SELECT 1 FROM thumbs WHERE path=?1 AND size=?2 AND mtime=?3;");
    if (!st) return false;
    st.TextA(1, crypto_.PathKey(path));
    st.Int(2, static_cast<int64_t>(size));
    st.Int(3, mtime);
    return st.Step() == SQLITE_ROW;
}

void Database::PutThumb(const std::wstring& path, uint64_t size, int64_t mtime, const std::vector<uint8_t>& jpeg) {
    std::lock_guard<std::mutex> lock(thumbMutex_);
    std::vector<uint8_t> sealed = crypto_.Seal(jpeg.data(), jpeg.size());
    if (sealed.empty()) return;
    Stmt st(thumbs_, "INSERT OR REPLACE INTO thumbs(path, size, mtime, data) VALUES(?1, ?2, ?3, ?4);");
    if (!st) return;
    st.TextA(1, crypto_.PathKey(path));
    st.Int(2, static_cast<int64_t>(size));
    st.Int(3, mtime);
    st.Blob(4, sealed);
    st.Step();
}

Database::Stats Database::GetStats() {
    Stats st;
    {
        std::lock_guard<std::mutex> lock(metaMutex_);
        Stmt q(meta_, "SELECT COUNT(*), SUM(kind IN (1,2)), SUM(meta_ok) FROM files;");
        if (q && q.Step() == SQLITE_ROW) {
            st.files = static_cast<uint64_t>(q.ColInt(0));
            st.media = static_cast<uint64_t>(q.ColInt(1));
            st.withMeta = static_cast<uint64_t>(q.ColInt(2));
        }
    }
    {
        std::lock_guard<std::mutex> lock(thumbMutex_);
        Stmt q(thumbs_, "SELECT COUNT(*) FROM thumbs;");
        if (q && q.Step() == SQLITE_ROW) st.thumbs = static_cast<uint64_t>(q.ColInt(0));
    }
    return st;
}

void Database::ClearAll() {
    {
        std::lock_guard<std::mutex> lock(metaMutex_);
        Exec(meta_, "DELETE FROM files;");
        Exec(meta_, "PRAGMA wal_checkpoint(TRUNCATE);");
        Exec(meta_, "VACUUM;");
    }
    {
        std::lock_guard<std::mutex> lock(thumbMutex_);
        Exec(thumbs_, "DELETE FROM thumbs;");
        Exec(thumbs_, "PRAGMA wal_checkpoint(TRUNCATE);");
        Exec(thumbs_, "VACUUM;");
    }
}

} // namespace peek
