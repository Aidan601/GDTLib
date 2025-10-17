// GDTLibrary.h — header-only helpers + background watcher for *.gdt changes
// Recursive, multi-threaded enumeration; watches subdirectories; reads seed dirs
// from <TA_TOOLS_PATH>\bin\converter_gdt_dirs_0.txt (creates file with defaults if missing)
//
// Build: Windows/MSVC, C++17+
// Depends on: GDT.h (class gdt), XAsset.h (class xasset)
// Env: TA_TOOLS_PATH must point at the tools root

#pragma once
#ifndef GDT_LIBRARY_H
#define GDT_LIBRARY_H

// ===========================
// Windows + CRT
// ===========================
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include <windows.h>

// ===========================
// STL
// ===========================
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <locale>
#include <map>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <cwctype>
#include <regex>

// ===========================
// Project headers
// ===========================
#include "GDT.h"     // class gdt { Name, RelPath, Time, std::map<std::string,xasset> Assets; ... }
#include "XAsset.h"  // class xasset { Name, Type, ... }

namespace gdtlib {

    // ===========================
    // UTF-8 / UTF-16 helpers
    // ===========================
    inline std::string WideToUTF8(const std::wstring& w) {
        if (w.empty()) return {};
        int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        std::string s(n, '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
        return s;
    }
    inline std::wstring UTF8ToWide(std::string_view v) {
        if (v.empty()) return {};
        int n = ::MultiByteToWideChar(CP_UTF8, 0, v.data(), (int)v.size(), nullptr, 0);
        std::wstring w(n, L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, v.data(), (int)v.size(), w.data(), n);
        return w;
    }

    // ===========================
    // Small path helpers
    // ===========================
    inline bool EndsWithI(std::wstring_view s, std::wstring_view suf) {
        if (s.size() < suf.size()) return false;
        size_t i = s.size() - suf.size();
        for (size_t k = 0; k < suf.size(); ++k) {
            wchar_t a = (wchar_t)std::towlower(s[i + k]);
            wchar_t b = (wchar_t)std::towlower(suf[k]);
            if (a != b) return false;
        }
        return true;
    }
    inline std::wstring Stem(const std::wstring& path) {
        size_t p = path.find_last_of(L"/\\");
        size_t s = (p == std::wstring::npos) ? 0 : (p + 1);
        size_t dot = path.find_last_of(L'.');
        if (dot == std::wstring::npos || dot < s) dot = path.size();
        return path.substr(s, dot - s);
    }
    inline std::wstring NormalizeBackslashes(std::wstring v) {
        for (auto& ch : v) if (ch == L'/') ch = L'\\';
        return v;
    }
    inline std::wstring Join2(const std::wstring& a, const std::wstring& b) {
        if (a.empty()) return b;
        if (b.empty()) return a;
        if (a.back() == L'\\' || a.back() == L'/') return a + b;
        return a + L'\\' + b;
    }
    inline bool FileExistsW(const std::wstring& abs) {
        DWORD st = ::GetFileAttributesW(abs.c_str());
        return st != INVALID_FILE_ATTRIBUTES && !(st & FILE_ATTRIBUTE_DIRECTORY);
    }
    inline std::wstring RelPathFrom(const std::wstring& baseAbs, const std::wstring& fullAbs) {
        std::error_code ec;
        std::filesystem::path rel = std::filesystem::relative(fullAbs, baseAbs, ec);
        if (ec) {
            if (fullAbs.size() >= baseAbs.size() &&
                (fullAbs.compare(0, baseAbs.size(), baseAbs) == 0)) {
                std::wstring rest = fullAbs.substr(baseAbs.size());
                if (!rest.empty() && (rest[0] == L'/' || rest[0] == L'\\')) rest.erase(0, 1);
                return NormalizeBackslashes(rest);
            }
            return L"";
        }
        return NormalizeBackslashes(rel.wstring());
    }

    // ===========================
    // Environment and seed dirs
    // ===========================
    inline std::wstring GetToolsRootW() {
        wchar_t* val = nullptr; size_t len = 0;
        if (_wdupenv_s(&val, &len, L"TA_TOOLS_PATH") != 0 || !val) {
            std::cerr << "Error: TA_TOOLS_PATH not set.\n";
            return {};
        }
        std::wstring root(val);
        free(val);
        while (!root.empty() && (root.back() == L'/' || root.back() == L'\\')) root.pop_back();
        return root;
    }

    // Only looks for <TA_TOOLS_PATH>\bin\converter_gdt_dirs_0.txt
    // Creates it with defaults if missing, then loads it.
    inline std::vector<std::wstring> LoadTargetDirsRelW(const std::wstring& toolsRoot) {
        std::vector<std::wstring> out;

        static const wchar_t* kDefaults[] = {
            L"_custom",
            L"archetypes",
            L"model_export",
            L"source_data",
            L"texture_assets",
            L"xanim_export",
            L"art_assets",
            L"usermaps",
            L"mods"
        };

        const std::wstring binDir = Join2(toolsRoot, L"bin");
        const std::wstring cfgPath = Join2(binDir, L"converter_gdt_dirs_0.txt");

        std::error_code ec;
        if (!std::filesystem::exists(binDir, ec)) {
            std::filesystem::create_directories(binDir, ec);
        }

        if (!std::filesystem::exists(cfgPath, ec)) {
            std::wofstream create(cfgPath);
            create.imbue(std::locale(""));
            if (create) {
                for (auto* s : kDefaults) create << s << L"\n";
                create.close();
            }
            else {
                for (auto* s : kDefaults) out.emplace_back(s);
                return out;
            }
        }

        std::wifstream in(cfgPath);
        if (!in) {
            for (auto* s : kDefaults) out.emplace_back(s);
            return out;
        }

        in.imbue(std::locale(""));
        std::wstring line;
        while (std::getline(in, line)) {
            if (!line.empty() && line[0] == 0xFEFF) line.erase(0, 1);
            while (!line.empty() &&
                (line.back() == L'\r' || line.back() == L'\n' ||
                    line.back() == L' ' || line.back() == L'\t'))
                line.pop_back();

            size_t s = 0;
            while (s < line.size() && (line[s] == L' ' || line[s] == L'\t')) ++s;
            if (s) line.erase(0, s);

            if (line.empty() || line[0] == L'#') continue;

            for (auto& ch : line) if (ch == L'/') ch = L'\\';

            out.push_back(line);
        }
        return out;
    }

    // ===========================
    // Parsing API (declarations expected by callers)
    // ===========================
    // Implement this in your .cpp to fully populate gdt (including g.Assets).
    // This header also provides a minimal inline fallback at the end.
    bool ParseOneFile(const std::wstring& toolsRoot, const std::wstring& relPath, gdt& out);

    // ===========================
    // Multi-threaded, recursive enumeration and parse of all *.gdt
    // ===========================
    inline std::map<std::string, gdt> getAllGDTs() {
        std::map<std::string, gdt> out;

        const std::wstring rootW = GetToolsRootW();
        if (rootW.empty()) return out;

        const auto relDirs = LoadTargetDirsRelW(rootW);
        std::vector<std::pair<std::wstring /*absDir*/, std::wstring /*relDir*/>> dirs;
        dirs.reserve(relDirs.size());
        for (const auto& rd : relDirs) {
            std::wstring abs = Join2(rootW, rd);
            DWORD st = ::GetFileAttributesW(abs.c_str());
            if (st != INVALID_FILE_ATTRIBUTES && (st & FILE_ATTRIBUTE_DIRECTORY))
                dirs.emplace_back(abs, rd);
        }

        struct Job { std::wstring relPath; }; // rel path under tools root (e.g. L"source_data\\sub\\a.gdt")
        std::vector<Job> jobs;
        jobs.reserve(4096);

        namespace fs = std::filesystem;
        for (const auto& d : dirs) {
            const std::wstring& absDir = d.first;
            const std::wstring& relDir = d.second;
            std::error_code ec;
            fs::recursive_directory_iterator it(absDir, fs::directory_options::skip_permission_denied, ec), end;
            for (; it != end; it.increment(ec)) {
                if (ec) { ec.clear(); continue; }
                const fs::directory_entry& de = *it;
                if (!de.is_regular_file(ec)) continue;
                const std::wstring fileAbs = NormalizeBackslashes(de.path().wstring());
                if (!EndsWithI(fileAbs, L".gdt")) continue;
                std::wstring relInsideSeed = RelPathFrom(absDir, fileAbs);
                if (relInsideSeed.empty()) continue;
                std::wstring relUnderRoot = Join2(relDir, relInsideSeed);
                jobs.push_back({ std::move(relUnderRoot) });
            }
        }

        std::mutex mu;
        const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
        const unsigned threads = std::min<unsigned>(hw, (unsigned)std::max<size_t>(1, jobs.size()));
        std::atomic<size_t> idx{ 0 };
        std::vector<std::thread> workers;
        workers.reserve(threads);

        auto worker = [&]() {
            while (true) {
                size_t i = idx.fetch_add(1, std::memory_order_relaxed);
                if (i >= jobs.size()) break;
                const auto& jb = jobs[i];
                gdt tmp;
                if (ParseOneFile(rootW, jb.relPath, tmp)) {
                    const std::string key = WideToUTF8(Stem(jb.relPath));
                    std::lock_guard<std::mutex> lk(mu);
                    out.emplace(key, std::move(tmp));
                }
            }
            };

        for (unsigned t = 0; t < threads; ++t) workers.emplace_back(worker);
        for (auto& th : workers) th.join();

        return out;
    }

    // ==========================================================
    // Background watcher: gdtlib::watch::Registry
    // - Watches each seed dir (recursively) for *.gdt add/modify/delete
    // - Re-parses only changed files and keeps an internal cache
    // - Exposes generation(), snapshot(), drain_changes(), start()/stop()
    // ==========================================================
    namespace watch {

        enum class ChangeKind : int { Added = 1, Modified = 2, Deleted = 3 };

        struct Change {
            ChangeKind kind{};
            std::wstring relPathW;  // relative to TA_TOOLS_PATH, e.g. L"source_data\\sub\\a.gdt"
        };

        class Registry {
        public:
            static bool start(unsigned debounce_ms = 120) {
                std::lock_guard<std::mutex> lk(s_muStart_);
                if (s_running_) return true;

                s_rootW_ = GetToolsRootW();
                if (s_rootW_.empty()) return false;
                s_dirsRel_ = LoadTargetDirsRelW(s_rootW_);
                if (s_dirsRel_.empty()) return false;

                {
                    auto initial = getAllGDTs();
                    std::unique_lock<std::shared_mutex> lk2(s_mu_);
                    s_all_ = std::move(initial);
                    ++s_generation_;
                }

                s_handles_.clear();
                s_absDirs_.clear();
                s_mtimes_.clear();

                for (const auto& relW : s_dirsRel_) {
                    const std::wstring abs = Join2(s_rootW_, relW);
                    DWORD st = ::GetFileAttributesW(abs.c_str());
                    if (st == INVALID_FILE_ATTRIBUTES || !(st & FILE_ATTRIBUTE_DIRECTORY)) continue;

                    // TRUE => watch subtree recursively
                    HANDLE h = ::FindFirstChangeNotificationW(
                        abs.c_str(), TRUE,
                        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE
                    );
                    if (h != INVALID_HANDLE_VALUE) {
                        s_handles_.push_back(h);
                        s_absDirs_.push_back(abs);
                        seedDir(abs);
                    }
                }

                if (s_handles_.empty()) return false;

                s_stop_.store(false);
                s_thread_ = std::thread([debounce_ms]() { loop(debounce_ms); });
                s_running_ = true;
                return true;
            }

            static void stop() {
                std::lock_guard<std::mutex> lk(s_muStart_);
                if (!s_running_) return;
                s_stop_.store(true);
                if (s_thread_.joinable()) s_thread_.join();
                for (HANDLE h : s_handles_) if (h && h != INVALID_HANDLE_VALUE) ::FindCloseChangeNotification(h);
                s_handles_.clear();
                s_absDirs_.clear();
                s_mtimes_.clear();
                s_running_ = false;
            }

            static uint64_t generation() { return s_generation_.load(std::memory_order_acquire); }

            static void snapshot(std::map<std::string, gdt>& out) {
                std::shared_lock<std::shared_mutex> lk(s_mu_);
                out = s_all_;
            }

            static std::vector<Change> drain_changes() {
                std::lock_guard<std::mutex> lk(s_muQ_);
                std::vector<Change> out; out.swap(s_queue_);
                return out;
            }

            static void force_rescan_all() {
                auto fresh = getAllGDTs();
                std::unique_lock<std::shared_mutex> lk(s_mu_);
                s_all_.swap(fresh);
                s_generation_.fetch_add(1, std::memory_order_acq_rel);
            }

        private:
            static inline std::atomic<bool>          s_stop_{ false };
            static inline std::atomic<uint64_t>      s_generation_{ 0 };
            static inline std::shared_mutex          s_mu_;
            static inline std::map<std::string, gdt> s_all_;

            static inline std::mutex                 s_muStart_;
            static inline bool                       s_running_ = false;
            static inline std::thread                s_thread_;

            static inline std::vector<HANDLE>        s_handles_;
            static inline std::vector<std::wstring>  s_absDirs_;
            static inline std::vector<std::wstring>  s_dirsRel_;
            static inline std::wstring               s_rootW_;

            // absDir -> (relPathInsideAbsDir -> FILETIME)
            static inline std::unordered_map<std::wstring,
                std::unordered_map<std::wstring, FILETIME>> s_mtimes_;

            static inline std::mutex                 s_muQ_;
            static inline std::vector<Change>        s_queue_;

            static void push(ChangeKind k, const std::wstring& relW) {
                std::lock_guard<std::mutex> lk(s_muQ_);
                s_queue_.push_back(Change{ k, relW });
            }

            static std::wstring relFromAbs(const std::wstring& absDir, const std::wstring& relInsideAbs) {
                // absDir is an absolute seed dir path; relInsideAbs is relative to absDir
                std::wstring relDirFromRoot = absDir.substr(s_rootW_.size());
                if (!relDirFromRoot.empty() && (relDirFromRoot[0] == L'\\' || relDirFromRoot[0] == L'/')) relDirFromRoot.erase(0, 1);
                if (!relDirFromRoot.empty() && relDirFromRoot.back() != L'\\') relDirFromRoot.push_back(L'\\');
                return NormalizeBackslashes(relDirFromRoot + relInsideAbs);
            }

            static void seedDir(const std::wstring& absDir) {
                std::unordered_map<std::wstring, FILETIME> m;
                namespace fs = std::filesystem;
                std::error_code ec;
                for (fs::recursive_directory_iterator it(absDir, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
                    if (ec) { ec.clear(); continue; }
                    const fs::directory_entry& de = *it;
                    if (!de.is_regular_file(ec)) continue;
                    const std::wstring fileAbs = NormalizeBackslashes(de.path().wstring());
                    if (!EndsWithI(fileAbs, L".gdt")) continue;
                    std::wstring relInside = RelPathFrom(absDir, fileAbs);
                    WIN32_FILE_ATTRIBUTE_DATA fad{};
                    if (::GetFileAttributesExW(fileAbs.c_str(), GetFileExInfoStandard, &fad))
                        m.emplace(std::move(relInside), fad.ftLastWriteTime);
                }
                s_mtimes_[absDir] = std::move(m);
            }

            static void loop(unsigned debounce_ms) {
                const DWORD n = (DWORD)s_handles_.size();
                while (!s_stop_.load(std::memory_order_relaxed)) {
                    DWORD w = ::WaitForMultipleObjects(n, s_handles_.data(), FALSE, 200);
                    if (w >= WAIT_OBJECT_0 && w < WAIT_OBJECT_0 + n) {
                        const DWORD idx = w - WAIT_OBJECT_0;
                        ::Sleep(debounce_ms); // coalesce bursts
                        rescanDir(s_absDirs_[idx]);
                        ::FindNextChangeNotification(s_handles_[idx]);
                    }
                    else if (w == WAIT_TIMEOUT) {
                        // allow stop flag to be observed
                    }
                    else {
                        // unexpected; break to avoid tight spin
                        break;
                    }
                }
            }

            static bool parseOneRelIntoCache(const std::wstring& relW) {
                gdt g;
                if (!ParseOneFile(s_rootW_, relW, g)) return false;
                const std::string key = WideToUTF8(Stem(relW));
                std::unique_lock<std::shared_mutex> lk(s_mu_);
                s_all_[key] = std::move(g);
                return true;
            }

            static void rescanDir(const std::wstring& absDir) {
                std::unordered_map<std::wstring, FILETIME> now;
                namespace fs = std::filesystem;
                std::error_code ec;
                for (fs::recursive_directory_iterator it(absDir, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
                    if (ec) { ec.clear(); continue; }
                    const fs::directory_entry& de = *it;
                    if (!de.is_regular_file(ec)) continue;
                    const std::wstring fileAbs = NormalizeBackslashes(de.path().wstring());
                    if (!EndsWithI(fileAbs, L".gdt")) continue;
                    std::wstring relInside = RelPathFrom(absDir, fileAbs);
                    WIN32_FILE_ATTRIBUTE_DATA fad{};
                    if (::GetFileAttributesExW(fileAbs.c_str(), GetFileExInfoStandard, &fad))
                        now.emplace(std::move(relInside), fad.ftLastWriteTime);
                }

                auto& prev = s_mtimes_[absDir];
                bool any = false;

                // Deletions
                for (const auto& kv : prev) {
                    if (!now.count(kv.first)) {
                        const std::wstring rel = relFromAbs(absDir, kv.first);
                        const std::string key = WideToUTF8(Stem(rel));
                        {
                            std::unique_lock<std::shared_mutex> lk(s_mu_);
                            if (s_all_.erase(key)) any = true;
                        }
                        push(ChangeKind::Deleted, rel);
                    }
                }

                // Additions & Modifications
                for (const auto& kv : now) {
                    const auto it = prev.find(kv.first);
                    const bool isNew = (it == prev.end());
                    const bool changed = (!isNew &&
                        (kv.second.dwLowDateTime != it->second.dwLowDateTime ||
                            kv.second.dwHighDateTime != it->second.dwHighDateTime));
                    if (isNew || changed) {
                        const std::wstring rel = relFromAbs(absDir, kv.first);
                        if (parseOneRelIntoCache(rel)) {
                            any = true;
                            push(isNew ? ChangeKind::Added : ChangeKind::Modified, rel);
                        }
                    }
                }

                prev = std::move(now);

                // Bump generation ONCE for this directory rescan
                if (any) {
                    s_generation_.fetch_add(1, std::memory_order_acq_rel);
                }
            }
        };

    } // namespace watch

    // ===========================
    // Minimal inline ParseOneFile fallback
    // Replace with your real parser for full fidelity.
    // ===========================
    inline bool ParseOneFile(const std::wstring& toolsRoot, const std::wstring& relPath, gdt& out) {
        const std::wstring abs = Join2(toolsRoot, relPath);

        std::ifstream in(abs, std::ios::binary);
        if (!in) return false;

        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

        out = gdt{};
        out.Name = WideToUTF8(Stem(relPath));
        out.RelPath = WideToUTF8(relPath);
        out.Assets.clear();

        // Simple extractor: lines like  "assetName" ("type.gdf") { ... }
        static const std::regex decl(R"regex("([^"]+)"\s*\(\s*"([^"]+)\.gdf"\s*\)\s*\{)regex");
        for (auto it = std::sregex_iterator(text.begin(), text.end(), decl);
            it != std::sregex_iterator(); ++it) {
            xasset xa;
            xa.Name = (*it)[1].str();
            xa.Type = (*it)[2].str();
            xa.GdtRelPath = out.RelPath;
            out.Assets[xa.Name] = std::move(xa);
        }

        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (::GetFileAttributesExW(abs.c_str(), GetFileExInfoStandard, &fad)) {
            ULARGE_INTEGER uli{};
            uli.LowPart = fad.ftLastWriteTime.dwLowDateTime;
            uli.HighPart = fad.ftLastWriteTime.dwHighDateTime;
            out.Time = static_cast<uint64_t>(uli.QuadPart);
        }
        return true;
    }

} // namespace gdtlib

#endif // GDT_LIBRARY_H
