// ClientMain.cpp
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>
#include <filesystem>

using namespace std::filesystem;

#include "gdt.h" // gdt with .Name, .RelPath, .Time, .Assets (map-like); xasset with .Name,.Type,.value

//======================================
// UTF helpers
//======================================
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

//======================================
// Environment
//======================================
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

//======================================
// Utility helpers
//======================================
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

//======================================
// Load converter_gdt_dirs_0.txt
//======================================
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

//======================================
// Work queue scanner
//======================================
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
            FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
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

//======================================
// Fast file read (fallback)
//======================================
static std::string ReadFileToStringFast(const std::wstring& absPath) {
    std::string data;
    std::error_code ec;
    uintmax_t sz = std::filesystem::file_size(absPath, ec);
    std::ifstream in(WideToUTF8(absPath), std::ios::binary);
    if (!in) return {};
    if (!ec && sz > 0 && sz <= SIZE_MAX) {
        data.resize(static_cast<size_t>(sz));
        in.read(data.data(), static_cast<std::streamsize>(data.size()));
        if (!in) return {};
    }
    else {
        std::ostringstream ss;
        ss << in.rdbuf();
        data = std::move(ss).str();
    }
    return data;
}

//======================================
// Memory-mapped file helper (fast path)
//======================================
struct MappedFile {
    HANDLE hFile = INVALID_HANDLE_VALUE;
    HANDLE hMap = nullptr;
    const char* data = nullptr;
    size_t len = 0;

    bool open(const std::wstring& path) {
        hFile = CreateFileW(path.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER sz{};
        if (!GetFileSizeEx(hFile, &sz)) return false;
        len = size_t(sz.QuadPart);
        if (len == 0) return false;
        hMap = CreateFileMappingW(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!hMap) return false;
        data = (const char*)MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
        return data != nullptr;
    }
    ~MappedFile() {
        if (data) UnmapViewOfFile(data);
        if (hMap) CloseHandle(hMap);
        if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
    }
};

//======================================
// Single-pass pointer parser
// Parses: "asset" ( "file.gdf" ) { "k" "v" ... }
//======================================
struct Cursor {
    const char* p;
    const char* e;
    inline bool ok() const { return p < e; }
};

static inline void skip_ws(Cursor& c) {
    while (c.p < c.e && (unsigned char)*c.p <= ' ') ++c.p;
}

static inline bool scan_char(Cursor& c, char ch) {
    if (c.p < c.e && *c.p == ch) { ++c.p; return true; }
    return false;
}

static inline bool scan_quoted(Cursor& c, std::string_view& out) {
    if (!scan_char(c, '"')) return false;
    const char* s = c.p;
    // Format is simple ("..."), no escapes assumed in GDT (if escapes needed, extend here)
    while (c.p < c.e && *c.p != '"') ++c.p;
    if (c.p >= c.e) return false;
    out = std::string_view(s, size_t(c.p - s));
    ++c.p; // consume closing "
    return true;
}

static inline std::string_view StemNoExt(std::string_view s) {
    size_t slash = s.find_last_of("/\\");
    if (slash != std::string_view::npos) s.remove_prefix(slash + 1);
    size_t dot = s.rfind('.');
    if (dot != std::string_view::npos) s.remove_suffix(s.size() - dot);
    return s;
}

static void ParseAssetsFastViews(const char* data, size_t len, gdt& g) {
    Cursor c{ data, data + len };
    std::string_view name, gdf;

    while (c.ok()) {
        skip_ws(c);
        if (!c.ok()) break;

        // Try to read "asset_name"
        const char* save = c.p;
        if (!scan_quoted(c, name)) { ++c.p; continue; } // not a quoted token here; advance

        skip_ws(c);

        // Expect ( "file.gdf" )
        if (!scan_char(c, '(')) { c.p = save + 1; continue; } // rollback-ish if not an asset header
        skip_ws(c);
        if (!scan_quoted(c, gdf)) { c.p = save + 1; continue; }
        skip_ws(c);
        if (!scan_char(c, ')')) { c.p = save + 1; continue; }
        skip_ws(c);

        // Expect { ... } with "k" "v" pairs
        if (!scan_char(c, '{')) { c.p = save + 1; continue; }

        // Parse body key/values
        std::vector<std::pair<std::string_view, std::string_view>> kvs;
        for (;;) {
            skip_ws(c);
            if (!c.ok()) break;
            if (*c.p == '}') { ++c.p; break; }
            std::string_view k, v;
            if (!scan_quoted(c, k)) { /* malformed; try to recover */ ++c.p; continue; }
            skip_ws(c);
            if (!scan_quoted(c, v)) { /* malformed; try to recover */ ++c.p; continue; }
            kvs.emplace_back(k, v);
        }

        // Build xasset (copy strings once into your containers)
        xasset xa;
        xa.Name.assign(name.data(), name.size());

        std::string_view tv = StemNoExt(gdf);
        xa.Type.assign(tv.data(), tv.size());

        // Move kvs into xa.value
        // (Works for std::map or std::unordered_map; no .reserve() to keep it container-agnostic)
        for (auto& kv : kvs) {
            xa.value.emplace(
                std::string(kv.first.data(), kv.first.size()),
                std::string(kv.second.data(), kv.second.size())
            );
        }

        // Insert into gdt
        g.Assets.emplace(xa.Name, std::move(xa));
    }
}

//======================================
// Progress bar
//======================================
static std::string FormatHMS(std::chrono::seconds s) {
    auto h = std::chrono::duration_cast<std::chrono::hours>(s);
    s -= std::chrono::duration_cast<std::chrono::seconds>(h);
    auto m = std::chrono::duration_cast<std::chrono::minutes>(s);
    s -= std::chrono::duration_cast<std::chrono::seconds>(m);
    std::ostringstream os;
    if (h.count() > 0) os << std::setw(2) << std::setfill('0') << h.count() << ":";
    os << std::setw(2) << std::setfill('0') << m.count() << ":"
        << std::setw(2) << std::setfill('0') << s.count();
    return os.str();
}

static void ProgressThread(const std::atomic<size_t>& processed,
    size_t total,
    std::atomic<bool>& doneFlag,
    std::chrono::steady_clock::time_point t0,
    unsigned barWidth = 48,
    const char* label = "Parsing GDTs")
{
    using namespace std::chrono_literals;
    while (!doneFlag.load(std::memory_order_relaxed)) {
        size_t p = processed.load(std::memory_order_relaxed);
        if (p > total) p = total;
        double frac = (total > 0) ? (double)p / (double)total : 1.0;
        unsigned filled = static_cast<unsigned>(frac * barWidth);
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - t0);

        // ETA
        std::chrono::seconds eta(0);
        if (p > 0) {
            double rate = (double)p / std::max(1.0, (double)elapsed.count());
            if (rate > 0.0) {
                double remain = (double)(total - p) / rate;
                eta = std::chrono::seconds((long long)(remain + 0.5));
            }
        }

        std::ostringstream bar;
        bar << "\r" << label << " [";
        for (unsigned i = 0; i < barWidth; ++i) bar << (i < filled ? '#' : ' ');
        bar << "] " << std::setw(3) << (int)(frac * 100.0 + 0.5) << "%  "
            << p << "/" << total
            << "  elapsed " << FormatHMS(elapsed)
            << "  ETA " << (p == total ? "00:00" : FormatHMS(eta));

        std::cout << bar.str() << std::flush;

        if (p >= total) break;
        std::this_thread::sleep_for(250ms); // lower refresh rate to reduce TTY contention
    }
    std::cout << "\n";
}

