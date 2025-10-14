#ifndef XASSET_H
#define XASSET_H

#include <iostream>
#include <string>
#include <map>
#include <filesystem>
#include <cstring>     // std::strcmp
#include <sqlite3.h>   // or keep your "db.h" if it includes sqlite3.h
#include "db.h"        // if you need other helpers from your project

class xasset
{
public:
    std::string Name = "";
    std::string Type = "";
    std::map<std::string, std::string> value;

    // Constructor
    xasset(std::string nameInput, std::string typeInput)
    {
        Name = std::move(nameInput);
        Type = std::move(typeInput);
        value = GetInitialValues(Type);
        defaultValues = value; // store defaults for reset later
    }

    // --- Edit an existing key's value ---
    bool EditValue(const std::string& key, const std::string& newValue)
    {
        auto it = value.find(key);
        if (it != value.end())
        {
            it->second = newValue;
            return true;
        }
        else
        {
            std::cerr << "Warning: key '" << key << "' not found in asset '" << Name << "'\n";
            return false;
        }
    }

    // --- Reset a key back to its default ---
    bool ResetValue(const std::string& key)
    {
        auto def = defaultValues.find(key);
        if (def != defaultValues.end())
        {
            value[key] = def->second;
            return true;
        }
        else
        {
            std::cerr << "Warning: default value for key '" << key << "' not found in asset '" << Name << "'\n";
            return false;
        }
    }

    // --- Reset all keys to defaults ---
    void ResetAllValues()
    {
        value = defaultValues;
    }

    // --- Print all key-value pairs ---
    void PrintValues() const
    {
        std::cout << "Asset: " << Name << " (" << Type << ")\n";
        for (const auto& [key, val] : value)
        {
            std::cout << "  " << key << " = \"" << val << "\"\n";
        }
    }

private:
    std::map<std::string, std::string> defaultValues;

    // --- Helper: removes surrounding quotes from a string, if any ---
    static std::string StripQuotes(const char* str)
    {
        if (!str) return "";
        std::string s(str);
        if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
            s = s.substr(1, s.size() - 2);
        return s;
    }

    // --- Callback used by sqlite3_exec. `ctx` carries &dict. ---
    static int SchemaCB(void* ctx, int argc, char** argv, char** /*azColName*/)
    {
        auto* out = static_cast<std::map<std::string, std::string>*>(ctx);

        // PRAGMA table_info columns: cid|name|type|notnull|dflt_value|pk
        const char* name_raw = (argc > 1 ? argv[1] : nullptr);
        if (!name_raw) return 0;

        std::string name = StripQuotes(name_raw);

        // Skip PK and any column starting with '_'
        if (name == "PK_id" || (!name.empty() && name[0] == '_'))
            return 0;

        const char* dflt_raw = (argc > 4 && argv[4]) ? argv[4] : "";
        std::string dflt = StripQuotes(dflt_raw);

        (*out)[name] = dflt;
        return 0;
    }


    std::map<std::string, std::string> GetInitialValues(const std::string& table_name)
    {
        std::map<std::string, std::string> dict;

        // Build DB path
        std::filesystem::path db_path = std::filesystem::path(getGameRoot())
            / "gdtlib" / "gdtlib.db";

        sqlite3* DB = nullptr;
        std::string db_path_str = db_path.string();
        int rc = sqlite3_open_v2(db_path_str.c_str(), &DB, SQLITE_OPEN_READONLY, nullptr);
        if (rc != SQLITE_OK)
        {
            std::cerr << "Error opening DB: "
                << (DB ? sqlite3_errmsg(DB) : "unknown") << '\n';
            if (DB) sqlite3_close(DB);
            return dict;
        }

        std::string sql = "PRAGMA table_info(\"" + table_name + "\");";

        char* err = nullptr;
        rc = sqlite3_exec(DB, sql.c_str(), &xasset::SchemaCB, &dict, &err);
        if (rc != SQLITE_OK)
        {
            std::cerr << "SQL error: " << (err ? err : "unknown") << '\n';
            if (err) sqlite3_free(err);
            sqlite3_close(DB);
            return dict;
        }

        sqlite3_close(DB);
        return dict;
    }

    std::string getGameRoot()
    {
        char* ta_env = nullptr;
        size_t len = 0;
        if (_dupenv_s(&ta_env, &len, "TA_GAME_PATH") != 0 || !ta_env)
        {
            std::cerr << "Error: TA_GAME_PATH environment variable not set.\n";
            return {};
        }
        std::string base(ta_env);
        free(ta_env);
        return base;
    }
};

#endif
