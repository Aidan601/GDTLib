#pragma once
#include <sqlite3.h>
#include <iostream>
#include <string>

// prints rows as "col: value"
static int callback(void *, int argc, char **argv, char **azColName)
{
    for (int i = 0; i < argc; i++)
        std::cout << azColName[i] << ": " << (argv[i] ? argv[i] : "NULL") << '\n';
    std::cout << '\n';
    return 0;
}

static int exec_sql(sqlite3 *DB, const char *sql)
{
    char *err = nullptr;
    int rc = sqlite3_exec(DB, sql, callback, nullptr, &err);
    if (rc != SQLITE_OK)
    {
        std::cerr << "SQL error: " << (err ? err : "unknown") << '\n';
        if (err)
            sqlite3_free(err);
    }
    return rc;
}

// List all tables to confirm you're in the right DB
static int listTables(const char *db_path)
{
    sqlite3 *DB = nullptr;
    int rc = sqlite3_open_v2(db_path, &DB, SQLITE_OPEN_READONLY, nullptr);
    if (rc != SQLITE_OK)
    {
        std::cerr << "Error opening DB: "
                  << (DB ? sqlite3_errmsg(DB) : "unknown") << '\n';
        if (DB)
            sqlite3_close(DB);
        return -1;
    }

    std::cout << "\n-- Tables in DB --\n";
    rc = exec_sql(DB,
                  "SELECT name AS table_name FROM sqlite_master "
                  "WHERE type='table' ORDER BY name;");
    sqlite3_close(DB);
    return (rc == SQLITE_OK) ? 0 : -2;
}

// Count rows and optionally dump some rows from a table
static int selectTable(const char *db_path, const char *table, int limit = 50)
{
    sqlite3 *DB = nullptr;
    int rc = sqlite3_open_v2(db_path, &DB, SQLITE_OPEN_READONLY, nullptr);
    if (rc != SQLITE_OK)
    {
        std::cerr << "Error opening DB: "
                  << (DB ? sqlite3_errmsg(DB) : "unknown") << '\n';
        if (DB)
            sqlite3_close(DB);
        return -1;
    }

    // 1) Show row count
    std::string countSql = "SELECT COUNT(*) AS row_count FROM ";
    countSql += table;
    countSql += ";";

    std::cout << "\n-- Row count for '" << table << "' --\n";
    rc = exec_sql(DB, countSql.c_str());
    if (rc != SQLITE_OK)
    {
        sqlite3_close(DB);
        return -2;
    }

    // 2) Dump first N rows (if any)
    std::string dumpSql = "SELECT * FROM ";
    dumpSql += table;
    dumpSql += " LIMIT ";
    dumpSql += std::to_string(limit);
    dumpSql += ";";

    std::cout << "\n-- First " << limit << " row(s) from '" << table << "' --\n";
    rc = exec_sql(DB, dumpSql.c_str());
    if (rc != SQLITE_OK)
    {
        sqlite3_close(DB);
        return -3;
    }

    sqlite3_close(DB);
    return 0;
}

static int getTableStructure(const char *db_path, const char *table_name)
{
    sqlite3 *DB = nullptr;
    int rc = sqlite3_open_v2(db_path, &DB, SQLITE_OPEN_READONLY, nullptr);
    if (rc != SQLITE_OK)
    {
        std::cerr << "Error opening DB: "
                  << (DB ? sqlite3_errmsg(DB) : "unknown") << '\n';
        if (DB)
            sqlite3_close(DB);
        return -1;
    }

    std::string sql = "PRAGMA table_info(";
    sql += table_name;
    sql += ");";

    std::cout << "\n-- Structure of table '" << table_name << "' --\n";

    auto schemaCallback = [](void *, int argc, char **argv, char **azColName) -> int
    {
        // Expected columns from PRAGMA table_info:
        // cid | name | type | notnull | dflt_value | pk
        for (int i = 0; i < argc; i++)
        {
            std::cout << azColName[i] << ": " << (argv[i] ? argv[i] : "NULL") << '\n';
        }
        std::cout << '\n';
        return 0;
    };

    char *err = nullptr;
    rc = sqlite3_exec(DB, sql.c_str(), schemaCallback, nullptr, &err);
    if (rc != SQLITE_OK)
    {
        std::cerr << "SQL error: " << (err ? err : "unknown") << '\n';
        if (err)
            sqlite3_free(err);
        sqlite3_close(DB);
        return -2;
    }

    sqlite3_close(DB);
    return 0;
}
