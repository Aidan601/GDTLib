# GDTLib

A C++ DLL library for reading and writing Call of Duty Black Ops III Game Definition Table (GDT) databases.

## Overview

GDTLib provides a high-level API for interacting with the BO3 mod tools SQLite database (`gdt.db`). It handles entity/asset queries, property retrieval, GDT file I/O, and supports both regular and derived (inherited) assets.

## Requirements

- **Black Ops III Mod Tools** - Must be installed and `TA_TOOLS_PATH` environment variable must point to the installation directory
- **Platform**: Windows x64
- **Build System**: Visual Studio 2022 (MSVC v145 toolset)
- **Language Standard**: C++17

## Build

```bash
# Command line
msbuild GDTLib.vcxproj /p:Configuration=Debug /p:Platform=x64
msbuild GDTLib.vcxproj /p:Configuration=Release /p:Platform=x64

# Or open GDTLib.sln in Visual Studio 2022 and build (Ctrl+Shift+B)
```

Build outputs:

- `x64/Debug/GDTLib.dll` - Debug DLL
- `x64/Debug/GDTLib.lib` - Import library

The post-build step automatically copies headers, DLL, and lib to the APEx project directory.

## API Reference

### Initialization

```cpp
#include "GDTLib.h"

// Initialize the database connection (reads from TA_TOOLS_PATH\gdtdb\gdt.db)
int result = GdtDB_Init();  // Returns SQLITE_OK (0) on success

// Get raw SQLite handle (for advanced queries)
sqlite3* db = GdtDB_Get();

// Shutdown and close database
GdtDB_Shutdown();
```

### Path Helpers

```cpp
const char* root = GetRootPath();      // Returns TA_TOOLS_PATH value
const char* dbPath = GetGdtDbPath();   // Returns full path to gdt.db
```

### GDT Queries

```cpp
// Get all GDTs (Game Definition Tables)
std::map<std::string, gdt> gdts = GetGDTs();

// Get entities for a specific GDT
std::map<std::string, entity> entities = GetEntities(myGdt);

// Get all entities
std::map<std::string, entity> allEntities = GetEntities();

// Get entities grouped by GDT ID (for batch processing)
std::unordered_map<int, std::map<std::string, entity>> grouped = GetEntitiesByGdt();
```

### Entity Queries

```cpp
// Find entity by name
entity ent = FindEntityByName("my_weapon");

// Find entity by database ID
entity ent = FindEntityById(12345);

// Get entity names by asset type
std::vector<std::string> weapons = GetEntityNamesByType("bulletweapon");

// Get entity properties
std::map<std::string, std::string> props = GetEntityProperties(ent);
```

### Asset Type (GDF) Queries

```cpp
// Get all GDF type names (115 asset types)
std::vector<std::string> types = GetGDFTypes();

// Get default property values for a GDF type
std::map<std::string, std::string> defaults = GetGDFDefaultProperties("material");
```

### File I/O

```cpp
// Export entire GDT to file
bool success = WriteGDTToFile(myGdt);

// Update asset properties in GDT file
std::map<std::string, std::string> props = { {"damage", "100"}, {"fireRate", "600"} };
bool success = WriteAssetToGDT("my_weapon", "path/to/my.gdt", props);

// Create new asset with default values
bool success = CreateNewAsset("new_weapon", "bulletweapon", "path/to/my.gdt");

// Append asset with specific properties
bool success = AppendAssetToGDT("new_weapon", "bulletweapon", "path/to/my.gdt", props);

// Rename asset
bool success = RenameAssetInGDT("old_name", "new_name", "path/to/my.gdt", props);

// Delete asset
bool success = DeleteAssetFromGDT("my_weapon", "path/to/my.gdt");
```

### Derived Assets (Inheritance)

Derived assets inherit properties from a parent asset, only storing overridden values.

```cpp
// Get parent entity name
std::string parentName = GetParentEntityName(derivedEntity);

// Get resolved properties (merges entire inheritance chain)
std::map<std::string, std::string> resolved = GetResolvedEntityProperties(derivedEntity);

// Create derived asset (inherits all properties from parent)
bool success = CreateDerivedAsset("variant_weapon", "base_weapon", "path/to/my.gdt");

// Create derived asset with property overrides
std::map<std::string, std::string> overrides = { {"damage", "150"} };
bool success = AppendDerivedAssetToGDT("variant_weapon", "base_weapon", "path/to/my.gdt", overrides);
```

### Database Maintenance

```cpp
// Run gdtdb.exe /update (incremental update)
bool success = RunGdtDbUpdate();

// Run gdtdb.exe /rebuild (full rebuild)
bool success = RunGdtDbRebuild();
```

## Data Types

### `gdt` Class

```cpp
class gdt {
    int id;                 // Database primary key
    std::string path;       // Full path to .gdt file
    std::string name;       // Filename without extension
    int timestamp;          // Last modification time
    bool openForEdit;       // Edit lock flag
};
```

### `entity` Class

```cpp
class entity {
    int id;                 // Database primary key
    std::string name;       // Asset name
    int gdtSeqNum;          // Sequence number in GDT
    int parentId;           // Parent entity ID (-1 if none)
    int gdfId;              // Asset type ID (1-115)
    int gdtId;              // GDT ID
    int lineNum;            // Line number in source file
    int bExport;            // Export flag
    std::string gdfName;    // Asset type name (e.g., "bulletweapon")
    std::string parentName; // Parent asset name (empty if none)
};
```

## GDT File Format

Regular assets use parentheses with the GDF type:

```
{
    "my_weapon" ( "bulletweapon.gdf" )
    {
        "damage" "100"
        "fireRate" "600"
    }
}
```

Derived assets use brackets with the parent asset name:

```
{
    "variant_weapon" [ "my_weapon" ]
    {
        "damage" "150"
    }
}
```
