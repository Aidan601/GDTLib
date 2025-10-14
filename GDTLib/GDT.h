#pragma once

#include <iostream>
#include <map>
#include <string>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cstdlib> // for _dupenv_s

#include "XAsset.h"

class gdt
{
public:
    std::string Name;
    std::string RelPath;
    int Time;
    std::map<std::string, xasset> Assets; //  Changed from vector to map

    gdt()
    {
        Name = "";
        RelPath = "";
        Time = 0;
        Assets = {};
    }

    gdt(std::string nameInput, std::string relPathInput, int timeInput)
    {
        Name = std::move(nameInput);
        RelPath = std::move(relPathInput);
        Time = timeInput;
        Assets = {};
    }

    // Build full absolute path using TA_TOOLS_PATH
    std::string GetAbsPath() const
    {
        char* ta_env = nullptr;
        size_t len = 0;

        if (_dupenv_s(&ta_env, &len, "TA_TOOLS_PATH") != 0 || ta_env == nullptr)
        {
            std::cerr << "Error: TA_TOOLS_PATH environment variable not set.\n";
            return "";
        }

        std::filesystem::path basePath(ta_env);
        free(ta_env);

        std::filesystem::path rel(RelPath);
        std::filesystem::path full = basePath / rel;

        return full.string();
    }

    //  Add or replace asset
    void AddAsset(const xasset& asset)
    {
        Assets[asset.Name] = asset; // insert or overwrite
    }

    //  Remove asset by name; returns true if removed
    bool RemoveAssetByName(const std::string& assetName)
    {
        return Assets.erase(assetName) > 0;
    }

    //  Get asset by name (mutable)
    xasset* GetAssetByName(const std::string& assetName)
    {
        auto it = Assets.find(assetName);
        return (it != Assets.end()) ? &it->second : nullptr;
    }

    //  Get asset by name (const)
    const xasset* GetAssetByName(const std::string& assetName) const
    {
        auto it = Assets.find(assetName);
        return (it != Assets.end()) ? &it->second : nullptr;
    }

    //  Debug helper
    void PrintAssets() const
    {
        std::cout << "Assets in GDT '" << Name << "':\n";
        for (const auto& [name, a] : Assets)
            std::cout << " - " << name << " (" << a.Type << ")\n";
    }

    //  Write minimal GDT file (placeholder)
    bool WriteGDTFile() const
    {
        namespace fs = std::filesystem;
        try
        {
            fs::path path = GetAbsPath();

            if (path.has_parent_path())
                fs::create_directories(path.parent_path());

            std::ofstream out(path, std::ios::out | std::ios::trunc);
            if (!out.is_open())
            {
                std::cerr << "Error: Failed to open file for writing: " << path << "\n";
                return false;
            }

            out << "{\n";
            for (auto it = Assets.begin(); it != Assets.end(); ++it)
            {
                if (it != Assets.begin()) out << ",\n";
                out << "\t\"" << it->first << "\"";
            }
            out << "\n}\n";

            out.close();
            std::cout << "Wrote minimal GDT to: " << path << "\n";
            return true;
        }
        catch (const std::exception& e)
        {
            std::cerr << "Exception writing GDT file: " << e.what() << "\n";
            return false;
        }
    }
};
