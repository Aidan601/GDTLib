// GDTLib.cpp - uses statically linked SQLite
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
#include "sqlite3.h"   // statically linked

#include "gdt.h"
#include "entity.h"
#include "GDTLib.h"

// ============================================================================
// Globals
// ============================================================================
static sqlite3* g_db = nullptr;
static std::mutex g_dbMutex;

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
// Static SQLite wrapper (mimics old dynamic API for minimal code changes)
// ============================================================================
namespace {
    struct SqliteAPI {
        // Direct function pointers to static SQLite functions
        decltype(&sqlite3_open_v2) open_v2 = sqlite3_open_v2;
        decltype(&sqlite3_close_v2) close_v2 = sqlite3_close_v2;
        decltype(&sqlite3_exec) exec = sqlite3_exec;
        decltype(&sqlite3_prepare_v2) prepare_v2 = sqlite3_prepare_v2;
        decltype(&sqlite3_step) step = sqlite3_step;
        decltype(&sqlite3_finalize) finalize = sqlite3_finalize;
        decltype(&sqlite3_busy_timeout) busy_timeout = sqlite3_busy_timeout;
        decltype(&sqlite3_errmsg) errmsg = sqlite3_errmsg;
        decltype(&sqlite3_bind_int) bind_int = sqlite3_bind_int;
        decltype(&sqlite3_bind_text) bind_text = sqlite3_bind_text;
        decltype(&sqlite3_column_text) column_text = sqlite3_column_text;
        decltype(&sqlite3_column_int) column_int = sqlite3_column_int;
        decltype(&sqlite3_column_type) column_type = sqlite3_column_type;
        decltype(&sqlite3_libversion) libversion = sqlite3_libversion;
        decltype(&sqlite3_libversion_number) libversion_number = sqlite3_libversion_number;

        bool loaded() const noexcept { return true; } // Always loaded (static)
    };

    static SqliteAPI g_sql;

    static void EnsureSqliteLoaded() {
        // No-op: SQLite is statically linked
    }
} // anonymous namespace

// Deprecated: no longer needed with static linking
GDTDB_API void GdtDB_SetSqliteBinDir(const wchar_t* /*bo3BinDirW*/) {
    // No-op: SQLite is now statically linked
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
    if (g_db) {
        OutputDebugStringA("GdtDB_Init: already initialized\n");
        return SQLITE_OK;
    }

    try { EnsureSqliteLoaded(); }
    catch (...) {
        OutputDebugStringA("GdtDB_Init: EnsureSqliteLoaded failed\n");
        return SQLITE_CANTOPEN;
    }

    const char* path = GetGdtDbPath();
    if (!path || !*path) {
        OutputDebugStringA("GdtDB_Init: GetGdtDbPath returned empty\n");
        return SQLITE_CANTOPEN;
    }

    char buf[1024];
    snprintf(buf, sizeof(buf), "GdtDB_Init: opening %s\n", path);
    OutputDebugStringA(buf);

    int rc = g_sql.open_v2(path, &g_db, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (rc != SQLITE_OK) {
        snprintf(buf, sizeof(buf), "GdtDB_Init: open failed rc=%d\n", rc);
        OutputDebugStringA(buf);
        if (g_db) { g_sql.close_v2(g_db); g_db = nullptr; }
        return rc;
    }

    OutputDebugStringA("GdtDB_Init: success\n");
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
    if (!db) {
        OutputDebugStringA("GetGDTs: db is null\n");
        return results;
    }

    const char* sql = "SELECT PK_id, name, bOpenForEdit, timestamp FROM _gdt;";
    sqlite3_stmt* stmt = nullptr;

    int rc = g_sql.prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        char buf[256];
        snprintf(buf, sizeof(buf), "GetGDTs: prepare failed rc=%d err=%s\n", rc, g_sql.errmsg(db));
        OutputDebugStringA(buf);
        return results;
    }

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

    char buf[256];
    snprintf(buf, sizeof(buf), "GetGDTs: returning %zu results\n", results.size());
    OutputDebugStringA(buf);

    return results;
}

