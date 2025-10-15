// GDTLibrary.h
#pragma once
#ifndef GDT_LIBRARY_H
#define GDT_LIBRARY_H

// Windows + CRT
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// Project headers
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
    inline std::wstring UTF8ToWide(const std::string& s) {
        if (s.empty()) return {};
        int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
        std::wstring w(n, L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
        return w;
    }

    // ===========================
    // Paths / env
    // ===========================
    inline std::wstring Join2(const std::wstring& a, const std::wstring& b) {
        if (a.empty()) return b;
        if (b.empty()) return a;
        if (a.back() == L'\\' || a.back() == L'/') return a + b;
        return a + L'\\' + b;
    }
    inline std::wstring GetToolsRootW() {
        wchar_t* val = nullptr; size_t len = 0;
        if (_wdupenv_s(&val, &len, L"TA_TOOLS_PATH") != 0 || !val) {
            std::cerr << "Error: TA_TOOLS_PATH not set.\n";
            return {};
        }
        std::wstring root(val);
        free(val);
        while (!root.empty() && (root.back() == L'\\' || root.back() == L'/'))
            root.pop_back();
        return root;
    }
    inline bool EndsWithGdtCI(const wchar_t* name, size_t len) {
        if (len < 4) return false;
        wchar_t a = name[len - 4], b = name[len - 3], c = name[len - 2], d = name[len - 1];
        return (a == L'.') && (b == L'g' || b == L'G') && (c == L'd' || c == L'D') && (d == L't' || d == L'T');
    }
    inline std::wstring Stem(const std::wstring& filename) {
        size_t pos = filename.find_last_of(L'.');
        if (pos == std::wstring::npos || pos == 0) return filename;
        return filename.substr(0, pos);
    }
    inline std::string_view StemNoExt(std::string_view s) {
        size_t slash = s.find_last_of("/\\");
        if (slash != std::string::npos) s.remove_prefix(slash + 1);
        size_t dot = s.find_last_of('.');
        if (dot != std::string::npos) s.remove_suffix(s.size() - dot);
        return s;
    }

    // ===========================
    // Load converter_gdt_dirs_0.txt
    // ===========================
    inline std::vector<std::wstring> LoadTargetDirsRelW(const std::wstring& root) {
        std::vector<std::wstring> out;
        const std::wstring cfg = Join2(Join2(root, L"bin"), L"converter_gdt_dirs_0.txt");

        std::ifstream in(WideToUTF8(cfg), std::ios::in | std::ios::binary);
        if (!in) {
            std::cerr << "Error: cannot open list file: " << WideToUTF8(cfg) << "\n";
            return out;
        }

        std::set<std::wstring> seen;
        std::string lineUtf8;
        while (std::getline(in, lineUtf8)) {
            if (!lineUtf8.empty() && (unsigned char)lineUtf8[0] == 0xEF &&
                lineUtf8.size() >= 3 &&
                (unsigned char)lineUtf8[1] == 0xBB &&
                (unsigned char)lineUtf8[2] == 0xBF) {
                lineUtf8.erase(0, 3);
            }
            // trim
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

    // ===========================
    // Enumerate *.gdt files under target dirs (PARALLEL)
    // ===========================
    inline std::vector<std::wstring> FindAllGdtRelPathsConcurrent(
        const std::wstring& toolsRoot,
        const std::vector<std::wstring>& dirsRel)
    {
        std::vector<std::wstring> relpaths;
        relpaths.reserve(4096);
        std::mutex rel_m;

        const unsigned threads = std::max(2u, std::thread::hardware_concurrency());
        std::atomic<size_t> idx{ 0 };
        std::vector<std::thread> pool;
        pool.reserve(threads);

        auto worker = [&]() {
            for (;;) {
                size_t i = idx.fetch_add(1, std::memory_order_relaxed);
                if (i >= dirsRel.size()) break;
                const auto& rel = dirsRel[i];

                std::wstring absDir = Join2(toolsRoot, rel);
                std::wstring pattern = Join2(absDir, L"*");

                WIN32_FIND_DATAW fd;
                HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
                if (h == INVALID_HANDLE_VALUE) continue;

                std::vector<std::wstring> local;
                do {
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    size_t len = wcslen(fd.cFileName);
                    if (EndsWithGdtCI(fd.cFileName, len)) {
                        local.push_back(Join2(rel, fd.cFileName)); // keep rel path
                    }
                } while (FindNextFileW(h, &fd));
                FindClose(h);

                if (!local.empty()) {
                    std::lock_guard<std::mutex> lk(rel_m);
                    relpaths.insert(relpaths.end(), local.begin(), local.end());
                }
            }
            };

        for (unsigned t = 0; t < threads; ++t) pool.emplace_back(worker);
        for (auto& th : pool) th.join();

        return relpaths;
    }

    // ===========================
    // Simple memory-mapped file
    // ===========================
    struct MappedFile {
        HANDLE hFile = INVALID_HANDLE_VALUE;
        HANDLE hMap = nullptr;
        const char* data = nullptr;
        size_t len = 0;

        bool open(const std::wstring& path) {
            hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (hFile == INVALID_HANDLE_VALUE) return false;
            LARGE_INTEGER sz{}; if (!GetFileSizeEx(hFile, &sz)) return false;
            len = size_t(sz.QuadPart); if (!len) return false;
            hMap = CreateFileMappingW(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
            if (!hMap) return false;
            data = (const char*)MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
            return data != nullptr;
        }
        void close() {
            if (data) UnmapViewOfFile(data);
            if (hMap) CloseHandle(hMap);
            if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
            data = nullptr; hMap = nullptr; hFile = INVALID_HANDLE_VALUE; len = 0;
        }
        ~MappedFile() { close(); }
    };

    // ===========================
    // Tiny scanner
    // ===========================
    struct Cursor {
        const char* p, * e;
        bool ok() const { return p && p < e; }
    };
    inline void skip_ws(Cursor& c) {
        while (c.ok() && (unsigned char)*c.p <= ' ') ++c.p;
    }
    inline bool scan_char(Cursor& c, char ch) {
        if (!c.ok() || *c.p != ch) return false;
        ++c.p; return true;
    }
    inline bool scan_quoted(Cursor& c, std::string_view& out) {
        if (!scan_char(c, '"')) return false;
        const char* beg = c.p;
        while (c.ok() && *c.p != '"') ++c.p;
        if (!c.ok()) return false;
        out = std::string_view(beg, (size_t)(c.p - beg));
        ++c.p;
        return true;
    }
    inline void skip_braced_block(Cursor& c) {
        if (!scan_char(c, '{')) return;
        int depth = 1;
        while (c.ok() && depth > 0) {
            char ch = *c.p++;
            if (ch == '"') {
                while (c.ok() && *c.p != '"') ++c.p;
                if (c.ok()) ++c.p;
            }
            else if (ch == '{') {
                ++depth;
            }
            else if (ch == '}') {
                --depth;
            }
        }
    }

    // ===========================
    // Parse headers only:  "asset_name" ( "file.gdf" ) { ... }
    // Fills g.Assets with xasset{Name, Type} only; also sets a.GdtRelPath if available on gdt
    // ===========================
    inline void ParseAssetHeadersOnly(const char* data, size_t len, gdt& g) {
        Cursor c{ data, data + len };
        std::string_view name, gdf;

        while (c.ok()) {
            skip_ws(c);
            if (!c.ok()) break;

            const char* save = c.p;
            if (!scan_quoted(c, name)) { ++c.p; continue; }

            skip_ws(c);
            if (!scan_char(c, '(')) { c.p = save + 1; continue; }
            skip_ws(c);
            if (!scan_quoted(c, gdf)) { c.p = save + 1; continue; }
            skip_ws(c);
            if (!scan_char(c, ')')) { c.p = save + 1; continue; }
            skip_ws(c);

            if (!scan_char(c, '{')) { c.p = save + 1; continue; }
            --c.p;
            skip_braced_block(c);

            xasset xa;
            xa.Name.assign(name.data(), name.size());
            std::string_view tv = StemNoExt(gdf);
            xa.Type.assign(tv.data(), tv.size());

            xa.GdtRelPath = g.RelPath;

            g.Assets.emplace(xa.Name, std::move(xa));
        }
    }


    // ===========================
    // Parse a single file into gdt (thread worker)
    // ===========================
    inline bool ParseOneFile(const std::wstring& toolsRoot, const std::wstring& relPath, gdt& outG) {
        std::wstring abs = Join2(toolsRoot, relPath);

        // Build gdt shell
        size_t pos = relPath.find_last_of(L'\\');
        std::wstring file = (pos == std::wstring::npos) ? relPath : relPath.substr(pos + 1);
        std::string gdtName = WideToUTF8(Stem(file));
        std::string rel = WideToUTF8(relPath);
        gdt g(gdtName, rel, /*Time*/ 0);

        // Fast parse
        MappedFile mf;
        if (mf.open(abs)) {
            ParseAssetHeadersOnly(mf.data, mf.len, g);
        }
        else {
            // Fallback: std::ifstream
            std::ifstream in(WideToUTF8(abs), std::ios::in | std::ios::binary);
            if (!in) {
                std::cerr << "Warning: cannot open: " << WideToUTF8(abs) << "\n";
                return false;
            }
            std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (!text.empty()) ParseAssetHeadersOnly(text.data(), text.size(), g);
        }

        outG = std::move(g);
        return true;
    }

    // ===========================
    // Build all GDTs (FULLY PARALLEL: enumerate + parse)
    // ===========================
    inline std::map<std::string, gdt> getAllGDTs() {
        std::map<std::string, gdt> out;

        std::wstring toolsRoot = GetToolsRootW();
        if (toolsRoot.empty()) return out;

        auto dirsRel = LoadTargetDirsRelW(toolsRoot);
        if (dirsRel.empty()) {
            std::cerr << "No folders to scan.\n";
            return out;
        }

        auto filesRel = FindAllGdtRelPathsConcurrent(toolsRoot, dirsRel);
        std::cout << "Found " << filesRel.size() << " .gdt file(s) across " << dirsRel.size() << " seed folder(s).\n";
        if (filesRel.empty()) return out;

        const unsigned threads = std::max(2u, std::thread::hardware_concurrency());
        std::atomic<size_t> idx{ 0 };
        std::vector<std::thread> pool;
        pool.reserve(threads);

        // Collect results thread-safely (vector for speed; we'll move into map at the end)
        std::vector<std::pair<std::string, gdt>> results;
        results.reserve(filesRel.size());
        std::mutex res_m;

        auto worker = [&]() {
            for (;;) {
                size_t i = idx.fetch_add(1, std::memory_order_relaxed);
                if (i >= filesRel.size()) break;

                const auto& rel = filesRel[i];
                gdt gtmp("", "", 0);
                if (ParseOneFile(toolsRoot, rel, gtmp)) {
                    std::lock_guard<std::mutex> lk(res_m);
                    results.emplace_back(gtmp.Name, std::move(gtmp));
                }
            }
            };

        for (unsigned t = 0; t < threads; ++t) pool.emplace_back(worker);
        for (auto& th : pool) th.join();

        // Move into map (if duplicate names exist, last one wins; adjust if needed)
        for (auto& kv : results) {
            out.emplace(std::move(kv.first), std::move(kv.second));
        }

        return out;
    }

} // namespace gdtlib

#endif // GDT_LIBRARY_H
