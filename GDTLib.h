// GDTLib.h  (replace the first lines)

#pragma once
// #include <sqlite3.h>           // remove this include from the public header
#include <map>
#include <unordered_map>
#include <string>
#include "gdt.h"
#include "entity.h"
#pragma warning(disable : 4996)

// Forward declarations to avoid forcing sqlite3.h on all consumers:
struct sqlite3;
struct sqlite3_stmt;

#ifndef SQLITE_OK
#define SQLITE_OK 0
#define SQLITE_CANTOPEN 14
#define SQLITE_ROW 100
#define SQLITE_DONE 101
// Column type code:
#define SQLITE_NULL 5
// Open flags used internally by GdtDB_Init()
#define SQLITE_OPEN_READONLY 0x00000001
#define SQLITE_OPEN_NOMUTEX 0x00008000
#endif

#ifdef GDTLIB_EXPORTS
#define GDTDB_API __declspec(dllexport)
#define GDTDB_CAPI extern "C" __declspec(dllexport)
#else
#define GDTDB_API __declspec(dllimport)
#define GDTDB_CAPI extern "C" __declspec(dllimport)
#endif

GDTDB_API const char* GetRootPath();
GDTDB_API const char* GetGdtDbPath();
GDTDB_API bool RunGdtDbUpdate();
GDTDB_API bool RunGdtDbRebuild();

GDTDB_CAPI int GdtDB_Init();
GDTDB_CAPI sqlite3* GdtDB_Get();
GDTDB_CAPI void GdtDB_Shutdown();

GDTDB_API std::map<std::string, gdt> GetGDTs();
GDTDB_API std::map<std::string, entity> GetEntities();
GDTDB_API std::map<std::string, entity> GetEntities(const gdt& GDT);
GDTDB_API std::unordered_map<int, std::map<std::string, entity>> GetEntitiesByGdt();
GDTDB_API std::map<std::string, std::string> GetEntityProperties(const entity& ent);
GDTDB_API bool WriteGDTToFile(const gdt& GDT);

// (Optional) Let callers override where to load sqlite64r.dll from.
// If not called, we default to:  GetRootPath() + "\\bin"
GDTDB_API void GdtDB_SetSqliteBinDir(const wchar_t* bo3BinDirW);