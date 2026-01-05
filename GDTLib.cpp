// GDTLib.cpp � dynamic-loads BO3's sqlite64r.dll from TA_TOOLS_PATH\bin
#include "pch.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <mutex>
#include <cctype>
#include <fstream>

#include <windows.h>
#include <sqlite3.h>   // internal only

#include "gdt.h"
#include "entity.h"
#include "GDTLib.h"

// ============================================================================
// Globals
// ============================================================================
static sqlite3* g_db = nullptr;
static std::mutex g_dbMutex;

// ============================================================================
// Helpers
// ============================================================================
static bool FileExistsW(const std::wstring& path) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static std::wstring Utf8ToWide(const char* s) {
    if (!s) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n ? n - 1 : 0, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

static HMODULE LoadSqliteFromFullPath(const std::wstring& fullPath) {
    // A) simplest � load by full path
    HMODULE mod = LoadLibraryW(fullPath.c_str());
    if (mod) return mod;

    // B) safe search model � search the DLL's own dir for its deps
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
    mod = LoadLibraryExW(fullPath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
    if (mod) return mod;

    // C) legacy fallback � temporarily put dir on search path
    std::wstring dir = fullPath.substr(0, fullPath.find_last_of(L"\\/"));
    SetDllDirectoryW(dir.c_str());
    mod = LoadLibraryW(fullPath.c_str());
    SetDllDirectoryW(L"");

    return mod;
}

// ============================================================================
// Public path helpers
// ============================================================================
GDTDB_API const char* GetRootPath() {
    const char* env = std::getenv("TA_TOOLS_PATH");
    return env ? env : "";
}

GDTDB_API const char* GetGdtDbPath() {
    static char gdt_db_path[1024];
    const char* root = GetRootPath();
    if (root[0] == '\0') return "";
    std::snprintf(gdt_db_path, sizeof(gdt_db_path), "%s\\gdtdb\\gdt.db", root);
    return gdt_db_path;
}

// ============================================================================
// Dynamic SQLite loader (only symbols used here)
// ============================================================================
namespace {
    struct SqliteAPI {
        HMODULE mod{};

        // Core API used in this file
        int(__cdecl* open_v2)(const char*, sqlite3**, int, const char*) = nullptr;
        int(__cdecl* close_v2)(sqlite3*) = nullptr;
        int(__cdecl* exec)(sqlite3*, const char*, int(*)(void*, int, char**, char**), void*, char**) = nullptr;
        int(__cdecl* prepare_v2)(sqlite3*, const char*, int, sqlite3_stmt**, const char**) = nullptr;
        int(__cdecl* step)(sqlite3_stmt*) = nullptr;
        int(__cdecl* finalize)(sqlite3_stmt*) = nullptr;
        int(__cdecl* busy_timeout)(sqlite3*, int) = nullptr;
        const char* (__cdecl* errmsg)(sqlite3*) = nullptr;

        int(__cdecl* bind_int)(sqlite3_stmt*, int, int) = nullptr;
        const unsigned char* (__cdecl* column_text)(sqlite3_stmt*, int) = nullptr;
        int(__cdecl* column_int)(sqlite3_stmt*, int) = nullptr;
        int(__cdecl* column_type)(sqlite3_stmt*, int) = nullptr;

        // Optional version probes
        const char* (__cdecl* libversion)() = nullptr;
        int(__cdecl* libversion_number)() = nullptr;

        bool loaded() const noexcept { return mod != nullptr; }
    };

    static SqliteAPI g_sql;
    static std::wstring g_forcedSqliteBinDir; // optional override via API

    template<typename T>
    static void Require(T*& fn, HMODULE m, const char* name) {
        fn = reinterpret_cast<T*>(GetProcAddress(m, name));
        if (!fn) {
            std::string msg = "Missing SQLite symbol: ";
            msg += name;
            throw std::runtime_error(msg);
        }
    }

    static std::wstring DefaultBo3BinDir() {
        const char* root = GetRootPath(); // e.g., E:\...\Call of Duty Black Ops III
        if (!root || !*root) return {};
        std::wstring wroot = Utf8ToWide(root);
        if (wroot.empty()) return {};
        if (wroot.back() == L'\\' || wroot.back() == L'/')
            return wroot + L"bin";
        return wroot + L"\\bin";
    }

    static std::wstring ComposeSqliteFullPath() {
        std::wstring binDir = g_forcedSqliteBinDir.empty() ? DefaultBo3BinDir() : g_forcedSqliteBinDir;
        if (binDir.empty()) return {};
        if (binDir.back() == L'\\' || binDir.back() == L'/')
            return binDir + L"sqlite64r.dll";
        return binDir + L"\\sqlite64r.dll";
    }

    static void EnsureSqliteLoaded() {
        if (g_sql.loaded()) return;

        std::wstring dllPath = ComposeSqliteFullPath();
        if (dllPath.empty()) throw std::runtime_error("TA_TOOLS_PATH not set");
        if (!FileExistsW(dllPath)) throw std::runtime_error("sqlite64r.dll not found");

        HMODULE mod = LoadSqliteFromFullPath(dllPath);
        if (!mod) throw std::runtime_error("LoadLibraryExW failed for sqlite64r.dll");

        Require(g_sql.open_v2, mod, "sqlite3_open_v2");
        Require(g_sql.close_v2, mod, "sqlite3_close_v2");
        Require(g_sql.exec, mod, "sqlite3_exec");
        Require(g_sql.prepare_v2, mod, "sqlite3_prepare_v2");
        Require(g_sql.step, mod, "sqlite3_step");
        Require(g_sql.finalize, mod, "sqlite3_finalize");
        Require(g_sql.busy_timeout, mod, "sqlite3_busy_timeout");
        Require(g_sql.errmsg, mod, "sqlite3_errmsg");
        Require(g_sql.bind_int, mod, "sqlite3_bind_int");
        Require(g_sql.column_text, mod, "sqlite3_column_text");
        Require(g_sql.column_int, mod, "sqlite3_column_int");
        Require(g_sql.column_type, mod, "sqlite3_column_type");

        g_sql.libversion = reinterpret_cast<const char* (__cdecl*)(void)>(GetProcAddress(mod, "sqlite3_libversion"));
        g_sql.libversion_number = reinterpret_cast<int(__cdecl*)(void)>(GetProcAddress(mod, "sqlite3_libversion_number"));

        g_sql.mod = mod;
    }
} // anonymous namespace

// Allow callers to override where to load sqlite64r.dll from (e.g., ...\bin)
GDTDB_API void GdtDB_SetSqliteBinDir(const wchar_t* bo3BinDirW) {
    std::lock_guard<std::mutex> lock(g_dbMutex);
    g_forcedSqliteBinDir = bo3BinDirW ? bo3BinDirW : L"";
}

// ============================================================================
// Pragmas / indexes
// ============================================================================
static void ApplyReadPragmas(sqlite3* db) {
    g_sql.exec(db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    g_sql.exec(db, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
    g_sql.exec(db, "PRAGMA temp_store=MEMORY;", nullptr, nullptr, nullptr);
    g_sql.exec(db, "PRAGMA mmap_size=268435456;", nullptr, nullptr, nullptr);
}

static void EnsureIndexes(sqlite3* db) {
    g_sql.exec(db, "CREATE INDEX IF NOT EXISTS idx_entity_fk_gdt ON _entity(FK_gdt);", nullptr, nullptr, nullptr);
    g_sql.exec(db, "CREATE INDEX IF NOT EXISTS idx_entity_name_gdt ON _entity(name, FK_gdt);", nullptr, nullptr, nullptr);
}

// ============================================================================
// DB lifecycle
// ============================================================================
GDTDB_CAPI int GdtDB_Init() {
    std::lock_guard<std::mutex> lock(g_dbMutex);
    if (g_db) return SQLITE_OK;

    try { EnsureSqliteLoaded(); }
    catch (...) { return SQLITE_CANTOPEN; }

    const char* path = GetGdtDbPath();
    if (!path || !*path) return SQLITE_CANTOPEN;

    int rc = g_sql.open_v2(path, &g_db, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr);
    if (rc != SQLITE_OK) {
        if (g_db) { g_sql.close_v2(g_db); g_db = nullptr; }
        return rc;
    }

    ApplyReadPragmas(g_db);
    EnsureIndexes(g_db);
    g_sql.busy_timeout(g_db, 250);
    return SQLITE_OK;
}

GDTDB_CAPI sqlite3* GdtDB_Get() {
    if (g_db) return g_db;
    GdtDB_Init();
    return g_db;
}

GDTDB_CAPI void GdtDB_Shutdown() {
    std::lock_guard<std::mutex> lock(g_dbMutex);
    if (g_db) {
        g_sql.close_v2(g_db);
        g_db = nullptr;
    }
}

// ============================================================================
// Queries
// ============================================================================
GDTDB_API std::map<std::string, gdt> GetGDTs() {
    std::map<std::string, gdt> results;

    sqlite3* db = GdtDB_Get();
    if (!db) return results;

    const char* sql = "SELECT PK_id, name, bOpenForEdit, timestamp FROM _gdt;";
    sqlite3_stmt* stmt = nullptr;

    int rc = g_sql.prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return results;

    while ((rc = g_sql.step(stmt)) == SQLITE_ROW) {
        int id = g_sql.column_int(stmt, 0);
        const unsigned char* pathU8 = g_sql.column_text(stmt, 1);
        int openForEdit = g_sql.column_int(stmt, 2);
        int timestamp = g_sql.column_int(stmt, 3);

        const char* pathC = reinterpret_cast<const char*>(pathU8 ? pathU8 : (const unsigned char*)"");
        gdt g(id, pathC, timestamp);
        g.openForEdit = (openForEdit != 0);

        results[g.name] = std::move(g);
    }

    g_sql.finalize(stmt);
    return results;
}

GDTDB_API std::map<std::string, entity> GetEntities() {
    std::map<std::string, entity> results;

    sqlite3* db = GdtDB_Get();
    if (!db) return results;

    const char* sql =
        "SELECT PK_id, name, iGdtSeqNum, FK_parent_id, FK_gdf, FK_gdt, _gdt_linenum, bExport "
        "FROM _entity;";

    sqlite3_stmt* stmt = nullptr;
    int rc = g_sql.prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return results;

    while ((rc = g_sql.step(stmt)) == SQLITE_ROW) {
        const int id = g_sql.column_int(stmt, 0);
        const unsigned char* nameU8 = g_sql.column_text(stmt, 1);
        const int seqNum = g_sql.column_int(stmt, 2);
        int parentID = (g_sql.column_type(stmt, 3) == SQLITE_NULL) ? -1 : g_sql.column_int(stmt, 3);
        const int gdfID = g_sql.column_int(stmt, 4);
        const int gdtID = g_sql.column_int(stmt, 5);
        const int lineNum = g_sql.column_int(stmt, 6);
        const int bExport = g_sql.column_int(stmt, 7);

        entity e(id,
            reinterpret_cast<const char*>(nameU8 ? nameU8 : (const unsigned char*)""),
            seqNum, parentID, gdfID, gdtID, lineNum);
        e.bExport = bExport;

        results[e.name] = std::move(e);
    }

    g_sql.finalize(stmt);
    return results;
}

GDTDB_API std::unordered_map<int, std::map<std::string, entity>> GetEntitiesByGdt() {
    std::unordered_map<int, std::map<std::string, entity>> grouped;

    sqlite3* db = GdtDB_Get();
    if (!db) return grouped;

    g_sql.exec(db, "BEGIN;", nullptr, nullptr, nullptr);
    const char* sql =
        "SELECT PK_id, name, iGdtSeqNum, FK_parent_id, FK_gdf, FK_gdt, _gdt_linenum, bExport "
        "FROM _entity ORDER BY FK_gdt, iGdtSeqNum;";

    sqlite3_stmt* stmt = nullptr;
    int rc = g_sql.prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        g_sql.exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return grouped;
    }

    while ((rc = g_sql.step(stmt)) == SQLITE_ROW) {
        const int id = g_sql.column_int(stmt, 0);
        const unsigned char* nameU8 = g_sql.column_text(stmt, 1);
        const int seqNum = g_sql.column_int(stmt, 2);
        int parentID = (g_sql.column_type(stmt, 3) == SQLITE_NULL) ? -1 : g_sql.column_int(stmt, 3);
        const int gdfID = g_sql.column_int(stmt, 4);
        const int gdtID = g_sql.column_int(stmt, 5);
        const int lineNum = g_sql.column_int(stmt, 6);
        const int bExport = g_sql.column_int(stmt, 7);

        entity e(id,
            reinterpret_cast<const char*>(nameU8 ? nameU8 : (const unsigned char*)""),
            seqNum, parentID, gdfID, gdtID, lineNum);
        e.bExport = bExport;

        grouped[gdtID].emplace(e.name, std::move(e));
    }

    g_sql.finalize(stmt);
    g_sql.exec(db, "COMMIT;", nullptr, nullptr, nullptr);
    return grouped;
}

GDTDB_API std::map<std::string, entity> GetEntities(const gdt& GDT) {
    std::map<std::string, entity> results;

    sqlite3* db = GdtDB_Get();
    if (!db) return results;

    const char* sql =
        "SELECT PK_id, name, iGdtSeqNum, FK_parent_id, FK_gdf, FK_gdt, _gdt_linenum, bExport "
        "FROM _entity WHERE FK_gdt = ? ORDER BY iGdtSeqNum;";

    sqlite3_stmt* stmt = nullptr;
    int rc = g_sql.prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return results;

    g_sql.bind_int(stmt, 1, GDT.id);

    while ((rc = g_sql.step(stmt)) == SQLITE_ROW) {
        const int id = g_sql.column_int(stmt, 0);
        const unsigned char* nameU8 = g_sql.column_text(stmt, 1);
        const int seqNum = g_sql.column_int(stmt, 2);
        int parentID = (g_sql.column_type(stmt, 3) == SQLITE_NULL) ? -1 : g_sql.column_int(stmt, 3);
        const int gdfID = g_sql.column_int(stmt, 4);
        const int gdtID = g_sql.column_int(stmt, 5);
        const int lineNum = g_sql.column_int(stmt, 6);
        const int bExport = g_sql.column_int(stmt, 7);

        entity e(id,
            reinterpret_cast<const char*>(nameU8 ? nameU8 : (const unsigned char*)""),
            seqNum, parentID, gdfID, gdtID, lineNum);
        e.bExport = bExport;

        results[e.name] = std::move(e);
    }

    g_sql.finalize(stmt);
    return results;
}

// Allow only identifiers that are safe to embed in SQL when quoting isn't possible
// (table names can't be bound as parameters, so we must construct those parts).
static bool IsSafeSqlIdentifier(const std::string& s) {
    if (s.empty()) return false;
    for (unsigned char c : s) {
        if (!(std::isalnum(c) || c == '_')) return false;
    }
    return true;
}

static std::string QuoteIdent(const std::string& ident) {
    // SQLite uses double-quotes for identifiers. Escape any embedded quotes defensively.
    std::string out;
    out.reserve(ident.size() + 2);
    out.push_back('"');
    for (char c : ident) {
        if (c == '"') out.push_back('"');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

GDTDB_API std::map<std::string, std::string> GetEntityProperties(const entity& ent)
{
    std::map<std::string, std::string> results;

    sqlite3* db = GdtDB_Get();
    if (!db) return results;

    // 1) Determine which GDF table to read from (e.g., "material", "tagfx", ...).
    const std::string table = ent.gdfName;
    if (!IsSafeSqlIdentifier(table) || table == "unknown_gdf")
        return results;

    // 2) Discover columns for that table via PRAGMA table_info(...)
    //    (so we don't need sqlite3_column_count/column_name symbols).
    std::vector<std::string> cols;
    {
        std::string pragmaSql = "PRAGMA table_info(" + QuoteIdent(table) + ");";
        sqlite3_stmt* stmtCols = nullptr;

        int rc = g_sql.prepare_v2(db, pragmaSql.c_str(), -1, &stmtCols, nullptr);
        if (rc != SQLITE_OK || !stmtCols)
            return results;

        while ((rc = g_sql.step(stmtCols)) == SQLITE_ROW) {
            // PRAGMA table_info(...) returns: cid, name, type, notnull, dflt_value, pk
            const unsigned char* nameU8 = g_sql.column_text(stmtCols, 1);
            std::string col = reinterpret_cast<const char*>(nameU8 ? nameU8 : (const unsigned char*)"");
            if (col.empty()) continue;

            // Skip internal bookkeeping columns.
            if (col == "PK_id" || col == "_precalc_md5" || col == "_derived_bits")
                continue;

            cols.emplace_back(std::move(col));
        }

        g_sql.finalize(stmtCols);
    }

    if (cols.empty())
        return results;

    // 3) Read the row by PK_id (this should match _entity.PK_id in gdtdb).
    std::string selectSql = "SELECT ";
    for (size_t i = 0; i < cols.size(); ++i) {
        if (i) selectSql += ",";
        selectSql += QuoteIdent(cols[i]);
    }
    selectSql += " FROM " + QuoteIdent(table) + " WHERE PK_id = ?;";

    sqlite3_stmt* stmt = nullptr;
    int rc = g_sql.prepare_v2(db, selectSql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK || !stmt)
        return results;

    g_sql.bind_int(stmt, 1, ent.id);

    rc = g_sql.step(stmt);
    if (rc == SQLITE_ROW) {
        for (int i = 0; i < static_cast<int>(cols.size()); ++i) {
            const int t = g_sql.column_type(stmt, i);
            if (t == SQLITE_NULL) {
                results[cols[i]] = "";
                continue;
            }

            // For numeric/text types, sqlite3_column_text returns a UTF-8 representation.
            // We skip blobs by design ("_derived_bits" was already removed).
            const unsigned char* vU8 = g_sql.column_text(stmt, i);
            results[cols[i]] = reinterpret_cast<const char*>(vU8 ? vU8 : (const unsigned char*)"");
        }
    }

    g_sql.finalize(stmt);
    return results;
}

static std::string ToLowerASCII(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) out.push_back((char)std::tolower(c));
    return out;
}

GDTDB_API bool WriteGDTToFile(const gdt& GDT) {
    std::string filePath = "C:\\Users\\aidan\\Downloads\\test.gdt";
    auto entities = GetEntities(GDT);

    std::ofstream file(filePath);
    if (!file.is_open()) return false;

    file << "{\n";
    for (const auto& [name, ent] : entities)
    {
        file << "\t\"" << name << "\" ( \"" << ent.gdfName << ".gdf\" )\n\t{\n";

        auto props = GetEntityProperties(ent);

        // Copy to vector and sort case-insensitively (with a tie-breaker)
        std::vector<std::pair<std::string, std::string>> sorted(props.begin(), props.end());
        std::sort(sorted.begin(), sorted.end(),
            [](const auto& a, const auto& b) {
                const std::string al = ToLowerASCII(a.first);
                const std::string bl = ToLowerASCII(b.first);
                if (al != bl) return al < bl;
                return a.first < b.first; // tie-break so ordering is strict & deterministic
            });

        for (const auto& [propName, propValue] : sorted)
        {
            if (propName != "_name")
                file << "\t\t\"" << propName << "\" \"" << propValue << "\"\n";
        }

        file << "\t}\n";
    }
    file << "}";
    return true;
}

// ============================================================================
// Launch gdtdb.exe /update and print its stdout/stderr
// ============================================================================
GDTDB_API bool RunGdtDbUpdate() {
    const char* root = GetRootPath();
    if (!root || !*root) return false;

    char exePath[MAX_PATH];
    std::snprintf(exePath, sizeof(exePath), "%s\\gdtdb\\gdtdb.exe", root);
    if (GetFileAttributesA(exePath) == INVALID_FILE_ATTRIBUTES) return false;

    std::string cmd = "\"";
    cmd += exePath;
    cmd += "\" /update";

    // Create pipes for stdout/stderr capture
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE hRead = nullptr, hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return false;
    if (!SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(hRead); CloseHandle(hWrite);
        return false;
    }

    STARTUPINFOA si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWrite;  // redirect child's stdout
    si.hStdError = hWrite;  // redirect child's stderr
    si.hStdInput = nullptr;

    BOOL ok = CreateProcessA(
        nullptr,
        cmd.data(),
        nullptr, nullptr,
        TRUE,                   // inherit handles so child can use hWrite
        CREATE_NO_WINDOW,
        nullptr, nullptr,
        &si, &pi
    );

    // Parent doesn't need the write end
    CloseHandle(hWrite);

    if (!ok) {
        CloseHandle(hRead);
        return false;
    }

    // Read child's combined stdout/stderr and print it
    char buffer[4096];
    DWORD bytesRead = 0;
    for (;;) {
        BOOL readOK = ReadFile(hRead, buffer, sizeof(buffer) - 1, &bytesRead, nullptr);
        if (!readOK || bytesRead == 0) break;
        buffer[bytesRead] = '\0';
        // Print exactly what the exe produced:
        std::fwrite(buffer, 1, bytesRead, stdout);
        std::fflush(stdout);
    }

    CloseHandle(hRead);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return exitCode == 0;
}

// ============================================================================
// Launch gdtdb.exe /rebuild and print its stdout/stderr
// ============================================================================
GDTDB_API bool RunGdtDbRebuild() {
    const char* root = GetRootPath();
    if (!root || !*root) return false;

    char exePath[MAX_PATH];
    std::snprintf(exePath, sizeof(exePath), "%s\\gdtdb\\gdtdb.exe", root);
    if (GetFileAttributesA(exePath) == INVALID_FILE_ATTRIBUTES) return false;

    std::string cmd = "\"";
    cmd += exePath;
    cmd += "\" /rebuild";

    // Create pipes for stdout/stderr capture
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE hRead = nullptr, hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return false;
    if (!SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(hRead); CloseHandle(hWrite);
        return false;
    }

    STARTUPINFOA si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWrite;  // redirect child's stdout
    si.hStdError = hWrite;  // redirect child's stderr
    si.hStdInput = nullptr;

    BOOL ok = CreateProcessA(
        nullptr,
        cmd.data(),
        nullptr, nullptr,
        TRUE,                   // inherit handles so child can use hWrite
        CREATE_NO_WINDOW,
        nullptr, nullptr,
        &si, &pi
    );

    // Parent doesn't need the write end
    CloseHandle(hWrite);

    if (!ok) {
        CloseHandle(hRead);
        return false;
    }

    // Read child's combined stdout/stderr and print it
    char buffer[4096];
    DWORD bytesRead = 0;
    for (;;) {
        BOOL readOK = ReadFile(hRead, buffer, sizeof(buffer) - 1, &bytesRead, nullptr);
        if (!readOK || bytesRead == 0) break;
        buffer[bytesRead] = '\0';
        // Print exactly what the exe produced:
        std::fwrite(buffer, 1, bytesRead, stdout);
        std::fflush(stdout);
    }

    CloseHandle(hRead);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return exitCode == 0;
}