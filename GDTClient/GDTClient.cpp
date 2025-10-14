// ClientMain.cpp
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cctype>
#include <fstream>
#include <iostream>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>
#include <filesystem>
using namespace std::filesystem;

#include <sqlite3.h>
#include "gdt.h" // gdt(std::string nameInput, std::string relPathInput) and sets Time

// ---------------- UTF helpers ----------------
static std::string WideToUTF8(const std::wstring &w)
{
    if (w.empty())
        return {};
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static std::wstring UTF8ToWide(const std::string &s)
{
    if (s.empty())
        return {};
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// ---------------- Environment ----------------
static std::wstring GetToolsRootW()
{
    wchar_t *val = nullptr;
    size_t len = 0;
    if (_wdupenv_s(&val, &len, L"TA_TOOLS_PATH") != 0 || !val)
    {
        std::cerr << "Error: TA_TOOLS_PATH not set.\n";
        return {};
    }
    std::wstring root(val);
    free(val);
    while (!root.empty() && (root.back() == L'\\' || root.back() == L'/'))
        root.pop_back();
    return root;
}

static std::wstring GetGameRootW()
{
    wchar_t *val = nullptr;
    size_t len = 0;
    if (_wdupenv_s(&val, &len, L"TA_GAME_PATH") != 0 || !val)
    {
        std::cerr << "Error: TA_GAME_PATH not set.\n";
        return {};
    }
    std::wstring root(val);
    free(val);
    while (!root.empty() && (root.back() == L'\\' || root.back() == L'/'))
        root.pop_back();
    return root;
}

static std::wstring DbAbsPath()
{
    std::wstring root = GetGameRootW();
    if (root.empty())
        return {};
    return root + L"\\gdtlib\\gdtlib.db";
}

// ---------------- Small path helpers ----------------
static inline bool EndsWithGdtCI(const wchar_t *name, size_t len)
{
    if (len < 4)
        return false;
    wchar_t a = name[len - 4], b = name[len - 3], c = name[len - 2], d = name[len - 1];
    return (a == L'.') &&
           (b == L'g' || b == L'G') &&
           (c == L'd' || c == L'D') &&
           (d == L't' || d == L'T');
}

static std::wstring Join2(const std::wstring &a, const std::wstring &b)
{
    if (a.empty())
        return b;
    if (b.empty())
        return a;
    if (a.back() == L'\\' || a.back() == L'/')
        return a + b;
    return a + L'\\' + b;
}

static std::wstring Stem(const std::wstring &filename)
{
    auto pos = filename.find_last_of(L'.');
    if (pos == std::wstring::npos || pos == 0)
        return filename;
    return filename.substr(0, pos);
}

// ---------------- Load converter_gdt_dirs_0.txt ----------------
static std::vector<std::wstring> LoadTargetDirsRelW(const std::wstring &toolsRoot)
{
    std::vector<std::wstring> out;
    const std::wstring cfg = Join2(Join2(toolsRoot, L"bin"), L"converter_gdt_dirs_0.txt");

    std::ifstream in(WideToUTF8(cfg), std::ios::in | std::ios::binary);
    if (!in)
    {
        std::cerr << "Error: cannot open list file: " << WideToUTF8(cfg) << "\n";
        return out;
    }

    std::unordered_set<std::wstring> seen;
    std::string lineUtf8;
    bool first = true;

    while (std::getline(in, lineUtf8))
    {
        if (first)
        {
            first = false;
            if (lineUtf8.size() >= 3 &&
                (unsigned char)lineUtf8[0] == 0xEF &&
                (unsigned char)lineUtf8[1] == 0xBB &&
                (unsigned char)lineUtf8[2] == 0xBF)
            {
                lineUtf8.erase(0, 3);
            }
        }
        auto l = lineUtf8.begin();
        while (l != lineUtf8.end() && (unsigned char)*l <= ' ')
            ++l;
        auto r = lineUtf8.end();
        while (r != l && (unsigned char)*(r - 1) <= ' ')
            --r;
        std::string trimmed(l, r);
        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';')
            continue;

        std::wstring rel = UTF8ToWide(trimmed);
        for (auto &ch : rel)
            if (ch == L'/')
                ch = L'\\';

        std::wstring abs = Join2(toolsRoot, rel);
        DWORD attr = GetFileAttributesW(abs.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
        {
            std::cerr << "Warning: folder not found: " << WideToUTF8(abs) << "\n";
            continue;
        }
        if (seen.insert(rel).second)
            out.push_back(std::move(rel));
    }
    return out;
}

// ---------------- Work queue scanner ----------------
struct Shared
{
    std::wstring root;
    std::queue<std::wstring> q; // relative dirs under root
    std::mutex m;
    std::condition_variable cv;
    bool no_more_push = false;
    std::atomic<size_t> files_found{0};
};

static void Worker(Shared &S, std::vector<std::wstring> &out_relpaths)
{
    for (;;)
    {
        std::wstring relDir;
        {
            std::unique_lock<std::mutex> lk(S.m);
            S.cv.wait(lk, [&]
                      { return !S.q.empty() || S.no_more_push; });
            if (S.q.empty())
            {
                if (S.no_more_push)
                    return;
                continue;
            }
            relDir = std::move(S.q.front());
            S.q.pop();
        }

        std::wstring absDir = Join2(S.root, relDir);
        std::wstring pattern = Join2(absDir, L"*");

        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW(pattern.c_str(),
                                    FindExInfoBasic,
                                    &fd,
                                    FindExSearchNameMatch,
                                    nullptr,
                                    FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE)
            continue;

        do
        {
            const wchar_t *name = fd.cFileName;
            if (name[0] == L'.' && (name[1] == 0 || (name[1] == L'.' && name[2] == 0)))
                continue;

            const bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (isDir)
            {
                std::wstring childRel = relDir;
                if (!childRel.empty())
                    childRel += L'\\';
                childRel += name;
                {
                    std::lock_guard<std::mutex> lk(S.m);
                    S.q.push(std::move(childRel));
                    S.cv.notify_one();
                }
            }
            else
            {
                size_t nlen = wcslen(name);
                if (!EndsWithGdtCI(name, nlen))
                    continue;

                std::wstring rel = relDir;
                if (!rel.empty())
                    rel += L'\\';
                rel += name;

                out_relpaths.emplace_back(std::move(rel));
                S.files_found.fetch_add(1, std::memory_order_relaxed);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

// ---------------- DB: persist GDT list into _gdt ----------------
static bool SaveGdtListToDb(const std::vector<gdt> &GDT)
{
    std::wstring dbw = DbAbsPath();
    if (dbw.empty())
        return false;
    std::string db(dbw.begin(), dbw.end());

    sqlite3 *conn = nullptr;
    int rc = sqlite3_open_v2(db.c_str(), &conn,
                             SQLITE_OPEN_READWRITE /*| SQLITE_OPEN_CREATE*/, nullptr);
    if (rc != SQLITE_OK)
    {
        std::cerr << "SQLite open error: " << (conn ? sqlite3_errmsg(conn) : "unknown") << "\n";
        if (conn)
            sqlite3_close(conn);
        return false;
    }

    sqlite3_exec(conn, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(conn, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);

    const char *kSelect = "SELECT PK_id, timestamp FROM _gdt WHERE name = ?1;";
    const char *kInsert = "INSERT INTO _gdt (name, bOpenForEdit, timestamp) VALUES (?1, 0, ?2);";
    const char *kUpdate = "UPDATE _gdt SET timestamp = ?2 WHERE PK_id = ?3;";

    sqlite3_stmt *sel = nullptr, *ins = nullptr, *upd = nullptr;
    rc = sqlite3_prepare_v2(conn, kSelect, -1, &sel, nullptr);
    if (rc == SQLITE_OK)
        rc = sqlite3_prepare_v2(conn, kInsert, -1, &ins, nullptr);
    if (rc == SQLITE_OK)
        rc = sqlite3_prepare_v2(conn, kUpdate, -1, &upd, nullptr);
    if (rc != SQLITE_OK)
    {
        std::cerr << "SQLite prepare error: " << sqlite3_errmsg(conn) << "\n";
        if (sel)
            sqlite3_finalize(sel);
        if (ins)
            sqlite3_finalize(ins);
        if (upd)
            sqlite3_finalize(upd);
        sqlite3_close(conn);
        return false;
    }

    rc = sqlite3_exec(conn, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK)
        std::cerr << "BEGIN failed: " << sqlite3_errmsg(conn) << "\n";

    for (const auto &g : GDT)
    {
        sqlite3_reset(sel);
        sqlite3_clear_bindings(sel);
        sqlite3_bind_text(sel, 1, g.Name.c_str(), -1, SQLITE_TRANSIENT);

        int step = sqlite3_step(sel);
        if (step == SQLITE_ROW)
        {
            int pk = sqlite3_column_int(sel, 0);
            int dbTime = sqlite3_column_int(sel, 1);

            if (g.Time > dbTime)
            {
                sqlite3_reset(upd);
                sqlite3_clear_bindings(upd);
                sqlite3_bind_int(upd, 2, g.Time);
                sqlite3_bind_int(upd, 3, pk);

                if (sqlite3_step(upd) == SQLITE_DONE)
                {
                    std::cout << "Updated " << g.Name << " (local newer)\n";
                }
                else
                {
                    std::cerr << "UPDATE error for " << g.Name
                              << ": " << sqlite3_errmsg(conn) << "\n";
                }
            }
            // else skip silently
        }
        else if (step == SQLITE_DONE)
        {
            // No existing row → insert new
            sqlite3_reset(ins);
            sqlite3_clear_bindings(ins);
            sqlite3_bind_text(ins, 1, g.Name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(ins, 2, g.Time);

            if (sqlite3_step(ins) == SQLITE_DONE)
            {
                std::cout << "Inserted " << g.Name << " (new file)\n";
            }
            else
            {
                std::cerr << "INSERT error for " << g.Name
                          << ": " << sqlite3_errmsg(conn) << "\n";
            }
        }
        else
        {
            std::cerr << "SELECT error for " << g.Name
                      << ": " << sqlite3_errmsg(conn) << "\n";
        }
    }

    rc = sqlite3_exec(conn, "COMMIT;", nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK)
        std::cerr << "COMMIT failed: " << sqlite3_errmsg(conn) << "\n";

    sqlite3_finalize(sel);
    sqlite3_finalize(ins);
    sqlite3_finalize(upd);
    sqlite3_close(conn);
    return true;
}

// ---------------- Main ----------------
std::vector<gdt> getAllGDTs()
{
    // 1) Read root and list of seed directories
    std::wstring toolsRoot = GetToolsRootW();
    if (toolsRoot.empty())
        return {};

    auto targets = LoadTargetDirsRelW(toolsRoot);
    if (targets.empty())
    {
        std::cerr << "No folders to scan.\n";
        return {};
    }

    // 2) Launch worker pool, scan in parallel
    Shared S;
    S.root = toolsRoot;
    {
        std::lock_guard<std::mutex> lk(S.m);
        for (auto &d : targets)
            S.q.push(d);
    }

    unsigned N = std::clamp<unsigned>(std::thread::hardware_concurrency(), 2, 16);
    std::vector<std::thread> threads;
    std::vector<std::vector<std::wstring>> locals(N);
    threads.reserve(N);

    for (unsigned i = 0; i < N; ++i)
        threads.emplace_back(Worker, std::ref(S), std::ref(locals[i]));

    {
        std::lock_guard<std::mutex> lk(S.m);
        S.no_more_push = true;
    }
    S.cv.notify_all();
    for (auto &t : threads)
        t.join();

    // 3) Merge & dedupe -> build GDT vector
    std::unordered_set<std::wstring> seen;
    std::vector<gdt> GDT;

    for (auto &bucket : locals)
    {
        for (auto &relW : bucket)
        {
            if (seen.insert(relW).second)
            {
                size_t pos = relW.find_last_of(L'\\');
                std::wstring file = (pos == std::wstring::npos) ? relW : relW.substr(pos + 1);

                // Build absolute path
                std::wstring abs = Join2(S.root, relW);

                // Default to current time if lookup fails
                int timeInt = static_cast<int>(
                    std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));

                try
                {
                    auto ftime = std::filesystem::last_write_time(abs);

                    // Convert from file_time_type (implementation-defined clock) to system_clock
                    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                        ftime - decltype(ftime)::clock::now() + std::chrono::system_clock::now());

                    timeInt = static_cast<int>(std::chrono::system_clock::to_time_t(sctp));
                }
                catch (const std::exception &e)
                {
                    std::cerr << "Warning: could not get time for "
                              << WideToUTF8(abs) << " (" << e.what() << ")\n";
                }

                // Construct gdt with timestamp
                GDT.emplace_back(WideToUTF8(Stem(file)), WideToUTF8(relW), timeInt);
            }
        }
    }

    std::sort(GDT.begin(), GDT.end(),
              [](const gdt &a, const gdt &b)
              { return a.RelPath < b.RelPath; });

    std::cout << "Found " << GDT.size() << " .gdt file(s) across "
              << targets.size() << " seed folder(s).\n";

    return GDT;
}

int main()
{
    std::vector<gdt> GDT = getAllGDTs();
}