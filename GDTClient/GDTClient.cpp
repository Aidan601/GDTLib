// ClientMain.cpp
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>
#include <filesystem>
using namespace std::filesystem;

#include "gdt.h" // your gdt class

// ---------------- UTF helpers ----------------
static std::string WideToUTF8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static std::wstring UTF8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// ---------------- Environment ----------------
static std::wstring GetToolsRootW() {
    wchar_t* val = nullptr;
    size_t len = 0;
    if (_wdupenv_s(&val, &len, L"TA_TOOLS_PATH") != 0 || !val) {
        std::cerr << "Error: TA_TOOLS_PATH not set.\n";
        return {};
    }
    std::wstring root(val);
    free(val);
    while (!root.empty() && (root.back() == L'\\' || root.back() == L'/')) root.pop_back();
    return root;
}

// ---------------- Utility helpers ----------------
static inline bool EndsWithGdtCI(const wchar_t* name, size_t len) {
    if (len < 4) return false;
    wchar_t a = name[len - 4], b = name[len - 3], c = name[len - 2], d = name[len - 1];
    return (a == L'.') &&
        (b == L'g' || b == L'G') &&
        (c == L'd' || c == L'D') &&
        (d == L't' || d == L'T');
}

static std::wstring Join2(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + L'\\' + b;
}

static std::wstring Stem(const std::wstring& filename) {
    auto pos = filename.find_last_of(L'.');
    if (pos == std::wstring::npos || pos == 0) return filename;
    return filename.substr(0, pos);
}

// ---------------- Load converter_gdt_dirs_0.txt ----------------
static std::vector<std::wstring> LoadTargetDirsRelW(const std::wstring& root) {
    std::vector<std::wstring> out;
    const std::wstring cfg = Join2(Join2(root, L"bin"), L"converter_gdt_dirs_0.txt");

    std::ifstream in(WideToUTF8(cfg), std::ios::in | std::ios::binary);
    if (!in) {
        std::cerr << "Error: cannot open list file: " << WideToUTF8(cfg) << "\n";
        return out;
    }

    std::unordered_set<std::wstring> seen;
    std::string lineUtf8;
    bool first = true;
    while (std::getline(in, lineUtf8)) {
        if (first) {
            first = false;
            if (lineUtf8.size() >= 3 &&
                (unsigned char)lineUtf8[0] == 0xEF &&
                (unsigned char)lineUtf8[1] == 0xBB &&
                (unsigned char)lineUtf8[2] == 0xBF)
                lineUtf8.erase(0, 3);
        }
        auto l = lineUtf8.begin();
        while (l != lineUtf8.end() && (unsigned char)*l <= ' ') ++l;
        auto r = lineUtf8.end();
        while (r != l && (unsigned char)*(r - 1) <= ' ') --r;
        std::string trimmed(l, r);
        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') continue;

        std::wstring rel = UTF8ToWide(trimmed);
        for (auto& ch : rel) if (ch == L'/') ch = L'\\';

        std::wstring abs = Join2(root, rel);
        DWORD attr = GetFileAttributesW(abs.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            std::cerr << "Warning: folder not found: " << WideToUTF8(abs) << "\n";
            continue;
        }
        if (seen.insert(rel).second) out.push_back(std::move(rel));
    }
    return out;
}

// ---------------- Work queue scanner ----------------
struct Shared {
    std::wstring root;
    std::queue<std::wstring> q;
    std::mutex m;
    std::condition_variable cv;
    bool no_more_push = false;
    std::atomic<size_t> files_found{ 0 };
};

