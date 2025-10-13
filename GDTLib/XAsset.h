#pragma once

#include <string>
#include <map>
#include <utility>
#include <sstream>
#include <algorithm>
#include <cctype>

// --- Asset type enum & helpers ------------------------------------------------

enum class XType
{
	Accolade,
	AccoladeList,
	Unknown
};

inline std::string to_string(XType t)
{
	switch (t)
	{
	case XType::Accolade:
		return "accolade";
	case XType::AccoladeList:
		return "accoladelist";
	default:
		return "unknown";
	}
}

inline XType xtype_from_string(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(),
				   [](unsigned char c)
				   { return (char)std::tolower(c); });
	if (s == "accolade")
		return XType::Accolade;
	if (s == "accoladelist")
		return XType::AccoladeList;
	return XType::Unknown;
}

// --- xasset -------------------------------------------------------------------

class xasset
{
public:
	std::string Name;
	XType Type{XType::Unknown};
	std::map<std::string, std::string> value; // loosely-typed field bag

	xasset() = default;

	xasset(std::string nameInput, XType typeInput)
		: Name(std::move(nameInput)), Type(typeInput), value(GetInitialValues(Type)) {}

	// Convenience: construct from string type label (e.g., "accolade")
	xasset(std::string nameInput, const std::string &typeLabel)
		: Name(std::move(nameInput))
	{
		Type = xtype_from_string(typeLabel);
		value = GetInitialValues(Type);
	}

	std::string ToString() const
	{
		std::ostringstream os;
		os << to_string(Type) << " asset '" << Name << "'";
		return os.str();
	}

private:
	static std::map<std::string, std::string> GetInitialValues(XType type)
	{
		std::map<std::string, std::string> dict;

		switch (type)
		{
		case XType::Accolade:
		{
			dict["image_rewardImage"] = "";
			dict["vmType"] = "None";
			dict["xstring_centerText"] = "";
			dict["xstring_challengeDetails"] = "";
			dict["xstring_challengeName"] = "";
			dict["xstring_challengeReference"] = "";
			dict["xstring_challengeWidget"] = "Accuracy";
			dict["xstring_rewardName"] = "";
			break;
		}
		case XType::AccoladeList:
		{
			for (int i = 0; i <= 40; ++i)
				dict[std::string("accolade") + std::to_string(i)] = "";
			dict["accoladeCount"] = "0";
			break;
		}
		default:
			// Unknown types start empty
			break;
		}

		return dict;
	}
};