GDTDB_API std::map<std::string, entity> GetEntities() {
    std::map<std::string, entity> results;

    sqlite3* db = GdtDB_Get();
    if (!db) return results;

    const char* sql =
        "SELECT e.PK_id, e.name, e.iGdtSeqNum, e.FK_parent_id, e.FK_gdf, e.FK_gdt, e._gdt_linenum, e.bExport, "
        "p.name AS parent_name "
        "FROM _entity e "
        "LEFT JOIN _entity p ON e.FK_parent_id = p.PK_id;";

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
        const unsigned char* parentNameU8 = g_sql.column_text(stmt, 8);

        entity e(id,
            reinterpret_cast<const char*>(nameU8 ? nameU8 : (const unsigned char*)""),
            seqNum, parentID, gdfID, gdtID, lineNum);
        e.bExport = bExport;
        e.parentName = reinterpret_cast<const char*>(parentNameU8 ? parentNameU8 : (const unsigned char*)"");

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
        "SELECT e.PK_id, e.name, e.iGdtSeqNum, e.FK_parent_id, e.FK_gdf, e.FK_gdt, e._gdt_linenum, e.bExport, "
        "p.name AS parent_name "
        "FROM _entity e "
        "LEFT JOIN _entity p ON e.FK_parent_id = p.PK_id "
        "ORDER BY e.FK_gdt, e.iGdtSeqNum;";

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
        const unsigned char* parentNameU8 = g_sql.column_text(stmt, 8);

        entity e(id,
            reinterpret_cast<const char*>(nameU8 ? nameU8 : (const unsigned char*)""),
            seqNum, parentID, gdfID, gdtID, lineNum);
        e.bExport = bExport;
        e.parentName = reinterpret_cast<const char*>(parentNameU8 ? parentNameU8 : (const unsigned char*)"");

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
        "SELECT e.PK_id, e.name, e.iGdtSeqNum, e.FK_parent_id, e.FK_gdf, e.FK_gdt, e._gdt_linenum, e.bExport, "
        "p.name AS parent_name "
        "FROM _entity e "
        "LEFT JOIN _entity p ON e.FK_parent_id = p.PK_id "
        "WHERE e.FK_gdt = ? ORDER BY e.iGdtSeqNum;";

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
        const unsigned char* parentNameU8 = g_sql.column_text(stmt, 8);

        entity e(id,
            reinterpret_cast<const char*>(nameU8 ? nameU8 : (const unsigned char*)""),
            seqNum, parentID, gdfID, gdtID, lineNum);
        e.bExport = bExport;
        e.parentName = reinterpret_cast<const char*>(parentNameU8 ? parentNameU8 : (const unsigned char*)"");

        results[e.name] = std::move(e);
    }

    g_sql.finalize(stmt);
    return results;
}

entity FindEntityByName(const std::string& name)
{
    // Create default invalid entity (will have id = -1)
    entity ent;

    // Get database handle
    sqlite3* db = GdtDB_Get();
    if (!db)
        return ent;

    // SQL query to find entity by name (with parent name join)
    const char* sql =
        "SELECT e.PK_id, e.name, e.iGdtSeqNum, e.FK_parent_id, e.FK_gdf, e.FK_gdt, e._gdt_linenum, e.bExport, "
        "p.name AS parent_name "
        "FROM _entity e "
        "LEFT JOIN _entity p ON e.FK_parent_id = p.PK_id "
        "WHERE e.name = ? LIMIT 1;";

    sqlite3_stmt* stmt = nullptr;
    int rc = g_sql.prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK || !stmt)
        return ent;

    // Bind the asset name parameter
    g_sql.bind_text(stmt, 1, name.c_str(), -1, SQLITE_STATIC);

    // Execute query
    rc = g_sql.step(stmt);
    if (rc == SQLITE_ROW)
    {
        // Read entity data from query result
        const int id = g_sql.column_int(stmt, 0);
        const unsigned char* nameU8 = g_sql.column_text(stmt, 1);
        const int seqNum = g_sql.column_int(stmt, 2);
        const int parentID = (g_sql.column_type(stmt, 3) == SQLITE_NULL)
            ? -1
            : g_sql.column_int(stmt, 3);
        const int gdfID = g_sql.column_int(stmt, 4);
        const int gdtID = g_sql.column_int(stmt, 5);
        const int lineNum = g_sql.column_int(stmt, 6);
        const int bExport = g_sql.column_int(stmt, 7);
        const unsigned char* parentNameU8 = g_sql.column_text(stmt, 8);

        // Construct entity object
        ent = entity(id,
            reinterpret_cast<const char*>(nameU8 ? nameU8 : (const unsigned char*)""),
            seqNum, parentID, gdfID, gdtID, lineNum);
        ent.bExport = bExport;
        ent.parentName = reinterpret_cast<const char*>(parentNameU8 ? parentNameU8 : (const unsigned char*)"");
    }

    g_sql.finalize(stmt);
    return ent;
}

