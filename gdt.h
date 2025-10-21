#pragma once

#include <string>
#include <filesystem>

class gdt
{
public:
	int id;
	std::string path;
	bool openForEdit = 0;
	int timestamp;
	std::string name;

    gdt()
    {
        id = -1;
        path = "";
        timestamp = 0;
		name = "";
    }

	gdt(int idInput, std::string pathInput, int timeInput)
	{
		id = idInput;
		path = pathInput;
		timestamp = timeInput;
		name = GetName(pathInput);
	}
private:
    std::string GetName(const std::string& s)
    {
        try {
            // Use std::filesystem to extract filename and stem (no extension)
            std::filesystem::path p(s);
            return p.stem().string(); // e.g. "example" from "C:\path\to\example.gdt"
        }
        catch (...) {
            // Fallback manual parsing if the path is malformed
            size_t slash = s.find_last_of("\\/");
            std::string fname = (slash == std::string::npos) ? s : s.substr(slash + 1);
            size_t dot = fname.find_last_of('.');
            if (dot != std::string::npos)
                fname = fname.substr(0, dot);
            return fname;
        }
    }
};