static void Worker(Shared& S, std::vector<std::wstring>& out_relpaths) {
    for (;;) {
        std::wstring relDir;
        {
            std::unique_lock<std::mutex> lk(S.m);
            S.cv.wait(lk, [&] { return !S.q.empty() || S.no_more_push; });
            if (S.q.empty()) {
                if (S.no_more_push) return;
                continue;
            }
            relDir = std::move(S.q.front());
            S.q.pop();
        }

        std::wstring absDir = Join2(S.root, relDir);
        std::wstring pattern = Join2(absDir, L"*");

        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd,
            FindExSearchNameMatch, nullptr,
            FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) continue;

        do {
            const wchar_t* name = fd.cFileName;
            if (name[0] == L'.' && (name[1] == 0 || (name[1] == L'.' && name[2] == 0))) continue;

            const bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (isDir) {
                std::wstring childRel = relDir;
                if (!childRel.empty()) childRel += L'\\';
                childRel += name;
                {
                    std::lock_guard<std::mutex> lk(S.m);
                    S.q.push(std::move(childRel));
                    S.cv.notify_one();
                }
            }
            else {
                size_t nlen = wcslen(name);
                if (!EndsWithGdtCI(name, nlen)) continue;

                std::wstring rel = relDir;
                if (!rel.empty()) rel += L'\\';
                rel += name;
                out_relpaths.emplace_back(std::move(rel));
                S.files_found.fetch_add(1, std::memory_order_relaxed);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

// ---------------- Asset search & dump ----------------
static std::string LoadFileUTF8(const std::wstring& absPath) {
    std::ifstream in(std::string(absPath.begin(), absPath.end()), std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static bool ExtractEnclosingBlock(const std::string& text, size_t idx, size_t& outStart, size_t& outEnd) {
    size_t open = text.rfind('{', idx);
    if (open == std::string::npos) return false;
    int depth = 0;
    for (size_t i = open; i < text.size(); ++i) {
        if (text[i] == '{') ++depth;
        else if (text[i] == '}') {
            --depth;
            if (depth == 0) {
                outStart = open;
                outEnd = i + 1;
                return true;
            }
        }
    }
    return false;
}

static std::string ExtractHeaderLine(const std::string& text, size_t blockStart) {
    size_t lineEnd = (blockStart == 0) ? 0 : blockStart - 1;
    size_t lineBeg = text.rfind('\n', lineEnd);
    if (lineBeg == std::string::npos) lineBeg = 0; else ++lineBeg;
    size_t nextNew = text.find('\n', lineBeg);
    if (nextNew == std::string::npos) nextNew = blockStart;
    std::string line = text.substr(lineBeg, std::min(nextNew, blockStart) - lineBeg);
    while (!line.empty() && (unsigned char)line.front() <= ' ') line.erase(line.begin());
    while (!line.empty() && (unsigned char)line.back() <= ' ') line.pop_back();
    return line;
}

static bool DumpAssetFromGdtByNeedle(const std::vector<gdt>& GDT,
    const std::string& targetGdtStem,
    const std::string& needle)
{
    auto ci_eq = [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); };
    const gdt* found = nullptr;
    for (const auto& g : GDT) {
        if (g.Name.size() == targetGdtStem.size() &&
            std::equal(g.Name.begin(), g.Name.end(), targetGdtStem.begin(), ci_eq)) {
            found = &g; break;
        }
    }
    if (!found) {
        std::cerr << "GDT '" << targetGdtStem << "' not found.\n";
        return false;
    }

    std::wstring root = GetToolsRootW();
    std::wstring relW(found->RelPath.begin(), found->RelPath.end());
    for (auto& ch : relW) if (ch == L'/') ch = L'\\';
    std::wstring abs = Join2(root, relW);

    std::string text = LoadFileUTF8(abs);
    if (text.empty()) {
        std::cerr << "Unable to read file: " << std::string(abs.begin(), abs.end()) << "\n";
        return false;
    }

    size_t pos = text.find(needle);
    if (pos == std::string::npos) {
        std::cerr << "Asset '" << needle << "' not found in " << found->RelPath << "\n";
        return false;
    }

    size_t b0 = 0, b1 = 0;
    if (!ExtractEnclosingBlock(text, pos, b0, b1)) {
        std::cerr << "Failed to locate enclosing block.\n";
        return false;
    }

    std::string header = ExtractHeaderLine(text, b0);
    std::string block = text.substr(b0, b1 - b0);

    std::cout << "=== " << found->Name << " / " << needle << " ===\n";
    if (!header.empty()) std::cout << header << "\n";
    std::cout << block << "\n";
    return true;
}

// ---------------- Main ----------------
std::vector<gdt> getAllGDTs() {
    std::wstring toolsRoot = GetToolsRootW();
    if (toolsRoot.empty()) return {};

    auto targets = LoadTargetDirsRelW(toolsRoot);
    if (targets.empty()) {
        std::cerr << "No folders to scan.\n";
        return {};
    }

    Shared S;
    S.root = toolsRoot;
    { std::lock_guard<std::mutex> lk(S.m); for (auto& d : targets) S.q.push(d); }

    unsigned N = std::clamp<unsigned>(std::thread::hardware_concurrency(), 2, 16);
    std::vector<std::thread> threads;
    std::vector<std::vector<std::wstring>> locals(N);
    threads.reserve(N);

    for (unsigned i = 0; i < N; ++i)
        threads.emplace_back(Worker, std::ref(S), std::ref(locals[i]));

    { std::lock_guard<std::mutex> lk(S.m); S.no_more_push = true; }
    S.cv.notify_all();
    for (auto& t : threads) t.join();

    std::unordered_set<std::wstring> seen;
    std::vector<gdt> GDT;

    for (auto& bucket : locals)
    {
        for (auto& relW : bucket)
        {
            if (seen.insert(relW).second)
            {
                size_t pos = relW.find_last_of(L'\\');
                std::wstring file = (pos == std::wstring::npos) ? relW : relW.substr(pos + 1);

                // Build absolute path
                std::wstring abs = Join2(S.root, relW);

                // Default to current time if lookup fails
                int timeInt = static_cast<int>(
                    std::chrono::system_clock::to_time_t(std::chrono::system_clock::now())
                    );

                try
                {
                    auto ftime = std::filesystem::last_write_time(abs);

                    // Convert from file_time_type (implementation-defined clock) to system_clock
                    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                        ftime - decltype(ftime)::clock::now()
                        + std::chrono::system_clock::now()
                    );

                    timeInt = static_cast<int>(std::chrono::system_clock::to_time_t(sctp));

                }
                catch (const std::exception& e) {
                    std::cerr << "Warning: could not get time for "
                        << WideToUTF8(abs) << " (" << e.what() << ")\n";
                }

                // Construct gdt with timestamp
                GDT.emplace_back(WideToUTF8(Stem(file)), WideToUTF8(relW), timeInt);
            }
        }
    }
    std::cout << "Scanned " << GDT.size() << " .gdt file(s).\n";

    // --- TEST: find asset in gdt ---
    return GDT;
}

int main()
{
	std::vector<gdt> allGDTs = getAllGDTs();
    DumpAssetFromGdtByNeedle(allGDTs, "t7_micro_materials", "i_t7_micro_coir_01_c");
    return 0;
}