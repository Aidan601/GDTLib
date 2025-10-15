// GDTClient.cpp
#define NOMINMAX
#include <windows.h>
#include <iostream>
#include <string>
#include <algorithm>

#include "GDTLibrary.h"  // all parsing & threading logic
#include "XAsset.h"      // includes LoadValuesFromSource() if you added GdtRelPath

using namespace gdtlib;  // so we can call getAllGDTs() etc. directly

int main() {
    SetConsoleOutputCP(CP_UTF8);
    auto all = getAllGDTs();
    if (all.empty()) {
        std::cout << "No GDTs parsed.\n";
        return 0;
    }

    auto it = all.find("apex");

    // Case-insensitive fallback
    if (it == all.end()) {
        for (const auto& kv : all) {
            std::string k = kv.first;
            std::transform(k.begin(), k.end(), k.begin(),
                [](unsigned char c) { return (char)std::tolower(c); });
            if (k == "apex") { it = all.find(kv.first); break; }
        }
    }

    if (it == all.end()) {
        std::cout << "\"apex\" GDT not found.\n";
        return 0;
    }

    auto& g = it->second;
    std::cout << "GDT: " << g.Name << "  [" << g.RelPath << "]\n";

    if (g.Assets.empty()) {
        std::cout << "  (no assets)\n";
        return 0;
    }
    for (auto& akv : g.Assets) {
        auto& a = akv.second;
        std::cout << "  - " << a.Name << "  (type=" << a.Type << ")\n";

        // Each xasset already knows which GDT it's from (a.GdtRelPath)
        a.LoadValuesFromSource();   // parse the .gdt file automatically
        a.PrintValues();
        std::cout << std::endl;
    }

    return 0;
}