GDTDB_API std::vector<std::string> GetEntityNamesByType(const std::string& gdfName)
{
    std::vector<std::string> results;

    sqlite3* db = GdtDB_Get();
    if (!db)
        return results;

    // Query to get all entity names for a given GDF type
    const char* sql =
        "SELECT e.name FROM _entity e "
        "INNER JOIN _gdf g ON e.FK_gdf = g.PK_id "
        "WHERE g.name = ? "
        "ORDER BY e.name;";

    sqlite3_stmt* stmt = nullptr;
    int rc = g_sql.prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK || !stmt)
        return results;

    // Bind the GDF name parameter
    g_sql.bind_text(stmt, 1, gdfName.c_str(), -1, SQLITE_STATIC);

    // Execute query and collect results
    while ((rc = g_sql.step(stmt)) == SQLITE_ROW) {
        const unsigned char* nameU8 = g_sql.column_text(stmt, 0);
        if (nameU8) {
            results.push_back(reinterpret_cast<const char*>(nameU8));
        }
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
        // Check if this is a derived asset (has a parent)
        if (!ent.parentName.empty()) {
            // Derived asset: use brackets with parent name
            file << "\t\"" << name << "\" [ \"" << ent.parentName << "\" ]\n\t{\n";
        } else {
            // Regular asset: use parentheses with GDF type
            file << "\t\"" << name << "\" ( \"" << ent.gdfName << ".gdf\" )\n\t{\n";
        }

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

GDTDB_API bool WriteAssetToGDT(const std::string& assetName, const std::string& gdtPath,
                                const std::map<std::string, std::string>& properties) {
    if (assetName.empty() || gdtPath.empty())
        return false;

    // Read the entire GDT file
    std::ifstream inFile(gdtPath);
    if (!inFile.is_open())
        return false;

    std::string content((std::istreambuf_iterator<char>(inFile)),
                         std::istreambuf_iterator<char>());
    inFile.close();

    // Find the asset entry: "assetname" ( for regular assets or "assetname" [ for derived assets
    std::string assetPattern = "\"" + assetName + "\" (";
    size_t assetStart = content.find(assetPattern);
    char closingChar = ')';  // Track which closing char to look for

    if (assetStart == std::string::npos) {
        // Try alternate format without space (regular asset)
        assetPattern = "\"" + assetName + "\"(";
        assetStart = content.find(assetPattern);
    }

    if (assetStart == std::string::npos) {
        // Try derived asset format with brackets
        assetPattern = "\"" + assetName + "\" [";
        assetStart = content.find(assetPattern);
        closingChar = ']';
    }

    if (assetStart == std::string::npos) {
        // Try alternate derived format without space
        assetPattern = "\"" + assetName + "\"[";
        assetStart = content.find(assetPattern);
        closingChar = ']';
    }

    if (assetStart == std::string::npos)
        return false;

    // Find the closing paren/bracket after the asset type
    size_t parenClose = content.find(closingChar, assetStart);
    if (parenClose == std::string::npos)
        return false;

    // Find the opening brace for properties
    size_t braceStart = content.find('{', parenClose);
    if (braceStart == std::string::npos)
        return false;

    // Find the matching closing brace
    int braceCount = 1;
    size_t braceEnd = braceStart + 1;
    while (braceEnd < content.size() && braceCount > 0) {
        if (content[braceEnd] == '{')
            braceCount++;
        else if (content[braceEnd] == '}')
            braceCount--;
        braceEnd++;
    }

    if (braceCount != 0)
        return false;

    // Build the new properties block (sorted case-insensitively)
    std::vector<std::pair<std::string, std::string>> sorted(properties.begin(), properties.end());
    std::sort(sorted.begin(), sorted.end(),
        [](const auto& a, const auto& b) {
            const std::string al = ToLowerASCII(a.first);
            const std::string bl = ToLowerASCII(b.first);
            if (al != bl) return al < bl;
            return a.first < b.first;
        });

    std::string newProps = "{\n";
    for (const auto& [propName, propValue] : sorted) {
        // Skip internal/empty keys
        if (propName.empty() || propName[0] == '_' || propName == "PK_id")
            continue;
        newProps += "\t\t\"" + propName + "\" \"" + propValue + "\"\n";
    }
    newProps += "\t}";

    // Replace the old properties block with the new one
    std::string before = content.substr(0, braceStart);
    std::string after = content.substr(braceEnd);
    content = before + newProps + after;

    // Write the file back
    std::ofstream outFile(gdtPath, std::ios::trunc);
    if (!outFile.is_open())
        return false;

    outFile << content;
    outFile.close();

    return true;
}

GDTDB_API bool RenameAssetInGDT(const std::string& oldName, const std::string& newName,
                                 const std::string& gdtPath, const std::map<std::string, std::string>& properties) {
    if (oldName.empty() || newName.empty() || gdtPath.empty())
        return false;

    // Read the entire GDT file
    std::ifstream inFile(gdtPath);
    if (!inFile.is_open())
        return false;

    std::string content((std::istreambuf_iterator<char>(inFile)),
                         std::istreambuf_iterator<char>());
    inFile.close();

    // Find the asset entry by OLD name: "oldname" ( or "oldname" [
    std::string assetPattern = "\"" + oldName + "\" (";
    size_t assetStart = content.find(assetPattern);
    char closingChar = ')';

    if (assetStart == std::string::npos) {
        assetPattern = "\"" + oldName + "\"(";
        assetStart = content.find(assetPattern);
    }

    if (assetStart == std::string::npos) {
        assetPattern = "\"" + oldName + "\" [";
        assetStart = content.find(assetPattern);
        closingChar = ']';
    }

    if (assetStart == std::string::npos) {
        assetPattern = "\"" + oldName + "\"[";
        assetStart = content.find(assetPattern);
        closingChar = ']';
    }

    if (assetStart == std::string::npos)
        return false;

    // Find where the old name ends (the closing quote)
    size_t nameStart = assetStart + 1;  // Skip opening quote
    size_t nameEnd = content.find('"', nameStart);
    if (nameEnd == std::string::npos)
        return false;

    // Replace old name with new name
    content.replace(nameStart, nameEnd - nameStart, newName);

    // Now find the properties block (recalculate positions after name change)
    // Try both regular and derived asset patterns
    assetPattern = "\"" + newName + "\" (";
    assetStart = content.find(assetPattern);
    closingChar = ')';

    if (assetStart == std::string::npos) {
        assetPattern = "\"" + newName + "\"(";
        assetStart = content.find(assetPattern);
    }

    if (assetStart == std::string::npos) {
        assetPattern = "\"" + newName + "\" [";
        assetStart = content.find(assetPattern);
        closingChar = ']';
    }

    if (assetStart == std::string::npos) {
        assetPattern = "\"" + newName + "\"[";
        assetStart = content.find(assetPattern);
        closingChar = ']';
    }

    if (assetStart == std::string::npos)
        return false;

    // Find the closing paren/bracket after the asset type
    size_t parenClose = content.find(closingChar, assetStart);
    if (parenClose == std::string::npos)
        return false;

    // Find the opening brace for properties
    size_t braceStart = content.find('{', parenClose);
    if (braceStart == std::string::npos)
        return false;

    // Find the matching closing brace
    int braceCount = 1;
    size_t braceEnd = braceStart + 1;
    while (braceEnd < content.size() && braceCount > 0) {
        if (content[braceEnd] == '{')
            braceCount++;
        else if (content[braceEnd] == '}')
            braceCount--;
        braceEnd++;
    }

    if (braceCount != 0)
        return false;

    // Build the new properties block (sorted case-insensitively)
    std::vector<std::pair<std::string, std::string>> sorted(properties.begin(), properties.end());
    std::sort(sorted.begin(), sorted.end(),
        [](const auto& a, const auto& b) {
            const std::string al = ToLowerASCII(a.first);
            const std::string bl = ToLowerASCII(b.first);
            if (al != bl) return al < bl;
            return a.first < b.first;
        });

    std::string newProps = "{\n";
    for (const auto& [propName, propValue] : sorted) {
        // Skip internal/empty keys
        if (propName.empty() || propName[0] == '_' || propName == "PK_id")
            continue;
        newProps += "\t\t\"" + propName + "\" \"" + propValue + "\"\n";
    }
    newProps += "\t}";

    // Replace the old properties block with the new one
    std::string before = content.substr(0, braceStart);
    std::string after = content.substr(braceEnd);
    content = before + newProps + after;

    // Write the file back
    std::ofstream outFile(gdtPath, std::ios::trunc);
    if (!outFile.is_open())
        return false;

    outFile << content;
    outFile.close();

    return true;
}

// ============================================================================
// Launch gdtdb.exe /update
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

    STARTUPINFOA si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    BOOL ok = CreateProcessA(
        nullptr,
        cmd.data(),
        nullptr, nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr, nullptr,
        &si, &pi
    );

    if (!ok) {
        return false;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return exitCode == 0;
}

// ============================================================================
// Get all GDF type names (asset types)
// ============================================================================
GDTDB_API std::vector<std::string> GetGDFTypes() {
    std::vector<std::string> results;

    sqlite3* db = GdtDB_Get();
    if (!db) return results;

    const char* sql = "SELECT name FROM _gdf ORDER BY name;";
    sqlite3_stmt* stmt = nullptr;

    int rc = g_sql.prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return results;

    while ((rc = g_sql.step(stmt)) == SQLITE_ROW) {
        const unsigned char* nameU8 = g_sql.column_text(stmt, 0);
        if (nameU8) {
            results.emplace_back(reinterpret_cast<const char*>(nameU8));
        }
    }

    g_sql.finalize(stmt);
    return results;
}

// ============================================================================
// Get default property values for a GDF type from _meta table
// ============================================================================
GDTDB_API std::map<std::string, std::string> GetGDFDefaultProperties(const std::string& gdfName) {
    std::map<std::string, std::string> results;

    sqlite3* db = GdtDB_Get();
    if (!db) return results;

    // Query _meta table for default values
    const char* sql = "SELECT _key, _default FROM _meta WHERE _name = ? ORDER BY _index;";
    sqlite3_stmt* stmt = nullptr;

    int rc = g_sql.prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return results;

    g_sql.bind_text(stmt, 1, gdfName.c_str(), -1, SQLITE_STATIC);

    while ((rc = g_sql.step(stmt)) == SQLITE_ROW) {
        const unsigned char* keyU8 = g_sql.column_text(stmt, 0);
        const unsigned char* valueU8 = g_sql.column_text(stmt, 1);

        if (keyU8) {
            std::string key = reinterpret_cast<const char*>(keyU8);
            std::string value = valueU8 ? reinterpret_cast<const char*>(valueU8) : "";
            results[key] = value;
        }
    }

    g_sql.finalize(stmt);
    return results;
}

// ============================================================================
// Create a new asset with default values and append to GDT file
// ============================================================================
GDTDB_API bool CreateNewAsset(const std::string& assetName, const std::string& gdfName, const std::string& gdtPath) {
    if (assetName.empty() || gdfName.empty() || gdtPath.empty())
        return false;

    // Check if asset already exists
    entity existing = FindEntityByName(assetName);
    if (existing.id != -1) {
        return false; // Asset already exists
    }

    // Get default properties for this GDF type
    std::map<std::string, std::string> defaults = GetGDFDefaultProperties(gdfName);

    // Read the entire GDT file
    std::ifstream inFile(gdtPath);
    if (!inFile.is_open())
        return false;

    std::string content((std::istreambuf_iterator<char>(inFile)),
                         std::istreambuf_iterator<char>());
    inFile.close();

    // Find the last closing brace (the end of the GDT)
    size_t lastBrace = content.rfind('}');
    if (lastBrace == std::string::npos)
        return false;

    // Build the new asset entry
    std::string newEntry = "\t\"" + assetName + "\" ( \"" + gdfName + ".gdf\" )\n\t{\n";

    // Sort properties case-insensitively
    std::vector<std::pair<std::string, std::string>> sorted(defaults.begin(), defaults.end());
    std::sort(sorted.begin(), sorted.end(),
        [](const auto& a, const auto& b) {
            const std::string al = ToLowerASCII(a.first);
            const std::string bl = ToLowerASCII(b.first);
            if (al != bl) return al < bl;
            return a.first < b.first;
        });

    for (const auto& [propName, propValue] : sorted) {
        // Skip internal keys
        if (propName.empty() || propName[0] == '_')
            continue;
        newEntry += "\t\t\"" + propName + "\" \"" + propValue + "\"\n";
    }

    newEntry += "\t}\n";

    // Insert the new entry before the last closing brace
    std::string before = content.substr(0, lastBrace);
    std::string after = content.substr(lastBrace);
    content = before + newEntry + after;

    // Write the file back
    std::ofstream outFile(gdtPath, std::ios::trunc);
    if (!outFile.is_open())
        return false;

    outFile << content;
    outFile.close();

    return true;
}

// ============================================================================
// Append a new asset with given properties to GDT file
// ============================================================================
GDTDB_API bool AppendAssetToGDT(const std::string& assetName, const std::string& gdfName,
                                 const std::string& gdtPath, const std::map<std::string, std::string>& properties) {
    if (assetName.empty() || gdfName.empty() || gdtPath.empty())
        return false;

    // Read the entire GDT file
    std::ifstream inFile(gdtPath);
    if (!inFile.is_open())
        return false;

    std::string content((std::istreambuf_iterator<char>(inFile)),
                         std::istreambuf_iterator<char>());
    inFile.close();

    // Find the last closing brace (the end of the GDT)
    size_t lastBrace = content.rfind('}');
    if (lastBrace == std::string::npos)
        return false;

    // Build the new asset entry
    std::string newEntry = "\t\"" + assetName + "\" ( \"" + gdfName + ".gdf\" )\n\t{\n";

    // Sort properties case-insensitively
    std::vector<std::pair<std::string, std::string>> sorted(properties.begin(), properties.end());
    std::sort(sorted.begin(), sorted.end(),
        [](const auto& a, const auto& b) {
            const std::string al = ToLowerASCII(a.first);
            const std::string bl = ToLowerASCII(b.first);
            if (al != bl) return al < bl;
            return a.first < b.first;
        });

    for (const auto& [propName, propValue] : sorted) {
        // Skip internal keys
        if (propName.empty() || propName[0] == '_')
            continue;
        newEntry += "\t\t\"" + propName + "\" \"" + propValue + "\"\n";
    }

    newEntry += "\t}\n";

    // Insert the new entry before the last closing brace
    std::string before = content.substr(0, lastBrace);
    std::string after = content.substr(lastBrace);
    content = before + newEntry + after;

    // Write the file back
    std::ofstream outFile(gdtPath, std::ios::trunc);
    if (!outFile.is_open())
        return false;

    outFile << content;
    outFile.close();

    return true;
}

// ============================================================================
// Delete an asset from a GDT file
// ============================================================================
GDTDB_API bool DeleteAssetFromGDT(const std::string& assetName, const std::string& gdtPath) {
    if (assetName.empty() || gdtPath.empty())
        return false;

    // Read the entire GDT file
    std::ifstream inFile(gdtPath);
    if (!inFile.is_open())
        return false;

    std::string content((std::istreambuf_iterator<char>(inFile)),
                         std::istreambuf_iterator<char>());
    inFile.close();

    // Detect line ending style used in file
    std::string lineEnding = "\n";
    if (content.find("\r\n") != std::string::npos)
        lineEnding = "\r\n";

    // Find the asset entry: "assetname" ( or "assetname" [
    std::string assetPattern = "\"" + assetName + "\" (";
    size_t assetStart = content.find(assetPattern);
    char closingChar = ')';

    if (assetStart == std::string::npos) {
        assetPattern = "\"" + assetName + "\"(";
        assetStart = content.find(assetPattern);
    }

    if (assetStart == std::string::npos) {
        assetPattern = "\"" + assetName + "\" [";
        assetStart = content.find(assetPattern);
        closingChar = ']';
    }

    if (assetStart == std::string::npos) {
        assetPattern = "\"" + assetName + "\"[";
        assetStart = content.find(assetPattern);
        closingChar = ']';
    }

    if (assetStart == std::string::npos)
        return false;  // Asset not found in file

    // Find the closing paren/bracket after the asset type
    size_t parenClose = content.find(closingChar, assetStart);
    if (parenClose == std::string::npos)
        return false;

    // Find the opening brace for properties
    size_t braceStart = content.find('{', parenClose);
    if (braceStart == std::string::npos)
        return false;

    // Find the matching closing brace
    int braceCount = 1;
    size_t braceEnd = braceStart + 1;
    while (braceEnd < content.size() && braceCount > 0) {
        if (content[braceEnd] == '{')
            braceCount++;
        else if (content[braceEnd] == '}')
            braceCount--;
        braceEnd++;
    }

    if (braceCount != 0)
        return false;

    // braceEnd now points to the position right after the closing '}'
    // The asset entry spans from the line containing "assetname" to the line containing '}'

    // Find the start of this asset's line by scanning back to find a newline
    size_t entryStart = assetStart;
    while (entryStart > 0) {
        char c = content[entryStart - 1];
        if (c == '\n')
            break;
        entryStart--;
    }
    // entryStart now points to the first character of the asset's line (typically a tab)

    // Find the end of the closing brace's line
    size_t entryEnd = braceEnd;
    // Skip any characters on the same line as the closing brace (there shouldn't be any)
    while (entryEnd < content.size() && content[entryEnd] != '\n' && content[entryEnd] != '\r')
        entryEnd++;
    // Consume the line ending
    if (entryEnd < content.size() && content[entryEnd] == '\r')
        entryEnd++;
    if (entryEnd < content.size() && content[entryEnd] == '\n')
        entryEnd++;

    // Build the result
    std::string before = content.substr(0, entryStart);
    std::string after = content.substr(entryEnd);

    // Check if we're deleting the first asset (before would end with just "{" or "{\r\n" etc.)
    // We need to ensure the opening brace is followed by a newline
    bool isFirstAsset = false;
    size_t checkPos = before.size();
    // Skip back past any whitespace/newlines
    while (checkPos > 0 && (before[checkPos - 1] == ' ' || before[checkPos - 1] == '\t' ||
                             before[checkPos - 1] == '\r' || before[checkPos - 1] == '\n'))
        checkPos--;
    if (checkPos > 0 && before[checkPos - 1] == '{')
        isFirstAsset = true;

    // Ensure proper formatting
    if (isFirstAsset) {
        // Trim 'before' to just the opening brace and proper line ending
        size_t openBrace = before.rfind('{');
        if (openBrace != std::string::npos) {
            before = before.substr(0, openBrace + 1) + lineEnding;
        }
    }

    // Ensure 'after' starts with proper indentation if there's content
    // (it should already have the tab from the next asset's line)

    content = before + after;

    // Write the file back using binary mode to preserve line endings
    std::ofstream outFile(gdtPath, std::ios::trunc | std::ios::binary);
    if (!outFile.is_open())
        return false;

    outFile << content;
    outFile.close();

    return true;
}

// ============================================================================
// Find entity by database ID
// ============================================================================
GDTDB_API entity FindEntityById(int entityId)
{
    entity ent;

    sqlite3* db = GdtDB_Get();
    if (!db)
        return ent;

    const char* sql =
        "SELECT e.PK_id, e.name, e.iGdtSeqNum, e.FK_parent_id, e.FK_gdf, e.FK_gdt, e._gdt_linenum, e.bExport, "
        "p.name AS parent_name "
        "FROM _entity e "
        "LEFT JOIN _entity p ON e.FK_parent_id = p.PK_id "
        "WHERE e.PK_id = ? LIMIT 1;";

    sqlite3_stmt* stmt = nullptr;
    int rc = g_sql.prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK || !stmt)
        return ent;

    g_sql.bind_int(stmt, 1, entityId);

    rc = g_sql.step(stmt);
    if (rc == SQLITE_ROW) {
        const int id = g_sql.column_int(stmt, 0);
        const unsigned char* nameU8 = g_sql.column_text(stmt, 1);
        const int seqNum = g_sql.column_int(stmt, 2);
        const int parentID = (g_sql.column_type(stmt, 3) == SQLITE_NULL) ? -1 : g_sql.column_int(stmt, 3);
        const int gdfID = g_sql.column_int(stmt, 4);
        const int gdtID = g_sql.column_int(stmt, 5);
        const int lineNum = g_sql.column_int(stmt, 6);
        const int bExport = g_sql.column_int(stmt, 7);
        const unsigned char* parentNameU8 = g_sql.column_text(stmt, 8);

        ent = entity(id,
            reinterpret_cast<const char*>(nameU8 ? nameU8 : (const unsigned char*)""),
            seqNum, parentID, gdfID, gdtID, lineNum);
        ent.bExport = bExport;
        ent.parentName = reinterpret_cast<const char*>(parentNameU8 ? parentNameU8 : (const unsigned char*)"");
    }

    g_sql.finalize(stmt);
    return ent;
}

