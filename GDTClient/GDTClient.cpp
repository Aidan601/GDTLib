// GDTClient.cpp
// Simple console client that uses gdtlib to print all GDTs and their assets.
// Build as a small console program alongside your project.
// Usage:
//   GDTClient.exe            -> print once and exit
//   GDTClient.exe --watch    -> live-watch changes, print events, press Ctrl+C to exit

#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <thread>
#include <chrono>
#include <csignal>

#ifdef _WIN32
#include <windows.h>
#endif

#include "GDT.h"
#include "XAsset.h"
#include "GDTLibrary.h"

static volatile bool g_running = true;

#ifdef _WIN32
static BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) {
    switch (ctrlType) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
        g_running = false;
        return TRUE;
    default:
        return FALSE;
    }
}
#endif

static void print_header_once() {
    std::cout << "==== GDT Client ====\n";
    std::cout << "TA_TOOLS_PATH = ";
    const std::wstring rootW = gdtlib::GetToolsRootW();
    if (rootW.empty()) {
        std::cout << "(not set)\n";
    }
    else {
        std::cout << gdtlib::WideToUTF8(rootW) << "\n";
    }
    std::cout << std::endl;
}

static void print_snapshot(const std::map<std::string, gdt>& all) {
    std::cout << "Total GDTs: " << all.size() << "\n";
    for (const auto& kv : all) {
        const std::string& gdtName = kv.first;
        const gdt& g = kv.second;
        std::cout << "\nGDT: " << gdtName
            << "  (assets: " << g.Assets.size() << ")"
            << "  rel: " << g.RelPath
            << "\n";

        // g.Assets is a map<string, xasset>, so it’s already sorted by name.
        for (const auto& akv : g.Assets) {
            const xasset& a = akv.second;
            std::cout << "  - [" << a.Type << "] " << a.Name << "\n";
        }
    }
    std::cout << std::endl;
}

static void print_changes_and_refresh() {
    // Drain events, refresh a snapshot, and print the changed GDTs.
    auto changes = gdtlib::watch::Registry::drain_changes();
    if (changes.empty()) return;

    // Pull a fresh snapshot
    std::map<std::string, gdt> all;
    gdtlib::watch::Registry::snapshot(all);

    // Compute the set of GDT stems that changed
    std::vector<std::string> changedStems;
    changedStems.reserve(changes.size());
    for (const auto& ev : changes) {
        const std::string stem = gdtlib::WideToUTF8(gdtlib::Stem(ev.relPathW));
        changedStems.push_back(stem);
        std::cout << "[EVENT] "
            << (ev.kind == gdtlib::watch::ChangeKind::Added ? "Added   : " :
                ev.kind == gdtlib::watch::ChangeKind::Modified ? "Modified: " :
                "Deleted : ")
            << gdtlib::WideToUTF8(ev.relPathW) << "\n";
    }
    std::sort(changedStems.begin(), changedStems.end());
    changedStems.erase(std::unique(changedStems.begin(), changedStems.end()), changedStems.end());

    // Print details for changed GDTs (skip those that were deleted and are no longer present)
    for (const auto& stem : changedStems) {
        auto it = all.find(stem);
        if (it == all.end()) continue; // deleted
        const gdt& g = it->second;
        std::cout << "\nGDT: " << stem
            << "  (assets: " << g.Assets.size() << ")"
            << "  rel: " << g.RelPath
            << "\n";
        for (const auto& akv : g.Assets) {
            const xasset& a = akv.second;
            std::cout << "  - [" << a.Type << "] " << a.Name << "\n";
        }
    }
    std::cout << std::endl;
}

int main(int argc, char** argv) {
#ifdef _WIN32
    // Make Windows console UTF-8 friendly.
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
#endif

    const bool watch = (argc > 1 && std::string(argv[1]) == "--watch");

    print_header_once();

    // Ensure TA_TOOLS_PATH is set
    const std::wstring rootW = gdtlib::GetToolsRootW();
    if (rootW.empty()) {
        std::cerr << "Error: TA_TOOLS_PATH is not set in the environment.\n";
        return 1;
    }

    if (!watch) {
        // One-shot: enumerate recursively and print
        auto all = gdtlib::getAllGDTs();
        print_snapshot(all);
        return 0;
    }

    // Live mode: start the background registry, print initial snapshot, then stream changes
    if (!gdtlib::watch::Registry::start(150)) {
        std::cerr << "Error: failed to start gdtlib watcher (check TA_TOOLS_PATH and seed folders).\n";
        return 2;
    }

    std::map<std::string, gdt> all;
    gdtlib::watch::Registry::snapshot(all);
    print_snapshot(all);

    std::cout << "Watching for changes (press Ctrl+C to exit)...\n\n";

    while (g_running) {
        print_changes_and_refresh();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }

    gdtlib::watch::Registry::stop();
    std::cout << "Exited.\n";
    return 0;
}
