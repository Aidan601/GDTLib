#pragma once
#include <map>
#include <unordered_map>
#include <string>
#include <vector>
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
GDTDB_API entity FindEntityByName(const std::string& name);
GDTDB_API std::vector<std::string> GetEntityNamesByType(const std::string& gdfName);
GDTDB_API std::map<std::string, std::string> GetEntityProperties(const entity& ent);
GDTDB_API bool WriteGDTToFile(const gdt& GDT);
GDTDB_API bool WriteAssetToGDT(const std::string& assetName, const std::string& gdtPath,
                                const std::map<std::string, std::string>& properties);

// Get all GDF type names (asset types)
GDTDB_API std::vector<std::string> GetGDFTypes();

// Get default property values for a GDF type from _meta table
GDTDB_API std::map<std::string, std::string> GetGDFDefaultProperties(const std::string& gdfName);

// Create a new asset with default values and append to GDT file
// Returns true on success, false on failure
GDTDB_API bool CreateNewAsset(const std::string& assetName, const std::string& gdfName, const std::string& gdtPath);

// Append a new asset with given properties to GDT file
// Returns true on success, false on failure
GDTDB_API bool AppendAssetToGDT(const std::string& assetName, const std::string& gdfName,
                                 const std::string& gdtPath, const std::map<std::string, std::string>& properties);

// Rename an asset in a GDT file and update its properties
// Returns true on success, false on failure
GDTDB_API bool RenameAssetInGDT(const std::string& oldName, const std::string& newName,
                                 const std::string& gdtPath, const std::map<std::string, std::string>& properties);

// Delete an asset from a GDT file
// Returns true on success, false on failure
GDTDB_API bool DeleteAssetFromGDT(const std::string& assetName, const std::string& gdtPath);

// Get an entity by its database ID
GDTDB_API entity FindEntityById(int entityId);

// Get the parent entity name for a derived asset
// Returns empty string if the entity has no parent
GDTDB_API std::string GetParentEntityName(const entity& ent);

// Get resolved properties for an entity (merges parent properties with child overrides)
// This walks up the inheritance chain and returns the final property values
GDTDB_API std::map<std::string, std::string> GetResolvedEntityProperties(const entity& ent);

// Create a new derived asset (child of another asset)
// Returns true on success, false on failure
GDTDB_API bool CreateDerivedAsset(const std::string& assetName, const std::string& parentAssetName,
                                   const std::string& gdtPath);

// Append a derived asset with given properties to GDT file
// Only writes properties that differ from the parent
GDTDB_API bool AppendDerivedAssetToGDT(const std::string& assetName, const std::string& parentAssetName,
                                        const std::string& gdtPath, const std::map<std::string, std::string>& properties);

// (Optional) Let callers override where to load sqlite64r.dll from.
// If not called, we default to:  GetRootPath() + "\\bin"
GDTDB_API void GdtDB_SetSqliteBinDir(const wchar_t* bo3BinDirW);