// ============================================================================
// Get parent entity name for a derived asset
// ============================================================================
GDTDB_API std::string GetParentEntityName(const entity& ent)
{
    if (ent.parentId <= 0)
        return "";

    entity parent = FindEntityById(ent.parentId);
    return parent.name;
}

// ============================================================================
// Get resolved properties (merges parent properties with child overrides)
// ============================================================================
GDTDB_API std::map<std::string, std::string> GetResolvedEntityProperties(const entity& ent)
{
    std::map<std::string, std::string> resolved;

    // Build inheritance chain (child -> parent -> grandparent -> ...)
    std::vector<entity> chain;
    entity current = ent;
    while (current.id != -1) {
        chain.push_back(current);
        if (current.parentId <= 0)
            break;
        current = FindEntityById(current.parentId);
    }

    // Apply properties from root (oldest ancestor) to leaf (the entity itself)
    // Later properties override earlier ones
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        auto props = GetEntityProperties(*it);
        for (const auto& [key, value] : props) {
            resolved[key] = value;
        }
    }

    return resolved;
}

// ============================================================================
// Create a new derived asset
// ============================================================================
GDTDB_API bool CreateDerivedAsset(const std::string& assetName, const std::string& parentAssetName,
                                   const std::string& gdtPath) {
    if (assetName.empty() || parentAssetName.empty() || gdtPath.empty())
        return false;

    // Check if asset already exists
    entity existing = FindEntityByName(assetName);
    if (existing.id != -1)
        return false; // Asset already exists

    // Read the entire GDT file
    std::ifstream inFile(gdtPath);
    if (!inFile.is_open())
        return false;

    std::string content((std::istreambuf_iterator<char>(inFile)),
                         std::istreambuf_iterator<char>());
    inFile.close();

    // Find the last closing brace (the end of the GDT)
    size_t lastBrace = content.rfind('}');
    if (lastBrace == std::string::npos)
        return false;

    // Build the new derived asset entry (empty properties - inherits all from parent)
    std::string newEntry = "\t\"" + assetName + "\" [ \"" + parentAssetName + "\" ]\n\t{\n\t}\n";

    // Insert the new entry before the last closing brace
    std::string before = content.substr(0, lastBrace);
    std::string after = content.substr(lastBrace);
    content = before + newEntry + after;

    // Write the file back
    std::ofstream outFile(gdtPath, std::ios::trunc);
    if (!outFile.is_open())
        return false;

    outFile << content;
    outFile.close();

    return true;
}