//======================================
// Optional: raw dump by needle (kept for convenience)
//======================================
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
static bool DumpAssetFromGdtByNeedle(
    const std::map<std::string, gdt>& GDTs,
    const std::string& targetGdtStem,
    const std::string& needle)
{
    const gdt* found = nullptr;
    if (auto it = GDTs.find(targetGdtStem); it != GDTs.end()) {
        found = &it->second;
    }
    else {
        auto ci_equal = [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) ==
                std::tolower(static_cast<unsigned char>(b));
            };
        for (const auto& kv : GDTs) {
            const std::string& name = kv.first;
            if (name.size() == targetGdtStem.size() &&
                std::equal(name.begin(), name.end(), targetGdtStem.begin(), ci_equal)) {
                found = &kv.second;
                break;
            }
        }
    }

    if (!found) {
        std::cerr << "GDT '" << targetGdtStem << "' not found.\n";
        return false;
    }

    std::wstring root = GetToolsRootW();
    if (root.empty()) return false;

    std::wstring relW(found->RelPath.begin(), found->RelPath.end());
    for (auto& ch : relW) if (ch == L'/') ch = L'\\';
    std::wstring abs = Join2(root, relW);

    // prefer mmap for dump as well
    std::string text;
    {
        MappedFile mf;
        if (mf.open(abs)) {
            text.assign(mf.data, mf.data + mf.len);
        }
        else {
            text = ReadFileToStringFast(abs);
        }
    }
    if (text.empty()) {
        std::cerr << "Unable to read file: " << WideToUTF8(abs) << "\n";
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

//======================================
// Build map<string,gdt> with populated Assets (fast parser + mmap)
//======================================
std::map<std::string, gdt> getAllGDTs()
{
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

    // Stage 1: multithreaded directory walk
    for (unsigned i = 0; i < N; ++i)
        threads.emplace_back(Worker, std::ref(S), std::ref(locals[i]));

    { std::lock_guard<std::mutex> lk(S.m); S.no_more_push = true; }
    S.cv.notify_all();
    for (auto& t : threads) t.join();
    threads.clear();

    // Flatten file list
    std::vector<std::wstring> allFiles;
    size_t total = 0;
    for (auto& v : locals) total += v.size();
    allFiles.reserve(total);
    for (auto& v : locals) {
        for (auto& w : v) allFiles.emplace_back(std::move(w));
        v.clear();
    }

    // Stage 2: parallel parse + progress bar
    if (allFiles.empty()) {
        std::cout << "Scanned 0 .gdt file(s).\n";
        return {};
    }

    std::atomic<size_t> processed{ 0 };
    std::atomic<bool> doneFlag{ false };
    auto t0 = std::chrono::steady_clock::now();
    std::thread progress(ProgressThread, std::ref(processed), allFiles.size(),
        std::ref(doneFlag), t0, 48u, "Parsing GDTs");

    std::vector<std::vector<std::pair<std::string, gdt>>> perThread(N);
    threads.reserve(N);

    auto parseWorker = [&](unsigned tid) {
        const size_t begin = (tid * allFiles.size()) / N;
        const size_t end = ((tid + 1) * allFiles.size()) / N;
        auto& out = perThread[tid];
        out.reserve(end > begin ? (end - begin) : 0);

        for (size_t k = begin; k < end; ++k) {
            const std::wstring& relW = allFiles[k];

            // Build absolute path
            std::wstring abs = Join2(S.root, relW);

            // Create minimal gdt (skip last_write_time syscall for speed)
            size_t pos = relW.find_last_of(L'\\');
            std::wstring file = (pos == std::wstring::npos) ? relW : relW.substr(pos + 1);
            std::string gdtName = WideToUTF8(Stem(file));
            std::string relPath = WideToUTF8(relW);
            gdt g(gdtName, relPath, /*Time*/0);

            // Parse via mmap fast path (fallback to stream)
            MappedFile mf;
            if (mf.open(abs)) {
                ParseAssetsFastViews(mf.data, mf.len, g);
            }
            else {
                std::string text = ReadFileToStringFast(abs);
                if (!text.empty()) ParseAssetsFastViews(text.data(), text.size(), g);
            }

            out.emplace_back(std::move(gdtName), std::move(g));
            processed.fetch_add(1, std::memory_order_relaxed);
        }
        };

    for (unsigned i = 0; i < N; ++i)
        threads.emplace_back(parseWorker, i);
    for (auto& t : threads) t.join();

    doneFlag.store(true, std::memory_order_relaxed);
    if (progress.joinable()) progress.join();

    // Merge per-thread outputs
    std::map<std::string, gdt> GDTs;
    for (auto& bucket : perThread) {
        for (auto& kv : bucket) {
            GDTs.emplace(std::move(kv.first), std::move(kv.second));
        }
        bucket.clear();
    }

    std::cout << "Scanned " << GDTs.size() << " .gdt file(s).\n";
    return GDTs;
}

//======================================
// Main
//======================================
int main()
{
    // Optional: ensure console prints UTF-8 nicely
    SetConsoleOutputCP(CP_UTF8);

    auto allGDTs = getAllGDTs();

    // Example lookup:
    const std::string gdtName = "t7_micro_materials";
    const std::string assetName = "1_t7_micro_wood_planks_c";

    auto itG = allGDTs.find(gdtName);
    if (itG == allGDTs.end()) {
        std::cerr << "GDT not found: " << gdtName << "\n";
        return 1;
    }

    auto itA = itG->second.Assets.find(assetName);
    if (itA == itG->second.Assets.end()) {
        std::cerr << "Asset not found in " << gdtName << ": " << assetName << "\n";
        return 1;
    }

    std::cout << "Found asset: " << itA->second.Name
        << " (type=" << itA->second.Type << ") in GDT " << gdtName << "\n";

    // Show a couple of parsed values if present
    if (auto kv = itA->second.value.find("type"); kv != itA->second.value.end())
        std::cout << "  body.type = " << kv->second << "\n";
    if (auto kv = itA->second.value.find("imageType"); kv != itA->second.value.end())
        std::cout << "  imageType  = " << kv->second << "\n";

    // Optional: dump a raw block for another asset, by needle:
    // DumpAssetFromGdtByNeedle(allGDTs, "t7_micro_materials", "i_t7_micro_coir_01_c");

    return 0;
}
