#pragma once

#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <algorithm> // std::remove_if
#include <chrono>	 // time funcs
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cstdlib>      // for _dupenv_s

#include "XAsset.h"

class gdt
{
public:
	std::string Name;
	std::string RelPath;
	int Time;
	std::vector<xasset> Assets;

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

	std::string GetAbsPath() const
	{
		char* ta_env = nullptr;
		size_t len = 0;

		// Safe environment variable access
		if (_dupenv_s(&ta_env, &len, "TA_TOOLS_PATH") != 0 || ta_env == nullptr)
		{
			std::cerr << "Error: TA_TOOLS_PATH environment variable not set.\n";
			return "";
		}

		// Build full path using std::filesystem to ensure proper joining
		std::filesystem::path basePath(ta_env);
		free(ta_env); // must free what _dupenv_s allocates

		std::filesystem::path rel(RelPath);
		std::filesystem::path full = basePath / rel;

		return full.string();
	}

	// Add an asset
	void AddAsset(const xasset &asset)
	{
		Assets.push_back(asset);
	}

	// Remove all assets with a matching name; returns true if anything was removed
	bool RemoveAssetByName(const std::string &assetName)
	{
		auto it = std::remove_if(Assets.begin(), Assets.end(),
								 [&](const xasset &a)
								 { return a.Name == assetName; });
		bool removed = (it != Assets.end());
		if (removed)
			Assets.erase(it, Assets.end());
		return removed;
	}

	// Optional: remove by index (bounds-checked)
	bool RemoveAssetAt(size_t index)
	{
		if (index >= Assets.size())
			return false;
		Assets.erase(Assets.begin() + static_cast<std::ptrdiff_t>(index));
		return true;
	}

	// Find an asset by name (mutable)
	xasset *GetAssetByName(const std::string &assetName)
	{
		for (auto &a : Assets)
			if (a.Name == assetName)
				return &a;
		return nullptr;
	}

	// Find an asset by name (const)
	const xasset *GetAssetByName(const std::string &assetName) const
	{
		for (const auto &a : Assets)
			if (a.Name == assetName)
				return &a;
		return nullptr;
	}

	// Debug helper
	void PrintAssets() const
	{
		std::cout << "Assets in GDT '" << Name << "':\n";
		for (const auto &a : Assets)
			std::cout << " - " << a.Name << " (" << a.Type << ")\n";
	}

	// Write a minimal GDT file with placeholder content
	bool WriteGDTFile() const
	{
		namespace fs = std::filesystem;
		try
		{
			// Determine absolute path (using RelPath)
			fs::path path = GetAbsPath();

			// Ensure parent directories exist
			if (path.has_parent_path())
				fs::create_directories(path.parent_path());

			std::ofstream out(path, std::ios::out | std::ios::trunc);
			if (!out.is_open())
			{
				std::cerr << "Error: Failed to open file for writing: " << path << "\n";
				return false;
			}

			// Write minimal content (placeholder)
			std::cout << Assets.size() << "\n";
			for (int i = 0; i < Assets.size(); ++i)
				out << (i == 0 ? "{\n\t\"" : ",\n\t\"") << Assets[i].Name << "\"";	

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