// ============================================================================
// Append a derived asset with given properties to GDT file
// ============================================================================
GDTDB_API bool AppendDerivedAssetToGDT(const std::string& assetName, const std::string& parentAssetName,
                                        const std::string& gdtPath, const std::map<std::string, std::string>& properties) {
    if (assetName.empty() || parentAssetName.empty() || gdtPath.empty())
        return false;

    // Read the entire GDT file
    std::ifstream inFile(gdtPath);
    if (!inFile.is_open())
        return false;

    std::string content((std::istreambuf_iterator<char>(inFile)),
                         std::istreambuf_iterator<char>());
    inFile.close();

    // Find the last closing brace (the end of the GDT)
    size_t lastBrace = content.rfind('}');
    if (lastBrace == std::string::npos)
        return false;

    // Build the new derived asset entry
    std::string newEntry = "\t\"" + assetName + "\" [ \"" + parentAssetName + "\" ]\n\t{\n";

    // Sort properties case-insensitively
    std::vector<std::pair<std::string, std::string>> sorted(properties.begin(), properties.end());
    std::sort(sorted.begin(), sorted.end(),
        [](const auto& a, const auto& b) {
            const std::string al = ToLowerASCII(a.first);
            const std::string bl = ToLowerASCII(b.first);
            if (al != bl) return al < bl;
            return a.first < b.first;
        });

    for (const auto& [propName, propValue] : sorted) {
        // Skip internal keys
        if (propName.empty() || propName[0] == '_')
            continue;
        newEntry += "\t\t\"" + propName + "\" \"" + propValue + "\"\n";
    }

    newEntry += "\t}\n";

    // Insert the new entry before the last closing brace
    std::string before = content.substr(0, lastBrace);
    std::string after = content.substr(lastBrace);
    content = before + newEntry + after;

    // Write the file back
    std::ofstream outFile(gdtPath, std::ios::trunc);
    if (!outFile.is_open())
        return false;

    outFile << content;
    outFile.close();

    return true;
}

// ============================================================================
// Launch gdtdb.exe /rebuild
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

    STARTUPINFOA si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    BOOL ok = CreateProcessA(
        nullptr,
        cmd.data(),
        nullptr, nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr, nullptr,
        &si, &pi
    );

    if (!ok) {
        return false;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return exitCode == 0;
}