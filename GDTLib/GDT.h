#pragma once

#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "XAsset.h"

class gdt
{
public:
	std::string Name;
	std::string RelPath;
	std::string AbsPath;
	std::string Hash; // placeholder for later

	// Group assets by type, then list of assets for that type
	std::map<XType, std::vector<xasset>> XAssetsByType;

	// Fast lookup by name -> (type, index in vector)
	std::unordered_map<std::string, std::pair<XType, std::size_t>> IndexByName;

	explicit gdt(std::string nameInput)
		: Name(std::move(nameInput)),
		  RelPath(std::string("source_data\\") + Name + ".gdt") {}

	// Build AbsPath using TA_TOOLS_PATH; prints an error if missing
	std::string GetAbsPath()
	{
		char *ta_env = nullptr;
		size_t len = 0;
		if (_dupenv_s(&ta_env, &len, "TA_TOOLS_PATH") == 0 && ta_env)
		{
			AbsPath = std::string(ta_env) + "\\" + RelPath;
			free(ta_env); // must free what _dupenv_s allocates
			return AbsPath;
		}

		if (ta_env)
		{
			AbsPath = std::string(ta_env) + "\\" + RelPath;
			return AbsPath;
		}
		std::cerr << "Error: TA_TOOLS_PATH environment variable not set.\n";
		AbsPath.clear();
		return AbsPath;
	}

	// Insert or replace by name. Returns true if inserted new, false if replaced.
	bool AddXAsset(const xasset &asset)
	{
		if (asset.Name.empty())
		{
			std::cerr << "Error: xasset has empty Name; cannot add to GDT.\n";
			return false;
		}

		std::unordered_map<std::string, std::pair<XType, std::size_t>>::iterator it =
			IndexByName.find(asset.Name);

		if (it != IndexByName.end())
		{
			// Replace existing (may also migrate type if changed)
			XType oldType = it->second.first;
			std::size_t idx = it->second.second;

			if (oldType == asset.Type)
			{
				XAssetsByType[oldType][idx] = asset;
				return false; // replaced in-place
			}
			else
			{
				// Remove from old type vector by swap-pop, update moved index
				std::vector<xasset> &vecOld = XAssetsByType[oldType];
				if (idx < vecOld.size())
				{
					bool isLast = (idx == vecOld.size() - 1);
					if (!isLast)
					{
						vecOld[idx] = vecOld.back();
						// Fix index of the element we just moved into idx
						IndexByName[vecOld[idx].Name] = std::make_pair(oldType, idx);
					}
					vecOld.pop_back();
				}
				// Add into new type vector
				std::vector<xasset> &vecNew = XAssetsByType[asset.Type];
				vecNew.push_back(asset);
				it->second = std::make_pair(asset.Type, vecNew.size() - 1);
				return false; // treated as replace (name existed)
			}
		}

		// Fresh insert
		std::vector<xasset> &vec = XAssetsByType[asset.Type];
		vec.push_back(asset);
		IndexByName.emplace(asset.Name, std::make_pair(asset.Type, vec.size() - 1));
		return true;
	}

	// Lookup by name; pointer valid until vector reallocation for that type
	xasset *GetXAsset(const std::string &name)
	{
		std::unordered_map<std::string, std::pair<XType, std::size_t>>::const_iterator it =
			IndexByName.find(name);
		if (it == IndexByName.end())
			return nullptr;

		XType t = it->second.first;
		std::size_t idx = it->second.second;

		std::map<XType, std::vector<xasset>>::iterator mapIt = XAssetsByType.find(t);
		if (mapIt == XAssetsByType.end())
			return nullptr;

		std::vector<xasset> &vec = mapIt->second;
		return (idx < vec.size()) ? &vec[idx] : nullptr;
	}

	// Readonly access to list by type
	const std::vector<xasset> *GetXAssets(XType type) const
	{
		std::map<XType, std::vector<xasset>>::const_iterator it =
			XAssetsByType.find(type);
		return (it == XAssetsByType.end()) ? nullptr : &it->second;
	}

	// Remove by name; returns true if removed
	bool RemoveByName(const std::string &name)
	{
		std::unordered_map<std::string, std::pair<XType, std::size_t>>::iterator it =
			IndexByName.find(name);
		if (it == IndexByName.end())
			return false;

		XType t = it->second.first;
		std::size_t idx = it->second.second;

		std::map<XType, std::vector<xasset>>::iterator mIt = XAssetsByType.find(t);
		if (mIt == XAssetsByType.end())
		{
			IndexByName.erase(it);
			return false;
		}

		std::vector<xasset> &vec = mIt->second;
		if (idx >= vec.size())
		{
			IndexByName.erase(it);
			return false;
		}

		bool isLast = (idx == vec.size() - 1);
		if (!isLast)
		{
			vec[idx] = vec.back();
			IndexByName[vec[idx].Name] = std::make_pair(t, idx);
		}
		vec.pop_back();
		IndexByName.erase(it);
		return true;
	}

	// Counts
	std::size_t Count(XType t) const
	{
		std::map<XType, std::vector<xasset>>::const_iterator it =
			XAssetsByType.find(t);
		return (it == XAssetsByType.end()) ? 0 : it->second.size();
	}

	std::size_t TotalCount() const
	{
		std::size_t n = 0;
		for (std::map<XType, std::vector<xasset>>::const_iterator it = XAssetsByType.begin();
			 it != XAssetsByType.end(); ++it)
		{
			n += it->second.size();
		}
		return n;
	}

	// Debug string
	std::string ToString() const { return "GDT file '" + Name + "'"; }
};
